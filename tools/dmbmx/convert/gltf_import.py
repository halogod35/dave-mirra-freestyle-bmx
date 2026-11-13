"""glTF (.glb) -> ZMD importer (DM-021). Pure stdlib.

Rebuilds a ZMD from a .glb written by :func:`dmbmx.convert.gltf.export_glb` and then edited
(vertex positions/normals/UVs/colours, topology, skin weights, textures, frame transforms).
Spec: ``docs/tools/import.md``.

Principle: every chunk that is *derived* from the geometry (BinMesh 0x50E, Bone 0x10E ranges, the
morph-target bounding sphere) is kept byte-for-byte from ``extras.dmbmx`` **as long as it is still
consistent** with the imported geometry, and regenerated otherwise. Everything that is not geometry
(MDEF, MADF, materials, frame records, unknown plugins) comes from the extras verbatim. So an
unedited export imports byte-identically, and an edit touches only what it has to.

    from dmbmx.convert.gltf_import import import_glb
    zmd_bytes, report = import_glb(glb_bytes)          # report.warnings, report.changes
"""
from __future__ import annotations

import hashlib
import math
import struct

from dmbmx.convert import imaging
from dmbmx.convert.gltf import EXTRAS_KEY, EXTRAS_VERSION, read_glb
from dmbmx.core.chunks import RW, Chunk, parse_4cc, parse_rw
from dmbmx.core.errors import FormatError, LimitError
from dmbmx.formats import zmd as zmdfmt

__all__ = ["import_glb", "ImportReport", "GltfImportError", "stripify", "strip_triangles", "BINMESH_MODES",
           "PALETTISED_MODES"]

BINMESH_MODES = ("auto", "strips", "lists")
PALETTISED_MODES = ("promote", "quantize")
COLOR_KEY = (255, 0, 255)     # DM-014 [V] 0x00498661: this RGB in a BMP becomes transparent
MAX_VERTICES = 65535          # triangle indices are u16 in the RW geometry struct
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
_COMP = {5120: ("b", 127.0), 5121: ("B", 255.0), 5122: ("h", 32767.0), 5123: ("H", 65535.0),
         5125: ("I", None), 5126: ("f", None)}


class GltfImportError(FormatError):
    """The .glb cannot be turned into a valid ZMD (bad structure, or an unsupported edit)."""

    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="GLB", offset=offset)


class GltfImportLimit(GltfImportError, LimitError):
    """An edit exceeds what the ZMD format / engine can hold (e.g. > 65535 vertices)."""


class ImportReport:
    """What the import did. ``warnings``: things that may not work in game; ``changes``:
    human-readable list of what differs from the exported original; ``binmesh``: per
    ``(lod, atomic)`` 'kept' | 'strips' | 'lists'."""

    def __init__(self):
        self.warnings, self.changes, self.binmesh = [], [], {}
        self.raw_frames = {}     # (lod, frame) -> exact 56-byte record of unchanged frames (_patch_frames)

    def warn(self, msg):
        self.warnings.append(msg)

    def change(self, msg):
        self.changes.append(msg)

    def __repr__(self):
        return f"<ImportReport {len(self.changes)} changes, {len(self.warnings)} warnings>"


def _unb64(s):
    import base64
    try:
        return base64.b64decode(str(s).encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError) as e:
        raise GltfImportError(f"bad base64 in extras: {e}") from e


def _rw_chunks(b64):
    raw = _unb64(b64)
    try:
        return parse_rw(raw) if raw else []
    except FormatError as e:
        raise GltfImportError(f"extras chunk data: {e}") from e


def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


# ------------------------------------------------------------------------------- glTF access

class _Gltf:
    def __init__(self, data):
        try:
            self.js, self.bin = read_glb(data)
        except FormatError as e:
            raise GltfImportError(str(e)) from e
        js = self.js
        if not isinstance(js, dict) or not isinstance(js.get("nodes"), list):
            raise GltfImportError("JSON has no node list")
        self.nodes = js["nodes"]
        self.parent = {}
        for i, n in enumerate(self.nodes):
            if not isinstance(n, dict):
                raise GltfImportError(f"node {i} is not an object")
            for c in n.get("children", []):
                if not isinstance(c, int) or not 0 <= c < len(self.nodes) or c in self.parent:
                    raise GltfImportError(f"node {i}: bad or shared child {c!r}")
                self.parent[c] = i

    def ex(self, node):
        e = node.get("extras")
        e = e.get(EXTRAS_KEY) if isinstance(e, dict) else None
        return e if isinstance(e, dict) else {}

    def subtree(self, i):
        out, stack, seen = [], [i], set()
        while stack:
            k = stack.pop()
            if k in seen:
                raise GltfImportError(f"node cycle at {k}")
            seen.add(k)
            out.append(k)
            stack.extend(reversed(self.nodes[k].get("children", [])))
        return out

    def get(self, key, i):
        arr = self.js.get(key)
        if not isinstance(arr, list) or not isinstance(i, int) or not 0 <= i < len(arr):
            raise GltfImportError(f"{key}[{i!r}] does not exist")
        return arr[i]

    def accessor(self, i):
        """List of tuples (VECn) or scalars, as Python numbers; normalized ints -> floats."""
        acc = self.get("accessors", i)
        if "sparse" in acc:
            raise GltfImportError(f"accessor {i}: sparse accessors are not supported")
        if "bufferView" not in acc:
            raise GltfImportError(f"accessor {i}: no bufferView")
        typ, ct, count = acc.get("type"), acc.get("componentType"), acc.get("count")
        if typ not in _NCOMP or ct not in _COMP or not isinstance(count, int) or count < 0:
            raise GltfImportError(f"accessor {i}: bad type/componentType/count")
        bv = self.get("bufferViews", acc["bufferView"])
        if bv.get("buffer", 0) != 0:
            raise GltfImportError(f"accessor {i}: external buffers are not supported")
        fmt, norm = _COMP[ct]
        n = _NCOMP[typ]
        size = struct.calcsize("<" + fmt) * n
        bvo, bvl = bv.get("byteOffset", 0), bv.get("byteLength", 0)
        start = bvo + acc.get("byteOffset", 0)
        stride = bv.get("byteStride") or size
        if stride < size:
            raise GltfImportError(f"accessor {i}: byteStride {stride} < element size {size}")
        end = start + (stride * (count - 1) + size if count else 0)
        if end > bvo + bvl or bvo + bvl > len(self.bin):
            raise GltfImportError(f"accessor {i} runs past its bufferView/buffer")
        if stride == size:
            flat = struct.unpack_from(f"<{n * count}{fmt}", self.bin, start)
            vals = [flat[k * n:(k + 1) * n] for k in range(count)]
        else:
            vals = [struct.unpack_from(f"<{n}{fmt}", self.bin, start + k * stride) for k in range(count)]
        if acc.get("normalized") and norm:
            vals = [tuple(max(-1.0, v / norm) for v in t) for t in vals]
        for t in vals:
            if fmt == "f" and not all(math.isfinite(v) for v in t):
                raise GltfImportError(f"accessor {i}: non-finite float")
        return [t[0] for t in vals] if n == 1 else [tuple(t) for t in vals]

    def image(self, i):
        im = self.get("images", i)
        if "bufferView" not in im:
            raise GltfImportError(f"image {i}: only images embedded in the .glb are supported")
        bv = self.get("bufferViews", im["bufferView"])
        o, ln = bv.get("byteOffset", 0), bv.get("byteLength", 0)
        if o + ln > len(self.bin):
            raise GltfImportError(f"image {i}: bufferView past the buffer")
        return self.bin[o:o + ln]


def _node_matrix(n):
    """Local 4x4 (row-major nested lists, column-vector convention) of a glTF node."""
    if "matrix" in n:
        m = n["matrix"]
        if not isinstance(m, list) or len(m) != 16:
            raise GltfImportError("node matrix must have 16 numbers")
        return [[float(m[c * 4 + r]) for c in range(4)] for r in range(4)]
    t = n.get("translation", [0.0, 0.0, 0.0])
    x, y, z, w = n.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = n.get("scale", [1.0, 1.0, 1.0])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[i][0] * s[0], r[i][1] * s[1], r[i][2] * s[2], float(t[i])] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def _is_identity(m, eps=1e-9):
    return all(abs(m[r][c] - (1.0 if r == c else 0.0)) <= eps for r in range(4) for c in range(4))


def _normal_matrix(m):
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-20:
        raise GltfImportError("mesh node transform is singular")
    cof = [[(a[(i + 1) % 3][(j + 1) % 3] * a[(i + 2) % 3][(j + 2) % 3]
             - a[(i + 1) % 3][(j + 2) % 3] * a[(i + 2) % 3][(j + 1) % 3]) for j in range(3)] for i in range(3)]
    return [[cof[i][j] / det for j in range(3)] for i in range(3)]  # inverse-transpose


def _norm(v):
    ln = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / ln, v[1] / ln, v[2] / ln) if ln > 1e-20 else (0.0, 1.0, 0.0)


# ------------------------------------------------------------------------------- strips

def strip_triangles(ix):
    """Triangles of an RW triangle strip, CCW, skipping degenerate ones (RW/D3D strip rule:
    triangle k is (s[k], s[k+1], s[k+2]) for even k, (s[k+1], s[k], s[k+2]) for odd k)."""
    out = []
    for k in range(len(ix) - 2):
        a, b, c = ix[k], ix[k + 1], ix[k + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return out


def _canon(t):
    i = t.index(min(t))
    return tuple(t[i:] + t[:i])


def stripify(tris):
    """Greedy stripifier: CCW triangles -> one RW strip (sub-strips joined by degenerate
    triangles, with parity padding so every triangle keeps its winding). Verified by
    :func:`strip_triangles`."""
    tris = [tuple(t) for t in tris if len(set(t)) == 3]
    if not tris:
        return []
    by_edge = {}
    for ti, (a, b, c) in enumerate(tris):
        for p, q in ((a, b), (b, c), (c, a)):
            by_edge.setdefault((p, q), []).append(ti)
    used = [False] * len(tris)
    strip = []
    for start in range(len(tris)):
        if used[start]:
            continue
        used[start] = True
        a, b, c = tris[start]
        if strip:
            # join: repeat last index, then the new first index; pad so the new strip starts even
            strip += [strip[-1], a]
            if len(strip) % 2:
                strip.append(a)
        s = [a, b, c]
        while True:
            k = len(strip) + len(s) - 2           # position of the next triangle in the whole strip
            p, q = (s[-2], s[-1]) if k % 2 == 0 else (s[-1], s[-2])
            nxt = None
            for ti in by_edge.get((p, q), ()):
                if not used[ti]:
                    nxt = ti
                    break
            if nxt is None:
                break
            used[nxt] = True
            t = tris[nxt]
            i = t.index(p)
            s.append(t[(i + 2) % 3])
        strip += s
    from collections import Counter
    if Counter(_canon(t) for t in strip_triangles(strip)) != Counter(_canon(t) for t in tris):
        raise AssertionError("stripify produced a strip that does not decode to the input")  # pragma: no cover
    return strip


# ------------------------------------------------------------------------------- import

def import_glb(data: bytes, *, base: bytes | None = None, binmesh: str = "auto", strict: bool = False,
               palettised: str = "promote"):
    """Rebuild ZMD bytes from a dmbmx-exported (and possibly edited) .glb. See :func:`_import_glb`.
    Any malformed input raises :class:`GltfImportError` (a ``FormatError``), never a bare
    ``KeyError``/``TypeError``/``IndexError`` (error contract, docs/testing.md section 3)."""
    if binmesh not in BINMESH_MODES:
        raise ValueError(f"binmesh must be one of {BINMESH_MODES}")
    if palettised not in PALETTISED_MODES:
        raise ValueError(f"palettised must be one of {PALETTISED_MODES}")
    try:
        return _import_glb(data, base=base, binmesh=binmesh, strict=strict, palettised=palettised)
    except FormatError:
        raise
    except (KeyError, TypeError, IndexError, AttributeError, ValueError, struct.error, OverflowError,
            ZeroDivisionError, RecursionError) as e:
        raise GltfImportError(f"malformed .glb / extras: {type(e).__name__}: {e}") from e


def _import_glb(data, *, base=None, binmesh="auto", strict=False, palettised="promote"):
    return _import_glb_inner(data, base=base, binmesh=binmesh, strict=strict, palettised=palettised)


def _flush_denormals(rec):
    """``rec`` with every denormal f32 of the 12 matrix floats replaced by a zero of the same sign
    (what an FTZ/DAZ host writes back)."""
    b = bytearray(rec)
    for k in range(12):
        w = struct.unpack_from("<I", b, 4 * k)[0]
        if w & 0x7F800000 == 0 and w & 0x007FFFFF:
            struct.pack_into("<I", b, 4 * k, w & 0x80000000)
    return bytes(b)


def _patch_frames(out, raw):
    """Write the stored 56-byte records of unchanged frames back verbatim. Decoding them to floats and
    re-packing is exact in normal Python, but hosts that flush denormal floats to zero (Blender) lose
    denormals ([V] bike_hires.zmd has two). Byte-level: the frame list layout is unchanged."""
    if not raw:
        return out
    buf = bytearray(out)
    modl = parse_4cc(out, zmdfmt.CONTAINERS_4CC)[0]
    lods = [c for c in modl.children if c.tag == b"MLOD"]
    for li, ml in enumerate(lods):
        mdff = next(c for c in ml.children if c.tag == b"MDFF")
        clump = parse_rw(mdff.data, base=mdff.offset + 8)[0]
        fl = next(c for c in clump.children if c.tag == RW.FRAMELIST)
        st = fl.children[0]
        start = st.offset + 12 + 4
        for (l, fi), rec in raw.items():
            if l != li or 56 * (fi + 1) > len(st.data) - 4:
                continue
            o = start + 56 * fi
            cur = bytes(buf[o:o + 56])
            if cur != rec and cur in (struct.pack("<12fiI", *struct.unpack("<12fiI", rec)), _flush_denormals(rec)):
                buf[o:o + 56] = rec
    return bytes(buf)


def _import_glb_inner(data, *, base=None, binmesh="auto", strict=False, palettised="promote"):
    """Rebuild ZMD bytes from a dmbmx-exported (and possibly edited) .glb.

    ``base``: the original ZMD bytes (optional). It is checked against the SHA-256 in the extras and
    supplies the LODs that a partial (``--lod``) export does not contain.
    ``binmesh``: form of *regenerated* BinMesh chunks: 'auto' (same as the original geometry),
    'strips' or 'lists'. Unchanged geometry always keeps its original BinMesh.
    ``strict``: turn warnings into :class:`GltfImportError`.
    Returns ``(zmd_bytes, ImportReport)``.
    """
    if binmesh not in BINMESH_MODES:
        raise ValueError(f"binmesh must be one of {BINMESH_MODES}")
    rep = ImportReport()
    g = _Gltf(data)
    root_i = next((i for i, n in enumerate(g.nodes) if g.ex(n).get("format") == "dmbmx-zmd"), None)
    if root_i is None:
        raise GltfImportError("no dmbmx root node: this .glb was not written by `dmbmx export`, or its "
                              "extras were stripped (export with custom properties / extras enabled)")
    rx = g.ex(g.nodes[root_i])
    if rx.get("version") != EXTRAS_VERSION:
        raise GltfImportError(f"extras version {rx.get('version')!r}, this importer reads {EXTRAS_VERSION}")
    base_zmd = None
    if base is not None:
        want = (rx.get("source") or {}).get("sha256")
        if hashlib.sha256(base).hexdigest() != want:
            rep.warn("--base file is not the file this .glb was exported from (SHA-256 differs)")
        base_zmd = zmdfmt.read(base)
    if not rx.get("complete") and base_zmd is None:
        raise GltfImportError("partial export (--lod): pass the original file as base to fill in the other LODs")
    if not isinstance(rx.get("items"), list):
        raise GltfImportError("root extras have no item list")

    lod_nodes = {g.ex(n).get("lod"): i for i, n in enumerate(g.nodes)
                 if "lod" in g.ex(n) and "mldf" in g.ex(n) and root_i in _ancestors(g, i)}
    items = []
    for it in rx["items"]:
        t = it.get("type") if isinstance(it, dict) else None
        if t == "MLOD":
            li = it.get("lod")
            if li in lod_nodes:
                items.append(_import_lod(g, lod_nodes[li], rep, binmesh))
            elif base_zmd is not None and isinstance(li, int) and li < len(base_zmd.lods):
                items.append(base_zmd.lods[li])
                rep.change(f"LOD {li}: taken from --base (not in this .glb)")
            else:
                raise GltfImportError(f"LOD {li!r} missing from the .glb")
        elif t == "MDEF":
            try:
                items.append(zmdfmt.Mdef.decode(_unb64(it["data"])))
            except (KeyError, FormatError) as e:
                raise GltfImportError(f"MDEF extras: {e}") from e
        elif t == "MATI":
            items.append(_import_mati(g, it, rep, palettised))
        elif t == "chunk":
            try:
                items.append(parse_4cc(_unb64(it["chunk"]))[0])
            except (KeyError, IndexError, FormatError) as e:
                raise GltfImportError(f"raw chunk extras: {e}") from e
        else:
            raise GltfImportError(f"unknown item type {t!r} in root extras")
    _check_root(g, root_i, rx, rep)
    try:
        out = zmdfmt.Zmd(items).write()
    except (ValueError, struct.error, OverflowError) as e:
        raise GltfImportError(f"cannot encode the ZMD: {e}") from e
    out = _patch_frames(out, rep.raw_frames)
    # self-check: the result must parse with the strict reader
    try:
        zmdfmt.read(out)
    except FormatError as e:  # pragma: no cover - would be an importer bug
        raise GltfImportError(f"internal error, the written ZMD does not re-read: {e}") from e
    if strict and rep.warnings:
        raise GltfImportError("strict mode: " + "; ".join(rep.warnings))
    return out, rep


def _ancestors(g, i):
    out = set()
    while i in g.parent:
        i = g.parent[i]
        if i in out:
            raise GltfImportError("node cycle")
        out.add(i)
    return out


def _check_root(g, root_i, rx, rep):
    corr = (rx.get("axis") or {}).get("root_correction")
    from dmbmx.convert.gltf import UPRIGHT_ROTATIONS
    n = g.nodes[root_i]
    want = [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    if corr in UPRIGHT_ROTATIONS:
        want = _node_matrix({"rotation": UPRIGHT_ROTATIONS[corr]})
    have = _node_matrix(n)
    if any(abs(have[r][c] - want[r][c]) > 1e-5 for r in range(4) for c in range(4)):
        rep.warn("the model root node was moved/rotated/scaled; root transforms are presentation only "
                 "and are ignored (edit the frames or the vertices instead)")


# -- LOD / frames -------------------------------------------------------------

def _import_lod(g, ln_i, rep, binmesh):
    lx = g.ex(g.nodes[ln_i])
    li = lx.get("lod")
    try:
        nframes = int(lx["frames"])
        lod = zmdfmt.Lod(tuple(lx["mldf"]), zmdfmt.Clump(), int(lx["rw_version"]))
    except (KeyError, TypeError, ValueError) as e:
        raise GltfImportError(f"LOD extras: {e}") from e
    cl = lod.clump
    cl.struct_extra = _unb64(lx.get("clump_struct_extra", ""))
    cl.ext = _rw_chunks(lx.get("clump_ext", ""))
    if not _is_identity(_node_matrix(g.nodes[ln_i]), 1e-6):
        rep.warn(f"LOD {li}: the LOD node has a transform; it is ignored")

    sub = g.subtree(ln_i)
    frame_nodes, atomic_nodes = {}, {}
    for i in sub:
        e = g.ex(g.nodes[i])
        if "record" in e and "frame" in e:
            if e["frame"] in frame_nodes:
                raise GltfImportError(f"LOD {li}: frame {e['frame']} appears twice (duplicated node?)")
            frame_nodes[e["frame"]] = i
        elif "atomic" in e and "geometry" in e:
            if e["atomic"] in atomic_nodes:
                raise GltfImportError(f"LOD {li}: atomic {e['atomic']} appears twice (duplicated object?)")
            atomic_nodes[e["atomic"]] = i
    if sorted(frame_nodes) != list(range(nframes)):
        raise GltfImportError(f"LOD {li}: expected frames 0..{nframes - 1}, found {sorted(frame_nodes)[:8]}; "
                              "adding or deleting bones/frames is not supported (animations address them)")
    want_atomics = len(lx.get("atomic_nodes", []))
    if sorted(atomic_nodes) != list(range(want_atomics)):
        raise GltfImportError(f"LOD {li}: expected atomics 0..{want_atomics - 1}, found {sorted(atomic_nodes)}; "
                              "adding or deleting parts is not supported")

    frames = []
    for fi in range(nframes):
        ni = frame_nodes[fi]
        fx = g.ex(g.nodes[ni])
        raw = _unb64(fx["record"])
        if len(raw) != 56:
            raise GltfImportError(f"frame {fi}: record is {len(raw)} bytes, need 56")
        rec = struct.unpack("<12fiI", raw)
        rep.raw_frames[(li, fi)] = raw
        f = zmdfmt.Frame(rec[0:3], rec[3:6], rec[6:9], rec[9:12], rec[12], rec[13], ext=_rw_chunks(fx.get("ext", "")))
        for c in f.ext:  # same as the reader: Animation(0x108) = bone id
            if c.tag == RW.ANIMATION and c.data is not None and len(c.data) == 8:
                f.anim_id, f.anim_extra = struct.unpack("<II", c.data)
        if not (-1 <= f.parent < fi):
            raise GltfImportError(f"LOD {li}: frame {fi} record has parent {f.parent}")
        m = _node_matrix(g.nodes[ni])
        cur = f.matrix()
        if any(_f32(m[r][c]) != cur[r][c] for r in range(3) for c in range(4)):
            f.right = tuple(_f32(m[r][0]) for r in range(3))
            f.up = tuple(_f32(m[r][1]) for r in range(3))
            f.at = tuple(_f32(m[r][2]) for r in range(3))
            f.pos = tuple(_f32(m[r][3]) for r in range(3))
            rep.change(f"LOD {li}: frame {fi} ({g.nodes[ni].get('name')}) transform changed")
            rep.raw_frames.pop((li, fi), None)
        pnode = g.parent.get(ni)
        pfx = g.ex(g.nodes[pnode]) if pnode is not None else {}
        expect_parent = pfx.get("frame", -1) if "record" in pfx else -1
        if expect_parent != f.parent:
            rep.warn(f"LOD {li}: frame {fi} was re-parented in the glTF; re-parenting is ignored "
                     f"(the file keeps parent {f.parent})")
        frames.append(f)
    cl.frames = frames

    for ai in range(want_atomics):
        ni = atomic_nodes[ai]
        ax = g.ex(g.nodes[ni])
        try:
            fr = int(ax["frame"])
            geo = _import_geometry(g, ni, ax["geometry"], frame_nodes, nframes, cl, rep, binmesh, f"LOD {li} atomic {ai}")
            cl.atomics.append(zmdfmt.Atomic(fr, geo, int(ax["flags"]), int(ax["unused"]), _rw_chunks(ax.get("ext", ""))))
        except (KeyError, TypeError) as e:
            raise GltfImportError(f"LOD {li} atomic {ai}: bad extras ({e!r})") from e
        if not 0 <= fr < nframes:
            raise GltfImportError(f"LOD {li} atomic {ai}: frame {fr} out of range")
    return lod


# -- geometry -----------------------------------------------------------------

def _gather_vertices(g, mesh, where):
    """Merge the primitives' vertex streams. Returns (attrs dict of lists, [(prim, base_offset)])."""
    prims = mesh.get("primitives")
    if not isinstance(prims, list) or not prims:
        raise GltfImportError(f"{where}: mesh has no primitives")
    for p in prims:
        if p.get("mode", 4) != 4:
            raise GltfImportError(f"{where}: primitive mode {p.get('mode')} (only triangle lists are supported)")
        if "indices" not in p:
            raise GltfImportError(f"{where}: non-indexed primitives are not supported")
        if "targets" in p:
            raise GltfImportError(f"{where}: glTF morph targets (shape keys) are not supported")
    keys = set(prims[0]["attributes"])
    if any(set(p["attributes"]) != keys for p in prims):
        raise GltfImportError(f"{where}: primitives have different vertex attributes")
    if "POSITION" not in keys:
        raise GltfImportError(f"{where}: no POSITION attribute")
    shared = all(p["attributes"] == prims[0]["attributes"] for p in prims)
    attrs, offsets = {k: [] for k in keys}, []
    sources = [prims[0]] if shared else prims
    off = 0
    for p in sources:
        n = None
        for k in sorted(keys):
            vals = g.accessor(p["attributes"][k])
            if n is None:
                n = len(vals)
            elif len(vals) != n:
                raise GltfImportError(f"{where}: attribute {k} has {len(vals)} values, POSITION has {n}")
            attrs[k] += vals
        offsets.append(off)
        off += n
    prim_off = [(p, 0 if shared else offsets[i]) for i, p in enumerate(prims)]
    return attrs, prim_off


def _import_geometry(g, ni, gx, frame_nodes, nframes, cl, rep, binmesh, where):
    node = g.nodes[ni]
    geo = zmdfmt.Geometry()
    geo.flags = int(gx["flags"])
    geo.surface = tuple(gx["surface"])
    mts = gx["morph_targets"]
    if len(mts) != 1:
        raise GltfImportError(f"{where}: {len(mts)} morph targets (only 1 is supported)")
    mt0 = mts[0]
    ml = _rw_chunks(gx["material_list"])
    if len(ml) != 1 or ml[0].tag != RW.MATLIST or not ml[0].children:
        raise GltfImportError(f"{where}: bad MaterialList in extras")
    st = ml[0].children[0].data or b""
    if len(st) < 4 or len(st) != 4 + 4 * struct.unpack_from("<I", st, 0)[0]:
        raise GltfImportError(f"{where}: bad MaterialList struct in extras")
    geo.material_indices = list(struct.unpack_from(f"<{(len(st) - 4) // 4}i", st, 4))
    try:
        geo.materials = [zmdfmt._material_from_chunk(m) for m in ml[0].children[1:]]
    except FormatError as e:
        raise GltfImportError(f"{where}: MaterialList: {e}") from e
    slots = geo.material_list()
    nslots = len(slots)
    ext = _rw_chunks(gx["ext"])
    old_bm = next((c for c in ext if c.tag == RW.BINMESH), None)
    old_bone = next((c for c in ext if c.tag == RW.BONE), None)
    old_nv = int(gx["vertices"])

    if "mesh" not in node:
        raise GltfImportError(f"{where}: node has no mesh (deleting a part's geometry is not supported)")
    mesh = g.get("meshes", node["mesh"])
    attrs, prim_off = _gather_vertices(g, mesh, where)
    P = [tuple(map(float, p)) for p in attrs["POSITION"]]
    nv = len(P)
    if nv == 0:
        raise GltfImportError(f"{where}: mesh has no vertices")
    if nv > MAX_VERTICES:
        raise GltfImportLimit(f"{where}: {nv} vertices; a ZMD geometry holds at most {MAX_VERTICES} "
                              "(u16 triangle indices). Split the part or decimate it")

    # -- per-slot triangles, CCW (a, b, c) in glTF order
    by_slot = {}
    slot_names = [(m.texture.name.upper() if m is not None and m.texture is not None else None) for m in slots]
    for p, off in prim_off:
        pe = p.get("extras", {}).get(EXTRAS_KEY, {}) if isinstance(p.get("extras"), dict) else {}
        slot = pe.get("material_slot")
        if slot is None and "material" in p:
            mname = str(g.get("materials", p["material"]).get("name", "")).upper()
            slot = slot_names.index(mname) if mname in slot_names else None
        if not isinstance(slot, int) or not 0 <= slot < nslots:
            raise GltfImportError(f"{where}: primitive material cannot be mapped to one of the {nslots} "
                                  "original material slots (new materials are not supported)")
        ix = g.accessor(p["indices"])
        if len(ix) % 3:
            raise GltfImportError(f"{where}: index count {len(ix)} is not a multiple of 3")
        tl = by_slot.setdefault(slot, [])
        for k in range(0, len(ix), 3):
            t = (ix[k] + off, ix[k + 1] + off, ix[k + 2] + off)
            if max(t) >= nv:
                raise GltfImportError(f"{where}: index {max(t)} >= {nv} vertices")
            tl.append(t)
    rle = gx.get("tri_materials", [])
    counts = {}
    for mid, cnt in rle:
        counts[mid] = counts.get(mid, 0) + cnt
    if counts == {k: len(v) for k, v in by_slot.items() if v}:
        order = [mid for mid, cnt in rle for _ in range(cnt)]
    else:
        order = [mid for mid in sorted(by_slot) for _ in by_slot[mid]]
        rep.change(f"{where}: triangle count per material changed "
                   f"({sum(counts.values())} -> {sum(len(v) for v in by_slot.values())} triangles)")
    if len(order) > 0xFFFFFFFF:
        raise GltfImportLimit(f"{where}: too many triangles")

    # -- vertex attributes
    is_skinned = old_bone is not None
    if not is_skinned and not _is_identity(_node_matrix(node), 1e-9):
        m = _node_matrix(node)
        P = [tuple(m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] for r in range(3)) for p in P]
        nm = _normal_matrix(m)
        if "NORMAL" in attrs:
            attrs["NORMAL"] = [_norm(tuple(nm[r][0] * n[0] + nm[r][1] * n[1] + nm[r][2] * n[2] for r in range(3)))
                               for n in attrs["NORMAL"]]
        rep.change(f"{where}: mesh node transform baked into the vertices")
    N = None
    if mt0.get("has_normals"):
        if "NORMAL" in attrs:
            N = [tuple(map(float, n)) for n in attrs["NORMAL"]]
        else:
            N = _face_normals(P, [t for v in by_slot.values() for t in v])
            rep.warn(f"{where}: no NORMAL attribute; normals recomputed from the faces")
    elif "NORMAL" in attrs:
        rep.warn(f"{where}: NORMAL ignored (the original geometry has no normals)")
    uv_sets = []
    nuv = 2 if geo.flags & zmdfmt.GEO_TEXTURED2 else (1 if geo.flags & zmdfmt.GEO_TEXTURED else 0)
    for k in range(nuv):
        key = f"TEXCOORD_{k}"
        if key not in attrs:
            raise GltfImportError(f"{where}: {key} missing but the geometry is textured; "
                                  "the engine would sample garbage (re-unwrap or keep the UV map)")
        uv_sets.append([(float(u), float(v)) for u, v in attrs[key]])
    colors = None
    if geo.flags & zmdfmt.GEO_PRELIT:
        if "COLOR_0" in attrs:
            colors = [_rgba8(c) for c in attrs["COLOR_0"]]
        else:
            colors = [(255, 255, 255, 255)] * nv
            rep.warn(f"{where}: no COLOR_0 on a prelit geometry; vertex colours set to white")

    # -- skinning: one bone per vertex
    perm = None
    bones = None
    if not is_skinned and ("JOINTS_0" in attrs or "skin" in node):
        rep.warn(f"{where}: skin/JOINTS_0 on a rigid part is ignored (skinning a rigid part is not supported)")
    if is_skinned:
        if "JOINTS_0" not in attrs or "skin" not in node:
            raise GltfImportError(f"{where}: skinned geometry lost its skin/JOINTS_0 (re-bind to the armature)")
        skin = g.get("skins", node["skin"])
        jframe = []
        for j in skin.get("joints", []):
            jx = g.ex(g.get("nodes", j))
            if "frame" not in jx or "record" not in jx or frame_nodes.get(jx["frame"]) != j:
                raise GltfImportError(f"{where}: skin joint node {j} is not one of this LOD's frames")
            jframe.append(jx["frame"])
        if len(jframe) != nframes:
            raise GltfImportError(f"{where}: skin has {len(jframe)} joints, the model has {nframes} frames "
                                  "(bone count changes are not supported)")
        W = attrs.get("WEIGHTS_0")
        vf, multi = [], 0
        for v, js_ in enumerate(attrs["JOINTS_0"]):
            ws = W[v] if W is not None else (1.0, 0.0, 0.0, 0.0)
            k = max(range(len(js_)), key=lambda q: ws[q])
            if sum(1 for w in ws if w > 1e-6) > 1:
                multi += 1
            j = int(js_[k])
            if not 0 <= j < len(jframe):
                raise GltfImportError(f"{where}: vertex {v} uses joint {j}, skin has {len(jframe)}")
            vf.append(jframe[j])
        if multi:
            rep.warn(f"{where}: {multi} vertices have several bone weights; the ZMD Bone plugin is rigid, "
                     "each vertex goes to its largest weight")
        perm, bones = _bone_ranges(vf, old_bone, cl.frames, where, rep, old_nv)

    if perm is not None:
        inv = [0] * nv
        for new, old in enumerate(perm):
            inv[old] = new
        P = [P[o] for o in perm]
        N = [N[o] for o in perm] if N is not None else None
        uv_sets = [[uv[o] for o in perm] for uv in uv_sets]
        colors = [colors[o] for o in perm] if colors is not None else None
        by_slot = {k: [(inv[a], inv[b], inv[c]) for a, b, c in v] for k, v in by_slot.items()}
        rep.change(f"{where}: vertices re-sorted by bone")

    cursor = {k: 0 for k in by_slot}
    tris = []
    for mid in order:
        a, b, c = by_slot[mid][cursor[mid]]
        cursor[mid] += 1
        tris.append((b, a, mid, c))
    geo.triangles = tris
    geo.prelit = colors
    geo.uv_sets = uv_sets
    sphere = _sphere(P, tuple(mt0["sphere"]))
    if sphere != tuple(mt0["sphere"]):
        rep.change(f"{where}: bounding sphere recomputed")
    geo.morph_targets = [zmdfmt.MorphTarget(sphere, P if mt0.get("has_vertices", True) else None, N)]
    if not mt0.get("has_vertices", True):
        raise GltfImportError(f"{where}: geometry without vertex positions is not supported")

    # -- BinMesh: keep while consistent, else regenerate
    new_by_slot = {k: [(a, b, c) for a, b, c in v] for k, v in by_slot.items()}
    key = where
    if old_bm is not None:
        try:
            bm = zmdfmt.BinMesh.decode(old_bm.data or b"")
        except FormatError as e:
            raise GltfImportError(f"{where}: stored BinMesh: {e}") from e
        if _binmesh_consistent(bm, new_by_slot, nslots) and nv == old_nv:
            rep.binmesh[key] = "kept"
        else:
            form = binmesh if binmesh != "auto" else ("strips" if bm.flags == 1 else "lists")
            geo.bin_mesh = _make_binmesh(new_by_slot, nslots, form)
            rep.binmesh[key] = form
            if form == "strips":
                geo.flags |= zmdfmt.GEO_TRISTRIP
            else:
                geo.flags &= ~zmdfmt.GEO_TRISTRIP
            rep.change(f"{where}: BinMesh regenerated as {form}")
    if bones is not None:
        geo.bones = bones
    geo.ext = ext
    return geo


def _rgba8(c):
    vals = list(c) + [1.0] * (4 - len(c))
    if all(isinstance(v, int) for v in vals) and max(vals) > 1:
        return tuple(max(0, min(255, int(v))) for v in vals[:4])
    return tuple(max(0, min(255, int(round(float(v) * 255.0)))) for v in vals[:4])


def _face_normals(P, tris):
    acc = [[0.0, 0.0, 0.0] for _ in P]
    for a, b, c in tris:
        u = [P[b][k] - P[a][k] for k in range(3)]
        v = [P[c][k] - P[a][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for i in (a, b, c):
            for k in range(3):
                acc[i][k] += n[k]
    return [_norm(n) for n in acc]


def _sphere(P, old):
    """Keep the stored sphere if it is still the bbox-centred enclosing sphere of ``P``;
    otherwise centre = bbox centre, radius = max distance (+ a hair, so f32 rounding
    never leaves a vertex outside). [V] all 410 corpus spheres are bbox-centred and enclose
    every vertex."""
    mn = [min(p[k] for p in P) for k in range(3)]
    mx = [max(p[k] for p in P) for k in range(3)]
    c = [(mn[k] + mx[k]) / 2 for k in range(3)]
    scale = max(1e-6, max(mx[k] - mn[k] for k in range(3)))
    if old and len(old) == 4 and all(abs(c[k] - old[k]) <= 1e-5 * scale + 1e-7 for k in range(3)):
        rmax = max(math.dist(p, old[:3]) for p in P)
        if old[3] >= rmax * (1 - 1e-5) - 1e-7:
            return tuple(old)
    r = max(math.dist(p, c) for p in P)
    return (_f32(c[0]), _f32(c[1]), _f32(c[2]), _f32(r * (1 + 1e-5) + 1e-7))


def _binmesh_consistent(bm, by_slot, nslots):
    """True if the stored BinMesh draws exactly the imported triangles (same winding) per slot.
    [V] holds for all 720 (geometry, material) pairs of the unedited corpus."""
    got = {}
    for mid, ix in bm.meshes:
        tl = strip_triangles(ix) if bm.flags == 1 else [tuple(ix[k:k + 3]) for k in range(0, len(ix) - 2, 3)]
        got.setdefault(mid, set()).update(_canon(t) for t in tl)
    for mid in set(got) | set(by_slot):
        want = {_canon(t) for t in by_slot.get(mid, []) if len(set(t)) == 3}
        if got.get(mid, set()) != want:
            return False
    return True


def _make_binmesh(by_slot, nslots, form):
    meshes = []
    for mid in range(nslots):
        tl = by_slot.get(mid, [])
        if form == "strips":
            ix = stripify(tl)
        else:
            ix = [v for t in tl for v in t]
        meshes.append((mid, ix))
    return zmdfmt.BinMesh(1 if form == "strips" else 0, meshes)


def _bone_ranges(vf, old_bone, frames, where, rep, old_nv):
    """Vertex->frame list -> (permutation or None, Bone entries). Keeps the stored entry order
    (it is the authoring order, not the frame order) and sorts vertices by it."""
    d = old_bone.data or b""
    if len(d) % 12:
        raise GltfImportError(f"{where}: stored Bone plugin has a bad size")
    entries = [struct.unpack_from("<3I", d, k) for k in range(0, len(d), 12)]
    fid = [f.anim_id for f in frames]
    # vertex -> frame implied by the stored ranges
    id2frame = {}
    for i, f in enumerate(frames):
        if f.anim_id is not None:
            id2frame.setdefault(f.anim_id, i)
    old_map = [None] * old_nv
    for first, count, bid in entries:
        for v in range(first, min(first + count, old_nv)):
            old_map[v] = id2frame.get(bid)
    if len(vf) == old_nv and old_map == vf:
        return None, None                      # unchanged: keep the chunk byte-for-byte
    for fr in set(vf):
        if fid[fr] is None or fid[fr] == zmdfmt.NO_ID:
            raise GltfImportError(f"{where}: vertices assigned to frame {fr}, which has no bone id")
    rank_of = {}
    slots = []                                 # list of bone ids in entry order (None = unused slot)
    for first, count, bid in entries:
        if count and bid not in rank_of:
            rank_of[bid] = len(slots)
            slots.append(bid)
        else:
            slots.append(None)
    for fr in sorted(set(vf)):
        bid = fid[fr]
        if bid not in rank_of:
            k = slots.index(None) if None in slots else len(slots)
            if k == len(slots):
                slots.append(None)
                rep.warn(f"{where}: Bone plugin grows to {len(slots)} entries for newly skinned bone "
                         f"{bid:#x} (frame {fr})")
            slots[k] = bid
            rank_of[bid] = k
    key = [rank_of[fid[fr]] for fr in vf]
    perm = sorted(range(len(vf)), key=lambda v: key[v])
    if perm == list(range(len(vf))):
        perm = None
    counts = {}
    for k in key:
        counts[k] = counts.get(k, 0) + 1
    out, first, emptied = [], 0, []
    for k, bid in enumerate(slots):
        n = counts.get(k, 0)
        if bid is None or n == 0:
            if bid is not None:
                emptied.append(bid)
            out.append((0, 0, 0))
        else:
            out.append((first, n, bid))
            first += n
    if emptied:
        rep.warn(f"{where}: {len(emptied)} bone(s) lost all their vertices; their Bone entries are now empty")
    rep.change(f"{where}: Bone plugin ranges rebuilt from JOINTS_0")
    return perm, out


# -- textures -------------------------------------------------------------------

def _import_mati(g, it, rep, palettised="promote"):
    try:
        png = g.image(it["image"])
        tag = it["img_tag"]
        madf = _unb64(it["madf"])
        pad = _unb64(it["pad"])
    except KeyError as e:
        raise GltfImportError(f"MATI extras: missing {e}") from e
    if tag not in ("TBMP", "TPNG"):
        raise GltfImportError(f"MATI extras: unknown image tag {tag!r}")
    name = g.get("images", it["image"]).get("name", f"image{it['image']}")
    edited = hashlib.sha256(png).hexdigest() != it.get("png_sha256")
    try:
        if tag == "TPNG":
            img = _tpng(png, name, rep) if edited else png
        else:
            header, tail = _unb64(it["bmp_header"]), _unb64(it["bmp_tail"])
            img = _tbmp(png, header, tail, name, rep, palettised) if edited else imaging.png_to_bmp(png, header, tail)
    except imaging.ImageError as e:
        raise GltfImportError(f"texture {name}: {e}") from e
    if edited:
        rep.change(f"texture {name}: edited ({tag})")
        pad = b"\0" * (-len(img) % 4)
    ch = Chunk(b"MATI", children=[Chunk(b"MADF", data=madf), Chunk(tag.encode("ascii"), data=img + pad)])
    try:
        return zmdfmt.Mati.from_chunk(ch)
    except FormatError as e:
        raise GltfImportError(f"texture {name}: {e}") from e


def _pow2(n):
    return n > 0 and n & (n - 1) == 0


def _check_dims(w, h, name, rep, old=None):
    if old and (w, h) != old:
        rep.warn(f"texture {name}: size changed {old[0]}x{old[1]} -> {w}x{h}")
    if not (_pow2(w) and _pow2(h)):
        rep.warn(f"texture {name}: {w}x{h} is not a power of two (every corpus texture is; "
                 "DirectX 7 era hardware may refuse it)")
    if max(w, h) > 256:
        rep.warn(f"texture {name}: {w}x{h} is larger than any corpus texture (max 256)")


def _tpng(png, name, rep):
    img = imaging.png_decode(png)
    _check_dims(img["width"], img["height"], name, rep)
    if img["color_type"] == 6 and img["bit_depth"] == 8 and imaging.png_info(png)["interlace"] == 0:
        return bytes(png)                      # same pixel format as every corpus TPNG: write it as-is
    rgba = imaging.png_to_rgba_rows(img)
    rep.warn(f"texture {name}: PNG re-encoded as 8-bit RGBA (the corpus TPNG format)")
    return imaging.png_encode(img["width"], img["height"], rgba, color_type=6, bit_depth=8)


def _keyed_rgb_rows(rgba_rows):
    """RGBA rows -> RGB rows; pixels with alpha < 128 become the colour key (DM-014 rule)."""
    out, keyed = [], 0
    key = bytes(COLOR_KEY)
    for r in rgba_rows:
        b = bytearray(len(r) // 4 * 3)
        b[0::3], b[1::3], b[2::3] = r[0::4], r[1::4], r[2::4]
        for x, a in enumerate(r[3::4]):
            if a < 128:
                b[3 * x:3 * x + 3] = key
                keyed += 1
        out.append(bytes(b))
    return out, keyed


def _tbmp(png, header, tail, name, rep, palettised="promote"):
    """Edited TBMP. 24-bit slots keep their header (resized if needed). Palettised slots keep their
    indices when painted with the same palette; otherwise they are promoted to a 24-bit BMP (DM-014: the
    loader reads any 4/8/24-bit BMP in any slot) or, with ``palettised='quantize'``, mapped to the original
    palette. Alpha < 128 becomes the colour key 255,0,255 (transparent in game)."""
    info = imaging.bmp_info(_fake_bmp(header))
    img = imaging.png_decode(png)
    w, h = img["width"], img["height"]
    _check_dims(w, h, name, rep, (info.width, info.height))
    same_size = (w, h) == (info.width, info.height)
    if info.bpp <= 8:
        pal = info.palette
        if img["color_type"] == 3 and img["bit_depth"] == info.bpp and                 [tuple(c) for c in img["palette"][:len(pal)]] == [tuple(c) for c in pal]:
            newpng = imaging.png_encode(w, h, img["rows"], color_type=3, bit_depth=info.bpp,
                                        palette=list(pal) + [(0, 0, 0)] * ((1 << info.bpp) - len(pal)))
        elif palettised == "quantize":
            rows = imaging.quantize_rows(imaging.png_to_rgba_rows(img), w, pal, info.bpp)
            rep.warn(f"texture {name}: {info.bpp}-bit palettised BMP; edited colours were mapped to the "
                     f"nearest of its {len(pal)} original palette colours")
            newpng = imaging.png_encode(w, h, rows, color_type=3, bit_depth=info.bpp,
                                        palette=list(pal) + [(0, 0, 0)] * ((1 << info.bpp) - len(pal)))
        else:
            from dmbmx.formats import texture
            rgba = b"".join(imaging.png_to_rgba_rows(img))
            rep.change(f"texture {name}: {info.bpp}-bit palettised BMP promoted to 24-bit (DM-014: any "
                       "4/8/24-bit BMP loads in any slot)")
            return texture.encode_bmp(texture.Image(w, h, rgba), key=COLOR_KEY)
    else:
        rgb, keyed = _keyed_rgb_rows(imaging.png_to_rgba_rows(img))
        if keyed:
            rep.change(f"texture {name}: {keyed} transparent pixels written as the colour key 255,0,255")
        newpng = imaging.png_encode(w, h, rgb, color_type=2, bit_depth=8)
    hdr = imaging.bmp_header_resized(header, w, h) if not same_size else header
    return imaging.png_to_bmp(newpng, hdr, tail if same_size else b"")


def _fake_bmp(header):
    """bmp_info needs pixel bytes to exist; pad the stored header with zero pixels."""
    w, h, _, bpp = struct.unpack_from("<iiHH", header, 18)
    row = ((w * bpp + 31) // 32) * 4
    if not (0 < w <= imaging.MAX_DIM and 0 < abs(h) <= imaging.MAX_DIM):
        raise GltfImportError("stored BMP header has bad dimensions")
    return header + bytes(row * abs(h))

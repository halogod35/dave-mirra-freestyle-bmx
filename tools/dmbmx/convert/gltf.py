"""ZMD -> glTF 2.0 binary (.glb) exporter, written by hand (pure stdlib, no pygltflib).

Spec of the output (axes, node layout, ``extras``): ``docs/tools/export.md``.

Summary:
  * **Axes:** RW model space is used as glTF space unchanged (the game world is Y-up, +Z forward,
    right-handed, like glTF). Riders' bind pose is stored lying along +Z (the Hips frame rotates
    the skeleton's local Y-up to model +Z), so for those the model root node gets a -90 deg X
    rotation ("upright correction"). Vertex and frame data are never modified.
  * **Nodes:** model root -> one node per LOD -> frame hierarchy (one node per RwFrame, local
    matrix) -> rigid mesh nodes (bikes) under their frame; skinned meshes (riders) sit under the
    LOD node and use a glTF skin whose joints are all frames (joint index == frame index).
  * **Lossless:** everything that is not geometry/texture pixels is stored as base64 in
    ``extras.dmbmx`` on the nodes (and the source SHA-256 on the root). :func:`restore_zmd`
    rebuilds the original ZMD byte-for-byte from an unedited .glb (the DM-021 baseline).
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import struct

from dmbmx.convert import imaging
from dmbmx.core.chunks import RW, Chunk, parse_rw
from dmbmx.core.errors import FormatError
from dmbmx.formats import zmd as zmdfmt

__all__ = ["export_glb", "read_glb", "restore_zmd", "upright_correction", "GltfError", "EXTRAS_KEY",
           "EXTRAS_VERSION"]

EXTRAS_KEY = "dmbmx"
EXTRAS_VERSION = 1
GENERATOR = "dmbmx export (DM-020)"

# glTF enums
FLOAT, UBYTE, USHORT, UINT = 5126, 5121, 5123, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
_COMP_FMT = {FLOAT: "f", UBYTE: "B", USHORT: "H", UINT: "I"}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

# -90 deg about X: model +Z (rider up) -> glTF +Y, model -Y (rider forward) -> glTF +Z.
_S = math.sqrt(0.5)
UPRIGHT_ROTATIONS = {"rx-90": [-_S, 0.0, 0.0, _S], "rx+90": [_S, 0.0, 0.0, _S]}


class GltfError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="GLB", offset=offset)


def _b64(b: bytes) -> str:
    return base64.b64encode(bytes(b)).decode("ascii")


def _unb64(s: str) -> bytes:
    return base64.b64decode(s.encode("ascii"), validate=True)


def _colmajor(m):
    return [float(m[r][c]) for c in range(4) for r in range(4)]


def _is_identity(m, eps=0.0):
    return all(abs(m[r][c] - (1.0 if r == c else 0.0)) <= eps for r in range(4) for c in range(4))


# ------------------------------------------------------------------------------- axes

def upright_correction(model, lod=0):
    """Name of the root rotation that stands the model up in glTF (Y-up), or None.

    Rule [V on the corpus]: for a skinned LOD, take the first child of the root frame (the
    Hips); if its world-space local-Y axis points along model +Z (or -Z), the bind pose lies
    along Z, so rotate -90 (+90) deg about X. Bikes, sfx, ShadowVolume and the test asset
    DaveMirra.ZMD are already Y-up and get None.
    """
    lods = model.lods
    if not lods or lods[lod].clump is None:
        return None
    cl = lods[lod].clump
    if not any(a.geometry.bones for a in cl.atomics):
        return None
    worlds = cl.world_matrices()
    roots = [i for i, f in enumerate(cl.frames) if f.parent < 0]
    kids = [i for i, f in enumerate(cl.frames) if roots and f.parent == roots[0]]
    if not kids:
        return None
    w = worlds[kids[0]]
    up_z = w[2][1]
    if up_z > 0.9:
        return "rx-90"
    if up_z < -0.9:
        return "rx+90"
    return None


# ------------------------------------------------------------------------------- builder

class _Builder:
    def __init__(self):
        self.bin = bytearray()
        self.g = {"asset": {"version": "2.0", "generator": GENERATOR}, "scene": 0, "scenes": [{"nodes": []}],
                  "nodes": [], "meshes": [], "materials": [], "textures": [], "images": [], "samplers": [],
                  "accessors": [], "bufferViews": [], "buffers": [], "skins": []}

    def view(self, data: bytes, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.g["bufferViews"].append(bv)
        return len(self.g["bufferViews"]) - 1

    def accessor(self, values, comp, typ, *, target=None, normalized=False, minmax=False):
        """``values``: flat list of numbers (len = count * ncomp)."""
        n = _NCOMP[typ]
        if not values or len(values) % n:
            raise ValueError(f"accessor {typ}: {len(values)} values")
        count = len(values) // n
        data = struct.pack(f"<{len(values)}{_COMP_FMT[comp]}", *values)
        acc = {"bufferView": self.view(data, target), "componentType": comp, "count": count, "type": typ}
        if normalized:
            acc["normalized"] = True
        if minmax:
            if comp == FLOAT:  # min/max of the float32 values actually stored
                vals = struct.unpack(f"<{len(values)}f", data)
            else:
                vals = values
            acc["min"] = [min(vals[i::n]) for i in range(n)]
            acc["max"] = [max(vals[i::n]) for i in range(n)]
        self.g["accessors"].append(acc)
        return len(self.g["accessors"]) - 1

    def node(self, node, parent=None):
        self.g["nodes"].append(node)
        i = len(self.g["nodes"]) - 1
        if parent is None:
            self.g["scenes"][0]["nodes"].append(i)
        else:
            self.g["nodes"][parent].setdefault("children", []).append(i)
        return i

    def finish(self) -> bytes:
        g = self.g
        while len(self.bin) % 4:
            self.bin.append(0)
        g["buffers"] = [{"byteLength": len(self.bin)}]
        for k in ("meshes", "materials", "textures", "images", "samplers", "accessors", "bufferViews", "skins"):
            if not g[k]:
                del g[k]
        if not self.bin:
            del g["buffers"]
        js = json.dumps(g, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("utf-8")
        js += b" " * (-len(js) % 4)
        out = bytearray(struct.pack("<III", 0x46546C67, 2, 0))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js
        if self.bin:
            out += struct.pack("<II", len(self.bin), 0x004E4942) + self.bin
        struct.pack_into("<I", out, 8, len(out))
        return bytes(out)


# ------------------------------------------------------------------------------- export

def _material_key(m, tex_index):
    return (tex_index, tuple(m.color) if m is not None else (255, 255, 255, 255))


def export_glb(model, *, source: bytes | None = None, source_name: str | None = None, lods=None,
               upright: bool = True) -> bytes:
    """Convert a :class:`~dmbmx.formats.zmd.Zmd` to .glb bytes.

    ``source``: the original file bytes (for the SHA-256 in ``extras``; defaults to
    ``model.write()``). ``lods``: LOD indices to export (default: all; a partial export is
    flagged ``complete: false`` and cannot be restored losslessly). ``upright``: apply the
    rider upright correction (see :func:`upright_correction`).
    """
    if model.empty or not model.lods:
        raise ValueError("empty model: nothing to export")
    if source is None:
        source = model.write()
    all_lods = model.lods
    lods = list(range(len(all_lods))) if lods is None else list(lods)
    for li in lods:
        if not 0 <= li < len(all_lods):
            raise ValueError(f"LOD {li} out of range (model has {len(all_lods)})")
        if all_lods[li].clump is None:
            raise ValueError(f"LOD {li}: MDFF not decoded")
    b = _Builder()
    g = b.g
    name = (model.mdef.name if model.mdef and model.mdef.name else None) or (source_name or "zmd").rsplit(".", 1)[0]
    corr = upright_correction(model, lods[0]) if upright else None

    root_extras = {"format": "dmbmx-zmd", "version": EXTRAS_VERSION,
                   "source": {"name": source_name, "size": len(source),
                              "sha256": hashlib.sha256(source).hexdigest()},
                   "axis": {"rw_to_gltf": "identity", "root_correction": corr},
                   "complete": lods == list(range(len(all_lods))), "items": []}
    root = {"name": name, "extras": {EXTRAS_KEY: root_extras}}
    if corr:
        root["rotation"] = UPRIGHT_ROTATIONS[corr]
    root_i = b.node(root)

    # -- textures: one image per MATI, in file order
    matis = model.materials()
    tex_of = {}
    mati_extras = []
    if matis:
        g["samplers"].append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})
    for k, t in enumerate(matis):
        ex = {"type": "MATI", "mati": k, "image": k, "madf": _b64(_madf_bytes(t)),
              "img_tag": (t.img_tag or b"TBMP").decode("ascii"), "pad": _b64(t.pad)}
        if t.kind == "png":
            png = bytes(t.image)
        else:
            info = imaging.bmp_info(t.image)
            png = imaging.bmp_to_png(t.image)
            ex["bmp_header"] = _b64(info.header)
            ex["bmp_tail"] = _b64(info.tail)
        ex["png_sha256"] = hashlib.sha256(png).hexdigest()
        alpha = imaging.png_has_alpha(png)
        g["images"].append({"name": t.name, "mimeType": "image/png", "bufferView": b.view(png)})
        g["textures"].append({"sampler": 0, "source": k, "name": t.name})
        tex_of[t.name.upper()] = (k, alpha)
        mati_extras.append(ex)

    mat_cache = {}

    def gl_material(m):
        tex = None
        if m is not None and m.texture is not None and m.texture.name:
            tex = tex_of.get(m.texture.name.upper())
        key = _material_key(m, tex[0] if tex else None)
        if key in mat_cache:
            return mat_cache[key]
        col = [c / 255.0 for c in key[1]]
        pbr = {"baseColorFactor": col, "metallicFactor": 0.0, "roughnessFactor": 1.0}
        mat = {"name": (m.texture.name if m is not None and m.texture is not None and m.texture.name
                        else "untextured"), "pbrMetallicRoughness": pbr}
        alpha = tex[1] if tex else None
        if tex:
            pbr["baseColorTexture"] = {"index": tex[0]}
        if alpha == "blend" or key[1][3] < 255:
            mat["alphaMode"] = "BLEND"
        elif alpha == "mask":
            mat["alphaMode"] = "MASK"
        g["materials"].append(mat)
        mat_cache[key] = len(g["materials"]) - 1
        return mat_cache[key]

    raw_frames = _raw_frame_records(source)

    # -- LODs
    lod_node = {}
    for li in lods:
        lo = all_lods[li]
        cl = lo.clump
        ver = lo.rw_version
        lod_ex = {"lod": li, "mldf": list(lo.mldf), "rw_version": ver,
                  "clump_struct_extra": _b64(cl.struct_extra),
                  "clump_ext": _b64(b"".join(c.to_bytes() for c in cl.ext)),
                  "frames": len(cl.frames), "atomic_nodes": []}
        ln = b.node({"name": f"{name}_LOD{li}", "extras": {EXTRAS_KEY: lod_ex}}, root_i)
        lod_node[li] = ln
        skel = model.skeleton(li)
        frame_nodes = []
        for i, f in enumerate(cl.frames):
            rec = struct.pack("<12fiI", *f.right, *f.up, *f.at, *f.pos, f.parent, f.flags)
            raw = raw_frames.get((li, i))
            if raw is not None and struct.pack("<12fiI", *struct.unpack("<12fiI", raw)) == rec:
                rec = raw   # the file's exact bytes (survive hosts that flush denormal floats, e.g. Blender)
            fex = {"frame": i, "parent": f.parent, "anim_id": f.anim_id, "name_source": skel[i]["name_source"],
                   "record": _b64(rec),
                   "ext": _b64(b"".join(_frame_ext_chunks(f, ver)))}
            nd = {"name": skel[i]["name"], "extras": {EXTRAS_KEY: fex}}
            m = f.matrix()
            if not _is_identity(m):
                nd["matrix"] = _colmajor(m)
            parent = frame_nodes[f.parent] if 0 <= f.parent < i else ln
            frame_nodes.append(b.node(nd, parent))

        skin_i = None
        meshes = list(model.meshes(li, world=False))
        if any(ms["joints"] for ms in meshes):
            ibm = []
            for bone in skel:
                inv = bone["inverse_bind"] or [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
                ibm += _colmajor(inv)
            roots = [frame_nodes[i] for i, f in enumerate(cl.frames) if f.parent < 0 or f.parent >= i]
            g["skins"].append({"name": f"{name}_LOD{li}_skin", "joints": frame_nodes,
                               "inverseBindMatrices": b.accessor(ibm, FLOAT, "MAT4"),
                               "skeleton": roots[0]})
            skin_i = len(g["skins"]) - 1

        for ms, a in zip(meshes, cl.atomics):
            ai = ms["atomic"]
            geo = a.geometry
            gex = _geometry_extras(geo, ms, ver)
            aex = {"atomic": ai, "frame": a.frame, "flags": a.flags, "unused": a.unused,
                   "ext": _b64(b"".join(c.to_bytes() for c in a.ext)), "geometry": gex}
            fname = skel[a.frame]["name"]
            nd = {"name": f"{fname}_mesh{ai}", "extras": {EXTRAS_KEY: aex}}
            if ms["positions"] and ms["indices"]:
                nd["mesh"] = _add_mesh(b, ms, gl_material, geo, f"{name}_LOD{li}_atomic{ai}")
            if ms["joints"] and skin_i is not None and "mesh" in nd:
                nd["skin"] = skin_i
                ni = b.node(nd, ln)  # skinned: transform comes from the joints only
            else:
                ni = b.node(nd, frame_nodes[a.frame])
            lod_ex["atomic_nodes"].append(ni)

    # -- MODL children in file order
    lod_seen = 0
    for it in model.items:
        if isinstance(it, zmdfmt.Lod):
            root_extras["items"].append({"type": "MLOD", "lod": lod_seen, "node": lod_node.get(lod_seen)})
            lod_seen += 1
        elif isinstance(it, zmdfmt.Mdef):
            root_extras["items"].append({"type": "MDEF", "data": _b64(it.encode())})
        elif isinstance(it, zmdfmt.Mati):
            root_extras["items"].append(mati_extras[matis.index(it)])
        else:
            root_extras["items"].append({"type": "chunk", "chunk": _b64(it.to_bytes())})
    return b.finish()


def _raw_frame_records(source):
    """{(lod, frame): 56 raw bytes} straight from the file (byte-level parse, no float decoding).
    Blender runs Python with denormals flushed to zero, so decoding and re-packing a frame there loses
    denormal values ([V] bike_hires.zmd has two); the raw bytes keep them. Empty dict on any problem."""
    from dmbmx.core.chunks import parse_4cc
    out = {}
    try:
        modl = parse_4cc(source, zmdfmt.CONTAINERS_4CC)[0]
        lods = [c for c in modl.children if c.tag == b"MLOD"]
        for li, ml in enumerate(lods):
            mdff = next(c for c in ml.children if c.tag == b"MDFF")
            clump = parse_rw(mdff.data)[0]
            fl = next(c for c in clump.children if c.tag == RW.FRAMELIST)
            st = fl.children[0].data
            n = struct.unpack_from("<I", st, 0)[0]
            if len(st) != 4 + 56 * n:
                continue
            for i in range(n):
                out[(li, i)] = bytes(st[4 + 56 * i:4 + 56 * (i + 1)])
    except (FormatError, StopIteration, IndexError, AttributeError, struct.error, TypeError):
        return {}
    return out


def _madf_bytes(t):
    return t.to_chunk().children[0].data


def _frame_ext_chunks(f, ver):
    # identical to what the FrameList writer emits (0x108 regenerated from anim_id)
    out = []
    for c in f.ext:
        if c.tag == RW.ANIMATION and f.anim_id is not None and len(c.data) == 8:
            c = Chunk(c.tag, data=struct.pack("<II", f.anim_id, f.anim_extra), version=c.version)
        out.append(c.to_bytes())
    return out


def _rle(seq):
    out = []
    for v in seq:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return out


def _geometry_extras(geo, ms, ver):
    ch = geo.to_chunk(ver)
    mt = geo.morph_targets
    return {"flags": geo.flags, "surface": list(geo.surface), "vertices": len(ms["positions"]),
            "triangles": len(geo.triangles),
            "morph_targets": [{"sphere": list(m.sphere), "has_vertices": m.vertices is not None,
                               "has_normals": m.normals is not None} for m in mt],
            "material_list": _b64(ch.children[1].to_bytes()),
            "ext": _b64(b"".join(c.to_bytes() for c in ch.children[2].children)),
            "tri_materials": _rle(ms["material_ids"])}


def _add_mesh(b, ms, gl_material, geo, name):
    P = ms["positions"]
    attrs = {"POSITION": b.accessor([c for p in P for c in p], FLOAT, "VEC3", target=ARRAY_BUFFER, minmax=True)}
    if ms["normals"] is not None:
        attrs["NORMAL"] = b.accessor([c for n in ms["normals"] for c in n], FLOAT, "VEC3", target=ARRAY_BUFFER)
    if ms["uvs"] is not None:
        attrs["TEXCOORD_0"] = b.accessor([c for t in ms["uvs"] for c in t], FLOAT, "VEC2", target=ARRAY_BUFFER)
    if ms["uvs2"] is not None:
        attrs["TEXCOORD_1"] = b.accessor([c for t in ms["uvs2"] for c in t], FLOAT, "VEC2", target=ARRAY_BUFFER)
    if ms["colors"] is not None:
        attrs["COLOR_0"] = b.accessor([c for col in ms["colors"] for c in col], UBYTE, "VEC4",
                                      target=ARRAY_BUFFER, normalized=True)
    if ms["joints"] is not None:
        nj = max(j[0] for j in ms["joints"]) + 1
        jt = UBYTE if nj <= 256 else USHORT
        attrs["JOINTS_0"] = b.accessor([v for j in ms["joints"] for v in (j[0], 0, 0, 0)], jt, "VEC4",
                                       target=ARRAY_BUFFER)
        attrs["WEIGHTS_0"] = b.accessor([v for w in ms["weights"] for v in (w[0], 0.0, 0.0, 0.0)], FLOAT, "VEC4",
                                        target=ARRAY_BUFFER)
    slots = geo.material_list()
    idx = ms["indices"]
    by_mat = {}
    for t, mid in enumerate(ms["material_ids"]):
        by_mat.setdefault(mid, []).extend(idx[3 * t:3 * t + 3])
    itype = USHORT if len(P) <= 65535 else UINT
    prims = []
    for mid in sorted(by_mat):
        prim = {"attributes": attrs, "indices": b.accessor(by_mat[mid], itype, "SCALAR",
                                                           target=ELEMENT_ARRAY_BUFFER, minmax=True),
                "mode": 4, "material": gl_material(slots[mid] if mid < len(slots) else None),
                "extras": {EXTRAS_KEY: {"material_slot": mid}}}
        prims.append(prim)
    b.g["meshes"].append({"name": name, "primitives": prims})
    return len(b.g["meshes"]) - 1


# ------------------------------------------------------------------------------- reading

def read_glb(data: bytes):
    """Split a .glb into ``(json_dict, bin_bytes)`` with header/chunk bounds checks."""
    if len(data) < 20:
        raise GltfError("file shorter than the GLB header", offset=0)
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        raise GltfError(f"bad GLB magic/version ({magic:#x}, {version})", offset=0)
    if length != len(data):
        raise GltfError(f"header length {length} != file size {len(data)}", offset=8)
    off, js, bin_ = 12, None, b""
    while off < len(data):
        if off + 8 > len(data):
            raise GltfError("truncated chunk header", offset=off)
        n, typ = struct.unpack_from("<II", data, off)
        if n % 4 or off + 8 + n > len(data):
            raise GltfError(f"chunk length {n} unaligned or past end", offset=off)
        body = data[off + 8:off + 8 + n]
        if typ == 0x4E4F534A and js is None:
            try:
                js = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError) as e:
                raise GltfError(f"JSON chunk: {e}", offset=off) from e
        elif typ == 0x004E4942:
            bin_ = bytes(body)
        off += 8 + n
    if js is None:
        raise GltfError("no JSON chunk", offset=12)
    return js, bin_


def accessor_values(js, bin_, i):
    """Unpack accessor ``i`` as a list of tuples (or scalars). Bounds-checked."""
    acc = js["accessors"][i]
    bv = js["bufferViews"][acc["bufferView"]]
    n = _NCOMP[acc["type"]]
    fmt = _COMP_FMT[acc["componentType"]]
    size = struct.calcsize("<" + fmt) * n
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", size)
    end = start + stride * (acc["count"] - 1) + size
    if end > bv.get("byteOffset", 0) + bv["byteLength"] or end > len(bin_):
        raise GltfError(f"accessor {i} runs past its bufferView/buffer")
    vals = [struct.unpack_from(f"<{n}{fmt}", bin_, start + k * stride) for k in range(acc["count"])]
    return [v[0] for v in vals] if n == 1 else vals


def image_bytes(js, bin_, i):
    bv = js["bufferViews"][js["images"][i]["bufferView"]]
    o = bv.get("byteOffset", 0)
    return bin_[o:o + bv["byteLength"]]


# ------------------------------------------------------------------------------- restore

def restore_zmd(data: bytes) -> bytes:
    """Rebuild the original ZMD from an **unedited** .glb written by :func:`export_glb`.

    Uses the buffers for geometry (positions, normals, UVs, prelit colours, triangles) and
    texture pixels, and ``extras.dmbmx`` for everything else. This is the lossless baseline for
    the DM-021 importer; it does not support edits (topology, new materials, ...).
    """
    js, bin_ = read_glb(data)
    nodes = js["nodes"]
    root = next((n for n in nodes if EXTRAS_KEY in n.get("extras", {})
                 and n["extras"][EXTRAS_KEY].get("format") == "dmbmx-zmd"), None)
    if root is None:
        raise GltfError("no dmbmx root node (not exported by dmbmx?)")
    rx = root["extras"][EXTRAS_KEY]
    if rx.get("version") != EXTRAS_VERSION:
        raise GltfError(f"extras version {rx.get('version')} != {EXTRAS_VERSION}")
    if not rx.get("complete"):
        raise GltfError("partial export (--lod): cannot rebuild the whole file")
    items = []
    for it in rx["items"]:
        t = it["type"]
        if t == "MLOD":
            items.append(_restore_lod(js, bin_, nodes[it["node"]]))
        elif t == "MDEF":
            items.append(zmdfmt.Mdef.decode(_unb64(it["data"])))
        elif t == "MATI":
            png = image_bytes(js, bin_, it["image"])
            if it["img_tag"] == "TBMP":
                img = imaging.png_to_bmp(png, _unb64(it["bmp_header"]), _unb64(it["bmp_tail"]))
            else:
                img = png
            ch = Chunk(b"MATI", children=[Chunk(b"MADF", data=_unb64(it["madf"])),
                                          Chunk(it["img_tag"].encode("ascii"), data=img + _unb64(it["pad"]))])
            items.append(zmdfmt.Mati.from_chunk(ch))
        else:
            from dmbmx.core.chunks import parse_4cc
            items.append(parse_4cc(_unb64(it["chunk"]))[0])
    return zmdfmt.Zmd(items).write()


def _parse_chunks(b64):
    raw = _unb64(b64)
    return parse_rw(raw) if raw else []


def _restore_lod(js, bin_, ln):
    nodes = js["nodes"]
    lx = ln["extras"][EXTRAS_KEY]
    cl = zmdfmt.Clump()
    cl.struct_extra = _unb64(lx["clump_struct_extra"])
    cl.ext = _parse_chunks(lx["clump_ext"])
    frames = {}

    def walk(i):
        n = nodes[i]
        fx = n.get("extras", {}).get(EXTRAS_KEY, {})
        if "frame" in fx and "record" in fx:
            rec = struct.unpack("<12fiI", _unb64(fx["record"]))
            frames[fx["frame"]] = zmdfmt.Frame(rec[0:3], rec[3:6], rec[6:9], rec[9:12], rec[12], rec[13],
                                               ext=_parse_chunks(fx["ext"]))
        for c in n.get("children", []):
            walk(c)

    walk(nodes.index(ln))
    if sorted(frames) != list(range(lx["frames"])):
        raise GltfError(f"LOD {lx['lod']}: found frames {sorted(frames)[:5]}..., expected {lx['frames']}")
    cl.frames = [frames[i] for i in range(lx["frames"])]
    for ni in lx["atomic_nodes"]:
        ax = nodes[ni]["extras"][EXTRAS_KEY]
        geo = _restore_geometry(js, bin_, nodes[ni], ax["geometry"])
        cl.atomics.append(zmdfmt.Atomic(ax["frame"], geo, ax["flags"], ax["unused"], _parse_chunks(ax["ext"])))
    return zmdfmt.Lod(tuple(lx["mldf"]), cl, lx["rw_version"])


def _restore_geometry(js, bin_, node, gx):
    g = zmdfmt.Geometry()
    g.flags = gx["flags"]
    g.surface = tuple(gx["surface"])
    nv = gx["vertices"]
    prims = js["meshes"][node["mesh"]]["primitives"] if "mesh" in node else []
    attrs = prims[0]["attributes"] if prims else {}

    def attr(key):
        return accessor_values(js, bin_, attrs[key]) if key in attrs else None

    pos, nrm = attr("POSITION"), attr("NORMAL")
    uv0, uv1, col = attr("TEXCOORD_0"), attr("TEXCOORD_1"), attr("COLOR_0")
    g.prelit = [tuple(c) for c in col] if col is not None and g.flags & zmdfmt.GEO_PRELIT else None
    g.uv_sets = [list(u) for u in (uv0, uv1) if u is not None]
    by_slot = {}
    for p in prims:
        ix = accessor_values(js, bin_, p["indices"])
        by_slot[p["extras"][EXTRAS_KEY]["material_slot"]] = [ix[k:k + 3] for k in range(0, len(ix), 3)]
    cursor = {k: 0 for k in by_slot}
    tris = []
    for mid, count in gx["tri_materials"]:
        for _ in range(count):
            a, b_, c = by_slot[mid][cursor[mid]]
            cursor[mid] += 1
            tris.append((b_, a, mid, c))
    g.triangles = tris
    g.morph_targets = []
    for k, m in enumerate(gx["morph_targets"]):
        v = [tuple(p) for p in pos] if (m["has_vertices"] and k == 0 and pos is not None) else None
        n = [tuple(p) for p in nrm] if (m["has_normals"] and k == 0 and nrm is not None) else None
        g.morph_targets.append(zmdfmt.MorphTarget(tuple(m["sphere"]), v, n))
    if pos is not None and len(pos) != nv:
        raise GltfError(f"POSITION count {len(pos)} != {nv}")
    ml = parse_rw(_unb64(gx["material_list"]))[0]
    st = ml.children[0].data
    n = struct.unpack_from("<I", st, 0)[0]
    g.material_indices = list(struct.unpack_from(f"<{n}i", st, 4))
    g.materials = [zmdfmt._material_from_chunk(m) for m in ml.children[1:]]
    g.ext = _parse_chunks(gx["ext"])  # BinMesh/Bone/Morph kept verbatim (bin_mesh/bones stay None)
    return g

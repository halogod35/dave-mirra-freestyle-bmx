"""ZMD model files (riders, bikes, shadows, sfx): lossless reader/writer + geometry decoding.

Spec: ``docs/formats/ZMD.md``. Pure stdlib (vendored by the Blender add-on).

Quick use::

    from dmbmx.formats import zmd
    m = zmd.read(open("DaveMirra.ZMD", "rb").read())
    for mesh in m.meshes():               # LOD 0, plain lists
        mesh["positions"], mesh["indices"], mesh["joints"]
    m.skeleton()                          # bones: parent, name, matrices
    m.materials()                         # embedded textures (MATI: MADF + TBMP/TPNG)
    assert zmd.write(m) == original_bytes

File grammar (4CC chunks, ``u32 size`` excludes the 8-byte header)::

    MODL
      MLOD+          one per LOD: MLDF (8 bytes) + MDFF (RenderWare 3.0 clump stream)
      MDEF           model / skeleton name table
      MATI*          one per texture: MADF (material def) + TBMP (.bmp) | TPNG (.png)

The object model is *re-encoding*: :func:`write` rebuilds every decoded struct
(frames, geometry, materials, bin mesh, bone ranges, MDEF, MADF) from the Python
objects; only plugin chunks we do not decode (e.g. Morph ``0x105``) are kept as
raw :class:`~dmbmx.core.chunks.Chunk` objects. Edit the lists and write.

Conventions used by the accessors:
  * Matrices are 4x4 nested lists, **column-vector** convention (``p' = M @ p``),
    i.e. columns 0..2 are RW's right/up/at vectors and column 3 is the position.
  * Triangles are yielded **counter-clockwise** = file order ``(v1, v0, v2)``;
    the file stores ``u16 b, a, material, c`` (RW 3.x order).
"""
from __future__ import annotations

import struct

from dmbmx.core.chunks import (
    RW, Chunk, ChunkError, ChunkTruncatedError, Reader, Writer, parse_4cc, parse_rw, write_4cc,
)
from dmbmx.core.errors import BadMagicError, FormatError
from dmbmx.core.hash import zhash
from dmbmx.formats.mati import Mati, image_length as _image_length  # noqa: F401  (re-export)

__all__ = [
    "ZmdError", "Zmd", "Lod", "Clump", "Frame", "Atomic", "Geometry", "MorphTarget", "Material",
    "Texture", "BinMesh", "Mdef", "MdefEntry", "Mati", "read", "write", "name_hash",
    "GEO_TRISTRIP", "GEO_POSITIONS", "GEO_TEXTURED", "GEO_PRELIT", "GEO_NORMALS", "GEO_LIGHT",
    "GEO_MODULATE", "GEO_TEXTURED2", "BONE_NAMES", "NO_ID", "bone_name",
]

FMT = "ZMD"
CONTAINERS_4CC = (b"MODL", b"MLOD", b"MATI")

# RpGeometry flags (RW 3.x)
GEO_TRISTRIP = 0x01
GEO_POSITIONS = 0x02
GEO_TEXTURED = 0x04
GEO_PRELIT = 0x08
GEO_NORMALS = 0x10
GEO_LIGHT = 0x20
GEO_MODULATE = 0x40
GEO_TEXTURED2 = 0x80


class ZmdError(FormatError):
    """A ZMD-specific structural problem (wrong chunk where the grammar needs another)."""

    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


def _expect(chunk, tag, where):
    if chunk is None or chunk.tag != tag:
        got = "nothing" if chunk is None else chunk.name
        want = tag.decode() if isinstance(tag, bytes) else f"RW 0x{tag:X}"
        raise ZmdError(f"{where}: expected {want}, got {got}", offset=getattr(chunk, "offset", None))
    return chunk


def _kids(chunk, where):
    if chunk.children is None:
        raise ZmdError(f"{where}: {chunk.name} is not a valid container", offset=chunk.offset)
    return chunk.children


def name_hash(name) -> int:
    """Z-Axis name id (bone ids, MDEF/MADF ids, ZAL asset ids) = :func:`dmbmx.core.hash.zhash`
    (the engine's zHashName, VA 0x004828C0, DM-010). For ASCII names this equals
    ``(toupper(last) << 16) + sum((i+1) * toupper(c_i))`` mod 2**32.
    [V] matches 2205/2227 ids in the ZMD corpus; the 22 misses are renamed copies
    (``Hips01`` keeps the id of ``Hips``).
    """
    return zhash(name)


#: Frame "no bone id" sentinel (bike frame 5, ShadowVolume helper frame).
NO_ID = 0xFFFFFFFF

#: Candidate bone / part names for reverse hash lookup. [V] Every name occurs in a
#: corpus MDEF; the 26 marked in ZMD.md section 6 also occur as strings in NgBMX.exe.
BONE_NAMES = (
    "Skin", "Hips", "_Abdomen", "Chest", "Chestx", "Shoulders", "_LeftShoulder", "_RightShoulder",
    "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm", "RightHand",
    "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot",
    "Hipsdeck", "BackWheel", "FrontWheel", "Sprocket", "Handlebars", "SeatGrip", "LeftGrip",
    "RightGrip", "LeftPedal", "RightPedal", "SFXCube", "Cone01", "wheel",
)
_KNOWN = {}
for _n in BONE_NAMES:
    _KNOWN.setdefault(zhash(_n), _n)


def bone_name(bone_id, mdef_names=None, extra_names=(), index=None):
    """Name for a frame's bone id: the file's MDEF entry, else a candidate whose
    :func:`zhash` matches (``extra_names`` first, then :data:`BONE_NAMES`), else
    ``bone_0x%08X``. ``None``/:data:`NO_ID` give ``frame_<index>``.
    Returns ``(name, source)`` with source 'mdef' | 'hash' | 'unknown' | 'none'."""
    if bone_id is None or bone_id == NO_ID:
        return f"frame_{index}", "none"
    if mdef_names and bone_id in mdef_names:
        return mdef_names[bone_id], "mdef"
    for n in extra_names:
        if zhash(n) == bone_id:
            return n, "hash"
    if bone_id in _KNOWN:
        return _KNOWN[bone_id], "hash"
    return f"bone_0x{bone_id:08X}", "unknown"


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1")


def _pad_name(name: str, raw: bytes | None, size: int | None = None) -> bytes:
    """Encode ``name`` NUL-terminated; reuse ``raw`` (with its garbage padding) if unchanged."""
    if raw is not None and _cstr(raw) == name and (size is None or len(raw) == size):
        return raw
    b = name.encode("latin-1") + b"\0"
    if size is not None:
        if len(b) > size:
            raise ValueError(f"name {name!r} longer than {size - 1} bytes")
        return b.ljust(size, b"\0")
    return b.ljust((len(b) + 3) & ~3, b"\0")


# ------------------------------------------------------------------------------- matrices

def _mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _mat_inv_affine(m):
    """Inverse of an affine 4x4 (column-vector) matrix."""
    a, b, c = m[0][:3], m[1][:3], m[2][:3]
    det = (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
           + a[2] * (b[0] * c[1] - b[1] * c[0]))
    if abs(det) < 1e-20:
        raise ValueError("singular matrix")
    r = [[(b[1] * c[2] - b[2] * c[1]) / det, (a[2] * c[1] - a[1] * c[2]) / det, (a[1] * b[2] - a[2] * b[1]) / det],
         [(b[2] * c[0] - b[0] * c[2]) / det, (a[0] * c[2] - a[2] * c[0]) / det, (a[2] * b[0] - a[0] * b[2]) / det],
         [(b[0] * c[1] - b[1] * c[0]) / det, (a[1] * c[0] - a[0] * c[1]) / det, (a[0] * b[1] - a[1] * b[0]) / det]]
    t = [m[0][3], m[1][3], m[2][3]]
    out = [r[i] + [-sum(r[i][k] * t[k] for k in range(3))] for i in range(3)]
    return out + [[0.0, 0.0, 0.0, 1.0]]


def _xform_point(m, p):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3))


def _xform_dir(m, d):
    return tuple(m[i][0] * d[0] + m[i][1] * d[1] + m[i][2] * d[2] for i in range(3))


# ------------------------------------------------------------------------------- RW objects

class Frame:
    """One RwFrame (56-byte record in the FrameList struct) + its extension.

    ``right, up, at, pos``: 3-tuples (RW rows); ``parent``: frame index or -1;
    ``flags``: u32 (always 0 in the corpus). ``anim_id`` / ``anim_extra``: the
    two u32 of the ``Animation(0x108)`` extension = bone id (name hash) and 0.
    ``ext``: list of extension chunks (the 0x108 chunk is regenerated from
    ``anim_id``/``anim_extra``).
    """
    __slots__ = ("right", "up", "at", "pos", "parent", "flags", "anim_id", "anim_extra", "ext")

    def __init__(self, right=(1.0, 0.0, 0.0), up=(0.0, 1.0, 0.0), at=(0.0, 0.0, 1.0), pos=(0.0, 0.0, 0.0),
                 parent=-1, flags=0, anim_id=None, anim_extra=0, ext=None):
        self.right, self.up, self.at, self.pos = right, up, at, pos
        self.parent, self.flags = parent, flags
        self.anim_id, self.anim_extra = anim_id, anim_extra
        self.ext = ext if ext is not None else []

    def matrix(self):
        """Local 4x4 matrix (column-vector convention)."""
        r, u, a, p = self.right, self.up, self.at, self.pos
        return [[r[0], u[0], a[0], p[0]], [r[1], u[1], a[1], p[1]], [r[2], u[2], a[2], p[2]],
                [0.0, 0.0, 0.0, 1.0]]


class MorphTarget:
    """``sphere``: (x, y, z, radius); ``vertices`` / ``normals``: lists of 3-tuples or None."""
    __slots__ = ("sphere", "vertices", "normals")

    def __init__(self, sphere=(0.0, 0.0, 0.0, 0.0), vertices=None, normals=None):
        self.sphere, self.vertices, self.normals = sphere, vertices, normals


class Texture:
    """RwTexture: ``filter_addressing`` u32 (0x106 everywhere), ``name``, ``mask``.

    ``name_raw``/``mask_raw`` keep the on-disk String payloads (NUL + garbage
    padding to 4) so unchanged names re-encode byte-identically.
    """
    __slots__ = ("filter_addressing", "name", "mask", "name_raw", "mask_raw", "ext")

    def __init__(self, name="", mask="", filter_addressing=0x106, name_raw=None, mask_raw=None, ext=None):
        self.filter_addressing, self.name, self.mask = filter_addressing, name, mask
        self.name_raw, self.mask_raw = name_raw, mask_raw
        self.ext = ext if ext is not None else []


class Material:
    """RpMaterial: ``flags`` u32, ``color`` RGBA bytes, ``unused`` u32 (stale pointer),
    ``textured`` u32, ``surface`` (ambient, specular, diffuse) or None (only present
    in RW 0x303 files), ``texture`` :class:`Texture` or None, ``ext`` chunks."""
    __slots__ = ("flags", "color", "unused", "textured", "surface", "texture", "ext")

    def __init__(self, color=(255, 255, 255, 255), texture=None, flags=0, unused=0, textured=None,
                 surface=None, ext=None):
        self.flags, self.color, self.unused = flags, tuple(color), unused
        self.textured = (1 if texture else 0) if textured is None else textured
        self.surface, self.texture = surface, texture
        self.ext = ext if ext is not None else []


class BinMesh:
    """Mesh-split plugin (0x50E): ``flags`` (1 = triangle strips, 0 = lists),
    ``meshes``: list of ``(material_index, [u32 indices])``."""
    __slots__ = ("flags", "meshes")

    def __init__(self, flags=0, meshes=None):
        self.flags, self.meshes = flags, meshes if meshes is not None else []

    @classmethod
    def decode(cls, data, base=0):
        r = Reader(data, base=base, fmt=FMT, what="BinMesh")
        flags, n, total = r.unpack("III")
        meshes, seen = [], 0
        for _ in range(n):
            ni, mat = r.unpack("II")
            meshes.append((mat, r.array("I", ni)))
            seen += ni
        if seen != total or r.remaining():
            raise ZmdError(f"BinMesh: index total {seen} != header {total} or {r.remaining()} stray bytes",
                           offset=base)
        return cls(flags, meshes)

    def encode(self):
        w = Writer()
        w.pack("III", self.flags, len(self.meshes), sum(len(ix) for _, ix in self.meshes))
        for mat, ix in self.meshes:
            w.pack("II", len(ix), mat)
            w.array("I", ix)
        return w.getvalue()


class Geometry:
    """RpGeometry (RW 3.0 layout, no native data).

    ``flags``: u32 (GEO_* bits); ``surface``: (ambient, specular, diffuse);
    ``prelit``: list of RGBA 4-tuples or None; ``uv_sets``: list of lists of (u, v);
    ``triangles``: list of file-order tuples ``(b, a, material, c)``;
    ``morph_targets``: list of :class:`MorphTarget` (exactly 1 in the corpus);
    ``materials``: list of :class:`Material`; ``material_indices``: the MaterialList
    struct entries (-1 = "new material", else index of a previous one);
    ``bin_mesh``: :class:`BinMesh` or None; ``bones``: list of
    ``(first_vertex, vertex_count, bone_id)`` from the Bone (0x10E) plugin, or None;
    ``ext``: extension chunks in file order (BinMesh/Bone payloads are regenerated).
    """
    __slots__ = ("flags", "surface", "prelit", "uv_sets", "triangles", "morph_targets", "materials",
                 "material_indices", "bin_mesh", "bones", "ext")

    def __init__(self):
        self.flags = GEO_POSITIONS
        self.surface = (1.0, 1.0, 1.0)
        self.prelit = None
        self.uv_sets = []
        self.triangles = []
        self.morph_targets = []
        self.materials = []
        self.material_indices = []
        self.bin_mesh = None
        self.bones = None
        self.ext = []

    @property
    def num_vertices(self):
        mt = self.morph_targets[0] if self.morph_targets else None
        if mt and mt.vertices is not None:
            return len(mt.vertices)
        if self.uv_sets:
            return len(self.uv_sets[0])
        return len(self.prelit or ())

    # -- struct ------------------------------------------------------------
    def _decode_struct(self, data, base):
        r = Reader(data, base=base, fmt=FMT, what="Geometry struct")
        flags, nt, nv, nm = r.unpack("IIII")
        self.flags = flags
        self.surface = r.unpack("3f")
        self.prelit = r.tuples("4B", nv) if flags & GEO_PRELIT else None
        nuv = 2 if flags & GEO_TEXTURED2 else (1 if flags & GEO_TEXTURED else 0)
        self.uv_sets = [r.tuples("2f", nv) for _ in range(nuv)]
        self.triangles = r.tuples("4H", nt)
        if nm > 1024:
            raise ZmdError(f"Geometry: {nm} morph targets", offset=base)
        self.morph_targets = []
        for _ in range(nm):
            sphere = r.unpack("4f")
            hv, hn = r.unpack("II")
            v = r.tuples("3f", nv) if hv else None
            n = r.tuples("3f", nv) if hn else None
            self.morph_targets.append(MorphTarget(sphere, v, n))
        if r.remaining():
            raise ZmdError(f"Geometry struct: {r.remaining()} stray bytes", offset=base + r.pos)
        for t in self.triangles:
            if t[0] >= nv or t[1] >= nv or t[3] >= nv:
                raise ZmdError(f"Geometry: triangle {t} indexes past {nv} vertices", offset=base)

    def _encode_struct(self):
        nv = self.num_vertices
        w = Writer()
        w.pack("IIII", self.flags, len(self.triangles), nv, len(self.morph_targets))
        w.pack("3f", *self.surface)
        if self.flags & GEO_PRELIT:
            _check_len(self.prelit, nv, "prelit")
            w.tuples("4B", self.prelit)
        nuv = 2 if self.flags & GEO_TEXTURED2 else (1 if self.flags & GEO_TEXTURED else 0)
        if len(self.uv_sets) != nuv:
            raise ValueError(f"geometry flags need {nuv} UV sets, have {len(self.uv_sets)}")
        for uv in self.uv_sets:
            _check_len(uv, nv, "uv set")
            w.tuples("2f", uv)
        w.tuples("4H", self.triangles)
        for mt in self.morph_targets:
            w.pack("4f", *mt.sphere)
            w.pack("II", int(mt.vertices is not None), int(mt.normals is not None))
            if mt.vertices is not None:
                _check_len(mt.vertices, nv, "vertices")
                w.tuples("3f", mt.vertices)
            if mt.normals is not None:
                _check_len(mt.normals, nv, "normals")
                w.tuples("3f", mt.normals)
        return w.getvalue()

    # -- chunk -------------------------------------------------------------
    @classmethod
    def from_chunk(cls, ch):
        g = cls()
        kids = _kids(ch, "Geometry")
        if len(kids) != 3:
            raise ZmdError(f"Geometry: expected Struct, MaterialList, Extension; got {len(kids)} chunks",
                           offset=ch.offset)
        st, ml, ex = kids
        _expect(st, RW.STRUCT, "Geometry")
        g._decode_struct(st.data or b"", st.offset + 12)
        _expect(ml, RW.MATLIST, "Geometry")
        mk = _kids(ml, "MaterialList")
        _expect(mk[0] if mk else None, RW.STRUCT, "MaterialList")
        r = Reader(mk[0].data, base=mk[0].offset + 12, fmt=FMT, what="MaterialList struct")
        n = r.u32()
        g.material_indices = r.array("i", n)
        if r.remaining():
            raise ZmdError("MaterialList struct: stray bytes", offset=mk[0].offset)
        mats = mk[1:]
        if len(mats) != sum(1 for i in g.material_indices if i < 0):
            raise ZmdError(f"MaterialList: {len(mats)} materials for indices {g.material_indices[:8]}...",
                           offset=ml.offset)
        g.materials = [_material_from_chunk(m) for m in mats]
        nmat = len(g.material_indices)
        for t in g.triangles:
            if t[2] >= nmat:
                raise ZmdError(f"Geometry: triangle material {t[2]} >= {nmat}", offset=st.offset)
        _expect(ex, RW.EXTENSION, "Geometry")
        g.ext = list(_kids(ex, "Geometry extension"))
        for c in g.ext:
            if c.tag == RW.BINMESH:
                g.bin_mesh = BinMesh.decode(c.data or b"", c.offset + 12)
            elif c.tag == RW.BONE:
                d = c.data or b""
                if len(d) % 12:
                    raise ZmdError(f"Bone plugin: size {len(d)} not a multiple of 12", offset=c.offset)
                g.bones = Reader(d).tuples("3I", len(d) // 12)
                nv = g.num_vertices
                for first, count, _ in g.bones:
                    if first + count > nv:
                        raise ZmdError(f"Bone plugin: range {first}+{count} past {nv} vertices", offset=c.offset)
        return g

    def to_chunk(self, ver):
        mats = [_material_to_chunk(m, ver) for m in self.materials]
        ml_struct = struct.pack(f"<I{len(self.material_indices)}i", len(self.material_indices),
                                *self.material_indices)
        ext = []
        for c in self.ext:
            if c.tag == RW.BINMESH and self.bin_mesh is not None:
                c = Chunk(c.tag, data=self.bin_mesh.encode(), version=c.version)
            elif c.tag == RW.BONE and self.bones is not None:
                w = Writer()
                w.tuples("3I", self.bones)
                c = Chunk(c.tag, data=w.getvalue(), version=c.version)
            ext.append(c)
        return Chunk(RW.GEOMETRY, version=ver, children=[
            Chunk(RW.STRUCT, data=self._encode_struct(), version=ver),
            Chunk(RW.MATLIST, version=ver, children=[Chunk(RW.STRUCT, data=ml_struct, version=ver)] + mats),
            Chunk(RW.EXTENSION, version=ver, children=ext),
        ])

    def material_list(self):
        """Resolve ``material_indices`` to a list of :class:`Material` (one per slot)."""
        out, it = [], iter(self.materials)
        for i in self.material_indices:
            out.append(next(it) if i < 0 else out[i] if 0 <= i < len(out) else None)
        return out


def _check_len(seq, n, what):
    if seq is None or len(seq) != n:
        raise ValueError(f"{what}: {0 if seq is None else len(seq)} entries, need {n}")


def _material_from_chunk(ch):
    kids = _kids(ch, "Material")
    _expect(kids[0] if kids else None, RW.STRUCT, "Material")
    d = kids[0].data
    r = Reader(d, base=kids[0].offset + 12, fmt=FMT, what="Material struct")
    flags = r.u32()
    color = r.unpack("4B")
    unused, textured = r.unpack("II")
    surface = r.unpack("3f") if r.remaining() == 12 else None
    if r.remaining():
        raise ZmdError(f"Material struct: unexpected size {len(d)}", offset=kids[0].offset)
    m = Material(color, None, flags, unused, textured, surface)
    rest = kids[1:]
    if rest and rest[0].tag == RW.TEXTURE:
        m.texture = _texture_from_chunk(rest[0])
        rest = rest[1:]
    if len(rest) != 1:
        raise ZmdError("Material: expected a single Extension after Struct/Texture", offset=ch.offset)
    m.ext = list(_kids(_expect(rest[0], RW.EXTENSION, "Material"), "Material extension"))
    return m


def _material_to_chunk(m, ver):
    w = Writer()
    w.pack("I4BII", m.flags, *m.color, m.unused, m.textured)
    if m.surface is not None:
        w.pack("3f", *m.surface)
    kids = [Chunk(RW.STRUCT, data=w.getvalue(), version=ver)]
    if m.texture is not None:
        kids.append(_texture_to_chunk(m.texture, ver))
    kids.append(Chunk(RW.EXTENSION, version=ver, children=list(m.ext)))
    return Chunk(RW.MATERIAL, version=ver, children=kids)


def _texture_from_chunk(ch):
    kids = _kids(ch, "Texture")
    if len(kids) != 4 or [k.tag for k in kids] != [RW.STRUCT, RW.STRING, RW.STRING, RW.EXTENSION]:
        raise ZmdError("Texture: expected Struct, String, String, Extension", offset=ch.offset)
    st, nm, mk, ex = kids
    if len(st.data) != 4:
        raise ZmdError("Texture struct: size != 4", offset=st.offset)
    return Texture(_cstr(nm.data), _cstr(mk.data), struct.unpack("<I", st.data)[0], nm.data, mk.data,
                   list(_kids(ex, "Texture extension")))


def _texture_to_chunk(t, ver):
    return Chunk(RW.TEXTURE, version=ver, children=[
        Chunk(RW.STRUCT, data=struct.pack("<I", t.filter_addressing), version=ver),
        Chunk(RW.STRING, data=_pad_name(t.name, t.name_raw), version=ver),
        Chunk(RW.STRING, data=_pad_name(t.mask, t.mask_raw), version=ver),
        Chunk(RW.EXTENSION, version=ver, children=list(t.ext)),
    ])


class Atomic:
    """RpAtomic: ``frame`` index, ``flags`` (5 = collision test | render), ``unused`` u32,
    inline :class:`Geometry` (RW 3.0: no GeometryList), ``ext`` chunks (empty in corpus)."""
    __slots__ = ("frame", "flags", "unused", "geometry", "ext")

    def __init__(self, frame=0, geometry=None, flags=5, unused=0, ext=None):
        self.frame, self.flags, self.unused = frame, flags, unused
        self.geometry = geometry if geometry is not None else Geometry()
        self.ext = ext if ext is not None else []


class Clump:
    """RpClump: ``frames`` (list of :class:`Frame`), ``atomics``, ``struct_extra``
    (bytes after numAtomics in the clump struct; empty in RW 3.0), ``ext`` chunks."""
    __slots__ = ("frames", "atomics", "struct_extra", "ext")

    def __init__(self):
        self.frames, self.atomics, self.struct_extra, self.ext = [], [], b"", []

    @classmethod
    def from_chunk(cls, ch):
        c = cls()
        kids = _kids(_expect(ch, RW.CLUMP, "MDFF"), "Clump")
        if len(kids) < 3:
            raise ZmdError("Clump: too few children", offset=ch.offset)
        st = _expect(kids[0], RW.STRUCT, "Clump")
        r = Reader(st.data, base=st.offset + 12, fmt=FMT, what="Clump struct")
        na = r.u32()
        c.struct_extra = r.rest()
        fl = _expect(kids[1], RW.FRAMELIST, "Clump")
        c.frames = _frames_from_chunk(fl)
        atoms = kids[2:-1]
        if len(atoms) != na or any(a.tag != RW.ATOMIC for a in atoms):
            raise ZmdError(f"Clump: struct says {na} atomics, found {[a.name for a in atoms][:4]}",
                           offset=ch.offset)
        for a in atoms:
            ak = _kids(a, "Atomic")
            if len(ak) != 3:
                raise ZmdError("Atomic: expected Struct, Geometry, Extension", offset=a.offset)
            ast = _expect(ak[0], RW.STRUCT, "Atomic")
            if len(ast.data) != 12:
                raise ZmdError(f"Atomic struct: size {len(ast.data)} != 12", offset=ast.offset)
            fi, flags, unused = struct.unpack("<III", ast.data)
            if fi >= len(c.frames):
                raise ZmdError(f"Atomic: frame {fi} >= {len(c.frames)}", offset=ast.offset)
            geo = Geometry.from_chunk(_expect(ak[1], RW.GEOMETRY, "Atomic"))
            ext = list(_kids(_expect(ak[2], RW.EXTENSION, "Atomic"), "Atomic extension"))
            c.atomics.append(Atomic(fi, geo, flags, unused, ext))
        c.ext = list(_kids(_expect(kids[-1], RW.EXTENSION, "Clump"), "Clump extension"))
        return c

    def to_chunk(self, ver):
        kids = [Chunk(RW.STRUCT, data=struct.pack("<I", len(self.atomics)) + self.struct_extra, version=ver),
                _frames_to_chunk(self.frames, ver)]
        for a in self.atomics:
            kids.append(Chunk(RW.ATOMIC, version=ver, children=[
                Chunk(RW.STRUCT, data=struct.pack("<III", a.frame, a.flags, a.unused), version=ver),
                a.geometry.to_chunk(ver),
                Chunk(RW.EXTENSION, version=ver, children=list(a.ext)),
            ]))
        kids.append(Chunk(RW.EXTENSION, version=ver, children=list(self.ext)))
        return Chunk(RW.CLUMP, version=ver, children=kids)

    def world_matrices(self):
        """World 4x4 matrix per frame (parents always precede children in the corpus)."""
        out = [None] * len(self.frames)

        def get(i, depth=0):
            if out[i] is None:
                f = self.frames[i]
                if f.parent < 0 or depth > len(self.frames):
                    out[i] = f.matrix()
                else:
                    out[i] = _mat_mul(get(f.parent, depth + 1), f.matrix())
            return out[i]

        for i in range(len(self.frames)):
            get(i)
        return out


def _frames_from_chunk(fl):
    kids = _kids(fl, "FrameList")
    st = _expect(kids[0] if kids else None, RW.STRUCT, "FrameList")
    r = Reader(st.data, base=st.offset + 12, fmt=FMT, what="FrameList struct")
    n = r.u32()
    recs = r.tuples("12fiI", n)
    if r.remaining():
        raise ZmdError("FrameList struct: stray bytes", offset=st.offset)
    exts = kids[1:]
    if len(exts) != n or any(e.tag != RW.EXTENSION for e in exts):
        raise ZmdError(f"FrameList: {n} frames but {len(exts)} extensions", offset=fl.offset)
    frames = []
    for i, (rec, e) in enumerate(zip(recs, exts)):
        parent = rec[12]
        if parent >= n or parent < -1:
            raise ZmdError(f"Frame {i}: parent {parent} out of range", offset=st.offset)
        f = Frame(rec[0:3], rec[3:6], rec[6:9], rec[9:12], parent, rec[13], ext=list(_kids(e, "Frame extension")))
        for c in f.ext:
            if c.tag == RW.ANIMATION and len(c.data) == 8:
                f.anim_id, f.anim_extra = struct.unpack("<II", c.data)
        frames.append(f)
    return frames


def _frames_to_chunk(frames, ver):
    w = Writer()
    w.u32(len(frames))
    for f in frames:
        w.pack("12fiI", *f.right, *f.up, *f.at, *f.pos, f.parent, f.flags)
    kids = [Chunk(RW.STRUCT, data=w.getvalue(), version=ver)]
    for f in frames:
        ext = []
        for c in f.ext:
            if c.tag == RW.ANIMATION and f.anim_id is not None and len(c.data) == 8:
                c = Chunk(c.tag, data=struct.pack("<II", f.anim_id, f.anim_extra), version=c.version)
            ext.append(c)
        kids.append(Chunk(RW.EXTENSION, version=ver, children=ext))
    return Chunk(RW.FRAMELIST, version=ver, children=kids)


# ------------------------------------------------------------------------------- Z-Axis chunks

class Lod:
    """One ``MLOD``: ``mldf`` = (u32 flags, u32 unknown) from the 8-byte MLDF
    (flags 7 / 3 / 0x1B seen), ``clump`` (:class:`Clump`), ``rw_version``
    (0x301 or 0x303; every RW chunk in one MDFF shares it).

    If the MDFF could not be decoded (``read(strict=False)`` only), ``clump`` is
    None and ``mdff_raw`` holds the payload verbatim.
    """
    __slots__ = ("mldf", "clump", "rw_version", "mdff_raw")

    def __init__(self, mldf=(7, 0), clump=None, rw_version=0x301, mdff_raw=None):
        self.mldf, self.clump, self.rw_version, self.mdff_raw = mldf, clump, rw_version, mdff_raw

    @classmethod
    def from_chunk(cls, ch, strict=True):
        kids = _kids(ch, "MLOD")
        if len(kids) != 2:
            raise ZmdError("MLOD: expected MLDF + MDFF", offset=ch.offset)
        mldf = _expect(kids[0], b"MLDF", "MLOD")
        mdff = _expect(kids[1], b"MDFF", "MLOD")
        if len(mldf.data) != 8:
            raise ZmdError(f"MLDF: size {len(mldf.data)} != 8", offset=mldf.offset)
        lod = cls(struct.unpack("<II", mldf.data))
        try:
            rws = parse_rw(mdff.data, base=mdff.offset + 8)
            if len(rws) != 1:
                raise ZmdError(f"MDFF: expected one Clump, got {len(rws)} chunks", offset=mdff.offset)
            vers = {c.version for c in _iter_rw(rws)}
            if len(vers) != 1:
                raise ZmdError(f"MDFF: mixed RW versions {sorted(map(hex, vers))}", offset=mdff.offset)
            lod.rw_version = vers.pop()
            lod.clump = Clump.from_chunk(rws[0])
        except FormatError:
            if strict:
                raise
            lod.clump, lod.mdff_raw = None, mdff.data
        return lod

    def mdff_bytes(self):
        if self.clump is None:
            return self.mdff_raw
        return self.clump.to_chunk(self.rw_version).to_bytes()

    def to_chunk(self):
        return Chunk(b"MLOD", children=[Chunk(b"MLDF", data=struct.pack("<II", *self.mldf)),
                                        Chunk(b"MDFF", data=self.mdff_bytes())])


def _iter_rw(chunks):
    for c in chunks:
        yield c
        if c.children:
            yield from _iter_rw(c.children)


class MdefEntry:
    """One 20-byte MDEF record: ``name`` (char[16]) + ``id`` (u32 name hash)."""
    __slots__ = ("name", "id", "name_raw")

    def __init__(self, name, id=None, name_raw=None):
        self.name = name
        self.id = name_hash(name) if id is None else id
        self.name_raw = name_raw

    def __repr__(self):
        return f"MdefEntry({self.name!r}, 0x{self.id:X})"


class Mdef:
    """``MDEF``: model header + name table (bones for riders, parts for bikes).

    Layout (44-byte header, then ``count`` x 20-byte :class:`MdefEntry`)::

        0x00 u32      kind      (2 or 0)
        0x04 char[16] name      ("DAVEMIRRA"; garbage after NUL kept)
        0x14 u32      name_id   (name_hash(name))
        0x18 f32[4]   params    [I] unknown (bounds / centre?)
        0x28 u16      count
        0x2A u16      unk       (0xFFFF or 0x0012, 0x7800 ...)
    """
    __slots__ = ("kind", "name", "name_raw", "name_id", "params", "unk", "entries")

    @classmethod
    def decode(cls, data, base=0):
        m = cls()
        r = Reader(data, base=base, fmt=FMT, what="MDEF")
        m.kind = r.u32()
        m.name_raw = r.bytes(16)
        m.name = _cstr(m.name_raw)
        m.name_id = r.u32()
        m.params = r.unpack("4f")
        count, m.unk = r.unpack("HH")
        m.entries = []
        for _ in range(count):
            raw = r.bytes(16)
            m.entries.append(MdefEntry(_cstr(raw), r.u32(), raw))
        if r.remaining():
            raise ZmdError(f"MDEF: {r.remaining()} stray bytes", offset=base + r.pos)
        return m

    def encode(self):
        w = Writer()
        w.u32(self.kind)
        w.raw(_pad_name(self.name, self.name_raw, 16))
        w.u32(self.name_id)
        w.pack("4f", *self.params)
        w.pack("HH", len(self.entries), self.unk)
        for e in self.entries:
            w.raw(_pad_name(e.name, e.name_raw, 16))
            w.u32(e.id)
        return w.getvalue()

    def to_chunk(self):
        return Chunk(b"MDEF", data=self.encode())

    def by_id(self):
        return {e.id: e.name for e in self.entries}


# Mati (MATI/MADF/TBMP/TPNG) lives in dmbmx.formats.mati (shared with level Mat._sd, DM-014).


# ------------------------------------------------------------------------------- Zmd

class Zmd:
    """A whole ZMD file. ``items`` keeps the MODL children in file order:
    :class:`Lod`, :class:`Mdef`, :class:`Mati` or raw :class:`Chunk` (unknown tags).

    ``empty`` is True for the zero-byte files shipped in the install
    (``TroyMcMurray1v2/1v3.ZMD``); they write back as zero bytes.
    """

    def __init__(self, items=None, empty=False):
        self.items = items if items is not None else []
        self.empty = empty

    # -- io ----------------------------------------------------------------
    @classmethod
    def read(cls, data, strict=True):
        data = bytes(data)
        if not data:
            return cls(empty=True)
        if data[:4] != b"MODL":
            raise BadMagicError(f"expected MODL, got {data[:4]!r}", fmt=FMT, offset=0)
        try:
            roots = parse_4cc(data, CONTAINERS_4CC)
        except ChunkError as e:
            raise ZmdError(f"chunk tree: {e.msg}", offset=e.offset) from e
        if len(roots) != 1:
            raise ZmdError(f"expected a single MODL chunk, found {len(roots)}", offset=0)
        modl = roots[0]
        items = []
        try:
            for ch in _kids(modl, "MODL"):
                if ch.tag == b"MLOD":
                    items.append(Lod.from_chunk(ch, strict))
                elif ch.tag == b"MDEF":
                    items.append(Mdef.decode(ch.data, ch.offset + 8))
                elif ch.tag == b"MATI":
                    items.append(Mati.from_chunk(ch, FMT))
                else:
                    items.append(ch)
        except (struct.error, IndexError, UnicodeDecodeError, OverflowError) as e:  # defensive
            raise ZmdError(f"decode failed: {e!r}") from e
        z = cls(items)
        if modl.trailing:
            raise ZmdError(f"{len(modl.trailing)} stray bytes in MODL", offset=8)
        return z

    def write(self) -> bytes:
        if self.empty and not self.items:
            return b""
        kids = [it if isinstance(it, Chunk) else it.to_chunk() for it in self.items]
        return write_4cc([Chunk(b"MODL", children=kids)])

    # -- views -------------------------------------------------------------
    @property
    def lods(self):
        return [i for i in self.items if isinstance(i, Lod)]

    @property
    def mdef(self):
        return next((i for i in self.items if isinstance(i, Mdef)), None)

    def materials(self):
        """Embedded textures, in file order: list of :class:`Mati`."""
        return [i for i in self.items if isinstance(i, Mati)]

    def texture(self, name):
        """The :class:`Mati` whose name matches an RW texture name (case-insensitive)."""
        key = name.upper()
        return next((m for m in self.materials() if m.name.upper() == key), None)

    def skeleton(self, lod=0, extra_names=()):
        """Frame hierarchy of a LOD as bones: list of dicts with ``index``, ``parent``,
        ``id`` (anim/bone id = zhash of the name), ``name`` / ``name_source`` (see
        :func:`bone_name`: MDEF, else reverse hash lookup, else ``bone_0x........``,
        ``frame_<i>`` for frames without id), ``matrix`` (local),
        ``world`` and ``inverse_bind`` (inverse of ``world``; vertices are stored in
        model space in the bind pose)."""
        cl = self.lods[lod].clump
        names = self.mdef.by_id() if self.mdef else {}
        worlds = cl.world_matrices()
        out = []
        for i, f in enumerate(cl.frames):
            try:
                inv = _mat_inv_affine(worlds[i])
            except ValueError:
                inv = None
            name, src = bone_name(f.anim_id, names, extra_names, i)
            out.append({"index": i, "parent": f.parent, "id": f.anim_id,
                        "name": name, "name_source": src, "matrix": f.matrix(),
                        "world": worlds[i], "inverse_bind": inv})
        return out

    def meshes(self, lod=0, world=False):
        """Yield one dict per atomic of LOD ``lod`` with plain lists:

        ``name``, ``atomic``, ``frame``, ``matrix`` (world matrix of the atomic's frame),
        ``positions``/``normals`` (3-tuples; normals None if absent), ``uvs`` (first UV set
        or None), ``uvs2``, ``colors`` (RGBA bytes or None), ``indices`` (flat CCW triangle
        list), ``material_ids`` (per triangle, index into ``materials``), ``materials``
        (texture name or None per material slot), ``joints``/``weights`` (per vertex
        ``[frame_index]``/``[1.0]`` for skinned meshes, else None).

        ``world=True`` bakes the frame matrix into positions/normals (bike parts are
        stored relative to their frame; skinned riders sit on an identity frame).
        """
        lo = self.lods[lod]
        cl = lo.clump
        if cl is None:
            return
        worlds = cl.world_matrices()
        id2frame = {}
        for i, f in enumerate(cl.frames):
            if f.anim_id is not None:
                id2frame.setdefault(f.anim_id, i)
        for ai, a in enumerate(cl.atomics):
            g = a.geometry
            mt = g.morph_targets[0] if g.morph_targets else MorphTarget()
            pos = list(mt.vertices or [])
            nrm = list(mt.normals) if mt.normals is not None else None
            mat = worlds[a.frame]
            if world:
                pos = [_xform_point(mat, p) for p in pos]
                if nrm is not None:
                    nrm = [_xform_dir(mat, n) for n in nrm]
            idx, mids = [], []
            for b, a0, m, c in g.triangles:
                idx += (a0, b, c)
                mids.append(m)
            joints = weights = None
            if g.bones:
                joints = [[a.frame] for _ in pos]
                weights = [[1.0] for _ in pos]
                for first, count, bid in g.bones:
                    fi = id2frame.get(bid)
                    if count and fi is not None:
                        for v in range(first, min(first + count, len(pos))):
                            joints[v][0] = fi
            names = []
            for m in g.material_list():
                names.append(m.texture.name if m is not None and m.texture is not None else None)
            yield {"name": f"atomic{ai}", "atomic": ai, "frame": a.frame, "matrix": mat,
                   "positions": pos, "normals": nrm,
                   "uvs": list(g.uv_sets[0]) if g.uv_sets else None,
                   "uvs2": list(g.uv_sets[1]) if len(g.uv_sets) > 1 else None,
                   "colors": list(g.prelit) if g.prelit is not None else None,
                   "indices": idx, "material_ids": mids, "materials": names,
                   "joints": joints, "weights": weights}


def read(data, strict=True) -> Zmd:
    """Parse ZMD bytes. Raises :class:`~dmbmx.core.errors.FormatError` on malformed input."""
    return Zmd.read(data, strict)


def write(model: Zmd) -> bytes:
    """Serialise a :class:`Zmd` (byte-identical to the input for unmodified models)."""
    return model.write()

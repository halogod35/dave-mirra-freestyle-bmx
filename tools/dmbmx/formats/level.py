"""Level folders (``assets/<Park><Dirt|Street|Vert>/``): ZSD index + every ``_sd`` component.

Spec: ``docs/formats/LEVEL.md``. Pure stdlib (vendored by the Blender add-on).

One level folder holds::

    <name>.ZSD     SCEN index: which component files to load, with their sizes      (Zsd)
    Mat._sd        type 1: textures, flat MATI list                    (dmbmx.formats.mati.MatSd)
    PropDef._sd    type 2: prop definitions, PDBL{PDIN, OBJI?, MODL}                (PropDefSd)
    Tag._sd        type 3: tagged collision faces + kd-tree, TAGB{TAGD,TAGN*,TAGF*,TAGI} (TagSd)
    BSP._sd        type 4: RenderWare World (RW 0x0B)                (dmbmx.formats.rwworld.World)
    Prop._sd       type 5: prop instances, PRBL{PRBL, OBJI?}                         (PropSd)
    Scene._sd      type 6: scene points (SCPT{SDPT, OBJI?}) + panorama (SDPA{MODL})   (SceneSd)
    temp.dff       not referenced by the ZSD or the exe: a bare RW clump (5 levels only)

Every ``_sd`` (except BSP) is a tree of Z-Axis 4CC chunks (``char tag[4]; u32 size``; size
excludes the header and must be a multiple of 4: the engine's ziff reader asserts "Data
length not word size"). Fixed-size records start with a u32 that repeats their own size; the
loaders assert on it ("Prop header size invalid" etc.).

Coordinates: the RW world (``BSP._sd``) is the reference. The engine ("ZGE") works in
``E = 100 * (x - ox, -(y - oy), -(z - oz))`` with ``o = World.origin``. Tag faces and the tag
tree are stored in ZGE units; prop and scene-point positions are stored in ZGE/100 units
(the loaders multiply them by the world scale 100). :func:`zge_to_rw` / :func:`pos_to_rw`
convert.

Quick use::

    from dmbmx.formats import level
    lv = level.Level.read_dir("assets/AcclaimDirt")
    lv.props.props[0].pos = (1.0, 2.0, 3.0)
    lv.write_dir("out/AcclaimDirt")           # ZSD SCFI sizes are re-signed automatically
"""
from __future__ import annotations

import os
import struct

from dmbmx.core.chunks import Chunk, ChunkError, Reader, Writer, parse_4cc, parse_rw
from dmbmx.core.errors import BadMagicError, FormatError, LimitError
from dmbmx.core.hash import zhash
from dmbmx.formats import rwworld, zmd
from dmbmx.formats.mati import MatSd

__all__ = [
    "LevelError", "Zsd", "ScfiEntry", "Obji", "PropDef", "PropDefSd", "Prop", "PropSd", "ScenePoint",
    "Panorama", "SceneSd", "TagFace", "FaceTag", "EdgeTag", "TagNode", "TagSd", "TempDff", "Level",
    "COMPONENT_TYPES", "zge_to_rw", "pos_to_rw", "matrix_to_rw", "prop_matrix_to_rw", "ZGE_SCALE",
]

#: SCFI component type -> (default file name, reader class name). [V] 0x00491250..0x004913A0
#: (zsceneOpen) and 0x00496FF0 (zrwWorldHookRpMaterials) pick components by this type byte.
COMPONENT_TYPES = {1: "Mat._sd", 2: "PropDef._sd", 3: "Tag._sd", 4: "BSP._sd", 5: "Prop._sd", 6: "Scene._sd"}

#: World scale (ZGE units per RW unit). [V] NgApp sets it to 100.0 (0x42C80000) via zrwZgl case 1.
ZGE_SCALE = 100.0

SCFI_SIZE = 0x28        # [V] zrwWorldFindScfi 0x004962E0 asserts "Chunk out of sync" otherwise
SCFI_NAME = 32
PDIN_SIZE = 0x38        # [V] 0x00486A9B "Prop Def header size invalid"
PRBL_SIZE = 0x48        # [V] 0x00486C4B "Prop header size invalid"
SDPT_SIZE = 0x4C        # [V] 0x00491FAE "Point data size mismatch"
TAGF_SIZE = 0x50        # [V] 0x0048DCA5 "face structure mismatch"
TAGN_SIZE = 0x28        # [V] 0x0048D9A0 reads 0x28 bytes per node
MAX_TAG_DEPTH = 64      # the corpus needs < 20
MAX_TAG_NODES = 1 << 16


class LevelError(FormatError):
    def __init__(self, msg, *, fmt="LEVEL", offset=None):
        super().__init__(msg, fmt=fmt, offset=offset)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1")


def _name_bytes(name, raw, size):
    """Fixed-size NUL-terminated name; keeps ``raw`` (with its junk) when the name is unchanged."""
    return zmd._pad_name(name, raw, size)


def _check4(n, what):
    if n % 4:
        raise ValueError(f"{what}: payload of {n} bytes is not a multiple of 4 (the engine asserts)")


def _parse_seq(data, fmt, base=0):
    try:
        return parse_4cc(data, (), strict=True, base=base)
    except ChunkError as e:
        raise type(e)(f"chunk tree: {e.msg}", fmt=fmt, offset=e.offset) from e


def _sub(ch, fmt):
    """Parse the payload of a leaf chunk as a 4CC sequence."""
    return _parse_seq(ch.data, fmt, ch.offset + 8)


def _guard(fn, data, fmt):
    try:
        return fn(bytes(data))
    except FormatError:
        raise
    except (struct.error, IndexError, ValueError, UnicodeDecodeError, OverflowError, KeyError) as e:
        raise LevelError(f"decode failed: {e!r}", fmt=fmt) from e


# ------------------------------------------------------------------------------- coordinates

def zge_to_rw(p, origin):
    """ZGE (engine) units -> RW world space: ``o + (x, -y, -z) / 100``."""
    return (origin[0] + p[0] / ZGE_SCALE, origin[1] - p[1] / ZGE_SCALE, origin[2] - p[2] / ZGE_SCALE)


def pos_to_rw(p, origin):
    """Prop / scene-point position (stored in ZGE/100 units) -> RW world space."""
    return (origin[0] + p[0], origin[1] - p[1], origin[2] - p[2])


def matrix_to_rw(m9):
    """Scene-point matrix (rows right, up, at; ZGE axes) -> RW axes: ``F M F`` with F = diag(1,-1,-1),
    i.e. its axes taken as ZGE *vectors* (the game uses a scene point's rows only as directions).
    Returned as 3 rows. Props do NOT use this: see :func:`prop_matrix_to_rw`."""
    f = (1.0, -1.0, -1.0)
    return [tuple(m9[3 * r + c] * f[r] * f[c] for c in range(3)) for r in range(3)]


def prop_matrix_to_rw(m9):
    """Prop rotation (PRBL matrix, 9 f32 rows) -> the RW rotation rows the game renders it with: the
    stored rows **unchanged** (LEVEL.md section 2).

    [V] zpropLoad turns the rows into a quaternion q (matrix -> euler -> quaternion, the identity chain),
    and zrwModelUpdate 0x00488650 builds the RW frame with zrwModelQuatToRwMatrixMirror 0x00487FC0
    because ngappInit 0x0045B270 sets ZGL value 0x10 = 1 (g_zrwZglFlags bit 1). The mirror converter is
    ``D (F R(q) F) D`` with D = diag(-1, 1, 1) and F = diag(1, -1, -1); D F = -I, so the RW matrix is the
    stored one. For a pure yaw this is the opposite sign of ``F M F`` (the earlier convention).
    [V] Data: the game's own prop collision boxes (zcolBuildPropRails, built in ZGE from the def extents
    and the prop's euler) line up with the placed models to ~1 cm only with this convention."""
    return [tuple(float(m9[3 * r + c]) for c in range(3)) for r in range(3)]


# ------------------------------------------------------------------------------- ZSD

class ScfiEntry:
    """One SCFI record (0x28 bytes): ``type`` (component type, :data:`COMPONENT_TYPES`),
    ``junk`` (bytes 1-3, uninitialised memory, kept verbatim), ``size`` (component file size),
    ``name`` (file name, 32-byte field, NUL-terminated; ``name_raw`` keeps the junk after it)."""
    __slots__ = ("type", "junk", "size", "name", "name_raw")

    def __init__(self, type=0, name="", size=0, junk=b"\xcc\xcc\xcc", name_raw=None):
        self.type, self.name, self.size, self.junk, self.name_raw = type, name, size, junk, name_raw

    @classmethod
    def decode(cls, data, base=0):
        if len(data) != SCFI_SIZE:
            raise LevelError(f"SCFI payload is {len(data)} bytes, the engine requires 0x28", fmt="ZSD", offset=base)
        r = Reader(data, base=base, fmt="ZSD", what="SCFI")
        t = r.u8()
        junk = r.bytes(3)
        size = r.u32()
        raw = r.bytes(SCFI_NAME)
        return cls(t, _cstr(raw), size, junk, raw)

    def encode(self):
        if len(self.junk) != 3:
            raise ValueError("SCFI junk must be 3 bytes")
        return struct.pack("<B3sI", self.type, self.junk, self.size) + _name_bytes(self.name, self.name_raw, SCFI_NAME)

    def __repr__(self):
        return f"<ScfiEntry {self.type} {self.name!r} {self.size}>"


class Zsd:
    """``<name>.ZSD``: ``SCEN{ SCDF, SCFI x N }``.

    Logically the SCFI records belong to SCDF, but on disk SCDF's size covers only its own
    16 bytes and the SCFI chunks are its siblings inside SCEN (only SCEN's size covers them).
    ``scdf`` = SCDF payload (16 bytes: ``00`` + 15 bytes of 0xCC fill; [V] never read by the
    exe), ``entries`` = :class:`ScfiEntry` list, ``extra`` = any other SCEN children (none in
    the corpus), ``trailing`` = bytes after SCEN (none).
    """
    FMT = "ZSD"

    def __init__(self, scdf=b"\x00" + b"\xcc" * 15, entries=None, extra=None, trailing=b""):
        self.scdf, self.entries = scdf, entries if entries is not None else []
        self.extra, self.trailing = extra if extra is not None else [], trailing

    @classmethod
    def read(cls, data):
        return _guard(cls._read, data, cls.FMT)

    @classmethod
    def _read(cls, data):
        if data[:4] != b"SCEN":
            raise BadMagicError(f"expected SCEN, got {data[:4]!r}", fmt=cls.FMT, offset=0)
        try:
            roots = parse_4cc(data[:8 + struct.unpack_from("<I", data, 4)[0]] if len(data) >= 8 else data,
                              (b"SCEN",), strict=True)
        except ChunkError as e:
            raise type(e)(f"chunk tree: {e.msg}", fmt=cls.FMT, offset=e.offset) from e
        scen = roots[0]
        z = cls(trailing=data[8 + len(scen.payload()):])
        kids = scen.children
        if not kids or kids[0].tag != b"SCDF":
            raise LevelError("SCEN must start with SCDF", fmt=cls.FMT, offset=8)
        z.scdf = kids[0].data
        for c in kids[1:]:
            if c.tag == b"SCFI":
                z.entries.append(ScfiEntry.decode(c.data, c.offset + 8))
            else:
                z.extra.append(c)
        return z

    def write(self):
        kids = [Chunk(b"SCDF", data=self.scdf)] + [Chunk(b"SCFI", data=e.encode()) for e in self.entries] + self.extra
        for c in kids:
            _check4(len(c.payload()), c.name)
        return Chunk(b"SCEN", children=kids).to_bytes() + self.trailing

    def entry(self, type_or_name):
        for e in self.entries:
            if e.type == type_or_name or (isinstance(type_or_name, str) and e.name.lower() == type_or_name.lower()):
                return e
        return None


# ------------------------------------------------------------------------------- OBJI

class Obji:
    """Per-object attribute block ``OBJI``: ``u32 8 (header size); u32 n; u8 data[n]; pad to 4``.
    [V] Read by the PDBL/PRBL/SCPT handlers into a heap block (prop +0x24, prop def +0x28,
    scene point +0x48); its layout is per object class (LEVEL.md 5.4). ``pad`` keeps the
    alignment bytes (0 in the corpus)."""
    __slots__ = ("hdr", "data", "pad")

    def __init__(self, data=b"", pad=None, hdr=8):
        self.hdr, self.data, self.pad = hdr, bytes(data), pad

    @classmethod
    def decode(cls, data, base=0, fmt="LEVEL"):
        r = Reader(data, base=base, fmt=fmt, what="OBJI")
        hdr, n = r.unpack("II")
        if n > r.remaining():
            raise LevelError(f"OBJI data size {n} runs past the chunk", fmt=fmt, offset=base + 4)
        return cls(r.bytes(n), r.rest(), hdr)

    def encode(self):
        pad = self.pad if self.pad is not None and (len(self.data) + len(self.pad)) % 4 == 0 \
            else b"\0" * (-len(self.data) % 4)
        return struct.pack("<II", self.hdr, len(self.data)) + self.data + pad

    def to_chunk(self):
        return Chunk(b"OBJI", data=self.encode())


def _model_from_chunk(ch, fmt):
    try:
        return zmd.Zmd.read(ch.to_bytes())
    except FormatError as e:
        raise LevelError(f"embedded MODL: {e.msg}", fmt=fmt, offset=(ch.offset or 0) + (e.offset or 0)) from e


def _model_chunk(model):
    return parse_4cc(model.write(), (), strict=True)[0]


# ------------------------------------------------------------------------------- PropDef._sd

class PropDef:
    """One ``PDBL`` form: a prop definition (the model every :class:`Prop` with this ``id`` uses).

    PDIN (0x38 bytes): ``hdr`` (0x38), ``id`` (= zhash(name) [V 508/508]; props reference it),
    ``name`` (20-byte field), ``extents`` (3 f32; [I] size/collision box, non-zero exactly when
    an OBJI is present), ``junk`` (16 bytes: stale pointers, kept). ``obji`` (:class:`Obji`
    or None), ``model`` (:class:`dmbmx.formats.zmd.Zmd`; MODL without MATI: textures come from
    Mat._sd), ``order`` (child tags in file order)."""
    __slots__ = ("hdr", "id", "name", "name_raw", "extents", "junk", "obji", "model", "order", "extra")

    def __init__(self, name="", model=None, id=None):
        self.hdr, self.name, self.name_raw = PDIN_SIZE, name, None
        self.id = zhash(name) if id is None else id
        self.extents, self.junk = (0.0, 0.0, 0.0), b"\0" * 16
        self.obji, self.model = None, model
        self.order, self.extra = [b"PDIN", b"MODL"], []

    @classmethod
    def from_chunk(cls, ch, fmt):
        p = cls()
        p.order = []
        for c in _sub(ch, fmt):
            p.order.append(c.tag)
            if c.tag == b"PDIN":
                if len(c.data) != PDIN_SIZE:
                    raise LevelError(f"PDIN is {len(c.data)} bytes, need 0x38", fmt=fmt, offset=c.offset)
                r = Reader(c.data, base=c.offset + 8, fmt=fmt, what="PDIN")
                p.hdr, p.id = r.unpack("II")
                p.name_raw = r.bytes(20)
                p.name = _cstr(p.name_raw)
                p.extents = r.unpack("3f")
                p.junk = r.bytes(16)
            elif c.tag == b"OBJI":
                p.obji = Obji.decode(c.data, c.offset + 8, fmt)
            elif c.tag == b"MODL":
                p.model = _model_from_chunk(c, fmt)
            else:
                p.extra.append(c)
        if p.order.count(b"PDIN") != 1 or p.order.count(b"MODL") != 1 or p.order.count(b"OBJI") > 1:
            raise LevelError("PDBL needs exactly one PDIN and one MODL (and at most one OBJI)", fmt=fmt,
                             offset=ch.offset)
        return p

    def to_chunk(self):
        pdin = struct.pack("<II", self.hdr, self.id) + _name_bytes(self.name, self.name_raw, 20) \
            + struct.pack("<3f", *self.extents) + self.junk
        if len(pdin) != PDIN_SIZE:
            raise ValueError("PDIN junk must be 16 bytes")
        parts = {b"PDIN": Chunk(b"PDIN", data=pdin), b"MODL": _model_chunk(self.model),
                 b"OBJI": self.obji.to_chunk() if self.obji is not None else None}
        order = [t for t in self.order if t in parts]
        if self.obji is not None and b"OBJI" not in order:
            order.insert(1, b"OBJI")
        kids = [parts[t] for t in order if parts[t] is not None] + list(self.extra)
        return Chunk(b"PDBL", data=b"".join(k.to_bytes() for k in kids))


class _FormFile:
    """Shared reader/writer for the ``_sd`` files that are a flat list of 4CC forms."""
    FMT = "SD"
    MAGIC = ()

    def __init__(self, items=None):
        self.items = items if items is not None else []

    @classmethod
    def read(cls, data):
        return _guard(cls._read, data, cls.FMT)

    @classmethod
    def _read(cls, data):
        if data[:4] not in cls.MAGIC:
            raise BadMagicError(f"expected {b'/'.join(cls.MAGIC)!r}, got {data[:4]!r}", fmt=cls.FMT, offset=0)
        return cls([cls._item(c) for c in _parse_seq(data, cls.FMT)])

    @classmethod
    def _item(cls, ch):
        return ch

    def write(self):
        out = bytearray()
        for it in self.items:
            ch = it if isinstance(it, Chunk) else it.to_chunk()
            _check4(len(ch.payload()), ch.name)
            out += ch.to_bytes()
        return bytes(out)


class PropDefSd(_FormFile):
    """``PropDef._sd``: a list of ``PDBL`` forms (:class:`PropDef`). [V] Loaded by the PDBL
    handler 0x00486A20 (zpropLoadDef): PDIN, MODL (asserts if missing), optional OBJI."""
    FMT = "PROPDEF"
    MAGIC = (b"PDBL",)

    @classmethod
    def _item(cls, ch):
        return PropDef.from_chunk(ch, cls.FMT) if ch.tag == b"PDBL" else ch

    @property
    def defs(self):
        return [i for i in self.items if isinstance(i, PropDef)]

    def by_id(self):
        return {d.id: d for d in self.defs}


# ------------------------------------------------------------------------------- Prop._sd

class Prop:
    """One ``PRBL`` form: a placed prop. Inner ``PRBL`` record (0x48 bytes): ``hdr`` (0x48),
    ``def_id`` (:attr:`PropDef.id`; [V] the loader asserts "No prop def corresponds" otherwise),
    ``name`` (16-byte field; [V] always the def's name), ``matrix`` (9 f32: rows right, up, at;
    ZGE axes; orthonormal, det +1 in all 927), ``pos`` (3 f32, ZGE/100 units). ``obji``: optional
    :class:`Obji` (10 bytes in the corpus)."""
    __slots__ = ("hdr", "def_id", "name", "name_raw", "matrix", "pos", "obji", "order", "extra")

    def __init__(self, def_id=0, name="", matrix=(1, 0, 0, 0, 1, 0, 0, 0, 1), pos=(0.0, 0.0, 0.0)):
        self.hdr, self.def_id, self.name, self.name_raw = PRBL_SIZE, def_id, name, None
        self.matrix, self.pos = tuple(float(x) for x in matrix), tuple(pos)
        self.obji, self.order, self.extra = None, [b"PRBL"], []

    @classmethod
    def from_chunk(cls, ch, fmt):
        p = cls()
        p.order = []
        for c in _sub(ch, fmt):
            p.order.append(c.tag)
            if c.tag == b"PRBL":
                if len(c.data) != PRBL_SIZE:
                    raise LevelError(f"inner PRBL is {len(c.data)} bytes, need 0x48", fmt=fmt, offset=c.offset)
                r = Reader(c.data, base=c.offset + 8, fmt=fmt, what="PRBL")
                p.hdr, p.def_id = r.unpack("II")
                p.name_raw = r.bytes(16)
                p.name = _cstr(p.name_raw)
                p.matrix = r.unpack("9f")
                p.pos = r.unpack("3f")
            elif c.tag == b"OBJI":
                p.obji = Obji.decode(c.data, c.offset + 8, fmt)
            else:
                p.extra.append(c)
        if p.order.count(b"PRBL") != 1 or p.order.count(b"OBJI") > 1:
            raise LevelError("PRBL form needs exactly one PRBL record (and at most one OBJI)", fmt=fmt,
                             offset=ch.offset)
        return p

    def to_chunk(self):
        rec = struct.pack("<II", self.hdr, self.def_id) + _name_bytes(self.name, self.name_raw, 16) \
            + struct.pack("<9f3f", *self.matrix, *self.pos)
        kids = [Chunk(b"PRBL", data=rec)]
        if self.obji is not None:
            kids.append(self.obji.to_chunk())
        if self.order and self.order[0] != b"PRBL" and len(kids) == 2:
            kids.reverse()
        kids += list(self.extra)
        return Chunk(b"PRBL", data=b"".join(k.to_bytes() for k in kids))


class PropSd(_FormFile):
    """``Prop._sd``: a list of outer ``PRBL`` forms (:class:`Prop`). [V] handler 0x00486BE0."""
    FMT = "PROP"
    MAGIC = (b"PRBL",)

    @classmethod
    def _item(cls, ch):
        return Prop.from_chunk(ch, cls.FMT) if ch.tag == b"PRBL" else ch

    @property
    def props(self):
        return [i for i in self.items if isinstance(i, Prop)]


# ------------------------------------------------------------------------------- Scene._sd

class ScenePoint:
    """One ``SCPT`` form. SDPT record (0x4C bytes): ``hdr`` (0x4C), ``id`` (= zhash(name) [V]),
    ``type`` (u32: 1 Globals/Player/PlayerT/first SpotLight, 2, 3 = further SpotLights [V]),
    ``name`` (16-byte field), ``matrix`` (9 f32, rows), ``pos`` (3 f32, ZGE/100 units).
    [V] zscnptsLoad 0x00491F50 stores ``-row0, row1, -row2`` and ``pos * 100``. ``obji``: optional."""
    __slots__ = ("hdr", "id", "type", "name", "name_raw", "matrix", "pos", "obji", "order", "extra")

    def __init__(self, name="", type=1, matrix=(1, 0, 0, 0, 1, 0, 0, 0, 1), pos=(0.0, 0.0, 0.0), id=None):
        self.hdr, self.name, self.name_raw, self.type = SDPT_SIZE, name, None, type
        self.id = zhash(name) if id is None else id
        self.matrix, self.pos = tuple(float(x) for x in matrix), tuple(pos)
        self.obji, self.order, self.extra = None, [b"SDPT"], []

    @classmethod
    def from_chunk(cls, ch, fmt):
        p = cls()
        p.order = []
        for c in _sub(ch, fmt):
            p.order.append(c.tag)
            if c.tag == b"SDPT":
                if len(c.data) != SDPT_SIZE:
                    raise LevelError(f"SDPT is {len(c.data)} bytes, need 0x4C", fmt=fmt, offset=c.offset)
                r = Reader(c.data, base=c.offset + 8, fmt=fmt, what="SDPT")
                p.hdr, p.id, p.type = r.unpack("III")
                p.name_raw = r.bytes(16)
                p.name = _cstr(p.name_raw)
                p.matrix = r.unpack("9f")
                p.pos = r.unpack("3f")
            elif c.tag == b"OBJI":
                p.obji = Obji.decode(c.data, c.offset + 8, fmt)
            else:
                p.extra.append(c)
        if p.order.count(b"SDPT") != 1 or p.order.count(b"OBJI") > 1:
            raise LevelError("SCPT needs exactly one SDPT (and at most one OBJI)", fmt=fmt, offset=ch.offset)
        return p

    def to_chunk(self):
        rec = struct.pack("<III", self.hdr, self.id, self.type) + _name_bytes(self.name, self.name_raw, 16) \
            + struct.pack("<9f3f", *self.matrix, *self.pos)
        kids = [Chunk(b"SDPT", data=rec)]
        if self.obji is not None:
            kids.append(self.obji.to_chunk())
        kids += list(self.extra)
        return Chunk(b"SCPT", data=b"".join(k.to_bytes() for k in kids))


class Panorama:
    """``SDPA{ MODL }``: the sky panorama model. [V] handler 0x004A10A0 loads the MODL and
    registers it as scene model ``Panorama`` (zhash)."""
    __slots__ = ("model", "extra")

    def __init__(self, model=None):
        self.model, self.extra = model, []

    @classmethod
    def from_chunk(cls, ch, fmt):
        p = cls()
        for c in _sub(ch, fmt):
            if c.tag == b"MODL" and p.model is None:
                p.model = _model_from_chunk(c, fmt)
            else:
                p.extra.append(c)
        if p.model is None:
            raise LevelError("SDPA without MODL", fmt=fmt, offset=ch.offset)
        return p

    def to_chunk(self):
        kids = [_model_chunk(self.model)] + list(self.extra)
        return Chunk(b"SDPA", data=b"".join(k.to_bytes() for k in kids))


class SceneSd(_FormFile):
    """``Scene._sd``: ``SCPT`` forms (:class:`ScenePoint`) and one ``SDPA`` (:class:`Panorama`)
    in file order. [V] zsceneOpen loads it with the handler table 0x00561908 (SCPT, SDPA)."""
    FMT = "SCENE"
    MAGIC = (b"SCPT", b"SDPA")

    @classmethod
    def _item(cls, ch):
        if ch.tag == b"SCPT":
            return ScenePoint.from_chunk(ch, cls.FMT)
        if ch.tag == b"SDPA":
            return Panorama.from_chunk(ch, cls.FMT)
        return ch

    @property
    def points(self):
        return [i for i in self.items if isinstance(i, ScenePoint)]

    @property
    def panorama(self):
        return next((i for i in self.items if isinstance(i, Panorama)), None)


# ------------------------------------------------------------------------------- Tag._sd

class FaceTag:
    """Face tag record (12 bytes in TAGI): ``code`` u16 (bits 12-15 class, 0-11 id; [V] the
    collision code reads it and strips the class when class == 1 and ``extra`` == 0),
    ``kind`` u8 (0 in the corpus), ``flags`` u8 (bits 0-3 remapped into contact flags by
    NgIFzbsp/NgIFzcollide), ``param`` u32 (0-7), ``extra`` u32 (0)."""
    __slots__ = ("code", "kind", "flags", "param", "extra")
    SIZE = 12

    def __init__(self, code=0, kind=0, flags=0, param=0, extra=0):
        self.code, self.kind, self.flags, self.param, self.extra = code, kind, flags, param, extra

    def encode(self):
        return struct.pack("<HBBII", self.code, self.kind, self.flags, self.param, self.extra)

    def __repr__(self):
        return f"<FaceTag 0x{self.code:04x} k{self.kind} f{self.flags} p{self.param}>"


class EdgeTag:
    """Edge / vertex tag record (3 bytes + 1 alignment byte in TAGI): ``code`` u16, ``kind`` u8
    ([V] NgIFzcollide copies byte +2 and the u16), ``pad`` (0xCD in the corpus, MSVC heap fill)."""
    __slots__ = ("code", "kind", "pad")
    SIZE = 4

    def __init__(self, code=0, kind=0, pad=0xCD):
        self.code, self.kind, self.pad = code, kind, pad

    def encode(self):
        return struct.pack("<HBB", self.code, self.kind, self.pad)

    def __repr__(self):
        return f"<EdgeTag 0x{self.code:04x} k{self.kind}>"


class TagFace:
    """One TAGF record (0x50 bytes): ``hdr`` (0x50), ``verts`` (3 x 3 f32, ZGE units),
    ``normal`` (3 f32), then 7 i32 offsets into TAGI (-1 = none) for the face tag, the 3 edge
    tags (edge i = verts[i] -> verts[(i+1) % 3]) and the 3 vertex tags. Decoded here into
    ``tag`` / ``edge_tags`` / ``vertex_tags``; the writer regenerates TAGI and the offsets."""
    __slots__ = ("hdr", "verts", "normal", "tag", "edge_tags", "vertex_tags")

    def __init__(self, verts=((0.0,) * 3,) * 3, normal=(0.0, 1.0, 0.0)):
        self.hdr, self.verts, self.normal = TAGF_SIZE, [tuple(v) for v in verts], tuple(normal)
        self.tag, self.edge_tags, self.vertex_tags = None, [None] * 3, [None] * 3


class TagNode:
    """One TAGN node (0x28 bytes + u32 face indices for leaves): ``kind`` u8 (1 leaf, 2 split on
    x, 4 on y, 8 on z), ``junk`` (3 bytes, kept), ``split`` f32, ``bound_a``/``bound_b`` (3 f32
    each: the node's box corners, ZGE units; [V] the file order is (b, a) relative to the engine
    struct, see LEVEL.md), ``count_hi`` (upper 16 bits of the count word, 0), ``faces`` (face
    indices), ``low``/``high`` children (x < split / y > split / z < split go ``low``)."""
    __slots__ = ("hdr", "kind", "junk", "split", "bound_a", "bound_b", "count_hi", "faces", "low", "high")

    def __init__(self, kind=1):
        self.hdr, self.kind, self.junk, self.split = TAGN_SIZE, kind, b"\0\0\0", 0.0
        self.bound_a = self.bound_b = (0.0, 0.0, 0.0)
        self.count_hi, self.faces, self.low, self.high = 0, [], None, None

    def encode(self):
        if len(self.faces) > 0xFFFF:
            raise ValueError("a tag leaf holds at most 65535 faces (u16 count)")
        return struct.pack("<IB3sf3f3fI", self.hdr, self.kind, self.junk, self.split, *self.bound_a, *self.bound_b,
                           (self.count_hi << 16) | len(self.faces)) + struct.pack(f"<{len(self.faces)}I", *self.faces)

    def walk(self):
        """Pre-order iterator (file order)."""
        stack = [self]
        while stack:
            n = stack.pop()
            yield n
            if n.kind != 1:
                stack += [n.high, n.low]


class TagSd:
    """``Tag._sd``: ``TAGB{ TAGD, TAGN* (pre-order kd-tree), TAGF* (faces), TAGI (tag records) }``.

    [V] Loaded by the TAGB handler 0x0048DAE0 (ztaginfLoad): TAGD = ``u32 8; u32 faceCount``
    (asserts "Chunk sync error" unless the first word is 8); TAGI = ``u32 8; u32 dataSize;``
    records; TAGF x faceCount; then the TAGN tree (0x0048D9A0, recursive, low child first).
    ``faces`` (:class:`TagFace`), ``root`` (:class:`TagNode`), ``tagd_hdr``, ``tagi_hdr``,
    ``tagi_size`` (None = computed; set only when the file's value differs from the rule).
    """
    FMT = "TAG"

    def __init__(self):
        self.faces, self.root = [], TagNode()
        self.tagd_hdr, self.tagi_hdr, self.tagi_size = 8, 8, None
        self.extra = []

    @classmethod
    def read(cls, data):
        return _guard(cls._read, data, cls.FMT)

    @classmethod
    def _read(cls, data):
        fmt = cls.FMT
        if data[:4] != b"TAGB":
            raise BadMagicError(f"expected TAGB, got {data[:4]!r}", fmt=fmt, offset=0)
        top = _parse_seq(data, fmt)
        if len(top) != 1:
            raise LevelError(f"expected one TAGB form, found {len(top)}", fmt=fmt, offset=0)
        kids = _sub(top[0], fmt)
        t = cls()
        tags = [k.tag for k in kids]
        want = [b"TAGD"] + [b"TAGN"] * tags.count(b"TAGN") + [b"TAGF"] * tags.count(b"TAGF") + [b"TAGI"]
        if tags != want:
            raise LevelError("TAGB children must be TAGD, TAGN*, TAGF*, TAGI", fmt=fmt, offset=8)
        d = Reader(kids[0].data, base=kids[0].offset + 8, fmt=fmt, what="TAGD")
        t.tagd_hdr, count = d.unpack("II")
        if d.remaining():
            raise LevelError("TAGD: stray bytes", fmt=fmt, offset=kids[0].offset)
        fchunks = [k for k in kids if k.tag == b"TAGF"]
        if count != len(fchunks):
            raise LevelError(f"TAGD face count {count} != {len(fchunks)} TAGF chunks", fmt=fmt, offset=kids[0].offset)
        ti = kids[-1]
        r = Reader(ti.data, base=ti.offset + 8, fmt=fmt, what="TAGI")
        t.tagi_hdr, size = r.unpack("II")
        blob = r.rest()
        bbase = ti.offset + 16
        refs = []
        for fc in fchunks:
            if len(fc.data) != TAGF_SIZE:
                raise LevelError(f"TAGF is {len(fc.data)} bytes, need 0x50", fmt=fmt, offset=fc.offset)
            v = struct.unpack("<I12f7i", fc.data)
            f = TagFace([v[1:4], v[4:7], v[7:10]], v[10:13])
            f.hdr = v[0]
            t.faces.append(f)
            refs.append(v[13:20])
        # tag records: the blob is the face/edge/vertex records in face order [V 12/12]
        pos = 0
        for f, rr in zip(t.faces, refs):
            for k, off in enumerate(rr):
                if off == -1:
                    continue
                if off != pos:
                    raise LevelError(f"TAGI record offset {off} out of order (expected {pos})", fmt=fmt, offset=bbase)
                if k == 0:
                    if pos + 12 > len(blob):
                        raise LevelError("TAGI face record runs past the blob", fmt=fmt, offset=bbase + pos)
                    f.tag = FaceTag(*struct.unpack_from("<HBBII", blob, pos))
                    pos += 12
                else:
                    if pos + 3 > len(blob):
                        raise LevelError("TAGI edge record runs past the blob", fmt=fmt, offset=bbase + pos)
                    code, kind = struct.unpack_from("<HB", blob, pos)
                    padb = blob[pos + 3] if pos + 3 < len(blob) else None
                    tg = EdgeTag(code, kind, padb if padb is not None else 0)
                    (f.edge_tags if k <= 3 else f.vertex_tags)[(k - 1) % 3] = tg
                    pos += 4
        if pos != len(blob) and not (pos == len(blob) + 1 and blob):
            raise LevelError(f"TAGI: {len(blob) - pos} unreferenced bytes", fmt=fmt, offset=bbase + pos)
        t._blob_len = len(blob)
        if size != t._tagi_data_size():
            t.tagi_size = size
        # tree
        nodes = [k for k in kids if k.tag == b"TAGN"]
        if len(nodes) > MAX_TAG_NODES:
            raise LimitError(f"{len(nodes)} tag nodes", fmt=fmt, offset=8)
        it = iter(nodes)
        nfaces = len(t.faces)

        def node(depth):
            if depth > MAX_TAG_DEPTH:
                raise LimitError(f"tag tree deeper than {MAX_TAG_DEPTH}", fmt=fmt)
            c = next(it, None)
            if c is None:
                raise LevelError("tag tree: missing TAGN node", fmt=fmt)
            rr = Reader(c.data, base=c.offset + 8, fmt=fmt, what="TAGN")
            n = TagNode()
            n.hdr, n.kind, n.junk, n.split = rr.unpack("IB3sf")
            n.bound_a, n.bound_b = rr.unpack("3f"), rr.unpack("3f")
            cnt = rr.u32()
            n.count_hi = cnt >> 16
            n.faces = rr.array("I", cnt & 0xFFFF)
            if rr.remaining():
                raise LevelError("TAGN: stray bytes", fmt=fmt, offset=c.offset)
            if n.kind not in (1, 2, 4, 8):
                raise LevelError(f"TAGN: bad node kind {n.kind}", fmt=fmt, offset=c.offset)
            if any(i >= nfaces for i in n.faces):
                raise LevelError("TAGN: face index out of range", fmt=fmt, offset=c.offset)
            if n.kind != 1:
                n.low = node(depth + 1)
                n.high = node(depth + 1)
            return n

        t.root = node(0)
        if next(it, None) is not None:
            raise LevelError("tag tree: extra TAGN nodes", fmt=fmt)   # [V] the engine asserts too
        return t

    def _records(self):
        out = []
        for f in self.faces:
            for k, tg in enumerate([f.tag] + list(f.edge_tags) + list(f.vertex_tags)):
                out.append((k, tg))
        return out

    def _tagi_data_size(self):
        """[V 12/12] dataSize = blob length, minus the alignment byte when the last record is a
        3-byte edge/vertex record whose pad byte was never written (stored 0)."""
        last = None
        n = 0
        for f in self.faces:
            for tg in [f.tag] + list(f.edge_tags) + list(f.vertex_tags):
                if tg is not None:
                    last = tg
                    n += tg.SIZE
        if isinstance(last, EdgeTag) and last.pad == 0:
            return n - 1
        return n

    def write(self):
        blob = bytearray()
        fchunks = []
        for f in self.faces:
            offs = []
            for tg in [f.tag] + list(f.edge_tags) + list(f.vertex_tags):
                if tg is None:
                    offs.append(-1)
                else:
                    offs.append(len(blob))
                    blob += tg.encode()
            if len(f.verts) != 3:
                raise ValueError("a tag face has 3 vertices")
            fchunks.append(Chunk(b"TAGF", data=struct.pack("<I12f7i", f.hdr, *f.verts[0], *f.verts[1], *f.verts[2],
                                                           *f.normal, *offs)))
        size = self._tagi_data_size() if self.tagi_size is None else self.tagi_size
        kids = [Chunk(b"TAGD", data=struct.pack("<II", self.tagd_hdr, len(self.faces)))]
        for n in self.root.walk():
            if n.kind != 1 and (n.low is None or n.high is None):
                raise ValueError("tag split node without two children")
            if any(not 0 <= i < len(self.faces) for i in n.faces):
                raise ValueError("tag leaf references a missing face")
            kids.append(Chunk(b"TAGN", data=n.encode()))
        kids += fchunks
        kids.append(Chunk(b"TAGI", data=struct.pack("<II", self.tagi_hdr, size) + bytes(blob)))
        kids += self.extra
        return Chunk(b"TAGB", data=b"".join(k.to_bytes() for k in kids)).to_bytes()

    def leaves(self):
        return [n for n in self.root.walk() if n.kind == 1]


# ------------------------------------------------------------------------------- temp.dff

class TempDff:
    """``temp.dff``: a bare RW Clump (exporter leftover, never loaded; [V] the string does not
    occur in the exe). Decoded with :class:`dmbmx.formats.zmd.Clump`."""
    FMT = "DFF"

    def __init__(self, clump=None, version=0x301):
        self.clump, self.version = clump, version

    @classmethod
    def read(cls, data):
        def _r(data):
            try:
                roots = parse_rw(data)
            except ChunkError as e:
                raise type(e)(f"chunk tree: {e.msg}", fmt=cls.FMT, offset=e.offset) from e
            if len(roots) != 1 or roots[0].tag != 0x10:
                raise LevelError("expected a single RW Clump (0x10)", fmt=cls.FMT, offset=0)
            return cls(zmd.Clump.from_chunk(roots[0]), roots[0].version)
        return _guard(_r, data, cls.FMT)

    def write(self):
        return self.clump.to_chunk(self.version).to_bytes()


# ------------------------------------------------------------------------------- Level

READERS = {1: MatSd, 2: PropDefSd, 3: TagSd, 4: rwworld, 5: PropSd, 6: SceneSd}


class Level:
    """A whole level folder. ``zsd_name``, ``zsd`` (:class:`Zsd`), ``components``
    ({file name: decoded object}, keyed as named in the ZSD), ``temp_dff`` (:class:`TempDff` or
    None), ``extra_files`` ({name: bytes} for anything else in the folder).

    :meth:`files` serialises everything and **re-signs** the ZSD: every SCFI size is set to the
    length of the component it names, so edited ``_sd`` files load (docs/re/integrity-checks.md #9).
    """

    def __init__(self):
        self.name = ""
        self.zsd_name, self.zsd = None, Zsd()
        self.components = {}
        self.temp_dff = None
        self.extra_files = {}

    @classmethod
    def read_dir(cls, path, *, components=None):
        """Read a level folder. ``components``: optional iterable of SCFI types to decode
        (others are kept as raw bytes)."""
        lv = cls()
        lv.name = os.path.basename(os.path.normpath(str(path)))
        names = sorted(os.listdir(path))
        zsds = [n for n in names if n.lower().endswith(".zsd")]
        if len(zsds) != 1:
            raise LevelError(f"{path}: expected one .ZSD file, found {len(zsds)}")
        lv.zsd_name = zsds[0]
        lv.zsd = Zsd.read(_readf(path, zsds[0]))
        used = {zsds[0].lower()}
        for e in lv.zsd.entries:
            real = next((n for n in names if n.lower() == e.name.lower()), None)
            if real is None:
                raise LevelError(f"{path}: component {e.name} (type {e.type}) missing")
            used.add(real.lower())
            data = _readf(path, real)
            rd = READERS.get(e.type)
            if rd is None or (components is not None and e.type not in components):
                lv.components[e.name] = data
            else:
                lv.components[e.name] = rd.read(data)
        for n in names:
            if n.lower() in used or not os.path.isfile(os.path.join(path, n)):
                continue
            data = _readf(path, n)
            if n.lower() == "temp.dff":
                lv.temp_dff = TempDff.read(data)
            else:
                lv.extra_files[n] = data
        return lv

    def component(self, type_):
        e = self.zsd.entry(type_)
        return None if e is None else self.components.get(e.name)

    world = property(lambda self: self.component(4))
    materials = property(lambda self: self.component(1))
    prop_defs = property(lambda self: self.component(2))
    tags = property(lambda self: self.component(3))
    props = property(lambda self: self.component(5))
    scene = property(lambda self: self.component(6))

    def files(self):
        """{file name: bytes} for the whole folder, with the ZSD re-signed."""
        out = {}
        for name, obj in self.components.items():
            out[name] = obj if isinstance(obj, (bytes, bytearray)) else obj.write()
        for e in self.zsd.entries:
            if e.name in out:
                e.size = len(out[e.name])
        out[self.zsd_name] = self.zsd.write()
        if self.temp_dff is not None:
            out["temp.dff"] = self.temp_dff.write()
        out.update(self.extra_files)
        return out

    def write_dir(self, path):
        os.makedirs(path, exist_ok=True)
        files = self.files()
        for n, b in files.items():
            with open(os.path.join(path, n), "wb") as f:
                f.write(b)
        return files

    # -- helpers ----------------------------------------------------------
    def texture(self, name):
        m = self.materials
        return m.texture(name) if isinstance(m, MatSd) and name else None

    def prop_instances(self):
        """[(Prop, PropDef or None, rw_position, rw_rotation_rows)] with positions in RW world space;
        the rotation is :func:`prop_matrix_to_rw` (the stored rows, as the game renders them)."""
        origin = self.world.origin if isinstance(self.world, rwworld.World) else (0.0, 0.0, 0.0)
        defs = self.prop_defs.by_id() if isinstance(self.prop_defs, PropDefSd) else {}
        out = []
        for p in self.props.props if isinstance(self.props, PropSd) else []:
            out.append((p, defs.get(p.def_id), pos_to_rw(p.pos, origin), prop_matrix_to_rw(p.matrix)))
        return out


def _readf(path, name):
    with open(os.path.join(path, name), "rb") as f:
        return f.read()


def read_component(name, data):
    """Decode one level file by name (``BSP._sd``, ``Tag._sd``, ``foo.ZSD``, ``temp.dff`` ...)."""
    low = name.lower()
    if low.endswith(".zsd"):
        return Zsd.read(data)
    if low == "temp.dff":
        return TempDff.read(data)
    for t, n in COMPONENT_TYPES.items():
        if n.lower() == low:
            return READERS[t].read(data)
    raise LevelError(f"unknown level file {name}")

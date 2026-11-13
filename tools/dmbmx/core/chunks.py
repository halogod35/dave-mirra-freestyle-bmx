"""Generic, lossless chunk trees for the two chunk families used by the game.

Pure stdlib (the Blender add-on vendors ``dmbmx``). Every parser bounds-checks
sizes and raises :class:`ChunkError` on malformed input; nothing ever reads
past the buffer it was given.

Family 1: Z-Axis 4CC chunks
---------------------------
Layout (little endian)::

    char tag[4];  u32 size;  u8 payload[size];   // size excludes the 8-byte header

Used by ZMD (``MODL``/``MLOD``/``MLDF``/``MDFF``), ZSD (``SCEN``...), ``_sd`` files,
and ZAL inner data. Whether a chunk is a *container* (payload = more chunks) or a
*leaf* (payload = raw bytes) is not encoded in the file, so the caller passes the
set of container tags::

    from dmbmx.core.chunks import parse_4cc, write_4cc
    roots = parse_4cc(data, containers={b"MODL", b"MLOD"})
    assert write_4cc(roots) == data

Bytes after the last complete chunk at any level (padding, ``0xCC`` fill, junk)
are kept in the *parent's* ``trailing`` attribute (or returned by
:func:`parse_4cc_ex` for the top level), so serialisation is byte-identical.

Family 2: RenderWare 3.x binary stream
--------------------------------------
Layout (little endian)::

    u32 type;  u32 size;  u32 version;  u8 payload[size];   // 12-byte header

``version`` is kept verbatim (``0x00000301`` in this game = RW 3.0.1-era
encoding, no packed build number). Containers are the well-known RW section ids
(:data:`RW_CONTAINERS`, overridable)::

    from dmbmx.core.chunks import parse_rw, write_rw
    clump, = parse_rw(mdff_payload)
    geom_list = clump.find(RW.GEOMETRYLIST)

A container chunk whose payload does *not* parse cleanly as child chunks is
transparently kept as a leaf (``data`` set, ``children`` None), so unknown
variants never break round-trip.

Both families share :class:`Chunk`:

``Chunk.tag``      bytes (4CC) or int (RW type id)
``Chunk.version``  RW library word (int), ``None`` for 4CC chunks
``Chunk.data``     raw payload bytes for leaves, else ``None``
``Chunk.children`` list of child chunks for containers, else ``None``
``Chunk.trailing`` bytes after the last child inside a container (normally b"")

Sizes are recomputed on write, so editing ``data``/``children`` just works.
"""
from __future__ import annotations

import struct

from dmbmx.core.errors import FormatError, LimitError, TruncatedError

__all__ = [
    "ChunkError", "ChunkTruncatedError", "ChunkDepthError", "MAX_DEPTH", "Chunk", "RW", "RW_CONTAINERS", "RW_NAMES",
    "parse_4cc", "parse_4cc_ex", "write_4cc", "parse_rw", "parse_rw_ex", "write_rw",
    "walk", "Reader", "Writer",
]


class ChunkError(FormatError):
    """Malformed chunk data (bad size, stray bytes, bad tag, ...). A ``FormatError``."""


class ChunkTruncatedError(ChunkError, TruncatedError):
    """A chunk or a struct read runs past the end of its parent."""


class ChunkDepthError(ChunkError, LimitError):
    """Chunk nesting deeper than :data:`MAX_DEPTH`."""


#: Maximum container nesting depth accepted by the parsers (ZMD needs 7).
MAX_DEPTH = 32


# --------------------------------------------------------------------------- Chunk

class Chunk:
    """One chunk of either family. See the module docstring."""

    __slots__ = ("tag", "version", "data", "children", "trailing", "offset")

    def __init__(self, tag, data=None, children=None, version=None, trailing=b"", offset=None):
        if (data is None) == (children is None):
            raise ValueError("Chunk needs exactly one of data / children")
        self.tag = tag
        self.version = version
        self.data = data
        self.children = children
        self.trailing = trailing
        self.offset = offset  # absolute file offset of the header when parsed (informational)

    # -- convenience -------------------------------------------------------
    @property
    def is_rw(self) -> bool:
        return self.version is not None

    @property
    def is_container(self) -> bool:
        return self.children is not None

    @property
    def name(self) -> str:
        """Human-readable tag: ``'MODL'`` or ``'Clump(0x10)'``."""
        if isinstance(self.tag, (bytes, bytearray)):
            return self.tag.decode("latin-1")
        return f"{RW_NAMES.get(self.tag, 'Unknown')}(0x{self.tag:X})"

    def find(self, tag):
        """First direct child with ``tag`` (or None)."""
        for c in self.children or ():
            if c.tag == tag:
                return c
        return None

    def find_all(self, tag):
        """All direct children with ``tag``."""
        return [c for c in (self.children or ()) if c.tag == tag]

    def payload(self) -> bytes:
        """Serialised payload (children + trailing, or data)."""
        if self.children is None:
            return bytes(self.data)
        out = bytearray()
        for c in self.children:
            out += c.to_bytes()
        out += self.trailing
        return bytes(out)

    def to_bytes(self) -> bytes:
        body = self.payload()
        if self.version is None:
            return bytes(self.tag) + struct.pack("<I", len(body)) + body
        return struct.pack("<III", self.tag, len(body), self.version) + body

    def __repr__(self):
        kind = f"{len(self.children)} children" if self.children is not None else f"{len(self.data)} bytes"
        return f"<Chunk {self.name} {kind}>"


def walk(chunks, depth=0):
    """Depth-first iterator yielding ``(depth, chunk)`` over a list of chunks."""
    for c in chunks:
        yield depth, c
        if c.children is not None:
            yield from walk(c.children, depth + 1)


# --------------------------------------------------------------------------- 4CC

def _parse_4cc_seq(buf, start, end, containers, strict, base, depth=0):
    """Parse 4CC chunks in buf[start:end]. Returns (chunks, trailing_bytes)."""
    if depth > MAX_DEPTH:
        raise ChunkDepthError(f"4CC nesting deeper than {MAX_DEPTH}", offset=base + start)
    out = []
    pos = start
    while end - pos >= 8:
        tag = bytes(buf[pos:pos + 4])
        (size,) = struct.unpack_from("<I", buf, pos + 4)
        if pos + 8 + size > end or not _plausible_tag(tag):
            if strict:
                if not _plausible_tag(tag):
                    raise ChunkError(f"bad 4CC tag {tag!r}", offset=base + pos)
                raise ChunkTruncatedError(f"4CC chunk {tag!r} size {size} overruns parent end 0x{base + end:X}",
                                          offset=base + pos)
            break
        p0, p1 = pos + 8, pos + 8 + size
        if tag in containers:
            kids, trail = _parse_4cc_seq(buf, p0, p1, containers, strict, base, depth + 1)
            out.append(Chunk(tag, children=kids, trailing=trail, offset=base + pos))
        else:
            out.append(Chunk(tag, data=bytes(buf[p0:p1]), offset=base + pos))
        pos = p1
    if strict and pos != end:
        raise ChunkTruncatedError(f"{end - pos} stray bytes after last 4CC chunk", offset=base + pos)
    return out, bytes(buf[pos:end])


def _plausible_tag(tag):
    return all(0x20 <= b < 0x7F for b in tag)


def parse_4cc_ex(data, containers=(), strict=False, base=0):
    """Parse a 4CC chunk sequence. Returns ``(chunks, trailing)``.

    ``containers``: iterable of 4-byte tags whose payload is a chunk sequence.
    ``strict``: raise instead of keeping unparsable tails as ``trailing``.
    ``base``: offset added to ``Chunk.offset`` values (for nested use).
    """
    containers = {bytes(t) for t in containers}
    return _parse_4cc_seq(memoryview(data), 0, len(data), containers, strict, base)


def parse_4cc(data, containers=(), strict=True, base=0):
    """Parse a 4CC chunk sequence that must cover ``data`` exactly; returns the chunk list."""
    chunks, _ = parse_4cc_ex(data, containers, strict=strict, base=base)
    return chunks


def write_4cc(chunks, trailing=b"") -> bytes:
    """Serialise a list of 4CC chunks (+ optional top-level trailing bytes)."""
    return b"".join(c.to_bytes() for c in chunks) + bytes(trailing)


# --------------------------------------------------------------------------- RenderWare

class RW:
    """RenderWare 3.x section ids seen in (or relevant to) this game."""
    STRUCT = 0x01
    STRING = 0x02
    EXTENSION = 0x03
    CAMERA = 0x05
    TEXTURE = 0x06
    MATERIAL = 0x07
    MATLIST = 0x08
    ATOMICSECT = 0x09
    PLANESECT = 0x0A
    WORLD = 0x0B
    SPLINE = 0x0C
    MATRIX = 0x0D
    FRAMELIST = 0x0E
    GEOMETRY = 0x0F
    CLUMP = 0x10
    LIGHT = 0x12
    UNICODESTRING = 0x13
    ATOMIC = 0x14
    TEXTURENATIVE = 0x15
    TEXDICTIONARY = 0x16
    ANIMDATABASE = 0x17
    IMAGE = 0x18
    SKINANIMATION = 0x19
    GEOMETRYLIST = 0x1A
    HANIMANIMATION = 0x1B
    TEAM = 0x1C
    CROWD = 0x1D
    RIGHTTORENDER = 0x1F
    MTEFFECTNATIVE = 0x20
    MTEFFECTDICT = 0x21
    TEAMDICTIONARY = 0x22
    PITEXDICTIONARY = 0x23
    TOC = 0x24
    PRTSTDGLOBALDATA = 0x25
    ALTPIPE = 0x26
    PIPEDS = 0x27
    PATCHMESH = 0x28
    CHUNKGROUPSTART = 0x29
    CHUNKGROUPEND = 0x2A
    UVANIMDICT = 0x2B
    COLLTREE = 0x2C
    # plugin ids (toolkit 0x01xx, world 0x05xx)
    METRICS = 0x101
    SPLINE_PLG = 0x102
    STEREO = 0x103
    VRML = 0x104
    MORPH = 0x105
    PVS = 0x106
    MEMLEAK = 0x107
    ANIMATION = 0x108
    GLOSS = 0x109
    LOGO = 0x10A
    MEMINFO = 0x10B
    RANDOM = 0x10C
    PNGIMAGE = 0x10D
    BONE = 0x10E
    VRMLANIM = 0x10F
    SKYMIPMAP = 0x110
    MRM = 0x111
    LODATOMIC = 0x112
    ME = 0x113
    LIGHTMAP = 0x114
    REFINE = 0x115
    SKIN = 0x116
    LABEL = 0x117
    PARTICLES = 0x118
    GEOMTX = 0x119
    SYNTHCORE = 0x11A
    STQPP = 0x11B
    PARTPP = 0x11C
    COLLISION = 0x11D
    HANIM = 0x11E
    USERDATA = 0x11F
    MATERIALEFFECTS = 0x120
    BINMESH = 0x50E
    NATIVEDATA = 0x510


RW_NAMES = {v: k.title().replace("_", "") for k, v in vars(RW).items() if isinstance(v, int)}
RW_NAMES.update({
    0x01: "Struct", 0x02: "String", 0x03: "Extension", 0x0E: "FrameList", 0x0F: "Geometry",
    0x10: "Clump", 0x14: "Atomic", 0x1A: "GeometryList", 0x07: "Material", 0x08: "MaterialList",
    0x06: "Texture", 0x50E: "BinMesh", 0x11E: "HAnim", 0x116: "Skin", 0x10E: "Bone",
})

#: RW section ids parsed as containers by default.
RW_CONTAINERS = frozenset({
    RW.EXTENSION, RW.CAMERA, RW.TEXTURE, RW.MATERIAL, RW.MATLIST, RW.ATOMICSECT, RW.PLANESECT,
    RW.WORLD, RW.FRAMELIST, RW.GEOMETRY, RW.CLUMP, RW.LIGHT, RW.ATOMIC, RW.TEXTURENATIVE,
    RW.TEXDICTIONARY, RW.GEOMETRYLIST,
})


def _parse_rw_seq(buf, start, end, containers, strict, base, depth):
    out = []
    pos = start
    while end - pos >= 12:
        typ, size, ver = struct.unpack_from("<III", buf, pos)
        if pos + 12 + size > end:
            if strict:
                raise ChunkTruncatedError(f"RW chunk 0x{typ:X} size {size} overruns parent end 0x{base + end:X}",
                                          offset=base + pos)
            break
        p0, p1 = pos + 12, pos + 12 + size
        ch = None
        if typ in containers and depth < MAX_DEPTH:
            try:
                kids, trail = _parse_rw_seq(buf, p0, p1, containers, True, base, depth + 1)
                ch = Chunk(typ, children=kids, version=ver, trailing=trail, offset=base + pos)
            except ChunkError:
                ch = None  # not a clean chunk sequence: keep as opaque leaf
        if ch is None:
            ch = Chunk(typ, data=bytes(buf[p0:p1]), version=ver, offset=base + pos)
        out.append(ch)
        pos = p1
    if strict and pos != end:
        raise ChunkTruncatedError(f"{end - pos} stray bytes after last RW chunk", offset=base + pos)
    return out, bytes(buf[pos:end])


def parse_rw_ex(data, containers=RW_CONTAINERS, strict=False, base=0):
    """Parse an RW chunk stream. Returns ``(chunks, trailing)``."""
    return _parse_rw_seq(memoryview(data), 0, len(data), frozenset(containers), strict, base, 0)


def parse_rw(data, containers=RW_CONTAINERS, strict=True, base=0):
    """Parse an RW chunk stream that must cover ``data`` exactly; returns the chunk list."""
    chunks, _ = parse_rw_ex(data, containers, strict=strict, base=base)
    return chunks


def write_rw(chunks, trailing=b"") -> bytes:
    """Serialise a list of RW chunks (+ optional trailing bytes)."""
    return b"".join(c.to_bytes() for c in chunks) + bytes(trailing)


# --------------------------------------------------------------------------- struct helpers

class Reader:
    """Bounds-checked little-endian cursor over a bytes-like object.

    ``r = Reader(buf); n = r.u32(); xs = r.f32s(3); raw = r.bytes(16)``.
    Any read past the end raises :class:`ChunkError`.
    """

    __slots__ = ("buf", "pos", "end", "base", "fmt", "what")

    def __init__(self, buf, pos=0, end=None, *, base=0, fmt=None, what="struct"):
        self.buf = memoryview(buf)
        self.pos = pos
        self.end = len(self.buf) if end is None else end
        self.base = base  # added to offsets in error messages
        self.fmt = fmt
        self.what = what

    def remaining(self):
        return self.end - self.pos

    def _take(self, n):
        if n < 0 or self.pos + n > self.end:
            raise ChunkTruncatedError(f"{self.what}: need {n} bytes, {max(0, self.end - self.pos)} left",
                                      fmt=self.fmt, offset=self.base + self.pos)
        p = self.pos
        self.pos += n
        return p

    def unpack(self, fmt):
        fmt = "<" + fmt
        p = self._take(struct.calcsize(fmt))
        return struct.unpack_from(fmt, self.buf, p)

    def array(self, code, count):
        """``count`` items of struct code (e.g. 'f', 'H', 'I') as a list (size checked before allocating)."""
        if count < 0:
            raise ChunkError(f"{self.what}: negative count {count}", fmt=self.fmt, offset=self.base + self.pos)
        size = struct.calcsize("<" + code) * count
        p = self._take(size)
        return list(struct.unpack_from(f"<{count}{code}", self.buf, p))

    def u8(self):
        return self.unpack("B")[0]

    def u16(self):
        return self.unpack("H")[0]

    def u32(self):
        return self.unpack("I")[0]

    def i32(self):
        return self.unpack("i")[0]

    def f32(self):
        return self.unpack("f")[0]

    def tuples(self, fmt, count):
        """``count`` records of struct ``fmt`` (e.g. '3f') as a list of tuples."""
        if count < 0:
            raise ChunkError(f"{self.what}: negative count {count}", fmt=self.fmt, offset=self.base + self.pos)
        st = struct.Struct("<" + fmt)
        p = self._take(st.size * count)
        return list(st.iter_unpack(self.buf[p:p + st.size * count])) if count else []

    def bytes(self, n):
        p = self._take(n)
        return bytes(self.buf[p:p + n])

    def rest(self):
        return self.bytes(self.end - self.pos)


class Writer:
    """Little-endian byte builder mirroring :class:`Reader`."""

    __slots__ = ("out",)

    def __init__(self):
        self.out = bytearray()

    def pack(self, fmt, *vals):
        self.out += struct.pack("<" + fmt, *vals)

    def array(self, code, vals):
        vals = list(vals)
        self.out += struct.pack(f"<{len(vals)}{code}", *vals)

    def tuples(self, fmt, records):
        st = struct.Struct("<" + fmt)
        for r in records:
            self.out += st.pack(*r)

    def u32(self, v):
        self.pack("I", v)

    def raw(self, b):
        self.out += b

    def getvalue(self):
        return bytes(self.out)

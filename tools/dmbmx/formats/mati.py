"""MATI material/texture chunks, shared by ZMD models and level ``Mat._sd`` files.

Spec: docs/formats/TEXTURE.md. Grammar::

    MATI := "MATI" u32 size { MADF (TBMP | TPNG) }
    MADF := "MADF" u32 size { u32 nameOffset=0x48; u8 params[64]; u32 nameId; char name[] (NUL, pad 4) }
    TBMP := "TBMP" u32 size { complete .bmp file, zero-padded to 4 }
    TPNG := "TPNG" u32 size { complete .png file, zero-padded to 4 }

The engine also registers a ``TDDS`` (.dds) image chunk (NgBMX.exe 0x004986FA); no shipped file has one.

``Mat._sd`` (one per level folder) is just a flat sequence of MATI chunks: :class:`MatSd`.
"""
from __future__ import annotations

import struct

from dmbmx.core.chunks import Chunk, ChunkError, ChunkTruncatedError, parse_4cc_ex, write_4cc
from dmbmx.core.errors import FormatError
from dmbmx.core.hash import zhash

__all__ = ["Mati", "MatSd", "MatiError", "IMAGE_TAGS", "SURFACE_NAMES", "image_length"]

IMAGE_TAGS = {b"TBMP": "bmp", b"TPNG": "png", b"TDDS": "dds"}
_KIND_TAG = {v: k for k, v in IMAGE_TAGS.items()}

# [I] MADF +0x38 bits 0-3, from texture names in the 12 levels (no name table in the exe found yet)
SURFACE_NAMES = {
    0: "default", 1: "metal", 2: "wood", 3: "wall", 4: "brick", 5: "grass", 6: "ocean", 7: "foliage",
    8: "rubber", 9: "sand", 10: "dirt", 11: "water", 12: "chainlink", 13: "fence", 14: "foam",
}


class MatiError(FormatError):
    def __init__(self, msg, *, offset=None, fmt="MATI"):
        super().__init__(msg, fmt=fmt, offset=offset)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1")


def _pad_name(name: str, raw: bytes | None) -> bytes:
    """Encode ``name`` NUL-terminated and 4-padded; reuse ``raw`` (with its padding) if unchanged."""
    if raw is not None and _cstr(raw) == name:
        return raw
    b = name.encode("latin-1") + b"\0"
    return b.ljust((len(b) + 3) & ~3, b"\0")


def image_length(d, kind):
    """Length of the image file inside a padded TBMP/TPNG payload (falls back to len(d))."""
    if kind == "bmp" and len(d) >= 6 and d[:2] == b"BM":
        n = struct.unpack_from("<I", d, 2)[0]
        if n <= len(d) and not any(d[n:]):
            return n
    elif kind == "png":
        i = d.rfind(b"IEND")
        if i >= 4 and i + 8 <= len(d) and not any(d[i + 8:]):
            return i + 8
    return len(d)


def _madf_prop(off, fmt, doc):
    size = struct.calcsize(fmt)

    def get(self):
        return struct.unpack_from(fmt, self.madf_head, off)

    def set(self, v):
        h = bytearray(self.madf_head)
        struct.pack_into(fmt, h, off, *v)
        self.madf_head = bytes(h)
    get.__doc__ = doc
    assert off + size <= 68
    return property(get, set, doc=doc)


class Mati:
    """``MATI``: one embedded texture = ``MADF`` (material def) + ``TBMP``/``TPNG``.

    ``name`` / ``name_id``: from MADF (the RW texture name, upper-cased, and its zhash).
    ``madf_head``: MADF bytes 0..68 kept raw (u32 name offset 0x48 + 64 bytes of parameters);
    the typed views below read/write it in place. ``kind``: 'bmp' | 'png' | 'dds'; ``image``: the
    complete image file; ``pad``: zero bytes that pad the image to 4 (kept verbatim).
    """
    __slots__ = ("madf_head", "name", "name_id", "name_raw", "kind", "image", "pad", "img_tag")

    # [V] copied by the MATI handler (0x004892B0) into the engine material; [I] meaning
    flags = _madf_prop(0x04, "<I", "MADF+0x04 (bit 0 tested by 0x00489BA0); always 0 in the corpus")
    word08 = _madf_prop(0x08, "<I", "MADF+0x08, copied to material+0x0C; always 0")
    word0c = _madf_prop(0x0C, "<I", "MADF+0x0C, copied to material+0x10; always 0")
    # [V] not read by the loader; [I] exporter lighting parameters (Max material colours?)
    diffuse = _madf_prop(0x10, "<3f", "MADF+0x10 3 floats, always 0.9 (unused by the game)")
    color_b = _madf_prop(0x1C, "<3f", "MADF+0x1C 3 floats, mostly 0.5 (unused by the game)")
    color_c = _madf_prop(0x28, "<3f", "MADF+0x28 3 floats, mostly 0.1 (unused by the game)")
    param34 = _madf_prop(0x34, "<f", "MADF+0x34 float, always 0.25 (unused by the game)")
    # [V] copied to material+0x14/0x18/0x1C
    surface_word = _madf_prop(0x38, "<I", "MADF+0x38: bits 0-3 surface type [I]; bits 4-5, 8 and 13-14 animated-material flags/frame [I]")
    anim1 = _madf_prop(0x3C, "<I", "MADF+0x3C: animated-material parameter (FAN textures only) [I]")
    anim2 = _madf_prop(0x40, "<I", "MADF+0x40: animated-material parameter (FAN textures only) [I]")

    @property
    def surface(self):
        """[I] surface type (bits 0-3 of MADF+0x38): see SURFACE_NAMES."""
        return self.surface_word[0] & 0x0F

    @classmethod
    def new(cls, name, image, kind=None):
        """A fresh MATI with the corpus-default MADF parameters."""
        m = cls()
        m.madf_head = struct.pack("<I3I3f3f3f f3I", 0x48, 0, 0, 0, 0.9, 0.9, 0.9, 0.5, 0.5, 0.5,
                                  0.1, 0.1, 0.1, 0.25, 0, 0, 0)
        m.name, m.name_raw, m.name_id = name.upper(), None, zhash(name.upper())
        m.kind = kind or ("png" if image[:4] == b"\x89PNG" else "bmp")
        m.img_tag, m.image, m.pad = None, bytes(image), b""
        return m

    @classmethod
    def from_chunk(cls, ch, fmt="MATI"):
        kids = ch.children
        if kids is None or len(kids) != 2:
            raise MatiError("MATI: expected MADF + image chunk", offset=ch.offset, fmt=fmt)
        madf, img = kids
        if madf.tag != b"MADF" or madf.children is not None:
            raise MatiError(f"MATI: expected MADF, got {madf.tag!r}", offset=madf.offset, fmt=fmt)
        m = cls()
        d = madf.data
        if len(d) < 72:
            raise ChunkTruncatedError(f"MADF: {len(d)} bytes, need >= 72", fmt=fmt, offset=madf.offset)
        if len(d) >= 0x90:   # [V] 0x004893BE asserts "Data too big"
            raise MatiError(f"MADF: {len(d)} bytes, the game accepts < 0x90", offset=madf.offset, fmt=fmt)
        m.madf_head = d[:68]
        m.name_id = struct.unpack_from("<I", d, 68)[0]
        m.name_raw = d[72:]
        m.name = _cstr(m.name_raw)
        if img.tag not in IMAGE_TAGS or img.children is not None:
            raise MatiError(f"MATI: unknown image chunk {img.name}", offset=img.offset, fmt=fmt)
        m.img_tag = img.tag
        m.kind = IMAGE_TAGS[img.tag]
        n = image_length(img.data, m.kind)
        m.image, m.pad = img.data[:n], img.data[n:]
        return m

    def to_chunk(self):
        madf = self.madf_head + struct.pack("<I", self.name_id) + _pad_name(self.name, self.name_raw)
        pad = self.pad if (len(self.image) + len(self.pad)) % 4 == 0 else b"\0" * (-len(self.image) % 4)
        tag = self.img_tag if self.img_tag else _KIND_TAG[self.kind]
        return Chunk(b"MATI", children=[Chunk(b"MADF", data=madf), Chunk(tag, data=self.image + pad)])

    def set_image(self, image, kind=None):
        """Replace the embedded file. ``kind`` defaults to the sniffed type; the chunk tag follows it."""
        image = bytes(image)
        kind = kind or ("png" if image[:4] == b"\x89PNG" else "bmp" if image[:2] == b"BM" else None)
        if kind not in _KIND_TAG:
            raise ValueError("image must be a .bmp or .png file")
        self.image, self.pad, self.kind, self.img_tag = image, b"\0" * (-len(image) % 4), kind, _KIND_TAG[kind]

    @property
    def filename(self):
        return f"{self.name}.{self.kind}"

    def __repr__(self):
        return f"<Mati {self.name!r} {self.kind} {len(self.image)} bytes>"


class MatSd:
    """A level ``Mat._sd``: a flat list of MATI chunks (``items``; unknown chunks kept raw)."""

    FMT = "MAT_SD"

    def __init__(self, items=None, trailing=b""):
        self.items = items if items is not None else []
        self.trailing = trailing

    @classmethod
    def read(cls, data):
        data = bytes(data)
        if data[:4] != b"MATI":
            from dmbmx.core.errors import BadMagicError
            raise BadMagicError(f"expected MATI, got {data[:4]!r}", fmt=cls.FMT, offset=0)
        try:
            roots, trailing = parse_4cc_ex(data, (b"MATI",), strict=True)
        except ChunkError as e:
            raise MatiError(f"chunk tree: {e.msg}", offset=e.offset, fmt=cls.FMT) from e
        if trailing:
            raise MatiError(f"{len(trailing)} stray bytes after the last chunk", fmt=cls.FMT,
                            offset=len(data) - len(trailing))
        items = [Mati.from_chunk(c, cls.FMT) if c.tag == b"MATI" else c for c in roots]
        return cls(items, trailing)

    def write(self):
        return write_4cc([i.to_chunk() if isinstance(i, Mati) else i for i in self.items], self.trailing)

    def materials(self):
        return [i for i in self.items if isinstance(i, Mati)]

    def texture(self, name):
        key = name.upper()
        return next((m for m in self.materials() if m.name.upper() == key), None)

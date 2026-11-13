"""ZAL asset library (".zal"): reader / writer (DM-010). Spec: docs/formats/ZAL.md.

Layout (all little-endian, [V] over the 6 shipped files and NgBMX.exe zasset.c 0x004804D0):

    0x00  u32  magic 0x2A81511C ("1C 51 81 2A")
    0x04  12   reserved (all zero in the corpus; not read by the loader)
    0x10  u16  flags   bits 0-12 assets per group, bit 13 "ref mode", bit 15 compressed
    0x12  u16  count   bits 0-13 number of groups, bit 14 method (0 = LZRW3-A, 1 = LZO1X), bit 15 compressed
    0x14  group table: count x { u32 id; assets_per_group x asset record }
          asset record = u32 offset, u32 size [, u32 stored_size if compressed]
    ...   asset data; offsets are relative to 0x10; size bits 24-31 are masked off by the loader

The writer preserves every byte: the 12 reserved bytes, the raw size words, the
padding bytes found between assets (often 0xCD / leftover heap bytes) and any
trailing bytes after the last asset.
"""
from ..core.binio import align, p16, p32, take, u16, u32, u32s
from ..core.errors import BadMagicError, FormatError, LimitError, TruncatedError
from ..core import lzo, lzrw3a

MAGIC = 0x2A81511C
MAGIC_BYTES = b"\x1c\x51\x81\x2a"
HEADER_SIZE = 0x10       # magic + 12 reserved bytes; asset offsets are relative to this
TABLE_OFF = 0x14         # first group record

FLAG_PER_GROUP_MASK = 0x1FFF
FLAG_REF_MODE = 0x2000
FLAG_COMPRESSED = 0x8000
COUNT_MASK = 0x3FFF
COUNT_METHOD_BIT = 0x4000
COUNT_COMPRESSED = 0x8000

METHOD_NONE = 0
METHOD_1 = 1             # LZRW3-A (exe 0x0049BD10 / 0x0049B8E0, DM-042; dmbmx.core.lzrw3a); unused in corpus
METHOD_LZRW3A = METHOD_1
METHOD_LZO1X = 2         # exe 0x0049C0AC

SIZE_MASK = 0x00FFFFFF


class ZalError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="ZAL", offset=offset)


def is_zal(data):
    return bytes(data[:4]) == MAGIC_BYTES


class ZalAsset:
    """One asset of a group. ``stored`` is the bytes exactly as in the file (compressed or not)."""

    __slots__ = ("offset", "size_field", "stored", "pad", "pad_from")

    def __init__(self, stored, size_field, offset=None, pad=None, pad_from=None):
        self.stored = bytes(stored)
        self.size_field = size_field   # raw u32; low 24 bits = (uncompressed) size
        self.offset = offset           # as read (relative to 0x10); recomputed by write()
        self.pad = pad                 # bytes before this asset's data; None = align to 4 with zeros
        self.pad_from = pad_from       # file position where `pad` started when read; the writer
                                       # reuses `pad` only while the layout up to here is unchanged

    @property
    def size(self):
        return self.size_field & SIZE_MASK

    def __repr__(self):
        return f"ZalAsset(off=0x{self.offset or 0:x}, size=0x{self.size:x}, stored=0x{len(self.stored):x})"


class ZalEntry:
    """A group: an id plus ``assets_per_group`` assets (always 1 in the shipped files)."""

    __slots__ = ("id", "assets", "_archive")

    def __init__(self, id, assets, archive=None):
        self.id = id
        self.assets = assets
        self._archive = archive

    @property
    def raw(self):
        """Stored bytes of the first asset (compressed if the archive is compressed)."""
        return self.assets[0].stored

    def data(self, index=0):
        """The asset's payload, decompressed when needed."""
        return self._archive.asset_data(self.assets[index])

    @property
    def type(self):
        """First 4 bytes of the (decompressed) payload: a 4CC like b'FORM', b'BM..', b'STRS', or the ZAL magic."""
        return self.data()[:4]

    def __repr__(self):
        return f"ZalEntry(id=0x{self.id:x}, assets={self.assets})"


class ZalArchive:
    def __init__(self):
        self.reserved = bytes(12)
        self.flags = 1
        self.count_word = 0
        self.entries = []
        self.tail = b""
        self.headerless = False   # in-memory form: no magic/reserved; table at 0, offsets from 0 (DM-016)

    @property
    def _base(self):
        return 0 if self.headerless else HEADER_SIZE

    # ---- header bit fields -------------------------------------------------
    @property
    def assets_per_group(self):
        return self.flags & FLAG_PER_GROUP_MASK

    @property
    def ref_mode(self):
        return bool(self.flags & FLAG_REF_MODE)

    @property
    def compressed(self):
        # exe: field_1a = (flags >> 15) * 2 + (count >> 15); nonzero selects 12-byte asset records
        return bool(self.flags & FLAG_COMPRESSED or self.count_word & COUNT_COMPRESSED)

    @property
    def method(self):
        if not self.compressed:
            return METHOD_NONE
        return METHOD_LZO1X if self.count_word & COUNT_METHOD_BIT else METHOD_1

    @property
    def record_size(self):
        return 4 + self.assets_per_group * (12 if self.compressed else 8)

    # ---- read ----------------------------------------------------------------
    @classmethod
    def read(cls, data, headerless=False):
        """Parse a ZAL. ``headerless=True`` reads the in-memory form used inside motion.zal
        (u16 flags, u16 count, group table, data; offsets relative to byte 0; [V] NgBMX.exe
        0x00475200 accepts it when the magic is absent)."""
        data = memoryview(bytes(data))
        n = len(data)
        self = cls()
        self.headerless = headerless
        base = self._base
        if n < base + 4:
            raise TruncatedError(f"file is {n} bytes, header needs {base + 4}", fmt="ZAL", offset=0)
        if not headerless:
            if u32(data, 0, fmt="ZAL") != MAGIC:
                raise BadMagicError(f"bad magic {bytes(data[:4]).hex(' ')}, expected 1c 51 81 2a", fmt="ZAL",
                                    offset=0)
            self.reserved = take(data, 4, 12, fmt="ZAL", what="reserved")
        self.flags = u16(data, base, fmt="ZAL", what="flags")
        self.count_word = u16(data, base + 2, fmt="ZAL", what="count")
        table_off = base + 4
        per = self.assets_per_group
        count = self.count_word & COUNT_MASK
        if count and per == 0:
            raise ZalError("assets-per-group is 0 but the group count is not", offset=0x10)
        rec = self.record_size
        table_end = table_off + count * rec
        if table_end > n:
            raise TruncatedError(f"group table ({count} x {rec} bytes) runs past end of file ({n})",
                                 fmt="ZAL", offset=table_off)
        words = 3 if self.compressed else 2
        cursor = table_end
        for g in range(count):
            roff = table_off + g * rec
            gid = u32(data, roff, fmt="ZAL")
            assets = []
            for a in range(per):
                aoff = roff + 4 + a * 4 * words
                vals = u32s(data, aoff, words, fmt="ZAL", what="asset record")
                rel, size_field = vals[0], vals[1]
                stored_len = vals[2] if self.compressed else size_field & SIZE_MASK
                start = base + rel
                if start < cursor:
                    raise ZalError(f"group 0x{gid:x} asset {a}: data at 0x{start:x} overlaps the table or the "
                                   f"previous asset (ends 0x{cursor:x}); out-of-order layouts are unsupported",
                                   offset=aoff)
                if start + stored_len > n:
                    raise TruncatedError(f"group 0x{gid:x} asset {a}: 0x{stored_len:x} bytes at 0x{start:x} "
                                         f"run past end of file (0x{n:x})", fmt="ZAL", offset=aoff)
                pad = bytes(data[cursor:start])
                assets.append(ZalAsset(data[start:start + stored_len], size_field, rel, pad, cursor))
                cursor = start + stored_len
            self.entries.append(ZalEntry(gid, assets, self))
        self.tail = bytes(data[cursor:])
        return self

    # ---- write ---------------------------------------------------------------
    def write(self):
        count = len(self.entries)
        if count > COUNT_MASK:
            raise LimitError(f"{count} groups do not fit the 14-bit count field", fmt="ZAL")
        per = self.assets_per_group
        for e in self.entries:
            if len(e.assets) != per:
                raise ZalError(f"group 0x{e.id:x} has {len(e.assets)} assets, header says {per}")
        compressed = self.compressed
        rec = self.record_size
        out = bytearray()
        base = self._base
        if not self.headerless:
            out += MAGIC_BYTES + self.reserved
        out += p16(self.flags) + p16((self.count_word & ~COUNT_MASK & 0xFFFF) | count)
        table_at = len(out)
        out += bytes(count * rec)
        for g, e in enumerate(self.entries):
            out[table_at + g * rec:table_at + g * rec + 4] = p32(e.id)
            for a, asset in enumerate(e.assets):
                keep = asset.pad is not None and asset.pad_from in (None, len(out))
                pad = asset.pad if keep else bytes(align(len(out), 4) - len(out))
                out += pad
                rel = len(out) - base
                if not compressed and len(asset.stored) != asset.size:
                    raise ZalError(f"group 0x{e.id:x} asset {a}: stored length {len(asset.stored)} "
                                   f"!= size field {asset.size}")
                if rel > 0xFFFFFFFF or len(asset.stored) > SIZE_MASK and not compressed:
                    raise LimitError("archive too large for 32-bit offsets / 24-bit sizes", fmt="ZAL")
                asset.offset = rel
                rpos = table_at + g * rec + 4 + a * (12 if compressed else 8)
                vals = p32(rel) + p32(asset.size_field)
                if compressed:
                    vals += p32(len(asset.stored))
                out[rpos:rpos + len(vals)] = vals
                out += asset.stored
        out += self.tail
        return bytes(out)

    # ---- payload access ------------------------------------------------------
    def asset_data(self, asset):
        """Decompressed payload of ``asset`` (raises ZalError for unsupported codecs)."""
        m = self.method
        if m == METHOD_NONE:
            return asset.stored
        if m == METHOD_LZO1X:
            return lzo.decompress(asset.stored, asset.size)
        return lzrw3a.decompress(asset.stored, asset.size)

    def set_data(self, id, payload, index=0):
        """Replace one asset's payload (uncompressed bytes). Compressed archives re-encode it with
        LZO1X (dmbmx.core.lzo.compress); every other asset keeps its original stored bytes.
        Method-1 archives are re-encoded with LZRW3-A (dmbmx.core.lzrw3a.compress).
        Later assets are re-aligned to 4 bytes with zero padding when the layout shifts."""
        asset = self.find(id).assets[index]
        payload = bytes(payload)
        if len(payload) > SIZE_MASK:
            raise LimitError(f"payload of {len(payload)} bytes exceeds the 24-bit size field", fmt="ZAL")
        m = self.method
        if m == METHOD_NONE:
            stored = payload
        elif m == METHOD_LZO1X:
            stored = lzo.compress(payload)
        else:
            stored = lzrw3a.compress(payload)
        asset.stored = stored
        asset.size_field = (asset.size_field & ~SIZE_MASK & 0xFFFFFFFF) | len(payload)
        return asset

    def find(self, id):
        for e in self.entries:
            if e.id == id:
                return e
        raise KeyError(id)

    def __repr__(self):
        return (f"ZalArchive(groups={len(self.entries)}, per_group={self.assets_per_group}, "
                f"compressed={self.compressed}, method={self.method}, ref_mode={self.ref_mode})")


def read(data):
    return ZalArchive.read(data)


def write(archive):
    return archive.write()

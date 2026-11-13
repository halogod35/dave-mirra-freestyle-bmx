"""STRS string tables (miscdata.zal 0x7919-0x791C, 0x7D01-0x7D0C). Spec: docs/formats/ZAL.md 3.1 (DM-054/DM-066).

    "STRS", u16 base_id, u16 count, u16 offset[count] (from the tag), then NUL-terminated latin-1 strings

Text id = base_id + index (``zTextGet``). [V] The shipped tables share strings: 16-23 % of the
offsets point at a string already used (an equal text or the tail of one), so the string area is kept
as it is. ``set`` appends an edited string at the end and points only that id at it; an unedited table
writes back byte for byte.

    t = strs.read(data); t.get(0x24) -> "PROQUEST"; t.set(0x24, "MOD QUEST"); t.write()
"""
import struct

from ..core.errors import BadMagicError, FormatError, LimitError, need

FMT = "STRS"
FRONTEND_TABLES = (0x7919, 0x791A, 0x791B, 0x791C)     # 0x7919 + language (frontendLoadMacros)
FRONTEND_MAX = 0x3FFF                                     # "FRONTEND: Macro data too big or too small"
LANGUAGES = ("US English", "English", "French", "German")


class StrsError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


class Table:
    def __init__(self, base_id, offsets, blob):
        self.base_id = base_id
        self.offsets = list(offsets)          # from the "STRS" tag
        self.blob = bytes(blob)               # everything after the offset table

    @property
    def data_start(self):
        return 8 + 2 * len(self.offsets)

    def __len__(self):
        return len(self.offsets)

    def ids(self):
        return range(self.base_id, self.base_id + len(self.offsets))

    def get(self, text_id):
        k = text_id - self.base_id
        if not 0 <= k < len(self.offsets):
            raise KeyError(f"text 0x{text_id:x} not in table (0x{self.base_id:x}..)")
        o = self.offsets[k] - self.data_start
        end = self.blob.find(b"\0", o)
        return self.blob[o:end].decode("latin-1")

    def strings(self):
        return [self.get(i) for i in self.ids()]

    def set(self, text_id, s):
        """Point ``text_id`` at ``s`` (appended to the string area unless it is unchanged)."""
        if self.get(text_id) == s:
            return False
        enc = s.encode("latin-1")
        if b"\0" in enc:
            raise ValueError("a text cannot contain NUL")
        off = self.data_start + len(self.blob)
        if off > 0xFFFF:
            raise LimitError(f"string at 0x{off:x} past the u16 offset range", fmt=FMT)
        self.blob += enc + b"\0"
        self.offsets[text_id - self.base_id] = off
        return True

    def write(self):
        return write(self)


def read(data):
    data = bytes(data)
    need(data, 0, 8, fmt=FMT, what="header")
    if data[:4] != b"STRS":
        raise BadMagicError(f"tag {data[:4]!r}, expected b'STRS'", fmt=FMT, offset=0)
    base, n = struct.unpack_from("<HH", data, 4)
    need(data, 8, 2 * n, fmt=FMT, what="offset table")
    offs = struct.unpack_from(f"<{n}H", data, 8)
    start = 8 + 2 * n
    for k, o in enumerate(offs):
        if o < start or o >= len(data) or data.find(b"\0", o) < 0:
            raise StrsError(f"text {k}: offset 0x{o:x} outside the string area", offset=8 + 2 * k)
    return Table(base, offs, data[start:])


def write(t):
    n = len(t.offsets)
    out = b"STRS" + struct.pack(f"<HH{n}H", t.base_id, n, *t.offsets) + t.blob
    if len(out) > 0xFFFF + 1:
        raise ValueError("table larger than the u16 offsets can address")
    return out

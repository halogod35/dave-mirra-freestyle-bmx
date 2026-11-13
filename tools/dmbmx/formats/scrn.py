"""GUI screen groups and SCRN screens (Intwrap.zal group 0). Spec: docs/formats/ZAL.md 3.2.

The front end's art library ("intart.zal"; on PC the nested ZAL in Intwrap.zal group 0) holds 35
zprim *groups* (ids 0xE000-0xE022). A group is

    u32 count, count x (u32 asset_id, u32 offset), then the assets back to back

(zprimLoadGroup 0x0047B400). The asset type is the top three bits of its id: 0x0xxx picture,
0x4xxx sprite, 0x8xxx font, 0xAxxx screen (``SCRN``). Group 0xE000 is permanent (fonts, shared
sprites); group 0xE000 | n holds screen 0xA000 | n and its art (guiLoadScreenNow).

A SCRN (guiLoadScreenFile 0x00439A00) is a 0x2C-byte header, ``edef_count`` 0xD4-byte ``EDEF``
element records and a ``STRS`` block: the tag, then the element texts as NUL-terminated strings.
An EDEF's text offset (+0xB4; the toggle text +0xC8) counts from the ``STRS`` tag, 0 = no text.
[V] In all 34 shipped screens the strings are exactly the EDEF texts in EDEF order (text, then toggle
text), one string per text-bearing EDEF, ``text_count`` is their number and the block is padded with
NULs to a multiple of 4 bytes; ``Screen.set_text`` rebuilds the block that way. Every EDEF of the shipped screens is type 1 (a sprite / text element).

    grp = scrn.read_group(payload); scrn.write_group(grp) == payload
    scr = scrn.read_screen(grp.asset(0xA009)); scr.texts(); scr.set_text(3, "HELLO")
    grp.set_asset(0xA009, scr.write())
    scrn.read_intwrap(intwrap_zal_bytes) -> {group_id: Group}
"""
import struct

from ..core.errors import BadMagicError, FormatError, LimitError, TruncatedError, need

FMT = "SCRN"
MAX_ASSETS = 4096
MAX_EDEFS = 100            # GUI_MAX_ELEMENTS: guiCreateElement fails past 100
HEADER = 0x2C
EDEF_SIZE = 0xD4

TYPE_PICTURE, TYPE_SPRITE, TYPE_FONT, TYPE_SCREEN = 0x0000, 0x4000, 0x8000, 0xA000


class ScrnError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


def asset_type(asset_id):
    """'picture' / 'sprite' / 'sprite2' / 'font' / 'screen' / 'vsprite' (zprim.h ZPRIM_TYPE_*)."""
    return {0x0000: "picture", 0x2000: "picture", 0x4000: "sprite", 0x6000: "sprite2", 0x8000: "font",
            0xA000: "screen", 0xC000: "vsprite", 0xE000: "group"}[asset_id & 0xE000]


# ------------------------------------------------------------------ groups

class Group:
    """A zprim group: ``entries`` = [(asset_id, bytes)] in table order."""

    def __init__(self, entries=None):
        self.entries = list(entries or [])

    def ids(self):
        return [i for i, _ in self.entries]

    def asset(self, asset_id):
        for i, d in self.entries:
            if i == asset_id:
                return d
        raise KeyError(f"asset 0x{asset_id:04x} not in group")

    def set_asset(self, asset_id, data):
        for k, (i, _) in enumerate(self.entries):
            if i == asset_id:
                self.entries[k] = (i, bytes(data))
                return
        raise KeyError(f"asset 0x{asset_id:04x} not in group")

    def screens(self):
        return [i for i in self.ids() if i & 0xE000 == TYPE_SCREEN]

    def write(self):
        return write_group(self)

    def __repr__(self):
        return f"<Group {len(self.entries)} assets {[hex(i) for i in self.ids()]}>"


def read_group(data):
    """Parse a group. [V] Every shipped group is canonical: the assets follow the table in table
    order with no gaps, so the writer can lay them out again."""
    data = bytes(data)
    end = need(data, 0, 4, fmt=FMT, what="group count")
    (count,) = struct.unpack_from("<I", data, 0)
    if count > MAX_ASSETS:
        raise LimitError(f"group: {count} assets", fmt=FMT, offset=0)
    need(data, end, 8 * count, fmt=FMT, what="group table")
    table = [struct.unpack_from("<II", data, 4 + 8 * k) for k in range(count)]
    pos = 4 + 8 * count
    entries = []
    for k, (aid, off) in enumerate(table):
        if off != pos:
            raise ScrnError(f"group: asset {k} at 0x{off:x}, expected 0x{pos:x} (not back to back)",
                            offset=4 + 8 * k)
        nxt = table[k + 1][1] if k + 1 < count else len(data)
        if nxt < off or nxt > len(data):
            raise TruncatedError(f"group: asset {k} ends at 0x{nxt:x}", fmt=FMT, offset=4 + 8 * k)
        entries.append((aid, data[off:nxt]))
        pos = nxt
    if count == 0 and len(data) != 4:
        raise ScrnError("group: bytes after an empty table", offset=4)
    return Group(entries)


def write_group(group):
    out = bytearray(struct.pack("<I", len(group.entries)))
    pos = 4 + 8 * len(group.entries)
    for aid, d in group.entries:
        out += struct.pack("<II", aid, pos)
        pos += len(d)
    for _, d in group.entries:
        out += d
    return bytes(out)


# ------------------------------------------------------------------ screens

# EDEF fields: (name, struct code, offset). Unlisted bytes (pads) are kept in Edef.raw.
EDEF_FIELDS = (
    ("id", "I", 0x04), ("link", "4B", 0x08), ("flags", "H", 0x0C), ("type", "H", 0x0E),
    ("sprite_param", "H", 0x10), ("x", "h", 0x12), ("y", "h", 0x14), ("w", "h", 0x16), ("h", "h", 0x18),
    ("scale_x", "h", 0x1A), ("scale_y", "h", 0x1C), ("slide_x", "h", 0x1E), ("slide_y", "h", 0x20),
    ("priority", "H", 0x22), ("sprite", "H", 0x24), ("frame", "H", 0x26), ("anim_count", "H", 0x28),
    ("anim_frames", "30H", 0x2A), ("anim_times", "30H", 0x66), ("sel_sprite", "H", 0xA2),
    ("sprite_flags", "H", 0xA4), ("sprite_x", "h", 0xA6), ("sprite_y", "h", 0xA8), ("font", "H", 0xAA),
    ("text_flags", "H", 0xAC), ("text_x", "h", 0xAE), ("text_y", "h", 0xB0), ("text", "I", 0xB4),
    ("text_rgb", "3B", 0xB8), ("spacing", "h", 0xBC), ("toggle_font", "H", 0xC0),
    ("toggle_flags", "H", 0xC2), ("toggle_x", "h", 0xC4), ("toggle_y", "h", 0xC6), ("toggle_text", "I", 0xC8),
    ("toggle_rgb", "3B", 0xCC), ("toggle_spacing", "h", 0xD0),
)
HEADER_FIELDS = (
    ("size", "I", 0x04), ("id", "I", 0x08), ("backdrop", "H", 0x0C), ("unk_0e", "H", 0x0E),
    ("edef_count", "i", 0x10), ("edef_offset", "i", 0x14), ("text_count", "i", 0x18), ("strs_offset", "i", 0x1C),
    ("focus_font", "H", 0x20), ("focus_rgb", "3B", 0x22), ("blur_font", "H", 0x26), ("blur_rgb", "3B", 0x28),
)


def _get(raw, fields):
    out = {}
    for name, code, off in fields:
        v = struct.unpack_from("<" + code, raw, off)
        out[name] = list(v) if len(v) > 1 else v[0]
    return out


def _put(raw, fields, values):
    for name, code, off in fields:
        v = values[name]
        struct.pack_into("<" + code, raw, off, *(v if isinstance(v, (list, tuple)) else (v,)))


class Edef:
    """One element record. ``fields`` are the parsed values; ``raw`` keeps every byte."""

    def __init__(self, raw):
        self.raw = bytearray(raw)
        self.fields = _get(self.raw, EDEF_FIELDS)

    def __getattr__(self, name):
        try:
            return self.__dict__["fields"][name]
        except KeyError:
            raise AttributeError(name) from None

    def to_bytes(self):
        raw = bytearray(self.raw)
        _put(raw, EDEF_FIELDS, self.fields)
        return bytes(raw)


class Screen:
    """A SCRN: ``header`` (dict), ``edefs`` [Edef], ``strs`` (the STRS block, tag included)."""

    def __init__(self, header_raw, edefs, strs):
        self.header_raw = bytearray(header_raw)
        self.header = _get(self.header_raw, HEADER_FIELDS)
        self.edefs = edefs
        self.strs = bytes(strs)

    def string_at(self, off):
        if off == 0:
            return None
        if off < 4 or off >= len(self.strs):
            raise ScrnError(f"text offset 0x{off:x} outside STRS")
        end = self.strs.find(b"\0", off)
        if end < 0:
            raise ScrnError(f"text at 0x{off:x} is not terminated")
        return self.strs[off:end].decode("latin-1")

    def texts(self):
        """[(edef index, element id, text, toggle text)] for the elements that have text."""
        out = []
        for k, e in enumerate(self.edefs):
            t, g = self.string_at(e.text), self.string_at(e.toggle_text)
            if t is not None or g is not None:
                out.append((k, e.id, t, g))
        return out

    def rebuild_strs(self, override=None):
        """Lay the strings out again in EDEF order (text, then toggle text); ``override`` maps
        (edef index, 'text' | 'toggle_text') to a new string. Updates the offsets and text_count."""
        override = override or {}
        block = bytearray(b"STRS")
        n = 0
        for k, e in enumerate(self.edefs):
            for field in ("text", "toggle_text"):
                if (k, field) in override:
                    s = override[(k, field)]
                else:
                    s = self.string_at(e.fields[field])
                if s is None:
                    e.fields[field] = 0
                    continue
                e.fields[field] = len(block)
                block += s.encode("latin-1") + b"\0"
                n += 1
        block += b"\0" * (-len(block) % 4)          # the block is padded to 4 bytes with NULs
        self.strs = bytes(block)
        self.header["text_count"] = n

    def set_text(self, index, text, toggle=False):
        self.rebuild_strs({(index, "toggle_text" if toggle else "text"): text})

    def write(self):
        return write_screen(self)


def read_screen(data):
    """Parse a SCRN asset. Checks what guiLoadScreenFile relies on ("SCRN", "EDEF" tags, the table
    inside the record) plus the layout every shipped screen has (header, EDEFs, STRS, back to back)."""
    data = bytes(data)
    need(data, 0, HEADER, fmt=FMT, what="SCRN header")
    if data[:4] != b"SCRN":
        raise BadMagicError(f"tag {data[:4]!r}, expected b'SCRN'", fmt=FMT, offset=0)
    h = _get(data, HEADER_FIELDS)
    if h["size"] != len(data):
        raise TruncatedError(f"size field 0x{h['size']:x}, record is 0x{len(data):x}", fmt=FMT, offset=4)
    n = h["edef_count"]
    if n < 0 or n > MAX_EDEFS:
        raise LimitError(f"{n} elements", fmt=FMT, offset=0x10)
    if h["edef_offset"] != HEADER:
        raise ScrnError(f"elements at 0x{h['edef_offset']:x}, expected 0x2c", offset=0x14)
    end = need(data, HEADER, n * EDEF_SIZE, fmt=FMT, what="EDEF table")
    if h["strs_offset"] != end:
        raise ScrnError(f"STRS at 0x{h['strs_offset']:x}, expected 0x{end:x}", offset=0x1C)
    need(data, end, 4, fmt=FMT, what="STRS tag")
    if data[end:end + 4] != b"STRS":
        raise BadMagicError(f"tag {data[end:end + 4]!r}, expected b'STRS'", fmt=FMT, offset=end)
    edefs = []
    for k in range(n):
        raw = data[HEADER + k * EDEF_SIZE:HEADER + (k + 1) * EDEF_SIZE]
        if raw[:4] != b"EDEF":
            raise BadMagicError(f"element {k}: tag {raw[:4]!r}", fmt=FMT, offset=HEADER + k * EDEF_SIZE)
        edefs.append(Edef(raw))
    scr = Screen(data[:HEADER], edefs, data[end:])
    for k, e in enumerate(edefs):           # every text offset must name a string inside STRS
        for field in ("text", "toggle_text"):
            off = e.fields[field]
            if off and (off < 4 or off >= len(scr.strs) or scr.strs.find(b"\0", off) < 0):
                raise ScrnError(f"element {k}: {field} offset 0x{off:x} outside STRS",
                                offset=HEADER + k * EDEF_SIZE)
    return scr


def write_screen(scr):
    n = len(scr.edefs)
    if n > MAX_EDEFS:
        raise ValueError(f"{n} elements (the game creates at most {MAX_EDEFS})")
    h = dict(scr.header)
    h["edef_count"] = n
    h["edef_offset"] = HEADER
    h["strs_offset"] = HEADER + n * EDEF_SIZE
    h["size"] = h["strs_offset"] + len(scr.strs)
    head = bytearray(scr.header_raw)
    head[:4] = b"SCRN"
    _put(head, HEADER_FIELDS, h)
    return bytes(head) + b"".join(e.to_bytes() for e in scr.edefs) + scr.strs


# ------------------------------------------------------------------ Intwrap.zal

def read_intwrap(data):
    """{group id: Group} of the GUI art in Intwrap.zal (its group 0 is the nested archive)."""
    from . import zal
    outer = zal.ZalArchive.read(data)
    inner = zal.ZalArchive.read(outer.find(0).data())
    return {e.id: read_group(e.data()) for e in inner.entries}

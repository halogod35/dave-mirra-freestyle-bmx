"""HUD / overlay fonts (miscdata.zal 0x7531/0x7532/0x7534/0x7535). Spec: docs/formats/ZAL.md 3.1 (DM-054/DM-066).

``zoveriiLoadFont`` 0x00459FC0 (zoverii.c) reads

    u8 count, u8 pad[3], u8 height, u8 pad[7]                          (12-byte header)
    count x { u8 char, u16 u, u16 pad, u16 v, u16 pad, u16 w, u16 pad }  (13-byte records)

and builds a table indexed by ``char - min(char)``; every glyph is ``w`` x ``height`` pixels at
(u, v) of the font's image. [V] The images are Material.ZAL BMPs, through the overlay picture table
``g_zresImages`` 0x0055D978 (resource 0x30 + n = Material.ZAL asset 9 + n):

    font 0x7531 small  -> resource 0x30 -> Material.ZAL 9   (256x256, 4-bit)
    font 0x7532 tiny   -> resource 0x32 -> Material.ZAL 11  (256x256, 8-bit)
    font 0x7534 button -> resource 0x33 -> Material.ZAL 12  (128x128, 8-bit)
    font 0x7535 icons  -> resource 0x34 -> Material.ZAL 13  (64x48, 8-bit)

Every byte is kept (header pads, record pads): ``write(read(b)) == b`` for all four [V].
"""
import struct

from ..core.errors import FormatError, need

FMT = "HUDFONT"
HEADER = 0x0C
RECORD = 13

# font asset -> (name, Material.ZAL image asset, overlay resource id)
FONTS = {
    0x7531: ("small", 0x09, 0x30),
    0x7532: ("tiny", 0x0B, 0x32),
    0x7534: ("button", 0x0C, 0x33),
    0x7535: ("icons", 0x0D, 0x34),
}


class HudFontError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


class Glyph:
    __slots__ = ("char", "u", "v", "w", "pads")

    def __init__(self, char, u, v, w, pads=b"\0" * 6):
        self.char, self.u, self.v, self.w = char, u, v, w
        self.pads = bytes(pads)            # the high u16 of u, v, w (0 in every shipped record)

    def to_bytes(self):
        p = self.pads
        return struct.pack("<BH2sH2sH2s", self.char, self.u, p[0:2], self.v, p[2:4], self.w, p[4:6])


class HudFont:
    def __init__(self, glyphs, height, header=None):
        self.glyphs = glyphs
        self.height = height
        self.header = bytearray(header or bytes(HEADER))   # raw header (pads kept)

    def glyph_for(self, ch):
        c = ord(ch) if isinstance(ch, str) else ch
        for g in self.glyphs:
            if g.char == c:
                return g
        return None

    def write(self):
        return write(self)

    def __repr__(self):
        return f"<HudFont {len(self.glyphs)} glyphs, height {self.height}>"


def read(data):
    data = bytes(data)
    need(data, 0, HEADER, fmt=FMT, what="header")
    n = data[0]
    end = need(data, HEADER, RECORD * n, fmt=FMT, what="records")
    if end != len(data):
        raise HudFontError(f"{len(data) - end} bytes after {n} records", offset=end)
    glyphs = []
    for k in range(n):
        o = HEADER + RECORD * k
        c, u, pu, v, pv, w, pw = struct.unpack_from("<BH2sH2sH2s", data, o)
        glyphs.append(Glyph(c, u, v, w, pu + pv + pw))
    return HudFont(glyphs, data[4], data[:HEADER])


def write(font):
    n = len(font.glyphs)
    if n > 255:
        raise ValueError(f"{n} glyphs (the count is a byte)")
    if not 0 <= font.height <= 255:
        raise ValueError(f"height {font.height}")
    head = bytearray(font.header)
    head[0] = n
    head[4] = font.height
    return bytes(head) + b"".join(g.to_bytes() for g in font.glyphs)

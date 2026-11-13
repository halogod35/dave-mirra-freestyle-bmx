"""zprim menu art: pictures, sprites and fonts in the GUI groups (Intwrap.zal). Spec: docs/formats/ZAL.md 3.3 (DM-066).

The 35 GUI groups (``dmbmx.formats.scrn``) hold three kinds of 2D art besides the SCRN screens; the
game reads them in place (``zprim.c``: zprimLoadPicture 0x00479010, zprimLoadSprite 0x00479660,
zprimLoadFont 0x0047A420, zprimMakeMaterial 0x0047C030, psx2DDrawGlyph 0x00463C70):

    Image (picture 0x0xxx / sprite 0x4xxx):
        u16 count, u16 flags, u16 width, u16 height, u32 pixels, u32 palette, u16 bpp, u16 pad
        Frame[count]: u16 w, u16 h, u32 pixels, i16 hot_x, i16 hot_y, u32 material (0 in the file)
        frame pixels back to back (4 or 8 bpp, rows top-down, 4-bit: left pixel in the LOW nibble)
        Palette: u16 unk, u16 count, u16 colour[count] (15-bit BGR555, red in the low bits;
                 0x7C1F = transparent), then the rest of the 0x204-byte block

    Font (0x8xxx):
        u16 count, u16 line_height, u32 pixels, u16 width, u16 height, u32 materials (0),
        u32 palette, u16 bpp (4), u16 unk_16, Glyph[count] (10 bytes), pad to 4 bytes,
        bitmap (width x height at bpp), Palette (16 banks of 16 colours = the text colours)
        Glyph: u8 x, y, w, h; i8 xoff, yoff, advance, descent; u16 code

Every byte is kept: ``write(read(b)) == b`` for all 249 distinct shipped assets [V]. PNG export keeps
the palette (palettised PNG of the asset's bit depth; 0x7C1F entries get alpha 0 through tRNS);
``image_from_png`` / ``font_from_png`` re-encode an edited PNG the way the game expects and give the
original bytes back for an unedited one.

    img = zprim.read_image(data); img.frames[0].pixels; zprim.write_image(img) == data
    png = zprim.frame_png(img, 0); img2 = zprim.image_from_png(img, [png]); img2.write() == data
    fnt = zprim.read_font(data); fnt.glyphs[0x41 - fnt.first]
"""
import struct

from ..convert import imaging
from ..core.errors import FormatError, LimitError, TruncatedError, need

FMT = "ZPRIM"
TRANSPARENT = 0x7C1F
IMAGE_HEADER = 0x14
FRAME_SIZE = 0x10
FONT_HEADER = 0x18
GLYPH_SIZE = 10
PALETTE_BLOCK = 0x204          # [V] every shipped palette block is 0x204 bytes (count 16 or 256 + rest)
MAX_FRAMES = 256
MAX_DIM = 1024
MAX_COLOURS = 256


class ZprimError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


# ------------------------------------------------------------------ colours

def rgb_of(c):
    """15-bit colour -> (r, g, b, a): channels << 3 as zprimMakeMaterial does; 0x7C1F -> alpha 0."""
    c &= 0xFFFF
    return ((c & 0x1F) << 3, ((c >> 5) & 0x1F) << 3, ((c >> 10) & 0x1F) << 3, 0 if c == TRANSPARENT else 255)


def colour_of(r, g, b, a=255):
    """(r, g, b, a) -> 15-bit colour; alpha < 128 -> 0x7C1F (transparent)."""
    if a < 128:
        return TRANSPARENT
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10


def parse_colour(v):
    """A JSON palette entry: an int, "0x7c1f" / "7C1F" (15-bit value) or "#rrggbb" (converted)."""
    if isinstance(v, int):
        c = v
    elif isinstance(v, str) and v.startswith("#") and len(v) in (7, 9):
        r, g, b = int(v[1:3], 16), int(v[3:5], 16), int(v[5:7], 16)
        a = int(v[7:9], 16) if len(v) == 9 else 255
        c = colour_of(r, g, b, a)
    elif isinstance(v, str):
        c = int(v, 16)
    else:
        raise ValueError(f"bad palette entry {v!r}")
    if not 0 <= c <= 0xFFFF:
        raise ValueError(f"palette entry {v!r} out of range")
    return c


# ------------------------------------------------------------------ pixel packing

def unpack_indices(data, width, height, bpp):
    """Packed zprim pixels -> rows of index lists (4-bit: low nibble first)."""
    stride = width * bpp // 8
    rows = []
    for y in range(height):
        r = data[y * stride:(y + 1) * stride]
        if bpp == 8:
            rows.append(list(r))
        else:
            row = []
            for b in r:
                row += (b & 0xF, b >> 4)
            rows.append(row[:width])
    return rows


def pack_indices(rows, width, bpp):
    out = bytearray()
    for row in rows:
        if bpp == 8:
            out += bytes(row)
        else:
            for x in range(0, width, 2):
                out.append((row[x] & 0xF) | (row[x + 1] & 0xF) << 4)
    return bytes(out)


def _check_bpp(bpp, width, off):
    if bpp not in (4, 8):
        raise ZprimError(f"bpp {bpp} (only 4 and 8 exist)", offset=off)
    if width * bpp % 8:
        raise ZprimError(f"width {width} is not a whole number of bytes at {bpp} bpp", offset=off)


# ------------------------------------------------------------------ palette block

class Palette:
    """``unk`` (u16 +0), ``colours`` [u16], ``tail`` (the bytes up to the end of the block)."""

    def __init__(self, colours, unk=0, tail=b""):
        self.colours = list(colours)
        self.unk = unk
        self.tail = bytes(tail)

    def to_bytes(self):
        return struct.pack(f"<HH{len(self.colours)}H", self.unk, len(self.colours), *self.colours) + self.tail

    def rgba(self, n=None):
        return [rgb_of(c) for c in self.colours[:n]]


def _read_palette(data, off, end, what):
    need(data, off, 4, fmt=FMT, what=f"{what} palette header")
    unk, n = struct.unpack_from("<HH", data, off)
    if n > MAX_COLOURS:
        raise LimitError(f"{what}: palette of {n} colours", fmt=FMT, offset=off)
    need(data, off + 4, 2 * n, fmt=FMT, what=f"{what} palette")
    if off + 4 + 2 * n > end:
        raise TruncatedError(f"{what}: palette runs past its block", fmt=FMT, offset=off)
    cols = struct.unpack_from(f"<{n}H", data, off + 4)
    return Palette(cols, unk, data[off + 4 + 2 * n:end])


# ------------------------------------------------------------------ images (pictures, sprites)

class Frame:
    def __init__(self, width, height, pixels, hot_x=0, hot_y=0, material=0):
        self.width, self.height = width, height
        self.pixels = bytes(pixels)
        self.hot_x, self.hot_y = hot_x, hot_y
        self.material = material

    def indices(self, bpp):
        return unpack_indices(self.pixels, self.width, self.height, bpp)


class Image:
    """A picture or sprite: ``frames`` [Frame], ``palette`` Palette, header fields."""

    def __init__(self, frames, palette, bpp, flags=0, width=None, height=None, pad_12=0):
        self.frames = frames
        self.palette = palette
        self.bpp = bpp
        self.flags = flags
        self.width = frames[0].width if width is None and frames else width
        self.height = frames[0].height if height is None and frames else height
        self.pad_12 = pad_12

    def write(self):
        return write_image(self)

    def __repr__(self):
        return (f"<Image {len(self.frames)} frame(s) {self.width}x{self.height} {self.bpp}bpp "
                f"{len(self.palette.colours)} colours>")


def read_image(data):
    """Parse a picture / sprite. [V] Every shipped image is canonical: frame pixels follow the frame
    table back to back and the palette follows the last frame, so the writer lays it out again."""
    data = bytes(data)
    need(data, 0, IMAGE_HEADER, fmt=FMT, what="image header")
    count, flags, width, height, pix, pal, bpp, pad = struct.unpack_from("<HHHHIIHH", data, 0)
    if count == 0 or count > MAX_FRAMES:
        raise LimitError(f"image with {count} frames", fmt=FMT, offset=0)
    need(data, IMAGE_HEADER, FRAME_SIZE * count, fmt=FMT, what="frame table")
    pos = IMAGE_HEADER + FRAME_SIZE * count
    if pix != pos:
        raise ZprimError(f"pixels at 0x{pix:x}, expected 0x{pos:x}", offset=8)
    frames = []
    for i in range(count):
        fw, fh, fp, hx, hy, mat = struct.unpack_from("<HHIhhI", data, IMAGE_HEADER + FRAME_SIZE * i)
        _check_bpp(bpp, fw, IMAGE_HEADER + FRAME_SIZE * i)
        if fw > MAX_DIM or fh > MAX_DIM:
            raise LimitError(f"frame {i} is {fw}x{fh}", fmt=FMT, offset=IMAGE_HEADER + FRAME_SIZE * i)
        if fp != pos:
            raise ZprimError(f"frame {i} pixels at 0x{fp:x}, expected 0x{pos:x}", offset=IMAGE_HEADER + FRAME_SIZE * i + 4)
        size = fw * fh * bpp // 8
        need(data, fp, size, fmt=FMT, what=f"frame {i} pixels")
        frames.append(Frame(fw, fh, data[fp:fp + size], hx, hy, mat))
        pos += size
    if pal != pos:
        raise ZprimError(f"palette at 0x{pal:x}, expected 0x{pos:x}", offset=0x0C)
    palette = _read_palette(data, pal, len(data), "image")
    return Image(frames, palette, bpp, flags, width, height, pad)


def write_image(img):
    if not img.frames or len(img.frames) > MAX_FRAMES:
        raise ValueError(f"{len(img.frames)} frames")
    n = len(img.frames)
    pos = IMAGE_HEADER + FRAME_SIZE * n
    table = bytearray()
    for i, f in enumerate(img.frames):
        if img.bpp not in (4, 8) or f.width * img.bpp % 8:
            raise ValueError(f"frame {i}: width {f.width} at {img.bpp} bpp")
        if len(f.pixels) != f.width * f.height * img.bpp // 8:
            raise ValueError(f"frame {i}: {len(f.pixels)} pixel bytes for {f.width}x{f.height}")
        table += struct.pack("<HHIhhI", f.width, f.height, pos, f.hot_x, f.hot_y, f.material)
        pos += len(f.pixels)
    head = struct.pack("<HHHHIIHH", n, img.flags, img.width, img.height, IMAGE_HEADER + FRAME_SIZE * n,
                       pos, img.bpp, img.pad_12)
    return head + bytes(table) + b"".join(f.pixels for f in img.frames) + img.palette.to_bytes()


# ------------------------------------------------------------------ fonts

GLYPH_FIELDS = ("x", "y", "w", "h", "xoff", "yoff", "advance", "descent", "code")


class Glyph:
    __slots__ = GLYPH_FIELDS

    def __init__(self, x=0, y=0, w=0, h=0, xoff=0, yoff=0, advance=0, descent=0, code=0):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.xoff, self.yoff, self.advance, self.descent, self.code = xoff, yoff, advance, descent, code

    def to_bytes(self):
        return struct.pack("<4B4bH", self.x, self.y, self.w, self.h, self.xoff, self.yoff, self.advance,
                           self.descent, self.code)

    def as_dict(self):
        return {k: getattr(self, k) for k in GLYPH_FIELDS}


class Font:
    """A zprim font. Glyph lookup (zprimFindGlyph, inlined in zprimDrawText 0x0047AA80): with fewer
    than 0x100 glyphs, glyph k is character ``first + k`` (``first`` = glyph 0's code); otherwise
    glyphs are searched by ``code``. A lower-case letter without a glyph falls back to upper case."""

    def __init__(self, glyphs, width, height, pixels, palette, line_height, bpp=4, materials=0, unk_16=0,
                 pad=b""):
        self.glyphs = glyphs
        self.width, self.height = width, height
        self.pixels = bytes(pixels)
        self.palette = palette
        self.line_height = line_height
        self.bpp = bpp
        self.materials = materials
        self.unk_16 = unk_16
        self.pad = bytes(pad)

    @property
    def first(self):
        return self.glyphs[0].code if self.glyphs and len(self.glyphs) < 0x100 else 0

    @property
    def indexed(self):
        return len(self.glyphs) < 0x100

    def glyph_for(self, ch):
        c = ord(ch) if isinstance(ch, str) else ch
        while True:
            if self.indexed:
                k = c - self.first
                if 0 <= k < len(self.glyphs):
                    return self.glyphs[k]
            else:
                for g in self.glyphs:
                    if g.code == c:
                        return g
            if not ord("a") <= c <= ord("z"):
                return None
            c -= 0x20

    def text_width(self, s, spacing=0):
        """zprimGetTextWidth 0x0047AFD0."""
        w = 0
        for ch in s:
            g = self.glyph_for(ch)
            if g is not None:
                w += g.advance + g.xoff + g.w + spacing
        return w

    def write(self):
        return write_font(self)

    def __repr__(self):
        return f"<Font {len(self.glyphs)} glyphs, line {self.line_height}, {self.width}x{self.height} {self.bpp}bpp>"


def read_font(data):
    data = bytes(data)
    need(data, 0, FONT_HEADER, fmt=FMT, what="font header")
    n, line, pix, width, height, mats, pal, bpp, unk16 = struct.unpack_from("<HHIHHIIHH", data, 0)
    if n == 0 or n > 0x1000:
        raise LimitError(f"font with {n} glyphs", fmt=FMT, offset=0)
    _check_bpp(bpp, width, 0x14)
    if width > MAX_DIM or height > MAX_DIM:
        raise LimitError(f"font bitmap {width}x{height}", fmt=FMT, offset=8)
    gend = need(data, FONT_HEADER, GLYPH_SIZE * n, fmt=FMT, what="glyph table")
    glyphs = [Glyph(*struct.unpack_from("<4B4bH", data, FONT_HEADER + GLYPH_SIZE * i)) for i in range(n)]
    if pix < gend or pix - gend > 3:
        raise ZprimError(f"bitmap at 0x{pix:x}, glyphs end at 0x{gend:x}", offset=4)
    need(data, pix, 0, fmt=FMT, what="font bitmap")
    size = width * height * bpp // 8
    if pal != pix + size:
        raise ZprimError(f"palette at 0x{pal:x}, expected 0x{pix + size:x}", offset=0x10)
    need(data, pix, size, fmt=FMT, what="font bitmap")
    palette = _read_palette(data, pal, len(data), "font")
    return Font(glyphs, width, height, data[pix:pix + size], palette, line, bpp, mats, unk16, data[gend:pix])


def write_font(font):
    n = len(font.glyphs)
    if n == 0 or n > 0x1000:
        raise ValueError(f"{n} glyphs")
    if font.bpp not in (4, 8) or font.width * font.bpp % 8:
        raise ValueError(f"bitmap width {font.width} at {font.bpp} bpp")
    if len(font.pixels) != font.width * font.height * font.bpp // 8:
        raise ValueError(f"{len(font.pixels)} bitmap bytes for {font.width}x{font.height}")
    gend = FONT_HEADER + GLYPH_SIZE * n
    padlen = -gend % 4
    pad = font.pad if len(font.pad) == padlen else b"\xcd" * padlen   # MSVC fill, as shipped
    pix = gend + padlen
    head = struct.pack("<HHIHHIIHH", n, font.line_height, pix, font.width, font.height, font.materials,
                       pix + len(font.pixels), font.bpp, font.unk_16)
    return head + b"".join(g.to_bytes() for g in font.glyphs) + pad + font.pixels + font.palette.to_bytes()


# ------------------------------------------------------------------ PNG

def _png_palette(colours, bpp):
    """PLTE + tRNS for the first 2**bpp colours (the ones zprimMakeMaterial puts in the material)."""
    rgba = [rgb_of(c) for c in colours[:1 << bpp]]
    if not rgba:
        rgba = [(0, 0, 0, 255)]
    trns = bytes(c[3] for c in rgba)
    trns = trns.rstrip(b"\xff")
    return [c[:3] for c in rgba], (trns or None)


def indices_png(rows, width, height, bpp, colours):
    """Palettised PNG (bit depth = bpp) of index rows; PNG packs the left pixel in the high bits."""
    pal, trns = _png_palette(colours, bpp)
    hi = max((max(r) for r in rows if r), default=0)
    pal += [(0, 0, 0)] * max(0, hi + 1 - len(pal))
    packed = []
    for r in rows:
        if bpp == 8:
            packed.append(bytes(r))
        else:
            packed.append(bytes(((r[x] & 0xF) << 4) | (r[x + 1] & 0xF) for x in range(0, width, 2)))
    return imaging.png_encode(width, height, packed, color_type=3, bit_depth=bpp, palette=pal, trns=trns)


def frame_png(img, k=0):
    f = img.frames[k]
    return indices_png(f.indices(img.bpp), f.width, f.height, img.bpp, img.palette.colours)


def font_png(font):
    rows = unpack_indices(font.pixels, font.width, font.height, font.bpp)
    return indices_png(rows, font.width, font.height, font.bpp, font.palette.colours)


def _merge_palette(colours, png_pal, png_trns, n, reference=None):
    """New 15-bit colours for the first n entries from a PNG palette. An entry whose 8-bit colour
    and alpha equal the expansion of ``reference[i]`` (the colours the PNG was exported with;
    default ``colours``) is unchanged and keeps ``colours[i]`` exactly."""
    reference = colours if reference is None else reference
    out = list(colours)
    for i, rgb in enumerate(png_pal[:n]):
        a = png_trns[i] if png_trns and i < len(png_trns) else 255
        new = (rgb[0], rgb[1], rgb[2], 0 if a < 128 else 255)
        if i < len(reference) and rgb_of(reference[i]) == new:
            continue
        c = colour_of(*new)
        if i < len(out):
            out[i] = c
        else:
            out.append(c)
    return out


def png_indices(png, colours, bpp, warnings=None, reference=None):
    """Decode an edited PNG to (index rows, new colours, width, height).

    * palettised PNG: the indices are used as they are (each must be < 2**bpp) and its palette
      replaces the first 2**bpp colours (an unchanged colour keeps its exact 15-bit value);
    * any other PNG (RGB / RGBA / grey): every pixel is mapped to the nearest colour of the current
      palette (alpha < 128 -> the transparent 0x7C1F entry)."""
    img = imaging.png_decode(png)
    w, h, ct, bd = img["width"], img["height"], img["color_type"], img["bit_depth"]
    if ct == 3:
        if bd > 8:
            raise ZprimError(f"palette PNG of depth {bd}")
        per = 8 // bd
        mask = (1 << bd) - 1
        rows = []
        for r in img["rows"]:
            row = [(r[x // per] >> (8 - bd * (x % per + 1))) & mask for x in range(w)]
            if any(v >= 1 << bpp for v in row):
                raise ZprimError(f"PNG uses palette index {max(row)}, the asset is {bpp}-bit "
                                 f"(at most {(1 << bpp) - 1})")
            rows.append(row)
        new = _merge_palette(colours, img["palette"], img["trns"], 1 << bpp, reference)
        return rows, new, w, h
    rgba = imaging.png_to_rgba_rows(img)
    n = min(len(colours), 1 << bpp)
    pal = [rgb_of(c) for c in colours[:n]]
    clear = [i for i in range(n) if pal[i][3] == 0]
    opaque = [i for i in range(n) if pal[i][3] != 0]
    if not opaque:
        raise ZprimError("the palette has no opaque colour to map to")
    cache = {}
    rows = []
    lost = 0
    for r in rgba:
        row = []
        for x in range(w):
            p = (r[4 * x], r[4 * x + 1], r[4 * x + 2], r[4 * x + 3])
            if p[3] < 128:
                if not clear:
                    raise ZprimError("transparent pixels but the palette has no 0x7C1F entry")
                row.append(clear[0])
                continue
            i = cache.get(p[:3])
            if i is None:
                i = min(opaque, key=lambda j: (pal[j][0] - p[0]) ** 2 + (pal[j][1] - p[1]) ** 2 + (pal[j][2] - p[2]) ** 2)
                cache[p[:3]] = i
                if pal[i][:3] != tuple(c & 0xF8 for c in p[:3]):
                    lost += 1
            row.append(i)
        rows.append(row)
    if lost and warnings is not None:
        warnings.append(f"{lost} colour(s) not in the palette were mapped to the nearest entry")
    return rows, list(colours), w, h


def image_from_png(img, pngs, warnings=None, reference=None):
    """A copy of ``img`` with its frames taken from ``pngs`` (one PNG per frame, None = unchanged).
    Frame sizes may change (the offsets are recomputed); frame 0's size becomes the header size.
    Palette edits come from frame 0's PNG (all frames share the palette)."""
    colours = list(img.palette.colours)
    ref = list(img.palette.colours) if reference is None else list(reference)
    frames = []
    for k, f in enumerate(img.frames):
        png = pngs[k] if k < len(pngs) else None
        if png is None:
            frames.append(Frame(f.width, f.height, f.pixels, f.hot_x, f.hot_y, f.material))
            continue
        rows, new, w, h = png_indices(png, colours, img.bpp, warnings, ref)
        if k == 0:
            colours = new
        elif new != colours and warnings is not None:
            warnings.append(f"frame {k}: palette changes ignored (frame 0's palette is used)")
        if w * img.bpp % 8:
            raise ZprimError(f"frame {k}: width {w} must be even for a 4-bit image")
        if (w, h) != (f.width, f.height) and warnings is not None:
            warnings.append(f"frame {k}: size {f.width}x{f.height} -> {w}x{h}")
        frames.append(Frame(w, h, pack_indices(rows, w, img.bpp), f.hot_x, f.hot_y, f.material))
    pal = Palette(colours, img.palette.unk, img.palette.tail)
    if len(pal.to_bytes()) > PALETTE_BLOCK and len(img.palette.to_bytes()) <= PALETTE_BLOCK:
        raise ZprimError("palette grew past its 0x204-byte block")
    w0, h0 = (frames[0].width, frames[0].height)
    keep = (img.frames[0].width, img.frames[0].height) == (img.width, img.height)
    return Image(frames, pal, img.bpp, img.flags, w0 if keep else img.width, h0 if keep else img.height, img.pad_12)


def font_from_png(font, png, warnings=None, reference=None):
    """A copy of ``font`` with its bitmap (and bank-0 colours) taken from ``png``."""
    rows, colours, w, h = png_indices(png, font.palette.colours, font.bpp, warnings, reference)
    if w * font.bpp % 8:
        raise ZprimError(f"font bitmap width {w} must be even")
    if w > 255 or h > 255:
        if warnings is not None:
            warnings.append(f"bitmap {w}x{h}: glyph x / y are bytes, so only the top-left 256x256 is reachable")
    pal = Palette(colours, font.palette.unk, font.palette.tail)
    return Font(font.glyphs, w, h, pack_indices(rows, w, font.bpp), pal, font.line_height, font.bpp,
                font.materials, font.unk_16, font.pad)

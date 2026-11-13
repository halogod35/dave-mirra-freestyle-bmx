"""Minimal BMP <-> PNG conversion in pure Python (zlib + struct), for glTF texture embedding.

glTF only allows PNG/JPEG images, but ZMD ``TBMP`` textures are Windows ``.bmp`` files
(24-bit, and 4/8-bit palettised in the corpus). This module converts them losslessly:

* :func:`bmp_to_png` keeps palettised images palettised (same indices, same bit depth),
  and writes 24/32-bit images as RGB/RGBA. Rows are stored with filter 0 (no prediction).
* :func:`png_decode` decodes any non-interlaced PNG (all 5 filter types, bit depths 1-16,
  all colour types). Enough for re-importing edited textures (DM-021).
* :func:`png_to_bmp` rebuilds a BMP from a PNG plus the original BMP header bytes
  (and any trailing bytes), so an unedited texture round-trips byte-for-byte.

All parsers raise :class:`ImageError` (a :class:`~dmbmx.core.errors.FormatError`) on bad input.
"""
from __future__ import annotations

import struct
import zlib

from dmbmx.core.errors import FormatError

__all__ = ["ImageError", "BmpInfo", "bmp_info", "bmp_to_png", "png_encode", "png_decode", "png_info",
           "png_to_rgba_rows", "quantize_rows", "bmp_header_resized",
           "png_has_alpha", "png_to_bmp", "PNG_SIG"]

PNG_SIG = b"\x89PNG\r\n\x1a\n"
MAX_DIM = 16384


class ImageError(FormatError):
    def __init__(self, msg, *, fmt="IMG", offset=None):
        super().__init__(msg, fmt=fmt, offset=offset)


# ------------------------------------------------------------------------------- BMP

class BmpInfo:
    """Parsed BMP header. ``header`` = bytes ``[0:off_bits]`` (file header, info header, palette)."""
    __slots__ = ("width", "height", "top_down", "bpp", "off_bits", "palette", "row_bytes", "pixel_end",
                 "header", "tail")

    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


def bmp_info(data: bytes) -> BmpInfo:
    """Parse and bounds-check an uncompressed BMP (BI_RGB, 1/4/8/24/32 bpp)."""
    if len(data) < 54 or data[:2] != b"BM":
        raise ImageError("not a BMP (no 'BM' magic or < 54 bytes)", fmt="BMP", offset=0)
    off_bits = struct.unpack_from("<I", data, 10)[0]
    hs = struct.unpack_from("<I", data, 14)[0]
    if hs < 40 or 14 + hs > len(data):
        raise ImageError(f"unsupported info header size {hs}", fmt="BMP", offset=14)
    w, h, planes, bpp, comp = struct.unpack_from("<iiHHI", data, 18)
    clr_used = struct.unpack_from("<I", data, 46)[0]
    if comp != 0:
        raise ImageError(f"compressed BMP (biCompression={comp}) not supported", fmt="BMP", offset=30)
    if bpp not in (1, 4, 8, 24, 32):
        raise ImageError(f"unsupported bit depth {bpp}", fmt="BMP", offset=28)
    if not (0 < w <= MAX_DIM) or not (0 < abs(h) <= MAX_DIM):
        raise ImageError(f"bad dimensions {w}x{h}", fmt="BMP", offset=18)
    palette = None
    if bpp <= 8:
        n = clr_used or (1 << bpp)
        if n > (1 << bpp):
            raise ImageError(f"palette of {n} entries for {bpp}-bit image", fmt="BMP", offset=46)
        p0 = 14 + hs
        if p0 + 4 * n > off_bits or off_bits > len(data):
            raise ImageError("palette runs past the pixel data", fmt="BMP", offset=p0)
        palette = [(data[p0 + 4 * i + 2], data[p0 + 4 * i + 1], data[p0 + 4 * i]) for i in range(n)]
    row_bytes = ((w * bpp + 31) // 32) * 4
    end = off_bits + row_bytes * abs(h)
    if off_bits < 14 + hs or end > len(data):
        raise ImageError(f"pixel data [{off_bits}:{end}] past end of file ({len(data)})", fmt="BMP",
                         offset=10)
    return BmpInfo(width=w, height=abs(h), top_down=h < 0, bpp=bpp, off_bits=off_bits, palette=palette,
                   row_bytes=row_bytes, pixel_end=end, header=bytes(data[:off_bits]), tail=bytes(data[end:]))


def _bmp_rows(data, info):
    """Pixel rows top-to-bottom, each the exact packed bytes (no padding)."""
    used = (info.width * info.bpp + 7) // 8
    rows = [data[info.off_bits + y * info.row_bytes: info.off_bits + y * info.row_bytes + used]
            for y in range(info.height)]
    return rows if info.top_down else rows[::-1]


def bmp_to_png(data: bytes) -> bytes:
    """Convert a BMP file to PNG bytes (lossless; palettised stays palettised)."""
    info = bmp_info(data)
    rows = _bmp_rows(data, info)
    if info.bpp <= 8:
        pal = list(info.palette)
        # PNG decoders reject indices beyond the palette: pad it to the largest index used.
        hi = _max_index(rows, info.bpp, info.width)
        pal += [(0, 0, 0)] * max(0, hi + 1 - len(pal))
        return png_encode(info.width, info.height, rows, color_type=3, bit_depth=info.bpp, palette=pal)
    if info.bpp == 24:
        out = []
        for r in rows:
            b = bytearray(r)
            b[0::3], b[2::3] = r[2::3], r[0::3]
            out.append(bytes(b))
        return png_encode(info.width, info.height, out, color_type=2, bit_depth=8)
    # 32-bit BGRX/BGRA -> RGBA (an all-zero alpha channel is treated as opaque)
    alpha_used = any(r[3::4].strip(b"\0") for r in rows)
    out = []
    for r in rows:
        b = bytearray(r)
        b[0::4], b[2::4] = r[2::4], r[0::4]
        if not alpha_used:
            b[3::4] = b"\xff" * info.width
        out.append(bytes(b))
    return png_encode(info.width, info.height, out, color_type=6, bit_depth=8)


def _max_index(rows, bpp, width):
    if bpp == 8:
        return max((max(r) for r in rows if r), default=0)
    per = 8 // bpp
    mask = (1 << bpp) - 1
    hi = 0
    for r in rows:
        for x in range(width):
            v = (r[x // per] >> (8 - bpp * (x % per + 1))) & mask
            if v > hi:
                hi = v
    return hi


# ------------------------------------------------------------------------------- PNG

def _png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def png_encode(width, height, rows, *, color_type, bit_depth=8, palette=None, trns=None, level=9) -> bytes:
    """Encode packed scanlines (top to bottom, no filter byte) as a PNG (filter 0 on every row)."""
    if len(rows) != height:
        raise ValueError(f"{len(rows)} rows for height {height}")
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    out = [PNG_SIG, _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0))]
    if palette is not None:
        out.append(_png_chunk(b"PLTE", b"".join(bytes(c[:3]) for c in palette)))
    if trns is not None:
        out.append(_png_chunk(b"tRNS", bytes(trns)))
    out.append(_png_chunk(b"IDAT", zlib.compress(raw, level)))
    out.append(_png_chunk(b"IEND", b""))
    return b"".join(out)


_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _png_chunks(data):
    if data[:8] != PNG_SIG:
        raise ImageError("not a PNG (bad signature)", fmt="PNG", offset=0)
    off = 8
    while off < len(data):
        if off + 12 > len(data):
            raise ImageError("truncated chunk header", fmt="PNG", offset=off)
        n = struct.unpack_from(">I", data, off)[0]
        tag = data[off + 4:off + 8]
        if n > len(data) - off - 12:
            raise ImageError(f"chunk {tag!r} length {n} past end", fmt="PNG", offset=off)
        body = data[off + 8:off + 8 + n]
        crc = struct.unpack_from(">I", data, off + 8 + n)[0]
        if zlib.crc32(tag + body) & 0xFFFFFFFF != crc:
            raise ImageError(f"chunk {tag!r} CRC mismatch", fmt="PNG", offset=off)
        yield tag, body, off
        off += 12 + n
        if tag == b"IEND":
            return
    raise ImageError("no IEND chunk", fmt="PNG", offset=off)


def png_info(data: bytes) -> dict:
    """IHDR fields + PLTE/tRNS presence, without decompressing the pixels."""
    for tag, body, off in _png_chunks(data):
        if tag != b"IHDR" or len(body) != 13:
            raise ImageError("first chunk is not a 13-byte IHDR", fmt="PNG", offset=off)
        w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", body)
        return {"width": w, "height": h, "bit_depth": bd, "color_type": ct, "interlace": inter}
    raise ImageError("empty PNG", fmt="PNG", offset=8)


def png_decode(data: bytes) -> dict:
    """Decode a non-interlaced PNG.

    Returns ``{width, height, bit_depth, color_type, palette, trns, rows}`` where ``rows``
    are the unfiltered packed scanlines (top to bottom, no filter byte).
    """
    hdr = None
    palette = trns = None
    idat = []
    for tag, body, off in _png_chunks(data):
        if hdr is None:
            if tag != b"IHDR" or len(body) != 13:
                raise ImageError("first chunk is not a 13-byte IHDR", fmt="PNG", offset=off)
            hdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            if len(body) % 3 or not body:
                raise ImageError("bad PLTE length", fmt="PNG", offset=off)
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif tag == b"tRNS":
            trns = bytes(body)
        elif tag == b"IDAT":
            idat.append(body)
    w, h, bd, ct, comp, filt, inter = hdr
    if ct not in _CHANNELS or bd not in (1, 2, 4, 8, 16) or comp or filt:
        raise ImageError(f"unsupported IHDR (colour type {ct}, depth {bd})", fmt="PNG", offset=16)
    if inter:
        raise ImageError("interlaced PNG not supported", fmt="PNG", offset=28)
    if not (0 < w <= MAX_DIM and 0 < h <= MAX_DIM):
        raise ImageError(f"bad dimensions {w}x{h}", fmt="PNG", offset=16)
    if ct == 3 and palette is None:
        raise ImageError("palette image without PLTE", fmt="PNG", offset=8)
    bpp_bits = _CHANNELS[ct] * bd
    stride = (w * bpp_bits + 7) // 8
    bpp = max(1, bpp_bits // 8)
    d = zlib.decompressobj()
    try:
        raw = d.decompress(b"".join(idat), (stride + 1) * h + 1)
    except zlib.error as e:
        raise ImageError(f"IDAT: {e}", fmt="PNG") from e
    if len(raw) < (stride + 1) * h:
        raise ImageError(f"IDAT: {len(raw)} bytes, need {(stride + 1) * h}", fmt="PNG")
    rows, prev = [], bytes(stride)
    for y in range(h):
        p = y * (stride + 1)
        ft = raw[p]
        cur = bytearray(raw[p + 1:p + 1 + stride])
        if ft == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        elif ft != 0:
            raise ImageError(f"row {y}: bad filter type {ft}", fmt="PNG")
        rows.append(bytes(cur))
        prev = cur
    return {"width": w, "height": h, "bit_depth": bd, "color_type": ct, "palette": palette, "trns": trns,
            "rows": rows}


def png_has_alpha(data: bytes) -> str | None:
    """'blend' if some pixel is partially transparent, 'mask' if alpha is only 0/255
    (with at least one 0), None if the image is opaque."""
    img = png_decode(data)
    ct, bd = img["color_type"], img["bit_depth"]
    vals = set()
    if ct in (4, 6) and bd == 8:
        n = _CHANNELS[ct]
        for r in img["rows"]:
            vals.update(r[n - 1::n])
    elif ct == 3 and img["trns"]:
        vals = set(img["trns"]) | {255}
    elif ct in (4, 6):  # 16-bit: use the high byte
        n = _CHANNELS[ct] * 2
        for r in img["rows"]:
            vals.update(r[n - 2::n])
    if not vals or vals == {255}:
        return None
    return "mask" if vals <= {0, 255} else "blend"


def png_to_bmp(png: bytes, header: bytes, tail: bytes = b"") -> bytes:
    """Rebuild a BMP from a PNG using the original BMP ``header`` (bytes before the pixels,
    i.e. :attr:`BmpInfo.header`) and ``tail`` (bytes after them). The PNG must have the same
    size and a compatible pixel format (palette PNG of the same depth for palettised BMPs,
    RGB/RGBA 8-bit for 24/32-bit). Row padding is written as zeros.
    """
    img = png_decode(png)
    hd = header + bytes(max(0, 54 - len(header)))
    w, h, _, bpp = struct.unpack_from("<iiHH", hd, 18)
    if (img["width"], img["height"]) != (w, abs(h)):
        raise ImageError(f"PNG is {img['width']}x{img['height']}, BMP header says {w}x{abs(h)}", fmt="BMP")
    row_bytes = ((w * bpp + 31) // 32) * 4
    ct, bd = img["color_type"], img["bit_depth"]
    out_rows = []
    if bpp <= 8:
        if ct != 3 or bd != bpp:
            raise ImageError(f"{bpp}-bit BMP needs a {bpp}-bit palette PNG (got type {ct}, depth {bd})",
                             fmt="BMP")
        out_rows = list(img["rows"])
    elif bpp in (24, 32):
        if ct not in (2, 6) or bd != 8:
            raise ImageError(f"{bpp}-bit BMP needs an 8-bit RGB/RGBA PNG (got type {ct}, depth {bd})",
                             fmt="BMP")
        n = _CHANNELS[ct]
        for r in img["rows"]:
            b = bytearray(w * (bpp // 8))
            k = bpp // 8
            b[0::k], b[1::k], b[2::k] = r[2::n], r[1::n], r[0::n]
            if k == 4:
                b[3::4] = r[3::n] if n == 4 else b"\xff" * w
            out_rows.append(bytes(b))
    else:
        raise ImageError(f"unsupported BMP depth {bpp}", fmt="BMP")
    if h > 0:
        out_rows.reverse()
    pad = bytes(row_bytes)
    pix = b"".join(r + pad[:row_bytes - len(r)] for r in out_rows)
    return header + pix + tail


# ------------------------------------------------------------------------------- editing helpers (DM-021)

def png_to_rgba_rows(img: dict) -> list:
    """Rows of 8-bit RGBA bytes from a :func:`png_decode` result (any colour type / bit depth)."""
    w, ct, bd = img["width"], img["color_type"], img["bit_depth"]
    pal, trns = img["palette"], img["trns"]
    out = []
    for r in img["rows"]:
        if bd < 8:
            per = 8 // bd
            mask = (1 << bd) - 1
            s = [(r[x // per] >> (8 - bd * (x % per + 1))) & mask for x in range(w)]
        elif bd == 16:
            s = list(r[0::2])
        else:
            s = list(r)
        px = bytearray()
        if ct == 3:
            for i in s[:w]:
                if i >= len(pal):
                    raise ImageError(f"palette index {i} >= {len(pal)} entries", fmt="PNG")
                a = trns[i] if trns and i < len(trns) else 255
                px += bytes(pal[i]) + bytes((a,))
        elif ct == 0:
            scale = 255 // ((1 << bd) - 1) if bd < 8 else 1
            key = struct.unpack(">H", trns[:2])[0] if trns and len(trns) >= 2 else None
            for v in s[:w]:
                g = v * scale
                px += bytes((g, g, g, 0 if key is not None and v == (key >> 8 if bd == 16 else key) else 255))
        elif ct == 4:
            for k in range(w):
                px += bytes((s[2 * k], s[2 * k], s[2 * k], s[2 * k + 1]))
        elif ct == 2:
            for k in range(w):
                px += bytes(s[3 * k:3 * k + 3]) + b"\xff"
        elif ct == 6:
            px = bytearray(s[:4 * w])
        out.append(bytes(px))
    return out


def quantize_rows(rgba_rows, width, palette, bpp):
    """Map RGBA rows to the nearest palette entry (squared RGB distance); packed ``bpp``-bit rows."""
    cache = {}
    per = 8 // bpp
    out = []
    for r in rgba_rows:
        idx = []
        for k in range(width):
            c = (r[4 * k], r[4 * k + 1], r[4 * k + 2])
            i = cache.get(c)
            if i is None:
                i = min(range(len(palette)),
                        key=lambda j: (palette[j][0] - c[0]) ** 2 + (palette[j][1] - c[1]) ** 2 + (palette[j][2] - c[2]) ** 2)
                cache[c] = i
            idx.append(i)
        if bpp == 8:
            out.append(bytes(idx))
        else:
            b = bytearray((width + per - 1) // per)
            for x, v in enumerate(idx):
                b[x // per] |= v << (8 - bpp * (x % per + 1))
            out.append(bytes(b))
    return out


def bmp_header_resized(header: bytes, width: int, height: int) -> bytes:
    """Copy of a BMP header (file + info header + palette) with new dimensions and sizes."""
    h = bytearray(header)
    old_h = struct.unpack_from("<i", h, 22)[0]
    bpp = struct.unpack_from("<H", h, 28)[0]
    row = ((width * bpp + 31) // 32) * 4
    off = struct.unpack_from("<I", h, 10)[0]
    struct.pack_into("<I", h, 2, off + row * height)
    struct.pack_into("<ii", h, 18, width, -height if old_h < 0 else height)
    struct.pack_into("<I", h, 34, row * height)
    return bytes(h)

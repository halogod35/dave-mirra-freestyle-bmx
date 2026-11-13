"""Texture images: BMP/PNG inspection, pure-Python decode to RGBA, PNG/BMP encode,
and the constraints the game's loader puts on replacement images. Spec: docs/formats/TEXTURE.md.

Pure stdlib (via dmbmx.convert.imaging) so the Blender add-on can vendor it.
"""
from __future__ import annotations

import struct

from dmbmx.convert import imaging
from dmbmx.core.errors import FormatError, LimitError

__all__ = [
    "TextureError", "Image", "sniff", "bmp_info", "png_info", "info", "decode", "decode_bmp",
    "decode_png", "encode_png", "encode_bmp", "bmp_to_png", "apply_color_key", "resize_nearest", "check", "prepare_replacement",
    "COLOR_KEY", "MAX_DIM",
]

COLOR_KEY = (255, 0, 255)     # [V] 0x00498661: pixels/palette entries of exactly this RGB become transparent
MAX_DIM = 4096                # sanity cap for decoding (largest shipped texture is 512x512)
PNG_SIG = b"\x89PNG\r\n\x1a\n"


class TextureError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="IMAGE", offset=offset)


class Image:
    """Decoded image: ``width``, ``height``, ``rgba`` (top-down rows, 4 bytes per pixel)."""
    __slots__ = ("width", "height", "rgba")

    def __init__(self, width, height, rgba):
        if len(rgba) != width * height * 4:
            raise ValueError("rgba length does not match the size")
        self.width, self.height, self.rgba = width, height, bytes(rgba)

    @property
    def has_alpha(self):
        return any(a != 255 for a in self.rgba[3::4])

    def __repr__(self):
        return f"Image({self.width}x{self.height})"


def sniff(data):
    if data[:8] == PNG_SIG:
        return "png"
    if data[:2] == b"BM":
        return "bmp"
    if data[:4] == b"DDS ":
        return "dds"
    return None


# ----------------------------------------------------------------------------------- info
# Parsing, PNG (un)filtering and PNG writing are dmbmx.convert.imaging's; this module only
# adds RGBA expansion, BMP writing, the colour key and the game's constraints.

def _check_dims(w, h):
    if w <= 0 or h <= 0:
        raise TextureError(f"bad image size {w}x{h}")
    if w > MAX_DIM or h > MAX_DIM:
        raise LimitError(f"image {w}x{h} exceeds {MAX_DIM}x{MAX_DIM}", fmt="IMAGE")


def bmp_info(data):
    """BMP header as a dict (raises a FormatError for compressed / unsupported BMPs)."""
    i = imaging.bmp_info(data)
    return {"kind": "bmp", "width": i.width, "height": i.height, "top_down": i.top_down, "bpp": i.bpp,
            "compression": 0, "colors_used": len(i.palette) if i.palette else 0,
            "header_size": struct.unpack_from("<I", data, 14)[0], "data_offset": i.off_bits}


def png_info(data):
    d = dict(imaging.png_info(data))
    d["kind"] = "png"
    return d


def info(data):
    k = sniff(data)
    if k == "bmp":
        return bmp_info(data)
    if k == "png":
        return png_info(data)
    raise TextureError("unknown image type (expected BMP or PNG)", offset=0)


# --------------------------------------------------------------------------------- decode

def _unpack(row, bits, w):
    if bits == 8:
        return row[:w]
    per = 8 // bits
    mask = (1 << bits) - 1
    return [(row[x // per] >> (8 - bits * (x % per + 1))) & mask for x in range(w)]


def decode_bmp(data):
    """Uncompressed (BI_RGB) 1/4/8/24/32-bit BMP -> :class:`Image` (palette alpha ignored)."""
    i = imaging.bmp_info(data)
    w, h, bpp = i.width, i.height, i.bpp
    _check_dims(w, h)
    pal = [bytes(c) + b"\xff" for c in (i.palette or [])]
    out = bytearray()
    for row in range(h):
        y = row if i.top_down else h - 1 - row
        r = data[i.off_bits + y * i.row_bytes: i.off_bits + (y + 1) * i.row_bytes]
        if bpp in (24, 32):
            k = bpp // 8
            px = bytearray(w * 4)
            px[0::4], px[1::4], px[2::4] = r[2:w * k:k], r[1:w * k:k], r[0:w * k:k]
            px[3::4] = b"\xff" * w
            out += px
        else:
            for v in _unpack(r, bpp, w):
                out += pal[v] if v < len(pal) else b"\x00\x00\x00\xff"
    return Image(w, h, out)


def decode_png(data):
    """Non-interlaced PNG of any colour type / bit depth -> :class:`Image` (16-bit: top byte).
    CRCs are always checked (imaging.png_decode)."""
    d = imaging.png_decode(data)
    w, h, bd, ct = d["width"], d["height"], d["bit_depth"], d["color_type"]
    _check_dims(w, h)
    trns = d["trns"] or b""
    out = bytearray()
    if ct == 3:
        pal = [bytes(c) + bytes((trns[k] if k < len(trns) else 255,)) for k, c in enumerate(d["palette"])]
        for r in d["rows"]:
            for v in _unpack(r, bd, w):
                out += pal[v] if v < len(pal) else b"\x00\x00\x00\xff"
        return Image(w, h, out)
    if bd < 8:   # grey 1/2/4
        mask = (1 << bd) - 1
        for r in d["rows"]:
            for v in _unpack(r, bd, w):
                g = v * 255 // mask
                out += bytes((g, g, g, 255))
        return Image(w, h, out)
    chans = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    step = bd // 8
    for r in d["rows"]:
        px = r[0::step] if step > 1 else r       # 16-bit: keep the high byte
        for x in range(w):
            c = px[x * chans:(x + 1) * chans]
            if ct == 0:
                out += bytes((c[0], c[0], c[0], 255))
            elif ct == 2:
                out += bytes((c[0], c[1], c[2], 255))
            elif ct == 4:
                out += bytes((c[0], c[0], c[0], c[1]))
            else:
                out += bytes(c)
    return Image(w, h, out)


def decode(data):
    k = sniff(data)
    if k == "bmp":
        return decode_bmp(data)
    if k == "png":
        return decode_png(data)
    raise TextureError("unknown image type (expected BMP or PNG)", offset=0)


# --------------------------------------------------------------------------------- encode

def encode_png(img, alpha=None):
    """8-bit RGBA (colour type 6) or RGB (2) PNG with valid CRCs (imaging.png_encode).
    ``alpha=None``: RGBA only if some pixel is not opaque."""
    if alpha is None:
        alpha = img.has_alpha
    w, src = img.width, img.rgba
    rows = []
    for y in range(img.height):
        r = src[y * w * 4:(y + 1) * w * 4]
        if not alpha:
            rgb = bytearray(w * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = r[0::4], r[1::4], r[2::4]
            r = bytes(rgb)
        rows.append(r)
    return imaging.png_encode(w, img.height, rows, color_type=6 if alpha else 2)


def bmp_to_png(data):
    """Lossless BMP -> PNG for export (palettised stays palettised): imaging.bmp_to_png."""
    return imaging.bmp_to_png(data)


def encode_bmp(img, key=COLOR_KEY):
    """24-bit uncompressed bottom-up BMP. Pixels with alpha < 128 become the colour key (the game
    turns that RGB transparent)."""
    w, h, src = img.width, img.height, img.rgba
    stride = (w * 3 + 3) & ~3
    pix = bytearray()
    kb = bytes((key[2], key[1], key[0]))
    for y in range(h - 1, -1, -1):
        r = src[y * w * 4:(y + 1) * w * 4]
        row = bytearray(w * 3)
        row[0::3], row[1::3], row[2::3] = r[2::4], r[1::4], r[0::4]
        for x in range(w):
            if r[4 * x + 3] < 128:
                row[3 * x:3 * x + 3] = kb
        pix += row + bytes(stride - w * 3)
    off = 54
    hdr = b"BM" + struct.pack("<IHHI", off + len(pix), 0, 0, off)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pix), 2834, 2834, 0, 0)
    return hdr + bytes(pix)


def resize_nearest(img, w, h):
    _check_dims(w, h)
    sw, sh, src = img.width, img.height, img.rgba
    out = bytearray()
    for y in range(h):
        sy = y * sh // h
        for x in range(w):
            sx = x * sw // w
            o = (sy * sw + sx) * 4
            out += src[o:o + 4]
    return Image(w, h, out)


def apply_color_key(img, key=COLOR_KEY):
    """What the game does to BMP textures: exact key-colour pixels become RGBA 0,0,0,0."""
    b = bytearray(img.rgba)
    k = bytes(key)
    for o in range(0, len(b), 4):
        if b[o:o + 3] == k:
            b[o:o + 4] = b"\0\0\0\0"
    return Image(img.width, img.height, b)


# ---------------------------------------------------------------------------- constraints

def _pow2(n):
    return n > 0 and n & (n - 1) == 0


def check(data, kind=None, max_dim=512):
    """Constraints for an image the game will load. Returns ``(errors, warnings)`` (lists of str).

    errors   = the loader would reject it or we can't verify it (wrong type, compressed BMP,
               unsupported depth/colour type, interlaced PNG, non-power-of-two size)
    warnings = allowed but outside what the shipped files use (e.g. > ``max_dim``)
    """
    errors, warnings = [], []
    k = sniff(data)
    if k not in ("bmp", "png"):
        return ["not a BMP or PNG file"], warnings
    if kind and k != kind:
        errors.append(f"slot holds a {kind.upper()}, image is a {k.upper()} (convert it)")
    try:
        i = info(data)
    except FormatError as e:
        return [str(e)], warnings
    w, h = i["width"], i["height"]
    if not (_pow2(w) and _pow2(h)):
        warnings.append(f"size {w}x{h} is not a power of two (allowed: 2 level textures (80x80, 96x96) "
                        "and 14 material.zal BMPs ship like that, but some D3D7 cards need powers of two)")
    if max(w, h) > max_dim:
        warnings.append(f"size {w}x{h} is larger than the largest shipped texture (512x512)")
    if w != h:
        warnings.append(f"non-square {w}x{h} (all ZMD/Mat._sd textures are square; material.zal has non-square)")
    if k == "bmp":
        if i["compression"] != 0:
            errors.append("compressed BMP (RLE/bitfields); save as uncompressed")
        if i["bpp"] not in (4, 8, 24):
            errors.append(f"{i['bpp']}-bit BMP; shipped textures use 4, 8 or 24 bits")
        if i["top_down"]:
            warnings.append("top-down BMP (negative height); shipped BMPs are bottom-up")
    else:
        if i["interlace"]:
            errors.append("interlaced PNG; save without interlacing")
        if i["bit_depth"] != 8 or i["color_type"] not in (2, 3, 6):
            warnings.append(f"PNG colour type {i['color_type']} depth {i['bit_depth']}; shipped PNGs are "
                            "8-bit RGBA (type 6)")
    if not errors:
        try:
            decode(data)
        except FormatError as e:
            errors.append(f"does not decode: {e}")
    return errors, warnings


def prepare_replacement(data, kind, size=None, resize=False):
    """Make ``data`` fit a texture slot of type ``kind`` ('bmp'|'png').

    Returns ``(image_bytes, notes)``. A file that already passes :func:`check` for ``kind`` (and has
    ``size`` when given) is returned unchanged. Otherwise it is decoded and re-encoded: BMP slots get
    a 24-bit BMP (alpha < 128 -> colour key 255,0,255), PNG slots an 8-bit RGBA/RGB PNG; with
    ``resize=True`` it is scaled (nearest neighbour) to ``size`` (w, h). Raises TextureError if the
    result would still break a constraint.
    """
    notes = []
    errors, _ = check(data, kind)
    cur = info(data) if sniff(data) in ("bmp", "png") else None
    want = tuple(size) if size else None
    same_size = cur is not None and (want is None or (cur["width"], cur["height"]) == want)
    if not errors and same_size:
        return bytes(data), notes
    img = decode(data)
    if want and (img.width, img.height) != want:
        if not resize:
            raise TextureError(f"image is {img.width}x{img.height}, the slot is {want[0]}x{want[1]} "
                               "(use resize, or pass size=None to keep your size)")
        img = resize_nearest(img, *want)
        notes.append(f"resized to {want[0]}x{want[1]}")
    out = encode_bmp(img) if kind == "bmp" else encode_png(img)
    notes.append(f"re-encoded as {'24-bit BMP' if kind == 'bmp' else 'PNG'}")
    errors, _ = check(out, kind)
    if errors:
        raise TextureError("; ".join(errors))
    return out, notes

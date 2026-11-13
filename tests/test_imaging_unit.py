"""BMP -> PNG conversion, PNG decoder, PNG -> BMP rebuild (synthetic images + corpus)."""
import random
import struct
import zlib

import pytest
from conftest import corpus_files
from fuzz import fuzz_parser

from dmbmx.convert import imaging
from dmbmx.formats import zmd


def make_bmp(w, h, bpp, pixels_rows, palette=None, top_down=False, tail=b""):
    """pixels_rows: packed rows top->bottom (no padding)."""
    row_bytes = ((w * bpp + 31) // 32) * 4
    pal = b"".join(bytes((b, g, r, 0)) for r, g, b in (palette or []))
    off = 54 + len(pal)
    rows = pixels_rows if top_down else pixels_rows[::-1]
    pix = b"".join(r + bytes(row_bytes - len(r)) for r in rows)
    size = off + len(pix) + len(tail)
    hdr = b"BM" + struct.pack("<IHHI", size, 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, 0, len(pix), 2835, 2835,
                       len(palette or []), 0)
    return hdr + info + pal + pix + tail


def _rgb_rows(png):
    img = imaging.png_decode(png)
    return img, img["rows"]


@pytest.mark.parametrize("w,h,top_down", [(4, 3, False), (5, 2, True), (1, 1, False), (7, 5, False)])
def test_bmp24_to_png_pixels(w, h, top_down):
    rnd = random.Random(w * 31 + h)
    rows_rgb = [bytes(rnd.randrange(256) for _ in range(3 * w)) for _ in range(h)]
    rows_bgr = []
    for r in rows_rgb:
        b = bytearray(r)
        b[0::3], b[2::3] = r[2::3], r[0::3]
        rows_bgr.append(bytes(b))
    bmp = make_bmp(w, h, 24, rows_bgr, top_down=top_down, tail=b"\0\0")
    png = imaging.bmp_to_png(bmp)
    img, rows = _rgb_rows(png)
    assert (img["width"], img["height"], img["color_type"], img["bit_depth"]) == (w, h, 2, 8)
    assert rows == rows_rgb
    info = imaging.bmp_info(bmp)
    if not top_down:
        assert imaging.png_to_bmp(png, info.header, info.tail) == bmp


@pytest.mark.parametrize("bpp,w", [(8, 5), (4, 7), (4, 8), (1, 9)])
def test_palettised_bmp_keeps_indices(bpp, w):
    h = 3
    rnd = random.Random(bpp * 100 + w)
    npal = min(1 << bpp, 160)
    pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(npal)]
    used = (w * bpp + 7) // 8
    mask = (1 << bpp) - 1
    rows = []
    for _ in range(h):
        vals = [rnd.randrange(npal) & mask for _ in range(w)]
        b = bytearray(used)
        for x, v in enumerate(vals):
            b[x * bpp // 8] |= v << (8 - bpp - (x * bpp) % 8)
        rows.append(bytes(b))
    bmp = make_bmp(w, h, bpp, rows, palette=pal)
    png = imaging.bmp_to_png(bmp)
    img = imaging.png_decode(png)
    assert img["color_type"] == 3 and img["bit_depth"] == bpp
    assert img["rows"] == rows
    assert img["palette"][:npal] == pal
    info = imaging.bmp_info(bmp)
    assert imaging.png_to_bmp(png, info.header, info.tail) == bmp


def _png_with_filters(w, h, rows, bpp=4, ct=6):
    """Encode with each row using a different filter type (to test the decoder)."""
    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    out, prev = bytearray(), bytes(len(rows[0]))
    for y, r in enumerate(rows):
        ft = y % 5
        f = bytearray()
        for i, x in enumerate(r):
            a = r[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[ft]
            f.append((x - pred) & 0xFF)
        out += bytes([ft]) + f
        prev = r

    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (imaging.PNG_SIG + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
            + ch(b"IDAT", zlib.compress(bytes(out))) + ch(b"IEND", b""))


def test_png_decode_all_filters_and_alpha():
    rnd = random.Random(5)
    w, h = 6, 10
    rows = [bytes(rnd.randrange(256) for _ in range(4 * w)) for _ in range(h)]
    png = _png_with_filters(w, h, rows)
    assert imaging.png_decode(png)["rows"] == rows
    assert imaging.png_has_alpha(png) == "blend"
    opaque = [bytes(b if i % 4 != 3 else 255 for i, b in enumerate(r)) for r in rows]
    assert imaging.png_has_alpha(_png_with_filters(w, h, opaque)) is None
    cut = [bytes(b if i % 4 != 3 else (0 if b < 128 else 255) for i, b in enumerate(r)) for r in rows]
    assert imaging.png_has_alpha(_png_with_filters(w, h, cut)) == "mask"


def test_png_encode_roundtrip_rgba():
    rows = [bytes(range(16)), bytes(range(16, 32))]
    png = imaging.png_encode(4, 2, rows, color_type=6)
    assert imaging.png_decode(png)["rows"] == rows


@pytest.mark.parametrize("bad", [b"", b"BM", b"BM" + bytes(60), b"XX" + bytes(100)])
def test_bmp_rejects(bad):
    with pytest.raises(imaging.ImageError):
        imaging.bmp_to_png(bad)


def test_png_rejects_bad_crc():
    png = bytearray(imaging.png_encode(1, 1, [b"\0\0\0"], color_type=2))
    png[20] ^= 1
    with pytest.raises(imaging.ImageError):
        imaging.png_decode(bytes(png))


def test_png_to_bmp_size_mismatch():
    bmp = make_bmp(2, 2, 24, [bytes(6), bytes(6)])
    info = imaging.bmp_info(bmp)
    with pytest.raises(imaging.ImageError):
        imaging.png_to_bmp(imaging.png_encode(3, 2, [bytes(9)] * 2, color_type=2), info.header)


def _corpus_textures():
    out = []
    for f in corpus_files(".zmd"):
        m = zmd.read(f.read_bytes())
        out += [(f.name, t) for t in m.materials()]
    return out


def test_corpus_bmp_png_roundtrip():
    """Every TBMP converts to PNG and rebuilds byte-for-byte; every TPNG decodes."""
    tex = _corpus_textures()
    if not tex:
        pytest.skip("no corpus")
    nb = npng = 0
    for name, t in tex:
        if t.kind == "bmp":
            info = imaging.bmp_info(t.image)
            png = imaging.bmp_to_png(t.image)
            assert imaging.png_to_bmp(png, info.header, info.tail) == t.image, (name, t.name)
            nb += 1
        else:
            imaging.png_decode(t.image)
            npng += 1
    assert (nb, npng) == (215, 109)


def test_fuzz_bmp_and_png():
    bmp = make_bmp(8, 4, 8, [bytes(range(8))] * 4, palette=[(i, i, i) for i in range(8)])
    fuzz_parser(imaging.bmp_to_png, bmp, n=200, seed=0)
    fuzz_parser(imaging.png_decode, imaging.bmp_to_png(bmp), n=200, seed=0)
    tex = [t for _, t in _corpus_textures()[:40]]
    small_bmp = min((t.image for t in tex if t.kind == "bmp"), key=len, default=None)
    if small_bmp:
        fuzz_parser(imaging.bmp_to_png, small_bmp, n=200, seed=1, timeout=3.0)

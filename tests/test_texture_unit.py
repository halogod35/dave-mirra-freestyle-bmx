"""Unit tests on synthetic images (no game data) + fuzzing of the image decoders and MATI."""
import struct
import zlib

import pytest
from conftest import corpus_files
from fuzz import fuzz_parser

from dmbmx.core.chunks import parse_4cc
from dmbmx.core.errors import FormatError
from dmbmx.formats import texture as T
from dmbmx.formats import zmd
from dmbmx.formats.mati import Mati, MatSd


def _img(w, h):
    return T.Image(w, h, b"".join(bytes(((x * 37 + y * 11) & 255, (x * 5) & 255, (y * 9) & 255, 255))
                                  for y in range(h) for x in range(w)))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def _bmp_pal(w, h, bpp, idx, pal):
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            v = idx[y * w + x]
            per = 8 // bpp
            row[x // per] |= v << (8 - bpp * (x % per + 1))
        rows.append(bytes(row))
    pix = b"".join(rows)
    palb = b"".join(bytes((b, g, r, 0)) for r, g, b in pal)
    off = 54 + len(palb)
    return (b"BM" + struct.pack("<IHHI", off + len(pix), 0, 0, off) +
            struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(pix), 0, 0, len(pal), 0) + palb + pix)


def _png(w, h, ct, depth, rows_raw, filt=0, extra=b""):
    def ch(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    raw = b"".join(bytes((filt,)) + r for r in rows_raw)
    return (T.PNG_SIG + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, 0)) + extra +
            ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b""))


def test_bmp_24_roundtrip():
    im = _img(5, 3)   # odd width -> row padding
    assert T.decode_bmp(T.encode_bmp(im)).rgba == im.rgba


@pytest.mark.parametrize("bpp", [1, 4, 8])
def test_bmp_palettised(bpp):
    n = 1 << bpp
    pal = [(i * 7 % 256, i * 13 % 256, i * 29 % 256) for i in range(n)]
    w, h = 7, 4
    idx = [(x + y * 3) % n for y in range(h) for x in range(w)]
    im = T.decode_bmp(_bmp_pal(w, h, bpp, idx, pal))
    assert im.rgba == b"".join(bytes(pal[i]) + b"\xff" for i in idx)


def test_png_encode_valid_and_alpha_choice():
    im = _img(9, 4)
    png = T.encode_png(im)
    assert png[25] == 2                                  # RGB when fully opaque
    assert T.decode_png(png).rgba == im.rgba
    rgba = bytearray(im.rgba)
    rgba[3] = 0
    png2 = T.encode_png(T.Image(9, 4, rgba))
    assert png2[25] == 6 and T.decode_png(png2).rgba == bytes(rgba)
    bad = bytearray(png2)
    bad[-5] ^= 1                                        # IEND CRC
    with pytest.raises(FormatError):
        T.decode_png(bytes(bad))


@pytest.mark.parametrize("filt", [0, 1, 2, 3, 4])
def test_png_filters_rgba(filt):
    w, h = 6, 5
    # build the filtered rows with a tiny reference encoder, then decode them
    src = [b"".join(bytes(((x * 40 + y) & 255, (y * 50) & 255, (x * y) & 255, 200 + x)) for x in range(w))
           for y in range(h)]
    rows, prev = [], bytes(w * 4)
    for r in src:
        out = bytearray()
        for i, v in enumerate(r):
            a = r[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            pred = {0: 0, 1: a, 2: b, 3: (a + b) >> 1, 4: _paeth(a, b, c)}[filt]
            out.append((v - pred) & 255)
        rows.append(bytes(out))
        prev = r
    assert T.decode_png(_png(w, h, 6, 8, rows, filt)).rgba == b"".join(src)


def test_png_palette_trns_grey_16bit():
    plte = struct.pack(">I", 6) + b"PLTE" + b"\x10\x20\x30\x40\x50\x60"
    plte += struct.pack(">I", zlib.crc32(plte[4:]))
    trns = struct.pack(">I", 1) + b"tRNS" + b"\x00"
    trns += struct.pack(">I", zlib.crc32(trns[4:]))
    p = _png(4, 1, 3, 2, [bytes((0b00010001,))], extra=plte + trns)   # indices 0,1,0,1 at 2 bits
    assert T.decode_png(p).rgba == bytes((16, 32, 48, 0, 64, 80, 96, 255) * 2)
    g = _png(2, 1, 4, 8, [bytes((10, 20, 30, 40))])
    assert T.decode_png(g).rgba == bytes((10, 10, 10, 20, 30, 30, 30, 40))
    s = _png(1, 1, 2, 16, [bytes((1, 2, 3, 4, 5, 6))])
    assert T.decode_png(s).rgba == bytes((1, 3, 5, 255))


def test_check_and_prepare():
    png = T.encode_png(_img(64, 64))
    assert T.check(png, "png") == ([], [])
    errs, _ = T.check(png, "bmp")
    assert errs
    bmp, notes = T.prepare_replacement(png, "bmp", size=(64, 64))
    assert T.info(bmp)["bpp"] == 24 and notes
    same, notes = T.prepare_replacement(png, "png", size=(64, 64))
    assert same == png and notes == []
    with pytest.raises(FormatError):
        T.prepare_replacement(png, "png", size=(32, 32))
    small, _ = T.prepare_replacement(png, "png", size=(32, 32), resize=True)
    assert T.info(small)["width"] == 32
    _, warns = T.check(T.encode_png(_img(48, 48)), "png")
    assert any("power of two" in w for w in warns)


def test_color_key_alpha_to_bmp():
    rgba = bytearray(_img(2, 1).rgba)
    rgba[3] = 0
    bmp = T.encode_bmp(T.Image(2, 1, rgba))
    im = T.decode_bmp(bmp)
    assert im.rgba[:3] == bytes(T.COLOR_KEY)
    assert T.apply_color_key(im).rgba[:4] == b"\0\0\0\0"


def test_mati_new_set_image_and_matsd():
    png = T.encode_png(_img(4, 4))
    m = Mati.new("my_tex", png)
    assert m.kind == "png" and m.name == "MY_TEX"
    assert m.surface == 0 and m.diffuse == pytest.approx((0.9, 0.9, 0.9))
    s = MatSd([m])
    data = s.write()
    s2 = MatSd.read(data)
    assert s2.write() == data and s2.texture("my_tex").image == png
    m2 = s2.materials()[0]
    m2.set_image(T.encode_bmp(_img(4, 4)))
    assert m2.kind == "bmp" and m2.to_chunk().children[1].tag == b"TBMP"
    m2.surface_word = (0x2131,)
    assert m2.surface == 1
    assert zmd.Mati is Mati


def test_matsd_errors():
    with pytest.raises(FormatError):
        MatSd.read(b"")
    with pytest.raises(FormatError):
        MatSd.read(b"MATI\x10\x00\x00\x00" + bytes(8))


def test_fuzz_decoders_and_matsd():
    png = T.encode_png(_img(16, 16))
    fuzz_parser(T.decode_png, png, n=200, seed=0)
    bmp = _bmp_pal(16, 8, 4, [i % 16 for i in range(128)], [(i, i, i) for i in range(16)])
    fuzz_parser(T.decode_bmp, bmp, n=200, seed=0)
    fuzz_parser(T.decode_bmp, T.encode_bmp(_img(8, 8)), n=200, seed=1)
    data = MatSd([Mati.new("A", png), Mati.new("B", bmp)]).write()
    fuzz_parser(MatSd.read, data, n=200, seed=0)


def test_fuzz_real_mat_sd_and_texture():
    files = [p for p in corpus_files("._sd") if p.name.lower() == "mat._sd"]
    if not files:
        pytest.skip("no corpus")
    p = min(files, key=lambda f: f.stat().st_size)
    fuzz_parser(MatSd.read, p.read_bytes(), n=200, seed=0, timeout=3.0)
    m = min(MatSd.read(p.read_bytes()).materials(), key=lambda m: len(m.image))
    fuzz_parser(T.decode, m.image, n=200, seed=0)

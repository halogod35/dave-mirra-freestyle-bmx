"""zprim images / fonts, STRS tables, HUD fonts and the gui_dir helpers on synthetic data (DM-066)."""
import struct

import pytest

from dmbmx.convert import imaging
from dmbmx.core.errors import FormatError
from dmbmx.formats import gui_dir, hudfont, strs, zprim
from fuzz import fuzz_parser


def make_image(frames=((4, 2),), bpp=4, colours=(0x7C1F, 0x001F, 0x03E0, 0x7C00), tail=b""):
    fr = []
    for k, (w, h) in enumerate(frames):
        rows = [[(x + y + k) % len(colours) for x in range(w)] for y in range(h)]
        fr.append(zprim.Frame(w, h, zprim.pack_indices(rows, w, bpp), hot_x=k, hot_y=-k))
    return zprim.Image(fr, zprim.Palette(colours, 0, tail), bpp)


def make_font():
    glyphs = [zprim.Glyph(0, 0, 0, 0, 0, 0, 5, 0, 0x20), zprim.Glyph(0, 0, 3, 4, 0, 1, 1, 0, 0x21),
              zprim.Glyph(0, 0, 0, 0, 0, 0, 0, 0, 0), zprim.Glyph(4, 0, 4, 4, -1, 0, 2, 1, 0x23)]
    pix = zprim.pack_indices([[(x * y) % 16 for x in range(8)] for y in range(4)], 8, 4)
    return zprim.Font(glyphs, 8, 4, pix, zprim.Palette([0x7C1F] + list(range(1, 256))), line_height=6)


def test_colour_conversion():
    assert zprim.rgb_of(0x7C1F) == (248, 0, 248, 0)
    assert zprim.rgb_of(0x001F) == (248, 0, 0, 255)          # red in the low bits
    assert zprim.rgb_of(0x7C00) == (0, 0, 248, 255)
    for c in range(0, 0x8000, 7):
        assert zprim.colour_of(*zprim.rgb_of(c)) == c
    assert zprim.colour_of(10, 20, 30, 0) == zprim.TRANSPARENT
    assert zprim.parse_colour("#ff0000") == 0x1F and zprim.parse_colour("7C1F") == 0x7C1F
    assert zprim.parse_colour("#00000000") == zprim.TRANSPARENT
    with pytest.raises(ValueError):
        zprim.parse_colour("12345")


def test_pack_low_nibble_first():
    assert zprim.pack_indices([[1, 2, 3, 4]], 4, 4) == bytes([0x21, 0x43])
    assert zprim.unpack_indices(bytes([0x21, 0x43]), 4, 1, 4) == [[1, 2, 3, 4]]


@pytest.mark.parametrize("bpp", [4, 8])
def test_image_roundtrip_and_png(bpp):
    img = make_image(frames=((4, 2), (6, 3)), bpp=bpp, tail=b"\xab" * 6)
    data = img.write()
    back = zprim.read_image(data)
    assert back.write() == data
    assert [f.hot_x for f in back.frames] == [0, 1] and back.palette.tail == b"\xab" * 6
    pngs = [zprim.frame_png(back, k) for k in range(2)]
    dec = imaging.png_decode(pngs[0])
    assert dec["bit_depth"] == bpp and dec["trns"] == b"\x00"     # entry 0 is 0x7C1F: transparent
    assert zprim.image_from_png(back, pngs).write() == data
    # a PNG palette edit becomes the 15-bit colour; untouched entries keep their exact value
    pal = list(dec["palette"])
    pal[1] = (0, 248, 0)
    edited = imaging.png_encode(4, 2, dec["rows"], color_type=3, bit_depth=bpp, palette=pal, trns=dec["trns"])
    new = zprim.image_from_png(back, [edited, None])
    assert new.palette.colours == [0x7C1F, 0x03E0, 0x03E0, 0x7C00]
    assert new.frames[1].pixels == back.frames[1].pixels


def test_image_from_rgba_png_quantizes():
    img = make_image()
    rgba = [bytes([0, 0, 0, 0, 250, 0, 0, 255, 0, 250, 0, 255, 0, 0, 250, 255]),
            bytes([240, 10, 0, 255, 0, 0, 0, 0, 0, 0, 255, 255, 0, 255, 0, 255])]
    png = imaging.png_encode(4, 2, rgba, color_type=6, bit_depth=8)
    warn = []
    new = zprim.image_from_png(img, [png], warn)
    assert new.frames[0].indices(4) == [[0, 1, 2, 3], [1, 0, 3, 2]]
    assert new.palette.colours == img.palette.colours
    assert warn


def test_image_png_errors():
    img = make_image()
    png8 = imaging.png_encode(4, 2, [bytes([0, 1, 2, 17])] * 2, color_type=3, bit_depth=8,
                              palette=[(0, 0, 0)] * 18)
    with pytest.raises(FormatError, match="index 17"):
        zprim.image_from_png(img, [png8])
    odd = imaging.png_encode(3, 2, [bytes([0, 1, 2])] * 2, color_type=3, bit_depth=8, palette=[(0, 0, 0)] * 4)
    with pytest.raises(FormatError, match="even"):
        zprim.image_from_png(img, [odd])


def test_image_size_change():
    img = make_image()
    png = imaging.png_encode(8, 3, [bytes(range(4)) * 2] * 3, color_type=3, bit_depth=8, palette=[(0, 0, 0)] * 4)
    warn = []
    new = zprim.image_from_png(img, [png], warn)
    back = zprim.read_image(new.write())
    assert (back.width, back.height) == (8, 3) and any("size" in w for w in warn)


def test_image_errors():
    data = make_image().write()
    with pytest.raises(FormatError):
        zprim.read_image(data[:10])
    bad = bytearray(data)
    struct.pack_into("<H", bad, 0x10, 5)                    # bpp 5
    with pytest.raises(FormatError, match="bpp"):
        zprim.read_image(bytes(bad))
    bad = bytearray(data)
    struct.pack_into("<I", bad, 0x0C, 0x30)                 # palette moved
    with pytest.raises(FormatError, match="palette"):
        zprim.read_image(bytes(bad))
    fuzz_parser(zprim.read_image, make_image(frames=((4, 2), (2, 2))).write(), n=300, seed=3)


def test_font_roundtrip_and_lookup():
    f = make_font()
    data = f.write()
    assert data[0x18 + 40:0x18 + 40 + 0] == b""
    back = zprim.read_font(data)
    assert back.write() == data and back.pad == b""         # 0x18 + 40 is already 4-aligned
    assert back.first == 0x20 and back.indexed
    assert back.glyph_for("!").w == 3 and back.glyph_for('"').w == 0
    assert back.glyph_for("~") is None
    assert back.text_width(" !") == 5 + (1 + 0 + 3)
    assert zprim.font_from_png(back, zprim.font_png(back)).write() == data
    three = zprim.Font(f.glyphs[:3], 8, 4, f.pixels, f.palette, 6)
    raw = three.write()
    assert raw[0x18 + 30:0x18 + 32] == b"\xcd\xcd"           # alignment pad as the MSVC fill
    assert zprim.read_font(raw).write() == raw
    fuzz_parser(zprim.read_font, data, n=300, seed=4)


def test_strs():
    body = b"ABC\0\0DE\0"
    data = b"STRS" + struct.pack("<HH3H", 0x1000, 3, 14, 18, 19) + body
    t = strs.read(data)
    assert t.write() == data and t.strings() == ["ABC", "", "DE"]
    assert not t.set(0x1000, "ABC")
    assert t.set(0x1001, "NEW") and strs.read(t.write()).strings() == ["ABC", "NEW", "DE"]
    with pytest.raises(KeyError):
        t.get(0x1003)
    with pytest.raises(FormatError):
        strs.read(b"STRX" + data[4:])
    with pytest.raises(FormatError):
        strs.read(data[:-1].replace(b"DE\0", b"DEF"))
    fuzz_parser(strs.read, data, n=300, seed=5)


def test_hudfont():
    recs = b"".join(struct.pack("<BHxxHxxHxx", c, u, v, w) for c, u, v, w in ((65, 0, 0, 9), (66, 9, 0, 7)))
    data = bytes([2, 0, 0, 0, 24, 1, 2, 3, 4, 5, 6, 7]) + recs
    f = hudfont.read(data)
    assert f.write() == data and f.height == 24 and f.glyph_for("B").u == 9
    with pytest.raises(FormatError):
        hudfont.read(data + b"x")
    fuzz_parser(hudfont.read, data, n=300, seed=6)


def test_json_dump_compact():
    s = gui_dir._dumps({"a": [1, 2, 3], "g": [{"x": 1, "y": 2}], "rows": ["0001 0002"]})
    assert '"a": [1, 2, 3]' in s and '{"x": 1, "y": 2}' in s
    assert gui_dir._colours_parse(gui_dir._colours_json(list(range(20)))) == list(range(20))

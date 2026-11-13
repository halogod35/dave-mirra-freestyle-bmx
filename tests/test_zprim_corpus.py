"""Menu art, fonts, texts and HUD fonts over the real data: byte-identical round trips of every
GUI-group picture / sprite / font, the four HUD fonts and the 16 STRS tables; PNG export -> import
gives the same bytes; the export directory imports back to the same Intwrap.zal, and an edited one
still parses and round-trips. Facts stated in docs/formats/ZAL.md 3.3."""
import struct

import pytest

from conftest import corpus_files
from dmbmx.convert import imaging
from dmbmx.formats import gui_dir, hudfont, scrn, strs, zal, zprim
from fuzz import fuzz_parser


def _asset_file(name):
    files = [p for p in corpus_files(".zal") if p.name.lower() == name]
    if not files:
        pytest.skip("no corpus")
    assert len(files) == 1
    return files[0].read_bytes()


@pytest.fixture(scope="module")
def intwrap():
    return _asset_file("intwrap.zal")


@pytest.fixture(scope="module")
def misc():
    return _asset_file("miscdata.zal")


@pytest.fixture(scope="module")
def material():
    return _asset_file("material.zal")


@pytest.fixture(scope="module")
def assets(intwrap):
    """{asset id: bytes} of every distinct GUI asset (screens excluded) + the per-type entry counts."""
    out, entries = {}, {}
    for gid, g in scrn.read_intwrap(intwrap).items():
        for aid, d in g.entries:
            entries[scrn.asset_type(aid)] = entries.get(scrn.asset_type(aid), 0) + 1
            if aid in out:
                assert out[aid] == d, f"0x{aid:04x} differs between groups"   # ZAL.md 3.3: same id, same bytes
            out[aid] = d
    assert entries == {"screen": 34, "sprite": 260, "picture": 12, "font": 6}
    return {a: d for a, d in out.items() if scrn.asset_type(a) != "screen"}


def test_counts(assets):
    kinds = {}
    for a in assets:
        kinds[scrn.asset_type(a)] = kinds.get(scrn.asset_type(a), 0) + 1
    # distinct ids: 12 pictures, 199 sprites, 4 fonts (0x8001 sits in three groups)
    assert kinds == {"picture": 12, "sprite": 199, "font": 4}
    assert sorted(a for a in assets if a & 0xE000 == 0x8000) == [0x8001, 0x8002, 0x8003, 0x8005]


def test_images_roundtrip_and_png(assets):
    stats = {}
    for aid, d in sorted(assets.items()):
        if aid & 0xE000 == 0x8000:
            continue
        img = zprim.read_image(d)
        assert img.write() == d, hex(aid)
        pngs = [zprim.frame_png(img, k) for k in range(len(img.frames))]
        for k, p in enumerate(pngs):
            info = imaging.png_info(p)
            assert (info["width"], info["height"], info["bit_depth"], info["color_type"]) == \
                (img.frames[k].width, img.frames[k].height, img.bpp, 3)
        assert zprim.image_from_png(img, pngs).write() == d, hex(aid)
        # the facts of ZAL.md 3.3
        assert (img.width, img.height) == (img.frames[0].width, img.frames[0].height)
        assert all(f.material == 0 and f.hot_x == 0 and f.hot_y == 0 for f in img.frames)
        assert len(img.palette.to_bytes()) == zprim.PALETTE_BLOCK
        assert img.pad_12 == 0 and img.palette.unk == 0
        if aid & 0xE000 == 0:
            assert (img.width, img.height, img.bpp, len(img.frames)) == (512, 512, 8, 1)
        key = (scrn.asset_type(aid), img.bpp, len(img.palette.colours), img.flags)
        stats[key] = stats.get(key, 0) + 1
    assert stats == {("picture", 8, 256, 0): 12, ("sprite", 8, 256, 0): 114, ("sprite", 4, 16, 0): 52,
                     ("sprite", 4, 256, 1): 33}


def test_fonts_roundtrip_and_png(assets):
    for aid in (0x8001, 0x8002, 0x8003, 0x8005):
        d = assets[aid]
        f = zprim.read_font(d)
        assert f.write() == d
        assert zprim.font_from_png(f, zprim.font_png(f)).write() == d
        assert f.bpp == 4 and f.width == 128 and len(f.palette.colours) == 256 and f.materials == 0
        assert f.indexed
        # an indexed glyph slot holds its own code or 0 (no glyph there)
        assert all(g.code in (0, f.first + k) for k, g in enumerate(f.glyphs))
    big = zprim.read_font(assets[0x8003])
    assert big.line_height == 28 and big.first == 0x20
    # the big menu font has no lower case: those slots are empty (w = h = 0), so lower-case text
    # prints as blanks instead of falling back to upper case (the fallback needs a missing slot)
    assert big.glyph_for("a").w == 0 and big.glyph_for("A").w > 0
    assert zprim.read_font(assets[0x8005]).first == ord("1")


def test_nibble_order(assets):
    """4-bit pixels: the left pixel is the LOW nibble (zprimMakeMaterial swaps them for the BMP).
    Mirror-symmetric glyphs of font 0x8003 are far more symmetric read that way than swapped."""
    f = zprim.read_font(assets[0x8003])

    def asym(swap):
        rows = zprim.unpack_indices(f.pixels, f.width, f.height, 4)
        n = 0
        for ch in "HIOXVTUAM":
            g = f.glyph_for(ch)
            for y in range(g.y, g.y + g.h):
                r = [rows[y][(x ^ 1) if swap else x] != 0 for x in range(g.x, g.x + g.w)]
                n += sum(r[i] != r[-1 - i] for i in range(len(r)))
        return n
    assert asym(False) * 1.4 < asym(True)      # 368 vs 568 (the drop shadow is not symmetric)


def test_fuzz_image_and_font(assets):
    small = min((d for a, d in assets.items() if a & 0xE000 == 0x4000), key=len)
    fuzz_parser(zprim.read_image, small, n=200, seed=0)
    fuzz_parser(zprim.read_image, assets[0x40C4], n=200, seed=1)
    fuzz_parser(zprim.read_font, assets[0x8005], n=200, seed=0)


# ------------------------------------------------------------------ texts, HUD fonts

def test_strs_roundtrip(misc):
    arc = zal.ZalArchive.read(misc)
    tabs = gui_dir.strs_tables(arc)
    assert sorted(tabs) == [*range(0x7919, 0x791D), *range(0x7D01, 0x7D0D)]
    for tid, e in tabs.items():
        d = e.data()
        t = strs.read(d)
        assert t.write() == d, hex(tid)
        assert t.base_id == (0 if tid < 0x7D01 else 0x1000 * (1 + (tid - 0x7D01) // 4)), hex(tid)
    t = strs.read(tabs[0x7919].data())
    assert [t.get(i) for i in (0x24, 0x25, 0x26, 0x29, 0x2A, 0x2C)] == \
        ["PROQUEST", "SESSION", "FREERIDE", "COOL STUFF", "OPTIONS", "QUIT"]
    before = t.strings()
    t.set(0x24, "MOD QUEST")
    t2 = strs.read(t.write())
    assert t2.get(0x24) == "MOD QUEST" and t2.strings()[:0x24] == before[:0x24]
    assert t2.strings()[0x25:] == before[0x25:]
    fuzz_parser(strs.read, tabs[0x7D09].data(), n=200, seed=0)


def test_strs_frontend_limit(misc):
    big = "X" * 6000
    with pytest.raises(gui_dir.GuiDirError, match="Macro data"):
        gui_dir.set_texts(misc, {0x7919: {0x24: big}})


def test_hud_fonts_roundtrip(misc, material):
    arc = zal.ZalArchive.read(misc)
    mat = zal.ZalArchive.read(material)
    seen, outside = {}, []
    for fid, (name, image, res) in hudfont.FONTS.items():
        d = arc.find(fid).data()
        f = hudfont.read(d)
        assert f.write() == d
        bmp = mat.find(image).data()
        info = imaging.bmp_info(bmp)
        for g in f.glyphs:
            if not (g.u + g.w <= info.width and g.v + f.height <= info.height):
                outside.append((name, chr(g.char)))
        assert gui_dir.sheet_from_png(bmp, imaging.bmp_to_png(bmp)) == bmp
        seen[name] = (len(f.glyphs), f.height, info.width, info.height, info.bpp)
        fuzz_parser(hudfont.read, d, n=200, seed=fid)
    # [V] the replay icons pause / play / record sit at v = 48 of the 64x48 icon sheet (outside it)
    assert outside == [("icons", "p"), ("icons", "P"), ("icons", "R")]
    assert seen == {"small": (82, 32, 256, 256, 4), "tiny": (144, 24, 256, 256, 8),
                    "button": (15, 32, 128, 128, 8), "icons": (11, 16, 64, 48, 8)}
    assert "".join(chr(g.char) for g in hudfont.read(arc.find(0x7534).data()).glyphs) == "wedcxzaqXT1SC2 "


def test_hud_dir_roundtrip(misc, material, tmp_path):
    gui_dir.export_hud(misc, material, tmp_path)
    assert gui_dir.import_hud(misc, material, tmp_path) == (None, None)
    js = (tmp_path / "icons.json").read_text(encoding="utf-8").replace('"height": 16', '"height": 17')
    (tmp_path / "icons.json").write_text(js, encoding="utf-8")
    new_misc, new_mat = gui_dir.import_hud(misc, material, tmp_path)
    assert new_mat is None
    assert hudfont.read(zal.ZalArchive.read(new_misc).find(0x7535).data()).height == 17


# ------------------------------------------------------------------ export directory

def test_gui_dir_identity_and_edit(intwrap, tmp_path):
    n = gui_dir.export_gui(intwrap, tmp_path)
    assert n == 1 + 34 + 215 + sum(1 for _ in (tmp_path / "sprites").glob("*.png")) \
        + sum(1 for _ in (tmp_path / "pictures").glob("*.png")) + sum(1 for _ in (tmp_path / "fonts").glob("*.png"))
    assert gui_dir.import_gui(intwrap, tmp_path) == intwrap

    # edit: one palette entry of the rider-0 portrait (PNG), a screen text (JSON), a font bank (JSON)
    p = tmp_path / "sprites" / "40B6_0.png"
    img = imaging.png_decode(p.read_bytes())
    pal = list(img["palette"])
    pal[0] = (255, 0, 0)
    rows = img["rows"]
    p.write_bytes(imaging.png_encode(img["width"], img["height"], rows, color_type=3, bit_depth=8, palette=pal,
                                     trns=img["trns"]))
    sj = tmp_path / "screens" / "A009.json"
    sj.write_text(sj.read_text(encoding="utf-8").replace('"String to be preallocated"', '"EDITED"', 1),
                  encoding="utf-8")
    fj = tmp_path / "fonts" / "8002.json"
    t = fj.read_text(encoding="utf-8")
    fj.write_text(t.replace('"line_height": 20', '"line_height": 21'), encoding="utf-8")
    report = []
    out = gui_dir.import_gui(intwrap, tmp_path, report=report)
    assert sorted(a for a, *_ in report) == [0x40B6, 0x8002, 0xA009]
    groups = scrn.read_intwrap(out)
    sprite = zprim.read_image(groups[0xE004].asset(0x40B6))
    assert sprite.palette.colours[0] == 0x001F
    assert zprim.read_font(groups[0xE000].asset(0x8002)).line_height == 21
    scr = scrn.read_screen(groups[0xE009].asset(0xA009))
    assert "EDITED" in [t for _, _, t, _ in scr.texts()]
    # the edited archive round-trips, and only the three groups changed
    assert zal.ZalArchive.read(out).write() == out
    orig = scrn.read_intwrap(intwrap)
    assert sorted(g for g in groups if groups[g].write() != orig[g].write()) == [0xE000, 0xE004, 0xE009]
    for gid, g in groups.items():
        assert scrn.read_group(g.write()).write() == g.write()


def test_export_one_screen(intwrap, tmp_path):
    gui_dir.export_gui(intwrap, tmp_path, screens=[0xA005])
    names = sorted(p.name for p in tmp_path.rglob("*.json"))
    assert names == ["40C7.json", "A005.json", "_gui.json"]
    with pytest.raises(gui_dir.GuiDirError):
        gui_dir.export_gui(intwrap, tmp_path / "x", screens=[0xA0FF])

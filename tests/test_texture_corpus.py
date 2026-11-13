"""DM-014 corpus tests: Mat._sd round-trip, MADF invariants, image decode/encode, texture replacement."""
import struct

import pytest
from conftest import GAME_DIR, corpus_files

from dmbmx.cli import main
from dmbmx.core.hash import zhash
from dmbmx.formats import texture as T
from dmbmx.formats import zal, zmd
from dmbmx.formats.mati import MatSd


def _mat_sds():
    return [p for p in corpus_files("._sd") if p.name.lower() == "mat._sd"]


def _params():
    files = _mat_sds()
    if not files:
        return [pytest.param(None, marks=pytest.mark.skip(reason="no Mat._sd corpus"))]
    return [pytest.param(p, id=p.parent.name) for p in files]


def _all_matis():
    out = []
    for p in corpus_files(".zmd"):
        if p.stat().st_size:
            out += [(p, m) for m in zmd.read(p.read_bytes()).materials()]
    for p in _mat_sds():
        out += [(p, m) for m in MatSd.read(p.read_bytes()).materials()]
    return out


@pytest.mark.parametrize("path", _params())
def test_mat_sd_roundtrip(path):
    data = path.read_bytes()
    s = MatSd.read(data)
    assert s.write() == data
    assert s.materials() and len(s.materials()) == len(s.items)


def test_counts_and_madf_invariants(game_dir):
    assert len(_mat_sds()) == 12
    ms = _all_matis()
    zmd_n = sum(1 for p, _ in ms if p.suffix.lower() == ".zmd")
    assert (zmd_n, len(ms) - zmd_n) == (324, 872)
    kinds = {}
    for p, m in ms:
        kinds[m.kind] = kinds.get(m.kind, 0) + 1
        assert struct.unpack_from("<I", m.madf_head, 0)[0] == 0x48          # [V] 0x004893F5
        assert m.name_id == zhash(m.name), (p.name, m.name)
        assert len(m.to_chunk().children[0].data) < 0x90                   # [V] "Data too big"
        assert m.flags == (0,) and m.word08 == (0,) and m.word0c == (0,)
        assert m.param34 == (0.25,)
        assert 0 <= m.surface <= 14
    assert kinds == {"bmp": 1085, "png": 111}


def test_decode_every_zmd_texture(game_dir):
    """Decode all 324 ZMD textures; PNG <-> RGBA and BMP <-> RGBA re-encode losslessly."""
    for p, m in _all_matis():
        if p.suffix.lower() != ".zmd":
            continue
        img = T.decode(m.image)
        i = T.info(m.image)
        assert (img.width, img.height) == (i["width"], i["height"])
        png = T.encode_png(img)
        assert T.decode_png(png).rgba == img.rgba
        if m.kind == "bmp":
            assert T.decode_bmp(T.encode_bmp(img)).rgba == img.rgba
        errors, _ = T.check(m.image, m.kind)
        assert not errors, (p.name, m.name, errors)


def test_decode_level_and_material_zal_sample(game_dir):
    for p in _mat_sds():
        for m in MatSd.read(p.read_bytes()).materials()[::8]:
            assert not T.check(m.image, m.kind)[0] or (T.info(m.image)["width"] in (80, 96))
            T.decode(m.image)
    arc = zal.read((GAME_DIR / "assets" / "material.zal").read_bytes())
    for e in arc.entries[::25]:
        assert T.decode(e.data()).width > 0


def _checker(w, h, a=(0, 200, 0), b=(255, 140, 0), cell=8):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((a if ((x // cell) + (y // cell)) % 2 else b) + (255,))
    return T.Image(w, h, px)


def test_replace_bmp_slot_with_png(game_dir, tmp_path):
    src = GAME_DIR / "assets" / "DMbike01.zmd"
    orig = zmd.read(src.read_bytes())
    png = tmp_path / "frame.png"
    png.write_bytes(T.encode_png(_checker(32, 32)))
    out = tmp_path / "DMbike01.zmd"
    assert main(["tex", "replace", str(src), "name_801_1", str(png), str(out), "--resize"]) == 0
    new = zmd.read(out.read_bytes())
    assert zmd.write(new) == out.read_bytes()
    m = new.texture("NAME_801_1")
    i = T.info(m.image)
    assert (m.kind, i["bpp"], i["width"], i["height"]) == ("bmp", 24, 128, 128)
    assert T.decode(m.image).rgba == T.resize_nearest(_checker(32, 32), 128, 128).rgba
    for a, b in zip(orig.materials(), new.materials()):
        if a.name != "NAME_801_1":
            assert (a.image, a.madf_head, a.name_id) == (b.image, b.madf_head, b.name_id)
    # without --resize a size mismatch is refused
    with pytest.raises(SystemExit):
        main(["tex", "replace", str(src), "NAME_801_1", str(png), str(tmp_path / "x.zmd")])


def test_replace_png_slot_keeps_valid_file_verbatim(game_dir, tmp_path):
    src = GAME_DIR / "assets" / "DMbike01.zmd"
    tire = zmd.read(src.read_bytes()).texture("TIRE")
    img = T.decode(tire.image)
    rgba = bytearray(img.rgba)
    rgba[0::4] = bytes(255 - v for v in rgba[0::4])
    new_png = T.encode_png(T.Image(img.width, img.height, rgba))
    f = tmp_path / "tire.png"
    f.write_bytes(new_png)
    out = tmp_path / "b.zmd"
    assert main(["tex", "replace", str(src), "TIRE", str(f), str(out)]) == 0
    assert zmd.read(out.read_bytes()).texture("TIRE").image == new_png


def test_mat_sd_replace_and_export(game_dir, tmp_path):
    p = _mat_sds()[0]
    s = MatSd.read(p.read_bytes())
    name = s.materials()[0].name
    i = T.info(s.materials()[0].image)
    img = tmp_path / "x.bmp"
    img.write_bytes(T.encode_bmp(_checker(i["width"], i["height"])))
    out = tmp_path / "Mat._sd"
    assert main(["tex", "replace", str(p), name, str(img), str(out)]) == 0
    s2 = MatSd.read(out.read_bytes())
    assert s2.write() == out.read_bytes()
    assert s2.texture(name).image == img.read_bytes()
    d = tmp_path / "exp"
    assert main(["tex", "export", "--png", str(out), str(d)]) == 0
    assert (d / f"{name}.bmp").read_bytes() == img.read_bytes()
    assert T.decode_png((d / f"{name}.png").read_bytes()).rgba == T.decode(img.read_bytes()).rgba
    # a palettised level BMP exports as a palettised PNG (imaging.bmp_to_png, lossless)
    d2 = tmp_path / "exp2"
    assert main(["tex", "export", "--png", str(p), str(d2)]) == 0
    m0 = s.materials()[1]
    if T.info(m0.image)["bpp"] <= 8:
        assert T.png_info((d2 / f"{m0.name}.png").read_bytes())["color_type"] == 3


def test_export_material_zal_and_refuse_install(game_dir, tmp_path):
    d = tmp_path / "mz"
    assert main(["tex", "export", str(GAME_DIR / "assets" / "material.zal"), str(d)]) == 0
    assert len(list(d.glob("*.bmp"))) == 1262
    with pytest.raises(SystemExit):
        main(["tex", "export", str(GAME_DIR / "assets" / "DMbike01.zmd"), str(GAME_DIR / "assets" / "tx")])
    assert not (GAME_DIR / "assets" / "tx").exists()

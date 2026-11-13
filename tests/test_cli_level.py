"""`dmbmx level` CLI smoke tests."""
import json
import struct

import pytest

from dmbmx.cli import main


def test_level_help(capsys):
    with pytest.raises(SystemExit) as e:
        main(["level", "--help"])
    assert e.value.code == 0
    assert "dump-obj" in capsys.readouterr().out


def test_level_info(game_dir, capsys):
    assert main(["level", "info", str(game_dir / "assets" / "AcclaimDirt"), "--props", "--points"]) == 0
    out = capsys.readouterr().out
    assert "SCFI type 4 BSP._sd" in out and "MISMATCH" not in out
    assert "textures resolved in Mat._sd: 51/51" in out
    assert "Prop: 87 instances" in out and "Tag: 1628 faces" in out


def test_level_dump_obj(game_dir, tmp_path, capsys):
    out = tmp_path / "lv.obj"
    assert main(["level", "dump-obj", str(game_dir / "assets" / "WoodwardStreet"), str(out)]) == 0
    text = out.read_text()
    assert text.count("\nf ") > 3850 and "o props" in text
    mtl = out.with_suffix(".mtl").read_text()
    pngs = list(tmp_path.glob("lv_*.png"))
    assert pngs and mtl.count("map_Kd") == len(pngs)
    assert all(p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" for p in pngs)


def test_level_export_glb(game_dir, tmp_path, capsys):
    out = tmp_path / "lv.glb"
    assert main(["level", "export", str(game_dir / "assets" / "AcclaimDirt"), str(out)]) == 0
    data = out.read_bytes()
    magic, ver, total = struct.unpack_from("<III", data)
    assert (magic, ver, total) == (0x46546C67, 2, len(data))
    jl = struct.unpack_from("<I", data, 12)[0]
    js = json.loads(data[20:20 + jl])
    names = [n.get("name") for n in js["nodes"]]
    assert {"World", "Props", "Tags", "ScenePoints"} <= set(names)
    props = js["nodes"][names.index("Props")]["children"]
    assert len(props) == 87
    ex = js["nodes"][props[0]]["extras"]["dmbmx"]
    assert ex["type"] == "prop" and len(ex["matrix_zge"]) == 9
    assert js["nodes"][0]["extras"]["dmbmx"]["origin"] == pytest.approx([168.0, 55.545, 193.84], abs=1e-3)
    assert len(js["images"]) >= 51


def test_level_rewrite(game_dir, tmp_path, capsys):
    src = game_dir / "assets" / "GreenvilleDirt"
    assert main(["level", "rewrite", str(src), str(tmp_path / "out")]) == 0
    assert "8 byte-identical" in capsys.readouterr().out
    assert main(["level", "rewrite", str(src), str(src)]) == 2

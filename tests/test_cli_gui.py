"""`dmbmx gui` CLI smoke tests (output into tmp_path)."""
import subprocess
import sys
from pathlib import Path

import pytest
from conftest import corpus_files

from dmbmx.cli import main
from dmbmx.formats import scrn, strs, zal, zprim

REPO = Path(__file__).resolve().parents[1]
ACTIONS = ("list", "export", "import", "texts", "text-set", "text-import", "hud-export", "hud-import")


def _zal(name):
    for f in corpus_files(".zal"):
        if f.name.lower() == name:
            return f
    pytest.skip("no corpus")


def test_help():
    r = subprocess.run([sys.executable, "-m", "dmbmx", "gui", "--help"], capture_output=True, text=True, cwd=REPO)
    assert r.returncode == 0
    for a in ACTIONS:
        assert a in r.stdout
    for a in ACTIONS:
        r = subprocess.run([sys.executable, "-m", "dmbmx", "gui", a, "--help"], capture_output=True, text=True,
                           cwd=REPO)
        assert r.returncode == 0, a


def test_list(capsys):
    iw = _zal("intwrap.zal")
    assert main(["gui", "list", str(iw)]) == 0
    out = capsys.readouterr().out
    assert out.count("group 0x") == 35 and "0x8003   13952  font     189 glyphs, line 28" in out
    assert main(["gui", "list", str(iw), "--screen", "a009"]) == 0
    out = capsys.readouterr().out
    assert "screen 0xa009" in out and "'String to be preallocated'" in out and out.count("\n") >= 36


def test_export_edit_import(tmp_path, capsys):
    iw = _zal("intwrap.zal")
    d = tmp_path / "gui"
    assert main(["gui", "export", str(iw), str(d), "--screen", "0xA005"]) == 0
    js = d / "screens" / "A005.json"
    js.write_text(js.read_text(encoding="utf-8").replace('"5"', '"6"', 1), encoding="utf-8")
    out = tmp_path / "Intwrap.zal"
    assert main(["gui", "import", str(iw), str(d), str(out)]) == 0
    assert "1 asset(s) changed" in capsys.readouterr().out
    scr = scrn.read_screen(scrn.read_intwrap(out.read_bytes())[0xE005].asset(0xA005))
    assert scr.string_at(scr.edefs[5].text) == "6"


def test_texts(tmp_path, capsys):
    misc = _zal("miscdata.zal")
    assert main(["gui", "texts", str(misc), "--table", "0x7919"]) == 0
    assert "0x0024 'PROQUEST'" in capsys.readouterr().out
    out = tmp_path / "MiscData.ZAL"
    assert main(["gui", "text-set", str(misc), "0x24", "MOD QUEST", str(out)]) == 0
    arc = zal.ZalArchive.read(out.read_bytes())
    for tid in strs.FRONTEND_TABLES:
        assert strs.read(arc.find(tid).data()).get(0x24) == "MOD QUEST"
    assert strs.read(arc.find(0x7D01).data()).get(0x1000) == \
        strs.read(zal.ZalArchive.read(misc.read_bytes()).find(0x7D01).data()).get(0x1000)
    j = tmp_path / "t.json"
    assert main(["gui", "texts", str(out), "--json", str(j), "--table", "0x7919"]) == 0
    j.write_text(j.read_text(encoding="utf-8").replace('"MOD QUEST"', '"PROQUEST"'), encoding="utf-8")
    out2 = tmp_path / "m2" / "MiscData.ZAL"
    assert main(["gui", "text-import", str(out), str(j), str(out2)]) == 0
    assert strs.read(zal.ZalArchive.read(out2.read_bytes()).find(0x7919).data()).get(0x24) == "PROQUEST"


def test_hud(tmp_path, capsys):
    assets = _zal("miscdata.zal").parent
    d = tmp_path / "hud"
    assert main(["gui", "hud-export", str(assets), str(d)]) == 0
    assert sorted(p.name for p in d.iterdir()) == ["button.json", "button.png", "icons.json", "icons.png",
                                                   "small.json", "small.png", "tiny.json", "tiny.png"]
    assert main(["gui", "hud-import", str(assets), str(d), str(tmp_path / "o")]) == 0
    assert "nothing changed" in capsys.readouterr().out


def test_refuses_install(tmp_path):
    iw = _zal("intwrap.zal")
    with pytest.raises(SystemExit):
        main(["gui", "export", str(iw), str(iw.parent / "x_gui_out")])
    assert not (iw.parent / "x_gui_out").exists()

"""CLI smoke tests for `dmbmx zmd`."""
import subprocess
import sys

from dmbmx.cli import main


def test_zmd_help():
    r = subprocess.run([sys.executable, "-m", "dmbmx", "zmd", "--help"], capture_output=True, text=True)
    assert r.returncode == 0 and "dump-obj" in r.stdout and "extract-textures" in r.stdout


def test_zmd_commands(game_dir, tmp_path, capsys):
    bike = str(game_dir / "assets" / "bike.zmd")
    assert main(["zmd", "info", bike, "--tree"]) == 0
    out = capsys.readouterr().out
    assert "atomics=7" in out and "BinMesh" in out and "MATI: 'TIRE'" in out
    obj = tmp_path / "bike.obj"
    assert main(["zmd", "dump-obj", bike, str(obj)]) == 0
    text = obj.read_text()
    assert text.count("\nv ") == 456 and text.count("\nf ") == 408
    assert (tmp_path / "bike.mtl").exists() and (tmp_path / "TIRE.png").exists()
    assert main(["zmd", "extract-textures", bike, str(tmp_path / "tex")]) == 0
    assert sorted(p.suffix for p in (tmp_path / "tex").iterdir()) == [".bmp", ".bmp", ".bmp", ".png", ".png"]

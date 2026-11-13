"""`dmbmx export` CLI smoke tests (output into tmp_path)."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from conftest import corpus_files

from dmbmx.cli import main
from dmbmx.convert import gltf

REPO = Path(__file__).resolve().parents[1]


def _find(stem):
    for f in corpus_files(".zmd"):
        if f.stem.lower() == stem.lower():
            return f
    pytest.skip(f"{stem} not in corpus")


def test_help():
    r = subprocess.run([sys.executable, "-m", "dmbmx", "export", "--help"], capture_output=True, text=True,
                       cwd=REPO)
    assert r.returncode == 0 and "--all" in r.stdout and "--lod" in r.stdout


def test_export_one_with_obj(tmp_path, capsys):
    src = _find("DMbike01")
    out = tmp_path / "bike.glb"
    assert main(["export", str(src), str(out), "--obj"]) == 0
    assert "684 vertices, 408 triangles" in capsys.readouterr().out
    assert gltf.restore_zmd(out.read_bytes()) == src.read_bytes()
    obj = (tmp_path / "bike.obj").read_text()
    assert obj.count("\nf ") == 408 and (tmp_path / "bike.mtl").exists()
    assert list(tmp_path.glob("bike_*.png"))


def test_export_lod(tmp_path):
    src = _find("DaveMirra_shadow")
    out = tmp_path / "s.glb"
    assert main(["export", str(src), str(out), "--lod", "1"]) == 0
    js, _ = gltf.read_glb(out.read_bytes())
    assert js["nodes"][0]["extras"]["dmbmx"]["complete"] is False


def test_export_all_skips_empty(tmp_path, capsys):
    src = [_find("DMbike01"), _find("TroyMcMurray1v2")]
    game = tmp_path / "game" / "assets"
    game.mkdir(parents=True)
    for f in src:
        shutil.copy(f, game / f.name)
    outdir = tmp_path / "out"
    assert main(["export", "--all", str(outdir), "--game-dir", str(tmp_path / "game")]) == 0
    text = capsys.readouterr().out
    assert "skip" in text and "empty file" in text
    assert "exported 1, skipped 1 (empty), failed 0, of 2 files" in text
    assert [p.name for p in outdir.glob("*.glb")] == ["DMbike01.glb"]


def test_bad_usage(tmp_path):
    assert main(["export", "only_one_arg"]) == 2
    assert main(["export", "--all", "a", "b"]) == 2
    bad = tmp_path / "bad.zmd"
    bad.write_bytes(b"NOPE" + bytes(20))
    assert main(["export", str(bad), str(tmp_path / "x.glb")]) == 1

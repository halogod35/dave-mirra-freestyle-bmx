"""CLI smoke tests for `dmbmx zal list|extract`."""
import json

import pytest
from conftest import corpus_files

from dmbmx.cli import main


def _zal(name):
    for p in corpus_files(".zal"):
        if p.name.lower() == name:
            return p
    pytest.skip(f"no {name} in corpus")


def test_zal_help(capsys):
    with pytest.raises(SystemExit) as e:
        main(["zal", "--help"])
    assert e.value.code == 0
    assert "extract" in capsys.readouterr().out


def test_zal_list(capsys):
    assert main(["zal", "list", str(_zal("miscdata.zal"))]) == 0
    out = capsys.readouterr().out
    assert "0x00007531" in out and "STRS" in out


def test_zal_extract(tmp_path):
    assert main(["zal", "extract", "-r", str(_zal("tables.zal")), str(tmp_path / "t")]) == 0
    idx = json.loads((tmp_path / "t" / "_index.json").read_text())
    assert idx["groups"] == 1 and idx["entries"][0]["file"] == "00000000.iff"
    assert (tmp_path / "t" / "00000000.iff").read_bytes()[:4] == b"FORM"


def test_zal_extract_refuses_install(game_dir):
    with pytest.raises(SystemExit):
        main(["zal", "extract", str(_zal("tables.zal")), str(game_dir / "assets" / "x")])
    assert not (game_dir / "assets" / "x").exists()

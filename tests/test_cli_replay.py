"""CLI smoke tests for `dmbmx replay list|dump`."""
import json

import pytest
from conftest import corpus_files

from dmbmx.cli import main


def _replay():
    for p in corpus_files(".zal"):
        if p.name.lower() == "replay.zal":
            return p
    pytest.skip("no replay.zal in corpus")


def test_replay_help(capsys):
    with pytest.raises(SystemExit) as e:
        main(["replay", "--help"])
    assert e.value.code == 0
    assert "dump" in capsys.readouterr().out


def test_replay_list(capsys):
    assert main(["replay", "list", str(_replay())]) == 0
    out = capsys.readouterr().out
    assert out.count("(attract demo)") == 3 and " psx " in out and " raw " in out


def test_replay_dump(capsys):
    assert main(["replay", "dump", "8", str(_replay()), "--frames"]) == 0
    d = json.loads(capsys.readouterr().out)
    assert d["layout"] == "pc" and len(d["frames"]) == d["fields"]["frames"] == 3652
    assert d["events"][-1]["value"] == 0

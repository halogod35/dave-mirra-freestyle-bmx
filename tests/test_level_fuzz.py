"""Fuzz every level-file reader (docs/testing.md section 4): smallest + one representative file."""
import pytest

from conftest import corpus_files
from fuzz import fuzz_parser
from dmbmx.formats import level as lvl
from dmbmx.formats import rwworld

READERS = {
    "bsp._sd": rwworld.read,
    "prop._sd": lvl.PropSd.read,
    "propdef._sd": lvl.PropDefSd.read,
    "scene._sd": lvl.SceneSd.read,
    "tag._sd": lvl.TagSd.read,
    "temp.dff": lvl.TempDff.read,
}


def _pick(name):
    files = [p for p in corpus_files("._sd") + corpus_files(".dff") if p.name.lower() == name]
    if not files:
        pytest.skip("no level corpus")
    smallest = min(files, key=lambda p: p.stat().st_size)
    rep = next((p for p in files if p.parent.name == "AcclaimDirt"), files[-1])
    return {smallest, rep}


@pytest.mark.parametrize("name", sorted(READERS))
def test_level_fuzz(name):
    for f in _pick(name):
        rep = fuzz_parser(READERS[name], f.read_bytes(), n=200, seed=0)
        assert rep.rejected > 0


def test_zsd_fuzz():
    files = [p for p in corpus_files(".zsd") if p.parent.parent.name.lower() == "assets"]
    if not files:
        pytest.skip("no level corpus")
    for f in (files[0], files[-1]):
        rep = fuzz_parser(lvl.Zsd.read, f.read_bytes(), n=300, seed=0)
        assert rep.rejected > 0


def test_synthetic_tag_fuzz():
    from test_level_unit import make_tags, make_zsd, make_props, make_scene, tiny_world
    fuzz_parser(lvl.TagSd.read, make_tags().write(), n=300, seed=1)
    fuzz_parser(lvl.Zsd.read, make_zsd(), n=300, seed=1)
    fuzz_parser(lvl.PropSd.read, make_props(), n=300, seed=1)
    fuzz_parser(lvl.SceneSd.read, make_scene(), n=300, seed=1)
    fuzz_parser(rwworld.read, tiny_world().write(), n=300, seed=1)

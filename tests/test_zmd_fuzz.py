"""Fuzz the ZMD reader (docs/testing.md section 4)."""
import pytest

from conftest import corpus_files
from fuzz import fuzz_parser
from dmbmx.formats import zmd


def test_zmd_fuzz():
    files = [f for f in corpus_files(".zmd") if f.stat().st_size]
    if not files:
        pytest.skip("no corpus")
    smallest = min(files, key=lambda p: p.stat().st_size)
    rider = next((f for f in files if f.name == "DaveMirra.ZMD"), files[0])
    for f in (smallest, rider):
        rep = fuzz_parser(zmd.read, f.read_bytes(), n=200, seed=0)
        assert rep.rejected > 0

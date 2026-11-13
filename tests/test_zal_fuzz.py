"""Fuzz the ZAL reader and the LZO1X decoder with mutated real files (docs/testing.md section 4)."""
import pytest
from conftest import corpus_files
from fuzz import fuzz_parser

from dmbmx.core import lzo
from dmbmx.formats import zal


def _files():
    files = corpus_files(".zal")
    if not files:
        pytest.skip("no .zal corpus")
    return {p.name.lower(): p for p in files}


def _read_all(data):
    """Parse and decode every asset (exercises the LZO path from the table)."""
    arc = zal.read(data)
    for e in arc.entries:
        e.data()
    return arc


def test_zal_fuzz_smallest_and_representative():
    files = _files()
    smallest = min(files.values(), key=lambda p: p.stat().st_size)
    for p in (smallest, files["miscdata.zal"], files["replay.zal"]):
        fuzz_parser(zal.read, p.read_bytes(), n=200, seed=0)


def _small_compressed(n=8):
    """A real LZO-compressed archive cut down to its first n groups (built in memory, never saved)."""
    arc = zal.read(_files()["material.zal"].read_bytes())
    arc.entries = arc.entries[:n]
    return arc.write()


def test_zal_fuzz_compressed_decode():
    # full read + LZO decode of every group; the whole motion.zal is too slow under the fuzz tracer
    data = _small_compressed()
    assert len(_read_all(data).entries) == 8
    fuzz_parser(_read_all, data, n=200, seed=0, timeout=5.0)


def test_lzo_fuzz_material_entry():
    arc = zal.read(_small_compressed(40))
    a = max((e.assets[0] for e in arc.entries if len(e.assets[0].stored) < 1500), key=lambda a: len(a.stored))
    fuzz_parser(lambda b: lzo.decompress(b, a.size), a.stored, n=300, seed=0)
    fuzz_parser(lzo.decompress, a.stored, n=300, seed=1)

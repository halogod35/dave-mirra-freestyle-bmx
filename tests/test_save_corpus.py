"""SAVE.DAT against the real save in the install (skips without the game)."""
import pytest

from conftest import corpus_files, corpus_params
from fuzz import fuzz_parser
from dmbmx.formats import save
from dmbmx.integrity.crc import check_save, resign_save


def test_save_corpus_count(game_dir):
    assert [p.name for p in corpus_files(".dat")] == ["SAVE.DAT"]


@pytest.mark.parametrize("path", corpus_params(".dat"))
def test_save_roundtrip(path):
    data = path.read_bytes()
    s = save.read(data)
    out = save.write(s)
    assert out == data, f"first diff at 0x{next(i for i, (a, b) in enumerate(zip(out, data)) if a != b):x}"
    assert check_save(data) == (True, "ok")
    assert resign_save(data) == data                     # fix-crc on an unmodified file is a no-op
    assert len(data) == save.SIZE and s.extra == b""


@pytest.mark.parametrize("path", corpus_params(".dat"))
def test_save_fuzz(path):
    # The save is a fixed-size raw dump: most mutations still parse (returned), the rest
    # must be clean FormatErrors. n >= 200, fixed seed (docs/testing.md section 4).
    rep = fuzz_parser(save.read, path.read_bytes(), n=300, seed=0)
    rep2 = fuzz_parser(lambda b: save.read(b, lenient=True), path.read_bytes(), n=300, seed=1)
    assert rep.cases == 300 and rep2.cases == 300

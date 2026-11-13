"""LZO1X encoder unit tests (synthetic data) and decoder fuzzing with encoder output."""
import random

import pytest
from fuzz import fuzz_parser

from dmbmx.core import lzo
from dmbmx.core.errors import FormatError
from dmbmx.formats import zal_dir


def _samples():
    r = random.Random(7)
    blk = bytes(r.randrange(256) for _ in range(20000))
    yield b""
    yield b"a"
    yield b"abc"
    yield b"abcd"
    yield b"a" * 5000                                   # long overlapping match (len > 264)
    yield bytes(r.randrange(256) for _ in range(3000))  # incompressible: long literal runs
    yield b"xy" * 10 + bytes(r.randrange(256) for _ in range(300))
    # far matches: M3 (<= 0x4000) and M4 (> 0x4000, bit 14 of the distance set)
    yield blk + bytes(r.randrange(256) for _ in range(30000)) + blk[:5000] + blk[100:300]
    yield blk[:9000] + blk[:3] + blk[50:60]
    for _ in range(200):
        n = r.randrange(0, 2000)
        alph = r.randrange(1, 40)
        yield bytes(r.randrange(alph) for _ in range(n))


@pytest.mark.parametrize("level", [1, 2])
def test_roundtrip_synthetic(level):
    for s in _samples():
        z = lzo.compress(s, level)
        assert lzo.decompress(z, len(s)) == s
        assert z.endswith(b"\x11\x00\x00")


def test_empty_is_just_end_marker():
    assert lzo.compress(b"") == b"\x11\x00\x00"


def test_compresses_redundant_data():
    assert len(lzo.compress(b"abcdefgh" * 1000)) < 100


def test_fuzz_decoder_on_encoder_output():
    r = random.Random(3)
    src = bytes(r.randrange(8) for _ in range(1500)) + b"pattern!" * 50
    z = lzo.compress(src)
    fuzz_parser(lambda b: lzo.decompress(b, len(src)), z, n=300, seed=0)
    fuzz_parser(lzo.decompress, z, n=300, seed=1)


def test_pack_dir_errors(tmp_path):
    with pytest.raises(FormatError):
        zal_dir.pack_dir(tmp_path)                       # no _index.json
    (tmp_path / "_index.json").write_text('{"format": "other"}')
    with pytest.raises(FormatError):
        zal_dir.pack_dir(tmp_path)

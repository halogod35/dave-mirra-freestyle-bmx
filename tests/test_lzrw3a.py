"""LZRW3-A (ZAL compression method 1, exe 0x0049BD10) codec and its use in zal.py."""
import os
import random

import pytest

from conftest import GAME_DIR
from fuzz import fuzz_parser
from dmbmx.core import lzrw3a
from dmbmx.core.errors import FormatError
from dmbmx.formats import zal


def test_hash_matches_exe_arithmetic():
    """The exe computes -417*x (0x0049BEED..0x0049BF06); only bits 4..12 survive, so it equals 40543*x."""
    rnd = random.Random(1)
    for _ in range(2000):
        b = bytes(rnd.randrange(256) for _ in range(3))
        x = (b[0] << 8) ^ (b[1] << 4) ^ b[2]
        exe = (((-417 * x) & 0xFFFFFFFF) >> 4 & 0x1FF) << 3       # sar vs shr: bits 4..12 identical
        assert lzrw3a._hash(b, 0) == exe


def test_hand_built_streams():
    # one control word (item 0 = copy), copy index 0 (table still points at the start string), length 3+2
    stream = bytes([0, 0, 0, 0]) + bytes([0x01, 0x00]) + bytes([0x02, 0x00])
    assert lzrw3a.decompress(stream) == b"12345"
    # three literals then a copy of the slot the third literal hashed into
    lits = b"abc"
    h = lzrw3a._hash(lits, 0)            # literal #3 stores position 0 at slot h + cycle 0
    stream = bytes(4) + bytes([0x08, 0x00]) + lits + bytes([((h >> 4) & 0xF0) | 0, h & 0xFF])
    assert lzrw3a.decompress(stream) == b"abcabc"
    assert lzrw3a.decompress(bytes([1, 0, 0, 0]) + b"raw") == b"raw"     # FLAG_COPY


@pytest.mark.parametrize("data", [b"", b"x", b"abcabcabc" * 50, bytes(5000), b"123456789012345678" * 4,
                                  os.urandom(3000), bytes(range(256)) * 20])
def test_roundtrip_synthetic(data):
    z = lzrw3a.compress(data)
    assert lzrw3a.decompress(z, len(data)) == data
    assert len(z) <= len(data) + 4


def test_roundtrip_corpus(game_dir):
    for name in ("miscdata.zal", "replay.zal", "tables.zal"):     # uncompressed archives
        d = (GAME_DIR / "assets" / name).read_bytes()[:60000]
        z = lzrw3a.compress(d)
        assert lzrw3a.decompress(z, len(d)) == d
        assert len(z) < len(d)


def test_errors():
    with pytest.raises(FormatError):
        lzrw3a.decompress(b"\0\0")
    with pytest.raises(FormatError):
        lzrw3a.decompress(bytes(4) + b"\x01")                 # truncated control word
    with pytest.raises(FormatError):
        lzrw3a.decompress(bytes(4) + b"\x01\x00\x05")         # truncated copy item
    with pytest.raises(FormatError):
        lzrw3a.decompress(bytes([1, 0, 0, 0]) + b"ab", expected=3)


def test_fuzz():
    fuzz_parser(lzrw3a.decompress, lzrw3a.compress(b"hello world, hello zal " * 40), n=300, seed=0)


def test_zal_method1_roundtrip(game_dir):
    arc = zal.read((GAME_DIR / "assets" / "motion.zal").read_bytes())
    assert arc.method == zal.METHOD_LZO1X
    arc.count_word &= ~zal.COUNT_METHOD_BIT                    # switch the archive to method 1
    assert arc.method == zal.METHOD_LZRW3A
    payload = (GAME_DIR / "assets" / "tables.zal").read_bytes()[:20000]
    gid = arc.entries[0].id
    arc.set_data(gid, payload)
    back = zal.read(arc.write())
    assert back.method == zal.METHOD_LZRW3A
    assert back.asset_data(back.find(gid).assets[0]) == payload

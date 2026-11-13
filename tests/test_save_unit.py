"""SAVE.DAT unit tests on synthetic data (no game files needed)."""
import struct

import pytest

from dmbmx.core.errors import BadMagicError, ChecksumError, TruncatedError
from dmbmx.formats import save
from dmbmx.integrity.crc import check_save, resign_save


def synthetic():
    data = bytearray(save.SIZE)
    struct.pack_into("<II", data, 0, save.MAGIC, save.SIZE)
    data[0x0C:0x10] = bytes([18, 18, 1, 10])                 # init defaults (0x00407C10)
    o = save.SCORES_OFF
    struct.pack_into("<IBBB", data, o, 1234, 0, 0, 0)
    data[o + 7:o + 12] = b"Alice"
    data[save.PROFILES_OFF + 7:save.PROFILES_OFF + 13] = b"player"
    return bytes(data)


def test_layout_constants_cover_file():
    end_a = save.LEVELS_OFF + save.LEVEL_COUNT * save.LEVEL_LEN
    assert end_a == save.BLOCK_A_TAIL_OFF
    assert save.BLOCK_A_TAIL_OFF + save.BLOCK_A_TAIL_LEN == save.SCORES_OFF == 8 + 0x30F * 4
    assert save.SCORES_OFF + 12 * 24 * 20 == save.PROFILES_OFF == save.SCORES_OFF + 0x5A0 * 4
    assert save.PROFILES_OFF + 2 * save.PROFILE_LEN == save.SIZE == save.PROFILES_OFF + 0x26 * 4


def test_roundtrip_and_fields():
    data = synthetic()
    s = save.read(data)
    assert save.write(s) == data
    assert s.scores[0][0].score == 1234 and s.scores[0][0].name == "Alice"
    assert s.profiles[0].name == "player"
    assert (s.volume_0c, s.volume_0d) == (18, 18)


def test_edit_name_and_score():
    s = save.read(synthetic())
    s.scores[0][0].score = 999999
    s.profiles[0].name = "Test1"
    out = save.write(s)
    s2 = save.read(out)
    assert s2.scores[0][0].score == 999999 and s2.profiles[0].name == "Test1"
    with pytest.raises(ValueError):
        s.profiles[0].name = "x" * 40
    with pytest.raises(ValueError):
        s.scores[0][0].name = "x" * 13


def test_errors():
    data = synthetic()
    with pytest.raises(TruncatedError):
        save.read(data[:-1])
    with pytest.raises(TruncatedError):
        save.read(b"")
    bad_size = bytearray(data); bad_size[4] ^= 1
    with pytest.raises(ChecksumError):
        save.read(bytes(bad_size))
    assert save.read(bytes(bad_size), lenient=True).size_field == save.SIZE ^ 1
    bad_magic = bytearray(data); bad_magic[0] ^= 1
    with pytest.raises(BadMagicError):
        save.read(bytes(bad_magic))


def test_check_mirrors_exe():
    data = synthetic()
    assert check_save(data) == (True, "ok")
    assert check_save(data + b"junk")[0]                  # extra bytes ignored by the 0x235C read
    assert not check_save(data[:-1])[0]                   # short read -> code 9
    bad = bytearray(data); struct.pack_into("<I", bad, 4, save.SIZE + 1)
    assert not check_save(bytes(bad))[0]                  # code 8
    bad_magic = bytearray(data); bad_magic[0:4] = b"XXXX"
    assert check_save(bytes(bad_magic))[0]                # magic is not checked by the game


def test_resign():
    data = synthetic()
    assert resign_save(data) == data
    broken = bytearray(data); struct.pack_into("<II", broken, 0, 0, 0)
    fixed = resign_save(bytes(broken) + b"tail")
    assert fixed == data and check_save(fixed)[0]
    with pytest.raises(TruncatedError):
        resign_save(data[:100])

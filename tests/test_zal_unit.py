"""ZAL / LZO / hash unit tests on synthetic bytes (no game data needed)."""
import struct

import pytest

from dmbmx.core import lzo
from dmbmx.core.errors import BadMagicError, FormatError, TruncatedError
from dmbmx.core.hash import zhash
from dmbmx.formats import zal


def build(entries, flags=0x0001, count_hi=0, pad=b""):
    """Uncompressed archive: entries = [(id, payload)], payloads back-to-back after `pad`."""
    n = len(entries)
    out = bytearray(zal.MAGIC_BYTES + bytes(12) + struct.pack("<HH", flags, count_hi | n))
    table = len(out)
    out += bytes(12 * n)
    out += pad
    for i, (gid, payload) in enumerate(entries):
        struct.pack_into("<3I", out, table + 12 * i, gid, len(out) - 0x10, len(payload))
        out += payload
    return bytes(out)


def test_roundtrip_synthetic():
    data = build([(0, b"FORM\0\0\0\0"), (7, b"abc"), (0x7531, b"xyz!")], pad=b"\xcd\xcd")
    arc = zal.read(data)
    assert [e.id for e in arc.entries] == [0, 7, 0x7531]
    assert arc.entries[1].data() == b"abc"
    assert arc.entries[0].type == b"FORM"
    assert arc.entries[0].assets[0].pad == b"\xcd\xcd"
    assert arc.write() == data


def test_relayout_after_edit():
    arc = zal.read(build([(0, b"abc"), (1, b"defg")]))
    a = arc.entries[0].assets[0]
    a.stored, a.size_field = b"hello", 5
    arc.entries[1].assets[0].pad = None          # let the writer align it
    again = zal.read(arc.write())
    assert again.entries[0].data() == b"hello"
    assert again.entries[1].data() == b"defg"
    assert (again.entries[1].assets[0].offset + 0x10) % 4 == 0


def test_tail_preserved():
    data = build([(0, b"abc")]) + b"\xcd"
    arc = zal.read(data)
    assert arc.tail == b"\xcd" and arc.write() == data


def test_header_bits():
    arc = zal.read(zal.MAGIC_BYTES + bytes(12) + struct.pack("<HH", 0x8001, 0x4000))
    assert arc.compressed and arc.method == zal.METHOD_LZO1X and arc.record_size == 16
    arc = zal.read(zal.MAGIC_BYTES + bytes(12) + struct.pack("<HH", 0x8001, 0x0000))
    assert arc.method == zal.METHOD_1
    arc = zal.read(zal.MAGIC_BYTES + bytes(12) + struct.pack("<HH", 0x0003, 0x0000))
    assert arc.assets_per_group == 3 and arc.record_size == 4 + 3 * 8


@pytest.mark.parametrize("bad,exc", [
    (b"", TruncatedError),
    (b"\0" * 20, BadMagicError),
    (zal.MAGIC_BYTES + bytes(12) + struct.pack("<HH", 1, 5), TruncatedError),
])
def test_errors(bad, exc):
    with pytest.raises(exc):
        zal.read(bad)


def test_overlap_rejected():
    data = bytearray(build([(0, b"abcd"), (1, b"efgh")]))
    struct.pack_into("<I", data, 0x14 + 12 + 4, 0)   # second asset offset -> inside the table
    with pytest.raises(FormatError):
        zal.read(bytes(data))


def test_lzo_handmade():
    # 4 literals, then a match (len 4, dist 4), then the end-of-stream marker
    stream = bytes([17 + 4]) + b"abcd" + bytes([0x6C, 0x00]) + b"\x11\x00\x00"
    assert lzo.decompress(stream) == b"abcdabcd"
    assert lzo.decompress(stream, 8) == b"abcdabcd"
    with pytest.raises(lzo.LzoError):
        lzo.decompress(stream, 7)
    with pytest.raises(lzo.LzoError):
        lzo.decompress(stream[:-1])


def test_hash_vectors():
    # [V] these values occur as u32 in every rider .zmd (see test_hash_corpus.py)
    assert zhash("Hips") == 0x00530316
    assert zhash("Head") == 0x004402A5
    assert zhash("LeftFoot") == 0x00540AF9
    assert zhash("hips") == zhash("HIPS") == zhash(b"Hips")   # case-insensitive (toupper)
    assert zhash("") == 0

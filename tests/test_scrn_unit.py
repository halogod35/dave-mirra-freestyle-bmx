"""SCRN / GUI group codec on synthetic data (no game files): layout, errors, editing."""
import struct

import pytest

from dmbmx.core.errors import BadMagicError, FormatError, LimitError, TruncatedError
from dmbmx.formats import scrn as S


def edef(eid, text=0, etype=1):
    raw = bytearray(S.EDEF_SIZE)
    raw[:4] = b"EDEF"
    struct.pack_into("<IHH", raw, 4, eid, 0, etype)
    struct.pack_into("<I", raw, 0xB4, text)
    return bytes(raw)


def screen(texts):
    """a SCRN with one EDEF per text (None = no text), STRS laid out like the shipped files"""
    strs = bytearray(b"STRS")
    edefs = []
    for k, t in enumerate(texts):
        off = 0
        if t is not None:
            off = len(strs)
            strs += t.encode() + b"\0"
        edefs.append(edef(0xA0010001 + k, off))
    strs += b"\0" * (-len(strs) % 4)
    n = len(edefs)
    size = S.HEADER + n * S.EDEF_SIZE + len(strs)
    head = bytearray(S.HEADER)
    head[:4] = b"SCRN"
    struct.pack_into("<IIHHiiii", head, 4, size, 0xA001, 0, 0, n, S.HEADER,
                     sum(t is not None for t in texts), S.HEADER + n * S.EDEF_SIZE)
    return bytes(head) + b"".join(edefs) + bytes(strs)


def test_screen_roundtrip_and_texts():
    raw = screen(["HELLO", None, "WORLD"])
    s = S.read_screen(raw)
    assert s.write() == raw
    assert [(k, t) for k, _, t, _ in s.texts()] == [(0, "HELLO"), (2, "WORLD")]
    s.rebuild_strs()
    assert s.write() == raw
    s.set_text(1, "NEW")
    s2 = S.read_screen(s.write())
    assert [t for _, _, t, _ in s2.texts()] == ["HELLO", "NEW", "WORLD"]
    assert s2.header["text_count"] == 3 and len(s2.strs) % 4 == 0


def test_group_roundtrip():
    a, b = screen(["X"]), b"\x01\x00\x00\x00spritebytes"
    g = S.Group([(0xA001, a), (0x4001, b)])
    data = g.write()
    assert struct.unpack_from("<III", data) == (2, 0xA001, 20)
    g2 = S.read_group(data)
    assert g2.entries == g.entries and g2.screens() == [0xA001]
    assert S.asset_type(0x4001) == "sprite" and S.asset_type(0x8002) == "font"


def test_errors():
    raw = screen(["A"])
    with pytest.raises(BadMagicError):
        S.read_screen(b"XXXX" + raw[4:])
    with pytest.raises(TruncatedError):
        S.read_screen(raw[:-1])
    with pytest.raises(TruncatedError):
        S.read_screen(raw[:10])
    bad = bytearray(raw)
    bad[S.HEADER:S.HEADER + 4] = b"NOPE"
    with pytest.raises(BadMagicError):
        S.read_screen(bytes(bad))
    bad = bytearray(raw)
    struct.pack_into("<i", bad, 0x10, 101)
    with pytest.raises(LimitError):
        S.read_screen(bytes(bad))
    bad = bytearray(raw)
    struct.pack_into("<I", bad, S.HEADER + 0xB4, 0x7FFF)       # text offset outside STRS
    with pytest.raises(FormatError):
        S.read_screen(bytes(bad))
    with pytest.raises(TruncatedError):
        S.read_group(b"\x02\x00\x00\x00")
    with pytest.raises(LimitError):
        S.read_group(struct.pack("<I", 0x100000))
    with pytest.raises(FormatError):                             # not back to back
        S.read_group(struct.pack("<III", 1, 0xA001, 99) + b"x")

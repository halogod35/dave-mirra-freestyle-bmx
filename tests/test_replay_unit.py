"""dmbmx.formats.replay on synthetic records (no game data)."""
import struct

import pytest

from dmbmx.core.errors import FormatError
from dmbmx.formats import replay as R
from fuzz import fuzz_parser


def make_pc(inputs, data_size=0x200, flags=R.NO_RIDER):
    """A PC record as the recorder lays it out (bmxreplayCreate + bmxreplayUpdate + SetMode(off))."""
    evs = R.record(inputs)
    stream = R.encode_events(evs)
    stream += bytes(range(256)) * (data_size // 256 + 1)
    stream = stream[:data_size]
    off, f = R.HEADER + data_size, {}
    for bit, names, size in ((R.NO_RIDER, ("rider_start", "rider_live"), R.RIDER_SIZE),
                             (R.NO_PROPS, ("props_start", "props_live"), R.PROPS_SIZE),
                             (R.NO_GOALS, ("goals_start", "goals_live"), R.GOALS_SIZE)):
        if flags & bit:
            f[names[0]] = f[names[1]] = 0
        else:
            f[names[0]], f[names[1]] = off, off + size
            off += 2 * size
    blocks = bytes((i * 7) & 0xFF for i in range(off - R.HEADER - data_size))
    hdr = struct.pack("<12I4s", 2, len(inputs), f["rider_start"], f["rider_live"], f["props_start"],
                      f["props_live"], f["goals_start"], f["goals_live"], R.HEADER, data_size, 0x12345678,
                      inputs[-1][0] if inputs else 0, b"\0" * 4)
    return hdr + stream + blocks, evs


INPUTS = ([(0, ((0, 0), (0, 0)))] * 5 + [(0x80, ((0, 0), (0, 0)))] * 3 + [(0x81, ((127, -127), (0, 0)))] * 40 +
          [(0, ((0, 0), (5, 6)))] * 70 + [(0x10, ((0, 0), (0, 0)))])


def test_roundtrip_and_playback():
    data, evs = make_pc(INPUTS)
    r = R.read(data)
    assert r.layout == "pc" and r.write() == data
    assert r.flags() == R.NO_RIDER and r.events() == evs
    assert sum(e.delta for e in evs) == len(INPUTS) + 1
    frames = r.play()
    assert [(f.buttons, f.sticks) for f in frames] == INPUTS
    assert r.block("props_start") == r.blocks[:R.PROPS_SIZE] and r.block("rider_start") is None
    assert len(r.slack) == len(r.stream) - 3 * len(evs)


def test_keepalive_and_closing_events():
    evs = R.record([(0, ((0, 0), (0, 0)))] * 70)
    assert evs == [R.Event(31, 0, 0), R.Event(31, 0, 0), R.Event(9, 0, 0)]


@pytest.mark.parametrize("flags", [0, 1, 2, 3, 4, 5, 6, 7])
def test_all_block_combinations(flags):
    data, _ = make_pc(INPUTS, 0x100, flags)
    r = R.read(data)
    assert r.write() == data and r.flags() == flags


def test_psx_and_raw_layouts():
    hdr = bytearray(R.PSX_HEADER)
    struct.pack_into("<I", hdr, 0x1C, R.PSX_HEADER)
    struct.pack_into("<I", hdr, 0x20, 0x30)
    struct.pack_into("<II", hdr, 0x24, 0x80001000, 0x80001009)
    data = bytes(hdr) + bytes(0x30)
    r = R.read(data)
    assert r.layout == "psx" and r.write() == data and r.used_length() == 9
    assert R.read(b"\1\2\3").layout == "raw"


def test_errors():
    data, _ = make_pc(INPUTS)
    bad = bytearray(data)
    struct.pack_into("<I", bad, 0x10, 0x999)              # props_start not where the layout puts it
    with pytest.raises(FormatError):
        R.read(bytes(bad))
    with pytest.raises(FormatError):
        R.read(data[:-1])
    bad = bytearray(data)
    struct.pack_into("<I", bad, 0x04, 5000)                # more frames than events
    with pytest.raises(FormatError):
        R.read(bytes(bad)).events()
    with pytest.raises(ValueError):
        R.encode_events([R.Event(32, 0, 0)])


def parse_all(data):
    r = R.read(data)
    if r.layout in ("pc", "psx"):
        r.used_length()
    if r.layout == "pc":
        r.play()
    return r


def test_fuzz_synthetic():
    data, _ = make_pc(INPUTS, 0x80, R.NO_RIDER | R.NO_PROPS)
    fuzz_parser(parse_all, data, n=300, seed=0)

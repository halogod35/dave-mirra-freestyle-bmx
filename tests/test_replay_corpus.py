"""replay.zal: every record round-trips byte for byte, layouts as documented (REPLAY.md)."""
import pytest

from conftest import corpus_files
from dmbmx.formats import replay as R
from dmbmx.formats import zal as Z
from fuzz import fuzz_parser


def records():
    files = [p for p in corpus_files(".zal") if p.name.lower() == "replay.zal"]
    if not files:
        pytest.skip("no corpus")
    assert len(files) == 1
    return {e.id: e.data() for e in Z.read(files[0].read_bytes()).entries}


def test_replay_records_roundtrip():
    recs = records()
    assert sorted(recs) == list(range(24))
    layouts = {}
    for rid, data in recs.items():
        r = R.read(data)
        assert r.write() == data, rid
        layouts.setdefault(r.layout, []).append(rid)
        if r.layout == "psx":
            assert r.used_length() % 3 == 0
    assert layouts == {"pc": [0, 1, 2, 3, 4, 8, 10], "raw": [9],
                       "psx": [5, 6, 7] + list(range(11, 24))}


def test_replay_pc_records_decode():
    for rid, data in records().items():
        r = R.read(data)
        if r.layout != "pc":
            continue
        assert len(data) == 0x2702 and r.fields["data_size"] == 0x1000
        evs = r.events()
        assert all(e.kind == R.EV_BUTTONS for e in evs)          # no analog stick in these recordings
        assert evs[-1].value == 0                                # the closing event
        assert len(r.play()) == r.fields["frames"]


def test_replay_fuzz():
    recs = records()
    for rid in (13, 8):                                          # smallest stream / an attract-demo record
        fuzz_parser(lambda d: _parse(d), recs[rid], n=200, seed=0)


def _parse(data):
    r = R.read(data)
    if r.layout in ("pc", "psx"):
        r.used_length()
    if r.layout == "pc":
        r.play()
    return r

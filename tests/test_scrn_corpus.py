"""Intwrap.zal GUI groups and SCRN screens: byte-for-byte round trip of all 35 groups and 34 screens,
the STRS layout rule, and the facts docs/formats/ZAL.md 3.2 states."""
import struct

import pytest

from conftest import corpus_files
from dmbmx.formats import scrn as S
from dmbmx.formats import zal as Z
from fuzz import fuzz_parser


@pytest.fixture(scope="module")
def intwrap():
    files = [p for p in corpus_files(".zal") if p.name.lower() == "intwrap.zal"]
    if not files:
        pytest.skip("no corpus")
    assert len(files) == 1
    return files[0].read_bytes()


@pytest.fixture(scope="module")
def payloads(intwrap):
    inner = Z.ZalArchive.read(Z.ZalArchive.read(intwrap).find(0).data())
    return {e.id: e.data() for e in inner.entries}


def test_groups_roundtrip(payloads):
    assert sorted(payloads) == list(range(0xE000, 0xE023))
    kinds = {}
    for gid, p in payloads.items():
        g = S.read_group(p)
        assert g.write() == p, hex(gid)
        for aid, d in g.entries:
            kinds[S.asset_type(aid)] = kinds.get(S.asset_type(aid), 0) + 1
    assert kinds == {"screen": 34, "sprite": 260, "picture": 12, "font": 6}


def test_screens_roundtrip_and_layout(payloads):
    n = 0
    for gid, p in payloads.items():
        g = S.read_group(p)
        screens = g.screens()
        # group 0xE000 (fonts, shared sprites) has no screen; every other group holds 0xA000 | n
        assert screens == ([] if gid == 0xE000 else [0xA000 | (gid & 0xFFF)]), hex(gid)
        for sid in screens:
            raw = g.asset(sid)
            s = S.read_screen(raw)
            assert s.write() == raw, hex(sid)
            assert s.header["id"] == sid and s.header["size"] == len(raw)
            assert len(s.strs) % 4 == 0
            # the strings are the EDEF texts in EDEF order: rebuilding gives the same bytes
            s.rebuild_strs()
            assert s.write() == raw, hex(sid)
            for e in s.edefs:
                assert e.type == 1 and e.id >> 16 == sid       # every shipped element is a sprite / text
            n += 1
    assert n == 34


def test_screen_contents(payloads):
    scr = {sid: S.read_screen(S.read_group(p).asset(sid))
           for p in payloads.values() for sid in S.read_group(p).screens()}
    counts = {sid: len(s.edefs) for sid, s in scr.items() if s.edefs}
    assert counts == {0xA005: 16, 0xA009: 34, 0xA00C: 76, 0xA00D: 23, 0xA012: 20, 0xA013: 2, 0xA014: 6}
    texts = {t for s in scr.values() for _, _, t, _ in s.texts() if t}
    assert {"MEMCARD OPERATION", "SAVE - MEMORY CARD SLOT 1", "String to be preallocated"} <= texts
    # backdrops named in the header (guiLoadBackdrop)
    assert {sid: s.header["backdrop"] for sid, s in scr.items() if s.header["backdrop"]} == {0xA008: 0xA, 0xA00B: 6}


def test_set_text_edits_one_string(payloads):
    g = S.read_group(payloads[0xE012])
    s = S.read_screen(g.asset(0xA012))
    before = s.texts()
    k = next(k for k, _, t, _ in before if t == "MEMCARD OPERATION")
    s.set_text(k, "MENU MOD")
    raw = s.write()
    s2 = S.read_screen(raw)
    after = {k2: t for k2, _, t, _ in s2.texts()}
    assert after[k] == "MENU MOD"
    assert all(after[k2] == t for k2, _, t, _ in before if k2 != k)
    g.set_asset(0xA012, raw)
    assert S.read_group(g.write()).asset(0xA012) == raw


def test_intwrap_still_roundtrips(intwrap):
    """the GUI codec reads the nested archive in place; Intwrap.zal itself must keep round-tripping"""
    assert Z.ZalArchive.read(intwrap).write() == intwrap
    groups = S.read_intwrap(intwrap)
    assert len(groups) == 35


def test_scrn_fuzz(payloads):
    g = S.read_group(payloads[0xE013])
    small = g.asset(0xA013)
    big = S.read_group(payloads[0xE009]).asset(0xA009)
    for data in (small, big):
        fuzz_parser(S.read_screen, data, n=200, seed=0)
    smallest = min(payloads.values(), key=len)
    fuzz_parser(S.read_group, smallest, n=200, seed=0)
    assert struct.unpack_from("<I", smallest)[0] >= 1

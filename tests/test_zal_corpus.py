"""ZAL corpus tests (DM-010): byte-identical round-trip of every .zal, nested archives, LZO payloads."""
import pytest
from conftest import corpus_files, corpus_params

from dmbmx.formats import zal


def _first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


@pytest.mark.parametrize("path", corpus_params(".zal"))
def test_zal_roundtrip(path):
    data = path.read_bytes()
    out = zal.read(data).write()
    assert out == data, f"first difference at 0x{_first_diff(out, data):x}"


@pytest.mark.parametrize("path", corpus_params(".zal"))
def test_zal_payloads_and_nested(path):
    """Every asset decodes to exactly its declared size; nested ZALs round-trip too."""
    arc = zal.read(path.read_bytes())
    assert arc.assets_per_group == 1
    assert not arc.ref_mode
    for e in arc.entries:
        payload = e.data()
        assert len(payload) == e.assets[0].size
        if zal.is_zal(payload):
            sub = zal.read(payload)
            assert sub.write() == payload
            for se in sub.entries:
                assert len(se.data()) == se.assets[0].size


def test_zal_corpus_shape(game_dir):
    """Pinned facts from the survey (docs/formats/ZAL.md)."""
    files = {p.name.lower(): p for p in corpus_files(".zal")}
    assert len(files) == 6
    expect = {  # name: (groups, compressed, method)
        "intwrap.zal": (5, False, 0),
        "material.zal": (1262, True, zal.METHOD_LZO1X),
        "miscdata.zal": (21, False, 0),
        "motion.zal": (1, True, zal.METHOD_LZO1X),
        "replay.zal": (24, False, 0),
        "tables.zal": (1, False, 0),
    }
    for name, (groups, comp, method) in expect.items():
        arc = zal.read(files[name].read_bytes())
        assert (len(arc.entries), arc.compressed, arc.method) == (groups, comp, method), name
        assert arc.reserved == bytes(12)
    mat = zal.read(files["material.zal"].read_bytes())
    assert all(e.type[:2] == b"BM" for e in mat.entries)
    assert [e.id for e in mat.entries] == list(range(1262))

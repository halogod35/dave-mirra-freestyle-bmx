"""Extract -> pack round-trip, asset replacement, and the LZO1X encoder over the corpus."""
import json
import struct

import pytest
from conftest import corpus_files, corpus_params

from dmbmx.cli import main
from dmbmx.core import lzo
from dmbmx.formats import zal, zal_dir


def _zal(name):
    for p in corpus_files(".zal"):
        if p.name.lower() == name:
            return p
    pytest.skip(f"no {name} in corpus")


@pytest.mark.parametrize("path", corpus_params(".zal"))
def test_extract_pack_identical(path, tmp_path):
    data = path.read_bytes()
    d = tmp_path / "x"
    zal_dir.extract_dir(zal.read(data), d, recursive=True)
    report = []
    assert zal_dir.pack_dir(d, report=report) == data
    assert report and all(r[2] == "kept" for r in report)


@pytest.mark.parametrize("path", corpus_params(".zal"))
def test_encoder_roundtrip_corpus(path):
    """compress -> decompress is the identity for every compressed payload (nested included)."""
    def walk(arc):
        for e in arc.entries:
            d = e.data()
            if arc.compressed:
                z = lzo.compress(d)
                assert lzo.decompress(z, len(d)) == d, f"id 0x{e.id:x}"
            if zal.is_zal(d):
                walk(zal.read(d))
    walk(zal.read(path.read_bytes()))


def _invert(bmp):
    b = bytearray(bmp)
    hs = struct.unpack_from("<I", b, 14)[0]
    for p in range(14 + hs, 14 + hs + 16 * 4, 4):
        b[p:p + 3] = bytes(255 - x for x in b[p:p + 3])
    return bytes(b)


def test_pack_edited_material(tmp_path):
    src = _zal("material.zal").read_bytes()
    orig = zal.read(src)
    d = tmp_path / "m"
    zal_dir.extract_dir(orig, d)
    f = d / "0000000a.bmp"
    new = _invert(f.read_bytes())
    f.write_bytes(new)
    report = []
    out = zal_dir.pack_dir(d, report=report)
    assert [r[1] for r in report if r[2] != "kept"] == [10]
    arc = zal.read(out)
    assert arc.find(10).data() == new
    for a, b in zip(orig.entries, arc.entries):
        if a.id != 10:
            assert a.raw == b.raw                     # untouched streams kept verbatim
    assert all((e.assets[0].offset + 0x10) % 4 == 0 for e in arc.entries)


def test_replace_nested_and_cli(tmp_path):
    p = _zal("intwrap.zal")
    src = p.read_bytes()
    inner0 = zal.read(zal.read(src).find(0).data())
    target = inner0.find(0xE009).data()
    edited = target[:-4] + b"DM23"
    pf = tmp_path / "payload.bin"
    pf.write_bytes(edited)
    out = tmp_path / "Intwrap.zal"
    assert main(["zal", "replace", str(p), "0/0xe009", str(pf), str(out)]) == 0
    arc = zal.read(out.read_bytes())
    inner = zal.read(arc.find(0).data())
    assert inner.find(0xE009).data() == edited
    for a, b in zip(inner0.entries, inner.entries):
        if a.id != 0xE009:
            assert a.raw == b.raw
    for gid in (1, 2, 3, 4):
        assert arc.find(gid).data() == zal.read(src).find(gid).data()


def test_cli_extract_pack(tmp_path, capsys):
    p = _zal("miscdata.zal")
    d = tmp_path / "md"
    assert main(["zal", "extract", "-r", str(p), str(d)]) == 0
    meta = json.loads((d / "_index.json").read_text())
    assert meta["format"] == zal_dir.FORMAT_TAG and meta["tail"] == "cd"
    out = tmp_path / "md.zal"
    assert main(["zal", "pack", str(d), str(out)]) == 0
    assert out.read_bytes() == p.read_bytes()
    assert "0 re-encoded" in capsys.readouterr().out


def test_cli_refuses_install(game_dir, tmp_path):
    p = _zal("tables.zal")
    d = tmp_path / "t"
    zal_dir.extract_dir(zal.read(p.read_bytes()), d)
    with pytest.raises(SystemExit):
        main(["zal", "pack", str(d), str(game_dir / "assets" / "tables_repack.zal")])
    with pytest.raises(SystemExit):
        main(["zal", "replace", str(p), "0", str(d / "00000000.iff"), str(game_dir / "assets" / "t2.zal")])
    assert not (game_dir / "assets" / "tables_repack.zal").exists()
    assert not (game_dir / "assets" / "t2.zal").exists()

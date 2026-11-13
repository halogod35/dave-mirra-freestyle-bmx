"""PNG CRC helper, ZSD SCFI sizes, exe patcher, integrity CLI."""
import hashlib
import json
import struct
import zlib

import pytest

from conftest import GAME_DIR, corpus_files
from fuzz import fuzz_parser
from dmbmx.cli import main
from dmbmx.core.errors import FormatError
from dmbmx.integrity import png, zsd
from dmbmx.patch import patcher


# ---------------------------------------------------------------- PNG
def _chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def tiny_png(text=True):
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    body = _chunk(b"IHDR", ihdr) + (_chunk(b"tEXt", b"k\0v") if text else b"") + _chunk(b"IDAT", zlib.compress(b"\0\0"))
    return png.SIG + body + _chunk(b"IEND", b"")


def test_png_ok_and_fix():
    p = b"junk" + tiny_png() + b"tail"
    assert png.check(p) == [] and png.fix(p) == p
    types = [t for _o, t, *_ in png.chunks(p, 4)]
    assert types == ["IHDR", "tEXt", "IDAT", "IEND"]


@pytest.mark.parametrize("ctype,critical", [("IDAT", True), ("tEXt", False), ("IHDR", True)])
def test_png_bad_crc_detected_and_fixed(ctype, critical):
    good = tiny_png()
    o = next(o for o, t, *_ in png.chunks(good) if t == ctype)
    n = struct.unpack_from(">I", good, o)[0]
    bad = bytearray(good)
    bad[o + 8 + n] ^= 0x55
    res = png.check(bytes(bad))
    assert len(res) == 1 and res[0][2] == ctype and res[0][5] is critical
    assert png.fix(bytes(bad)) == good


def test_png_malformed():
    with pytest.raises(FormatError):
        list(png.chunks(b"not a png"))
    with pytest.raises(FormatError):
        list(png.chunks(tiny_png()[:20]))
    assert png.check(png.SIG + b"\0\0\0\xff")[0][1] is None     # reported, not raised
    fuzz_parser(lambda b: list(png.chunks(b)) if b[:8] == png.SIG else png.check(b), tiny_png(), n=300, seed=0)


def test_png_corpus_crcs_all_valid(game_dir):
    files = [p for ext in (".zmd", "._sd") for p in corpus_files(ext)]
    with_png = [p for p in files if png.SIG in p.read_bytes()]
    assert len(with_png) >= 50
    for p in with_png:
        assert png.check(p.read_bytes()) == [], p


def test_png_fuzz_real(game_dir):
    data = (GAME_DIR / "assets" / "DMbike01.zmd").read_bytes()
    p0 = png.find(data)[0]
    end = max(o + 12 + n for o, _t, n, *_ in png.chunks(data, p0))
    fuzz_parser(lambda b: list(png.chunks(b)), data[p0:end], n=200, seed=1)


# ---------------------------------------------------------------- ZSD
def synth_zsd(files):
    body = _c(b"SCDF", b"\0" * 0x10)
    for typ, size, name in files:
        e = bytearray(0x28)
        e[0] = typ
        struct.pack_into("<I", e, 4, size)
        e[8:8 + len(name)] = name
        body += _c(b"SCFI", bytes(e))
    return _c(b"SCEN", body)


def _c(tag, data):
    return tag + struct.pack("<I", len(data)) + data


def test_zsd_entries_and_resign(tmp_path):
    (tmp_path / "BSP._sd").write_bytes(b"x" * 100)
    (tmp_path / "Mat._sd").write_bytes(b"y" * 7)
    z = tmp_path / "L.ZSD"
    z.write_bytes(synth_zsd([(4, 100, b"BSP._sd"), (1, 9, b"Mat._sd"), (3, 5, b"Gone._sd")]))
    res = zsd.check(z)
    assert [(e.name, real, ok) for e, real, ok in res] == [("BSP._sd", 100, True), ("Mat._sd", 7, False), ("Gone._sd", None, False)]
    new = zsd.resign_file(z)
    assert [(e.name, e.size) for e in zsd.entries(new)] == [("BSP._sd", 100), ("Mat._sd", 7), ("Gone._sd", 5)]
    assert len(new) == len(z.read_bytes())
    with pytest.raises(FormatError):
        zsd.entries(b"SCEX\0\0\0\0")
    with pytest.raises(FormatError):
        zsd.entries(_c(b"SCEN", _c(b"SCFI", b"\0" * 8)))      # SCFI too short
    with pytest.raises(FormatError):
        zsd.entries(b"SCEN\xff\0\0\0")


def test_zsd_corpus(game_dir):
    zs = corpus_files(".zsd")
    level = [p for p in zs if p.parent.parent.name.lower() == "assets"]
    assert len(level) == 12
    n = 0
    for z in level:
        for e, real, ok in zsd.check(z):
            assert ok, (z, e)
            n += 1
        assert zsd.resign_file(z) == z.read_bytes()             # no-op on the stock files
    assert n == 72
    fuzz_parser(zsd.entries, level[0].read_bytes(), n=300, seed=0)


# ---------------------------------------------------------------- patcher
def test_patch_spec_consistent():
    spec = patcher.load()
    ids = {p["id"] for p in spec["patches"]}
    assert ids >= {"save-size-check", "assert-continue"}
    assert all(set(g) <= ids for g in spec["groups"].values())
    assert spec["groups"]["nocrc"] == ["save-size-check"]


def test_patch_real_exe(game_dir, tmp_path):
    exe = GAME_DIR / "NgBMX.exe"
    data = exe.read_bytes()
    spec = patcher.load()
    assert hashlib.sha256(data).hexdigest() == spec["sha256"]
    for p in spec["patches"]:
        assert data[p["off_i"]:p["off_i"] + len(p["orig_b"])] == p["orig_b"], p["id"]
    out = tmp_path / "NgBMX_nocrc.exe"
    patcher.apply_file(exe, out, group="nocrc")
    new = out.read_bytes()
    diff = [i for i in range(len(data)) if data[i] != new[i]]
    assert diff == [0x121FD, 0x121FE] and new[0x121FD:0x121FF] == b"\x90\x90"
    with pytest.raises(patcher.PatchError):                     # not pristine any more
        patcher.apply_file(out, tmp_path / "again.exe", group="nocrc")
    with pytest.raises(patcher.PatchError):
        patcher.apply_file(exe, exe, group="nocrc")
    (tmp_path / "_pristine").mkdir()
    with pytest.raises(patcher.PatchError):
        patcher.apply_file(exe, tmp_path / "_pristine" / "x.exe", group="nocrc")
    with pytest.raises(patcher.PatchError):
        patcher.apply_file(exe, tmp_path / "y.exe", ids=["nope"])


def test_patch_orig_mismatch(tmp_path):
    data = bytearray(0x20000)
    spec = {"exe": "x", "sha256": hashlib.sha256(bytes(data)).hexdigest(), "groups": {"g": ["a"]},
            "patches": [{"id": "a", "va": "0x004121fd", "file_offset": "0x121fd", "orig": "7542", "new": "9090",
                         "reason": "t"}]}
    sp = tmp_path / "p.json"
    sp.write_text(json.dumps(spec))
    loaded = patcher.load(sp)
    with pytest.raises(patcher.PatchError, match="expected 7542"):
        patcher.apply_bytes(bytes(data), patcher.select(loaded, group="g"), loaded)
    data[0x121FD:0x121FF] = b"\x75\x42"
    loaded["sha256"] = hashlib.sha256(bytes(data)).hexdigest()
    assert patcher.apply_bytes(bytes(data), patcher.select(loaded, group="g"), loaded)[0x121FD:0x121FF] == b"\x90\x90"


# ---------------------------------------------------------------- CLI
def test_cli(tmp_path, capsys):
    f = tmp_path / "x.bin"
    good = b"AA" + tiny_png()
    bad = bytearray(good)
    bad[-1] ^= 1                                                    # IEND CRC
    f.write_bytes(bytes(bad))
    assert main(["integrity", "check", str(f)]) == 1
    assert main(["integrity", "fix-png", str(f), "-o", str(tmp_path / "y.bin")]) == 0
    assert (tmp_path / "y.bin").read_bytes() == good
    assert main(["integrity", "check", str(tmp_path / "y.bin")]) == 0
    assert main(["patch", "list"]) == 0
    assert "save-size-check" in capsys.readouterr().out

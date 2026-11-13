"""DM-060: tools/3dsmax/parser/dmparse.py (independent reference parser) must agree with dmbmx on the
whole corpus: counts, positions, normals, UVs, colours, indices, skeleton, texture bytes, ZAL
assets, motion samples, level include files, sound banks, SAVE.DAT."""
import io
import json
import math
import struct
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files, corpus_params

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "parser"))
import dmparse  # noqa: E402

from dmbmx.core.hash import zhash  # noqa: E402
from dmbmx.formats import audio, mati, motion, zal, zmd  # noqa: E402


def test_dmparse_is_independent_of_dmbmx():
    src = (REPO / "tools" / "3dsmax" / "parser" / "dmparse.py").read_text(encoding="utf-8")
    assert "dmbmx" not in src.replace("independent of the `dmbmx`", "")
    assert len(src.splitlines()) <= 1450          # phase 1 ~1000 lines; phase 2 levels allowed to grow (Director)


@pytest.mark.parametrize("name", ["Hips", "Head", "LeftFoot", "DAVEMIRRA", "NAME_917_0", "tire", "\xe9t\xe9", ""])
def test_zhash(name):
    assert dmparse.zhash(name) == zhash(name)


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_matches_dmbmx(path):
    data = path.read_bytes()
    a, b = dmparse.read_zmd(data), zmd.read(data)
    if b.empty:
        assert a["empty"] and a["lods"] == []
        return
    assert len(a["lods"]) == len(b.lods)
    for li, (la, lb) in enumerate(zip(a["lods"], b.lods)):
        assert la["mldf"] == list(lb.mldf) and la["rw_version"] == lb.rw_version
        assert len(la["frames"]) == len(lb.clump.frames)
        for fa, fb in zip(la["frames"], lb.clump.frames):
            assert (tuple(fa["right"]), tuple(fa["up"]), tuple(fa["at"]), tuple(fa["pos"]), fa["parent"]) == \
                   (tuple(fb.right), tuple(fb.up), tuple(fb.at), tuple(fb.pos), fb.parent)
            assert fa["bone_id"] == fb.anim_id
        sa, sb = dmparse.skeleton(a, li), b.skeleton(li)
        assert [s["name"] for s in sa] == [s["name"] for s in sb]
        for x, y in zip(sa, sb):
            assert all(math.isclose(x["world"][r][c], y["world"][r][c], abs_tol=1e-9) for r in range(4) for c in range(4))
        meshes = list(b.meshes(li))
        assert len(la["atomics"]) == len(meshes)
        for ai, (aa, ms) in enumerate(zip(la["atomics"], meshes)):
            g = aa["geometry"]
            assert aa["frame"] == ms["frame"]
            assert [tuple(p) for p in g["positions"]] == [tuple(p) for p in ms["positions"]]
            assert (g["normals"] is None) == (ms["normals"] is None)
            if g["normals"] is not None:
                assert [tuple(n) for n in g["normals"]] == [tuple(n) for n in ms["normals"]]
            assert (g["uvs"][0] if g["uvs"] else None) == (list(map(list, ms["uvs"])) if ms["uvs"] else None)
            assert (g["colors"] is None) == (ms["colors"] is None)
            if g["colors"]:
                assert [tuple(c) for c in g["colors"]] == [tuple(c) for c in ms["colors"]]
            assert [v for t in g["triangles"] for v in t[:3]] == ms["indices"]
            assert [t[3] for t in g["triangles"]] == ms["material_ids"]
            assert [s["texture"] for s in g["material_slots"]] == ms["materials"]
            vf = dmparse.vertex_frames(a, li, ai)
            assert vf == ([j[0] for j in ms["joints"]] if ms["joints"] else None)
    assert [(t["name"], t["kind"], t["data"], t["name_id"]) for t in a["textures"]] == \
           [(t.name, t.kind, t.image, t.name_id) for t in b.materials()]
    md = b.mdef
    assert a["mdef"]["name"] == md.name and [e["id"] for e in a["mdef"]["entries"]] == [e.id for e in md.entries]
    from dmbmx.convert.gltf import upright_correction
    assert dmparse.upright_correction(a) == upright_correction(b)


def test_zal_assets_match_dmbmx():
    files = corpus_files(".zal")
    if not files:
        pytest.skip("no corpus")
    n = 0
    for f in files:
        d = f.read_bytes()
        a, b = dmparse.read_zal(d), zal.read(d)
        assert [(g["id"], g["data"]) for g in a["groups"]] == [(e.id, e.data()) for e in b.entries]
        n += len(a["groups"])
    assert n == 1314  # + 35 nested in Intwrap group 0 = 1349 assets


def test_motion_bank_matches_dmbmx():
    f = next((p for p in corpus_files(".zal") if p.name.lower() == "motion.zal"), None)
    if f is None:
        pytest.skip("no corpus")
    d = f.read_bytes()
    clips = dmparse.read_motion_bank(d)
    bank = motion.MotionBank.read(d)
    assert sorted(clips) == bank.ids and len(clips) == 238
    for mid in bank.ids:
        pa = dmparse.sample_motion(clips[mid])
        pb = bank.motion(mid).sample()
        assert len(pa) == len(pb)
        for x, y in zip(pa, pb):
            assert x["rider"] == y.rider and x["bike"] == y.bike
            assert x["rider_trans"] == y.rider_trans and x["bike_trans"] == y.bike_trans
    for name in motion.RIDER_CHANNELS + motion.BIKE_CHANNELS:      # DM-019: F q F + literal arm table
        for e in ((100, -2000, 30000), (0, 0, 0), (-32768, 1234, -999)):
            q = dmparse.channel_rotation(name, e)
            assert all(math.isclose(u, v, abs_tol=1e-12) for u, v in zip(q, motion.channel_rotation(name, e)))


def test_plyr_player_table():
    f = next((p for p in corpus_files(".zal") if p.name.lower() == "miscdata.zal"), None)
    if f is None:
        pytest.skip("no corpus")
    p = dmparse.read_plyr(f.read_bytes())
    assert p["count"] == 60 and p["entry_size"] == 24 and p["plhd_rest"] == [0, 6]
    names = [e["name"] for e in p["entries"]]
    assert [names[r * 5] for r in range(12)] == list(dmparse.RIDERS)
    assert all(e["ref_flags"] == 0x80 for e in p["entries"])
    # [V] the (offset, length) references are contiguous for the 52 distinct entries
    es = p["entries"]
    for x, y in zip(es, es[1:]):
        if y["id"] != x["id"]:
            assert y["ref_offset"] == x["ref_offset"] + x["ref_length"]
    assert [e["id"] for e in es[:5]] == [0x2C, 0x2D, 0x2E, 0x2F, 0x30]


def test_zmap_level_table():
    f = next((p for p in corpus_files(".zal") if p.name.lower() == "tables.zal"), None)
    if f is None:
        pytest.skip("no corpus")
    z = dmparse.read_zmap(f.read_bytes())
    assert z["count"] == 24 and z["record_size"] == 100
    assert (z["levels"][0]["name"], z["levels"][0]["zal"]) == ("ACCLAIM DIRT", "ACCLDIRT.ZAL")


def test_strs_tables():
    f = next((p for p in corpus_files(".zal") if p.name.lower() == "miscdata.zal"), None)
    if f is None:
        pytest.skip("no corpus")
    z = dmparse.read_zal(f.read_bytes())
    tabs = {g["id"]: dmparse.read_strs(g["data"]) for g in z["groups"] if g["data"][:4] == b"STRS"}
    assert len(tabs) == 16
    t = tabs[0x7919]
    assert t["base_id"] == 0 and t["strings"][0x25E] == "LOAD COMPLETE." and t["strings"][-1] == "FILE MAY BE CORRUPTED."
    assert tabs[0x7D01]["base_id"] == 0x1000


def _levels():
    root = GAME_DIR / "assets"
    return sorted(d for d in root.iterdir() if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())) \
        if root.is_dir() else []


def test_levels_zsd_and_mat_sd():
    levels = _levels()
    if not levels:
        pytest.skip("no corpus")
    assert len(levels) == 12
    for lv in levels:
        z = dmparse.read_zsd_level(str(lv))
        assert len(z["files"]) == 6
        assert sorted(f["kind"] for f in z["files"]) == sorted(dmparse.LOAD_ORDER.values())
        assert all(f["exists"] and f["size"] == f["actual_size"] for f in z["files"])  # [V] integrity #9
        b = mati.MatSd.read((lv / "Mat._sd").read_bytes())
        assert [(t["name"], t["kind"], t["data"]) for t in z["textures"]] == \
               [(m.name, m.kind, m.image) for m in b.materials()]


def test_zsnd_banks():
    banks = corpus_files(".zsd")
    banks = [b for b in banks if b.read_bytes()[:4] == b"ZSND"]
    if not banks:
        pytest.skip("no corpus")
    assert len(banks) == 22
    for f in banks:
        d = f.read_bytes()
        a, b = dmparse.read_zsnd(d), audio.read(d)
        assert a["counts"]["C"] == len(b.samples) and a["size"] == len(d)
        assert [d[s["offset"]:s["offset"] + s["size"]] for s in a["samples"]] == list(b.samples)
        assert [x["rate"] for x in a["sample_defs"]] == [x.rate for x in b.sample_defs]


def test_save():
    p = GAME_DIR / "save" / "SAVE.DAT"
    if not p.exists():
        pytest.skip("no save")
    from dmbmx.formats import save
    a, b = dmparse.read_save(p.read_bytes()), save.read(p.read_bytes())
    assert [x["name"] for x in a["profiles"]] == [x.name for x in b.profiles]
    assert a["riders_unlocked"] == p.read_bytes()[0x0F]


def test_rw_bsp_tree():
    levels = _levels()
    if not levels:
        pytest.skip("no corpus")
    d = (levels[0] / "BSP._sd").read_bytes()
    t = dmparse.read_any(d, "BSP._sd")
    assert t["type"] == "bsp" and t["sectors"] and t["slots"]


def test_cli_tree_and_dump(capsys):
    f = next((p for p in corpus_files(".zmd") if p.stem == "DMbike01"), None)
    if f is None:
        pytest.skip("no corpus")
    assert dmparse.main(["tree", str(f)]) == 0
    out = capsys.readouterr().out
    assert "MODL" in out and "Clump" in out and "BinMesh" in out
    assert dmparse.main(["dump", str(f)]) == 0
    js = json.loads(capsys.readouterr().out)
    assert js["type"] == "zmd" and len(js["lods"][0]["atomics"]) == 7
    lv = _levels()[0]
    assert dmparse.main(["dump", str(lv)]) == 0
    js = json.loads(capsys.readouterr().out)
    assert js["type"] == "level" and js["textures"] and js["world"]


def test_malformed_input_raises_parse_error():
    for bad in (b"MODL\xff\xff\xff\xff", b"MODL" + struct.pack("<I", 8) + b"MLOD" + struct.pack("<I", 99),
                b"SCEN\x00", struct.pack("<I", dmparse.ZAL_MAGIC) + bytes(12) + b"\x01\x00\x05\x00"):
        with pytest.raises(dmparse.ParseError):
            dmparse.read_any(bad)

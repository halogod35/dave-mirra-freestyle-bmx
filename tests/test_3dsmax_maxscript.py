"""The pure-MAXScript importer (tools/3dsmax/maxscript/DaveMirraImport.ms), executed by a
MAXScript-subset interpreter (tests/maxscript_interp.py) because 3ds Max is not installed:
its parser must agree with dmparse on every file, and its scene building passes the same checks as the
Python importer against the recording 3ds Max mock."""
import re
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files, corpus_params

REPO = Path(__file__).resolve().parents[1]
MS = REPO / "tools" / "3dsmax" / "maxscript" / "DaveMirraImport.ms"
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "parser"))
import dmparse  # noqa: E402
import max_checks as chk  # noqa: E402
from max_mock import P3, Runtime  # noqa: E402
from maxscript_interp import Interp, Thrown, tokenize  # noqa: E402


@pytest.fixture(scope="module")
def ms():
    it = Interp(scene=Runtime())
    it.run_file(str(MS))
    return it


def fresh(tmp_path):
    it = Interp(scene=Runtime())
    it.temp_dir = str(tmp_path / "temp")
    it.run_file(str(MS))
    return it


def _u32(v):
    return None if v is None else v & 0xFFFFFFFF


def _lst(p):
    return [float(c) for c in p]


def test_ms_tokenizes_whole_file_and_mirrors_exist():
    src = MS.read_text(encoding="utf-8")
    tokenize(src[:src.index("-- ==== UI") if "-- ==== UI" in src else len(src)])
    mirrors = set(re.findall(r"mirrors dmparse\.(\w+)", src))
    assert len(mirrors) >= 12
    for m in mirrors:
        assert hasattr(dmparse, m), f".ms says it mirrors dmparse.{m}, which does not exist"


@pytest.mark.parametrize("name", ["Hips", "Head", "LeftFoot", "DAVEMIRRA", "NAME_917_0", "tire", ""])
def test_ms_zhash(ms, name):
    assert ms.fn("DM_zhash")(name) & 0xFFFFFFFF == dmparse.zhash(name)     # .ms keeps it signed 32-bit


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_ms_parser_matches_dmparse(ms, path):
    data = path.read_bytes()
    a = ms.fn("DM_readZmd")(str(path))
    b = dmparse.read_zmd(data)
    assert a.get("empty") == b["empty"]
    if b["empty"]:
        return
    assert len(a.get("lods")) == len(b["lods"])
    for li, (la, lb) in enumerate(zip(a.get("lods"), b["lods"])):
        assert la.get("mldf") == lb["mldf"] and la.get("rwversion") == lb["rw_version"]
        for fa, fb in zip(la.get("frames"), lb["frames"]):
            assert [_lst(fa.get(k)) for k in ("right", "up", "fwd", "pos")] == [fb[k] for k in ("right", "up", "at", "pos")]
            assert fa.get("parent") == fb["parent"] and _u32(fa.get("boneid")) == fb["bone_id"]   # .ms reads ids signed
        for aa, ab in zip(la.get("atomics"), lb["atomics"]):
            ga, gb = aa.get("geometry"), ab["geometry"]
            assert aa.get("frame") == ab["frame"] and ga.get("flags") == gb["flags"]
            assert [_lst(p) for p in ga.get("positions")] == gb["positions"]
            assert (None if ga.get("normals") is None else [_lst(p) for p in ga.get("normals")]) == gb["normals"]
            assert ga.get("uvs") == gb["uvs"] and ga.get("colors") == gb["colors"]
            assert ga.get("triangles") == gb["triangles"] and ga.get("sphere") == gb["sphere"]
            assert ga.get("slots") == [[s["color"], s["texture"]] for s in gb["material_slots"]]
            assert (None if ga.get("bones") is None else [[x, y, _u32(z)] for x, y, z in ga.get("bones")]) == gb["bones"]
        assert ms.fn("DM_frameNames")(a, li) == [s["name"] for s in dmparse.skeleton(b, li)]
        for ai in range(len(lb["atomics"])):
            assert ms.fn("DM_vertexFrames")(a, li, ai) == dmparse.vertex_frames(b, li, ai)
        assert ms.fn("DM_upright")(a, li) == dmparse.upright_correction(b, li)
    tex = [(t.get("name"), t.get("kind"), data[t.get("dataoff"):t.get("dataoff") + t.get("datalen")]) for t in a.get("textures")]
    assert tex == [(t["name"], t["kind"], t["data"]) for t in b["textures"]]
    md = a.get("mdef")
    assert md.get("name") == b["mdef"]["name"]
    assert [[n, _u32(i)] for n, i in md.get("entries")] == [[e["name"], e["id"]] for e in b["mdef"]["entries"]]


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_ms_import_every_zmd(path, tmp_path):
    it = fresh(tmp_path)
    if path.stat().st_size == 0:
        with pytest.raises(Thrown, match="empty"):
            it.fn("DM_ImportZmd")(str(path), chk.SCALE, 0, True, str(tmp_path))
        return
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(path), chk.SCALE, 0, True, str(tmp_path), exactNormals=True)
    assert folder == str(tmp_path)
    _, (agree, checked) = chk.check_import(path, root, frames, meshes, tmp_path, normals=True)
    assert agree >= 0.75 * checked       # mostly with the CCW winding (99.6% of all model faces; DaveMirra.ZMD 77%)


def _find(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def test_ms_upright_and_default_temp_folder(tmp_path):
    it = fresh(tmp_path)
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(_find("DaveMirra1")), chk.SCALE, 0, True, "")
    chk.check_upright_rider(frames)
    assert folder.startswith(str(tmp_path / "temp")) and "DaveMirraTextures" in folder
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(_find("DMbike01")), chk.SCALE, 0, True, str(tmp_path))
    chk.check_upright_bike(frames)
    chk.check_import(_find("DMbike01"), root, frames, meshes, tmp_path)       # default: no Edit_Normals


def test_ms_refusals(tmp_path):
    it = fresh(tmp_path)
    p = _find("DaveMirra_shadow")
    with pytest.raises(Thrown, match="LOD"):
        it.fn("DM_ImportZmd")(str(p), chk.SCALE, 5, True, str(tmp_path))
    with pytest.raises(Thrown, match="game install"):
        it.fn("DM_ImportZmd")(str(p), chk.SCALE, 0, True, str(GAME_DIR / "assets" / "ms_textures_here"))
    assert not (GAME_DIR / "assets" / "ms_textures_here").exists()
    r = it.fn("DM_ImportZmd")(str(p), chk.SCALE, 1, True, str(tmp_path))
    assert len(r[2]) == 1
    with pytest.raises(Thrown, match="no MODL"):
        bad = tmp_path / "bad.zmd"
        bad.write_bytes(b"XXXX" + bytes(20))
        it.fn("DM_readZmd")(str(bad))


def _level_params():
    root = GAME_DIR / "assets"
    if not root.is_dir():
        return [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))]
    return [pytest.param(d, id=d.name) for d in sorted(root.iterdir())
            if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())]


def _p3(v):
    return [float(c) for c in v]


@pytest.mark.parametrize("d", _level_params())
def test_ms_level_parser_matches_dmparse(ms, d):
    a = ms.fn("DM_readLevel")(str(d))
    b = dmparse.read_level(str(d))
    zsd = next(f for f in d.iterdir() if f.suffix.lower() == ".zsd")
    assert a.get("files") == [[f["order"], f["size"], f["name"]] for f in dmparse.read_zsd(zsd.read_bytes())["files"]]
    w, wb = a.get("world"), b["world"]
    assert _p3(w.get("origin")) == [float(c) for c in wb["origin"]] and w.get("flags") == wb["flags"]
    assert w.get("slots") == [[s["color"], s["texture"]] for s in wb["slots"]]
    assert len(w.get("sectors")) == len(wb["sectors"])
    for x, y in zip(w.get("sectors"), wb["sectors"]):
        assert [_p3(v) for v in x.get("vertices")] == y["vertices"]
        assert (None if x.get("normals") is None else [_p3(n) for n in x.get("normals")]) == y["normals"]
        assert x.get("uvs") == y["uvs"] and x.get("colors") == y["colors"] and x.get("triangles") == y["triangles"]
    assert [(p.get("id") & 0xFFFFFFFF, p.get("name"), p.get("m9"), _p3(p.get("pos"))) for p in a.get("props")] ==            [(p["def_id"], p["name"], p["matrix"], p["pos"]) for p in b["props"]]
    assert [(x.get("id") & 0xFFFFFFFF, x.get("name")) for x in a.get("propdefs")] == [(x["id"], x["name"]) for x in b["prop_defs"]]
    assert [(p.get("id") & 0xFFFFFFFF, p.get("ptype"), p.get("name"), p.get("m9"), _p3(p.get("pos"))) for p in a.get("points")] ==            [(p["id"], p["type"], p["name"], p["matrix"], p["pos"]) for p in b["scene"]["points"]]
    tags = a.get("tags")
    assert len(tags) == len(b["tags"]["faces"])
    assert [t[3] for t in tags] == [f["code"] for f in b["tags"]["faces"]]
    import struct as _st
    assert all(_st.pack("<9f", *[c for v in t[:3] for c in v]) == _st.pack("<9f", *[c for v in f["verts"] for c in v])
               for t, f in zip(tags, b["tags"]["faces"]))
    mats = a.get("textures")
    data = (d / "Mat._sd").read_bytes()
    assert [(t.get("name"), data[t.get("dataoff"):t.get("dataoff") + t.get("datalen")]) for t in mats] ==            [(t["name"], t["data"]) for t in b["textures"]]


@pytest.mark.parametrize("d", _level_params())
def test_ms_import_every_level(d, tmp_path):
    it = fresh(tmp_path)
    root, world, props, tags, points, pano, folder = it.fn("DM_ImportLevel")(str(d), chk.SCALE, str(tmp_path / "tex"),
                                                                              exactNormals=True)
    chk.check_level(d, root, world, props, tags, points, pano, tmp_path / "tex", normals=True)
    assert "4=BSP._sd" in root.userprops["dmbmx_components"]


def test_interpreter_is_strict(tmp_path):
    it = Interp(scene=Runtime())
    src = tmp_path / "t.ms"
    src.write_text('fn f a = (b = a + 1)\n', encoding="utf-8")
    it.run_file(str(src))
    with pytest.raises(Exception, match="undeclared"):
        it.fn("f")(1)
    src.write_text('fn g = (mesh vertices:#() faces:#() materialIDs:#() bogus:1)\n', encoding="utf-8")
    it.run_file(str(src))
    with pytest.raises(Exception):
        it.fn("g")()
    for bad in ('struct S (tag, off)\n', 'fn k at = (at)\n', 'fn m = (local on = 1)\n'):   # reserved words
        src.write_text(bad, encoding="utf-8")
        with pytest.raises(Exception, match="reserved"):
            Interp().run_file(str(src))
    src.write_text('global hx = 0xFFFFFFFF\n', encoding="utf-8")
    it.run_file(str(src))
    assert it.globals["hx"] == -1                 # 32-bit signed hex literals, as in MAXScript
    src.write_text('fn h a = (a[1] + 7 / 2)\n', encoding="utf-8")
    it.run_file(str(src))
    assert it.fn("h")([10]) == 13            # 1-based index, integer division
    assert isinstance(it.eval(("point3", [("lit", 1), ("lit", 2), ("lit", 3)])), P3)

"""The 3ds Max Python importer, driven by a recording mock of pymxs (3ds Max is not installed).
For every ZMD: vertex/face/UV counts, material assignment and bitmap files, bone hierarchy, skin
weights, part parenting, and the final world positions of every vertex (compared with dmbmx).
Every ZMD and every level is imported with exact normals (Edit_Normals), checked by
max_checks.check_normals."""
import math
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files, corpus_params

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "python"))
import dave_mirra_import as dmi  # noqa: E402
import dmparse  # noqa: E402  (the vendored copy next to the importer)
from max_mock import M3, PymxsModule, Runtime  # noqa: E402
import max_checks as chk  # noqa: E402

from dmbmx.formats import zmd  # noqa: E402

SCALE = 100.0


@pytest.fixture
def rt():
    r = Runtime()
    dmi.set_runtime(r, PymxsModule(r))
    yield r
    dmi.set_runtime(None, None)


def test_vendored_parser_is_identical():
    a = (REPO / "tools" / "3dsmax" / "parser" / "dmparse.py").read_bytes()
    b = (REPO / "tools" / "3dsmax" / "python" / "dmparse.py").read_bytes()
    assert a == b, "copy tools/3dsmax/parser/dmparse.py to tools/3dsmax/python/ after editing it"


def _det(rows):
    (a, b, c), (d, e, f), (g, h, i) = rows
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_import_every_zmd(path, rt, tmp_path):
    data = path.read_bytes()
    ref = zmd.read(data)
    if ref.empty:
        with pytest.raises(ValueError, match="empty"):
            dmi.import_zmd(str(path), texture_dir=str(tmp_path))
        return
    s = dmi.import_zmd(str(path), scale=SCALE, texture_dir=str(tmp_path), explicit_normals=True)
    _, (agree, checked) = chk.check_import(path, s["root"], s["frames"], s["meshes"], tmp_path, normals=True)
    assert agree >= 0.75 * checked       # mostly with the CCW winding (99.6% of all model faces; DaveMirra.ZMD 77%)
    z = dmparse.read_zmd(data)
    lo = z["lods"][0]
    # root: a proper rotation
    assert abs(_det(s["root_transform"][0]) - 1.0) < 1e-9
    # frames: one node each, hierarchy as in the file
    assert len(s["frames"]) == len(lo["frames"])
    for i, (f, node) in enumerate(zip(lo["frames"], s["frames"])):
        assert node.parent is (s["frames"][f["parent"]] if f["parent"] >= 0 else s["root"])
        assert node.kind == ("bone" if s["skinned"] else "dummy")
    # meshes
    assert len(s["meshes"]) == len(lo["atomics"])
    meshes = list(ref.meshes(0, world=True))
    root = s["root"].transform
    for a, node, ms in zip(lo["atomics"], s["meshes"], meshes):
        g = a["geometry"]
        assert len(node.verts) == g["num_vertices"] and len(node.faces) == len(g["triangles"])
        assert node.matids == [t[3] + 1 for t in g["triangles"]]
        assert len(node.smooth) == len(node.faces)
        if g["uvs"]:
            m = node.maps[1]
            assert m["n"] == g["num_vertices"] and len(m["faces"]) == len(node.faces)
            assert all(abs(m["verts"][i + 1][1] - (1.0 - g["uvs"][0][i][1])) < 1e-12 for i in range(m["n"]))
        assert (0 in node.maps) == bool(g["colors"])
        mm = node.material
        assert mm.kind == "multi" and len(mm.subs) == len(g["material_slots"]) and all(mm.subs)
        for slot, sub in zip(g["material_slots"], mm.subs):
            if slot["texture"]:
                tex = next(t for t in z["textures"] if t["name"].upper() == slot["texture"].upper())
                assert Path(sub.diffuseMap.filename).read_bytes() == tex["data"]
                assert str(tmp_path) in sub.diffuseMap.filename
        # world positions: node transform (row vectors) applied to the vertices must equal the game's
        # world positions (dmbmx meshes(world=True)) * scale, mapped through the root rotation
        xf = node.transform
        for v, w in list(zip(node.verts, ms["positions"]))[::7]:
            want = root.xf(tuple(c * SCALE for c in w))
            got = xf.xf(v)
            assert all(abs(p - q) < 1e-6 * SCALE for p, q in zip(got, want)), (v, got, want)
        if g["bones"]:
            sk = node.modifiers[-1]                       # above the Edit_Normals
            assert sk.kind == "Skin" and sk.bones == s["frames"]
            vf = dmparse.vertex_frames(z, 0, lo["atomics"].index(a))
            assert sk.weights == {v + 1: f + 1 for v, f in enumerate(vf)}
            assert node.parent is s["root"]
        else:
            assert node.parent is s["frames"][a["frame"]]
            assert [m.kind for m in node.modifiers] == (["Edit_Normals"] if g["normals"] else [])


def _find(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _pos(node):
    return node.transform.pos


def test_rider_and_bike_stand_upright_in_max(rt, tmp_path):
    s = dmi.import_zmd(str(_find("DaveMirra1")), scale=SCALE, texture_dir=str(tmp_path))
    by = {n.name: n for n in s["frames"]}
    assert _pos(by["Head"])[2] - _pos(by["LeftFoot"])[2] > 1.0 * SCALE   # Max is Z-up
    assert _pos(by["LeftHand"])[0] > _pos(by["RightHand"])[0]             # left hand on +X (not mirrored)
    assert _pos(by["Head"])[1] > _pos(by["Hips"])[1] - 0.2 * SCALE        # facing -Y (Max front view) roughly
    s = dmi.import_zmd(str(_find("DMbike01")), scale=SCALE, texture_dir=str(tmp_path))
    by = {n.name: n for n in s["frames"]}
    assert _pos(by["SeatGrip"])[2] > _pos(by["BackWheel"])[2] + 0.3 * SCALE
    assert _pos(by["FrontWheel"])[1] < _pos(by["BackWheel"])[1]           # bike front towards -Y


def test_lod_choice_and_errors(rt, tmp_path):
    p = _find("DaveMirra_shadow")
    s = dmi.import_zmd(str(p), lod=1, texture_dir=str(tmp_path))
    assert len(s["meshes"]) == 1
    with pytest.raises(ValueError, match="LOD"):
        dmi.import_zmd(str(p), lod=5, texture_dir=str(tmp_path))
    with pytest.raises(ValueError, match="game install"):
        dmi.import_zmd(str(p), texture_dir=str(GAME_DIR / "assets" / "textures_here"))
    assert not (GAME_DIR / "assets" / "textures_here").exists()


def test_default_texture_folder_is_temp(rt):
    s = dmi.import_zmd(str(_find("DMbike01")))
    assert "DaveMirraTextures" in s["texture_dir"] and not dmi.inside_game_install(s["texture_dir"])


def test_explicit_normals_option(rt, tmp_path):
    p = _find("DMbike01")
    s = dmi.import_zmd(str(p), texture_dir=str(tmp_path))                      # default: no Edit_Normals
    chk.check_import(p, s["root"], s["frames"], s["meshes"], tmp_path)
    s = dmi.import_zmd(str(p), texture_dir=str(tmp_path), explicit_normals=True)
    for node in s["meshes"]:
        en = node.modifiers[0]
        assert en.kind == "Edit_Normals"
        # one GetNormalID per face corner, then SetNormal + SetNormalExplicit once per normal id
        nn = len(en.topo[1])
        assert len(en.explicit) == nn and en.calls == 3 * len(node.faces) + 2 * nn


def test_motion_keys(rt, tmp_path):
    mz = next((p for p in corpus_files(".zal") if p.name.lower() == "motion.zal"), None)
    if mz is None:
        pytest.skip("no corpus")
    s = dmi.import_zmd(str(_find("DaveMirra1")), texture_dir=str(tmp_path), motion_path=str(mz), motion_ids=[8])
    clip = dmparse.read_motion_bank(mz.read_bytes())[8]
    assert s["keys"] == clip["frames"] * len(s["frames"])
    head = next(n for n in s["frames"] if n.name == "Head")
    hips = next(n for n in s["frames"] if n.name == "Hips")
    assert len(head.keys) == clip["frames"]
    for (t1, m1), (t2, m2) in zip(head.keys, hips.keys):
        assert all(math.isfinite(c) for c in m1.pos)
        assert m1.pos[2] > m2.pos[2]                    # head above hips while riding (motion 8 pedals)
    # bike clip keys on a bike
    s = dmi.import_zmd(str(_find("DMbike01")), texture_dir=str(tmp_path), motion_path=str(mz), motion_ids=[8])
    sprocket = next(n for n in s["frames"] if n.name == "Sprocket")
    assert len({m.rows for _, m in sprocket.keys}) > 10       # the crank turns


def _level_dirs():
    root = GAME_DIR / "assets"
    if not root.is_dir():
        return [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))]
    return [pytest.param(d, id=d.name) for d in sorted(root.iterdir())
            if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())]


@pytest.mark.parametrize("d", _level_dirs())
def test_import_every_level(d, rt, tmp_path):
    s = dmi.import_level(str(d), scale=SCALE, texture_dir=str(tmp_path), explicit_normals=True)
    _, wind = chk.check_level(d, s["root"], s["world"], s["props"], list(s["tags"].values()), s["points"],
                              s["panorama"], tmp_path, normals=True)
    # World normals vs the Max face winding (the game's CCW rule, face by face in check_normals): the game
    # data agrees on 91-100% of faces, except two levels whose floor normals mostly point down
    agree, checked = wind["world"]
    if d.name in ("ProTourDirt", "WoodwardStreet"):
        assert agree < 0.35 * checked
    else:
        assert agree > 0.9 * checked
    assert "BSP=BSP._sd" in s["root"].userprops["dmbmx_components"]


def test_level_not_centred(rt, tmp_path):
    d = next(p.values[0] for p in _level_dirs())
    if d is None:
        pytest.skip("no corpus")
    s = dmi.import_level(str(d), texture_dir=str(tmp_path), centre=False)
    chk.check_level(d, s["root"], s["world"], s["props"], list(s["tags"].values()), s["points"], s["panorama"],
                    tmp_path, centre=False)


def test_mock_rejects_unknown_api(rt):
    with pytest.raises(AttributeError):
        rt.someFunctionWeNeverVerified()
    assert isinstance(rt.Matrix3((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)), M3)

"""The "Exact normals" option in both 3ds Max importers (Edit_Normals), for models and levels.

The every-model / every-level imports with exact normals are in test_3dsmax_max_python.py and
test_3dsmax_maxscript.py (max_checks.check_normals). This file holds:
* the Edit_Normals mock's own rules (it must never accept what 3ds Max would reject);
* both importers on a synthetic mesh (no game data needed): missing, zero-length and non-unit normals;
* the interpreter features the .ms now uses (keyword parameters, 'with undo/redraw off');
* the two importers setting the same explicit normals on real models and a level;
* the level-normal facts from the game data (zero-length normals, levels whose floor normals point down).
"""
import math
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files

REPO = Path(__file__).resolve().parents[1]
MS = REPO / "tools" / "3dsmax" / "maxscript" / "DaveMirraImport.ms"
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "python"))
import dave_mirra_import as dmi  # noqa: E402
import dmparse  # noqa: E402
import max_checks as chk  # noqa: E402
from max_mock import P3, PymxsModule, Runtime  # noqa: E402
from maxscript_interp import Interp, MXSError  # noqa: E402


@pytest.fixture
def rt():
    r = Runtime()
    dmi.set_runtime(r, PymxsModule(r))
    yield r
    dmi.set_runtime(None, None)


def _ms(tmp_path, scene=None):
    it = Interp(scene=scene or Runtime())
    it.temp_dir = str(tmp_path / "temp")
    it.run_file(str(MS))
    return it


# ----------------------------------------------------------------------------- the mock's rules

def _quad(rt):
    """Two triangles sharing an edge, one smoothing group (as the importers build meshes)."""
    m = rt.mesh(vertices=[rt.Point3(0, 0, 0), rt.Point3(1, 0, 0), rt.Point3(1, 1, 0), rt.Point3(0, 1, 0)],
                faces=[rt.Point3(1, 2, 3), rt.Point3(1, 3, 4)], materialIDs=[1, 1])
    for f in (1, 2):
        rt.setFaceSmoothGroup(m, f, 1)
    return m


def test_mock_edit_normals_needs_the_modify_panel(rt):
    m = _quad(rt)
    en = rt.Edit_Normals()
    rt.addModifier(m, en)
    with pytest.raises(AssertionError, match="Modify panel"):
        en.GetNumFaces()                                     # not current in the Modify panel
    rt.select(m)
    with pytest.raises(AssertionError, match="Modify panel"):
        en.GetNumFaces()                                     # still in the Create panel
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(en)
    assert en.GetNumFaces() == 2
    other = _quad(rt)
    rt.select(other)                                         # selecting another node breaks the calls
    with pytest.raises(AssertionError):
        en.GetNormalID(1, 1)


def test_mock_edit_normals_ids(rt):
    m = _quad(rt)
    en = rt.Edit_Normals()
    rt.addModifier(m, en)
    rt.select(m)
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(en)
    assert en.GetNumNormals() == 4                           # one per vertex in one smoothing group
    ids = [[en.GetNormalID(f, c) for c in (1, 2, 3)] for f in (1, 2)]
    assert ids[0][0] == ids[1][0] and ids[0][2] == ids[1][1]  # shared vertices share their normal id
    assert sorted({i for r in ids for i in r}) == [1, 2, 3, 4]
    assert [en.GetVertexID(1, c) for c in (1, 2, 3)] == [1, 2, 3]
    assert ids[0] != [1, 2, 3]                               # ids are not vertex ids
    for bad in ((0, 1), (3, 1), (1, 0), (1, 4), (1.0, 1)):
        with pytest.raises(AssertionError):
            en.GetNormalID(*bad)
    for nid in (0, 5):
        with pytest.raises(AssertionError):
            en.SetNormal(nid, rt.Point3(0, 0, 1))
    with pytest.raises(AssertionError):
        en.SetNormal(1, (0, 0, 1))                           # a Point3 is needed
    with pytest.raises(TypeError):
        en.SetNormalExplicit(1)                              # explicit: is passed
    en.SetNormal(ids[0][0], rt.Point3(0, 0, 1))              # not explicit: recomputed by Max, lost
    en.SetNormal(ids[0][1], rt.Point3(0, 1, 0))
    en.SetNormalExplicit(ids[0][1], explicit=True)
    assert en.result() == {2: (0.0, 1.0, 0.0)}


def test_mock_edit_normals_smoothing_group_zero_is_per_face(rt):
    m = rt.mesh(vertices=[rt.Point3(0, 0, 0), rt.Point3(1, 0, 0), rt.Point3(1, 1, 0), rt.Point3(0, 1, 0)],
                faces=[rt.Point3(1, 2, 3), rt.Point3(1, 3, 4)], materialIDs=[1, 1])
    en = rt.Edit_Normals()
    rt.addModifier(m, en)
    rt.select(m)
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(en)
    assert en.GetNumNormals() == 2 and en.GetNormalID(1, 1) == en.GetNormalID(1, 3) != en.GetNormalID(2, 1)


def test_mock_instances_share_the_modifier_and_need_node(rt):
    m = _quad(rt)
    en = rt.Edit_Normals()
    rt.addModifier(m, en)
    inst = rt.instance(m)
    assert inst.modifiers is m.modifiers
    rt.select(m)
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(en)
    with pytest.raises(AssertionError, match="node:"):
        en.GetNumFaces()
    assert en.GetNumFaces(node=m) == 2


# ----------------------------------------------------------------------------- synthetic mesh, both importers

# 4 vertices: a non-unit normal (s8/127 style), a missing one, a zero-length one, a unit one
_NORMALS = [[0.0, 0.0, 1.0157], None, [0.0, 0.0, 0.0], [0.6, 0.0, 0.8]]
_POS = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]
_TRIS = [[0, 1, 2, 0], [0, 2, 3, 0]]
_WANT = {1: (0.0, 0.0, 1.0), 4: (0.6, 0.0, 0.8)}


def _same(a, b, tol=1e-12):
    return set(a) == set(b) and all(all(abs(x - y) < tol for x, y in zip(a[k], b[k])) for k in a)


def test_python_explicit_normals_synthetic(rt):
    g = {"positions": _POS, "normals": _NORMALS, "uvs": [], "colors": None, "triangles": _TRIS,
         "material_slots": [{"texture": None, "color": [1, 2, 3, 255]}]}
    node = dmi.build_mesh("q", g, 1.0, explicit_normals=True)
    en = chk.edit_normals(node)
    assert _same(en.result(), _WANT)
    assert en.calls == 3 + 2 * 2                             # 3 corners use vertices 1 and 4; 2 ids x 2 calls
    node = dmi.build_mesh("q", dict(g, normals=[None, None, [0, 0, 0], None]), 1.0, explicit_normals=True)
    assert node.modifiers == []                              # nothing usable: no modifier at all
    assert dmi.unit_normal([0, 0, 2]) == (0, 0, 1) and dmi.unit_normal([0, 0, 0]) is None


def test_maxscript_explicit_normals_synthetic(tmp_path):
    rt = Runtime()
    it = _ms(tmp_path, rt)
    geo = it.fn("DM_Geometry")
    ms_n = [None if n is None else P3(*n) for n in _NORMALS]
    g = geo(flags=0, numvertices=4, positions=[P3(*p) for p in _POS], normals=ms_n, uvs=[], colors=None,
            triangles=[list(t) for t in _TRIS], slots=[], sphere=None, bones=None)
    node = it.fn("DM_buildMesh")("q", g, 1.0)
    assert node.modifiers == []
    en = it.fn("DM_setExplicitNormals")(node, g)
    assert chk.edit_normals(node) is en and _same(en.result(), _WANT)
    assert en.calls == 1 + 3 + 2 * 2                         # GetNumNormals, 3 corners, 2 ids x 2
    g.set("normals", [None, None, P3(0, 0, 0), None])
    node = it.fn("DM_buildMesh")("q", g, 1.0)
    assert it.fn("DM_setExplicitNormals")(node, g) is None and node.modifiers == []
    g.set("normals", None)
    assert it.fn("DM_setExplicitNormals")(node, g) is None


# ----------------------------------------------------------------------------- interpreter features

def test_interp_keyword_parameters_and_with_contexts(tmp_path):
    rt = Runtime()
    it = Interp(scene=rt)
    src = tmp_path / "k.ms"
    src.write_text('fn f a b:false c:(1 + 2) = (#(a, b, c))\n'
                   'fn u = (with undo off (with redraw off (#(1, 2))))\n', encoding="utf-8")
    it.run_file(str(src))
    assert it.fn("f")(1) == [1, False, 3]
    assert it.fn("f")(1, b=True) == [1, True, 3] and it.fn("f")(1, C=7) == [1, False, 7]
    with pytest.raises(MXSError):
        it.fn("f")(1, bogus=1)                               # an unknown keyword is a bug in the script
    with pytest.raises(MXSError):
        it.fn("f")()
    seen = []
    rt.Point3 = lambda *a: seen.append((rt.undo_on, rt.redraw_on))
    assert it.fn("u")() == [1, 2] and rt.undo_on is True and rt.redraw_on is True
    for bad in ('fn g a:1 b = (a)\n', 'fn h = (with quiet on (1))\n'):
        src.write_text(bad, encoding="utf-8")
        with pytest.raises(MXSError):
            Interp(scene=rt).run_file(str(src))
    src.write_text('fn w = (with undo off (length [3, 4, 0]))\nfn bad = (length #(1, 2))\n', encoding="utf-8")
    it.run_file(str(src))
    assert it.fn("w")() == 5.0
    with pytest.raises(MXSError):
        it.fn("bad")()


def test_ms_old_entry_points_still_work(tmp_path):
    """DM_ImportZmd (5 args), DM_ImportZmdMotion (7) and DM_ImportLevel (3) keep their signatures."""
    src = MS.read_text(encoding="utf-8")
    assert "fn DM_ImportZmd path scale lod upright texDir exactNormals:false boneAim:true flipNormals:false =" in src
    assert "fn DM_ImportZmdMotion path scale lod upright texDir motionPath motionIds exactNormals:false boneAim:true flipNormals:false =" in src
    assert "fn DM_ImportLevel folder scale texDir exactNormals:false flipNormals:false =" in src
    it = _ms(tmp_path)
    for name, n in (("DM_ImportZmd", 5), ("DM_ImportZmdMotion", 7), ("DM_ImportLevel", 3)):
        f = it.fn(name)
        want = ["exactnormals", "flipnormals"] if name == "DM_ImportLevel" else ["exactnormals", "boneaim", "flipnormals"]
        assert len(f.params) == n and list(f.kwparams) == want
    ui = src[src.index("rollout DaveMirraRollout"):]
    assert 'checkbox chkNormals "Exact normals (Edit_Normals, slower)"' in ui
    assert ui.count("exactNormals:chkNormals.checked") == 2


# ----------------------------------------------------------------------------- both importers agree

def _find(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _results(nodes):
    return [chk.edit_normals(n).result() if chk.edit_normals(n) else {} for n in nodes]


@pytest.mark.parametrize("stem", ["DMbike01", "DaveMirra1", "DaveMirra"])
def test_both_importers_set_the_same_model_normals(stem, rt, tmp_path):
    p = _find(stem)
    s = dmi.import_zmd(str(p), scale=chk.SCALE, texture_dir=str(tmp_path / "py"), explicit_normals=True)
    it = _ms(tmp_path)
    r = it.fn("DM_ImportZmd")(str(p), chk.SCALE, 0, True, str(tmp_path / "ms"), exactNormals=True)
    a, b = _results(s["meshes"]), _results(r[2])
    assert any(a) and len(a) == len(b)
    assert all(_same(x, y) for x, y in zip(a, b))


def test_both_importers_set_the_same_level_normals(rt, tmp_path):
    d = GAME_DIR / "assets" / "AcclaimStreet"                # has zero-length normals in sectors and props
    if not d.is_dir():
        pytest.skip("no corpus")
    s = dmi.import_level(str(d), scale=chk.SCALE, texture_dir=str(tmp_path / "py"), explicit_normals=True)
    it = _ms(tmp_path)
    root, world, props, tags, points, pano, folder = it.fn("DM_ImportLevel")(
        str(d), chk.SCALE, str(tmp_path / "ms"), exactNormals=True)
    py = [s["world"]] + s["props"] + [s["panorama"]]
    ms = [world] + props + [pano]
    a, b = _results(py), _results(ms)
    assert len(a) == len(b) and all(_same(x, y, 1e-9) for x, y in zip(a, b))
    # zero-length normals stay non-explicit
    secs = dmparse.read_level(str(d))["world"]["sectors"]
    nrm = [n for sec in secs for n in (sec["normals"] or [None] * len(sec["vertices"]))]
    zero = [i + 1 for i, n in enumerate(nrm) if n == [0.0, 0.0, 0.0]]
    assert len(zero) == 3 and not set(zero) & set(a[0])
    assert len(a[0]) == sum(n is not None for n in nrm) - 3
    for t in tags:
        assert not t.modifiers


# ----------------------------------------------------------------------------- game data facts

def _levels():
    root = GAME_DIR / "assets"
    if not root.is_dir():
        return []
    return [d for d in sorted(root.iterdir()) if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())]


def test_level_normal_facts():
    """What the level data holds (dmparse, all 12 levels), which the importers' behaviour is built on."""
    levels = _levels()
    if not levels:
        pytest.skip("no corpus")
    assert len(levels) == 12
    zero, down = {}, {}
    for d in levels:
        lv = dmparse.read_level(str(d))
        secs = lv["world"]["sectors"]
        with_n = [s for s in secs if s["normals"]]
        # every level has the normals flag (0x10); the sectors with an empty normals list (about 30%) are
        # exactly the empty sectors (0 vertices), so every world vertex has a stored normal
        assert lv["world"]["flags"] & 0x10 and all(s["normals"] is not None for s in secs)
        assert all(not s["vertices"] for s in secs if not s["normals"]) and 0.5 < len(with_n) / len(secs) < 0.9
        lens = [math.sqrt(sum(c * c for c in n)) for s in with_n for n in s["normals"] if any(n)]
        assert 0.995 < min(lens) and max(lens) < 1.02                 # s8/127: must be normalised
        z = sum(1 for s in with_n for n in s["normals"] if not any(n))
        if z:
            zero[d.name] = z
        # floors (faces whose CCW normal is within ~25 deg of +Y): do the game normals point down?
        floors = flipped = 0
        for s in with_n:
            P, N = s["vertices"], s["normals"]
            for _, a, b, c in s["triangles"]:
                fn = chk._cross(chk._sub(P[b], P[a]), chk._sub(P[c], P[a]))
                ln = math.sqrt(chk._dot(fn, fn))
                if ln > 0 and fn[1] / ln > 0.9:
                    floors += 1
                    flipped += N[a][1] + N[b][1] + N[c][1] < 0
        down[d.name] = flipped / floors
    assert zero == {"AcclaimStreet": 3, "GreenvilleVert": 3, "ProTourStreet": 5, "WoodwardVert": 3}
    # In two levels most floor normals point down, against the CCW winding: the game data, kept as is
    # by "Exact normals" (the game lights levels with the baked vertex colours, not these normals).
    assert {k for k, v in down.items() if v > 0.5} == {"ProTourDirt", "WoodwardStreet"}
    assert all(v < 0.11 for k, v in down.items() if k not in ("ProTourDirt", "WoodwardStreet"))


@pytest.mark.parametrize("stem", ["DMbike01", "DaveMirra1"])
def test_ms_model_geometry_rotates_normals_like_dmparse(stem, tmp_path):
    """DM_modelGeometry (props, panorama) = dave_mirra_import.merge_model (dmparse.model_meshes): normals
    turned by each atomic's frame. Every PropDef/Panorama frame in the game is the identity, so this uses
    models whose frames are rotated (bike parts)."""
    p = _find(stem)
    it = _ms(tmp_path)
    g = it.fn("DM_modelGeometry")(it.fn("DM_readZmd")(str(p)))
    want = dmi.merge_model(dmparse.read_zmd(p.read_bytes()))
    got = [None if n is None else list(n) for n in g.get("normals")]
    assert len(got) == len(want["normals"]) and any(want["normals"])
    for a, b in zip(got, want["normals"]):
        assert (a is None) == (b is None) and (a is None or all(abs(x - y) < 1e-9 for x, y in zip(a, b)))
    rotated = sum(1 for a in dmparse.skeleton(dmparse.read_zmd(p.read_bytes()), 0)
                  if any(abs(a["world"][i][j] - (i == j)) > 1e-3 for i in range(3) for j in range(3)))
    assert rotated > 0 or stem == "DaveMirra1"

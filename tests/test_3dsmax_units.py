"""Real-world size from the scene's System Unit Setup, in both 3ds Max importers.

Game units are metres. 3ds Max's System Unit Setup says 1 system unit = units.SystemScale units.SystemType,
so the real-world import scale is 1 / (metres per system unit). This file holds:
* the mock's System Unit Setup rules (pymxs and the MAXScript interpreter: never more permissive than Max);
* real_world_scale / DM_realWorldScale and the dialog label for every SystemType at scales 1, 5 and 0.1;
* identical geometry for one effective scale (real-world vs manual), and everything scaling together;
* the dialogs (source checks: the UI is not executed here) and a game-data size check (GreenvilleDirt Ebox).
"""
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files

REPO = Path(__file__).resolve().parents[1]
MS = REPO / "tools" / "3dsmax" / "maxscript" / "DaveMirraImport.ms"
PY = REPO / "tools" / "3dsmax" / "python" / "dave_mirra_import.py"
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "python"))
import dave_mirra_import as dmi  # noqa: E402
from max_mock import M3, P3, MxsName, PymxsModule, Runtime  # noqa: E402
from maxscript_interp import Interp, MXSError, Name, Parser, tokenize  # noqa: E402

METRES = {"inches": 0.0254, "feet": 0.3048, "miles": 1609.344, "millimeters": 0.001, "centimeters": 0.01,
          "meters": 1.0, "kilometers": 1000.0}
ABBR = {"inches": "in", "feet": "ft", "miles": "mi", "millimeters": "mm", "centimeters": "cm", "meters": "m",
        "kilometers": "km"}
CASES = [(t, s) for t in METRES for s in (1.0, 5.0, 0.1)]


@pytest.fixture
def rt():
    r = Runtime()
    dmi.set_runtime(r, PymxsModule(r))
    yield r
    dmi.set_runtime(None, None)


def _ms(tmp_path, scene):
    it = Interp(scene=scene)
    it.temp_dir = str(tmp_path / "temp")
    it.run_file(str(MS))
    return it


def _run(src, scene):
    it = Interp(scene=scene)
    for e in Parser(tokenize(src)).program():
        it.eval(e)
    return it


# ----------------------------------------------------------------------------- the mocks' rules

def test_mock_units_default_and_types(rt):
    assert rt.units.SystemType == rt.Name("inches") and rt.units.SystemScale == 1.0     # Max's US default
    t = rt.units.SystemType
    assert not isinstance(t, str) and str(t) == "inches" and repr(t) == "#inches"
    assert t != "inches"                                     # a name value is not a string
    assert rt.Name("Inches") == t                            # names compare case-insensitively
    with pytest.raises(AttributeError):
        t.lower()                                            # no str methods on a pymxs name


def test_mock_units_refuses_what_max_refuses(rt):
    with pytest.raises(AssertionError):
        rt.units.SystemType = "centimeters"                  # a string, not a name
    with pytest.raises(AssertionError):
        rt.units.SystemType = rt.Name("parsecs")             # not a System Unit Setup type
    with pytest.raises(AssertionError):
        rt.Name("#meters")                                   # rt.Name takes the bare name
    for bad in (0, -1.0, True, "5"):
        with pytest.raises(AssertionError):
            rt.units.SystemScale = bad
    with pytest.raises(AttributeError):
        rt.units.DisplayType                                 # not modelled (unverified)
    with pytest.raises(AttributeError):
        rt.units.MetricType = rt.Name("meters")
    rt.units.SystemType = rt.Name("Centimeters")
    rt.units.SystemScale = 5
    assert str(rt.units.SystemType) == "centimeters" and rt.units.SystemScale == 5.0


def test_interp_units_and_formatted_print(tmp_path):
    scene = Runtime()
    scene.units.set("centimeters", 5.0)
    it = _run('global t = units.SystemType\nglobal s = units.SystemScale\nglobal c = (t == #centimeters)\n'
              'global f = formattedPrint 39.37007874 format:".4g"\nglobal g = formattedPrint 5.0 format:"g"', scene)
    assert isinstance(it.globals["t"], Name) and it.globals["t"] == "centimeters"
    assert it.globals["s"] == 5.0 and it.globals["c"] is True
    assert it.globals["f"] == "39.37" and it.globals["g"] == "5"
    _run("units.SystemType = #meters\nunits.SystemScale = 0.1", scene)
    assert scene.units.SystemType == MxsName("meters") and scene.units.SystemScale == 0.1
    for bad in ('units.SystemType = "meters"', "units.DisplayType = #metric", "global x = units.DisplayType",
                'global x = formattedPrint 5 format:"g"', 'global x = formattedPrint 5.0 format:"d"',
                'global x = formattedPrint 5.0 format:"%g"', 'global x = formattedPrint "5" format:"s"'):
        with pytest.raises((MXSError, AssertionError)):
            _run(bad, scene)
    with pytest.raises(AssertionError):
        _run("units.SystemType = #parsecs", scene)


# ----------------------------------------------------------------------------- real-world scale and label

def _fmt(v):
    return "%.0f" % v if v >= 1000.0 else "%.4g" % v


def _label(t, s, manual=None):
    mpu = s * METRES[t]
    head = "Scene: 1 unit = %g %s" % (s, ABBR[t])
    if manual is None:
        r = _fmt(1.0 / mpu)
        return f"{head} -> scale {r} (game 1 m = {r} units)"
    return f"{head} -> manual scale {_fmt(manual)}: 1 game metre = {_fmt(manual * mpu)} m in this scene"


@pytest.mark.parametrize("t,s", CASES)
def test_real_world_scale_every_unit(t, s, rt, tmp_path):
    rt.units.set(t, s)
    want = 1.0 / (s * METRES[t])
    assert dmi.real_world_scale() == pytest.approx(want, rel=1e-12)
    assert dmi.scene_units() == (t, s)
    it = _ms(tmp_path, rt)
    assert it.fn("DM_realWorldScale")() == pytest.approx(want, rel=1e-12)
    assert it.fn("DM_metresPerSystemUnit")() == pytest.approx(s * METRES[t], rel=1e-12)
    for manual in (None, 40.0, 0.25, 2500.0):
        label = _label(t, s, manual)
        assert dmi.scale_label(manual) == label
        assert it.fn("DM_scaleLabel")(manual) == label


@pytest.mark.parametrize("t,s,scale,label", [
    ("centimeters", 1.0, 100.0, "Scene: 1 unit = 1 cm -> scale 100 (game 1 m = 100 units)"),
    ("meters", 1.0, 1.0, "Scene: 1 unit = 1 m -> scale 1 (game 1 m = 1 units)"),
    ("inches", 1.0, 39.37007874015748, "Scene: 1 unit = 1 in -> scale 39.37 (game 1 m = 39.37 units)"),
    ("centimeters", 5.0, 20.0, "Scene: 1 unit = 5 cm -> scale 20 (game 1 m = 20 units)"),
    ("millimeters", 0.1, 10000.0, "Scene: 1 unit = 0.1 mm -> scale 10000 (game 1 m = 10000 units)"),
    ("kilometers", 1.0, 0.001, "Scene: 1 unit = 1 km -> scale 0.001 (game 1 m = 0.001 units)"),
])
def test_real_world_examples(t, s, scale, label, rt, tmp_path):
    rt.units.set(t, s)
    assert dmi.real_world_scale() == pytest.approx(scale, rel=1e-12)
    assert dmi.scale_label() == label
    assert _ms(tmp_path, rt).fn("DM_scaleLabel")(None) == label


def test_manual_label_example(rt, tmp_path):
    rt.units.set("centimeters", 5.0)
    want = "Scene: 1 unit = 5 cm -> manual scale 40: 1 game metre = 2 m in this scene"
    assert dmi.scale_label(40.0) == want
    assert _ms(tmp_path, rt).fn("DM_scaleLabel")(40.0) == want


def test_unknown_unit_type_fails_clearly(rt, tmp_path):
    object.__setattr__(rt.units, "_type", "furlongs")        # impossible in Max; the importers must not guess
    with pytest.raises(ValueError, match="unknown system unit type"):
        dmi.real_world_scale()
    with pytest.raises(Exception, match="unknown system unit type"):
        _ms(tmp_path, rt).fn("DM_realWorldScale")()


# ----------------------------------------------------------------------------- geometry

def _find(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _scaled(v, k):
    """Every length in a node attribute multiplied by k (Point3, matrix translation, numbers)."""
    if isinstance(v, P3):
        return P3(*(c * k for c in v))
    if isinstance(v, M3):
        return M3(*v.rows, tuple(c * k for c in v.pos))
    return v * k


def _close(a, b, tol=1e-9):
    if isinstance(a, M3):
        return _close(a.rows, b.rows, tol) and _close(a.pos, b.pos, tol)
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(_close(x, y, tol) for x, y in zip(a, b))
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def _lengths(n):
    """A node's attributes that are lengths in Max units."""
    out = {"transform": n.transform, "keys": [m for _, m in n.keys]}
    if n.kind == "mesh":
        out["verts"] = n.verts
    for k in ("boxsize", "length", "width", "height"):
        if k in n.__dict__:
            out[k] = n.__dict__[k]
    return out


def _check_scaled(nodes_a, nodes_b, k):
    assert [n.name for n in nodes_a] == [n.name for n in nodes_b]
    for a, b in zip(nodes_a, nodes_b):
        la, lb = _lengths(a), _lengths(b)
        assert la.keys() == lb.keys(), a.name
        for key in la:
            va, vb = la[key], lb[key]
            if key in ("verts", "keys"):
                assert len(va) == len(vb) and all(_close(_scaled(x, k), y) for x, y in zip(va, vb)), (a.name, key)
            elif va is not None:
                assert _close(_scaled(va, k), vb), (a.name, key, va, vb)


def _all_nodes(r, frames):
    """The scene's meshes, dummies and bones (BoneSys.createBone nodes are in the mock's list too)."""
    assert all(n in r.nodes for n in frames)
    return r.nodes


def _py_zmd(p, units, scale, tmp_path, **kw):
    r = Runtime()
    r.units.set(*units)
    dmi.set_runtime(r, PymxsModule(r))
    try:
        s = dmi.import_zmd(str(p), scale=dmi.real_world_scale() if scale is None else scale,
                           texture_dir=str(tmp_path / "py"), **kw)
    finally:
        dmi.set_runtime(None, None)
    return _all_nodes(r, s["frames"])


def _ms_zmd(p, units, scale, tmp_path, *motion):
    r = Runtime()
    r.units.set(*units)
    it = _ms(tmp_path, r)
    s = it.fn("DM_realWorldScale")() if scale is None else scale
    if motion:
        res = it.fn("DM_ImportZmdMotion")(str(p), s, 0, True, str(tmp_path / "ms"), *motion)
    else:
        res = it.fn("DM_ImportZmd")(str(p), s, 0, True, str(tmp_path / "ms"))
    return _all_nodes(r, res[1])


@pytest.mark.parametrize("stem", ["DMbike01", "DaveMirra1"])
def test_real_world_import_equals_manual_scale(stem, tmp_path):
    """A 5 cm scene with real-world size = a manual scale of 20, in both importers; = scale 100 x 0.2."""
    p = _find(stem)
    real = _py_zmd(p, ("centimeters", 5.0), None, tmp_path)
    manual = _py_zmd(p, ("inches", 1.0), 20.0, tmp_path)
    assert any(n.kind == "bone" for n in real) or stem == "DMbike01"
    _check_scaled(real, manual, 1.0)
    _check_scaled(_py_zmd(p, ("inches", 1.0), 100.0, tmp_path), real, 0.2)     # everything scales together
    ms_real = _ms_zmd(p, ("centimeters", 5.0), None, tmp_path)
    ms_manual = _ms_zmd(p, ("inches", 1.0), 20.0, tmp_path)
    _check_scaled(ms_real, ms_manual, 1.0)
    _check_scaled(_ms_zmd(p, ("inches", 1.0), 100.0, tmp_path), ms_real, 0.2)


def test_motion_keys_scale_together(tmp_path):
    mz = next((p for p in corpus_files(".zal") if p.name.lower() == "motion.zal"), None)
    if mz is None:
        pytest.skip("no corpus")
    p = _find("DaveMirra1")
    a = _py_zmd(p, ("meters", 1.0), 100.0, tmp_path, motion_path=str(mz), motion_ids=[8])
    b = _py_zmd(p, ("meters", 1.0), None, tmp_path, motion_path=str(mz), motion_ids=[8])
    assert any(n.keys for n in b)
    _check_scaled(a, b, 0.01)
    c = _ms_zmd(p, ("meters", 1.0), None, tmp_path, str(mz), [8])
    assert any(n.keys for n in c)
    _check_scaled(a, c, 0.01)


def test_level_props_and_points_scale_together(tmp_path):
    d = GAME_DIR / "assets" / "GreenvilleDirt"
    if not d.is_dir():
        pytest.skip("no corpus")
    out = []
    for units, scale in ((("inches", 1.0), 100.0), (("centimeters", 5.0), None)):
        r = Runtime()
        r.units.set(*units)
        dmi.set_runtime(r, PymxsModule(r))
        try:
            s = dmi.import_level(str(d), scale=dmi.real_world_scale() if scale is None else scale,
                                 texture_dir=str(tmp_path / "py"))
        finally:
            dmi.set_runtime(None, None)
        out.append((r.nodes, s))
    _check_scaled(out[0][0], out[1][0], 0.2)
    # the GreenvilleDirt Ebox (1.50 x 2.02 x 1.50 m in game) at real size in a 5 cm scene: 30 x 40.4 x 30 units
    nodes, s = out[1]
    ebox = [n for n in s["props"] if n.name.startswith("Ebox_")]
    assert ebox
    master = s["masters"][int(ebox[0].userprops["dmbmx_def_id"])]
    ext = [max(v[i] for v in master.verts) - min(v[i] for v in master.verts) for i in range(3)]
    assert sorted(round(e * 0.05, 2) for e in ext) == [1.5, 1.5, 2.02]


# ----------------------------------------------------------------------------- the dialogs (source)

def test_ms_dialog_real_world_by_default():
    """Source checks; the dialog itself runs in tests/test_3dsmax_ui.py."""
    ui = MS.read_text(encoding="utf-8")
    ui = ui[ui.index("============ UI"):]
    assert 'checkbox chkReal "Real-world size (use scene units)" checked:true' in ui
    assert 'spinner spnScale "Scale" range:[0.000001, 1000000000, 100] type:#float enabled:false' in ui
    assert "label lblUnits" in ui and "DM_scaleLabel" in ui
    assert "refreshUnits()\n" in ui[ui.index("on DaveMirraRollout open do"):]
    assert "on btnUnits pressed do refreshUnits()" in ui
    assert "fn importScale = (if chkReal.checked then DM_realWorldScale() else spnScale.value)" in ui
    assert ui.count("(DaveMirraRollout.importScale())") == 2 and "spnScale.value (" not in ui


def test_py_dialog_real_world_by_default():
    src = PY.read_text(encoding="utf-8")
    ui = src[src.index("def show_dialog"):]
    assert 'QCheckBox("Real-world size (use scene units)")' in ui and "self.real.setChecked(True)" in ui
    assert "scale_label(None if real else self.scale.value())" in ui
    assert "self.scale.setEnabled(not real)" in ui
    assert ui.count("scale=self.import_scale()") == 2 and "scale=self.scale.value()" not in ui
    assert "QtCore.QEvent.ActivationChange" in ui


def test_scripted_scale_argument_unchanged():
    import inspect
    assert inspect.signature(dmi.import_zmd).parameters["scale"].default == 100.0
    assert inspect.signature(dmi.import_level).parameters["scale"].default == 100.0

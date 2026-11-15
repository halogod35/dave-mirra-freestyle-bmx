"""The 3ds Max importers' dialogs: a header with the options every import uses, a Models tab and a
Levels tab, run as a user would use them.

- MAXScript (DaveMirraImport.ms, the whole file including its UI section) runs in the MAXScript-subset
  interpreter, whose rollout model (tests/maxscript_interp.py) follows 3ds Max's rules: a .NET TabControl in a
  dotNetControl as the tab strip, a subRollout showing one tab rollout at a time, controls rebuilt from their
  defaults each time a rollout opens;
- Python (dave_mirra_import.show_dialog) runs against the Qt mock (max_mock.QtMock: QTabWidget, QGroupBox, ...).
Both: tab switching, choices kept across tab switches and for the session, the ZMD / level imports with the
header options, and the motion import chained from the dialog."""
import sys
from pathlib import Path

import pytest
from conftest import corpus_files

REPO = Path(__file__).resolve().parents[1]
MS = REPO / "tools" / "3dsmax" / "maxscript" / "DaveMirraImport.ms"
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "python"))
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "parser"))
import dave_mirra_import as dmi  # noqa: E402
import dmparse  # noqa: E402
import max_checks as chk  # noqa: E402
from max_mock import PymxsModule, QtMock, Runtime  # noqa: E402
from maxscript_interp import Interp, MXSError, MXSRuntimeError, Parser, tokenize  # noqa: E402

A, B = 8, 15


def _motion_zal():
    p = next((p for p in corpus_files(".zal") if p.name.lower() == "motion.zal"), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _zmd(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


@pytest.fixture(scope="module")
def bank():
    return dmparse.read_motion_bank(_motion_zal().read_bytes())


@pytest.fixture(scope="module")
def warm_cache():
    it = Interp(scene=Runtime())
    it.run_file(str(MS))
    it.fn("DM_motionGroups")(str(_motion_zal()))
    return it.globals["dm_motioncache"]


# ============================================================================ MAXScript dialog

def _dialog(tmp_path, cache=None, rt=None):
    """Run the whole .ms (it ends with createDialog) in a fresh 3ds Max session."""
    it = Interp(scene=rt or Runtime(), stop_marker=None)
    it.temp_dir = str(tmp_path / "temp")
    if cache is not None:
        it.globals["dm_motioncache"] = cache
    it.run_file(str(MS))
    return it


def _ro(it, name):
    return it.globals[name.lower()]


def _shown(it):
    return [r.name for r in _ro(it, "DaveMirraRollout").ctrl("subTabs").rollouts]


def test_ms_dialog_header_and_tabs(tmp_path):
    it = _dialog(tmp_path)
    d, models, levels = _ro(it, "DaveMirraRollout"), _ro(it, "DM_ModelsTab"), _ro(it, "DM_LevelsTab")
    assert d.is_open and d.host == "dialog"
    # the header: scale (real-world by default), LOD, exact normals, textures
    assert d.ctrl("chkReal").checked and not d.ctrl("spnScale").enabled
    assert d.ctrl("spnScale").value == pytest.approx(1 / 0.0254)              # Max's default unit: 1 inch
    assert d.ctrl("lblUnits").caption == "Scene: 1 unit = 1 in\n-> scale 39.37 (game 1 m = 39.37 units)"
    assert d.ctrl("spnLod").value == 0 and not d.ctrl("chkNormals").checked and d.ctrl("edtTex").caption == ""
    # the tab strip: a .NET TabControl with two pages; the Models tab rollout shown in the subRollout
    tabs = d.ctrl("tabs").net
    assert [p.Text for p in tabs.TabPages._pages] == ["Models", "Levels"] and tabs._index == 0
    assert _shown(it) == ["dm_modelstab"] and models.is_open and models.host is d.ctrl("subTabs")
    assert not levels.is_open
    # Models: Geometry (upright, bones, Import ZMD) and Animation (archive, motion, start frame, load)
    groups = {it_[1]: [c[2] for c in it_[2]] for it_ in models.items if it_[0] == "group"}
    assert groups == {"Geometry": ["chkupright", "ddlbones", "btnzmd"],
                      "Animation": ["btnzal", "lblzal", "ddlmotion", "chkchain", "spnstart", "btnmotion", "lbltarget"]}
    assert models.ctrl("chkUpright").checked and models.ctrl("ddlBones").items == list(dmi.BONE_MODES)
    assert models.ctrl("ddlMotion").items == [] and models.ctrl("lblZal").caption == "(no motion archive)"
    assert models.ctrl("chkChain").checked and not models.ctrl("spnStart").enabled and models.ctrl("spnStart").value == 0
    # Levels: flip normals and Import Level (only there)
    tabs.click_tab(1)
    assert _shown(it) == ["dm_levelstab"] and not models.is_open
    assert set(levels.controls) == {"chkflip", "btnlevel"} and not levels.ctrl("chkFlip").checked
    assert "chkflip" not in models.controls and "chkflip" not in d.controls
    assert it.messages == []


def test_ms_tabs_switch_and_keep_choices(bank, warm_cache, tmp_path):
    it = _dialog(tmp_path, warm_cache)
    d, models, levels = _ro(it, "DaveMirraRollout"), _ro(it, "DM_ModelsTab"), _ro(it, "DM_LevelsTab")
    tabs = d.ctrl("tabs").net
    models.ctrl("chkUpright").click()
    models.ctrl("ddlBones").choose(2)
    it.ui_answers = [str(_motion_zal())]
    models.ctrl("btnZal").press()
    models.ctrl("ddlMotion").choose(25)
    models.ctrl("chkChain").click()
    models.ctrl("spnStart").enter(40)
    d.ctrl("edtTex").type_text(str(tmp_path / "tex"))
    tabs.click_tab(1)
    levels.ctrl("chkFlip").click()
    with pytest.raises(MXSError, match="not open"):
        models.mxs_get("chkupright")                       # a closed rollout has no controls
    tabs.click_tab(0)
    assert _shown(it) == ["dm_modelstab"]
    # the Models rollout was rebuilt from its defaults, then restored from the session state
    assert not models.ctrl("chkUpright").checked and models.ctrl("ddlBones").selection == 2
    assert len(models.ctrl("ddlMotion").items) == 238 and models.ctrl("ddlMotion").selection == 25
    assert not models.ctrl("chkChain").checked and models.ctrl("spnStart").enabled
    assert models.ctrl("spnStart").value == 40
    tabs.click_tab(1)
    assert levels.ctrl("chkFlip").checked
    # the script run again (how the dialog is opened again in a session): last tab, textures, archive remembered
    it.run_file(str(MS))
    d2 = _ro(it, "DaveMirraRollout")
    assert d2 is not d and not d.is_open and d2.is_open
    assert d2.ctrl("tabs").net._index == 1 and _shown(it) == ["dm_levelstab"]
    assert d2.ctrl("edtTex").caption == str(tmp_path / "tex")
    d2.ctrl("tabs").net.click_tab(0)
    m2 = _ro(it, "DM_ModelsTab")
    assert m2.ctrl("lblZal").caption == str(_motion_zal()) and m2.ctrl("ddlMotion").selection == 25
    assert it.messages == []


def test_ms_dialog_imports_a_model_with_the_header_options(tmp_path):
    rt = Runtime()
    it = _dialog(tmp_path, rt=rt)
    d, models = _ro(it, "DaveMirraRollout"), _ro(it, "DM_ModelsTab")
    d.ctrl("chkReal").click()                              # manual scale
    assert d.ctrl("spnScale").enabled
    d.ctrl("spnScale").enter(100)
    assert "manual scale 100" in d.ctrl("lblUnits").caption
    d.ctrl("chkNormals").click()
    d.ctrl("edtTex").type_text(str(tmp_path / "tex"))
    models.ctrl("ddlBones").choose(2)
    it.ui_answers = [None]                                 # Cancel: nothing happens
    models.ctrl("btnZmd").press()
    assert not rt.nodes
    it.ui_answers = [str(_zmd("DaveMirra1"))]
    models.ctrl("btnZmd").press()
    assert it.messages == []
    root = next(n for n in rt.nodes if n.name == "DAVEMIRRA1")
    assert root.boxsize[0] == 10.0                         # 0.1 m at scale 100
    assert root.userprops["dmbmx_bones"] == "frame" and root.userprops["dmbmx_upright"] == "1"
    assert any(m.kind == "Edit_Normals" for n in rt.nodes if n.kind == "mesh" for m in n.modifiers)
    assert any((tmp_path / "tex").iterdir())               # the textures went to the chosen folder
    assert d.ctrl("lblInfo").caption.startswith("Imported DAVEMIRRA1. Textures: ")
    it.ui_answers = [str(tmp_path / "missing.zmd")]
    models.ctrl("btnZmd").press()                          # a failed import: a message, the dialog stays usable
    assert len(it.messages) == 1 and it.messages[0][0] == "Dave Mirra" and "missing.zmd" in it.messages[0][1]


def test_ms_dialog_levels_tab_passes_its_options(tmp_path):
    it = _dialog(tmp_path)
    d, levels = _ro(it, "DaveMirraRollout"), _ro(it, "DM_LevelsTab")
    calls = []

    def fake_level(folder, scale, texDir, exactNormals=False, flipNormals=False):
        calls.append((folder, scale, texDir, exactNormals, flipNormals))
        return [None] * 6 + ["texfolder"]
    it.globals["dm_importlevel"] = fake_level
    d.ctrl("tabs").net.click_tab(1)
    d.ctrl("chkNormals").click()
    levels.ctrl("chkFlip").click()
    it.ui_answers = [str(tmp_path / "AcclaimDirt")]
    levels.ctrl("btnLevel").press()
    assert calls == [(str(tmp_path / "AcclaimDirt"), pytest.approx(1 / 0.0254), "", True, True)]
    assert d.ctrl("lblInfo").caption == "Level imported. Textures: texfolder"
    it.ui_answers = [None]
    levels.ctrl("btnLevel").press()
    assert len(calls) == 1


def test_ms_dialog_chains_motions(bank, warm_cache, tmp_path):
    rt = Runtime()
    it = _dialog(tmp_path, warm_cache, rt)
    d, models = _ro(it, "DaveMirraRollout"), _ro(it, "DM_ModelsTab")
    na, nb = bank[A]["frames"], bank[B]["frames"]
    models.ctrl("btnMotion").press()
    assert it.messages == [("Dave Mirra", "Choose the motion archive (motion.zal) first.")]
    it.messages.clear()
    d.ctrl("chkReal").click()
    d.ctrl("spnScale").enter(chk.SCALE)
    it.ui_answers = [str(_zmd("DaveMirra1"))]
    models.ctrl("btnZmd").press()
    it.ui_answers = [str(_motion_zal())]
    models.ctrl("btnZal").press()
    entries = it.fn("DM_ListMotions")(str(_motion_zal()))
    assert models.ctrl("ddlMotion").items == [it.fn("DM_motionLabel")(e) for e in entries]
    assert len(entries) == 238 and models.ctrl("ddlMotion").selection == 1
    assert models.ctrl("spnStart").value == 0
    models.ctrl("ddlMotion").choose(A)
    models.ctrl("btnMotion").press()
    assert it.messages == [] and models.ctrl("spnStart").value == na
    assert d.ctrl("lblInfo").caption == f"Motion {A} on DAVEMIRRA1: frames 0 to {na - 1}"
    models.ctrl("ddlMotion").choose(B)
    models.ctrl("btnMotion").press()
    assert models.ctrl("spnStart").value == na + nb
    frames = [n for n in rt.nodes if "dmbmx_frame" in getattr(n, "userprops", {})]
    it2 = _dialog(tmp_path / "2", warm_cache)
    comb = it2.fn("DM_ImportZmdMotion")(str(_zmd("DaveMirra1")), chk.SCALE, 0, True, str(tmp_path), str(_motion_zal()), [B])
    for a, b in zip(frames, comb[1]):
        ka = dict(a.keys)
        for t, m in b.keys:
            assert ka[na + t].rows == m.rows and ka[na + t].pos == m.pos
    # an explicit start frame
    models.ctrl("chkChain").click()
    models.ctrl("spnStart").enter(150)
    models.ctrl("btnMotion").press()
    assert max(dict(frames[0].keys)) == 150 + nb - 1 and models.ctrl("spnStart").value == 150
    assert (rt.animationRange.start.frame, rt.animationRange.end.frame) == (0, 150 + nb - 1)
    # a second model and nothing selected: refused with a message
    it.ui_answers = [str(_zmd("DMbike01"))]
    models.ctrl("btnZmd").press()
    rt.selection = []
    models.ctrl("btnMotion").press()
    assert len(it.messages) == 1 and "there are 2 imported models in the scene" in it.messages[0][1]
    # a file that is not a motion archive: refused, the list stays
    bad = tmp_path / "bad.zal"
    bad.write_bytes(b"\0" * 64)
    it.ui_answers = [str(bad)]
    models.ctrl("btnZal").press()
    assert len(it.messages) == 2 and len(models.ctrl("ddlMotion").items) == 238


# ============================================================================ the interpreter's rollout rules

def _run(src, tmp_path):
    it = Interp(scene=Runtime(), stop_marker=None)
    for e in Parser(tokenize(src)).program():
        it.eval(e)
    return it


def test_interp_rollouts_follow_max(tmp_path):
    it = _run('global n = 0\n'
              'rollout R "R"\n(\n'
              '    checkbox chk "c" checked:true\n'
              '    spinner spn "s" range:[0, 10, 5] type:#integer\n'
              '    dropdownlist ddl "d" items:#("a", "b")\n'
              '    local v = 7\n'
              '    fn bump = (n += v)\n'
              '    on R open do bump()\n'
              '    on chk changed state do (v = 1)\n'
              ')\n'
              'createDialog R\n', tmp_path)
    r = it.globals["r"]
    assert it.globals["n"] == 7 and r.ctrl("spn").value == 5 and r.ctrl("ddl").selection == 1
    r.ctrl("chk").click()
    assert r.locals["v"] == 1 and not r.ctrl("chk").checked
    r.ctrl("spn").enter(99)
    assert r.ctrl("spn").value == 10                                   # clamped to the range
    for bad in ("R.spn.value = \"x\"", "R.ddl.selection = 3", "R.chk.checked = 1", "R.lbl.text = \"x\"",
                "R.spn.text2 = 1", "R.chk = 3"):
        with pytest.raises(MXSError):
            for e in Parser(tokenize(bad)).program():
                it.eval(e)
    with pytest.raises(MXSRuntimeError):
        it.builtins["createdialog"](r)                                 # open twice
    it.builtins["destroydialog"](r)
    assert not r.is_open
    with pytest.raises(MXSError, match="not open"):
        r.mxs_get("spn")
    it.builtins["createdialog"](r)
    assert r.ctrl("chk").checked and r.locals["v"] == 7 and it.globals["n"] == 14     # rebuilt from the defaults
    # destroyDialog of undefined is a run-time error a try/catch catches
    _run('try (destroyDialog undefined) catch ()\n', tmp_path)
    with pytest.raises(MXSRuntimeError):
        _run('destroyDialog undefined\n', tmp_path)
    # handlers: known events only, never more arguments than the event passes; no statements in a rollout
    for bad in ('rollout Q "Q" (\nbutton b "b"\non b clicked do ()\n)\ncreateDialog Q\n',
                'rollout Q "Q" (\nbutton b "b"\non b pressed x do ()\n)\ncreateDialog Q\n',
                'rollout Q "Q" (\ncheckbox c "c"\non c changed a b do ()\n)\ncreateDialog Q\n',
                'rollout Q "Q" (\non nope pressed do ()\n)\ncreateDialog Q\n',
                'rollout Q "Q" (\nbutton b "b" checked:true\n)\ncreateDialog Q\n',
                'rollout Q "Q" (\nif true then 1\n)\n',
                'rollout Q "Q" (\ndotNetControl t "System.Windows.Forms.ListView"\n)\ncreateDialog Q\n'):
        with pytest.raises(MXSError):
            _run(bad, tmp_path)


def test_interp_tab_control_and_subrollouts(tmp_path):
    it = _run('global log = #()\n'
              'rollout P1 "P1" (\non P1 open do append log "open1"\non P1 close do append log "close1"\n)\n'
              'rollout P2 "P2" (\non P2 open do append log "open2"\n)\n'
              'rollout M "M" (\n'
              '    dotNetControl tabs "System.Windows.Forms.TabControl" width:200 height:24\n'
              '    subRollout sub "" height:100\n'
              '    on tabs Selecting s e do append log "selecting"\n'
              '    on tabs Selected e do append log "selected"\n'
              '    on tabs SelectedIndexChanged do append log ("changed " + (tabs.SelectedIndex as string))\n'
              '    on M open do (\n'
              '        append log ("index " + (tabs.SelectedIndex as string))\n'
              '        tabs.TabPages.Add (dotNetObject "System.Windows.Forms.TabPage" "One")\n'
              '        tabs.TabPages.Add "Two"\n'
              '        append log ("index " + (tabs.SelectedIndex as string) + " of " + (tabs.TabPages.Count as string))\n'
              '        addSubRollout sub P1\n'
              '    )\n'
              ')\n'
              'createDialog M\n', tmp_path)
    m, p1, p2 = it.globals["m"], it.globals["p1"], it.globals["p2"]
    log = it.globals["log"]
    assert log == ["index -1", "index 0 of 2", "open1"]               # the first page selected, no event
    tabs = m.ctrl("tabs").net
    tabs.click_tab(1)
    assert log[3:] == ["selecting", "selected", "changed 1"]
    tabs.click_tab(1)
    assert len(log) == 6                                              # no change, no event
    with pytest.raises(MXSRuntimeError, match="ArgumentOutOfRange"):
        m.ctrl("tabs").mxs_set("selectedindex", 2)
    sub = m.ctrl("sub")
    with pytest.raises(MXSRuntimeError):
        it.builtins["removesubrollout"](sub, p2)                       # not in this subRollout
    with pytest.raises(MXSRuntimeError):
        it.builtins["addsubrollout"](sub, p1)                          # already open
    it.builtins["removesubrollout"](sub, p1)
    it.builtins["addsubrollout"](sub, p2)
    assert log[6:] == ["close1", "open2"] and sub.rollouts == [p2] and not p1.is_open
    it.builtins["destroydialog"](m)
    assert not p2.is_open and not m.is_open                          # the dialog closes its subrollouts


# ============================================================================ Python dialog (Qt)

@pytest.fixture
def qt(monkeypatch):
    q = QtMock().install(monkeypatch)
    monkeypatch.setattr(dmi, "SESSION", {"tab": 0, "texdir": "", "zal": ""})
    rt = Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    q.rt = rt
    yield q
    dmi.set_runtime(None, None)


def test_py_dialog_header_and_tabs(qt):
    d = dmi.show_dialog()
    assert d._shown and d.windowTitle() == f"Dave Mirra Importer {dmi.VERSION}"
    assert d.real.isChecked() and not d.scale.isEnabled() and d.scale.value() == pytest.approx(39.370079)
    assert d.units.text() == "Scene: 1 unit = 1 in -> scale 39.37 (game 1 m = 39.37 units)"
    assert d.lod.value() == 0 and not d.normals.isChecked() and d.texdir.text() == ""
    assert [d.tabs.tabText(i) for i in range(d.tabs.count())] == ["Models", "Levels"] and d.tabs.currentIndex() == 0
    models, levels = d.tabs.widget(0), d.tabs.widget(1)
    geo, anim = models._layout._items
    assert (geo.title(), anim.title()) == ("Geometry", "Animation")
    assert geo._layout._items == [d.upright, d.bones, d.btn_zmd]
    assert anim._layout._items[:6] == [d.btn_zal, d.zal, d.motion, d.chain, d.start, d.btn_motion]
    assert levels._layout._items[:2] == [d.flip, d.btn_level]
    assert d.upright.isChecked() and [d.bones.itemText(i) for i in range(2)] == list(dmi.BONE_MODES)
    assert d.motion.count() == 0 and d.chain.isChecked() and not d.start.isEnabled() and d.start.value() == 0
    assert not d.flip.isChecked() and qt.messages == []


def test_py_dialog_tabs_and_session(qt, tmp_path):
    d = dmi.show_dialog()
    d.tabs.click_tab(1)
    d.texdir.type_text(str(tmp_path))
    qt.answers = [str(_motion_zal())]
    d.btn_zal.click()
    assert d.motion.count() == 238 and dmi.SESSION == {"tab": 1, "texdir": str(tmp_path), "zal": str(_motion_zal())}
    d2 = dmi.show_dialog()                                            # opened again in the same session
    assert d2.tabs.currentIndex() == 1 and d2.texdir.text() == str(tmp_path)
    assert d2.motion.count() == 238 and d2.zal.text() == str(_motion_zal())
    qt.answers = [str(tmp_path)]
    d2.btn_tex.click()
    assert d2.texdir.text() == str(tmp_path) and dmi.SESSION["texdir"] == str(tmp_path)
    dmi.SESSION["zal"] = str(tmp_path / "gone.zal")
    d3 = dmi.show_dialog()                                            # a remembered archive that is gone: forgotten
    assert d3.motion.count() == 0 and dmi.SESSION["zal"] == "" and qt.messages == []


def test_py_dialog_imports_with_the_header_options(qt, tmp_path, monkeypatch):
    d = dmi.show_dialog()
    d.real.click()
    d.scale.enter(100.0)
    d.normals.click()
    d.texdir.type_text(str(tmp_path))
    d.bones.choose(1)
    d.upright.click()
    qt.answers = [str(_zmd("DMbike01"))]
    d.btn_zmd.click()
    assert qt.messages[-1][:2] == ("information", "Dave Mirra")
    root = next(n for n in qt.rt.nodes if n.name == "HIPSDECK")
    assert root.boxsize[0] == 10.0 and root.userprops["dmbmx_upright"] == "0"
    calls = []
    monkeypatch.setattr(dmi, "import_level", lambda *a, **kw: calls.append((a, kw)) or {"texture_dir": "t"})
    d.tabs.click_tab(1)
    d.flip.click()
    qt.answers = [str(tmp_path / "AcclaimDirt")]
    d.btn_level.click()
    assert calls == [((str(tmp_path / "AcclaimDirt"),), {"scale": 100.0, "texture_dir": str(tmp_path),
                                                         "explicit_normals": True, "flip_normals": True})]
    qt.answers = [None]
    d.btn_level.click()
    assert len(calls) == 1
    qt.answers = [str(tmp_path / "missing.zmd")]
    d.btn_zmd.click()
    assert qt.messages[-1][0] == "critical"


def test_py_dialog_chains_motions(qt, bank, tmp_path):
    na, nb = bank[A]["frames"], bank[B]["frames"]
    d = dmi.show_dialog()
    d.btn_motion.click()
    assert qt.messages == [("warning", "Dave Mirra", "Choose the motion archive (motion.zal) first.")]
    d.real.click()
    d.scale.enter(chk.SCALE)
    d.texdir.type_text(str(tmp_path))
    qt.answers = [str(_zmd("DaveMirra1"))]
    d.btn_zmd.click()
    qt.answers = [str(_motion_zal())]
    d.btn_zal.click()
    entries = dmi.list_motions(str(_motion_zal()))
    assert [d.motion.itemText(i) for i in range(d.motion.count())] == [dmi.motion_label(e) for e in entries]
    assert d.start.value() == 0
    d.motion.choose(A - 1)
    d.btn_motion.click()
    assert d.start.value() == na and d.info.text() == f"Motion {A} on DAVEMIRRA1: frames 0 to {na - 1}"
    d.motion.choose(B - 1)
    d.btn_motion.click()
    assert d.start.value() == na + nb
    frames = [n for n in qt.rt.nodes if "dmbmx_frame" in getattr(n, "userprops", {})]
    rt2 = Runtime()
    dmi.set_runtime(rt2, PymxsModule(rt2))
    comb = dmi.import_zmd(str(_zmd("DaveMirra1")), scale=chk.SCALE, texture_dir=str(tmp_path),
                          motion_path=str(_motion_zal()), motion_ids=[B])
    dmi.set_runtime(qt.rt, PymxsModule(qt.rt))
    for a, b in zip(frames, comb["frames"]):
        ka = dict(a.keys)
        for t, m in b.keys:
            assert ka[na + t].rows == m.rows and ka[na + t].pos == m.pos
    d.chain.click()
    assert d.start.isEnabled()
    d.start.enter(150)
    d.btn_motion.click()
    assert max(dict(frames[0].keys)) == 150 + nb - 1 and d.start.value() == 150
    qt.answers = [str(_zmd("DMbike01"))]
    d.btn_zmd.click()
    qt.rt.selection = []
    n = len(qt.messages)
    d.btn_motion.click()
    assert qt.messages[n][0] == "critical" and "there are 2 imported models in the scene" in qt.messages[n][2]


def test_qt_mock_follows_qt():
    q = QtMock()
    W = q.QtWidgets
    sb, dsb = W.QSpinBox(), W.QDoubleSpinBox()
    assert (sb.value(), dsb.value()) == (0, 0.0)
    sb.setValue(500)
    assert sb.value() == 99                                            # default range 0..99
    with pytest.raises(AssertionError):
        sb.setValue(1.5)                                               # QSpinBox takes ints
    dsb.setRange(0.0, 1000.0)
    dsb.setValue(39.370078740157)
    assert dsb.value() == 39.37                                        # 2 decimals by default
    dsb.setDecimals(6)
    dsb.setValue(39.370078740157)
    assert dsb.value() == 39.370079
    got = []
    cb = W.QCheckBox("x")
    cb.toggled.connect(lambda: got.append("no-arg slot"))
    cb.toggled.connect(lambda b: got.append(b))
    cb.setChecked(False)                                               # no change, no signal
    cb.setChecked(True)
    cb.blockSignals(True)
    cb.setChecked(False)
    cb.blockSignals(False)
    assert got == ["no-arg slot", True]
    cb.setEnabled(False)
    with pytest.raises(AssertionError):
        cb.click()                                                     # a disabled widget ignores the user
    combo, tw = W.QComboBox(), W.QTabWidget()
    idx = []
    combo.currentIndexChanged.connect(idx.append)
    combo.addItems(["a", "b"])
    combo.setCurrentIndex(5)
    combo.clear()
    assert idx == [0, -1, ] and combo.currentIndex() == -1
    tw.addTab(W.QWidget(), "A")
    tw.addTab(W.QWidget(), "B")
    tw.setCurrentIndex(7)                                              # out of range: ignored
    assert tw.currentIndex() == 0
    lay, w = W.QVBoxLayout(), W.QLabel("l")
    lay.addWidget(w)
    with pytest.raises(AssertionError):
        W.QVBoxLayout().addWidget(w)                                   # one layout per widget
    host = W.QWidget()
    W.QVBoxLayout(host)
    with pytest.raises(AssertionError):
        W.QFormLayout(host)                                            # one layout per widget
    with pytest.raises(AssertionError):
        W.QFileDialog.getOpenFileName(None, "x")                       # the test must answer
    q.answers = [None]
    assert W.QFileDialog.getExistingDirectory(None, "x") == ""         # Cancel
    with pytest.raises(AttributeError):
        W.QTreeView                                                    # not modelled

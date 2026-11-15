"""A motion from motion.zal onto a model already in the scene, in both 3ds Max importers
(dave_mirra_import.import_motion / list_motions and DaveMirraImport.ms DM_ImportMotion / DM_ListMotions).

- motions chained back to back on a model imported without motions give exactly the keys of the combined import
  (import_zmd with motion ids / DM_ImportZmdMotion) for each motion, shifted by its start frame: per key and per
  bone, for bones aimed at their children and for game-frame bones, riders and bikes;
- the default start frame is the frame after the model's last key; the animation range grows and never shrinks;
- wrong targets (no model, several, a foreign node, a missing bone, a model without rider/bike bones) are refused;
- the motion list holds every motion of the archive, the same in both importers;
- the mock rules the motion import relies on (controllers and key times, animationRange, user properties)."""
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
from max_mock import M3, Interval, MxsTime, PymxsModule, Runtime  # noqa: E402
from maxscript_interp import Interp, MXSError, MXSRuntimeError, Thrown, arith  # noqa: E402

A, B = 8, 15                    # pedal (36 frames), take-off (34 frames)


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
def warm_cache(tmp_path_factory):
    """motion.zal decoded once by the MAXScript decoder (DM_motionCache), handed to each test's interpreter
    as in one 3ds Max session."""
    it = Interp(scene=Runtime())
    it.run_file(str(MS))
    it.fn("DM_motionGroups")(str(_motion_zal()))
    return it.globals["dm_motioncache"]


@pytest.fixture
def py():
    rt = Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    yield rt
    dmi.set_runtime(None, None)


def _ms(rt, tmp_path, cache=None):
    it = Interp(scene=rt)
    it.temp_dir = str(tmp_path / "temp")
    it.run_file(str(MS))
    if cache is not None:
        it.globals["dm_motioncache"] = cache
    return it


def _keys(node):
    """{time: matrix} (the last key set at each time)."""
    return dict(node.keys)


def _same(a, b):
    return a.rows == b.rows and a.pos == b.pos


def _py_combined(stem, mid, tmp_path, aim=True, scale=chk.SCALE):
    rt = Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    try:
        return dmi.import_zmd(str(_zmd(stem)), scale=scale, texture_dir=str(tmp_path / "comb"),
                              motion_path=str(_motion_zal()), motion_ids=[mid], bone_aim_children=aim)
    finally:
        dmi.set_runtime(None, None)


def _check_chained(frames, combined, start):
    """frames keyed by the motion import from start = combined's keys shifted by start, exactly."""
    assert [n.name for n in frames] == [n.name for n in combined]
    for a, b in zip(frames, combined):
        ka = _keys(a)
        assert b.keys
        for t, m in b.keys:
            assert _same(ka[start + t], m), (a.name, t)


# ============================================================================ chained = combined, shifted

@pytest.mark.parametrize("stem,aim", [("DaveMirra1", True), ("DaveMirra1", False), ("DMbike01", True)])
def test_py_chained_motions_equal_combined_import(stem, aim, py, bank, tmp_path):
    zal = str(_motion_zal())
    s = dmi.import_zmd(str(_zmd(stem)), scale=chk.SCALE, texture_dir=str(tmp_path), bone_aim_children=aim)
    assert not any(n.keys for n in s["frames"])
    a = dmi.import_motion(zal, A)
    b = dmi.import_motion(zal, B)
    na, nb = bank[A]["frames"], bank[B]["frames"]
    assert (a["start"], a["end"], b["start"], b["end"]) == (0, na - 1, na, na + nb - 1)
    assert a["keys"] == na * len(s["frames"]) and b["keys"] == nb * len(s["frames"])
    assert a["root"] is s["root"] and a["frames"] == s["frames"]
    for mid, start in ((A, 0), (B, na)):
        _check_chained(s["frames"], _py_combined(stem, mid, tmp_path, aim)["frames"], start)
    for n in s["frames"]:
        assert sorted(_keys(n)) == list(range(na + nb))               # nothing else keyed
    for m in s["meshes"]:
        assert not m.keys


@pytest.mark.parametrize("stem,aim", [("DaveMirra1", True), ("DaveMirra1", False), ("DMbike01", True)])
def test_ms_chained_motions_equal_combined_import(stem, aim, bank, warm_cache, tmp_path):
    zal = str(_motion_zal())
    it = _ms(Runtime(), tmp_path, warm_cache)
    root, frames, meshes, _ = it.fn("DM_ImportZmd")(str(_zmd(stem)), chk.SCALE, 0, True, str(tmp_path), boneAim=aim)
    ra = it.fn("DM_ImportMotion")(zal, A)
    rb = it.fn("DM_ImportMotion")(zal, B)
    na, nb = bank[A]["frames"], bank[B]["frames"]
    assert ra[0] is root and ra[1:] == [0, na - 1, na * len(frames)]
    assert rb[1:] == [na, na + nb - 1, nb * len(frames)]
    for mid, start in ((A, 0), (B, na)):
        it2 = _ms(Runtime(), tmp_path, warm_cache)
        comb = it2.fn("DM_ImportZmdMotion")(str(_zmd(stem)), chk.SCALE, 0, True, str(tmp_path), zal, [mid],
                                            boneAim=aim)
        _check_chained(frames, comb[1], start)
    for n in frames:
        assert sorted(_keys(n)) == list(range(na + nb))


def test_py_exact_at_any_scale(py, bank, tmp_path):
    """The scale goes through the user property text whole (repr), so the keys stay exact at a scale a 32-bit
    float cannot hold (1 inch: 39.37007874015748); getUserProp would round it."""
    scale = 1 / 0.0254
    zal = str(_motion_zal())
    s = dmi.import_zmd(str(_zmd("DaveMirra1")), scale=scale, texture_dir=str(tmp_path))
    assert s["root"].userprops["dmbmx_scale"] == repr(scale)
    assert py.getUserProp(s["root"], "dmbmx_scale") != scale          # what getUserProp would give
    dmi.import_motion(zal, B, start_frame=7)
    _check_chained(s["frames"], _py_combined("DaveMirra1", B, tmp_path, scale=scale)["frames"], 7)


def test_ms_and_py_motion_imports_agree(bank, warm_cache, tmp_path):
    """The MAXScript motion import = the Python one (key by key, as the combined imports are compared)."""
    zal = str(_motion_zal())
    rt = Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    try:
        s = dmi.import_zmd(str(_zmd("DaveMirra1")), scale=chk.SCALE, texture_dir=str(tmp_path))
        dmi.import_motion(zal, A)
        dmi.import_motion(zal, B)
    finally:
        dmi.set_runtime(None, None)
    it = _ms(Runtime(), tmp_path, warm_cache)
    _, frames, _, _ = it.fn("DM_ImportZmd")(str(_zmd("DaveMirra1")), chk.SCALE, 0, True, str(tmp_path))
    it.fn("DM_ImportMotion")(zal, A)
    it.fn("DM_ImportMotion")(zal, B)
    for a, b in zip(frames, s["frames"]):
        assert [t for t, _ in a.keys] == [t for t, _ in b.keys]
        for (_, ma), (_, mb) in zip(a.keys, b.keys):
            assert all(abs(x - y) < 1e-5 for x, y in zip(sum(ma.rows, ()), sum(mb.rows, ())))
            assert all(abs(x - y) < 1e-5 * chk.SCALE for x, y in zip(ma.pos, mb.pos))


def test_motion_import_reads_either_importers_tags(bank, warm_cache, tmp_path):
    """The model tags are one contract: the Python motion import keys a rider the MAXScript importer built, and
    the other way round, with the same keys."""
    zal = str(_motion_zal())
    rt = Runtime()
    it = _ms(rt, tmp_path, warm_cache)
    root, frames, _, _ = it.fn("DM_ImportZmd")(str(_zmd("DaveMirra1")), chk.SCALE, 0, True, str(tmp_path))
    props = root.userprops
    assert props["dmbmx_zmd"] == str(_zmd("DaveMirra1")) and props["dmbmx_lod"] == "0"
    assert props["dmbmx_scale"] == "100" and props["dmbmx_bones"] == "aim" and props["dmbmx_upright"] == "1"
    assert [n.userprops["dmbmx_frame"] for n in frames] == [str(i) for i in range(len(frames))]
    dmi.set_runtime(rt, PymxsModule(rt))
    try:
        r = dmi.import_motion(zal, A)
    finally:
        dmi.set_runtime(None, None)
    assert r["root"] is root and r["start"] == 0
    _check_chained(frames, _py_combined("DaveMirra1", A, tmp_path)["frames"], 0)

    rt2 = Runtime()
    dmi.set_runtime(rt2, PymxsModule(rt2))
    try:
        s = dmi.import_zmd(str(_zmd("DMbike01")), scale=chk.SCALE, texture_dir=str(tmp_path))
    finally:
        dmi.set_runtime(None, None)
    assert s["root"].userprops["dmbmx_scale"] == "100.0" and s["root"].userprops["dmbmx_bones"] == "frame"
    it2 = _ms(rt2, tmp_path, warm_cache)
    it2.fn("DM_ImportMotion")(zal, B, startFrame=3)
    it3 = _ms(Runtime(), tmp_path, warm_cache)                            # the MAXScript combined import
    comb = it3.fn("DM_ImportZmdMotion")(str(_zmd("DMbike01")), chk.SCALE, 0, True, str(tmp_path), zal, [B])
    _check_chained(s["frames"], comb[1], 3)


# ============================================================================ start frame and animation range

def _both(tmp_path, warm_cache, stem="DaveMirra1"):
    """(name, import_model([stem]) -> frame nodes, import_motion(id, start, target) -> (start, end), runtime) for
    each importer, each on its own scene."""
    out = []
    rt = Runtime()

    def py_model(st=stem):
        dmi.set_runtime(rt, PymxsModule(rt))
        return dmi.import_zmd(str(_zmd(st)), scale=chk.SCALE, texture_dir=str(tmp_path))["frames"]

    def py_motion(mid, start=None, target=None):
        dmi.set_runtime(rt, PymxsModule(rt))
        r = dmi.import_motion(str(_motion_zal()), mid, start_frame=start, target=target)
        return r["start"], r["end"]
    out.append(("py", py_model, py_motion, rt))
    rt2 = Runtime()
    it = _ms(rt2, tmp_path, warm_cache)

    def ms_model(st=stem):
        return it.fn("DM_ImportZmd")(str(_zmd(st)), chk.SCALE, 0, True, str(tmp_path))[1]

    def ms_motion(mid, start=None, target=None):
        r = it.fn("DM_ImportMotion")(str(_motion_zal()), mid, startFrame=start, target=target)
        return r[1], r[2]
    out.append(("ms", ms_model, ms_motion, rt2))
    return out


def test_default_start_chains_after_the_last_key(bank, warm_cache, tmp_path):
    na, nb = bank[A]["frames"], bank[B]["frames"]
    try:
        for name, model, motion, rt in _both(tmp_path, warm_cache):
            frames = model()
            assert motion(A) == (0, na - 1), name                         # no keys yet: from frame 0
            assert motion(B, start=200) == (200, 200 + nb - 1), name      # an explicit start
            assert motion(A) == (200 + nb, 200 + nb + na - 1), name       # after the last key, wherever it is
            assert motion(B, start=10) == (10, 9 + nb), name              # over existing keys: they are replaced
            assert motion(A) == (200 + nb + na, 200 + nb + 2 * na - 1), name
            assert max(_keys(frames[0])) == 200 + nb + 2 * na - 1
    finally:
        dmi.set_runtime(None, None)


def test_first_motion_at_a_later_frame_keeps_the_bind_pose_at_0(bank, warm_cache, tmp_path):
    """3ds Max keys frame 0 with the old value when a node is first keyed elsewhere (the mock does too): the
    bind pose stays at frame 0 and the next default start is after the motion."""
    na = bank[A]["frames"]
    try:
        for name, model, motion, rt in _both(tmp_path, warm_cache):
            frames = model()
            bind = [n.transform for n in frames]
            assert motion(A, start=50) == (50, 49 + na), name
            for n, m in zip(frames, bind):
                assert sorted(_keys(n)) == [0] + list(range(50, 50 + na)) and _keys(n)[0] is m
            assert motion(B) == (50 + na, 49 + na + bank[B]["frames"]), name
    finally:
        dmi.set_runtime(None, None)


def test_animation_range_grows_and_never_shrinks(bank, warm_cache, tmp_path):
    na, nb = bank[A]["frames"], bank[B]["frames"]

    def rng(rt):
        return rt.animationRange.start.frame, rt.animationRange.end.frame
    try:
        for name, model, motion, rt in _both(tmp_path, warm_cache):
            model()
            assert rng(rt) == (0, 100)
            motion(A)                                       # 0..35: inside 0..100
            assert rng(rt) == (0, 100), name
            motion(B, start=90)                             # 90..123
            assert rng(rt) == (0, 89 + nb), name
            motion(A, start=-10)                            # -10..25
            assert rng(rt) == (-10, 89 + nb), name
            motion(A, start=5)                              # inside: unchanged
            assert rng(rt) == (-10, 89 + nb), name
            motion(B)                                       # after the last key (123): 124..157
            assert rng(rt) == (-10, 90 + 2 * nb - 1), name
    finally:
        dmi.set_runtime(None, None)


def test_moved_root_carries_the_keys(bank, py, tmp_path):
    """A model moved after its import gets the keys moved with it (world keys = combined keys x the move)."""
    s = dmi.import_zmd(str(_zmd("DaveMirra1")), scale=chk.SCALE, texture_dir=str(tmp_path))
    root = s["root"]
    move = M3((0, 1, 0), (-1, 0, 0), (0, 0, 1), (250.0, -40.0, 12.5))      # turned 90 degrees and moved
    root.transform = arith("*", root.transform, move)
    dmi.import_motion(str(_motion_zal()), A)
    comb = _py_combined("DaveMirra1", A, tmp_path)["frames"]
    for a, b in zip(s["frames"], comb):
        for t, m in b.keys:
            got = _keys(a)[t]
            want = arith("*", m, move)
            assert all(abs(x - y) < 1e-9 for x, y in zip(sum(got.rows, ()), sum(want.rows, ())))
            assert all(abs(x - y) < 1e-6 for x, y in zip(got.pos, want.pos))


# ============================================================================ targets

def test_targets_are_found_or_refused(bank, warm_cache, tmp_path):
    na, nb = bank[A]["frames"], bank[B]["frames"]
    for name, model, motion, rt in _both(tmp_path, warm_cache):
        err = (ValueError, Thrown)
        try:
            with pytest.raises(err, match="no imported rider or bike in the scene"):
                motion(A)
            rider = model()
            rt.selection = []
            assert motion(A) == (0, na - 1)                          # the only model in the scene
            rt.selection = [rider[0].parent]                         # its root
            assert motion(A) == (na, 2 * na - 1)
            bike = model("DMbike01")
            rt.selection = []
            with pytest.raises(err, match="there are 2 imported models in the scene"):
                motion(A)
            rt.selection = [rider[3], bike[2]]
            with pytest.raises(err, match="the selection belongs to several imported models"):
                motion(A)
            rt.selection = [bike[2], bike[0], bike[0].parent]       # nodes of one model: that one
            assert motion(B) == (0, nb - 1), name
            assert all(n.keys for n in bike) and max(_keys(rider[0])) == 2 * na - 1
            rt.selection = [rt.Dummy(name="other", boxsize=rt.Point3(1, 1, 1))] + [rider[4]]   # not a model: ignored
            assert motion(B) == (2 * na, 2 * na + nb - 1)
            assert motion(A, target=bike[5]) == (nb, nb + na - 1)   # target: wins over the selection
            stray = rt.Dummy(name="stray", boxsize=rt.Point3(1, 1, 1))
            with pytest.raises(err, match="stray is not part of a model imported by this importer"):
                motion(A, target=stray)
        finally:
            dmi.set_runtime(None, None)


def test_refuses_a_broken_or_foreign_rig(warm_cache, tmp_path):
    zal = str(_motion_zal())
    for name, model, motion, rt in _both(tmp_path, warm_cache):
        err = (ValueError, Thrown)
        try:
            frames = model()
            rt.nodes.remove(frames[7])                                 # a deleted bone
            with pytest.raises(err, match="frame 7 of the model is not in the scene"):
                motion(A)
            rt.nodes.append(frames[7])
            rt.setUserProp(frames[9], "dmbmx_frame", "7")                # two nodes claim one frame
            with pytest.raises(err, match="are both frame 7"):
                motion(A)
            rt.setUserProp(frames[9], "dmbmx_frame", "99")
            with pytest.raises(err, match="is frame 99, but the model file has 20 frames"):
                motion(A)
            rt.setUserProp(frames[9], "dmbmx_frame", "9")
            root = frames[0].parent
            rt.setUserProp(root, "dmbmx_zmd", str(tmp_path / "gone.zmd"))
            with pytest.raises(err, match="is not at .*gone.zmd"):
                motion(A)
            assert not any(n.keys for n in frames)
        finally:
            dmi.set_runtime(None, None)


@pytest.mark.parametrize("stem", ["rider_shadow", "sfx"])
def test_refuses_a_model_without_rider_or_bike_bones(stem, warm_cache, tmp_path):
    """rider_shadow is a skinned rider whose bones are Hips01, Chest01, ...: not the motion's skeleton."""
    for name, model, motion, rt in _both(tmp_path, warm_cache, stem):
        try:
            frames = model()
            with pytest.raises((ValueError, Thrown), match="animates riders and bikes, and .* has none of their bones"):
                motion(A)
            assert not any(n.keys for n in frames)
        finally:
            dmi.set_runtime(None, None)


def test_refuses_a_missing_motion_or_archive(warm_cache, tmp_path):
    for name, model, motion, rt in _both(tmp_path, warm_cache):
        try:
            model()
            with pytest.raises((ValueError, Thrown), match="motion id 239 is not in motion.zal"):
                motion(239)
        finally:
            dmi.set_runtime(None, None)


# ============================================================================ the motion list

def test_motion_list_has_every_motion(bank, warm_cache, tmp_path):
    zal = str(_motion_zal())
    py_list = dmi.list_motions(zal)
    assert [e["id"] for e in py_list] == list(range(1, 239)) == list(bank)
    assert [e["frames"] for e in py_list] == [bank[m]["frames"] for m in bank]
    it = _ms(Runtime(), tmp_path, warm_cache)
    ms_list = it.fn("DM_ListMotions")(zal)
    assert ms_list == [[e["id"], e["frames"], e["name"]] for e in py_list]
    labels = [dmi.motion_label(e) for e in py_list]
    assert [it.fn("DM_motionLabel")(e) for e in ms_list] == labels
    assert labels[24] == "25: turndown (start), %d frames" % bank[25]["frames"]
    assert all(e["name"] for e in py_list)


def test_motion_names(warm_cache, tmp_path):
    it = _ms(Runtime(), tmp_path, warm_cache)
    want = {1: "not used by the game", 2: "coast (standing still)", 8: "pedal", 15: "take-off (hop)",
            24: "air idle", 25: "turndown (start)", 27: "turndown (release)", 34: "backflip (start)",
            70: "superman indian (start)", 72: "superman indian (release)", 73: "barspin modifier (start)",
            186: "busdriver modifier (release)", 187: "double peg grind (enter)", 189: "double peg grind (exit)",
            231: "disaster (exit)", 232: "fast plant (enter)", 237: "wall tap (exit)", 238: "not used by the game",
            0: "", 239: "", 1000: ""}
    for m, name in want.items():
        assert dmi.motion_name(m) == name and it.fn("DM_motionName")(m) == name, m
    for m in range(-2, 242):
        assert it.fn("DM_motionName")(m) == dmi.motion_name(m)
    assert dmi.motion_label({"id": 300, "frames": 4, "name": ""}) == "300: 4 frames"
    assert it.fn("DM_motionLabel")([300, 4, ""]) == "300: 4 frames"


# ============================================================================ mock rules

def test_mock_controllers_and_key_times():
    rt = Runtime()
    pm = PymxsModule(rt)
    n = rt.Dummy(name="n", boxsize=rt.Point3(1, 1, 1))
    m = M3((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))
    n.transform = m
    c = n.controller
    assert c.numKeys() == -1 and c.numSubs == 3                       # PRS: keys only on the leaves
    pos = c.subanim(1).controller
    assert pos.numKeys() == -1 and pos.numSubs == 3
    x = pos.subanim(1).controller
    assert x.numKeys() == 0 and x.numSubs == 0
    with pytest.raises(AssertionError):
        c.getKeyTime(1)
    with pytest.raises(AssertionError):
        c.subanim(4)
    with pm.animate(True):
        with pm.attime(12):
            n.transform = m
        with pm.attime(3):
            n.transform = m
    assert [t for t, _ in n.keys] == [0, 12, 3]                       # frame 0 keyed with the old value
    assert x.numKeys() == 3 and [x.getKeyTime(i).frame for i in (1, 2, 3)] == [0, 3, 12]
    assert c.subanim(3).controller.numKeys() == 3
    with pytest.raises(AssertionError):
        x.getKeyTime(4)
    t = MxsTime(12)
    assert t.frame == 12.0 and int(t) == 12 * 160 and t == MxsTime(12.0) and t != 12
    with pytest.raises(AssertionError):
        rt.StandardMaterial("m").controller                             # materials have no controller


def test_mock_user_props_and_scene_lists():
    rt = Runtime()
    n = rt.Dummy(name="n", boxsize=rt.Point3(1, 1, 1))
    rt.setUserProp(n, "dmbmx_zmd", r"C:\Games\Dave Mirra\assets\DaveMirra1.ZMD")
    rt.setUserProp(n, "dmbmx_lod", "0")
    rt.setUserProp(n, "dmbmx_scale", "39.37007874015748")
    rt.setUserProp(n, "dmbmx_bones", "aim")
    assert rt.getUserProp(n, "DMBMX_LOD") == 0                          # numbers come back as numbers
    assert rt.getUserProp(n, "dmbmx_scale") == pytest.approx(39.370079, abs=1e-6)
    assert rt.getUserProp(n, "dmbmx_scale") != 39.37007874015748        # a 32-bit MAXScript Float
    assert rt.getUserProp(n, "dmbmx_bones") == "aim" and rt.getUserProp(n, "nope") is None
    assert rt.getUserPropBuffer(n) == ("dmbmx_zmd = C:\\Games\\Dave Mirra\\assets\\DaveMirra1.ZMD\r\n"
                                       "dmbmx_lod = 0\r\ndmbmx_scale = 39.37007874015748\r\ndmbmx_bones = aim\r\n")
    for bad in (1.5, True, None, "a\nb"):
        with pytest.raises(AssertionError):
            rt.setUserProp(n, "k", bad)
    b = rt.BoneSys.createBone(rt.Point3(0, 0, 0), rt.Point3(1, 0, 0), rt.Point3(0, 0, 1))
    assert rt.objects == [n, b]                                          # bones are scene nodes too


def test_interp_motion_builtins(tmp_path):
    rt = Runtime()
    f = tmp_path / "t.ms"
    f.write_text('global n = dummy name:"n" boxsize:[1, 1, 1]\n'
                 'fn keyAt t = (animate on (at time t (n.transform = matrix3 [1,0,0] [0,1,0] [0,0,1] [0,0,0])))\n'
                 'fn last = (getKeyTime n.controller[1].controller[2].controller (numKeys n.controller[1].controller[2].controller))\n'
                 'fn rng = (#(animationRange.start.frame, animationRange.end.frame))\n'
                 'fn setRange a b = (animationRange = interval a b)\n'
                 'fn fl x = (floor x)\n'
                 'fn inv m = (inverse m)\n'
                 'fn props = (#(getUserProp n "k", getUserProp n "f"))\n', encoding="utf-8")
    it = Interp(scene=rt)
    it.run_file(str(f))
    it.globals["n"].transform = M3((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))
    it.fn("keyAt")(4)
    it.fn("keyAt")(9)
    t = it.fn("last")()
    assert isinstance(t, MxsTime) and t.frame == 9.0
    assert it.fn("rng")() == [0.0, 100.0]
    it.fn("setRange")(-5, 120)
    assert it.fn("rng")() == [-5.0, 120.0]
    with pytest.raises(MXSRuntimeError):
        it.fn("setRange")(10, 10)                                       # an empty range: refused
    assert it.fn("fl")(2.7) == 2.0 and it.fn("fl")(-2.5) == -3.0
    m = it.fn("inv")(M3((2, 0, 0), (0, 1, 0), (0, 0, 1), (4, 0, 0)))
    assert m.rows[0] == (0.5, 0.0, 0.0) and m.pos == (-2.0, 0.0, 0.0)
    rt.setUserProp(it.globals["n"], "k", "12")
    rt.setUserProp(it.globals["n"], "f", "frame")
    assert it.fn("props")() == [12, "frame"]
    for bad in ('fn g = (animationRange = 5)\n', 'fn g = (objects = 5)\n', 'fn g = (numKeys 3)\n',
                'fn g = (getKeyTime n.controller 1)\n', 'fn g = (n.controller[4])\n'):
        f.write_text('global n = dummy name:"n" boxsize:[1, 1, 1]\n' + bad, encoding="utf-8")
        it = Interp(scene=Runtime())
        it.run_file(str(f))
        with pytest.raises((MXSError, AssertionError)):
            it.fn("g")()


def test_python_runs_its_maxscript_helpers_in_the_interpreter(py):
    """import_motion reads key times and sets the range in MAXScript (rt.execute): the same code as the .ms."""
    src = MS.read_text(encoding="utf-8")
    for py_src, ms_name in ((dmi.MXS_LAST_KEY_TIME, "DM_lastKeyTime"), (dmi.MXS_GROW_RANGE, "DM_growRange")):
        ms_fn = src[src.index(f"fn {ms_name} "):]
        ms_fn = ms_fn[:ms_fn.index("\n)\n") + 2]
        assert py_src == ms_fn.replace(ms_name, "dmbmx_" + ms_name[3].lower() + ms_name[4:])
    assert dmi.grow_range(10, 150) is True and py.animationRange.end.frame == 150
    assert dmi.grow_range(10, 150) is False
    with pytest.raises(AssertionError):
        py.execute("max quick render")

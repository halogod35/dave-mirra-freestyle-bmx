"""Motion clips in the pure-MAXScript importer (tools/3dsmax/maxscript/DaveMirraImport.ms), run by the
MAXScript-subset interpreter (tests/maxscript_interp.py) because 3ds Max is not installed.

- its LZO1X / ZAL / motion decoder must equal dmparse on all 238 clips, frame by frame and channel by channel;
- keyed rider and bike imports must give the same keys (node, frame, transform) as the Python importer
  (tools/3dsmax/python/dave_mirra_import.py) under its pymxs mock."""
import math
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
from max_mock import PymxsModule, Runtime  # noqa: E402
from maxscript_interp import Interp, MXSError, Thrown  # noqa: E402

ROT_TOL = 1e-5                 # acceptance: rotation rows within 1e-5
POS_TOL = 1e-5 * chk.SCALE


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
def ms(tmp_path_factory):
    """One interpreter for the module: motion.zal is decoded once and cached in DM_motionCache, as in a
    3ds Max session."""
    it = Interp(scene=Runtime())
    it.temp_dir = str(tmp_path_factory.mktemp("temp"))
    it.run_file(str(MS))
    return it


@pytest.fixture(scope="module")
def ms_bank(ms):
    mz = _motion_zal()
    ids, clips = ms.fn("DM_readMotionBank")(str(mz))
    return dict(zip(ids, clips))


@pytest.fixture(scope="module")
def py_bank():
    return dmparse.read_motion_bank(_motion_zal().read_bytes())


# ---------------------------------------------------------------------------- decoder = dmparse

def test_ms_lzo_and_zal_motion(ms):
    mz = _motion_zal()
    data = mz.read_bytes()
    outer = dmparse.read_zal(data)
    inner = dmparse.read_zal(outer["groups"][0]["data"], headerless=True)
    groups = ms.fn("DM_motionGroups")(str(mz))
    assert len(groups) == len(inner["groups"]) == 238
    g0 = groups[0]
    assert bytes(g0.get("data")) == outer["groups"][0]["data"]          # the whole LZO1X stream, 202,116 bytes
    for a, b in zip(groups, inner["groups"]):
        assert (a.get("id"), a.get("size")) == (b["id"], b["size"])
        s, n = a.get("start"), a.get("len")
        assert bytes(a.get("data")[s:s + n]) == b["data"]
    # cached: the same list comes back without decoding again
    assert ms.fn("DM_motionGroups")(str(mz)) is groups


def test_ms_lzo_material_assets(ms):
    """More LZO1X streams: a spread of material.zal assets (the whole archive is too slow to interpret)."""
    mat = next((p for p in corpus_files(".zal") if p.name.lower() == "material.zal"), None)
    if mat is None:
        pytest.skip("no corpus")
    data = mat.read_bytes()
    z = dmparse.read_zal(data)
    src = list(data)
    flags_count = 0x10
    per = 1
    rec = 12
    n = len(z["groups"])
    for k in list(range(0, n, 37)) + [n - 1]:
        p = flags_count + 4 + k * (4 + per * rec)                      # group table entry k
        off, size, stored = (int.from_bytes(data[p + 4 + 4 * i:p + 8 + 4 * i], "little") for i in range(3))
        out = ms.fn("DM_lzo1x")(src, 0x10 + off, stored, size & 0xFFFFFF)
        assert bytes(out) == z["groups"][k]["data"], k


def test_ms_lzo_rejects_bad_streams(ms):
    with pytest.raises(Thrown, match="input overrun"):
        ms.fn("DM_lzo1x")([0x15, 1, 2], 0, 3, 4)                     # 4 literals announced, 2 present
    with pytest.raises(Thrown, match="lookbehind"):
        ms.fn("DM_lzo1x")([0x12, 65, 0x40, 9, 0x11, 0, 0], 0, 7, 3)   # match distance past the output start
    with pytest.raises(Thrown, match="expected 9"):
        ms.fn("DM_lzo1x")([0x12, 65, 0x11, 0, 0], 0, 5, 9)            # end marker, too little output
    assert ms.fn("DM_lzo1x")([0x12, 65, 0x11, 0, 0], 0, 5, 1) == [65]
    with pytest.raises(Thrown, match="bad magic"):
        ms.fn("DM_readZalBytes")([0] * 24, 0, 24, False)


def _rots(rots):
    return {i: list(v) for i, v in enumerate(rots) if v is not None}


def test_ms_motion_bank_matches_dmparse(ms_bank, py_bank):
    assert sorted(ms_bank) == sorted(py_bank) == list(range(1, 239))
    for mid, b in py_bank.items():
        a = ms_bank[mid]
        assert (a.get("frames"), a.get("framems"), a.get("wide"), a.get("endtime")) == \
               (b["frames"], b["frame_ms"], b["wide"], b["end_time"]), mid
        assert a.get("sections") == b["sections"], mid
        for x, y in zip(a.get("inits"), b["inits"]):
            if y is None:
                assert x is None, mid
                continue
            assert (x.get("section") - 1, x.get("time"), x.get("trans"), _rots(x.get("rots"))) == \
                   (y["section"], y["time"], y["trans"], y["rots"]), mid
        assert len(a.get("records")) == len(b["records"]), mid
        for x, y in zip(a.get("records"), b["records"]):
            assert (x.get("section") - 1, x.get("time"), x.get("trans"), _rots(x.get("rots"))) == \
                   (y["section"], y["time"], y["trans"], y["rots"]), mid


def test_ms_sample_motion_all_clips(ms, ms_bank, py_bank):
    """Every frame of every clip: rider and bike Euler angles (exact) and translations."""
    sample = ms.fn("DM_sampleMotion")
    frames = 0
    for mid, clip in py_bank.items():
        ref = dmparse.sample_motion(clip)
        got = sample(ms_bank[mid])
        assert len(got) == len(ref) == clip["frames"], mid
        for k, (g, r) in enumerate(zip(got, ref)):
            assert g.get("time") == r["time"], (mid, k)
            for key, rkey in (("rider", "rider"), ("bike", "bike")):
                want = None if r[rkey] is None else [list(e) for e in r[rkey]]
                assert g.get(key) == want, (mid, k, key)
            for key, rkey in (("ridertrans", "rider_trans"), ("biketrans", "bike_trans")):
                want = None if r[rkey] is None else list(r[rkey])
                assert g.get(key) == want, (mid, k, key)
        frames += len(ref)
    assert frames == 2301


ALL_CHANNELS = dmparse.RIDER_CHANNELS + dmparse.BIKE_CHANNELS


@pytest.mark.parametrize("name", ALL_CHANNELS + ("NotAChannel",))
def test_ms_channel_rotation(ms, name):
    rot = ms.fn("DM_channelRotation")
    for e in ((0, 0, 0), (0x4000, 0, 0), (0, 0x4000, 0), (0, 0, -0x4000), (1234, -32768, 32767), (-9000, 20000, 511)):
        a = rot(name, list(e))
        b = dmparse.channel_rotation(name, e)
        assert all(abs(x - y) < 1e-12 for x, y in zip(a, b)), (name, e, a, b)
        m = ms.fn("DM_quatMatrix")(a, ms.eval(("point3", [("lit", 1), ("lit", 2), ("lit", 3)])))
        rows = dmi.quat_rows(b)
        assert all(abs(m.rows[i][j] - rows[i][j]) < 1e-12 for i in range(3) for j in range(3))
        assert m.pos == (1.0, 2.0, 3.0)


# ---------------------------------------------------------------------------- keyed imports = Python importer

def _python_import(path, mz, ids, tex):
    rt = Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    try:
        return dmi.import_zmd(str(path), scale=chk.SCALE, texture_dir=str(tex), motion_path=str(mz), motion_ids=ids)
    finally:
        dmi.set_runtime(None, None)


def _ms_import(path, mz, ids, tex):
    it = Interp(scene=Runtime())
    it.temp_dir = str(tex)
    it.run_file(str(MS))
    return it.fn("DM_ImportZmdMotion")(str(path), chk.SCALE, 0, True, str(tex), str(mz), ids)


def _close(m1, m2):
    return (all(abs(m1.rows[i][j] - m2.rows[i][j]) < ROT_TOL for i in range(3) for j in range(3))
            and all(abs(a - b) < POS_TOL for a, b in zip(m1.pos, m2.pos)))


@pytest.mark.parametrize("stem,ids", [("DaveMirra1", [1, 8, 15]), ("DMbike01", [8, 15]), ("DaveMirra1", [238])])
def test_ms_keys_match_python_importer(stem, ids, tmp_path):
    path, mz = _zmd(stem), _motion_zal()
    s = _python_import(path, mz, ids, tmp_path / "py")
    root, frames, meshes, folder, keys = _ms_import(path, mz, ids, tmp_path / "ms")
    assert keys == s["keys"] > 0
    assert _close(root.transform, s["root"].transform)              # animated riders: no upright correction
    assert [n.name for n in frames] == [n.name for n in s["frames"]]
    for a, b in zip(frames, s["frames"]):
        assert len(a.keys) == len(b.keys) > 0, a.name
        for (ta, ma), (tb, mb) in zip(a.keys, b.keys):
            assert ta == tb, a.name
            assert _close(ma, mb), (a.name, ta, ma.rows, mb.rows)
        assert _close(a.transform, b.transform), a.name                # the last key = the node's transform
    for a, b in zip(meshes, s["meshes"]):
        assert a.name == b.name and not a.keys and _close(a.transform, b.transform)


def test_ms_motion_keys_pose(tmp_path):
    """The manual in-game check's expectations for motion 8: head above the hips on every frame; the crank turns."""
    mz = _motion_zal()
    root, frames, meshes, folder, keys = _ms_import(_zmd("DaveMirra1"), mz, [8], tmp_path)
    by = {n.name: n for n in frames}
    nframes = dmparse.read_motion_bank(mz.read_bytes())[8]["frames"]
    assert keys == nframes * len(frames) and len(by["Head"].keys) == nframes
    assert [t for t, _ in by["Head"].keys] == list(range(nframes))       # one key per game frame from 0
    for (_, h), (_, p) in zip(by["Head"].keys, by["Hips"].keys):
        assert all(math.isfinite(c) for c in h.pos) and h.pos[2] > p.pos[2]
    root, frames, meshes, folder, keys = _ms_import(_zmd("DMbike01"), mz, [8], tmp_path)
    sprocket = next(n for n in frames if n.name == "Sprocket")
    assert len({m.rows for _, m in sprocket.keys}) > 10


def test_ms_import_without_motion_is_unchanged(tmp_path):
    """DM_ImportZmd (no motion) still returns 4 values, stands the rider upright and sets no keys."""
    it = Interp(scene=Runtime())
    it.run_file(str(MS))
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(_zmd("DaveMirra1")), chk.SCALE, 0, True, str(tmp_path))
    chk.check_upright_rider(frames)
    assert not any(n.keys for n in frames)


def test_ms_motion_errors(ms, tmp_path):
    p = _zmd("DMbike01")
    imp = ms.fn("DM_ImportZmdMotion")
    with pytest.raises(Thrown, match="motion id 239 is not in motion.zal"):
        imp(str(p), chk.SCALE, 0, True, str(tmp_path), str(_motion_zal()), [8, 239])
    with pytest.raises(Thrown, match="motion.zal not found"):
        imp(str(p), chk.SCALE, 0, True, str(tmp_path), None, [8])
    with pytest.raises(Thrown, match="motion.zal not found"):
        imp(str(p), chk.SCALE, 0, True, str(tmp_path), str(tmp_path / "nope.zal"), [8])


def test_ms_parse_motion_ids_and_find_zal(ms, tmp_path):
    parse = ms.fn("DM_parseMotionIds")
    assert parse("") == []
    assert parse("1,8, 15") == [1, 8, 15]
    assert parse(" , 3 ,,") == [3]
    for bad in ("a", "1 2", "-1", "1.5", "1234567890"):
        with pytest.raises(Thrown, match="bad motion id"):
            parse(bad)
    find = ms.fn("DM_findMotionZal")
    assert Path(find(str(_zmd("DaveMirra1")))) == _motion_zal()
    assert find(str(tmp_path / "x.zmd")) is None


# ---------------------------------------------------------------------------- the interpreter's new built-ins

def _run(src, tmp_path, scene=None):
    f = tmp_path / "t.ms"
    f.write_text(src, encoding="utf-8")
    it = Interp(scene=scene or Runtime())
    it.run_file(str(f))
    return it


def test_interpreter_motion_builtins(tmp_path):
    it = _run('fn f = (#(sin 90, cos 180, sin 30.0))\n'
              'fn g = (#(bit.shift 1 31, bit.or 65535 (bit.shift 65535 16), bit.shift (-8) (-1), bit.and (-1) 255))\n'
              'fn h = (2147483647 + 1)\n'
              'fn i = (#(filterString " 1,,8 ,15" ",", trimLeft "  x ", "15" as integer))\n'
              'fn j = ("1.5" as integer)\n', tmp_path)
    s, c, s30 = it.fn("f")()
    assert s == 1.0 and c == -1.0 and abs(s30 - 0.5) < 1e-15            # degrees, as in MAXScript
    assert it.fn("g")() == [-2147483648, -1, -4, 255]                   # 32-bit wrap
    with pytest.raises(MXSError, match="overflow"):
        it.fn("h")()
    assert it.fn("i")() == [[" 1", "8 ", "15"], "x ", 15]
    with pytest.raises(MXSError, match="as integer"):
        it.fn("j")()


def test_interpreter_animate_at_time(tmp_path):
    rt = Runtime()
    it = _run('global n1 = dummy name:"a" boxsize:[1, 1, 1]\n'
              'fn f m = (\n'
              '    n1.transform = m\n'
              '    animate on\n'
              '    (\n'
              '        at time 3 (n1.transform = m)\n'
              '        at time (2 + 2)\n'
              '        (\n'
              '            n1.transform = m\n'
              '        )\n'
              '    )\n'
              '    at time 7 (n1.transform = m)\n'
              ')\n', tmp_path, rt)
    m = it.eval(("call", ("name", "matrix3"), [("point3", [("lit", 1), ("lit", 0), ("lit", 0)])] * 4, {}))
    it.fn("f")(m)
    # keys only inside animate on + at time; the first key at a frame other than 0 also keys frame 0 (Max)
    assert [t for t, _ in it.globals["n1"].keys] == [0, 3, 4]
    assert it.globals["n1"].keys[0][1] is m
    for bad in ('fn g = (animate (1))\n', 'fn g = (at 3 (1))\n'):
        with pytest.raises(MXSError):
            _run(bad, tmp_path)
    with pytest.raises(MXSError, match="redefines"):
        _run('struct S (a)\nfn s x = (x)\n', tmp_path)                   # case-insensitive: S and s collide

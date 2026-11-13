"""DM-016: motion bank (motion.zal) reader/writer/evaluator, glTF animation export, fuzz."""
import json
import math
import os
import struct
import subprocess
from pathlib import Path

import pytest
from conftest import GAME_DIR
from fuzz import fuzz_parser

from dmbmx.cli import main
from dmbmx.convert import anim_gltf, gltf
from dmbmx.convert.gltf import read_glb
from dmbmx.formats import motion as M
from dmbmx.core.errors import FormatError
from dmbmx.formats import zal, zmd

BLENDER = Path(r"C:\Program Files\Blender Foundation\Blender 5.3\blender.exe")


@pytest.fixture(scope="module")
def bank():
    f = GAME_DIR / "assets" / "motion.zal"
    if not f.is_file():
        pytest.skip(f"no motion.zal at {f}")
    return M.read_bank(f.read_bytes())


# ------------------------------------------------------------------ unit (no game data)

def test_euler_to_quat_axes():
    s = math.sqrt(0.5)
    for e, q in (((0, 0, 0), (0, 0, 0, 1)), ((0x4000, 0, 0), (s, 0, 0, s)),
                 ((0, 0x4000, 0), (0, s, 0, s)), ((0, 0, 0x4000), (0, 0, s, s))):
        assert M.euler_to_quat(e) == pytest.approx(q, abs=1e-9)
    # composition order: q = qz * qy * qx
    e = (1000, -2000, 3000)
    qx, qy, qz = (M.euler_to_quat((e[0], 0, 0)), M.euler_to_quat((0, e[1], 0)), M.euler_to_quat((0, 0, e[2])))
    assert M.euler_to_quat(e) == pytest.approx(M.quat_mul(qz, M.quat_mul(qy, qx)), abs=1e-9)


def test_arm_fix():
    s = math.sqrt(0.5)
    F = M.engine_to_model
    assert M.channel_rotation("Head", (0, 0, 0)) == pytest.approx((0, 0, 0, 1))
    # DM-019: NgIFzactor's literal table, then the engine -> model conjugation (x, -y, -z, w)
    assert M.ARM_FIX_AS_DECOMPILED is M.ARM_FIX
    assert M.channel_rotation("LeftUpArm", (0, 0, 0)) == pytest.approx((0, 0, -s, s))
    assert M.channel_rotation("RightUpArm", (0, 0, 0)) == pytest.approx((0, 0, s, s))
    assert M.channel_rotation("LeftUpArm", (0, 0, 0), frf=False) == pytest.approx((0, 0, s, s))
    for n in M.RIDER_CHANNELS + M.BIKE_CHANNELS:
        e = (1234, -567, 890)
        assert M.channel_rotation(n, e) == pytest.approx(F(M.channel_rotation(n, e, frf=False)))
        if n not in M.ARM_FIX:
            assert M.channel_rotation(n, e) == pytest.approx(F(M.euler_to_quat(e)))
    # legacy DM-017: Rx(180) * q * Cz(+-90) on the upper arms, no conjugation
    x180 = M.euler_to_quat((-0x8000, 0, 0))
    assert M.channel_rotation("LeftUpArm", (0, 0, 0), legacy=True) == pytest.approx(M.quat_mul(x180, (0, 0, s, s)))
    assert M.channel_rotation("Head", (100, 200, 300), legacy=True) == pytest.approx(M.euler_to_quat((100, 200, 300)))


def test_engine_to_model_is_F_conjugation():
    """[V] zrwModelQuatToRwMatrix 0x00487E70: R(x, -y, -z, w) = F R(q) F, F = diag(1, -1, -1)."""
    q = M.euler_to_quat((3000, -7000, 12000))
    R, G = M._qmat(q), M._qmat(M.engine_to_model(q))
    f = (1, -1, -1)
    for i in range(3):
        for j in range(3):
            assert G[i][j] == pytest.approx(f[i] * R[i][j] * f[j])


def _synthetic():
    m = M.Motion()
    m.frames_word, m.flags, m.frame_ms, m.extra = 3, 0x8023, 34, 0
    m.sections = [(2, 0x80, 0), (1, 0x80, 1)]
    m.inits = [M.Record(0, 0x1FFFF, 0, 0, 0.5, (8, 16, -8), {0: (100, 0, 0), 1: (0, 0, 0)}, init_rider=True),
               M.Record(1, 0x10000006, 0, 0, None, (0, 0, 0), {0: (0, 0, 0)})]
    m.records = [M.Record(0, 0x00000006, 34, 0, None, (256, 0, 0), {0: (10, 0, 0)}),
                 M.Record(1, 0x10000004, 34, 0, None, None, {0: (0, 0, 5)}),
                 M.Record(0, 0x00000008, 68, 0, None, None, {1: (0, 3, 0)})]   # keeps the stream going
    m.end_time = 102
    return m


def test_synthetic_roundtrip_and_sample():
    m = _synthetic()
    data = m.write()
    m2 = M.Motion.read(data)
    assert m2.write() == data
    P = m2.sample()
    assert len(P) == 3
    assert P[0].rider[0] == (100, 0, 0) and P[0].rider_trans == (1.0, 2.0, -1.0)
    # deltas persist: +10 per frame on channel 0, root x moves 1.0 per frame
    assert P[2].rider[0] == (120, 0, 0) and P[2].rider_trans[0] == pytest.approx(3.0)
    assert P[2].bike[0] == (0, 0, 10) and P[2].rider[1] == (0, 3, 0)
    # DM-053: once the record stream is used up the game's decoder stops; later frames hold the pose
    m2.records = m2.records[:2]
    P = m2.sample()
    assert P[2].rider[0] == P[1].rider[0] == (110, 0, 0) and P[2].rider_trans == P[1].rider_trans


def test_motion_errors():
    with pytest.raises(FormatError):
        M.Motion.read(bytes([1, 0]))
    data = bytearray(_synthetic().write())
    struct.pack_into("<I", data, 72, 0x50000004)          # first delta record -> missing section 5
    with pytest.raises(FormatError):
        M.Motion.read(bytes(data))


def test_fuzz_synthetic():
    fuzz_parser(M.Motion.read, _synthetic().write(), n=300, seed=0)


def test_matrix_to_trs():
    q = M.euler_to_quat((3000, -7000, 12000))
    x, y, z, w = q
    R = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    col = [R[0][0], R[1][0], R[2][0], 0, R[0][1], R[1][1], R[2][1], 0, R[0][2], R[1][2], R[2][2], 0, 1, 2, 3, 1]
    t, r, s = anim_gltf.matrix_to_trs(col)
    assert t == (1, 2, 3) and s == pytest.approx((1, 1, 1))
    assert abs(sum(a * b for a, b in zip(r, q))) == pytest.approx(1.0)


# ------------------------------------------------------------------ corpus

def test_bank_shape_and_roundtrip(bank, game_dir):
    src = (game_dir / "assets" / "motion.zal").read_bytes()
    assert bank.write() == src
    assert bank.ids == list(range(1, 239))
    inner = bank.archive.entries[0].data()
    assert bank.bank.headerless and bank.bank.write() == inner
    for mid in bank.ids:
        raw = bank.bank.find(mid).data()
        m = M.Motion.read(raw)
        assert m.write() == raw, mid
        assert (m.flags, m.frame_ms, m.extra, m.end_time) == (0x9823, 34, 0, m.nframes * 34)
        assert [s[2] for s in m.sections] == [0, 1] and m.sections[0][0] == 15
        assert 1 <= m.sections[1][0] <= 5 and m.trailing == b"" and m.block is None
        assert all(r.time % 34 == 0 and 0 < r.time < m.end_time for r in m.records)


def test_set_motion_reencodes(bank, game_dir):
    b = M.read_bank((game_dir / "assets" / "motion.zal").read_bytes())
    m = b.motion(11)
    m.inits[0].rots[2] = (1, 2, 3)                       # Head init angles
    b.set_motion(11, m)
    again = M.read_bank(b.write())
    assert again.motion(11).sample()[0].rider[2] == (1, 2, 3)
    assert again.motion(12).write() == bank.motion(12).write()


def test_pedalling_motion(bank):
    """[V] motion 8: sprocket and both pedals turn one full revolution about X only."""
    P = bank.motion(8).sample()
    for ch in (2, 3, 4):
        xs = [p.bike[ch][0] for p in P]
        total = sum(M._s16(b - a) for a, b in zip(xs, xs[1:]))
        assert abs(abs(total) - 65536) < 2500
        assert {p.bike[ch][1] for p in P} == {0} and {p.bike[ch][2] for p in P} == {0}


def _fk(skel, rot_for):
    def qm(q):
        x, y, z, w = q
        return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    W = []
    for b in skel:
        L = [row[:] for row in b["matrix"]]
        q = rot_for(b["name"])
        if q is not None:
            R = qm(q)
            for r in range(3):
                L[r][:3] = R[r]
        if b["parent"] is not None and b["parent"] >= 0:
            P = W[b["parent"]]
            L = [[sum(P[i][k] * L[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
        W.append(L)
    return {b["name"]: [W[i][r][3] for r in range(3)] for i, b in enumerate(skel)}


def test_hands_on_handlebars(bank, game_dir):
    """[V] DM-019 (DM-045 finding): rider posed on the bike (feet on pedals) with the engine's F q F and the
    literal arm table - hands on the handlebars, hand gap = bar width. Without F the literal table puts the
    hands ~0.7 away; the legacy DM-017 Rx(180) table is kept for comparison."""
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "research"))
    from arm_pose_check import setup, summarize
    rider, bsk, bars, b = setup(GAME_DIR, "DaveMirra1.ZMD", "DMbike01.zmd")
    mids = [8, 5, 36, 22, 57, 106]
    new = summarize(rider, bsk, bars, b, mids, step=2)
    nof = summarize(rider, bsk, bars, b, mids, step=2, frf=False)
    legacy = summarize(rider, bsk, bars, b, mids, step=2, legacy=True)
    assert new["feet"] < 0.11
    assert new["bar"] < 0.15 and new["barend"] < 0.18 and new["barend"] < legacy["barend"]
    assert nof["bar"] > 4 * new["bar"]
    assert 0.45 < new["gap"] < 0.65


def test_pose_positions_bike_wheels_level(bank, game_dir):
    """Bike FK with the clip's bike channels: both wheel axles at the same height (motion 8)."""
    skb = zmd.read((game_dir / "assets" / "DMbike01.zmd").read_bytes()).skeleton()
    for p in bank.motion(8).sample()[::5]:
        P = M.pose_positions(skb, M.bike_rotations(p))
        assert abs(P["FrontWheel"][1] - P["BackWheel"][1]) < 0.03
        assert 0.8 < math.dist(P["FrontWheel"], P["BackWheel"]) < 1.0


def test_fuzz_real_motions(bank):
    fuzz_parser(M.Motion.read, bank.bank.find(11).data(), n=200, seed=0)
    fuzz_parser(M.Motion.read, bank.bank.find(1).data(), n=200, seed=1)
    fuzz_parser(lambda d: zal.ZalArchive.read(d, headerless=True), bank.archive.entries[0].data()[:4000],
                n=200, seed=0)


# ------------------------------------------------------------------ glTF / CLI

def test_add_animations(bank, game_dir, tmp_path):
    data = (game_dir / "assets" / "DaveMirra1.ZMD").read_bytes()
    glb = gltf.export_glb(zmd.read(data), source=data, source_name="DaveMirra1.ZMD")
    out = anim_gltf.add_animations(glb, [("m1", bank.motion(1)), ("m8", bank.motion(8))])
    js, bin_ = read_glb(out)
    assert [a["name"] for a in js["animations"]] == ["m1", "m8"]
    a = js["animations"][1]
    assert len(a["channels"]) == 15
    names = {js["nodes"][c["target"]["node"]]["name"] for c in a["channels"]}
    assert names == set(M.RIDER_CHANNELS)
    for c in a["channels"]:
        n = js["nodes"][c["target"]["node"]]
        assert "matrix" not in n and "rotation" in n and "translation" in n
    root = js["nodes"][js["scenes"][0]["nodes"][0]]
    assert "rotation" not in root
    acc = js["accessors"][a["samplers"][0]["output"]]
    assert acc["count"] == bank.motion(8).nframes and acc["type"] == "VEC4"


def test_cli_motion_and_export(game_dir, tmp_path, capsys):
    assert main(["motion", "list"]) == 0
    assert "238 motions" in capsys.readouterr().out
    assert main(["motion", "dump", "11", "--quat"]) == 0
    d = json.loads(capsys.readouterr().out)
    assert d["frames"] == 2 and len(d["poses"]) == 2 and "Hips" in d["poses"][0]["rider_quat"]
    out = tmp_path / "dm.glb"
    assert main(["export", str(game_dir / "assets" / "DaveMirra1.ZMD"), str(out), "--anims", "8,10-11"]) == 0
    js, _ = read_glb(out.read_bytes())
    assert len(js["animations"]) == 3


@pytest.mark.skipif(not BLENDER.exists() or os.environ.get("DMBMX_NO_BLENDER"), reason="Blender 5.3 not installed")
def test_blender_anim_check(game_dir, tmp_path):
    """Blender headless: every frame of a few tricks keeps the mesh finite and un-exploded."""
    out = tmp_path / "anim.glb"
    assert main(["export", str(game_dir / "assets" / "DaveMirra1.ZMD"), str(out), "--anims", "8,15,59"]) == 0
    script = Path(__file__).resolve().parents[1] / "tools" / "blender" / "scripts" / "check_anim.py"
    r = subprocess.run([str(BLENDER), "-b", "--factory-startup", "--python", str(script), "--", str(out)],
                       capture_output=True, text=True, timeout=600)
    assert "ANIM CHECK OK" in r.stdout, r.stdout[-2000:]

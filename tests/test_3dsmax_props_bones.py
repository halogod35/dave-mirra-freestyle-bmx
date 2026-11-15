"""The prop rotation convention (pinned to the exe and cross-checked with the game's own prop
collision boxes), bones aimed at their children in both 3ds Max importers (skin and keys unchanged), and
the "Flip normals to faces" option."""
import math
import struct
import sys
from pathlib import Path

import pytest
from conftest import GAME_DIR, corpus_files

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "python"))
import dave_mirra_import as dmi  # noqa: E402
import dmparse  # noqa: E402  (the vendored copy next to the importer; identical to tools/3dsmax/parser)
import max_checks as chk  # noqa: E402
from max_mock import M3, P3, PymxsModule, Runtime, skin_world, transform_at  # noqa: E402
from maxscript_interp import Interp, MXSError  # noqa: E402

from dmbmx.formats import level as L  # noqa: E402

MS = REPO / "tools" / "3dsmax" / "maxscript" / "DaveMirraImport.ms"
EXE = GAME_DIR / "NgBMX.exe"
SCALE = chk.SCALE
F = (1.0, -1.0, -1.0)


def _level_dirs():
    root = GAME_DIR / "assets"
    if not root.is_dir():
        return []
    return [d for d in sorted(root.iterdir()) if d.is_dir() and any(f.suffix.lower() == ".zsd" for f in d.iterdir())]


def _level_params():
    dirs = _level_dirs()
    if not dirs:
        return [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))]
    return [pytest.param(d, id=d.name) for d in dirs]


def _zmd(stem):
    p = next((p for p in corpus_files(".zmd") if p.stem.lower() == stem.lower()), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _motion_zal():
    p = next((p for p in corpus_files(".zal") if p.name.lower() == "motion.zal"), None)
    if p is None:
        pytest.skip("no corpus")
    return p


def _fmf(m9):
    return [tuple(m9[3 * r + c] * F[r] * F[c] for c in range(3)) for r in range(3)]


def _yaw(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return (c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c)


# ============================================================================ 1. prop rotation convention

def test_prop_matrix_is_the_stored_rows():
    """Props: RW rotation = the stored rows (prop_matrix_to_rw), in dmbmx and both dmparse copies. For a
    pure yaw that is the opposite sign of the earlier F M F (the field report: "+80 shows as -80").
    Scene points keep F M F (their rows are only used as ZGE direction vectors)."""
    m = _yaw(80.0)
    want = [m[0:3], m[3:6], m[6:9]]
    for f in (L.prop_matrix_to_rw, dmparse.prop_matrix_to_rw):
        r = f(m)
        assert all(r[i] == pytest.approx(want[i]) for i in range(3))
    old = _fmf(m)
    assert old[0] == pytest.approx((m[0], -m[1], -m[2])) and old[0][2] == pytest.approx(-m[2])   # yaw -80
    assert L.matrix_to_rw(m) == old and [tuple(r) for r in dmparse.matrix_to_rw(m)] == old
    src = (REPO / "tools" / "3dsmax" / "parser" / "dmparse.py").read_text(encoding="utf-8")
    assert "prop_matrix_to_rw(p[\"matrix\"])" in src
    ms = MS.read_text(encoding="utf-8")
    assert "local rows = DM_propMatrixToRw p.m9" in ms and "local rows = DM_matrixToRw sp.m9" in ms


def test_ms_prop_matrix_mirrors_dmparse():
    it = Interp()
    it.run_file(str(MS))
    m = list(_yaw(37.5))
    assert [tuple(r) for r in it.fn("DM_propMatrixToRw")(m)] == [tuple(r) for r in dmparse.prop_matrix_to_rw(m)]


def _image():
    if not EXE.is_file():
        pytest.skip("no NgBMX.exe")
    import pefile
    pe = pefile.PE(str(EXE), fast_load=True)
    return pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image()


def _stores(img, base, va, end):
    """fstp dword ptr [eax + off] in [va, end): {off: preceded by fchs}."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out, prev = {}, None
    for ins in md.disasm(bytes(img[va - base:end - base]), va):
        if ins.mnemonic == "fstp" and ins.op_str.startswith("dword ptr [eax"):
            s = ins.op_str[len("dword ptr [eax"):].rstrip("]").replace(" ", "")
            out[int(s[1:], 16) if s else 0] = prev == "fchs"
        prev = ins.mnemonic
    return out


def test_exe_renders_props_with_the_mirrored_quaternion_converter():
    """The exe evidence (VAs in the 2000-11-22 build; LEVEL.md section 2):
    1. ngappInit 0x0045B270: xor ebx,ebx ... push ebx; push 1; push 0x10; call [0x00561484] = ZGL set value
       0x10 = 1, and the slot 0x00561484 holds zrwZglSetValue 0x0048AB10;
    2. zrwZglSetValue's case 0x10 sets g_zrwZglFlags (0x00697728) bit 1 from the value;
    3. zrwModelUpdate 0x00488650 tests that bit and then calls zrwModelQuatToRwMatrixMirror 0x00487FC0
       instead of zrwModelQuatToRwMatrix 0x00487E70 for the model's root frame;
    4. the mirror converter writes the same nine terms with right.y, right.z, up.x and at.x negated
       (fchs before exactly those stores): mirror = D * plain * D, D = diag(-1, 1, 1)."""
    base, img = _image()

    def at(va, n):
        return bytes(img[va - base:va - base + n])

    assert at(0x0045B274, 2) == bytes.fromhex("33db")                               # xor ebx, ebx
    assert at(0x0045B2D6, 11) == bytes.fromhex("536a016a10ff1584145600")            # (0x10, 1, 0) -> [0x561484]
    assert struct.unpack("<I", at(0x00561484, 4))[0] == 0x0048AB10
    assert at(0x0048AC72, 25) == bytes.fromhex("a128776900" "83e201" "24fd" "d1e2" "0bd0" "b801000000" "891528776900")
    assert at(0x00488653, 5) == bytes.fromhex("a028776900")                         # mov al, [g_zrwZglFlags]
    assert at(0x00488663, 2) == bytes.fromhex("a802")                              # test al, 2
    assert at(0x00488669, 2) == bytes.fromhex("7411")                              # je -> the plain call

    def call_target(va):
        assert img[va - base] == 0xE8
        return va + 5 + struct.unpack("<i", at(va + 1, 4))[0]

    assert call_target(0x00488675) == 0x00487FC0 and call_target(0x00488687) == 0x00487E70
    plain = _stores(img, base, 0x00487E70, 0x00487FB2)
    mirror = _stores(img, base, 0x00487FC0, 0x0048810A)
    terms = {0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28}               # the 3x3 of an RwMatrix
    assert set(plain) == set(mirror) == terms
    assert not any(plain.values())
    assert {o for o, neg in mirror.items() if neg} == {0x04, 0x08, 0x10, 0x20}


def _plain(q):
    """zrwModelQuatToRwMatrix 0x00487E70 (decomp/src/NgEngine/Zge/Com/ZglRw/zrwModel.c): rows of R(q') with
    q' = (x, -y, -z, w)."""
    x, y, z, w = q[0], -q[1], -q[2], q[3]
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    return [(1 - 2 * (z * z + y * y), 2 * (y * x + z * w), 2 * (z * x - y * w)),
            (2 * (y * x - z * w), 1 - 2 * (z * z + x * x), 2 * (z * y + x * w)),
            (2 * (z * x + y * w), 2 * (z * y - x * w), 1 - 2 * (y * y + x * x))]


def _mirror(q):
    d = (-1.0, 1.0, 1.0)
    return [tuple(r[c] * d[i] * d[c] for c in range(3)) for i, r in enumerate(_plain(q))]


def _euler(m9):
    """zmathMatrixToEuler 0x00481FC0 (radians; the exe quantises to 16-bit angles)."""
    cy = math.hypot(m9[0], m9[1])
    if cy > 0.001:
        return math.atan2(m9[5], m9[8]), math.atan2(-m9[2], cy), math.atan2(m9[1], m9[0])
    return math.atan2(-m9[7], m9[4]), math.atan2(-m9[2], cy), 0.0


def _quat(e):
    """zquatFromEuler 0x004832B0: q = qz * qy * qx."""
    cx, sx = math.cos(e[0] / 2), math.sin(e[0] / 2)
    cy, sy = math.cos(e[1] / 2), math.sin(e[1] / 2)
    cz, sz = math.cos(e[2] / 2), math.sin(e[2] / 2)
    return (sx * cz * cy - cx * sz * sy, cx * cz * sy + sx * sz * cy, cx * sz * cy - sx * cz * sy, sx * sz * sy + cx * cz * cy)


def test_every_prop_renders_with_its_stored_rows():
    """Through the exe's chain (zpropLoad: rows -> euler -> quaternion; zrwModelUpdate in mirrored mode)
    every one of the 927 props gets an RW rotation equal to its stored rows = prop_instances' R, and the
    old F M F is what the non-mirrored converter would give."""
    dirs = _level_dirs()
    if not dirs:
        pytest.skip("no corpus")
    n = differ = 0
    for d in dirs:
        lv = L.Level.read_dir(str(d))
        for p, _d, _t, R in lv.prop_instances():
            q = _quat(_euler(p.matrix))
            rw = _mirror(q)
            assert max(abs(rw[i][j] - R[i][j]) for i in range(3) for j in range(3)) < 1e-5, (d.name, p.name)
            old = _fmf(p.matrix)
            assert max(abs(_plain(q)[i][j] - old[i][j]) for i in range(3) for j in range(3)) < 1e-5
            differ += max(abs(old[i][j] - R[i][j]) for i in range(3) for j in range(3)) > 0.05
            n += 1
    assert n == 927 and differ > 300          # most props are yawed: the old convention placed them wrong


def _rot_xyz(v, e):
    """zmathVecRotateXYZ 0x00481300 (NgEngine/Zge/zmath.c): X, then Y (its own sense), then Z."""
    x, y, z = v
    c, s = math.cos(e[0]), math.sin(e[0])
    y, z = c * y - s * z, c * z + s * y
    c, s = math.cos(e[1]), math.sin(e[1])
    x, z = c * x - s * z, c * z + s * x
    c, s = math.cos(e[2]), math.sin(e[2])
    return c * x - s * y, c * y + s * x, z


def test_prop_collision_boxes_line_up_with_the_models():
    """Data cross-check, independent of the render path: the game builds each prop's collision box (the
    8 corners zcolBuildPropRails 0x0046A4D0 turns into grind rails) in ZGE space from the def's extents and
    extra floats and the prop's euler angles. Mapped to RW, the corners sit on the placed model's vertices
    (horizontal distance, mean of the 8 corners) with prop_instances' rotation, and metres away with the
    old F M F. Checked on every prop whose def has a box and whose yaw is not a multiple of 90 degrees
    (where a centred box cannot tell the two apart)."""
    dirs = _level_dirs()
    if not dirs:
        pytest.skip("no corpus")
    new_d, old_d = [], []
    for d in dirs:
        lv = L.Level.read_dir(str(d))
        o = lv.world.origin
        cache = {}
        for p, df, t, R in lv.prop_instances():
            if df is None or df.obji is None:
                continue
            e = _euler(p.matrix)
            if abs(math.sin(2 * e[1])) < 0.1:
                continue
            extra = struct.unpack("<4f", df.junk)
            w, h, dd = df.extents[0] * 50, df.extents[1] * 100, df.extents[2] * 50
            lift = (df.extents[1] * 0.5 + extra[1]) * 100
            top = 0.0 if extra[3] <= 1e-5 else h - extra[3] * 10000
            corners = [(-w, -h, -dd), (-w, -h, dd), (-w, 0, dd), (-w, 0, -dd),
                       (w, top - h, -dd), (w, top - h, dd), (w, top, dd), (w, top, -dd)]
            pos = [int(c * 100) for c in p.pos]                      # NgPropCol.pos: whole ZGE units
            rw = []
            for c in corners:
                r = _rot_xyz((c[0], c[1] + lift, c[2]), e)
                rw.append(L.zge_to_rw(tuple(r[k] + pos[k] for k in range(3)), o))
            if df.id not in cache:
                cache[df.id] = list({tuple(q) for ms in df.model.meshes(0, world=True) for q in ms["positions"]})
            for rows, acc in ((R, new_d), (_fmf(p.matrix), old_d)):
                placed = [tuple(sum(q[k] * rows[k][j] for k in range(3)) + t[j] for j in range(3)) for q in cache[df.id]]
                acc.append(sum(min(math.hypot(c[0] - v[0], c[2] - v[2]) for v in placed) for c in rw) / 8)
    n = len(new_d)
    assert n > 60
    wins = sum(a < b for a, b in zip(new_d, old_d))
    assert wins >= 0.9 * n, (wins, n)
    assert sorted(new_d)[n // 2] < 0.05 and sorted(old_d)[n // 2] > 1.0      # medians: ~1 cm vs metres
    assert sum(x < 0.15 for x in new_d) >= 0.6 * n             # palms, pipes: models bigger than their box
    print(f"prop box check: {n} props, new wins {wins}, median {sorted(new_d)[n // 2]:.3f} vs {sorted(old_d)[n // 2]:.3f} m")


# ============================================================================ 2. bones aimed at their children

def test_mock_createbone_is_faithful_and_strict():
    rt = Runtime()
    b = rt.BoneSys.createBone(P3(1, 2, 3), P3(1, 2, 5), P3(0, 1, 0))
    assert b.kind == "bone" and b.length == pytest.approx(2.0)
    assert b.transform.rows[0] == pytest.approx((0, 0, 1))                # X towards the end point
    assert b.transform.rows[2] == pytest.approx((0, 1, 0))                # Z = the zAxis made perpendicular
    assert b.transform.rows[1] == pytest.approx((1, 0, 0))                # Y = Z x X
    assert b.transform.pos == (1.0, 2.0, 3.0)
    with pytest.raises(AssertionError):
        rt.BoneSys.createBone(P3(1, 2, 3), P3(1, 2, 3), P3(0, 0, 1))     # zero length
    with pytest.raises(AssertionError):
        rt.BoneSys.createBone(P3(0, 0, 0), P3(0, 0, 1), P3(0, 0, 2))     # zAxis along the bone
    with pytest.raises(AssertionError):
        b.length = 0.0
    with pytest.raises(AssertionError):
        b.width = -1.0


def test_mock_skin_deforms_from_the_bind_pose():
    rt = Runtime()
    m = rt.mesh(vertices=[P3(1, 0, 0), P3(0, 1, 0)], faces=[P3(1, 2, 1)], materialIDs=[1])
    m.transform = M3((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))
    b = rt.BoneSys.createBone(P3(0, 0, 0), P3(1, 0, 0), P3(0, 0, 1))
    sk = rt.Skin()
    rt.addModifier(m, sk)
    with pytest.raises(AssertionError):
        rt.skinOps.addBone(sk, rt.Dummy(name="x", boxsize=P3(1, 1, 1)), 1)   # no transform yet: no bind pose
    rt.skinOps.addBone(sk, b, 1)
    rt.skinOps.ReplaceVertexWeights(sk, 1, 1, 1.0)
    rt.skinOps.ReplaceVertexWeights(sk, 2, 1, 1.0)
    assert skin_world(sk) == {1: (1.0, 0.0, 0.0), 2: (0.0, 1.0, 0.0)}
    b.transform = M3((0, 1, 0), (-1, 0, 0), (0, 0, 1), (0, 0, 5))         # turn 90 degrees about Z, lift 5
    got = skin_world(sk)
    assert got[1] == pytest.approx((0, 1, 5)) and got[2] == pytest.approx((-1, 0, 5))


def test_interpreter_vector_builtins_are_strict():
    it = Interp()
    b = it.builtins
    assert b["cross"](P3(1, 0, 0), P3(0, 1, 0)) == (0, 0, 1)
    assert b["dot"](P3(1, 2, 3), P3(4, 5, 6)) == 32
    assert b["normalize"](P3(0, 3, 4)) == pytest.approx((0, 0.6, 0.8))
    assert b["abs"](-2.5) == 2.5
    for f, a in (("normalize", (P3(0, 0, 0),)), ("dot", (1, 2)), ("cross", ([1, 0, 0], P3(0, 1, 0))), ("abs", ("1",))):
        with pytest.raises(MXSError):
            b[f](*a)


def _py(rt=None):
    rt = rt or Runtime()
    dmi.set_runtime(rt, PymxsModule(rt))
    return rt


def _ms(tmp_path):
    it = Interp(scene=Runtime())
    it.temp_dir = str(tmp_path / "temp")
    it.run_file(str(MS))
    return it


def test_bone_rule_on_the_rider(tmp_path):
    """The main-child rule on DaveMirra1 (README "Bones"): Skin -> Hips; Hips (child of a root) -> the
    child with the most descendants (_Abdomen, the spine) rather than a leg; Chestx -> Head (continues the
    spine) rather than a shoulder; arms and legs follow their chains; Head, hands and feet are leaves that
    continue their parent. Both importers compute the same offsets and lengths."""
    p = _zmd("DaveMirra1")
    z = dmparse.read_zmd(p.read_bytes())
    sk = dmparse.skeleton(z, 0)
    names = [s["name"] for s in sk]
    tg = chk.bone_targets(z, 0)
    child = {names[i]: (names[c] if c is not None else None) for i, (_d, _l, c) in enumerate(tg)}
    assert child["Skin"] == "Hips" and child["Hips"] == "_Abdomen" and child["Chestx"] == "Head"
    assert child["_Abdomen"] == "Chest" and child["Chest"] == "Chestx"
    assert child["RightUpArm"] == "RightLowArm" and child["LeftLowLeg"] == "LeftFoot"
    for leaf, parent in (("Head", "Chestx"), ("LeftHand", "LeftLowArm"), ("RightFoot", "RightLowLeg")):
        assert child[leaf] is None and tg[names.index(leaf)][0] == tg[names.index(parent)][0]
        assert tg[names.index(leaf)][1] == pytest.approx(0.5 * tg[names.index(parent)][1])
    _py()
    try:
        s = dmi.import_zmd(str(p), scale=SCALE, texture_dir=str(tmp_path / "py"))
    finally:
        dmi.set_runtime(None, None)
    it = _ms(tmp_path)
    lo = it.fn("DM_readZmd")(str(p)).get("lods")[0]
    world = []
    for i, f in enumerate(lo.get("frames")):
        m = M3(f.get("right"), f.get("up"), f.get("fwd"), f.get("pos"))
        par = f.get("parent")
        if 0 <= par < i:                                    # m * world[par] (row vectors, as MAXScript)
            w = world[par]
            m = M3(*(tuple(sum(m.rows[r][k] * w.rows[k][c] for k in range(3)) for c in range(3)) for r in range(3)),
                   w.xf(m.pos))
        world.append(m)
    aim = it.fn("DM_boneAim")(lo, world)
    for (o, length), po, pl in zip(aim, s["bone_offsets"], s["bone_lengths"]):
        assert length == pytest.approx(pl, abs=1e-12)
        assert all(o.rows[i] == pytest.approx(po[i], abs=1e-12) for i in range(3))


@pytest.mark.parametrize("ids", [[8], [1, 15]])
def test_bone_modes_give_the_same_skin_and_keys(ids, tmp_path):
    """Aimed bones (default) vs game-frame bones, both importers, with motion: every key keeps its joint
    position, the aimed key is O * the game-frame key (one constant O per bone), and the Skin modifier puts
    every vertex at the same place at the bind pose and at every key."""
    p, mz = _zmd("DaveMirra1"), _motion_zal()
    runs = {}
    for aim in (True, False):
        _py()
        try:
            s = dmi.import_zmd(str(p), scale=SCALE, texture_dir=str(tmp_path / "py"), motion_path=str(mz),
                               motion_ids=ids, bone_aim_children=aim)
        finally:
            dmi.set_runtime(None, None)
        runs["py", aim] = (s["frames"], s["meshes"], s["bone_offsets"])
        it = _ms(tmp_path)
        r = it.fn("DM_ImportZmdMotion")(str(p), SCALE, 0, True, str(tmp_path / "ms"), str(mz), ids, boneAim=aim)
        assert r[4] == s["keys"]
        runs["ms", aim] = (r[1], r[2], None)
    times = sorted({t for t, _ in runs["py", True][0][0].keys})
    assert len(times) > 1
    offsets = runs["py", True][2]
    ref_frames, ref_meshes, _ = runs["py", False]
    ref_skin = [[x for x in m.modifiers if x.kind == "Skin"][0] for m in ref_meshes if any(x.kind == "Skin" for x in m.modifiers)]
    for key, (frames, meshes, _) in runs.items():
        skins = [[x for x in m.modifiers if x.kind == "Skin"][0] for m in meshes if any(x.kind == "Skin" for x in m.modifiers)]
        assert len(skins) == len(ref_skin) >= 1
        for t in [None] + times:
            for n, g, o in zip(frames, ref_frames, offsets):
                a, b = transform_at(n, t), transform_at(g, t)
                assert chk._close(a.pos, b.pos, 1e-9 * SCALE), (key, n.name, t)
                want = o if key[1] else dmi.IDENTITY3
                ob = tuple(tuple(sum(want[i][k] * b.rows[k][j] for k in range(3)) for j in range(3)) for i in range(3))
                assert all(chk._close(a.rows[i], ob[i], 1e-9) for i in range(3)), (key, n.name, t)
            for sa, sb in zip(skins, ref_skin):
                wa, wb = skin_world(sa, t), skin_world(sb, t)
                assert wa.keys() == wb.keys()
                assert all(chk._close(wa[v], wb[v], 1e-6 * SCALE) for v in wa), (key, t)


def test_game_frame_mode_keeps_the_old_bones(tmp_path):
    p = _zmd("DaveMirra")
    _py()
    try:
        s = dmi.import_zmd(str(p), scale=SCALE, texture_dir=str(tmp_path / "py"), bone_aim_children=False)
    finally:
        dmi.set_runtime(None, None)
    chk.check_import(p, s["root"], s["frames"], s["meshes"], tmp_path / "py", bone_aim=False)
    assert s["bone_lengths"] is None
    it = _ms(tmp_path)
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(p), SCALE, 0, True, str(tmp_path / "ms"), boneAim=False)
    chk.check_import(p, root, frames, meshes, tmp_path / "ms", bone_aim=False)


def test_dialogs_offer_the_bone_and_flip_options():
    """Bones on the Models tab, flip normals on the Levels tab (scripts keep flipNormals:/flip_normals= for
    models; the dialogs are run in tests/test_3dsmax_ui.py)."""
    src = MS.read_text(encoding="utf-8")
    ui = src[src.index("============ UI"):]
    models = ui[ui.index('rollout DM_ModelsTab'):ui.index('rollout DM_LevelsTab')]
    levels = ui[ui.index('rollout DM_LevelsTab'):ui.index('rollout DaveMirraRollout')]
    assert 'dropdownlist ddlBones "Bones" items:#("aim at children", "game frame orientation") selection:1' in models
    assert 'checkbox chkFlip "Flip normals to faces (with Exact normals)" checked:false' in levels
    assert "boneAim:(ddlBones.selection == 1)" in models and "flipNormals" not in models
    assert levels.count("flipNormals:chkFlip.checked") == 1
    py = (REPO / "tools" / "3dsmax" / "python" / "dave_mirra_import.py").read_text(encoding="utf-8")
    assert dmi.BONE_MODES == ("aim at children", "game frame orientation")
    assert "bone_aim_children=self.bones.currentIndex() == 0" in py
    assert py.count("flip_normals=self.flip.isChecked()") == 1


# ============================================================================ 3. flip normals to faces

# a strip: faces 0, 1 up (+Z, CCW seen from above), face 2 tiny and down; vertex 6 in no face
_POS = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (2, 0, 0), (2, 0.01, 0), (5, 5, 5)]
_TRIS = [(0, 1, 2, 0), (1, 3, 2, 0), (1, 5, 4, 0)]
_NRM = [(0, 0, 1), (0, 0, -2), (0.1, 0, -1), None, (0, 0, 1), (0, 0, -1), (0, 0, -1)]
#        agree     against     against       none  only the tiny down face  no face: kept
_WANT = [(0, 0, 1), (0, 0, 2), (-0.1, 0, 1), None, (0, 0, -1), (0, 0, -1), (0, 0, -1)]


def test_flip_rule_both_importers():
    """Area-weighted vote: vertex 1 has two up faces and one tiny down face, so its down normal flips
    up; vertices 4/5 are only in the down face, so up flips down and down stays; vertex 6 is in no face."""
    got_py = dmi.flip_normals_to_faces(_POS, _NRM, _TRIS)
    assert [None if n is None else tuple(n) for n in got_py] == [None if n is None else tuple(float(c) for c in n) for n in _WANT]
    assert chk.flip_ref(_POS, _NRM, _TRIS) == [None if n is None else tuple(float(c) for c in n) for n in _WANT]
    it = Interp()
    it.run_file(str(MS))
    got_ms = it.fn("DM_flipNormalsToFaces")([P3(*p) for p in _POS], [None if n is None else P3(*n) for n in _NRM],
                                            [list(t) for t in _TRIS])
    assert [None if n is None else tuple(n) for n in got_ms] == [None if n is None else tuple(n) for n in got_py]


def test_flip_needs_exact_normals(tmp_path):
    _py()
    try:
        s = dmi.import_zmd(str(_zmd("DMbike01")), scale=SCALE, texture_dir=str(tmp_path), flip_normals=True)
    finally:
        dmi.set_runtime(None, None)
    assert not any(m.modifiers for m in s["meshes"])


def test_flip_on_a_model_both_importers(tmp_path):
    """DaveMirra.ZMD has the most faces against its normals (77% agree): with the flip, both importers set
    flip_ref's normals and more faces agree (84%; the rest are faces whose winding disagrees with their
    neighbours', which a per-vertex flip cannot fix)."""
    p = _zmd("DaveMirra")
    _py()
    try:
        s = dmi.import_zmd(str(p), scale=SCALE, texture_dir=str(tmp_path / "py"), explicit_normals=True, flip_normals=True)
    finally:
        dmi.set_runtime(None, None)
    _, (agree, checked) = chk.check_import(p, s["root"], s["frames"], s["meshes"], tmp_path / "py", normals=True, flip=True)
    assert 0.8 * checked < agree < 0.9 * checked
    it = _ms(tmp_path)
    root, frames, meshes, folder = it.fn("DM_ImportZmd")(str(p), SCALE, 0, True, str(tmp_path / "ms"),
                                                         exactNormals=True, flipNormals=True)
    _, wind = chk.check_import(p, root, frames, meshes, tmp_path / "ms", normals=True, flip=True)
    assert wind == (agree, checked)


@pytest.mark.parametrize("d", _level_params())
def test_flip_every_level_python(d, tmp_path):
    """Every level with exact normals + flip: the explicit normals are flip_ref of the game's (World, every
    prop, Panorama; check_level), and the World now agrees with the CCW winding on nearly every face,
    ProTourDirt and WoodwardStreet included (15% and 30% without the flip)."""
    _py()
    try:
        s = dmi.import_level(str(d), scale=SCALE, texture_dir=str(tmp_path), explicit_normals=True, flip_normals=True)
    finally:
        dmi.set_runtime(None, None)
    _, wind = chk.check_level(d, s["root"], s["world"], s["props"], list(s["tags"].values()), s["points"],
                              s["panorama"], tmp_path, normals=True, flip=True)
    agree, checked = wind["world"]
    assert agree > 0.95 * checked, (d.name, agree, checked)


@pytest.mark.parametrize("name", ["ProTourDirt", "WoodwardStreet"])
def test_flip_level_maxscript_matches_python(name, tmp_path):
    d = next((x for x in _level_dirs() if x.name == name), None)
    if d is None:
        pytest.skip("no corpus")
    it = _ms(tmp_path)
    root, world, props, tags, points, pano, folder = it.fn("DM_ImportLevel")(str(d), SCALE, str(tmp_path / "ms"),
                                                                              exactNormals=True, flipNormals=True)
    _, wind = chk.check_level(d, root, world, props, tags, points, pano, tmp_path / "ms", normals=True, flip=True)
    _py()
    try:
        s = dmi.import_level(str(d), scale=SCALE, texture_dir=str(tmp_path / "py"), explicit_normals=True, flip_normals=True)
    finally:
        dmi.set_runtime(None, None)
    a, b = chk.edit_normals(world).result(), chk.edit_normals(s["world"]).result()
    assert a.keys() == b.keys() and all(chk._close(a[v], b[v], 1e-12) for v in a)
    assert wind["world"][0] > 0.95 * wind["world"][1]


def test_level_glb_props_use_the_stored_rows():
    """dmbmx level export (glTF, the Blender path): each prop node's matrix columns are its stored
    rows (glTF column-major 4x4: columns right, up, at, position), and the OBJ path uses the same
    prop_instances rotation."""
    dirs = _level_dirs()
    if not dirs:
        pytest.skip("no corpus")
    from dmbmx.convert import gltf, level_export
    lv = L.Level.read_dir(str(next(d for d in dirs if d.name == "AcclaimStreet")))
    js, _ = gltf.read_glb(level_export.export_glb(lv, tags=False, points=False))
    nodes = {n["name"]: n for n in js["nodes"]}
    yawed = 0
    for k, p in enumerate(lv.props.props):
        m = nodes[f"{p.name}_{k}"]["matrix"]
        assert [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]] == pytest.approx(list(p.matrix), abs=1e-6)
        yawed += abs(p.matrix[2]) > 0.2
    assert yawed > 30

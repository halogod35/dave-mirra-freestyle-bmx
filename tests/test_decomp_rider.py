"""Self-consistency checks for the decomp of rider.c (state machine, trick selection, motions).

No C compiler on the build PC, so the C is checked the way the other decomp tests do it:
1. structure: all 34 rider.c functions drafted, names agree with decomp/functions.csv and
   decomp/symbols/symbols.csv (last row wins), no Ghidra artefacts, the rider.h layouts agree with their "+0x.."
   comments and ghidra_types.txt, every assert (message, file, line) is a call site in NgBMX.exe;
2. exe facts the C relies on: every BmxRider field rider.c names is accessed at that displacement
   inside rider.c's code, the state-procedure tables point at the named functions, the motion ids
   written as immediates, the selection tables and constants;
3. behaviour: dmbmx.formats.tricks (trick selection port) against the exe tables, the motion bank
   coverage, and the root-motion scale (engine units = 100 x model units) against the feet-on-pedals
   placement of dmbmx.convert.combo.
"""
import csv
import math
import re
import statistics
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR
from dmbmx.formats import tricks as T

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
RIDER = "zaxis/Bmx/Src/com/action/rider.c"
RIDER_LO, RIDER_HI = 0x00430E70, 0x00436500
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")
B = 0x400000


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions():
    out = []
    for m in VA_RE.finditer((SRC / RIDER).read_text(encoding="utf-8")):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == kind:
                out[int(r["address"], 16)] = r
    return out


# ------------------------------------------------------------------ structure
def test_every_function_drafted_and_named_consistently():
    rows = [r for r in csv_rows() if r["file"] == RIDER]
    assert len(rows) == 34
    cf = c_functions()
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    for va, size, status, name in cf:
        r = by_va[va]
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, hex(va)
        assert syms[va]["source_file"] == "Bmx/Src/com/action/rider.c"
        assert re.match(r"rider[A-Z]\w+$", name), name
    # the input copy came over from the replay/rider gap
    assert not (SRC / "_unassigned" / "gap_replay__rider.c").exists()


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|rider_[0-9a-f]{8}|rider_Create)\b")
    t = (SRC / RIDER).read_text(encoding="utf-8")
    assert "TODO(decomp)" not in t
    for n, line in enumerate(t.splitlines(), 1):
        assert not bad.search(line), f"{n}: {line.strip()}"
    for p in SRC.rglob("*.c"):
        code = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        assert not re.search(r"\b(rider_[0-9a-f]{8}|rider_Create)\b", code), p


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}


def layouts():
    out = {}
    t = (INC / "rider.h").read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields, maxal = 0, {}, 1
        for line in m.group(2).splitlines():
            d = re.match(r"\s*(?:const\s+)?(\w+)\s*(\*?)\s*(\w+)((?:\[\w+\])*);\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?", line)
            if not d:
                continue
            typ, ptr, name, dims, comment = d.groups()
            if ptr:
                size = align = 4
            elif typ in SIZES:
                size = align = SIZES[typ]
            else:
                size, align = out[typ][0], out[typ][2]
            count = 1
            for k in re.findall(r"\[(\w+)\]", dims):
                count *= int(k, 0)
            off = (off + align - 1) // align * align
            assert comment is None or int(comment, 16) == off, (m.group(3), name, hex(off))
            fields[name] = off
            off += size * count
            maxal = max(maxal, align)
        out[m.group(3)] = ((off + maxal - 1) // maxal * maxal, fields, maxal)
    return out


RIDER_C_FIELDS = {   # BmxRider fields rider.c added (offset)
    "dirCount": 0x5, "dirBuf": 0x6, "input": 0xC, "state": 0x3D, "stateTimer": 0x42, "impactTimer": 0x4D,
    "rotVel": 0x478, "airLean": 0x480, "trickMatrix": 0x734,
    "flipVel": 0x774, "motion": 0xB9C, "modBlend": 0xBBC, "rootMotion": 0xBFC, "bikeMatrix": 0xC20,
    "pedalForce": 0xC60, "wheelDrag": 0xC64,
}
# physics.c merge: snapFrames -> physTimer, carriedSpin -> physAirRot[1], the wheel fields -> wheel[2]
RIDER_C_PATHS = {"physTimer": 0x3DE, "physAirRot": 0x72C, "wheel": 0xC88}
WHEEL_FIELDS = {"wheel[0].speed": 0xCAC, "wheel[0].contact": 0xCF0, "wheel[1].speed": 0xD30, "wheel[1].contact": 0xD74}


def test_struct_layouts():
    lay = layouts()
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    for n in ("BmxInput", "BmxMotionSlot", "BmxSigCombo", "BmxRider"):
        assert lay[n][0] == want[n], n
    assert (lay["BmxInput"][0], lay["BmxMotionSlot"][0], lay["BmxSigCombo"][0], lay["BmxRider"][0]) == \
        (0x28, 0x10, 0x6, 0xF70)
    f = lay["BmxRider"][1]
    for n, o in list(RIDER_C_FIELDS.items()) + list(RIDER_C_PATHS.items()):
        assert f[n] == o, (n, hex(f[n]))
    w = lay["BmxWheel"][1]
    for path, o in WHEEL_FIELDS.items():
        i, field = int(path[6]), path.split(".")[1]
        assert f["wheel"] + i * lay["BmxWheel"][0] + w[field] == o, path
    # game.c's fields did not move
    for n, o in {"stateFlags": 0x38, "slot": 0x3C, "trickId": 0x4A, "spin": 0x50, "flip": 0x54, "crash": 0x58,
                 "matrix": 0x3E0, "pos": 0x420, "frameDist": 0x484, "stats": 0x6D0, "contact": 0x77C,
                 "riderActor": 0xB94, "animCtl": 0xB98, "cleared_bc0": 0xBC0, "bikeModel": 0xC0C,
                 "wheel": 0xC88, "bikeActor": 0xD90, "ik": 0xD98}.items():
        assert f[n] == o, n
    t = (INC / "rider.h").read_text(encoding="utf-8")
    for n in ("BmxInput", "BmxMotionSlot", "BmxSigCombo"):
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        snames = {r["name"] for r in csv.DictReader(fh) if r["kind"] == "struct" and r["type"] == "decomp/include/rider.h"}
    assert {"BmxInput", "BmxMotionSlot", "BmxSigCombo"} <= snames


# ------------------------------------------------------------------ exe
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def u32(img, va):
    return struct.unpack_from("<I", img, va - B)[0]


def f32(img, va):
    return struct.unpack_from("<f", img, va - B)[0]


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def test_assert_sites_exist_in_exe(img):
    t = (SRC / RIDER).read_text(encoding="utf-8")
    path = re.search(r'#define RIDER_SRC "([^"]+)"', t).group(1).replace("\\\\", "\\")
    sites = {(m.group(1).encode().decode("unicode_escape"), int(m.group(2), 16))
             for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", RIDER_SRC, (0x[0-9a-f]+)', t)}
    assert len(sites) == 4
    p_addr = img.find(path.encode() + b"\0") + B
    assert p_addr == 0x005589D0
    for msg, line in sites:
        addrs = [m.start() + B for m in re.finditer(re.escape(msg.encode() + b"\0"), img)]
        pats = [push(line) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", a) for a in addrs]
        assert any(p in img[RIDER_LO - B:RIDER_HI - B] for p in pats), (msg, hex(line))


def rider_code(img):
    cs = pytest.importorskip("capstone")
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    out = []
    for r in csv_rows():
        if r["file"] == RIDER:
            va, n = int(r["va"], 16), int(r["size"])
            out += list(md.disasm(img[va - B:va - B + n], va))
    return out


def test_rider_fields_are_accessed_by_rider_c(img):
    ins = rider_code(img)
    disps = set()
    for i in ins:
        for m in re.finditer(r"\[e(?:a|b|c|d)x \+ (0x[0-9a-f]+)\]|\[e(?:s|d)i \+ (0x[0-9a-f]+)\]|\[ebp \+ (0x[0-9a-f]+)\]",
                             i.op_str):
            disps.add(int(next(g for g in m.groups() if g), 16))
    # riderUpdateAll walks the riders with a pointer biased by +0x14 / +0x10 (input copy)
    biased = {d + 0x14 for d in disps} | {d + 0x10 for d in disps}
    for n, o in RIDER_C_FIELDS.items():
        if n in ("dirBuf", "dirCount", "input", "trickMatrix", "bikeMatrix"):
            continue                                  # reached through lea / loops (checked by hand)
        assert o in disps or o in biased, (n, hex(o))
    for n, o in [("physTimer", 0x3DE), ("physAirRot[1]", 0x72E), ("wheel[0].contact", 0xCF0)]:
        assert o in disps or o in biased, (n, hex(o))
    # every field write the C names exists: the motion slot and timers
    for o in (0xB9C, 0xBA0, 0xBA4, 0xBA8, 0xBAC, 0xBB0, 0xBB4, 0xBB8, 0xBBC, 0x42, 0x44, 0x46, 0x48):
        assert o in disps or o in biased, hex(o)


def test_state_tables_point_at_the_named_functions(img):
    name = {int(r["va"], 16): r["name"] for r in csv_rows()}
    tables = {0x005588B8: ["riderStateRiding", "riderStateAir", "riderStateGrind", "riderStateStall",
                           "riderStateManual", "riderStateLanding", "riderStateCrash"],
              0x005588D4: ["riderRideCoast", "riderRidePedal", "riderRideSkid", "riderRideCrouch",
                           "riderRideTurnaround", "riderRideNull"],
              0x005588EC: ["riderAirIdle", "riderAirTrick", "riderAirModifier"],
              0x005588F8: ["riderGrind"], 0x005588FC: ["riderStall"], 0x00558900: ["riderManual"]}
    for va, names in tables.items():
        assert [name[u32(img, va + 4 * i)] for i in range(len(names))] == names, hex(va)
    # the C's tables are declared with these sizes
    t = (SRC / RIDER).read_text(encoding="utf-8")
    for decl in ("g_riderMainProc[7]", "g_riderRideProc[6]", "g_riderAirProc[3]", "g_riderGrindProc[1]"):
        assert decl in t


def test_motion_immediates(img):
    """The constant motion ids rider.c writes to motion[0] / motion[1] (mov word [r + 0xB9C/0xBAC], imm)."""
    ids = {}
    for i in rider_code(img):
        m = re.match(r"word ptr \[e\w\w \+ 0x(b9c|bac)\], (0x[0-9a-f]+|\d+)$", i.op_str)
        if i.mnemonic == "mov" and m:
            ids.setdefault(int(m.group(2), 0), set()).add(m.group(1))
    assert ids.pop(T.SECOND_SLOT_MOTION) == {"bac"}
    # 2 (idle) goes through a register, 19 is computed: (motion == 15) + 19
    assert set(ids) == set(T.STATE_MOTIONS) - {2, 19}
    t = (SRC / RIDER).read_text(encoding="utf-8")
    defs = {int(v) for v in re.findall(r"#define MOTION_[A-Z_]+\s+(\d+)\s", t)}
    assert defs == set(T.STATE_MOTIONS)


def test_tables_match_exe(img):
    b = lambda va, n: tuple(img[va - B:va - B + n])  # noqa: E731
    assert b(T.DIR_FROM_MASK_VA, 16) == T.DIR_FROM_MASK
    assert b(T.AIR_TRICK_DIR1_VA, 9) == T.AIR_TRICK_DIR1
    assert tuple(b(T.AIR_TRICK_DIR2_VA + 8 * p, 8) for p in range(8)) == T.AIR_TRICK_DIR2
    assert b(T.MOD_DIR1_VA, 9) == T.MOD_DIR1
    assert tuple(b(T.MOD_DIR2_VA + 8 * p, 8) for p in range(8)) == T.MOD_DIR2
    assert tuple(b(T.TRICK_MOD_TABLE_VA + 38 * t, 38) for t in range(27)) == T.trick_mod_table()
    assert T.TRICK_MOD_TABLE_VA + 27 * 38 + 2 == T.GRIND_TRICK_DIR_VA      # 27 rows, 2 pad bytes, next table
    assert b(T.GRIND_TRICK_DIR_VA - 2, 2) == (0, 0)
    assert b(T.GRIND_TRICK_DIR_VA, 9) == T.GRIND_TRICK_DIR
    assert b(T.STALL_TRICK_DIR_VA, 9) == T.STALL_TRICK_DIR
    assert b(T.PLANT_TRICK_DIR_VA, 9) == T.PLANT_TRICK_DIR
    for r in range(12):
        combos = []
        for i in range(4):
            d = b(T.SIGNATURE_TRICKS_VA + r * 0x18 + i * 6, 6)
            if d[0]:
                combos.append((d[1:1 + d[0]], d[4], d[5]))
            else:
                assert d == (0, 0, 0, 0, 0xFF, 0xFF)
        assert tuple(combos) == T.SIGNATURE_TRICKS[r], r
    consts = {0x0055886C: 43.0, 0x00558870: 3.0, 0x00558874: 2.4, 0x00558878: 21.0, 0x0055887C: 15.0,
              0x00558888: 0.1, 0x0055888C: 15.0, 0x00558890: 75.0, 0x00558894: 0.15, 0x00558898: 0.2,
              0x0055889C: 8.0, 0x005588A0: 12.0, 0x005588A4: 3.0, 0x005588A8: 12.0, 0x005588B0: 1.5}
    for va, v in consts.items():
        assert f32(img, va) == pytest.approx(v), hex(va)
    assert u32(img, 0x005588B4) == 25
    assert [round(f32(img, 0x00558914 + 4 * i), 2) for i in range(14)] == \
        [0.67, 0.38, 0.38, 0.5, 0.5, 0.44, 0.44, 0.5, 0.5, 0.56, 0.56, 0.5, 0.5, 2.0]
    assert f32(img, 0x0054D1F0) == pytest.approx(1 / 12)           # riderAddCrashDamage
    assert f32(img, 0x0054D01C) == pytest.approx(65536 / 360)       # DEG_TO_ANGLE


# ------------------------------------------------------------------ behaviour (Python port)
def test_selection_port():
    # single directions, pairs, signature combos
    assert [T.select_air_trick([d]) for d in range(1, 9)] == \
        [(0, None), (1, None), (2, None), (3, None), (7, None), (4, None), (5, None), (6, None)]
    assert T.select_air_trick([4, 4]) == (8, None)                   # down, down: double backflip
    assert T.select_air_trick([3, 3]) == (10, None)                  # up, up: front flip
    assert T.select_air_trick([1, 4]) == (12, None)                  # left, down: turndown backflip
    assert T.select_air_trick([4, 4, 4], rider=0) == (9, None)       # Dave Mirra: triple backflip
    assert T.select_air_trick([4, 4, 4], rider=1) == (8, None)       # others: the pair table
    assert T.select_air_trick([3, 2, 6, 2], rider=0) == (26, 17)     # oldest entry ignored
    assert T.select_air_trick([]) == (None, None)
    assert T.select_air_trick([5, 1]) == (0, None)                   # (up-left, left) unset: last one
    for p in range(1, 9):
        for last in range(1, 9):
            t, m = T.select_air_trick([p, last], rider=11)
            assert t is not None and 0 <= t < T.AIR_TRICKS and m is None
    # modifiers: identity except the per-trick exceptions; rocket airs refused on can-cans
    assert T.select_modifier(0, [4]) == 0                            # barspin
    assert T.trick_mod_table()[1][28] == T.NONE and T.select_modifier(1, [5, 3]) is None   # crotch rocket
    assert T.select_modifier(1, [5]) == 32                           # can-can + rocket air -> rocket one footer
    assert T.modifier_slot([]) is None
    assert T.select_ground_trick("grind") == 27                      # no direction: double peg
    assert T.select_ground_trick("stall", [3]) == 37
    assert T.select_ground_trick("plant", [1]) == T.FAST_PLANT
    assert T.select_ground_trick("plant", [2]) == T.WALL_TAP
    assert T.select_ground_trick("plant", [3]) is None
    assert T.direction(0, -1.0, 0.0) == 1 and T.direction(0, 0.0, -1.0) == 3 and T.direction(2) == 4
    assert T.direction(0, 0.7, 0.7) == 8 and T.direction(0, 0.3, 0.3) == 0


def test_plant_motions_and_bank_coverage():
    assert T.trick_motions(T.FAST_PLANT) == (232, 233, 234)
    assert T.trick_motions(T.WALL_TAP) == (235, 236, 237)
    used = set(T.STATE_MOTIONS) | {T.SECOND_SLOT_MOTION}
    for t in range(46):
        used |= set(T.trick_motions(t) or ())
    for m in range(T.MODIFIERS):
        used |= set(T.modifier_motions(m))
    assert used | set(T.UNUSED_MOTIONS) == set(range(1, 239))
    assert not used & set(T.UNUSED_MOTIONS)


# ------------------------------------------------------------------ root motion (needs the assets)
@pytest.fixture(scope="module")
def models():
    a = GAME_DIR / "assets"
    if not (a / "motion.zal").is_file():
        pytest.skip("no game assets")
    from dmbmx.formats import motion as M
    from dmbmx.formats import zmd
    return (zmd.read((a / "DaveMirra1.ZMD").read_bytes()).skeleton(),
            zmd.read((a / "DMbike01.zmd").read_bytes()).skeleton(), M.read_bank((a / "motion.zal").read_bytes()))


def test_root_motion_scale_is_one_hundredth(models):
    """The clip's bike-section translation b places the bike relative to the rider in engine units:
    F(-b) / 100 plus one constant reproduces the feet-on-pedals placement of every on-pedal frame of the
    bank; 1/200 (the first motion reader's guess) and 1/50 do much worse, and the per-clip calibration of
    combo.rider_offsets(fill="engine") brings it to ~2 cm."""
    from dmbmx.convert import combo
    rsk, bsk, bank = models
    data, per_clip = [], []
    for mid in bank.ids:
        mo = bank.motion(mid)
        info = {}
        feet = combo.rider_offsets(rsk, bsk, mo, info, fill="interpolate")
        good = [(feet[i], p.bike_trans) for i, (p, g) in enumerate(zip(mo.sample(), info["good"])) if g]
        data += good
        if len(good) >= 3:
            eng = combo.engine_offsets(mo, (0.0, 0.0, 0.0))
            idx = [i for i, g in enumerate(info["good"]) if g]
            c = [statistics.median(feet[i][a] - eng[i][a] for i in idx) for a in range(3)]
            per_clip += [math.dist(feet[i], [eng[i][a] + c[a] for a in range(3)]) for i in idx]
    assert len(data) > 1000

    def residual(s):
        e = [(-b[0] * s, b[1] * s, b[2] * s) for _, b in data]
        c = [statistics.median(f[a] - x[a] for (f, _), x in zip(data, e)) for a in range(3)]
        return statistics.median(math.dist(f, [x[a] + c[a] for a in range(3)]) for (f, _), x in zip(data, e)), c

    r100, c100 = residual(combo.ENGINE_SCALE)
    assert combo.ENGINE_SCALE == 0.01 and r100 < 0.06
    assert c100 == pytest.approx(combo.ENGINE_OFFSET, abs=0.002)
    assert residual(0.005)[0] > 1.3 * r100 and residual(0.02)[0] > 4 * r100
    assert statistics.median(per_clip) < 0.03

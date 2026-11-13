"""DM-058 self-consistency: decomp of the crash rag doll and the limb IK (bmx/dyn.c, the old
bmxsound/zoverii gap), zoveriiInit (zoverii.c) and gameSetWord (game.c).

No C compiler on the build PC, so the C is checked the way DM-041..DM-057 do it:
1. structure: all 74 dyn.c functions drafted, names agree with decomp/functions.csv and decomp/symbols/symbols.csv
   (the last row wins), no Ghidra artefacts and no old names (FUN_ of the gap, DM-047's
   dynStart/dynStop/dynIsRunning/dynGoLimp/dynRestore*, cameraInitRider/cameraMeasureRider, r->camera)
   anywhere in decomp/src; the gap file is gone; dyn.h / BmxIk types size-checked in C, in
   ghidra_types.txt, with struct rows, and their "+0x.." comments agree with a computed x86 layout;
2. exe facts: dynCreate's allocation, the particle stride, the .data constants, the rider springs
   (every dynAddSpring call of dynCreateRider: particles and weight), the particle -> skeleton bone
   table (dynPoseFromActor's jump table), the IK limb lengths (dynIkMeasure: part -> BmxIk offset), the
   tuck targets, zoveriiInit's pool sizes, gameSetWord's assert site;
3. motion.py: the bone / channel names the C gives particles, pose channels and IK parts equal
   dmbmx.formats.motion.RIDER_CHANNELS at those indices.
"""
import csv
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR
from dmbmx.formats import motion as M

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
DYN = "zaxis/Bmx/Src/com/zge/bmx/dyn.c"
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")
B = 0x400000
CREATED = {0x00453AB0: "dynBrace", 0x004542F0: "dynSetLimitsOff", 0x00454320: "dynSetLimitsOn",
           0x00455E90: "dynIkSetLooseGains"}


def text(f):
    return (SRC / f).read_text(encoding="utf-8")


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def c_functions(f):
    out = []
    for m in VA_RE.finditer(text(f)):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", " ".join(m.group(4).split())).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        for r in csv.DictReader(fh):
            if r["kind"] == kind:
                out[r["name"] if kind == "struct" else int(r["address"], 16)] = r
    return out


# ------------------------------------------------------------------ structure
def test_dyn_functions_drafted_and_named():
    rows = [r for r in csv_rows() if r["file"] == DYN]
    assert len(rows) == 74
    cf = c_functions(DYN)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    for va, size, status, name in cf:
        r = by_va[va]
        assert 0x00450060 <= va < 0x004578F0
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name and syms[va]["card"] == "DM-058", hex(va)
        assert syms[va]["source_file"] == "Bmx/Src/com/zge/bmx/dyn.c"
        assert name.startswith("dyn"), name
    names = {va: n for va, _, _, n in cf}
    for va, n in CREATED.items():
        assert names[va] == n
    assert re.search(r"\* 74 functions, VA 0x", text(DYN))
    assert not (SRC / "_unassigned" / "gap_bmxsound__zoverii.c").exists()


def test_moved_and_created_functions():
    syms = last_symbols("func")
    for f, va, name, n in (("zaxis/Bmx/Src/com/zge/bmx/zoverii.c", 0x004578F0, "zoveriiInit", 19),
                           ("zaxis/Bmx/Src/com/action/game.c", 0x0040BFD0, "gameSetWord", 47)):
        cf = {v: (s, st, nm) for v, s, st, nm in c_functions(f)}
        assert len(cf) == n and cf[va][2] == name and cf[va][1] == "draft"
        assert syms[va]["name"] == name and syms[va]["card"] == "DM-058"
        row = [r for r in csv_rows() if int(r["va"], 16) == va][0]
        assert row["file"] == f and row["status"] == "draft" and int(row["size"]) == cf[va][0]


def test_no_artefacts_or_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+)\b")
    for n, line in enumerate(text(DYN).splitlines(), 1):
        assert not bad.search(line), f"{n}: {line.strip()}"
    old = re.compile(r"\b(FUN_0045[0-7][0-9a-fA-F]{3}|cameraInitRider|cameraMeasureRider|dynStart|dynStop|"
                     r"dynIsRunning|dynGoLimp|dynRestoreJoints|dynRestoreLimbs|camScale)\b")
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        code = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        m = old.search(code)
        assert m is None, (p, m and m.group(0))
        if p.name in ("rider.c", "physics.c"):         # the rider's IK block is r->ik now
            assert "r->camera" not in code, p


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4, "RwV3d": 12, "RwMatrix": 64, "DynImpactFn": 4}
DYN_STRUCTS = {"DynParticleDesc": 0x20, "DynParticle": 0xB4, "DynSpring": 0x20, "DynLimitDesc": 0x24,
               "DynLimit": 0x28, "DynBody": 0xE88, "DynSlab": 0x1C}


def layout(header, name, known):
    """x86 layout of a struct: [(field, offset, commented offset or None)], size."""
    t = (INC / header).read_text(encoding="utf-8")
    body = re.search(r"typedef struct %s \{(.*?)\} %s;" % (name, name), t, re.S).group(1)
    off, out, align = 0, [], 1
    for m in re.finditer(r"^\s*([A-Za-z_]\w*)\s+(\*?)(\w+)((?:\[[^\]]+\])*);(?:\s*/\* \+0x([0-9A-Fa-f]+))?",
                         body, re.M):
        ty, ptr, field, dims, cm = m.groups()
        size = 4 if ptr else known[ty]
        al = 4 if ptr else min(4, {1: 1, 2: 2}.get(known[ty], 4))
        n = 1
        for d in re.findall(r"\[([^\]]+)\]", dims):
            n *= int(d, 0)
        off = (off + al - 1) // al * al
        out.append((field, off, int(cm, 16) if cm else None))
        off += size * n
        align = max(align, al)
    return out, (off + align - 1) // align * align


def test_types_layout_and_registration():
    known = dict(SIZES)
    known.update({"DynParticle": 0xB4, "DynSpring": 0x20, "DynLimit": 0x28, "BmxIkTarget": 0x20})
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert types.index("dyn.h") > types.index("ngif.h") > types.index("rider.h")
    structs = last_symbols("struct")
    for header, items in (("dyn.h", DYN_STRUCTS), ("rider.h", {"BmxIkTarget": 0x20, "BmxIk": 0x1D8})):
        t = (INC / header).read_text(encoding="utf-8")
        for n, size in items.items():
            fields, computed = layout(header, n, known)
            assert computed == size == want[n], (n, hex(computed))
            for field, off, cm in fields:
                assert cm is None or cm == off, (n, field, hex(off), cm and hex(cm))
            assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, size), t), n
            assert structs[n]["card"] == "DM-058" and structs[n]["type"] == "decomp/include/" + header
    # BmxIk replaces the camera block and still ends the 0xF70 rider
    rider, _ = layout("rider.h", "BmxRider", {**known, "BmxTrickRec": 0x10, "BmxScoreBlock": 0x80,
                                              "BmxAreaStats": 0x0E, "BmxJudgeRun": 0x0C, "BmxContact": 0x20C,
                                              "BmxInput": 0x28, "BmxMotionSlot": 0x10, "BmxSigCombo": 6,
                                              "BmxWheel": 0x84, "BmxIk": 0x1D8})
    ik = [f for f in rider if f[0] == "ik"]
    assert ik and ik[0][2] == 0xD98


# ------------------------------------------------------------------ exe
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def disasm(img, va, size):
    cs = pytest.importorskip("capstone")
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    md.detail = True
    return list(md.disasm(img[va - B:va - B + size], va))


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]


def c_float(s):
    return float(s.strip().rstrip("f"))


def test_allocation_stride_and_data(img):
    assert img[0x00450060 - B:0x00450065 - B] == b"\x68" + struct.pack("<I", 0xE88)
    # particle stride: dynCreateRider's lowest-y loops step 0xB4
    assert any(i.mnemonic == "add" and i.op_str.endswith(", 0xb4") for i in disasm(img, 0x00452E80, 1200))
    t = text(DYN)
    for name, va in (("g_dynGravity", 0x0055B958), ("g_dynGrabDrop", 0x0055B95C)):
        v = c_float(re.search(r"float %s = ([-\d.]+f);\s+/\* 0x%08X" % (name, va), t).group(1))
        assert abs(struct.unpack_from("<f", img, va - B)[0] - v) < 1e-6, name


def test_rider_springs(img):
    """Every dynAddSpring call (thunk 0x0040169A) of dynCreateRider, emulating register constants."""
    regs, pushes, calls = {}, [], []
    for i in disasm(img, 0x00452E80, 1200):
        if i.mnemonic == "mov" and len(i.operands) == 2 and i.operands[0].type == 1 and i.operands[1].type == 2:
            regs[i.reg_name(i.operands[0].reg)] = i.operands[1].imm & 0xFFFFFFFF
        elif i.mnemonic == "xor" and i.op_str.split(", ")[0] == i.op_str.split(", ")[-1]:
            regs[i.op_str.split(", ")[0]] = 0
        elif i.mnemonic == "push":
            op = i.operands[0]
            pushes.append(op.imm & 0xFFFFFFFF if op.type == 2 else regs.get(i.reg_name(op.reg)) if op.type == 1 else None)
        elif i.mnemonic == "call":
            if i.op_str == "0x40169a":
                k, j, w = pushes[-2], pushes[-3], pushes[-4]     # cdecl: (body, i, j, weight)
                calls.append((k, j, round(f32(w), 4)))
            pushes = []
    c = [(int(a), int(b), round(c_float(w), 4))
         for a, b, w in re.findall(r"dynAddSpring\(b, (\d+), (\d+), ([\d.]+f)\)",
                                   text(DYN).split("void *dynCreateRider(")[1].split("\n}\n")[0])]
    assert len(calls) == 21 and calls == c


def test_particle_bones_and_motion_names(img):
    t = text(DYN)
    body = re.search(r"s_dynParticleBone\[DYNP_RIDER_COUNT\] = \{(.*?)\};", t, re.S).group(1)
    c = [(int(b), name) for b, name in re.findall(r"^\s*(\d+),?\s*/\* \d+ .*?(\w+) \*/", body, re.M)]
    assert len(c) == 14
    table = struct.unpack_from("<14I", img, 0x00452DA8 - B)
    for k, target in enumerate(table):
        disp = bone = None
        for i in disasm(img, target, 0x20):
            if i.mnemonic == "lea" and disp is None:
                disp = i.operands[1].mem.disp
            if i.mnemonic == "push" and i.operands[0].type == 2:
                bone = i.operands[0].imm
            if i.mnemonic == "jmp":
                break
        assert (disp - 0x58) // 0xB4 == k and (disp - 0x58) % 0xB4 == 0
        assert bone == c[k][0], k
        assert M.RIDER_CHANNELS[bone] == c[k][1], (k, bone)


def test_pose_channels_match_motion():
    t = text(DYN)
    step = t.split("void dynStepRider(")[1].split("\n}\n")[0]
    ik = t.split("void dynIkSolve(")[1].split("\n}\n")[0]
    seen = {}
    for idx, name in re.findall(r"rot\[(\d+)\]\);?\s+/\* (\w+) \*/", step):
        seen[int(idx)] = name
    assert sorted(seen) == list(range(15))
    for idx, name in seen.items():
        assert M.RIDER_CHANNELS[idx] == name
    pairs = re.findall(r"rot\[(\d+)\], rot\[(\d+)\]\);\s+/\* (\w+), (\w+) \*/", ik)
    assert len(pairs) == 4
    for a, b, na, nb in pairs:
        assert (M.RIDER_CHANNELS[int(a)], M.RIDER_CHANNELS[int(b)]) == (na, nb)


def test_ik_lengths(img):
    """dynIkMeasure: push part; call; fstp [esi + off]  ==  ik->field = zactorGetPartLength(a, part)."""
    exe, part = [], None
    for i in disasm(img, 0x00455F10, 216):
        if i.mnemonic == "push" and i.operands[0].type == 2:
            part = i.operands[0].imm
        if i.mnemonic in ("fstp", "fst") and part is not None:
            exe.append((part, i.operands[0].mem.disp))
            part = None
    fields = {f: off for f, off, _ in layout("rider.h", "BmxIk", {**SIZES, "BmxIkTarget": 0x20})[0]}
    body = text(DYN).split("void dynIkMeasure(")[1].split("\n}\n")[0]
    c = [(int(p), fields[f], n) for f, p, n in
         re.findall(r"ik->(\w+) = zactorGetPartLength\(a, (\d+)\);\s+/\* (\w+) \*/", body)]
    assert [(p, o) for p, o, _ in c] == exe and len(exe) == 8
    for p, _, n in c:
        assert M.RIDER_CHANNELS[p] == n


def test_tuck_targets(img):
    exe = [f32(i.operands[1].imm) for i in disasm(img, 0x00453C30, 660)
           if i.mnemonic == "mov" and i.operands[0].type == 3 and i.operands[1].type == 2]
    body = re.search(r"s_dynTuck\[8\] = \{(.*?)\};", text(DYN), re.S).group(1)
    c = [c_float(v) for row in re.findall(r"\{([^}]*)\}", body) for v in row.split(",")[3:]]
    assert len(c) == 24
    nz = [v for v in c if v != 0.0]
    assert [round(v, 3) for v in exe if v != 0.0] == [round(v, 3) for v in nz]


def test_zoverii_init_pools(img):
    counts = [i.operands[1].imm for i in disasm(img, 0x004578F0, 409)
              if i.mnemonic == "mov" and i.operands[0].type == 1 and i.operands[1].type == 2
              and i.operands[1].imm in (0xF, 0x10, 0x7F, 0x80, 0x1B, 0x1C, 0x190)]
    counts += [i.operands[1].imm for i in disasm(img, 0x004578F0, 409)
               if i.mnemonic == "mov" and i.operands[0].type == 3 and i.operands[1].imm == 0x18F]
    t = text("zaxis/Bmx/Src/com/zge/bmx/zoverii.c").split("int zoveriiInit(")[1].split("\n}\n")[0]
    c = [int(n) for n in re.findall(r"zoverInitPool\(\w+\[b\], (\d+)\)", t)]
    assert c == [16, 128, 28, 400]
    assert sorted(counts) == sorted([n - 1 for n in c] + c)


def test_game_set_word_assert(img):
    msg = img.find(b"horse word too long\0") + B
    path = img.find(b"C:\\zaxis\\Bmx\\Src\\com\\action\\game.c\0") + B
    pat = b"\x68" + struct.pack("<I", 0x5E4) + b"\x68" + struct.pack("<I", path) + b"\x68" + struct.pack("<I", msg)
    assert img[0x0040BFD0 - B:0x0040C029 - B].find(pat) > 0
    assert re.search(r'zAssertFail\("horse word too long", GAME_SRC, 0x5e4, 0\)',
                     text("zaxis/Bmx/Src/com/action/game.c"))

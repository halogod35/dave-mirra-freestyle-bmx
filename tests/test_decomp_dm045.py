"""DM-045 self-consistency: decomp of zmath.c, zquat.c, zanim.c (+ the two plane helpers in
_unassigned/gap_zrwCol__zobjPEmit.c).

No C compiler on the build PC, so the C is checked two ways (as DM-041..DM-044 do):
1. structure: every function of the three files is drafted, its name agrees with
   decomp/functions.csv and decomp/symbols/symbols.csv (the last row wins, as in ApplySymbols), no Ghidra
   artefacts remain, the zanim.h / zmath.h layouts agree with their "+0x.." comments and the
   ghidra_types.txt sizes, every assert (message, file, line) exists as a call site in NgBMX.exe,
   and the table/constant facts the C relies on are read back from the exe;
2. behaviour: Python ports of the C (table-exact: zmathInitTables' sine/asin/atan tables,
   zquatFromEuler's truncated half angles, zquatMul, zquatToMatrix, zmathAtan2,
   zmathMatrixToEuler, zquatSlerp, the zanim chunk handlers and blender) are checked against the
   motion reference dmbmx.formats.motion on every frame of every motion clip, and the node
   conversion zrwModelQuatToRwMatrix (q -> (x, -y, -z, w)) is shown to explain the DM-017 arm flip.
"""
import csv
import math
import re
import statistics
import struct
import sys
from pathlib import Path

import pytest

from conftest import GAME_DIR

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
FILES = {"NgEngine/Zge/zmath.c": 59, "NgEngine/Zge/zquat.c": 17, "NgEngine/Zge/zanim.c": 36}
GAP = "_unassigned/gap_zrwCol__zobjPEmit.c"
GAP_DRAFT = {0x004956F0: "zmathPlaneFromPointNormal", 0x00495730: "zmathClipSegmentToPlane"}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    """[(va, size, status, name)] in source order."""
    out = []
    for m in VA_RE.finditer((SRC / f).read_text(encoding="utf-8")):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbols():
    """{va: row} of func rows in decomp/symbols/symbols.csv, later rows win (ApplySymbols)."""
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == "func":
                out[int(r["address"], 16)] = r
    return out

# DM-061 created the three table builders in the zasset/zmath hole
DM061_ZMATH = {0x00480C40, 0x00480C80, 0x00480CD0}


# ------------------------------------------------------------------ structure
def test_every_function_drafted_and_named_consistently():
    rows = csv_rows()
    syms = last_symbols()
    for f, n in FILES.items():
        fr = [r for r in rows if r["file"] == f]
        assert len(fr) == n, (f, len(fr))
        cf = c_functions(f)
        assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in fr), f
        by_va = {int(r["va"], 16): r for r in fr}
        for va, size, status, name in cf:
            r = by_va[va]
            assert status == "draft" and r["status"] == "draft", (f, hex(va))
            assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
            assert syms[va]["name"] == name and syms[va]["card"] == ("DM-061" if va in DM061_ZMATH else "DM-045"), (hex(va), name)
            assert syms[va]["source_file"] == f, hex(va)
    gap = {va: (status, name) for va, _s, status, name in c_functions(GAP)}
    for va, name in GAP_DRAFT.items():
        assert gap[va] == ("draft", name) and syms[va]["name"] == name
    # zmath.c ends at 0x00482081: the zobjCamera vtable 0x00560518 starts with 0x00482090
    cam = {int(r["va"], 16): r for r in rows if r["file"] == "NgEngine/Zge/ZgeMods/zobjCamera.c"}
    assert {0x00482090, 0x004820C0, 0x004820F0, 0x00482110} <= set(cam)


def test_renamed_callers_use_the_new_names():
    for p in SRC.rglob("*.c"):
        t = p.read_text(encoding="utf-8")
        assert not re.search(r"\bzmath_[0-9a-f]{8}\b|\bzquat_\w+|\bzanimTableGetCount\b|\bzobj_(Find|Set)Node", t), p
    t = (SRC / "NgEngine/Zge/zScene.c").read_text(encoding="utf-8")
    assert "zanimGetId(table)" in t


def test_no_ghidra_artefacts():
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|PTR_\w+|in_stack_\w+|"
                     r"unaff_\w+|[iu]Var\d+|p[A-Z]\w*Var\d+|f\w*Var\d+|[a-z]Stack_\w+|float10|CONCAT\d+|"
                     r"TODO\(decomp\))\b")
    texts = [(f, (SRC / f).read_text(encoding="utf-8")) for f in FILES]
    g = (SRC / GAP).read_text(encoding="utf-8")
    texts.append((GAP, g[g.index("/* Plane helpers"):g.index("/* VA 0x00495860")]))
    for f, t in texts:
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
        for va, _s, _st, name in (c_functions(f) if f in FILES else []):
            assert not name.startswith("FUN_") and not re.search(r"_[0-9a-f]{8}$", name), name


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}


def layouts(header):
    """{struct: (size, [(field, offset, commented_offset)])} (x86, natural alignment)."""
    t = (INC / header).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    out = {}
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields = 0, []
        for line in m.group(2).splitlines():
            d = re.match(r"\s*((?:struct\s+)?\w+)\s*(\*?)\s*(\w+)(?:\[(\w+)\])?;\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?", line)
            if not d:
                continue
            typ, ptr, name, count, comment = d.groups()
            typ = typ.replace("struct ", "")
            size = 4 if ptr else SIZES.get(typ) or out[typ][0]
            align = 4 if ptr else min(size, 4)
            off = (off + align - 1) // align * align
            fields.append((name, off, int(comment, 16) if comment else None))
            off += size * (int(count, 0) if count else 1)
        out[m.group(3)] = ((off + 3) // 4 * 4, fields)
    return out


@pytest.mark.parametrize("header,structs", [
    ("zanim.h", {"ZanimKey", "ZanimChannel", "ZanimDefName", "ZanimNameList", "ZanimContents", "ZanimTrack",
                 "ZanimNode", "ZanimNodeTable", "ZanimObj", "ZanimWeight", "ZanimWeights"}),
    ("zmath.h", {"Zquat"}),
])
def test_struct_layouts(header, structs):
    lay = layouts(header)
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    assert set(lay) == structs
    assert header in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    t = (INC / header).read_text(encoding="utf-8")
    for name, (size, fields) in lay.items():
        assert size == want[name], (name, hex(size))
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (name, want[name]), t), name
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))
    # struct rows for every type, and none for the pre-DM-045 names
    srows = [r for r in csv.DictReader(open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8")) if r["kind"] == "struct"]
    names = {r["name"] for r in srows}
    assert structs <= names and not {"ZanimLink", "ZanimLink4"} & names


# ------------------------------------------------------------------ facts read from the exe
def load_image():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True)


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def c_asserts():
    out = set()
    for f in ("NgEngine/Zge/zquat.c", "NgEngine/Zge/zanim.c"):
        t = (SRC / f).read_text(encoding="utf-8")
        path = re.search(r'#define \w+_FILE "([^"]+)"', t).group(1).replace("\\\\", "\\")
        for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", \w+_FILE, (0x[0-9A-Fa-f]+), 0\)', t):
            out.add((path, m.group(1), int(m.group(2), 16)))
    return out


def test_assert_sites_exist_in_exe():
    img = load_image().get_memory_mapped_image()
    sites = c_asserts()
    assert len(sites) == 15
    for path, msg, line in sorted(sites):
        p_addr = img.find(path.encode() + b"\0") + 0x400000
        assert p_addr > 0x400000, path
        addrs = [m.start() + 0x400000 for m in re.finditer(re.escape(msg.encode() + b"\0"), img)]
        assert addrs, msg
        pats = [push(line) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", a) for a in addrs]
        assert any(p in img for p in pats), (path, msg, hex(line))


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def test_tables_and_constants_match_exe():
    """The sine table is computed at run time (zmathInitTables), so the exe's .bss holds no data to
    compare; what the exe does hold is checked: the table addresses and bounds in the code, the
    float constants, and that the table area is uninitialised (no file bytes)."""
    pe = load_image()
    img = pe.get_memory_mapped_image()
    rd = lambda va, fmt: struct.unpack_from(fmt, img, va - 0x400000)[0]
    # zmathInitTables: cos = sin + 2048 floats; asin/atan centres = start + 4096 int16; loop ends
    assert rd(0x00480D21, "<H") == 0x05C7 and rd(0x00480D23, "<I") == 0x006682BC and rd(0x00480D27, "<I") == 0x0065E2BC + 2048 * 4
    assert rd(0x00480D2E, "<I") == 0x0065E2BC and rd(0x00480D49, "<I") == 0x0065E2BC + 10240 * 4
    assert rd(0x00480D5E, "<I") == 0x0066C2C4 and rd(0x00480D62, "<I") == 0x006682C0 + 4096 * 2
    assert rd(0x00480D9A, "<I") == 0x006682C0 + 8193 * 2
    assert rd(0x00480DA2, "<I") == 0x006702CC and rd(0x00480DA6, "<I") == 0x0066C2C8 + 4096 * 2
    assert rd(0x00480DDD, "<I") == 0x0066C2C8 + 8193 * 2
    # float constants named in the C
    assert rd(0x0054DCC4, "<f") == f32(2 * math.pi / 65536) == 9.58738019107841e-05
    assert rd(0x0054DCC8, "<f") == f32(65536 / (2 * math.pi)) == 10430.3779296875
    assert rd(0x0054DCD8, "<f") == -4096.0 and rd(0x0054DCD0, "<d") == 1 / 4096
    assert rd(0x0054DCE0, "<f") == f32(0.9999) and rd(0x0054DCDC, "<f") == f32(99999.99)
    assert rd(0x0054D15C, "<f") == 360 / 65536 and rd(0x0054D01C, "<f") == f32(65536 / 360)
    assert rd(0x0054E100, "<f") == 32400.0 and rd(0x0054D118, "<f") == 10000.0
    assert rd(0x00481F9F, "<I") == 0x87654321                       # zmathRandom
    # the tables live in .bss: no raw data behind them
    for s in pe.sections:
        lo = pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress
        if lo <= 0x0065E2BC < lo + s.Misc_VirtualSize:
            assert 0x0065E2BC >= lo + s.SizeOfRawData, (s.Name, hex(lo), hex(s.SizeOfRawData))
            break
    else:
        raise AssertionError("no section holds the sine table")


# ------------------------------------------------------------------ ports
K = 9.58738019107841e-05     # 0x0054DCC4
U = 10430.3779296875         # 0x0054DCC8


def s16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


def trunc(x):
    return int(x)            # __ftol (round toward zero)


class ZmathPort:
    """zmathInitTables + the table readers (zmath.c)."""

    def __init__(self):
        self.sin_tab = [f32(math.sin(s16(i * 8) * K)) for i in range(10240)]
        self.asin_tab = [s16(trunc(math.asin((i - 4096) / 4096.0) * U)) for i in range(8193)]
        self.atan_tab = [s16(trunc(math.atan((i - 4096) / 4096.0) * U)) for i in range(8193)]

    def sin(self, a):
        return self.sin_tab[(a & 0xFFFF) >> 3]

    def cos(self, a):
        return self.sin_tab[2048 + ((a & 0xFFFF) >> 3)]

    def _inv(self, tab, x):
        return tab[4096 - trunc(x * -4096.0)]

    def acos(self, x):
        x = 1.0 if x >= 1.0 else -1.0 if x <= -1.0 else x
        return s16(0x3FFF - self._inv(self.asin_tab, x))

    def atan(self, x):
        if x > 1.0:
            return s16(0x4000 - self._inv(self.atan_tab, 1.0 / x))
        if x < -1.0:
            return s16(-0x4000 - self._inv(self.atan_tab, 1.0 / x))
        return self._inv(self.atan_tab, x)

    def atan2(self, y, x):
        neg, base = 0, 0
        if y < 0:
            y, neg = -y, -1
        if x < 0:
            x, base, neg = -x, 0x8000, neg + 1
        a = 0x4000 - self.atan(x / y) if y > x else self.atan(y / x)
        a += base
        return s16(-a if neg else a)

    # -- zquat.c
    def quat_from_euler(self, e):
        hx = e[0] * K * 0.5                     # stays on the FPU stack
        hy = f32(e[1] * K * 0.5)                # stored as floats
        hz = f32(e[2] * K * 0.5)
        ax, ay, az = (s16(trunc(h * U)) for h in (hx, hy, hz))
        cx, cy, cz = self.cos(ax), self.cos(ay), self.cos(az)
        sx, sy, sz = self.sin(ax), self.sin(ay), self.sin(az)
        return (sx * cz * cy - cx * sz * sy, cx * cz * sy + sx * sz * cy,
                cx * sz * cy - sx * cz * sy, sx * sz * sy + cx * cz * cy)

    def matrix_to_euler(self, m):
        """zmathMatrixToEuler; m = (right, up, at) rows."""
        (r0, r1, r2), (u0, u1, u2), (a0, a1, a2) = m
        cy = math.sqrt(r0 * r0 + r1 * r1)
        if cy > 0.001:
            return [self.atan2(u2, a2), self.atan2(-r2, cy), self.atan2(r1, r0)]
        return [self.atan2(-a1, u1), self.atan2(-r2, cy), 0]

    def slerp(self, a, b, t):
        d = a[0] * b[0] + a[2] * b[2] + a[1] * b[1] + b[3] * a[3]
        flip = d < 0
        d = abs(d)
        if 1.0 - d < 1e-5:
            s0, s1 = 1.0 - t, t
        else:
            th = self.acos(d) * K
            st = self.sin(trunc(th * U))
            s0 = self.sin(trunc((th - t * th) * U)) / st
            s1 = self.sin(trunc(t * th * U)) / st
        if flip:
            s1 = -s1
        return tuple(s0 * x + s1 * y for x, y in zip(a, b))


def quat_mul(a, b):
    """zquatMul (Hamilton a * b)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (bw * ax + bx * aw + bz * ay - by * az, by * aw - bz * ax + ay * bw + az * bx,
            bz * aw + by * ax - ay * bx + az * bw, bw * aw - ax * bx - by * ay - bz * az)


def quat_to_matrix(q):
    """zquatToMatrix: rows (right, up, at); normalises, then 2 / |q|."""
    n = math.sqrt(sum(v * v for v in q))
    x, y, z, w = (v / n for v in q)
    s = 2.0 / n if n > 0 else 0.0
    xs, ys, zs = x * s, y * s, z * s
    wx, wy, wz, xx, xy, xz, yy, yz, zz = w * xs, w * ys, w * zs, x * xs, x * ys, x * zs, y * ys, y * zs, z * zs
    return ((1 - (zz + yy), xy + wz, xz - wy), (xy - wz, 1 - (zz + xx), yz + wx), (xz + wy, yz - wx, 1 - (yy + xx)))


def quat_to_rw_matrix(q):
    """zrwModelQuatToRwMatrix 0x00487E70 (zrwModel.c): the node quaternion (x, -y, -z, w)."""
    return quat_to_matrix((q[0], -q[1], -q[2], q[3]))


@pytest.fixture(scope="module")
def zm():
    return ZmathPort()


def test_sine_table_port(zm):
    """zmathInitTables' sine table against libm: within float rounding, the int16-wrapped angles of
    the second half turn included, and cos = sin + a quarter turn."""
    for i in range(0, 10240, 7):
        assert abs(zm.sin_tab[i] - math.sin(i * 2 * math.pi / 8192)) < 2e-7, i
    for a in range(0, 65536, 97):
        assert abs(zm.cos(a) - math.cos((a & ~7) * 2 * math.pi / 65536)) < 2e-7, a
    assert zm.asin_tab[4096] == 0 and zm.asin_tab[8192] == 0x3FFF and zm.asin_tab[0] == -0x3FFF
    assert zm.atan_tab[8192] == 0x1FFF and zm.atan_tab[0] == -0x1FFF


def test_inverse_trig_ports(zm):
    import random
    rnd = random.Random(45)
    step = 2 * math.pi / 65536
    for _ in range(3000):
        y, x = rnd.uniform(-5, 5), rnd.uniform(-5, 5)
        a = zm.atan2(y, x)
        assert abs(s16(a - round(math.atan2(y, x) / step))) <= 4, (y, x, a)
    for v in (-1.0, -0.5, 0.0, 0.3, 1.0):
        assert abs(zm.acos(v) - math.acos(v) / step) <= 3


def test_euler_matrix_round_trip(zm):
    """zmathMatrixToEuler inverts zquatFromEuler + zquatToMatrix (R = Rz Ry Rx) away from gimbal lock."""
    import random
    rnd = random.Random(7)
    for _ in range(500):
        e = [rnd.randrange(-32768, 32768), rnd.randrange(-14000, 14000), rnd.randrange(-32768, 32768)]
        e2 = zm.matrix_to_euler(quat_to_matrix(zm.quat_from_euler(e)))
        assert all(abs(s16(a - b)) <= 40 for a, b in zip(e, e2)), (e, e2)


def test_slerp_port(zm):
    a, b = zm.quat_from_euler((0, 0, 0)), zm.quat_from_euler((0, 0x2000, 0))
    for t in (0.0, 0.25, 0.5, 1.0):
        q = zm.slerp(a, b, t)
        want = zm.quat_from_euler((0, round(0x2000 * t), 0))
        assert max(abs(x - y) for x, y in zip(q, want)) < 3e-3, (t, q, want)
    q = zm.slerp(a, tuple(-v for v in b), 0.5)      # the short way round
    assert q[3] > 0.9


# ------------------------------------------------------------------ the motion corpus
@pytest.fixture(scope="module")
def bank():
    mz = GAME_DIR / "assets" / "motion.zal"
    if not mz.is_file():
        pytest.skip("motion.zal not found")
    from dmbmx.formats import motion
    return motion.read_bank(mz.read_bytes())


def test_every_motion_frame_matches_reference(zm, bank):
    """For every clip, frame and channel (DM-016's decoder port Motion.sample), the C chain
    zquatFromEuler (+ zquatMul with NgIFzactor's arm table) equals dmbmx's exact-maths rotations
    to the quantisation of the tables (half angles truncated and stepped by 8 units)."""
    from dmbmx.formats import motion as M
    zp, zn = zm.quat_from_euler((0, 0, 0x4000)), zm.quat_from_euler((0, 0, -0x4000))   # Cz(+90), Cz(-90)
    fix = {"LeftUpArm": (None, zp), "LeftLowArm": (zn, zp), "LeftHand": (zn, zp),
           "RightUpArm": (None, zn), "RightLowArm": (zp, zn), "RightHand": (zp, zn)}
    worst = n = clips = 0
    for mid in bank.ids:
        clips += 1
        for pose in bank.motion(mid).sample():
            for names, eulers, arm in ((M.RIDER_CHANNELS, pose.rider, True), (M.BIKE_CHANNELS, pose.bike, False)):
                for name, e in zip(names, eulers or ()):
                    q = zm.quat_from_euler(e)
                    if arm and name in fix:
                        pre, post = fix[name]
                        if pre is not None:
                            q = quat_mul(pre, q)
                        q = quat_mul(q, post)
                        ref = M.channel_rotation(name, e, M.ARM_FIX_AS_DECOMPILED, frf=False)  # engine axes
                    else:
                        ref = M.euler_to_quat(e)
                    d = max(abs(a - b) for a, b in zip(q, ref))
                    worst = max(worst, d)
                    n += 1
                    assert d < 6e-3, (mid, name, e, q, ref)
    assert clips == 238 and n == 43895      # 238 clips, every frame, rider + bike channels
    assert worst > 1e-4          # the tables really are coarser than libm
    print(f"zquatFromEuler port vs motion.py: worst component difference {worst:.2e} over {n} channel frames")


def test_node_rotation_reaches_renderware_through_F():
    """zrwModelUpdate builds every animated frame's rotation with zrwModelQuatToRwMatrix, which
    negates y and z of the node quaternion: R_rw = F R(q) F with F = diag(1, -1, -1)."""
    img = load_image().get_memory_mapped_image()
    code = img[0x00487E70 - 0x400000:0x00487E70 - 0x400000 + 0x20]
    assert code[7:10] == b"\xD9\x40\x04" and code[0x0F:0x11] == b"\xD9\xE0"      # fld [q+4]; fchs
    assert code[0x15:0x18] == b"\xD9\x40\x08" and code[0x18:0x1A] == b"\xD9\xE0"  # fld [q+8]; fchs
    body = img[0x00488650 - 0x400000:0x00488885 - 0x400000]
    calls = [i for i in range(len(body) - 4) if body[i] == 0xE8 and
             0x00488650 + i + 5 + struct.unpack_from("<i", body, i + 1)[0] == 0x00487E70]
    assert len(calls) == 2           # the model orientation and every body part (node + 0x18)
    import random
    zmp = ZmathPort()
    rnd = random.Random(3)
    F = ((1, 0, 0), (0, -1, 0), (0, 0, -1))
    mul = lambda a, b: tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))
    for _ in range(50):
        q = zmp.quat_from_euler([rnd.randrange(-32768, 32768) for _ in range(3)])
        a, b = quat_to_rw_matrix(q), mul(mul(F, quat_to_matrix(q)), F)
        assert max(abs(x - y) for ra, rb in zip(a, b) for x, y in zip(ra, rb)) < 1e-6


def test_F_conjugation_explains_the_arm_flip():
    """DM-017 open question: with the game's conversion applied to every animated frame (rider and
    bike), NgIFzactor's arm table *as decompiled* puts the hands on the handlebars; without it the
    same table puts them ~0.7 away, which is what DM-017's empirical Rx(180) compensated."""
    if not (GAME_DIR / "assets" / "DaveMirra1.ZMD").is_file():
        pytest.skip("no corpus")
    sys.path.insert(0, str(REPO / "tools" / "research"))
    import arm_pose_check as A
    from dmbmx.formats import motion as M
    rider, bsk, bars, bank_ = A.setup(GAME_DIR, "DaveMirra1.ZMD", "DMbike01.zmd")
    mids = [8, 5, 36, 22, 57, 106]
    # DM-019 made the F conjugation dmbmx's default; frf=False / legacy=True give the old chains
    plain = A.summarize(rider, bsk, bars, bank_, mids, M.ARM_FIX_AS_DECOMPILED, step=2, frf=False)
    game = A.summarize(rider, bsk, bars, bank_, mids, M.ARM_FIX_AS_DECOMPILED, step=2, frf=True)
    dm017_in_game = A.summarize(rider, bsk, bars, bank_, mids, M.ARM_FIX_DM017, step=2, frf=True)
    # posture sanity in the game convention: head above hips, feet below, every frame of motion 8
    for p in bank_.motion(8).sample():
        P = M.pose_positions(rider, M.rider_rotations(p, M.ARM_FIX_AS_DECOMPILED, frf=True))
        assert P["Head"][1] > P["Hips"][1] + 0.3 and P["LeftFoot"][1] < P["Hips"][1] - 0.3
    assert plain["barend"] > 0.6 and plain["bar"] > 0.6                  # DM-016's reading, no F
    assert game["barend"] < 0.18 and game["bar"] < 0.15                   # the game's chain
    assert game["feet"] < 0.12
    assert dm017_in_game["barend"] > 0.6                                  # the Rx(180) is the F, not extra


# ------------------------------------------------------------------ zanim.c ports (synthetic data)
def chunk(tag, payload):
    return tag + struct.pack("<I", len(payload)) + payload


def zhash_port(name):
    from dmbmx.core.hash import zhash
    return zhash(name)


class ZanimPort:
    """zanimChunkDEFI/AROT/ATRA (handlers of zanimLoadIff) and zanimPlay + zanimEvaluate."""

    def __init__(self, zm):
        self.zm = zm
        self.id = None
        self.names = []           # newest first, as the C links them
        self.rot, self.trans = [], []

    def chunk_defi(self, data):
        assert (len(data) - 4) % 36 == 0, "DEFI size mismatch"
        self.id = struct.unpack_from("<I", data, 0)[0]
        self.names.insert(0, [(data[4 + 36 * i:4 + 36 * i + 32].split(b"\0")[0].decode(),
                               struct.unpack_from("<I", data, 4 + 36 * i + 32)[0]) for i in range((len(data) - 4) // 36)])

    def _chan(self, data, stride, nval):
        idx = struct.unpack_from("<i", data, 0)[0]
        keys = [struct.unpack_from("<%df" % (1 + nval), data, 4 + stride * i) for i in range((len(data) - 4) // stride)]
        return {"hash": self.names[0][idx][1], "keys": keys}

    def chunk_arot(self, data):
        self.rot.insert(0, self._chan(data, 20, 4))

    def chunk_atra(self, data):
        self.trans.insert(0, self._chan(data, 16, 3))

    @staticmethod
    def weight(tr, t):
        if t < tr["fadeInEnd"]:
            w = (t - tr["start"]) / (tr["fadeInEnd"] - tr["start"])
        elif t < tr["fadeOutStart"]:
            w = 1.0
        elif t < tr["fadeOutEnd"]:
            w = 1.0 - (t - tr["fadeOutStart"]) / (tr["fadeOutEnd"] - tr["fadeOutStart"])
        else:
            w = 0.0
        return w * tr["weight"]

    @staticmethod
    def segment(ch, k, lt):
        keys = ch["keys"]
        if lt < keys[k][0]:
            while k > 0 and not lt > keys[k][0]:
                k += 1
            return k
        while k < len(keys) - 1 and not lt < keys[k][0]:
            k += 1
        return k - 1

    def play(self, hashes, t):
        """zanimPlay on a node table with these (sorted) hashes."""
        nodes = []
        for h in hashes:
            r = next((c for c in self.rot if c["hash"] == h), None)
            m = next((c for c in self.trans if c["hash"] == h), None)
            tr = None
            if r or m:
                tr = {"start": t, "end": t + (r or m)["keys"][-1][0], "rot": r, "trans": m, "rotKey": 0,
                      "weight": 1.0, "fadeInEnd": t}
                tr["expire"] = tr["fadeOutStart"] = tr["end"] + 10000.0
                tr["fadeOutEnd"] = tr["fadeOutStart"] + 1.0
            nodes.append({"hash": h, "tracks": [tr] if tr else [], "quat": (0.0, 0.0, 0.0, 1.0)})
        return nodes

    def evaluate(self, nodes, t):
        for node in nodes:
            node["tracks"] = [tr for tr in node["tracks"] if not t > tr["expire"]]
            acc, flag = None, False
            for tr in node["tracks"]:
                if not tr.get("rot"):
                    continue
                w = self.weight(tr, t)
                if w < 1e-5:
                    continue
                if not flag:
                    acc, flag = (0.0, 0.0, 0.0, 0.0), True
                lt = min(t, tr["end"]) - tr["start"]
                k = tr["rotKey"] = self.segment(tr["rot"], tr["rotKey"], lt)
                keys = tr["rot"]["keys"]
                u = (lt - keys[k][0]) / (keys[k + 1][0] - keys[k][0])
                q = tuple(w * v for v in self.zm.slerp(keys[k][1:], keys[k + 1][1:], u))
                d = sum(a * b for a, b in zip(q, acc))
                sub = d <= 0 and sum(v * v for v in acc) >= 1e-5 and sum(v * v for v in q) >= 1e-5
                acc = tuple(a - b if sub else a + b for a, b in zip(acc, q))
            if flag:
                n = math.sqrt(sum(v * v for v in acc))
                node["quat"] = tuple(v / n for v in acc)


def test_zanim_iff_and_blender_port(zm):
    """A synthetic ANIM form (no NAD/IFF animation ships with the game: Scene.Txt's MainBank points
    at motion.zal, whose payload is the zactor motion bank, not IFF) parsed by the chunk-handler
    ports and played back with the zanimPlay + zanimEvaluate ports: keys are hit exactly, between
    keys the pose is the table slerp, after the end the last key is held."""
    names = ["Hips", "Chest"]
    defi = struct.pack("<I", 7) + b"".join(n.encode().ljust(32, b"\0") + struct.pack("<I", zhash_port(n)) for n in names)
    qa, qb, qc = zm.quat_from_euler((0, 0, 0)), zm.quat_from_euler((0, 0x2000, 0)), zm.quat_from_euler((0x1000, 0x2000, 0))
    arot = struct.pack("<i", 1) + b"".join(struct.pack("<5f", t, *q) for t, q in ((0, qa), (100, qb), (200, qc)))
    atra = struct.pack("<i", 0) + struct.pack("<4f", 0, 1, 2, 3) + struct.pack("<4f", 50, 2, 4, 6)
    form = chunk(b"FORM", b"ANIM" + chunk(b"DEFI", defi) + chunk(b"AROT", arot) + chunk(b"ATRA", atra))
    zp = ZanimPort(zm)
    p = 12
    while p < len(form):
        tag, size = form[p:p + 4], struct.unpack_from("<I", form, p + 4)[0]
        {b"DEFI": zp.chunk_defi, b"AROT": zp.chunk_arot, b"ATRA": zp.chunk_atra}[tag](form[p + 8:p + 8 + size])
        p += 8 + size
    assert zp.id == 7 and [n for n, _h in zp.names[0]] == names
    assert zp.rot[0]["hash"] == zhash_port("Chest") and zp.trans[0]["hash"] == zhash_port("Hips")
    assert len(zp.rot[0]["keys"]) == 3 and len(zp.trans[0]["keys"]) == 2
    hashes = sorted(h for _n, h in zp.names[0])
    nodes = zp.play(hashes, 1000.0)
    chest = next(n for n in nodes if n["hash"] == zhash_port("Chest"))
    hips = next(n for n in nodes if n["hash"] == zhash_port("Hips"))
    assert len(hips["tracks"]) == 1 and hips["tracks"][0]["rot"] is None      # translation-only track
    assert hips["tracks"][0]["end"] == 1050.0 and chest["tracks"][0]["end"] == 1200.0
    close = lambda a, b: max(abs(x - y) for x, y in zip(a, b)) < 2e-3
    for t, want in ((1000.0, qa), (1050.0, zm.slerp(qa, qb, 0.5)), (1100.0, qb), (1150.0, zm.slerp(qb, qc, 0.5)),
                    (1199.0, zm.slerp(qb, qc, 0.99)), (5000.0, zm.slerp(qb, qc, 1.0))):
        zp.evaluate(nodes, t)
        n = math.sqrt(sum(v * v for v in want))
        assert close(chest["quat"], tuple(v / n for v in want)), (t, chest["quat"], want)
    zp.evaluate(nodes, 1200.0 + 10000.0 + 0.5)     # expired: the track is dropped, the pose kept
    assert not chest["tracks"]

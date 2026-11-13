"""Self-consistency checks for the decomp of physics.c (rider physics) and what it reads from the levels.

1. structure: all 50 physics.c functions drafted (VA order, names = functions.csv = symbols.csv),
   no Ghidra artefacts, struct layouts (physics.h, the physics fields of rider.h) agree with their
   "+0x.." comments and ghidra_types.txt, the 15 functions found here are table / thunk targets;
2. against the exe: every assert (message, file, line) and debug message is a call site, the
   constants and the six state tables the C names equal the exe's;
3. behaviour: the Python port (dmbmx.formats.physics) against hand-computed cases, and the
   per-sector collision trees of every BSP._sd (dmbmx.formats.rwcoll).
"""
import csv
import math
import re
import struct
from pathlib import Path

import pytest

from conftest import corpus_files
from dmbmx.formats import physics as P
from dmbmx.formats import rwcoll, rwworld

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
PHYS = "zaxis/Bmx/Src/com/action/physics.c"
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")
NEW_FUNCS = {0x004203A0, 0x00420F00, 0x004210C0, 0x00421190, 0x00422830, 0x00422E50, 0x004243B0,
             0x00424A10, 0x00425610, 0x00425740, 0x00425760, 0x00425910, 0x00425940, 0x00428DF0,
             0x0042BA50}
B = 0x400000


def text():
    return (SRC / PHYS).read_text(encoding="utf-8")


def c_functions():
    out = []
    for m in VA_RE.finditer(text()):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == kind:
                out[int(r["address"], 16) if kind != "struct" else r["name"]] = r
    return out


# ------------------------------------------------------------------ structure
def test_every_function_drafted_and_named_consistently():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["file"] == PHYS]
    assert len(rows) == 50
    cf = c_functions()
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    syms = symbols("func")
    by_va = {int(r["va"], 16): r for r in rows}
    for va, size, status, name in cf:
        r = by_va[va]
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, hex(va)
        assert name.startswith("bmxphys"), name
    assert sum(size for _, size, _, _ in cf) == 45590
    # the skeleton's seed name moved to the real function; the gap functions are physics.c's
    assert syms[0x00428DF0]["name"] == "bmxphysUpdatePositionAir"
    assert syms[0x0042DAD0]["name"] == "bmxphysSetSurfaceFromHit"
    assert not (SRC / "_unassigned" / "gap_overlay__physics.c").exists()


def test_no_ghidra_artefacts_and_old_names_gone():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_0041fa[24]0|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|physics_[0-9a-f]{8})\b")
    t = text()
    assert "TODO(decomp)" not in t
    for n, line in enumerate(t.splitlines(), 1):
        assert not bad.search(line), f"{n}: {line.strip()}"
    for p in SRC.rglob("*.c"):
        code = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        assert not re.search(r"\bphysics_[0-9a-f]{8}\b", code), p


def test_new_functions_are_table_or_thunk_targets(game_dir):
    img = load_image(game_dir)
    tables = set(struct.unpack_from("<42I", img, 0x00557898 - B))
    thunks = set()
    va = 0x00401005
    while img[va - B] == 0xE9:
        thunks.add(va + 5 + struct.unpack_from("<i", img, va - B + 1)[0])
        va += 5
    for f in NEW_FUNCS:
        assert f in tables or f in thunks, hex(f)


def layouts():
    import test_decomp_game as T
    return T.layouts("rider.h", "physics.h")


def test_struct_layouts():
    lay = layouts()
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    for name in ("BmxWheel", "NgColFace", "NgColEdge", "NgColHit", "BmxProbe", "BmxRider"):
        size, fields, _ = lay[name]
        assert size == want[name], (name, hex(size))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert types.index("rider.h") < types.index("physics.h")
    t = (INC / "physics.h").read_text(encoding="utf-8")
    for n in ("NgColFace", "NgColEdge", "NgColHit", "BmxProbe"):
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n
        assert symbols("struct")[n]["type"] == "decomp/include/physics.h"


def test_rider_physics_fields():
    off = {n: o for n, o, _ in layouts()["BmxRider"][1]}
    expect = {"physState": 0x3DC, "surface": 0x3DD, "physTimer": 0x3DE, "matrix": 0x3E0, "pos": 0x420,
              "physHipOffset": 0x430, "wallHeight": 0x440, "grindBalance": 0x444, "vel": 0x448,
              "physSlopeAccel": 0x458, "physLockStep": 0x468, "rotVel": 0x478, "airLean": 0x480,
              "frameDist": 0x484, "surfaceObj": 0x488, "physSurfaceCopy": 0x48C, "physLockPos": 0x680,
              "physLockMatrix": 0x690, "stats": 0x6D0, "physAirMatrix": 0x6EC, "physAirRot": 0x72C, "motion": 0xB9C, "modBlend": 0xBBC,
              "trickMatrix": 0x734, "flipVel": 0x774, "contact": 0x77C, "rootMotion": 0xBFC,
              "spawnPos": 0xC10, "bikeMatrix": 0xC20, "pedalForce": 0xC60, "wheelDrag": 0xC64,
              "tuneC68": 0xC68, "suspension": 0xC74, "wheel": 0xC88, "bikeActor": 0xD90, "ik": 0xD98}
    for n, o in expect.items():
        assert off[n] == o, (n, hex(off[n]), hex(o))
    w = {n: o for n, o, _ in layouts()["BmxWheel"][1]}
    assert (w["radius"], w["vel"], w["speed"], w["frame"], w["contact"], w["angle"], w["spin"], w["travel"],
            w["material"]) == (0x10, 0x14, 0x24, 0x28, 0x68, 0x6A, 0x6C, 0x70, 0x80)
    # the surface copy holds the largest surface record (a 500-byte prop group)
    assert off["physLockPos"] - off["physSurfaceCopy"] == 500
    assert off["wheel"] + 2 * 0x84 == off["bikeActor"]


# ------------------------------------------------------------------ against the exe
def load_image(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def test_assert_sites_exist_in_exe(game_dir):
    img = load_image(game_dir)
    t = text()
    path = re.search(r'#define PHYSICS_SRC "([^"]+)"', t).group(1).replace("\\\\", "\\")
    sites = re.findall(r'zAssertFail\("([^"]+)", PHYSICS_SRC, (0x[0-9a-f]+)', t)
    assert len(sites) == 7
    p_addr = img.find(path.encode() + b"\0") + B
    assert p_addr == 0x005579A4
    for msg, line in sites:
        m_addr = img.find(msg.encode() + b"\0") + B
        assert m_addr > B, msg
        pat = push(int(line, 16)) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", m_addr)
        assert pat in img, (msg, line)


def test_debug_messages_exist_in_exe(game_dir):
    img = load_image(game_dir)
    msgs = re.findall(r'g_pfnDebugPrintf\("((?:[^"\\]|\\.)*)"', text())
    assert len(msgs) >= 8
    for m in msgs:
        raw = m.replace("\\n", "\n").encode()
        a = img.find(raw + b"\0")
        assert a > 0, m
        assert struct.pack("<I", a + B) in img, m          # referenced (pushed) somewhere


def test_constants_match_exe(game_dir):
    img = load_image(game_dir)
    got = P.read_exe_constants(img)
    for name, (_, _, want) in P.CONSTANTS.items():
        g = got[name]
        if isinstance(want, tuple):
            assert all(math.isclose(a, b, rel_tol=1e-6) for a, b in zip(g, want)), name
        else:
            assert math.isclose(g, want, rel_tol=1e-6), name
    # the C's named constants carry the same values and VAs
    t = text()
    n = 0
    for m in re.finditer(r"static const (float|int16_t|int32_t) (g_bmxphys\w+)\s*=\s*([-0-9.xA-Fa-f]+)f?;\s*/\* (0x[0-9A-F]{8})", t):
        typ, name, val, va = m.groups()
        fmt = {"float": "<f", "int16_t": "<h", "int32_t": "<i"}[typ]
        exe = struct.unpack_from(fmt, img, int(va, 16) - B)[0]
        assert math.isclose(exe, float(int(val, 0)) if typ != "float" else float(val.rstrip("f")), rel_tol=1e-6), name
        n += 1
    assert n >= 25


def test_state_tables_match_exe(game_dir):
    img = load_image(game_dir)
    t = text()
    funcs = {name: va for va, _, _, name in c_functions()}
    for m in re.finditer(r"const BmxPhys\w+ (g_bmxphys\w+)\[BMXPHYS_STATES\] = \{\s*/\* (0x[0-9A-F]{8}) \*/(.*?)\};", t, re.S):
        name, va, body = m.groups()
        entries = [e.strip() for e in body.replace("\n", " ").split(",")]
        assert len(entries) == 7, name
        exe = struct.unpack_from("<7I", img, int(va, 16) - B)
        assert [0 if e == "0" else funcs[e] for e in entries] == list(exe), name


def test_tag_flag_remap_matches_exe(game_dir):
    """zcolSegmentTest remaps the TAGI flags byte; the port's bit map matches the code bytes."""
    img = load_image(game_dir)
    assert [P.face_flags(1 << b) for b in range(4)] == [0x10, 0x08, 0x20, 0x40]
    assert P.face_flags(0xF) == 0x78
    # the same shift/or sequence is in the exe at least twice (NgIFzbsp 0x00468070, NgIFzcollide 0x00469B00)
    assert img.count(bytes.fromhex("83e00c")) >= 2


# ------------------------------------------------------------------ behaviour: the Python port
def test_air_step_hand_case():
    v, dist = P.air_step((3.0, -4.0, 0.0))
    assert math.isclose(dist, math.hypot(3.0, -4.0 + 1.58), rel_tol=1e-9)
    assert all(math.isclose(a, b) for a, b in zip(v, (3.0 * 0.999, -2.42 * 0.999, 0.0)))
    # a ballistic jump from rest at -20/frame peaks after ~13 frames (20 / 1.58 = 12.7)
    v, frames = (0.0, -20.0, 0.0), 0
    while v[1] < 0:
        v, _ = P.air_step(v)
        frames += 1
    assert frames == 13


def test_grind_step_hand_case():
    # 45 deg down-slope edge (y down), rider facing along it, stat 5 = 1 (best): friction 0.85 %
    d = (math.sqrt(0.5), math.sqrt(0.5), 0.0)
    v, acc, dist = P.grind_step((10.0, 0.0, 0.0), d, (1.0, 0.0, 0.0), 1.0)
    g = 1.58 * math.sqrt(0.5)
    assert math.isclose(acc[0], g * d[0]) and math.isclose(acc[1], g * d[1])
    assert math.isclose(v[0], (10.0 + 0.79) * (1 - 0.0085), rel_tol=1e-9)
    # facing the other way the edge direction flips, gravity still pulls downhill (+y)
    _, acc2, _ = P.grind_step((0.0, 0.0, 0.0), d, (-1.0, 0.0, 0.0), 1.0)
    assert acc2[1] > 0 and math.isclose(acc2[1], acc[1])
    # stat 5 = 0: friction x 11
    v0, _, _ = P.grind_step((10.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 0.0, 0.0), 0.0)
    assert math.isclose(v0[0], 10.0 * (1 - 11 * 0.0085))


def test_takeoff_caps():
    assert [P.takeoff_cap(s, 0x2000) for s in (0, 4, 5, 6, 7, 8)] == [16.0, 11.0, 15.0, 21.0, 23.0, 35.0]
    assert math.isclose(P.takeoff_cap(8, 0), 35.0)                 # exactly 45 deg: sin 0
    assert math.isclose(P.takeoff_cap(0, 0x1000), 16.0 * (1 + math.sin(math.pi / 8)))


def test_lockon_frames_and_flight():
    # horizontal approach at 10/frame, 100 away: 10 frames; clamped to [6, max]
    assert P.lockon_frames((10.0, 0.0, 0.0), (100.0, 0.0, 0.0), 24) == 10
    assert P.lockon_frames((10.0, 0.0, 0.0), (30.0, 0.0, 0.0), 24) == 6
    assert P.lockon_frames((10.0, 0.0, 0.0), (1000.0, 0.0, 0.0), 12) == 12
    # falling (vy=15 > horiz): frames from the parabola, (-vy + sqrt(vy^2 + 2 dy g)) / g
    f = P.lockon_frames((1.0, 15.0, 0.0), (5.0, 200.0, 0.0), 24)
    assert f == int((math.sqrt(15 * 15 + 2 * 200 * 1.58) - 15.0) / 1.58)
    # the flight covers (frames - 1) / frames of the way; the state change snaps the rest
    for delta, frames in (((120.0, -80.0, 40.0), 12), ((300.0, 50.0, -30.0), 24), ((10.0, 0.0, 0.0), 6)):
        reached = P.simulate_lockon(delta, frames)
        for a, d in zip(reached, delta):
            assert math.isclose(a, d * (frames - 1) / frames, abs_tol=1e-9)


def test_small_helpers():
    assert P.wheel_spin(2 * math.pi * 25) in (65535, -1, 0, 65536 - 65536)   # one turn per frame wraps
    assert P.wheel_spin(10.0) == int(10.0 * 0.006366203 * 65536) and P.wheel_spin(10.0, True) == -P.wheel_spin(10.0)
    assert P.edge_plane_distance((10.9, -3.7, 2.2), (1.0, 0.0, 0.0, -5.0)) == 5.0       # 10 - 5 (truncation)
    assert P.closest_point_on_segment((0, 0, 0), (100, 0, 0), (-5, 3, 0)) == (0.0, 0.0, 0.0)
    assert P.closest_point_on_segment((0, 0, 0), (100, 0, 0), (150, 3, 0)) == (100.0, 0.0, 0.0)
    c = P.closest_point_on_segment((0, 0, 0), (100, 0, 0), (40, 7, 0))
    assert abs(c[0] - 40.0) < 0.02 and c[1] == 0.0
    assert P.tag_class(0x1BB8) == 1 and P.tag_id(0x1BB8) == 0xBB8
    assert set(P.TAKEOFF_CAP) == {4, 5, 6, 7, 8} and set(P.FACE_CLASSES) == set(range(11))


# ------------------------------------------------------------------ the 384-byte collision block
def test_rwcoll_decode_synthetic():
    blk = bytearray(rwcoll.COLL_SIZE)
    blk[3:6] = bytes([0x80 | (2 << 5) | 1, 0, 0])      # root: split on z at vertex 0, owns triangle 0
    blk[6:9] = bytes([2, 0, 1])                        # node 2: leaf, triangles 1..2
    blk[9:12] = bytes([1, 0, 3])                       # node 3: leaf, triangle 3
    nodes = rwcoll.decode(bytes(blk))
    assert [(n.index, n.split, n.axis, n.start, n.count) for n in nodes] == \
        [(1, True, 2, 0, 1), (2, False, 0, 1, 2), (3, False, 0, 3, 1)]
    with pytest.raises(rwcoll.CollError):
        rwcoll.decode(bytes(10))
    deep = bytearray(rwcoll.COLL_SIZE)
    for i in range(1, 128):
        deep[3 * i] = 0x80                             # every node splits: children past 127
    with pytest.raises(rwcoll.CollError):
        rwcoll.decode(bytes(deep))


def test_rwcoll_corpus_every_sector_consistent():
    files = corpus_files("._sd")
    bsp = [f for f in files if f.name.lower() == "bsp._sd"]
    if not bsp:
        pytest.skip("no corpus")
    assert len(bsp) == 12
    sectors = 0
    for f in bsp:
        w = rwworld.read(f.read_bytes())
        for s in w.sectors():
            if not s.coll_present:
                continue
            sectors += 1
            assert rwcoll.check(s) == [], (f.parent.name, rwcoll.check(s)[:3])
            assert s.coll[:3] == b"\0\0\0"            # node 0 unused
    assert sectors == 784


def test_rwcoll_detects_stale_tree_after_geometry_edit():
    files = [f for f in corpus_files("._sd") if f.name.lower() == "bsp._sd"]
    if not files:
        pytest.skip("no corpus")
    w = rwworld.read(min(files, key=lambda p: p.stat().st_size).read_bytes())
    s = next(s for s in w.sectors() if s.coll_present and len(s.triangles) > 8)
    s.triangles = s.triangles + [s.triangles[0]]       # one more triangle, tree not rebuilt
    assert any("referenced" in p for p in rwcoll.check(s))

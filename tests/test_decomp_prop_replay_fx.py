"""Self-consistency checks for the decomp of prop.c, replay.c and fx.c (props, replays, effects).

1. structure: the 52 functions are drafted (VA order, names = functions.csv = symbols.csv), the three
   gap files are gone, no Ghidra artefacts or old names, struct layouts (bmxprop.h, replay.h, fx.h)
   agree with their "+0x.." comments, sizeof checks and ghidra_types.txt;
2. against the exe: every assert (message, file, line) is a call site, the debug messages and the zfx
   spawner names exist, the replay buffer constants of bmxreplayCreate / bmxreplayGetSaveArea are the
   ones dmbmx.formats.replay uses, the prop table g_bmxpropTable reads back with the header's types;
3. replay records decoded against the exe's layout: the prop snapshot blocks of the PC records hold
   exactly the slots bmxpropInit binds for that level (table x the level's Prop._sd), with the
   index / links / start state the C code writes; the demo records are PC records that fit the demo
   buffer; PropDef OBJI byte 0 (grind rails) never meets the class the rail builder cannot build.
"""
import csv
import re
import struct
from pathlib import Path

import pytest

from dmbmx.formats import level as L
from dmbmx.formats import replay as R
from dmbmx.formats import zal as Z

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
ACTION = "zaxis/Bmx/Src/com/action/"
FILES = {"fx.c": (21, 3074, "fx"), "prop.c": (18, 5854, "bmxprop"), "replay.c": (13, 3381, "bmxreplay")}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")
B = 0x400000
LEVELS = ("GreenvilleDirt", "GreenvilleStreet", "GreenvilleVert", "WoodwardDirt", "WoodwardStreet",
          "WoodwardVert", "ProTourDirt", "ProTourStreet", "ProTourVert", "AcclaimDirt", "AcclaimStreet",
          "AcclaimVert")


def text(name):
    return (SRC / (ACTION + name)).read_text(encoding="utf-8")


def code_only(t):
    return re.sub(r"/\*.*?\*/", "", t, flags=re.S)


def c_functions(name):
    out = []
    for m in VA_RE.finditer(text(name)):
        fn = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), fn))
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
        rows = list(csv.DictReader(f))
    syms = symbols("func")
    for name, (count, size, prefix) in FILES.items():
        mine = [r for r in rows if r["file"] == ACTION + name]
        cf = c_functions(name)
        assert len(mine) == len(cf) == count, name
        assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in mine)
        by_va = {int(r["va"], 16): r for r in mine}
        for va, sz, status, fn in cf:
            r = by_va[va]
            assert status == "draft" and r["status"] == "draft", hex(va)
            assert int(r["size"]) == sz and r["name"] == fn, (hex(va), r["name"], fn)
            assert syms[va]["name"] == fn, hex(va)
            assert syms[va]["source_file"] == "Bmx/Src/com/action/" + name, hex(va)
            assert fn.startswith(prefix), fn
        assert sum(sz for _, sz, _, _ in cf) == size, name
        assert re.search(r"\* %d functions, %d bytes" % (count, size), text(name)), name
    for gap in ("gap_contrler__fx.c", "gap_fx__game.c", "gap_prop__replay.c"):
        assert not (SRC / "_unassigned" / gap).exists(), gap


def test_no_ghidra_artefacts_and_old_names_gone():
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|PTR_\w+|[iu]Var\d+|pc?Var\d+|"
                     r"extraout_\w+|CONCAT\d+|thunk_\w+|switchD_\w+|FUN_[0-9a-f]{8}|__ftol)\b")
    for name in FILES:
        t = text(name)
        assert "TODO(decomp)" not in t
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{name}:{n}: {line.strip()}"
    old = re.compile(r"\b(prop_[0-9a-f]{8}|replay_[0-9a-f]{8}|fx_[0-9a-f]{8}|replay_SetMode|fxImpactSparks|"
                     r"FUN_(004089[0-9a-f]{2}|00408[a-f][0-9a-f]{2}|0040925[0-9a-f]|0042fce0|00409860))\b")
    for p in SRC.rglob("*.c"):
        assert not old.search(code_only(p.read_text(encoding="utf-8"))), p


SIZES = {"uint8_t": (1, 1), "int8_t": (1, 1), "char": (1, 1), "uint16_t": (2, 2), "int16_t": (2, 2),
         "uint32_t": (4, 4), "int32_t": (4, 4), "int": (4, 4), "float": (4, 4),
         "RwV3d": (0xC, 4), "RwMatrix": (0x40, 4), "RwRGBA": (4, 1)}


def layouts(*headers):
    """{struct: (size, [(field, offset, commented_offset)])} (x86, natural alignment)."""
    known = dict(SIZES)
    out = {}
    for header in headers:
        t = (INC / header).read_text(encoding="utf-8")
        t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
        for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
            off, fields, maxal = 0, [], 1
            for line in m.group(2).splitlines():
                d = re.match(r"\s*(?:const\s+)?(\w+)\s*(\*?)\s*(\w+)((?:\[\w+\])*);\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?",
                             line)
                if not d:
                    continue
                typ, ptr, name, dims, comment = d.groups()
                size, align = (4, 4) if ptr else known[typ]
                count = 1
                for k in re.findall(r"\[(\w+)\]", dims):
                    count *= int(k, 0)
                off = (off + align - 1) // align * align
                fields.append((name, off, int(comment, 16) if comment else None))
                off += size * count
                maxal = max(maxal, align)
            size = (off + maxal - 1) // maxal * maxal
            out[m.group(3)] = (size, fields)
            known[m.group(3)] = (size, maxal)
    return out


STRUCTS = {"bmxprop.h": ("BmxPropDef", "NgPropCol", "BmxProp", "BmxPropSwitch", "BmxPropState"),
           "replay.h": ("BmxReplayDesc", "BmxReplay", "BmxReplayRider"),
           "fx.h": ("FxParticle", "FxDust", "FxShard", "FxSkidCorner", "FxSkidQuad", "FxRipple", "FxPsxEffect",
                    "FxPsxBurst", "FxBodyPoint")}


def test_struct_layouts():
    lay = layouts(*STRUCTS)
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    for h, names in STRUCTS.items():
        for n in names:
            size, fields = lay[n]
            assert size == want[n], (n, hex(size))
            for fname, off, comment in fields:
                if comment is not None:
                    assert off == comment, (n, fname, hex(off), hex(comment))
            assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), (INC / h).read_text(encoding="utf-8")), n
            assert symbols("struct")[n]["type"] == "decomp/include/" + h
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert types.index("zprop.h") < types.index("bmxprop.h") and types.index("save.h") < types.index("replay.h")
    # the offsets other modules rely on
    prop = dict((f, o) for f, o, _ in lay["BmxProp"][1])
    assert (prop["col"], prop["pos"], prop["vel"], prop["matrix"], prop["spin"], prop["toggled"], prop["link"]) == \
        (8, 0xC, 0x1C, 0x2C, 0x6C, 0x78, 0x7C)                       # game.c reads col +2, physics +0x1EC
    assert dict((f, o) for f, o, _ in lay["NgPropCol"][1])["game"] == 0x1EC
    rep = dict((f, o) for f, o, _ in lay["BmxReplay"][1])
    assert (rep["controlConfig"], rep["dataOffset"], rep["stick"]) == (R.SAVE_START, 0x2C, 0x3C)
    assert lay["BmxReplayRider"][0] == R.RIDER_SIZE and lay["BmxPropState"][0] == R.PROPS_SIZE
    # the dmbmx header fields are BmxReplay +0x0C.. in order
    names = [f for f, o, _ in lay["BmxReplay"][1] if o >= R.SAVE_START and f != "stick"]
    assert len(names) == len(R.PC_FIELDS)


# ------------------------------------------------------------------ against the exe
def load_image(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def u32(img, va):
    return struct.unpack_from("<I", img, va - B)[0]


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def assert_site(img, msg, path_va, line):
    m_addr = img.find(msg.encode() + b"\0") + B
    assert m_addr > B, msg
    pat = push(line) + b"\x68" + struct.pack("<I", path_va) + b"\x68" + struct.pack("<I", m_addr)
    assert pat in img, (msg, hex(line))


PATHS = {"fx.c": ("FX_SRC", 0x00554C2C), "prop.c": ("PROP_SRC", 0x00558678), "replay.c": ("REPLAY_SRC", 0x005587E4)}


def test_assert_sites_exist_in_exe(game_dir):
    img = load_image(game_dir)
    n = 0
    for name, (macro, path_va) in PATHS.items():
        t = text(name)
        path = re.search(r'#define %s "([^"]+)"' % macro, t).group(1).replace("\\\\", "\\")
        assert img.find(path.encode() + b"\0") + B == path_va, name
        for msg, line in re.findall(r'zAssertFail\("((?:[^"\\]|\\.)+)", %s, (0x[0-9A-Fa-f]+)' % macro, t):
            assert_site(img, msg.replace("\\n", "\n"), path_va, int(line, 16))
            n += 1
    # replay.c's handle checks (replayGet(handle, line of the range assert, line of the NULL assert))
    pairs = re.findall(r"replayGet\(handle, (0x[0-9A-F]+), (0x[0-9A-F]+)\)", text("replay.c"))
    assert len(pairs) == 7
    for a, b in pairs:
        assert_site(img, "invalid replay handle", 0x005587E4, int(a, 16))
        assert_site(img, "invalid handle passed or replay has been destroyed", 0x005587E4, int(b, 16))
        n += 2
    assert n == 2 + 1 + 1 + 14


def test_debug_messages_and_spawner_names_exist_in_exe(game_dir):
    img = load_image(game_dir)
    n = 0
    for name in FILES:
        for m in re.findall(r'g_pfnDebugPrintf\(\s*"((?:[^"\\]|\\.)*)"', text(name)):
            a = img.find(m.replace("\\n", "\n").encode() + b"\0")
            assert a > 0 and struct.pack("<I", a + B) in img, m
            n += 1
    assert n == 1 + 4 + 2
    # every spawner fx.c calls is pushed inside that function's code
    rows = {r["name"]: r for r in csv.DictReader(open(REPO / "decomp" / "functions.csv", encoding="utf-8"))}
    calls = re.findall(r"^\w[^\n]*?(fx\w+)\([^;{]*\)\n\{(.*?)\n\}", text("fx.c"), re.S | re.M)
    seen = set()
    for fn, body in calls:
        for spawner in re.findall(r'zfxCallSpawner\("(\w+)"', body):
            va, size = int(rows[fn]["va"], 16), int(rows[fn]["size"])
            s_va = img.find(spawner.encode() + b"\0", 0x154000) + B
            assert (b"\x68" + struct.pack("<I", s_va)) in img[va - B:va - B + size], (fn, spawner)
            seen.add(spawner)
    assert seen == {"spark", "dust", "shard", "skid", "ripple", "splash"}


def test_replay_buffer_constants(game_dir):
    img = load_image(game_dir)
    create = img[0x00430180 - B:0x00430180 - B + 391]
    for k in (R.HEADER + 2 * R.RIDER_SIZE,               # 0x5C10 buffer with rider snapshots
              2 * R.PROPS_SIZE, R.PROPS_SIZE,            # 0x1680, the props copy (0xB40)
              0x2000):                                    # default stream size
        assert struct.pack("<I", k) in create, hex(k)
    assert bytes([0x83, 0xC1, 2 * R.GOALS_SIZE]) in create         # add ecx, 0x4E (two goal blocks)
    area = img[0x00430D70 - B:0x00430D70 - B + 138]
    for k in (2 * R.RIDER_SIZE, 2 * R.PROPS_SIZE):      # + 0x5BD0, + 0x1680
        assert struct.pack("<I", k) in area, hex(k)
    assert bytes([R.PC_HEADER]) in area and bytes([0x83, 0x01, 2 * R.GOALS_SIZE]) in area   # 0x34, add [ecx], 0x4E
    # action.c: the demo buffer (flags 7, 0x2800 stream bytes)
    assert struct.pack("<I", R.DEMO_BUFFER) in img[0x00404F40 - B:0x004056A0 - B]


def prop_table(img):
    return [[struct.unpack_from("<HBBHHbb", img, 0x00557D28 - B + (lv * 16 + i) * 10) for i in range(16)]
            for lv in range(14)]


def test_prop_table_vs_header(game_dir):
    img = load_image(game_dir)
    tab = prop_table(img)
    hdr = (INC / "bmxprop.h").read_text(encoding="utf-8")
    moves = {int(v) for v in re.findall(r"BMXMOVE_\w+\s*=\s*(\d+)", hdr)}
    for lv, rows in enumerate(tab):
        for i, (pid, typ, motion, l0, l1, sfx1, sfx2) in enumerate(rows):
            if pid == 0:
                assert (typ, motion, l0, l1) == (0, 0, 0, 0)
                continue
            assert typ <= 5 and motion in moves, (lv, i)
            if typ >= 4:
                assert motion == 0 and pid >= 1000, (lv, i)          # model pairs / sounds: OBJI u16 ids
            else:
                assert pid < 0x20, (lv, i)                            # groups: OBJI byte 7 (5 bits)
            for link in (l0, l1):                                     # links name other slots of the level
                if link and motion >= 13:
                    assert link in [r[0] for r in rows], (lv, i, link)
    # bmxpropInit's Greenville Dirt special case: ids 20..25 there are traffic (motions 8 / 9)
    assert sorted(r[0] for r in tab[0] if r[2] in (8, 9)) == [20, 21, 22, 23, 24, 25]
    assert all(not any(r[0] for r in tab[lv]) for lv in (12, 13))     # Test A / Test B
    assert u32(img, 0x00557D28 + 14 * 160) == 0


# ------------------------------------------------------------------ levels and replay records
def level_props(game_dir, lv):
    path = game_dir / "assets" / LEVELS[lv]
    if not path.is_dir():
        pytest.skip("level folder missing")
    lev = L.Level.read_dir(path)
    comp = {n.lower(): c for n, c in lev.components.items()}
    return comp["prop._sd"].props, comp["propdef._sd"].defs


def bound_slots(tab_level, props):
    """Slots bmxpropInit binds: types 0..3 by OBJI byte 7 (zbspFindPropsByGroup), 4 / 5 by OBJI u16 id."""
    byte7 = {p.obji.data[7] for p in props if p.obji}
    ids16 = {struct.unpack_from("<H", p.obji.data, 8)[0] for p in props if p.obji}
    return [i for i, (pid, typ, *_) in enumerate(tab_level)
            if pid and ((typ < 4 and pid in byte7) or (typ >= 4 and pid in ids16))]


def replays(game_dir):
    p = game_dir / "assets" / "replay.zal"
    if not p.is_file():
        pytest.skip("replay.zal missing")
    return R.records(Z.read(p.read_bytes()))


def test_replay_prop_blocks_match_bmxpropInit(game_dir):
    img = load_image(game_dir)
    tab = prop_table(img)
    recs = replays(game_dir)
    pc = {rid: r for rid, r in recs.items() if r.layout == "pc"}
    assert sorted(pc) == [0, 1, 2, 3, 4, 8, 10]
    checked = 0
    for rid, r in pc.items():
        lv = rid // 2                                   # action.c: record id = rider + 2 * level
        assert r.flags() == R.NO_RIDER                  # game.c records one player with flags 1
        start = r.block("props_start")
        slots = [struct.unpack_from("<BBBBiI", start, s * 0x84) for s in range(16)]
        used = [s for s in range(16) if slots[s][0]]
        on_pc = r.fields["cursor"] < 0x80000000         # recorded on the PC build (0x8C... = console RAM)
        for s in used:
            state, flags, index, player, timer, col = slots[s]
            links = struct.unpack_from("<II", start, s * 0x84 + 0x7C)
            assert index == s and tab[lv][s][0] != 0, (rid, s)
            if on_pc:                                   # (the console build's table differs in places)
                assert links == tab[lv][s][3:5], (rid, s)
            if lv == 0 and tab[lv][s][2] in (8, 9):         # Greenville Dirt traffic runs from the start
                assert (state, timer) == (3, -1), (rid, s)
            elif tab[lv][s][1] < 4:
                assert (state, flags, timer) == (1, 0, 0), (rid, s)
        if on_pc:
            props, _ = level_props(game_dir, lv)
            assert used == bound_slots(tab[lv], props), rid
            switches = [struct.unpack_from("<III", start, 0x840 + k * 12)[2] for k in range(64)]
            n_ids = sum(1 for p in props if p.obji and struct.unpack_from("<H", p.obji.data, 8)[0])
            assert sum(1 for x in switches if x) == n_ids, rid
            checked += 1
    assert checked == 4


def test_replay_records_round_trip_and_play(game_dir):
    recs = replays(game_dir)
    assert len(recs) == 24
    for rid, r in recs.items():
        if r.layout != "pc":
            continue
        evs = r.events()
        assert R.encode_events(evs) == r.stream[:r.used_length()]
        assert R.record((f.buttons, f.sticks) for f in r.play()) == evs, rid
        assert sum(e.delta for e in evs) == r.fields["frames"] + 1


def test_demo_records_fit_the_demo_buffer(game_dir):
    img = load_image(game_dir)
    runs = [tuple(img[0x00551A34 - B + 2 * k:0x00551A34 - B + 2 * k + 2]) for k in range(3)]   # g_actionDemoRuns
    recs = replays(game_dir)
    for rider, lv in runs:
        r = recs[rider + 2 * lv]
        assert r.layout == "pc" and r.fields["frames"] > 0
        assert len(r.write()) <= R.DEMO_SAVE_AREA       # "replay data too big for demo replay buffer"
        assert len(r.events()) > 100


def test_propdef_rail_flag_never_meets_uncoded_class(game_dir):
    """PropDef OBJI byte 0 bit 0 = build grind rails from the collision box (zrollerLoadAssets ->
    zcolCreatePropRails); file class 3 (engine class 1) would assert "Not coded"."""
    seen = 0
    for lv in range(12):
        props, defs = level_props(game_dir, lv)
        for d in defs:
            if d.obji and len(d.obji.data) == 23:
                assert d.obji.data[0] in (0, 1)
                if d.obji.data[0] & 1:
                    assert d.obji.data[22] in (1, 2, 4), (LEVELS[lv], d.name)
                    seen += 1
        for p in props:
            if p.obji:
                assert len(p.obji.data) == 10 and p.obji.data[4:7] == b"\0\0\0"
    assert seen == 92

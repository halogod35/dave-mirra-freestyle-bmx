"""Self-consistency checks for the decomp of game.c and comdata.c (game flow, goals, rewards, records).

No C compiler on the build PC, so the C is checked two ways:
1. structure: every function drafted, names agree with decomp/functions.csv and decomp/symbols/symbols.csv, no
   Ghidra artefacts, the struct layouts (game.h, rider.h) agree with their "+0x.." comments and the
   size checks in ghidra_types.txt, every assert (message, file, line) exists as a call site in
   NgBMX.exe, and the tables the Python port embeds equal the exe's;
2. behaviour: dmbmx.formats.progress (the Python port of the save accessors, the reward logic and
   the record switch) against save_fields.py and the real SAVE.DAT, and `dmbmx save complete-all`.
"""
import csv
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from dmbmx.formats import progress as P
from dmbmx.formats import save as S
from dmbmx.formats import save_fields as F
from dmbmx.formats import tricks as T

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
GAME = "zaxis/Bmx/Src/com/action/game.c"
COMDATA = "zaxis/Bmx/Src/com/action/comdata.c"
COUNTS = {GAME: 47, COMDATA: 3}   # + gameSetWord 0x0040BFD0 (found with dyn.c)
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    out = []
    for m in VA_RE.finditer((SRC / f).read_text(encoding="utf-8")):
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
    rows = csv_rows()
    syms = last_symbols("func")
    for f, n in COUNTS.items():
        fr = [r for r in rows if r["file"] == f]
        assert len(fr) == n, (f, len(fr))
        cf = c_functions(f)
        assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in fr), f
        by_va = {int(r["va"], 16): r for r in fr}
        for va, size, status, name in cf:
            r = by_va[va]
            assert status == "draft" and r["status"] == "draft", (f, hex(va))
            assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
            assert syms[va]["name"] == name, hex(va)
            prefix = "comdata" if f == COMDATA else "game"
            assert re.match(prefix + r"[A-Z]\w+$", name), name
    # the two accessors moved out of the camera gap into comdata.c
    assert syms[0x00407BA0]["source_file"] == syms[0x00407D20]["source_file"] == "Bmx/Src/com/action/comdata.c"


def test_no_ghidra_artefacts():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|(?:game|comdata)_[0-9a-f]{8})\b")
    old_names = re.compile(r"\b(save_InitDefaults|save_GetRecordPtr|game_[A-Z]\w+|comdata_[A-Z]\w+)\b")
    for f in COUNTS:
        t = (SRC / f).read_text(encoding="utf-8")
        assert "TODO(decomp)" not in t, f
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
        code = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
        assert not old_names.search(code), f
    # nobody else still calls the old names
    for p in SRC.rglob("*.c"):
        t = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        assert not re.search(r"\b(game_(Apply|Award|Advance|Get|Set|Is|Record)\w+|comdata_GetGoalInfo|save_InitDefaults|save_GetRecordPtr)\b", t), p


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}


def layouts(*headers):
    """{struct: (size, [(field, offset, commented_offset)])} (x86, natural alignment)."""
    out = {}
    for header in headers:
        t = (INC / header).read_text(encoding="utf-8")
        t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
        for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
            off, fields, maxal = 0, [], 1
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
                fields.append((name, off, int(comment, 16) if comment else None))
                off += size * count
                maxal = max(maxal, align)
            out[m.group(3)] = ((off + maxal - 1) // maxal * maxal, fields, maxal)
    return out


def ghidra_sizes():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    return want


def test_struct_layouts():
    lay = layouts("rider.h", "game.h")
    want = ghidra_sizes()
    names = {"BmxTrickRec", "BmxScoreBlock", "BmxAreaStats", "BmxJudgeRun", "BmxContact", "BmxRider",
             "BmxWheel",                                              # physics.c
             "GoalObjective", "GoalDef", "GoalTierDef", "GameReward", "GameRewardTables", "GameLiveRecord",
             "GameTrickInfo"}
    assert names <= set(lay)          # BmxInput, BmxMotionSlot, BmxSigCombo: test_decomp_rider.py
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert "game.h" in types and types.index("rider.h") < types.index("game.h")
    for name, (size, fields, _) in lay.items():
        assert size == want[name], (name, hex(size))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))
    for h, structs in (("rider.h", [n for n in names if n.startswith("Bmx")]),
                       ("game.h", [n for n in names if not n.startswith("Bmx")])):
        t = (INC / h).read_text(encoding="utf-8")
        for n in structs:
            assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n


def test_rider_fields_game_c_uses():
    """The BmxRider offsets game.c reads/writes (from the disassembly / Ghidra decompilation)."""
    off = {n: o for n, o, _ in layouts("rider.h", "game.h")["BmxRider"][1]}
    expect = {"stateFlags": 0x38, "slot": 0x3C, "trickId": 0x4A, "modId": 0x4B, "modCount": 0x4C,
              "hud4e": 0x4E, "crashKind": 0x4F, "spin": 0x50, "flip": 0x54, "crash": 0x58, "score": 0x5C,
              "trickState": 0xEC, "trickCount": 0xED, "landTimer": 0xEE, "trickLog": 0xEF, "area": 0x2C6,
              "total": 0x3A6, "bestWallHeight": 0x3B4, "skidLength": 0x3B8, "hintMask": 0x3BC,
              "runRewards": 0x3C0, "judge": 0x3C4, "surface": 0x3DD, "matrix": 0x3E0, "pos": 0x420,
              "wallHeight": 0x440, "grindBalance": 0x444, "vel": 0x448, "frameDist": 0x484,
              "surfaceObj": 0x488, "stats": 0x6D0, "contact": 0x77C, "riderActor": 0xB94,
              "bikeModel": 0xC0C, "spawnPos": 0xC10, "tuneC68": 0xC68, "bikeActor": 0xD90}
    for n, o in expect.items():
        assert off[n] == o, (n, hex(off[n]), hex(o))
    blk = {n: o for n, o, _ in layouts("rider.h")["BmxScoreBlock"][1]}
    assert (blk["runScore"], blk["totalScore"], blk["setScore"], blk["wins"], blk["trick"]) == (0, 4, 8, 0xC, 0x10)
    # contact[1].pos is what gameCheckTransfer reads at rider +0xB84
    assert off["contact"] + 0x20C + 0x1FC == 0xB84


def load_image(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def c_asserts():
    out = set()
    for f in COUNTS:
        t = (SRC / f).read_text(encoding="utf-8")
        defs = {m.group(1): m.group(2).replace("\\\\", "\\") for m in re.finditer(r'#define (\w+_SRC) "([^"]+)"', t)}
        for m in re.finditer(r'zAssertFail\(\s*"((?:[^"\\]|\\.)*)",\s*(\w+),\s*(0x[0-9a-f]+)', t):
            out.add((defs[m.group(2)], m.group(1), int(m.group(3), 16)))
    return out


def test_assert_sites_exist_in_exe(game_dir):
    img = load_image(game_dir)
    sites = c_asserts()
    assert len(sites) == 16   # + gameSetWord "horse word too long"
    for path, msg, line in sorted(sites):
        p_addr = img.find(path.encode() + b"\0") + 0x400000
        assert p_addr > 0x400000, path
        if msg:
            addrs = [m.start() + 0x400000 for m in re.finditer(re.escape(msg.encode() + b"\0"), img)]
        else:
            addrs = [0x0056BD44]                     # the empty message string game.c uses
        assert addrs, msg
        pats = [push(line) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", a) for a in addrs]
        assert any(p in img for p in pats), (path, msg, hex(line))


def test_embedded_tables_match_exe(game_dir):
    img = load_image(game_dir)
    goals, raw = P.read_exe_tables(img)
    assert tuple(tuple(len(g["objectives"]) for g in tier) for tier in goals) == P.OBJECTIVE_COUNTS
    lt, pk, fin, rf = P.rewards_from_raw(raw)
    assert (lt, pk, fin, rf) == (P.LEVEL_TIER_REWARDS, P.PARK_TIER_REWARDS, P.FINAL_REWARDS, P.RIDER_FINAL_REWARDS)
    b = 0x400000
    assert tuple(img[0x00553658 - b:0x00553658 - b + 12]) == P.MODE_STAT
    assert tuple(img[0x00554C8C - b:0x00554C8C - b + 12]) == P.MODE_ROUNDS
    assert struct.unpack_from("<6I", img, 0x00554C98 - b) == (40000, 50000, 60000, 45000, 55000, 70000)
    assert struct.unpack_from("<I", img, 0x00554CB0 - b)[0] == 120
    assert struct.unpack_from("<10b", img, 0x00554CC0 - b) == (10, 8, 2, 4, 6, -10, -2, -4, 0, 2)
    assert img[0x00554D00 - b:0x00554D04 - b] == b"BMX\0"
    assert struct.unpack_from("<27H", img, T.TRICK_MOTION_VA - b) == T.TRICK_MOTION
    assert struct.unpack_from("<38H", img, T.MOD_MOTION_VA - b) == T.MOD_MOTION
    # every objective type is one the C knows, and every goal has 1..4 objectives
    for tier in goals:
        for g in tier:
            assert 1 <= len(g["objectives"]) <= 4
            assert all(0 <= o["type"] <= 21 for o in g["objectives"])
    # the scoring tables the C names (gameScoreTrickSet)
    assert struct.unpack_from("<9H", img, 0x0055926C - b) == (0, 100, 200, 500, 1000, 2000, 3500, 5000, 7500)
    assert [round(x, 3) for x in struct.unpack_from("<9f", img, 0x0055934C - b)] == \
        [0.25, 1.0, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0]
    assert [round(x, 3) for x in struct.unpack_from("<4f", img, 0x00559340 - b)] == [1.0, 0.75, 0.5, 0.25]


def test_trick_motions_inside_the_bank():
    used = set()
    for t in range(46):
        m = T.trick_motions(t)
        if m is not None:
            assert 1 <= m[0] and m[2] <= 238, (t, m)
            used.update(m)
    for mod in range(T.MODIFIERS):
        m = T.modifier_motions(mod)
        assert 73 <= m[0] and m[2] <= 186
        used.update(m)
    assert T.trick_motions(0x1B)[0] == 187 and T.trick_motions(0x29)[2] == 231
    assert len(used) > 150


# ------------------------------------------------------------------ behaviour: records
def test_record_ids_cover_the_24_entries():
    hit = {}
    for mode in range(12):
        for sub in range(4):
            for stat in range(10):
                for idx in range(3):
                    e = P.record_entry(P.record_id(mode, sub, stat, 0, idx))
                    if e is not None:
                        assert 0 <= e < 24
                        hit.setdefault(e, set()).add((mode, stat))
    assert sorted(hit) == list(range(24))
    assert len(P.RECORD_ENTRIES) == 24
    # the stat games share the stat entries (game.c: g_gameModeStat)
    for mode in range(6, 11):
        assert P.record_entry(P.record_id(mode, 0, 1)) == P.MODE_STAT[mode] + 14


def real_save(game_dir):
    p = game_dir / "save" / "SAVE.DAT"
    if not p.is_file():
        pytest.skip("SAVE.DAT not found")
    return S.read(p.read_bytes())


def test_record_entries_in_real_save(game_dir):
    s = real_save(game_dir)
    # the four time limits of "best run" and "death match" have the same default records in levels
    # nobody played those modes on: entries 3..6 == 12..15, rising with the time limit
    same = [L for L in range(12) if [e.score for e in s.scores[L][3:7]] == [e.score for e in s.scores[L][12:16]]]
    assert len(same) >= 10
    for L in same:
        v = [e.score for e in s.scores[L][3:7]]
        assert v == sorted(v)


# ------------------------------------------------------------------ behaviour: goal bytes
def test_objective_accessors_match_save_fields(game_dir):
    s = real_save(game_dir)
    for r in range(12):
        for L in range(6):
            for t in range(3):
                for g in range(4):
                    for j in range(4):
                        nib = F.get(s, f"rider.{r}.level.{L}.goal.{t}.{g}.objective.{j}")
                        assert P.objective_state(s, r, L, t, g, j) == nib & 7
                        assert P.objective_flag(s, r, L, t, g, j) == nib >> 3


def test_set_objective_state_clears_the_flag():
    s = S.read(S.write(S.Save(riders=[bytearray(0x104) for _ in range(12)],
                              scores=[[S.ScoreEntry.parse(bytes(20)) for _ in range(24)] for _ in range(12)],
                              profiles=[S.Profile(bytearray(0x4C)) for _ in range(2)])))
    F.set(s, "rider.2.level.3.goal.1.2.objective.3", 0xA)
    P.set_objective_state(s, 2, 3, 1, 2, 3, 5)
    assert F.get(s, "rider.2.level.3.goal.1.2.objective.3") == 5          # bit 3 gone (whole nibble written)
    assert F.get(s, "rider.2.level.3.goal.1.2.objective.2") == 0


def test_done_goals_in_real_save_match_the_goal_table(game_dir):
    """Every goal the real save marks done has exactly the objective count of the exe's goal table
    and all its objectives REWARDED (0x40, "done this session", stays set until the next run)."""
    s = real_save(game_dir)
    n_done = 0
    for r in range(12):
        for L in range(6):
            for t in range(3):
                for g in range(4):
                    b = F.get(s, f"rider.{r}.level.{L}.goal.{t}.{g}.flags")
                    if b & 0x80:
                        n_done += 1
                        n = P.OBJECTIVE_COUNTS[L * 3 + t][g]
                        assert b & 0x3F == n, (r, L, t, g, hex(b))
                        assert all(P.objective_state(s, r, L, t, g, j) == P.STATE_REWARDED for j in range(n))
                    else:
                        assert not b & 0x40               # "done this session" only on done goals
    assert n_done > 100


# ------------------------------------------------------------------ behaviour: rewards
def default_save():
    """comdataInitDefaults 0x00407BA0 as a Save."""
    s = S.Save(riders=[bytearray(0x104) for _ in range(12)],
               scores=[[S.ScoreEntry.parse(bytes(20)) for _ in range(24)] for _ in range(12)],
               profiles=[S.Profile(bytearray(0x4C)) for _ in range(2)])
    for rec in s.riders:
        rec[4:7] = b"\x01\x01\x01"
    s.options[:] = bytes([0, 0, 0, 0, 18, 18, 1, 10])
    for p, pr in enumerate(s.profiles):
        pr.raw[0], pr.raw[1] = 3, p
        pr.raw[7:13] = b"player"
        struct.pack_into("<7I", pr.raw, 0x24, 0x80, 0x20, 0x40, 0x10, 0x100, 0x200, 0x4000)
    return S.read(S.write(s))


UNLOCKS = ["riders_unlocked", "cheats_unlocked"] + [f"rider.{r}.{k}" for r in range(12)
                                                    for k in ("levels_unlocked", "styles_unlocked",
                                                              "bikes_unlocked", "movie_unlocked")]


def test_complete_all_gives_exactly_what_unlock_all_unlocks():
    """The game's rewards for finishing everything = the unlocks of its cheats (unlock-all)."""
    a, b = default_save(), default_save()
    P.complete_all(a)
    F.unlock_all(b)
    assert {n: F.get(a, n) for n in UNLOCKS} == {n: F.get(b, n) for n in UNLOCKS}
    for r in range(12):
        assert F.get(a, f"rider.{r}.all_levels_complete") == 1
        assert all(F.get(a, f"rider.{r}.park_complete.{k}") for k in range(4))
        for L in range(6):
            assert P.goal_info(a, r, L) == (0, 2, 4)
        for L in range(6, 12):
            assert F.get(a, f"rider.{r}.comp.{L}") == 3


def test_complete_all_reward_order():
    """Slim Jim Guy comes from Acclaim Max Games Vert hardcore, Amish Boy when riders 0-9 are done."""
    s = default_save()
    P.complete_all(s)
    assert F.get(s, "riders_unlocked") == 12
    s = default_save()
    for r in range(9):                       # riders 0..8 only: no Amish Boy yet
        for L in range(12):
            for t in range(3):
                if L < 6:
                    P.complete_level_tier(s, r, L, t)
                else:
                    s.riders[r][P.COMP_OFF + L] = t + 1
                P.award_level_progress(s, r, L, from_tier=t, to_tier=t + 1)
    assert F.get(s, "riders_unlocked") == 11


def test_rewards_replayed_from_real_progress(game_dir):
    """Replaying the rewards for the progress stored in the real save never gives more than the save
    has (cheats can give more), and gives exactly the stored unlocks for riders without cheats."""
    real = real_save(game_dir)
    for r in range(12):
        s = default_save()
        for t in range(3):
            for L in range(12):
                if not P.tier_done(real, r, L, t):
                    continue
                if L < 6:
                    P.complete_level_tier(s, r, L, t)
                else:
                    s.riders[r][P.COMP_OFF + L] = t + 1
                P.award_level_progress(s, r, L, from_tier=t, to_tier=t + 1)
        for k in ("levels_unlocked", "styles_unlocked", "bikes_unlocked"):
            assert F.get(s, f"rider.{r}.{k}") <= F.get(real, f"rider.{r}.{k}"), (r, k)
        assert F.get(s, f"rider.{r}.flags") & 0x3E & ~F.get(real, f"rider.{r}.flags") == 0, r
    # riders 3, 6 and 9 were played without the unlock cheats: levels and styles agree exactly
    for r, lv, st in ((3, 2, 1), (6, 7, 3), (9, 6, 3)):
        assert (F.get(real, f"rider.{r}.levels_unlocked"), F.get(real, f"rider.{r}.styles_unlocked")) == (lv, st)


def test_apply_reward_raise_only_and_indirection():
    s = default_save()
    s.riders[4][6] = 5
    P.apply_reward(s, 4, (1, 1))
    assert s.riders[4][6] == 5 and F.get(s, "rider.4.flags") & 0x40
    assert P.apply_reward(s, 4, (9, 0)) == (10, 4)          # rider 4's final reward: cheat 4 (suspension)
    assert F.get(s, "cheat_unlocked.4") == 1
    P.apply_reward(s, 0, (4, 1), profile=0)
    assert F.get(s, "profile.0.tutorial_hints") == 0
    with pytest.raises(ValueError):
        P.apply_reward(s, 0, (3, 12))


def test_complete_all_is_raise_only_on_real_save(game_dir):
    real = real_save(game_dir)
    s = S.read(S.write(real))
    P.complete_all(s)
    for n in UNLOCKS:
        assert F.get(s, n) >= F.get(real, n), n
    for r in range(12):
        old, new = F.get(real, f"rider.{r}.flags"), F.get(s, f"rider.{r}.flags")
        assert old & ~new == 0, r
        assert s.riders[r][0xF7:] == real.riders[r][0xF7:]           # tricks landed, tail
    assert S.write(s)[0xC44:] == S.write(real)[0xC44:]               # scores and profiles untouched
    assert s.options[2:7] == real.options[2:7] and s.block_a_tail == real.block_a_tail


def test_cli_complete_all(game_dir, tmp_path):
    src = game_dir / "save" / "SAVE.DAT"
    if not src.is_file():
        pytest.skip("SAVE.DAT not found")
    inp = tmp_path / "in.dat"
    inp.write_bytes(src.read_bytes())
    out = tmp_path / "out.dat"
    r = subprocess.run([sys.executable, "-m", "dmbmx", "save", "complete-all", str(inp), "-o", str(out)],
                       capture_output=True, text=True, cwd=REPO)
    assert r.returncode == 0, r.stderr
    assert "game accepts: yes" in r.stdout
    s = S.read(out.read_bytes())
    assert all(P.goal_info(s, rr, L)[1:] == (2, 4) for rr in range(12) for L in range(6))
    assert inp.read_bytes() == src.read_bytes()
    r = subprocess.run([sys.executable, "-m", "dmbmx", "save", "complete-all", str(inp), "-o", str(inp)],
                       capture_output=True, text=True, cwd=REPO)
    assert r.returncode != 0                                        # refuses to overwrite without --in-place

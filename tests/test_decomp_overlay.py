"""Self-consistency checks for the decomp of overlay.c, overlymp.c and bmx/zoverii.c (the HUD and the
in-game overlay screens).

1. structure: all 122 + 11 + 18 functions are drafted (VA order, names = functions.csv = symbols.csv),
   the overlymp/overlay gap file is gone, no Ghidra artefacts and no old overlay_XXXXXXXX names
   anywhere in decomp/, the overlay.h / zoverii.h layouts agree with their "+0x.." comments, the
   sizeof checks and ghidra_types.txt;
2. against the exe: the assert sites of the three files (message, file, line) are exactly the ones
   the C reproduces; the HUD item table (0x00556F00), the 18 overlay screens and the screen table
   (0x005571B8), the memory-card pages, rectangles, glyph / name tables and initial values read back
   as the C initialises them;
3. against the data: every text id the C uses resolves through the STRS tables (ZAL.md 3.1) of its
   string set; the reward texts of every reward in g_rewards (the RESULTS screen: answers reward
   types 5/6/7), the hint strings vs the button font, the four overlay fonts (zoveriiLoadFont's
   format) and the PLYR names the records / standings screens print.
"""
import csv
import re
import struct
import sys
from pathlib import Path

import pytest

from conftest import GAME_DIR

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
sys.path.insert(0, str(REPO / "tools" / "3dsmax" / "parser"))
import dmparse  # noqa: E402

OVERLAY = "zaxis/Bmx/Src/com/action/overlay.c"
OVERLYMP = "zaxis/Bmx/Src/com/action/overlymp.c"
ZOVERII = "zaxis/Bmx/Src/com/zge/bmx/zoverii.c"
COUNTS = {OVERLAY: 122, OVERLYMP: 11, ZOVERII: 19}   # + zoveriiInit 0x004578F0 (found with dyn.c)
RANGES = {OVERLYMP: (0x004124C0, 0x004147E0), OVERLAY: (0x004147E0, 0x0041FA20), ZOVERII: (0x004578F0, 0x0045A1A0)}
PATHS = {OVERLYMP: 0x00557434, OVERLAY: 0x005575C8, ZOVERII: 0x0055B9AC}
SOURCE = {OVERLAY: "Bmx/Src/com/action/overlay.c", OVERLYMP: "Bmx/Src/com/action/overlymp.c",
          ZOVERII: "Bmx/Src/com/zge/bmx/zoverii.c"}
PREFIX = {OVERLAY: "overlay", OVERLYMP: "overlymp", ZOVERII: "zoverii"}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")
B = 0x400000


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
                out[int(r["address"], 16)] = r
    return out


def names_by_va():
    return {int(r["va"], 16): r["name"] for r in csv_rows()}


# ------------------------------------------------------------------ structure
@pytest.mark.parametrize("f", [OVERLAY, OVERLYMP, ZOVERII])
def test_every_function_drafted_and_named_consistently(f):
    rows = [r for r in csv_rows() if r["file"] == f]
    assert len(rows) == COUNTS[f]
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    lo, hi = RANGES[f]
    for va, size, status, name in cf:
        r = by_va[va]
        assert lo <= va < hi
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, hex(va)
        assert syms[va]["source_file"] == SOURCE[f]
        assert re.match(PREFIX[f] + r"[A-Z]\w+$", name), name
    assert not (SRC / "_unassigned" / "gap_overlymp__overlay.c").exists()


OLD = re.compile(r"\b(overlay_[0-9a-f]{8}|overlymp_[0-9a-f]{8}|zoverii_[0-9a-f]{8}|FUN_00414580)\b")


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|_DAT_\w+)\b")
    allowed = {"zcardSelectPort", "zcardCheck", "zcardFormat", "zcardStart", "zcardStop"}  # memory-card stubs
    for f in COUNTS:
        t = text(f)
        assert "TODO(decomp)" not in t
        assert "\0" not in t and "\r" not in t
        for n, line in enumerate(t.splitlines(), 1):
            for m in bad.finditer(line):
                assert m.group(0) in allowed, f"{f}:{n}: {line.strip()}"
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        assert not OLD.search(p.read_text(encoding="utf-8")), p
    # the callers use the names (action.c / game.c)
    assert "overlaySetItem(" in text("zaxis/Bmx/Src/com/action/game.c")
    assert "overlaySetScreen(" in text("zaxis/Bmx/Src/com/action/action.c")


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}
FNPTR = {"OverlayOnFn", "OverlayUpdateFn", "OverlayDrawFn", "OverlaySetFn", "OverlayGetFn"}


def layouts(header):
    out = {}
    t = (INC / header).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields, maxal = 0, {}, 1
        for line in m.group(2).splitlines():
            for d in re.finditer(r"(?:^|;)\s*(?:const\s+)?(?:struct\s+)?(\w+)\s+([\w\s,*\[\]]+?);\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?",
                                 line):
                typ, decls, comment = d.groups()
                for k, dec in enumerate(x.strip() for x in decls.split(",")):
                    dm = re.match(r"(\**)\s*(\w+)((?:\[\w+\])*)$", dec)
                    ptr, name, dims = dm.groups()
                    if ptr or typ in FNPTR:
                        size = align = 4
                    elif typ in SIZES:
                        size = align = SIZES[typ]
                    else:
                        size, align = out[typ][0], out[typ][2]
                    count = 1
                    for n in re.findall(r"\[(\w+)\]", dims):
                        count *= int(n, 0)
                    off = (off + align - 1) // align * align
                    if k == 0:
                        assert comment is None or int(comment, 16) == off, (m.group(3), name, hex(off))
                    fields[name] = off
                    off += size * count
                    maxal = max(maxal, align)
        out[m.group(3)] = ((off + maxal - 1) // maxal * maxal, fields, maxal)
    return out


def test_struct_layouts():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    per = {h: layouts(h) for h in ("zoverii.h", "overlay.h")}
    lay = {**per["zoverii.h"], **per["overlay.h"]}
    sized = {"ZoverPolyF4": 0x24, "ZoverPolyG4": 0x30, "ZoverPolyFT4": 0x3C, "ZoverPrim": 0xA0,
             "ZoverTexture": 0x14, "ZoverGlyph": 0x08, "ZoverFont": 0x0C, "ZoverFontHeader": 0x0C,
             "ZoverFontRecord": 0x0D, "ZoverSprite": 0x14, "OverlayScreen": 0x10, "OverlayItem": 0x18,
             "OverlayMenuEntry": 0x04, "OverlaySavePage": 0x04, "OverlayTrickEntry": 0x02,
             "OverlayScoreReq": 0x08, "OverlayMeterReq": 0x0C, "OverlayTimedText": 0x04, "OverlayMessage": 0x08,
             "OverlayFadeReq": 0x06, "OverlayFade": 0x20}
    for n, size in sized.items():
        assert lay[n][0] == size == want[n], n
    # the offsets the code uses
    assert lay["ZoverPrim"][1]["type"] == 0x9C and lay["ZoverPrim"][1]["next"] == 0x9E
    assert (lay["ZoverPolyFT4"][1]["clut"], lay["ZoverPolyFT4"][1]["tpage"], lay["ZoverPolyFT4"][1]["texture"],
            lay["ZoverPolyFT4"][1]["mode"], lay["ZoverPolyFT4"][1]["blend"]) == (0x10, 0x1A, 0x30, 0x34, 0x38)
    assert (lay["ZoverPolyF4"][1]["mode"], lay["ZoverPolyG4"][1]["mode"]) == (0x1C, 0x28)
    assert (lay["ZoverFont"][1]["height"], lay["ZoverFont"][1]["first"], lay["ZoverFont"][1]["glyphs"]) == (5, 6, 8)
    assert (lay["OverlayScreen"][1]["count"], lay["OverlayScreen"][1]["sel"]) == (0x0C, 0x0D)
    assert lay["OverlayMeterReq"][1]["stat"] == 0x0A and lay["OverlayFade"][1]["step"] == 0x14
    for h in ("zoverii.h", "overlay.h"):
        t = (INC / h).read_text(encoding="utf-8")
        for n in per[h]:
            if n in sized:
                assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n
        assert h in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        snames = {r["name"] for r in csv.DictReader(fh) if r["kind"] == "struct" and r["type"] in ("decomp/include/overlay.h", "decomp/include/zoverii.h")}
    assert snames == set(sized)


# ------------------------------------------------------------------ exe
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def cstr(img, va):
    return img[va - B:img.index(b"\0", va - B)].decode("latin-1")


def u32(img, va):
    return struct.unpack_from("<I", img, va - B)[0]


def exe_asserts(img, f):
    """(message, line) of every 'push line; push __FILE__; push msg' in the file's code range."""
    lo, hi = RANGES[f]
    out = set()
    pat = b"\x68" + struct.pack("<I", PATHS[f])
    for m in re.finditer(re.escape(pat), img[lo - B:hi - B]):
        p = lo - B + m.start()
        assert img[p + 5] == 0x68
        if img[p - 2] == 0x6A:
            line = img[p - 1]
        else:
            assert img[p - 5] == 0x68
            line = u32(img, p - 4 + B)
        out.add((cstr(img, u32(img, p + 6 + B)), line))
    return out


def c_asserts(f):
    t = text(f)
    out = set()
    for m in re.finditer(r'zAssertFail\(("(?:[^"\\]|\\.)*"|g_overlayEmpty), \w+_SRC, (0x[0-9A-Fa-f]+)', t):
        msg = "" if m.group(1) == "g_overlayEmpty" else m.group(1)[1:-1]
        out.add((msg, int(m.group(2), 16)))
    for m in re.finditer(r"zover(?:PushDirty|MarkDirty)\([^;]*?(0x[0-9A-Fa-f]+)\);", t):
        out.add(("dirty list overflow", int(m.group(1), 16)))
    for m in re.finditer(r'loadFont\([^;]*?(0x[0-9A-Fa-f]+),\s*"([^"]+)", "([^"]+)"\);', t):
        n = int(m.group(1), 16)
        out |= {(m.group(2), n), ("no memory for font", n + 3), (m.group(3), n + 6)}
    for m in re.finditer(r"line = (0x[0-9A-Fa-f]+);", t):
        out.add(("string table not found - invalid language?", int(m.group(1), 16)))
    return out


@pytest.mark.parametrize("f", [OVERLAY, OVERLYMP, ZOVERII])
def test_assert_sites_match_exe(img, f):
    exe, c = exe_asserts(img, f), c_asserts(f)
    assert cstr(img, PATHS[f]).replace("\\", "\\\\") in text(f)
    if f == OVERLAY:
        # overlayLoadStrings passes its second line (0x51F) in a register, not as an immediate push
        late = ("string table not found - invalid language?", 0x51F)
        assert c - exe == {late} and struct.pack("<I", 0x51F) in img[0x00414DD0 - B:0x00414F01 - B]
        c = c - {late}
    assert c == exe, (sorted(c - exe), sorted(exe - c))
    assert len(exe) == {OVERLAY: 19, OVERLYMP: 6, ZOVERII: 18}[f]


def c_initialiser(t, name):
    return re.search(name + r"\[[^\]]*\] = \{(.*?)\n\};", t, re.S).group(1)


def test_item_and_screen_tables_match_exe(img):
    t = text(OVERLAY) + text(OVERLYMP)
    va = {n: v for v, n in names_by_va().items()}
    # HUD items: { flags, init, draw, show, set, get }
    body = c_initialiser(t, "g_overlayItems")
    items = [[x.strip() for x in r.split(",")] for r in re.findall(r"\{([^{}]*)\}", body)]
    assert len(items) == 17
    for i, row in enumerate(items):
        exe = struct.unpack_from("<6I", img, 0x00556F00 + 0x18 * i - B)
        mine = [0 if x in ("0", "NULL") else va[x] for x in row]
        assert list(exe) == mine, i
    # screens: { enter, update, draw, count }
    screens = {}
    for m in re.finditer(r"static OverlayScreen (s_\w+)\s*=\s*\{([^}]*)\}; /\* 0x([0-9A-F]{8}) \*/", t):
        row = [x.strip() for x in m.group(2).split(",")]
        a = int(m.group(3), 16)
        exe = struct.unpack_from("<3IB", img, a - B)
        assert list(exe[:3]) == [0 if x == "NULL" else va[x] for x in row[:3]], m.group(1)
        assert exe[3] == int(row[3], 0), m.group(1)
        screens[m.group(1)] = a
    assert len(screens) == 18
    body = re.sub(r"/\*.*?\*/", "", c_initialiser(t, "g_overlayScreens"), flags=re.S)
    refs = [x.strip() for x in body.split(",") if x.strip()]
    exe = struct.unpack_from("<26I", img, 0x005571B8 - B)
    assert [0 if x == "NULL" else screens[x.lstrip("&")] for x in refs] == list(exe)


def test_data_tables_match_exe(img):
    t = text(OVERLAY)
    # memory-card pages {count, pad, text}
    body = c_initialiser(t, "g_overlaySavePages")
    pages = [tuple(int(x, 0) for x in r.split(",")) for r in re.findall(r"\{\s*([^{}]*?)\s*\}", body)]
    assert pages == [struct.unpack_from("<BBH", img, 0x00557224 + 4 * i - B) for i in range(12)]
    # rectangles
    assert struct.unpack_from("<2I", img, 0x00557260 - B) == (0x280, 0x3C)
    assert struct.unpack_from("<2I", img, 0x00557298 - B) == (0x280, 0x1E0)
    # glyph tables, as indexed by the code (0x005572BB + 'A', 0x005572CB + dir)
    assert img[0x005572FC - B:0x00557308 - B] == b"adwxXSTC2143"
    assert img[0x005572BB + ord("A") - B] == ord("a")
    assert img[0x005572CB - B:0x005572D4 - B] == b"\0adwxqezc"
    assert [cstr(img, u32(img, 0x005572D4 + 4 * i)) for i in range(5)] == ["", "R", "P", "p", ""]
    assert [u32(img, 0x005572E8 + 4 * i) for i in range(5)] == [0x2808080, 0x2800000, 0x2008000, 0x2000080, 0x2800000]
    assert [cstr(img, u32(img, 0x00557308 + 4 * i)) for i in range(4)] == ["double", "triple", "quad", "sick"]
    assert [cstr(img, u32(img, 0x005594E0 + 4 * i)) for i in range(9)] == \
        ["", "180", "360", "540", "720", "900", "1080", "1260", "extreme"]
    # the trick-name builder's words and the debug replay path
    for s in ("half-cab", "opposite", "alley-oop", "spin", "flair", "flail", "truck driver", "flip", "limo",
              "triple flip", "vette", " driver", "SAVING GAME DATA...", "s:\\bmx\\assets\\%s\\replay\\demo%02d%02d.rpy",
              "%1ld%c%02ld", "%ldft", "%0ld' %ld\"", "%ld-%s", "%02d.%d"):
        assert (s.encode("latin-1") + b"\0") in img, s
        assert s.replace("\\", "\\\\").replace('"', '\\"') in t or s in t, s
    # initial values
    assert img[0x00557254 - B:0x0055725E - B] == b"BMX\0\0\0\0\0\0\x02"
    assert "g_overlympWord[9] = \"BMX\"" in text(OVERLYMP) and "g_overlympWordCursor = 2" in text(OVERLYMP)
    assert img[0x00556EF8 - B] == 2 and "g_overlympSubMode = 2" in t
    assert u32(img, 0x00556EFC) == 0xFFFFFFFF and "g_overlaySaveResult = -1" in t
    assert img[0x0056BD44 - B] == 0
    # the reward icon letters (stack table of overlayAddReward: mov byte ptr [esp + k], c)
    icons = re.search(r"s_rewardIcon\[12\] = \{([^}]*)\}", t).group(1)
    letters = [x.strip().strip("'") for x in icons.split(",")]
    code = img[0x004180F0 - B:0x004180F0 + 0x60 - B]
    for c in set(letters):
        assert bytes([0xC6, 0x44, 0x24]) in code and bytes([ord(c)]) in code, c


# ------------------------------------------------------------------ data
@pytest.fixture(scope="module")
def misc():
    f = GAME_DIR / "assets" / "miscdata.zal"
    if not f.is_file():
        pytest.skip("no miscdata.zal")
    z = dmparse.read_zal(f.read_bytes())
    return {g["id"]: g["data"] for g in z["groups"]}


@pytest.fixture(scope="module")
def texts(misc):
    """English (language 0): HUD 0x1xxx, single player 0x2xxx, multiplayer 0x3xxx, frontend 0x0xxx."""
    out = {}
    for tid in (0x7919, 0x7D01, 0x7D05, 0x7D09):
        s = dmparse.read_strs(misc[tid])
        for i, v in enumerate(s["strings"]):
            out[s["base_id"] + i] = v
    return out


def test_text_ids_resolve(texts):
    for f in COUNTS:
        t = text(f)
        for m in re.finditer(r"zTextGet\(\(?(0x[0-9A-Fa-f]+)(u?\))?", t):
            tid = int(m.group(1), 16)
            assert tid in texts, (f, hex(tid))
            if m.group(2):                                    # a constant id: a real text
                assert texts[tid] != "", (f, hex(tid))
            # the multiplayer screens only use HUD and multiplayer ids (0x2xxx is not loaded there)
            if f == OVERLYMP:
                assert tid < 0x2000 or tid >= 0x3000, hex(tid)
    # families the C indexes
    assert [texts[0x1056 + i] for i in (0, 4, 12)] == ["look where you're going", "landed sideways", "you died"]
    assert [texts[0x1063 + i] for i in (0, 0xA, 0xC, 0xD)] == \
        ["ONE MORE TO GO...", "HIGH SCORE CLEARED!", "TRICK COMBO!", "NEW HIGH SCORE!"]
    assert [texts[0x1063 + i] for i in range(0x10, 0x16)] == \
        ["LONGEST GRIND", "LONGEST MANUAL", "LONGEST SKID", "HIGHEST MARK", "BIG AIR", "LONGEST JUMP"]
    assert [texts[0x1087 + c] for c in range(4)] == ["behind", "wide", "above", "first person"]
    assert [texts[0x1094 + s] for s in (1, 9)] == ["best run", "longest skid"]
    assert texts[0x1079] == "PAUSE" and texts[0x1091] == "NEW RECORD!" and texts[0x1092] == "ENTER YOUR NAME"
    assert [texts[0x107A + i] for i in range(7)] == ["continue", "restart run", "view challenges", "options",
                                                     "trick list", "view records", "end run"]
    assert texts[0x110F] == "SAVE" and texts[0x1112 + 4 * 7 + 1] == "Save complete."
    # SELECT GAME lists modes 1..10 (line + 1)
    assert [texts[0x3002 + i] for i in (0, 9)] == ["best run", "longest manual"] and texts[0x3001] == "single player"
    # the goal texts of the HUD item and the CHALLENGES screen: 0x2089 + 3 (12 level + 4 tier + goal)
    assert texts[0x2089 + 3 * (12 * 2 + 4 * 0 + 2)] == "pull a 540"           # Backyard Pools, amateur 2
    # what each tier unlocks, 0x2041 + 3 (4 level + tier)
    assert texts[0x2041] == "Complete the amateur challenges to"


def reward_text_id(rw, rider, sponsor, final):
    """overlayAddReward's id choice (Python port); returns (id, lines) or None."""
    t, p = rw
    if t == 1:
        return p * 2 + 0x21BB
    if t == 2:
        return 0x21CF if rider >= 10 else p * 2 + 0x21C5
    if t == 3:
        return p * 2 + 0x21BD
    if t == 4:
        return p * 2 + 0x21A1
    if t == 5:
        return sponsor[rider][p] * 2 + 0x21D5
    if t == 6:
        return p * 2 + 0x2243
    if t == 7:
        return (p + rider * 2) * 2 + 0x225F
    if t == 8:
        return p * 2 + 0x228F
    if t == 9:
        return reward_text_id(final[rider][p], rider, sponsor, final)
    if t == 10:
        return p * 2 + 0x2291
    return None


PREFIXES = {1: ("NEW BIKE!",), 2: ("NEW OUTFIT!",), 3: ("SECRET!",), 4: ("NEW LEVEL!",),
            5: ("NEW SPONSOR!", "NEW RESPECT!", "SLIM JIM!", "AMISH!"),
            6: ("TIP!", "SLIM JIM!", "CONGRATULATIONS!"), 7: ("TRICKTIP!",), 8: ("SECRET!",), 10: ("SECRET!",)}


def test_reward_texts(img, texts):
    """Every reward of g_rewards (0x005568B8) shows a text on the RESULTS screen; types 5/6/7 (no
    effect in gameApplyReward) are sponsor / tip / signature-trick texts."""
    raw = img[0x005568B8 - B:0x005568B8 + 0xE0 - B]
    pairs = [tuple(raw[i:i + 2]) for i in range(0, 0xE0, 2)]
    final = [[pairs[0x58 + r * 2], pairs[0x58 + r * 2 + 1]] for r in range(12)]
    sponsor = [list(img[0x00556998 + 8 * r - B:0x00556998 + 8 * r + 8 - B]) for r in range(12)]
    seen = {}
    for rider in range(12):
        for rw in pairs[:0x58]:
            if rw[0] == 0:
                continue
            tid = reward_text_id(rw, rider, sponsor, final)
            kind = rw[0] if rw[0] != 9 else final[rider][rw[1]][0]
            if kind == 0:
                continue
            assert tid is not None, rw
            assert texts[tid].startswith(PREFIXES[kind]), (rider, rw, hex(tid), texts[tid])
            seen.setdefault(kind, set()).add(texts[tid].split("!")[0])
    assert {5, 6, 7} <= set(seen)
    assert seen[5] >= {"NEW SPONSOR"} and seen[7] == {"TRICKTIP"}


def test_hint_texts_fit_the_button_font(misc, texts):
    """Hints 0..21: 3 lines at 0x114A + 3 id; capital letters are buttons (A..L), at most 4 per hint
    (g_hudHintText has 4 glyph slots: 'too many buttons in hint string')."""
    glyphs = "adwxXSTC2143"
    font = misc[0x7534]
    chars = {font[12 + 13 * i] for i in range(font[0])}
    missing = set()
    for h in range(22):
        lines = [texts[0x114A + 3 * h + k] for k in range(3)]
        assert any(lines), h
        caps = [c for c in "".join(lines) if "A" <= c <= "Z"]
        assert len(caps) <= 4 and all(c <= "L" for c in caps), (h, lines)
        missing |= {glyphs[ord(c) - 65] for c in caps if ord(glyphs[ord(c) - 65]) not in chars}
    # the PC button font has no L2 / R2 glyphs ("4", "3"); no hint asks for them
    assert not missing
    assert {ord(c) for c in "wedcxzaqXT1SC2 "} == chars


def test_font_assets(misc):
    """zoveriiLoadFont's format: u8 count, u8 height at +4, 13-byte records from +0xC (u8 char, u16 u,
    u16 v, u16 w at +1/+5/+9); the file is exactly that long."""
    want = {0x7531: (82, 32), 0x7532: (144, 24), 0x7534: (15, 32), 0x7535: (11, 16)}
    for fid, (n, h) in want.items():
        d = misc[fid]
        assert (d[0], d[4], len(d)) == (n, h, 12 + 13 * n), hex(fid)
        recs = [d[12 + 13 * i:25 + 13 * i] for i in range(n)]
        chars = [r[0] for r in recs]
        assert len(set(chars)) == n
        for r in recs:
            u, v, w = struct.unpack_from("<HxxHxxH", r, 1)
            assert w > 0 and u < 256 and v < 256
    icons = {misc[0x7535][12 + 13 * i] for i in range(11)}
    assert {ord(c) for c in "BOCLTMcRPp"} <= icons          # reward letters, landed mark, replay icons
    small = {misc[0x7531][12 + 13 * i] for i in range(82)}
    assert {ord(c) for c in "SAVING GAME DATA.:'\"0123456789-"} <= small


def test_plyr_names_on_the_records_screens(misc):
    """The records and standings screens print zresGetPlayer(rider * 5) + 8: PLYR entry name."""
    p = dmparse.read_plyr(misc[1])
    names = [p["entries"][r * 5]["name"] for r in range(12)]
    assert names[0] == "Dave Mirra" and names[1] == "Ryan Nyquist" and len(set(names)) == 12
    t = text(OVERLAY)
    assert t.count("zresGetPlayer((uint16_t)(rec->rider * 5)) + 8") == 1
    assert t.count("zresGetPlayer((uint16_t)(rider * 5)) + 8") == 1


def test_save_writes_and_name_entry(img):
    """Where SAVE.DAT is written during play (memcard_WriteSave 0x00412340) and how the name entry
    copies beaten records (comdataGetRecordPtr 0x00407D20)."""
    callers = set()
    for f, (lo, hi) in RANGES.items():
        for m in re.finditer(rb"\xe8", img[lo - B:hi - B]):
            p = lo + m.start()
            tgt = p + 5 + struct.unpack_from("<i", img, p + 1 - B)[0]
            if tgt >= 0x401005 and tgt < 0x402620 and img[tgt - B] == 0xE9:
                tgt = tgt + 5 + struct.unpack_from("<i", img, tgt + 1 - B)[0]
            if tgt == 0x00412340:
                callers.add(p)
    funcs = sorted((int(r["va"], 16), int(r["size"]), r["name"]) for r in csv_rows() if r["file"] in RANGES)
    names = {n for va, size, n in funcs for c in callers if va <= c < va + size}
    want = {"overlayGoalsUpdate", "overlayResultsUpdate", "overlayRecordsUpdate", "overlayStandingsUpdate",
            "overlaySaveUpdate"}
    assert names == want
    t = text(OVERLAY)
    for n in want:
        body = t[t.index(n + "(void)\n{"):]
        assert "memcard_WriteSave()" in body[:body.index("\n}\n")], n
    # the name entry: up to 10 letters (cursor < 9 before the copy), records copied for stats 0..9
    code = img[0x00418870 - B:0x00418870 + 695 - B]
    assert b"\x80\x3d" + struct.pack("<I", 0x0056BF64) + b"\x09" in code   # cmp g_overlayNameCursor, 9
    assert b"\x3c\x20" in code and b"\x3c\x5a" in code and b"\x3c\x7a" in code   # ' ' A..Z a..z
    assert b"\x83\xfb\x0a" in code                                # cmp ebx, 10 (stats 0..9)
    assert "g_overlayNameCursor >= 9" in t and "stat < 10" in t

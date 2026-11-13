"""Self-consistency checks for the decomp of the ZGE object model (zobj.c, zobjCamera/Model/Light/PEmit.c),
zinput.c, zge.c, the PC platform (pcinput.c, pcmovie.c, pcProfile.c) and the remaining engine gaps
(ztoken.c, zFont.c, zrwRamDisk.c, zprofile.c, ztimer.c, zheap.c, plus additions to zasset.c, zmath.c,
zutil.c, zactor.c, zprim.c).

No C compiler on the build PC, so the C is checked the way the other decomp tests do it:
1. structure: every function of these files drafted, VAs / sizes / names agree with
   decomp/functions.csv and decomp/symbols/symbols.csv (last row wins), the absorbed gap files are
   gone, no Ghidra artefacts and no old names of these VAs anywhere in decomp/src; every new struct
   is size-checked in C, listed in ghidra_types.txt and has a struct row;
2. exe facts: every assert (message, file, line) is a call site in NgBMX.exe; every debug message
   literal exists; the four zobj vtables equal the C initialisers; the joystick map table, its
   names / product GUID / button pairs, the DirectInput GUIDs and the joystick properties
   (range -127..127, dead zone 1000, cooperative levels) read back from the exe; zprofileInit's
   layout defaults; the C runtime starts at _fpmath 0x004A4A20;
3. behaviour: a port of ztokenNext tokenizes Scene.Txt like the zScene.c test's stand-in; the movies in
   bin\\movie satisfy pcmovie's stream / buffer assumptions; the zheap port keeps heap order.
"""
import csv
import random
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
B = 0x400000
Z = "NgEngine/Zge/"
FULL = {   # file -> function count (every function in it is checked here)
    Z + "zobj.c": 33, Z + "ZgeMods/zobjCamera.c": 11, Z + "ZgeMods/zobjModel.c": 35,
    Z + "ZgeMods/zobjLight.c": 11, Z + "ZgeMods/zobjPEmit.c": 12, Z + "zinput.c": 6, Z + "zge.c": 5,
    Z + "Pc/pcinput.c": 11, Z + "Pc/pcmovie.c": 22, Z + "ztoken.c": 18, Z + "ZgeMods/zFont.c": 11,
    Z + "Com/ZglRw/zrwRamDisk.c": 13, Z + "zprofile.c": 15, Z + "ztimer.c": 9, Z + "Pc/pcProfile.c": 5,
    Z + "zheap.c": 9,
}
PARTIAL = {  # file -> the functions checked here, in a file another test covers
    Z + "zasset.c": {0x00480450, 0x00480BC0, 0x00480BE0},
    Z + "zmath.c": {0x00480C40, 0x00480C80, 0x00480CD0},
    Z + "zutil.c": {0x004A2C70, 0x004A2CA0, 0x004A2D20, 0x004A2DD0, 0x004A2E10},
    "zaxis/Bmx/Src/com/zge/zactor.c": {0x00475000},
    "zaxis/Bmx/Src/com/zge/zprim.c": {0x00479240, 0x00479260},
}
GAPS = ("gap_zprop__zobj", "gap_zobjLight__zScene", "gap_pcmovie__zpanorama", "gap_zpanorama__zFont",
        "gap_zutil__zprofile", "gap_zprofile__pcProfile", "gap_pcProfile__end")
HEADERS = ("zobj.h", "zpc.h", "ztools.h")
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")


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
                out[int(r["address"], 16) if kind != "struct" else r["name"]] = r
    return out


def zobj_pass_vas():
    vas = set()
    for f in FULL:
        vas |= {va for va, *_ in c_functions(f)}
    for s in PARTIAL.values():
        vas |= s
    return vas


def my_text(f):
    """The C of the checked functions in f (all of it for FULL files, plus the file's #defines)."""
    t = (SRC / f).read_text(encoding="utf-8")
    if f not in PARTIAL:
        return t
    defs = "".join(m.group(0) for m in re.finditer(r"#define \w+_FILE [^\n]+\n", t))
    return defs + "".join(m.group(0) for m in FUNC_RE.finditer(t) if int(m.group(1), 16) in PARTIAL[f])


FUNC_RE = re.compile(r"/\* VA 0x([0-9a-f]{8})[^\n]*\n(?:[^\n]*\n)*?\}\n")


# ------------------------------------------------------------------ structure
@pytest.mark.parametrize("f", sorted(FULL) + sorted(PARTIAL))
def test_every_function_drafted_and_named_consistently(f):
    rows = {int(r["va"], 16): r for r in csv_rows() if r["file"] == f}
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(rows), f
    if f in FULL:
        assert len(cf) == FULL[f]
        assert re.search(r"\* %d functions, VA 0x" % FULL[f], (SRC / f).read_text(encoding="utf-8")), f
    mine = PARTIAL.get(f)
    syms = last_symbols("func")
    sf = f[len("zaxis/"):] if f.startswith("zaxis/") else f
    for va, size, status, name in cf:
        r = rows[va]
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        if mine is None or va in mine:
            # the clean-up moved the ZglRw gap drafts into these files and created a few hole functions
            assert syms[va]["name"] == name, hex(va)
            assert syms[va]["source_file"] == sf, hex(va)
    if mine is not None:
        assert mine <= set(rows)
    assert "TODO(decomp)" not in (SRC / f).read_text(encoding="utf-8")


def test_only_zglrw_todo_left():
    todo = {r["file"] for r in csv_rows() if r["status"] == "todo"}
    assert all("zrw" in f for f in todo), todo
    for g in GAPS:
        assert not (SRC / "_unassigned" / (g + ".c")).exists(), g


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|PTR_\w+|[iu]Var\d+|"
                     r"pc?Var\d+|extraout_\w+|unaff_\w+|CONCAT\d+|h16_[0-9a-f]{8})\b")
    for f in list(FULL) + list(PARTIAL):
        t = (SRC / f).read_text(encoding="utf-8")
        if f in PARTIAL:   # only the checked functions of a shared file
            t = "".join(m.group(0) for m in re.finditer(r"/\* VA 0x([0-9a-f]{8})[^\n]*\n(?:[^\n]*\n)*?\}\n", t)
                        if int(m.group(1), 16) in PARTIAL[f])
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
    # no old names of these functions anywhere (FUN_ / file_xxxxxxxx / zobjLight_ ...)
    mine = zobj_pass_vas()
    old = re.compile(r"\b(?:FUN|zobj|zobjCamera|zobjLight|zobjModel|zobjPEmit|zinput|pcinput|pcmovie|zFont|"
                     r"zprofile|pcProfile|zge|h16|zprimStub)_([0-9a-f]{8})\b")
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        for m in old.finditer(p.read_text(encoding="utf-8")):
            assert int(m.group(1), 16) not in mine, (p, m.group(0))


def header_structs(h):
    t = (INC / h).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    return re.findall(r"^\} (\w+);\s+/\* (0x[0-9A-F]+)", t, flags=re.M)


def test_types_checked_everywhere():
    raw = (INC / "ghidra_types.txt").read_text(encoding="utf-8")
    want = {l.split()[1]: int(l.split()[2], 16) for l in raw.splitlines() if l.startswith("size ")}
    listed = raw.split()
    structs = last_symbols("struct")
    n = 0
    for h in HEADERS:
        assert h in listed and listed.index(h) > listed.index("ngif.h")
        t = (INC / h).read_text(encoding="utf-8")
        for name, size in header_structs(h):
            n += 1
            assert want[name] == int(size, 16), name
            assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (name, want[name]), t), name
            assert structs[name]["type"] == "decomp/include/" + h, name
        # ImportTypes drops the preprocessor: no macro array sizes in imported structs
        body = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
        assert not re.search(r"^\s+[\w\s\*]+\[[A-Z_]+\];", body, flags=re.M), h
    assert n == 21    # + ZObj, declared as struct ZObj { } (checked below)
    assert "sizeof(ZObj) == 0x3C" in (INC / "zobj.h").read_text(encoding="utf-8") and want["ZObj"] == 0x3C
    assert structs["ZObj"]["type"] == "decomp/include/zobj.h"


# ------------------------------------------------------------------ exe facts
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.exists():
        pytest.skip("no NgBMX.exe")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def push(v):
    if v < 0x80:
        return bytes([0x6A, v])
    return b"\x68" + struct.pack("<I", v)


def cstr(img, s):
    return img.find(s.encode("latin1") + b"\0")


def unescape(s):
    return s.encode("latin1").decode("unicode_escape")


ALL_FILES = list(FULL) + list(PARTIAL)


def test_assert_sites_exist_in_exe(img):
    n = 0
    for f in ALL_FILES:
        t = my_text(f)
        macros = dict(re.findall(r'#define (\w+_FILE) "([^"]+)"', t))
        for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", (\w+_FILE), (0x[0-9A-Fa-f]+)', t):
            msg, path, line = unescape(m.group(1)), unescape(macros[m.group(2)]), int(m.group(3), 16)
            pa = cstr(img, path)
            assert pa > 0, path
            hits = [a.start() for a in re.finditer(re.escape(msg.encode("latin1") + b"\0"), img)]
            pats = [push(line) + b"\x68" + struct.pack("<I", pa + B) + b"\x68" + struct.pack("<I", a + B)
                    for a in hits]
            assert any(img.find(p) > 0 for p in pats), (f, msg, hex(line))
            n += 1
    assert n == 31


def test_debug_messages_exist(img):
    for f in ALL_FILES:
        t = my_text(f)
        for m in re.finditer(r'g_pfnDebugPrintf\("((?:[^"\\]|\\.)*)"', t):
            assert cstr(img, unescape(m.group(1))) > 0, (f, m.group(1))


def exe_dword(img, va):
    return struct.unpack_from("<I", img, va - B)[0]


def test_vtables_match_exe(img):
    names = {r["name"]: int(r["va"], 16) for r in csv_rows()}
    for f, var, va in ((Z + "ZgeMods/zobjCamera.c", "g_zobjCameraVtbl", 0x00560518),
                       (Z + "ZgeMods/zobjModel.c", "g_zobjModelAnimVtbl", 0x00560FA8),
                       (Z + "ZgeMods/zobjModel.c", "g_zobjModelVtbl", 0x00560FD0),
                       (Z + "ZgeMods/zobjLight.c", "g_zobjLightVtbl", 0x00561858),
                       (Z + "ZgeMods/zobjPEmit.c", "g_zobjPEmitVtbl", 0x00562098)):
        t = (SRC / f).read_text(encoding="utf-8")
        body = re.search(r"const ZobjVtbl %s = \{\s*/\* 0x%08X \*/(.*?)\};" % (var, va), t, re.S).group(1)
        slots = [s.strip() for s in body.split(",")]
        assert len(slots) == 9, var
        for i, s in enumerate(slots):
            want = 0 if s == "0" else int(s[4:], 16) if s.startswith("FUN_") else names[s]
            assert exe_dword(img, va + 4 * i) == want, (var, i, s)
        assert last_symbols("data")[va]["name"] == var


def test_joystick_maps(img):
    def strs(va):
        out = []
        while exe_dword(img, va):
            a = exe_dword(img, va) - B
            out.append(bytes(img[a:img.index(b"\0", a)]).decode())
            va += 4
        return out

    def pairs(va):
        out, a = {}, va - B
        while img[a] != 0xFF:
            out[img[a + 1]] = img[a] + 1          # channel -> 1-based button
            a += 2
        return out

    assert [exe_dword(img, 0x00561F2C + 4 * i) for i in range(3)] == [0x00561ED8, 0x00561F10, 0]
    assert exe_dword(img, 0x00561EA8) == 0 and exe_dword(img, 0x00561EAC) == 0     # default: no names
    assert strs(exe_dword(img, 0x00561ED8)) == ["Gravis GrIP", "Gravis GamePad Pro"]
    assert exe_dword(img, 0x00561EDC) == 0
    assert strs(exe_dword(img, 0x00561F10)) == ["Microsoft SideWinder game pad",
                                                 "Microsoft SideWinder (Auto Detect)"]
    prod = exe_dword(img, 0x00561F14)
    assert exe_dword(img, prod + 4) == 0 and exe_dword(img, prod) == 0x0054E0F0
    # channel -> PSX button (contrler.h poll table): 8 Tri, 9 Sq, 10 Ci, 11 X, 14 L1, 15 R1,
    # 16 L2, 17 R2, 20 Start, 21 Select; the values are docs/re/input-camera.md 2.1's table
    default = {8: 4, 9: 1, 10: 3, 11: 2, 14: 5, 15: 6, 16: 7, 17: 8, 20: 10, 21: 9}
    assert pairs(0x00561EB0) == default
    assert pairs(0x00561EE0) == default
    assert pairs(0x00561F18) == {8: 5, 9: 4, 10: 2, 11: 1, 14: 7, 15: 8, 20: 9, 21: 10}
    # the C picks the maps from this table
    t = (SRC / Z / "Pc/pcinput.c").read_text(encoding="utf-8")
    assert "g_pcinputJoyMaps" in t and "tszProductName" in t and "guidProduct" in t


def guid(img, va):
    d1, d2, d3 = struct.unpack_from("<IHH", img, va - B)
    return "%08X-%04X-%04X-%s" % (d1, d2, d3, bytes(img[va - B + 8:va - B + 16]).hex().upper())


def test_directinput_facts(img):
    assert guid(img, 0x0054F2F8) == "6F1D2B61-D5A0-11CF-BFC7444553540000"        # GUID_SysKeyboard
    assert guid(img, 0x0054F448) == "9A4CB684-236D-11D3-8E9D00C04F6844AE"        # IID_IDirectInput7A
    assert guid(img, 0x0054F408) == "5944E682-C92E-11CF-BFC7444553540000"        # IID_IDirectInputDevice2A
    code = bytes(img[0x00493310 - B:0x004937F0 - B])
    # DIPROPRANGE {0x18, 0x10, .., DIPH_BYOFFSET, -127, 127} and DIPROPDWORD dead zone 1000
    assert b"\xc7\x44\x24\x3c\x18\x00\x00\x00" in code and b"\x81\xff\xff\xff" in code
    assert b"\x7f\x00\x00\x00" in code and b"\xe8\x03\x00\x00" in code
    assert b"\x6a\x06" in code and b"\x6a\x05" in code                             # cooperative levels
    t = (SRC / Z / "Pc/pcinput.c").read_text(encoding="utf-8")
    for s in ("range.lMin = -127", "range.lMax = 127", "dead.dwData = 1000",
              "DISCL_NONEXCLUSIVE | DISCL_FOREGROUND", "DISCL_EXCLUSIVE | DISCL_FOREGROUND"):
        assert s in t, s


def test_zprofile_defaults(img):
    code = bytes(img[0x004A3B60 - B:0x004A3CB0 - B])
    # the immediate stores (8, 2 and 4 go through registers)
    layout = {0x00676470: 0x20, 0x00676480: 6, 0x00676498: 0x78, 0x0067649C: 0x3C, 0x00676444: 10,
              0x00676450: 0x42000000, 0x00676454: 0x42000000, 0x00676458: 0x40000000}
    for va, v in layout.items():
        assert b"\xc7\x05" + struct.pack("<II", va, v) in code, hex(va)
    t = (SRC / Z / "zprofile.c").read_text(encoding="utf-8")
    for s in ("margin = 0x20", "barHeight = 8", "fillHeight = 6", "peakPeriod = 0x78", "fps = 0x3C",
              "peakDelay = 10", "x = 32.0f"):
        assert s in t, s


def test_crt_starts_at_fpmath(img):
    import re as _re
    t = (REPO / "tools" / "ghidra" / "build_symbols.py").read_text(encoding="utf-8")
    assert _re.search(r"\(0x004A4A20, 0x004AF9D0, \"lib:msvcrt\"", t)
    syms = last_symbols("func")
    assert syms[0x004A4A20]["name"] == "_fpmath" and syms[0x004A4A20]["source_file"] == "lib:msvcrt"
    assert img[0x004A4A20 - B] == 0xE8 and bytes(img[0x004A4A34 - B:0x004A4A38 - B]) == b"\xdb\xe2\xc3\xc3"
    assert max(int(r["va"], 16) for r in csv_rows()) < 0x004A4A20


# ------------------------------------------------------------------ behaviour
def port_tokens(text):
    """Port of ztokenNext (capitalize on, preprocess on, no hash comments) over a whole file."""
    lines = [l + "\n" for l in text.split("\n")]
    sep = " \t\r\n,"
    syms, out = {}, []
    li, pos = 0, 0
    line = lines[0] if lines else ""
    depth = 0

    def next_raw():
        nonlocal li, pos, line
        while True:
            while pos < len(line) and line[pos] in sep:
                pos += 1
            if pos < len(line):
                break
            li += 1
            if li >= len(lines):
                return None, False
            line, pos = lines[li], 0
        if line[pos] == '"':
            end = line.index('"', pos + 1)
            tok, pos = line[pos + 1:end], end + 1
            return tok, True
        start = pos
        while pos < len(line) and line[pos] not in sep:
            pos += 1
        return line[start:pos], False

    def skip_line():
        nonlocal li, pos, line
        li += 1
        line, pos = (lines[li], 0) if li < len(lines) else ("", 0)

    while True:
        tok, quoted = next_raw()
        if tok is None:
            break
        if not quoted and (tok.startswith(";") or tok.startswith("//")):
            skip_line()
            continue
        prev = depth
        if not quoted and tok == "/*":
            depth += 1
        elif not quoted and tok == "*/":
            depth -= 1
        if depth > 0 or prev == 1:
            continue
        if not quoted:
            tok = tok.upper()
        if tok == "#DEFINE":
            name, _ = next_raw()
            value, _ = next_raw()
            syms["$" + name.upper()] = value.upper()
            continue
        if tok.startswith("$") and not quoted:
            tok = syms[tok]
        out.append(tok)
    return out


def test_ztoken_port_matches_stand_in_on_scene_txt():
    p = GAME_DIR / "Scene.Txt"
    if not p.exists():
        pytest.skip("no Scene.Txt")
    from test_decomp_zscene import c_tokens
    text = p.read_text(encoding="latin1")
    mine = port_tokens(text)
    theirs = c_tokens(text)
    assert len(mine) == len(theirs) > 100
    for a, b in zip(mine, theirs):
        assert a == b or a == b.upper(), (a, b)


def avi_streams(path):
    d = path.read_bytes()
    assert d[:4] == b"RIFF" and d[8:12] == b"AVI "

    def chunks(off, end):
        while off + 8 <= end:
            cid, size = struct.unpack_from("<4sI", d, off)
            yield cid, off + 8, size
            off += 8 + size + (size & 1)

    out = []
    for cid, o, s in chunks(12, len(d)):
        if cid == b"LIST" and d[o:o + 4] == b"hdrl":
            for c2, o2, s2 in chunks(o + 4, o + s):
                if c2 == b"LIST" and d[o2:o2 + 4] == b"strl":
                    st = {}
                    for c3, o3, s3 in chunks(o2 + 4, o2 + s2):
                        if c3 == b"strh":
                            st["type"] = d[o3:o3 + 4]
                            st["sampleSize"] = struct.unpack_from("<I", d, o3 + 44)[0]
                        if c3 == b"strf" and st.get("type") == b"vids":
                            st["w"], st["h"], _, st["bits"], st["comp"] = struct.unpack_from("<iiHHI", d, o3 + 4)
                        if c3 == b"strf" and st.get("type") == b"auds":
                            st["tag"], st["ch"], st["rate"], _, _, st["bits"] = struct.unpack_from("<HHIIHH", d, o3)
                    out.append(st)
    return out


def test_movies_fit_pcmovie():
    movies = sorted((GAME_DIR / "bin" / "movie").glob("*.avi")) if (GAME_DIR / "bin" / "movie").exists() else []
    if not movies:
        pytest.skip("no movies")
    assert [m.stem.lower() for m in movies] == ["acclaim", "amish", "credits", "intro", "mirra", "nyquist", "zaxis"]
    for m in movies:
        st = avi_streams(m)
        assert 1 <= len(st) <= 8, m.name                              # PcMovie.stream[8]
        v = [s for s in st if s["type"] == b"vids"]
        a = [s for s in st if s["type"] == b"auds"]
        assert st[0]["type"] == b"vids" and len(v) == 1 and len(a) == 1, m.name
        assert (v[0]["w"], v[0]["h"], v[0]["bits"]) == (320, 240, 24) and v[0]["comp"] == 0x64697663  # 'cvid'
        assert a[0]["tag"] == 1 and a[0]["bits"] == 16                 # PCM
        assert 0 < a[0]["sampleSize"] <= 0x8000                        # pcmovieOpenAudio's window
        assert 0x8000 // a[0]["sampleSize"] * a[0]["sampleSize"] <= 0x8000   # one buffer per read


class Heap:
    """Port of zheap.c (min-heap mode)."""

    def __init__(self, cap):
        self.items, self.count, self.cap = [(0, None)] + [None] * max(cap, 10), 0, max(cap, 10)

    def insert(self, key, value):
        if self.count == self.cap:
            return 0
        self.count += 1
        hole = self.count
        while key < self.items[hole // 2][0]:
            self.items[hole] = self.items[hole // 2]
            hole //= 2
        self.items[hole] = (key, value)
        return 1

    def pop(self):
        if self.count == 0:
            return None
        top, last = self.items[1], self.items[self.count]
        self.count -= 1
        hole = 1
        while hole * 2 <= self.count:
            c = hole * 2
            if c != self.count and self.items[c + 1][0] < self.items[c][0]:
                c += 1
            if last[0] <= self.items[c][0]:
                break
            self.items[hole] = self.items[c]
            hole = c
        self.items[hole] = last
        return top


def test_zheap_port_orders():
    rnd = random.Random(61)
    h = Heap(64)
    keys = [rnd.randrange(1, 10000) for _ in range(64)]
    for k in keys:
        assert h.insert(k, k)
    assert h.insert(1, 1) == 0                       # full
    assert [h.pop()[0] for _ in range(64)] == sorted(keys)
    t = (SRC / Z / "zheap.c").read_text(encoding="utf-8")
    assert "capacity < 10" in t and "0xFFFFFFFFu" in t

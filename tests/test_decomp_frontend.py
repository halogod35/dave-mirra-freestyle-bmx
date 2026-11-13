"""Self-consistency checks for the decomp of the front end (frontend.c, gui/guiinput.c, guimain.c, guisfile.c,
guitoggl.c and the gap before guiinput.c).

1. structure: every function of these files is drafted (VA order, names = functions.csv = symbols.csv
   rows), the two old gap files are gone, no Ghidra artefacts and no generated names left in
   decomp/, gui.h layouts agree with their "+0x.." comments, the sizeof checks and ghidra_types.txt;
2. against the exe: the assert sites (message, file, line) of guiinput.c / guitoggl.c / frontend.c, the
   GUIMAIN / GUISFILE / GUITOGGL / FRONTEND messages, the 95 functions the symbols.csv rows create (ILT targets after
   padding), guiCreateElement's type handlers, the data tables the C initialises (main menu, language
   codes, carousel, colour bars, movies, tier names), the cheat-code table built on the stack, and the
   save-block addresses the unlock gates read (cross-checked with tools/dmbmx/formats/save_fields.py);
3. against the data: the SCRN screens parsed with dmbmx.formats.scrn match what guiLoadScreenFile reads
   (asset references of every EDEF resolve in the groups the frontend loads), and every text id the
   frontend uses resolves in its STRS set.
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

GAP = "_unassigned/gap_rider__guiinput.c"
INPUT = "zaxis/Bmx/Src/com/menu/gui/guiinput.c"
MAIN = "zaxis/Bmx/Src/com/menu/gui/guimain.c"
SFILE = "zaxis/Bmx/Src/com/menu/gui/guisfile.c"
TOGGL = "zaxis/Bmx/Src/com/menu/gui/guitoggl.c"
FRONT = "zaxis/Bmx/Src/com/menu/frontend.c"
COUNTS = {GAP: 15, INPUT: 20, MAIN: 86, SFILE: 1, TOGGL: 14,  # the bogus 0x00439FA1 was removed
          FRONT: 136}
RANGES = {GAP: (0x00436520, 0x00436C20), INPUT: (0x00436C20, 0x00437460), MAIN: (0x00437460, 0x00439A00),
          SFILE: (0x00439A00, 0x00439F80), TOGGL: (0x00439F80, 0x0043BE90), FRONT: (0x0043BE90, 0x0044DD30)}
SOURCE = {GAP: "", INPUT: "Bmx/Src/com/menu/gui/guiinput.c", MAIN: "Bmx/Src/com/menu/gui/guimain.c",
          SFILE: "Bmx/Src/com/menu/gui/guisfile.c", TOGGL: "Bmx/Src/com/menu/gui/guitoggl.c",
          FRONT: "Bmx/Src/com/menu/frontend.c"}
PREFIX = {GAP: "gui", INPUT: "guiInput", MAIN: "gui", SFILE: "gui", TOGGL: "gui", FRONT: "frontend"}
PATHS = {INPUT: 0x00559D14, TOGGL: 0x0055A2AC, FRONT: 0x0055B4C0}
# the 95 functions created by symbols.csv rows (not functions in the Ghidra project before)
CREATED = [
    0x00436AA0, 0x00436B20, 0x00436B40, 0x00436B80, 0x00436BE0, 0x00436C00, 0x004373E0, 0x00437460,
    0x004374E0, 0x00437510, 0x00437540, 0x00437570, 0x00437610, 0x00438380, 0x00438410, 0x004387B0,
    0x00438820, 0x00438860, 0x00438880, 0x00438900, 0x004389B0, 0x00438B40, 0x00438BB0, 0x00438D40,
    0x00439090, 0x004390B0, 0x00439130, 0x004394E0, 0x00439800, 0x004399A0, 0x004399D0, 0x0043A840,
    0x0043AED0, 0x0043B660, 0x0043BD90, 0x0043CD80, 0x0043CF10, 0x0043CF40, 0x0043F9B0, 0x0043FB80,
    0x00440D80, 0x00440EB0, 0x00440FD0, 0x004410B0, 0x00441180, 0x00441860, 0x00441BD0, 0x00442730,
    0x00442A00, 0x00442B70, 0x00442C40, 0x00442CE0, 0x004433E0, 0x00443420, 0x00443830, 0x00443870,
    0x00443AF0, 0x00443CB0, 0x00443E30, 0x00443E70, 0x00444110, 0x00444140, 0x00444AE0, 0x00445090,
    0x00445290, 0x00445760, 0x00445B00, 0x00445D90, 0x00446280, 0x00446730, 0x004476C0, 0x00447BF0,
    0x00448A80, 0x004496A0, 0x004498D0, 0x00449A30, 0x00449A80, 0x0044A7B0, 0x0044A870, 0x0044AA30,
    0x0044AC00, 0x0044AE30, 0x0044B340, 0x0044B460, 0x0044BCD0, 0x0044BEA0, 0x0044C0A0, 0x0044C1C0,
    0x0044C390, 0x0044CA00, 0x0044CA40, 0x0044CC70, 0x0044D2A0, 0x0044D5A0, 0x004420A0,
]
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


def symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        for r in csv.DictReader(fh):
            if r["kind"] == kind:
                out[int(r["address"], 16)] = r
    return out


def names_by_va():
    return {int(r["va"], 16): r["name"] for r in csv_rows()}


# ------------------------------------------------------------------ structure
@pytest.mark.parametrize("f", list(COUNTS))
def test_every_function_drafted_and_named_consistently(f):
    rows = [r for r in csv_rows() if r["file"] == f]
    assert len(rows) == COUNTS[f]
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = symbols("func")
    lo, hi = RANGES[f]
    for va, size, status, name in cf:
        r = by_va[va]
        assert lo <= va < hi
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, hex(va)
        assert syms[va]["source_file"] == SOURCE[f]
        assert re.match(PREFIX[f] + r"[A-Z]\w+$", name), name
    for gone in ("gap_guiinput__guimain.c", "gap_guimain__guitoggl.c", "gap_guisfile__guitoggl.c"):
        assert not (SRC / "_unassigned" / gone).exists()


OLD = re.compile(r"\b(guimain_[0-9a-f]{8}|guiinput_[0-9a-f]{8}|guitoggl_[0-9a-f]{8}|frontend_[0-9a-f]{8}|"
                 r"frontend_(BuildCheatList|ShowStyleItem|RiderSelectCheats|BuildMovieList))\b")


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|_DAT_\w+)\b")
    allowed = {"ngappPlayMovie", "ngappEmptyHook", "zcardSelectPort", "zcardFormat", "zcardStart", "zcardStop"}
    for f in COUNTS:
        t = text(f)
        assert "TODO(decomp)" not in t
        assert "\0" not in t and "\r" not in t
        for n, line in enumerate(t.splitlines(), 1):
            for m in bad.finditer(line):
                assert m.group(0) in allowed, f"{f}:{n}: {line.strip()}"
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        assert not OLD.search(p.read_text(encoding="utf-8")), p


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}
FNPTR = {"GuiHandler", "GuiTask"}


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
                    ptr, name, dims = re.match(r"(\**)\s*(\w+)((?:\[\w+\])*)$", dec).groups()
                    size = align = 4 if (ptr or typ in FNPTR) else SIZES[typ]
                    count = 1
                    for n in re.findall(r"\[(\w+)\]", dims):
                        count *= int(n, 0)
                    off = (off + align - 1) // align * align
                    if k == 0:
                        assert comment is None or int(comment, 16) == off, (m.group(3), name, hex(off))
                    fields[name] = off
                    off += size * count
                    maxal = max(maxal, align)
        out[m.group(3)] = ((off + maxal - 1) // maxal * maxal, fields)
    return out


def test_struct_layouts():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    lay = layouts("gui.h")
    sized = {"GuiElement": 0xF0, "GuiToggleData": 0x24, "GuiStyle": 0x18, "GuiPad": 0x20, "GuiScrnHeader": 0x2C,
             "GuiEdef": 0xD4}
    for n, size in sized.items():
        assert lay[n][0] == size == want[n], n
    t = (INC / "gui.h").read_text(encoding="utf-8")
    for n in sized:
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, sized[n]), t), n
    assert "gui.h" in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    e = lay["GuiElement"][1]
    assert (e["sprite"], e["animFrames"], e["animTimes"], e["text"], e["handler"], e["typeHandler"], e["data"]) == \
        (0x34, 0x50, 0x8C, 0xD8, 0xE4, 0xE8, 0xEC)
    d = lay["GuiEdef"][1]
    assert (d["priority"], d["animFrames"], d["selSprite"], d["text"], d["toggleText"], d["toggleSpacing"]) ==         (0x22, 0x2A, 0xA2, 0xB4, 0xC8, 0xD0)
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        snames = {r["name"] for r in csv.DictReader(fh) if r["kind"] == "struct" and r["type"] == "decomp/include/gui.h"}
    assert snames == set(sized)


def test_scrn_codec_matches_gui_h():
    """dmbmx.formats.scrn uses the offsets of gui.h's GuiEdef / GuiScrnHeader"""
    from dmbmx.formats import scrn
    lay = layouts("gui.h")
    camel = {"sprite_param": "spriteParam", "scale_x": "scaleX", "scale_y": "scaleY", "slide_x": "slideX",
             "slide_y": "slideY", "anim_count": "animCount", "anim_frames": "animFrames", "anim_times": "animTimes",
             "sel_sprite": "selSprite", "sprite_flags": "spriteFlags", "sprite_x": "spriteX", "sprite_y": "spriteY",
             "text_flags": "textFlags", "text_x": "textX", "text_y": "textY", "text_rgb": "textRGB",
             "toggle_font": "toggleFont", "toggle_flags": "toggleFlags", "toggle_x": "toggleX", "toggle_y": "toggleY",
             "toggle_text": "toggleText", "toggle_rgb": "toggleRGB", "toggle_spacing": "toggleSpacing"}
    for name, _, off in scrn.EDEF_FIELDS:
        assert lay["GuiEdef"][1][camel.get(name, name)] == off, name
    hcamel = {"edef_count": "edefCount", "edef_offset": "edefOffset", "text_count": "textCount",
              "strs_offset": "strsOffset", "focus_font": "focusFont", "focus_rgb": "focusRGB",
              "blur_font": "blurFont", "blur_rgb": "blurRGB"}
    for name, _, off in scrn.HEADER_FIELDS:
        assert lay["GuiScrnHeader"][1][hcamel.get(name, name)] == off, name
    assert (scrn.HEADER, scrn.EDEF_SIZE) == (0x2C, 0xD4)


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


def thunk_targets(img):
    out = {}
    for p in range(0x401005, 0x402616, 5):
        if img[p - B] == 0xE9:
            out[p] = p + 5 + struct.unpack_from("<i", img, p + 1 - B)[0]
    return out


def unescape(s):
    return s.encode("latin-1").decode("unicode_escape")


def exe_asserts(img, f):
    lo, hi = RANGES[f]
    out = set()
    pat = b"\x68" + struct.pack("<I", PATHS[f])
    for m in re.finditer(re.escape(pat), img[lo - B:hi - B]):
        p = lo - B + m.start()
        assert img[p + 5] == 0x68
        line = img[p - 1] if img[p - 2] == 0x6A else u32(img, p - 4 + B)
        out.add((cstr(img, u32(img, p + 6 + B)), line))
    return out


def c_asserts(f):
    t = text(f)
    out = {(unescape(m.group(1)), int(m.group(2), 16))
           for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", \w+_SRC, (0x[0-9A-Fa-f]+)', t)}
    for m in re.finditer(r"lockData\(e, (0x[0-9A-Fa-f]+)\)", t):
        out.add(("GUITOGGL: Failed to lock toggle data\n", int(m.group(1), 16)))
    for m in re.finditer(r"drawToggleText\(e, a, (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)", t):
        out.add(("GUITOGGL: Failed to lock toggle data\n", int(m.group(1), 16)))
        out.add(("GUITOGGL: Failed to lock toggle text\n", int(m.group(2), 16)))
    return out


@pytest.mark.parametrize("f", [INPUT, TOGGL, FRONT])
def test_assert_sites_match_exe(img, f):
    exe, c = exe_asserts(img, f), c_asserts(f)
    assert cstr(img, PATHS[f]).replace("\\", "\\\\") in text(f)
    assert c == exe, (sorted(c - exe), sorted(exe - c))
    assert len(exe) == {INPUT: 2, TOGGL: 19, FRONT: 3}[f]


def test_messages_are_in_the_exe(img):
    """every GUIMAIN / GUISFILE / GUITOGGL / FRONTEND / APP message the C prints exists in .data"""
    n = 0
    for f in COUNTS:
        for m in re.finditer(r'g_pfnDebugPrintf\("((?:[^"\\]|\\.)*)"', text(f)):
            s = unescape(m.group(1))
            assert s.encode("latin-1") + b"\0" in img, s
            n += 1
        for m in re.finditer(r'zconsoleLog\("((?:[^"\\]|\\.)*)"', text(f)):
            assert unescape(m.group(1)).encode() + b"\0" in img
    assert n >= 30
    assert img[0x0055A210 - B:0x0055A214 - B] == b"SCRN" and img[0x0055A1DC - B:0x0055A1E0 - B] == b"EDEF"
    assert cstr(img, u32(img, 0x00559D48)) == "intart.zal"          # g_guiArtLibrary
    assert struct.unpack_from("<i", img, 0x00559D4C - B)[0] == 1     # g_guiPulseStep
    assert struct.unpack_from("<i", img, 0x0055A354 - B)[0] == 2     # g_feCursorStep


def test_created_functions_are_thunk_targets(img):
    """the 95 created functions are /INCREMENTAL thunk targets (0x004420A0 is pushed directly), each
    16-byte aligned after padding or a ret; decomp/symbols/symbols.csv names them, the export has them"""
    assert len(CREATED) == len(set(CREATED)) == 95
    syms = symbols("func")
    ilt = set(thunk_targets(img).values())
    with open(REPO / "decomp" / "symbols" / "functions.csv", encoding="utf-8", newline="") as fh:
        fc = {int(r["va"], 16) for r in csv.DictReader(fh)}
    for va in CREATED:
        assert va in syms and va in fc, hex(va)
        assert va in ilt or va == 0x004420A0, hex(va)
        assert va % 16 == 0 and img[va - 1 - B] in (0x90, 0xCC, 0xC3), hex(va)
    assert bytes([0x68]) + struct.pack("<I", 0x004420A0) in img[0x00441CB0 - B:0x00441F80 - B]


def test_create_element_type_handlers(img):
    """guiCreateElement 0x00438470 stores one type handler per type into +0xE8 (mov [reg+0xE8], imm)"""
    code = img[0x00438470 - B:0x00438470 + 283 - B]
    th = thunk_targets(img)
    got = set()
    for m in re.finditer(rb"\xc7[\x40-\x47\x80-\x87]\xe8\x00\x00\x00(....)", code, re.S):
        v = struct.unpack("<I", m.group(1))[0]
        got.add(th.get(v, v))
    va = {n: v for v, n in names_by_va().items()}
    t = text(MAIN)
    sw = t[t.index("GuiElement *guiCreateElement(uint16_t type"):]
    sw = sw[:sw.index("\n}\n")]
    names = set(re.findall(r"typeHandler = (?:\(GuiHandler\))?(\w+);", sw))
    assert got == {va[n] for n in names} == {0x00439F80, 0x00436520, 0x0043A840, 0x00436AE0, 0x00437460, 0x004398D0}


def c_array(t, name):
    body = re.search(name + r"(?:\[[^\]]*\])+ = \{(.*?)\};", t, re.S).group(1)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", re.sub(r"/\*.*?\*/", "", body, flags=re.S))]


def test_data_tables_match_exe(img):
    t = text(FRONT)
    assert c_array(t, "g_feMainItems") == list(struct.unpack_from("<6H", img, 0x0055A35C - B))
    assert c_array(t, "g_feLanguageCodes") == list(img[0x0055A358 - B:0x0055A35C - B])
    assert c_array(t, "g_feMainRowPos") == list(struct.unpack_from("<14h", img, 0x0055A368 - B))
    assert c_array(t, "g_feMainRowAlpha") == list(struct.unpack_from("<8h", img, 0x0055A384 - B))
    assert c_array(t, "g_feMainRowGrey") == list(struct.unpack_from("<8h", img, 0x0055A394 - B))
    assert c_array(t, "g_feOnOff") == list(struct.unpack_from("<2I", img, 0x0055A3B0 - B))
    assert c_array(t, "g_feColorBars") == list(img[0x0055A3DC - B:0x0055A3F4 - B])
    assert c_array(t, "g_feTierNames") == list(struct.unpack_from("<3H", img, 0x0055B130 - B))
    sel = [(int(m, 16)) for m in re.findall(r"SEL\((0x[0-9A-F]+)\)", re.search(
        r"g_feCarouselSprite\[3\]\s*=\s*\{([^}]*)\}", t).group(1))]
    assert [0xA0090000 | n for n in sel] == list(struct.unpack_from("<3I", img, 0x0054D224 - B))
    sel = [(int(m, 16)) for m in re.findall(r"SEL\((0x[0-9A-F]+)\)", re.search(
        r"g_feCarouselText\[3\]\s*=\s*\{([^}]*)\}", t).group(1))]
    assert [0xA0090000 | n for n in sel] == list(struct.unpack_from("<3I", img, 0x0054D230 - B))
    # the movies: 4 x (title[100], path, 320, 240, ...) at 0x0055AF40
    movies = [(cstr(img, 0x0055AF40 + 0x7C * k), cstr(img, u32(img, 0x0055AF40 + 0x7C * k + 0x64)))
              for k in range(4)]
    assert movies == [("INTRO MOVIE", "bin\\movie\\Intro.str"), ("DAVE MIRRA", "bin\\movie\\Mirra.str"),
                      ("RYAN NYQUIST", "bin\\movie\\Nyquist.str"), ("AMISH BOY", "bin\\movie\\amish.str")]
    code = img[0x0044D080 - B:0x0044D080 + 421 - B]
    for v in (0x140, 0xF0, 0xD15, 0x18):
        assert struct.pack("<I", v) in code
    assert "credits.str" in t and b"bin\\movie\\credits.str\0" in img
    # the cheat menu names (table 0x0055A3BC: u16 text, u8 bit)
    bits = [img[0x0055A3BC + 4 * k + 2 - B] for k in range(8)]
    assert bits == list(range(8))


def test_cheat_codes_match_exe(img):
    """frontendSelectCheats builds 6 x 2 x 16 int16 codes on the stack (mov word [esp+d], imm)"""
    capstone = pytest.importorskip("capstone")
    from capstone import x86
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    code = bytes(img[0x00446AC0 - B:0x00446AC0 + 2113 - B])
    regs, stores = {}, {}
    full = {"ax": "eax", "bx": "ebx", "cx": "ecx", "dx": "edx", "si": "esi", "di": "edi", "bp": "ebp"}
    for ins in md.disasm(code, 0x00446AC0):
        ops = ins.operands
        if ins.mnemonic == "xor" and len(ops) == 2 and ops[0].type == ops[1].type == x86.X86_OP_REG \
                and ops[0].reg == ops[1].reg:
            regs[ins.reg_name(ops[0].reg)] = 0
        elif ins.mnemonic == "mov" and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_IMM:
            regs[ins.reg_name(ops[0].reg)] = ops[1].imm & 0xFFFF
        elif ins.mnemonic == "mov" and ops[0].type == x86.X86_OP_MEM and ops[0].size == 2 \
                and ins.reg_name(ops[0].mem.base) == "esp":
            if ops[1].type == x86.X86_OP_IMM:
                v = ops[1].imm & 0xFFFF
            else:
                r = ins.reg_name(ops[1].reg)
                v = regs.get(r, regs.get(full.get(r, r)))
            stores[ops[0].mem.disp] = v
        if ins.mnemonic == "call":
            break
    base = min(d for d, v in stores.items() if v == 0x1F)           # the first code starts with Down
    table = [stores.get(base + 2 * k) for k in range(0xC0)]
    flat = []
    t = text(FRONT)
    body = re.search(r"s_cheatCodes\[6\]\[2\]\[16\] = \{(.*?)\n\};", t, re.S).group(1)
    for seq in re.findall(r"\{\s*(0x[^{}]*)\}", re.sub(r"/\*.*?\*/", "", body, flags=re.S)):
        vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", seq)]
        flat += vals + [0] * (16 - len(vals))
    assert len(flat) == 0xC0
    assert flat == table
    # pad-2 variant = pad-1 + 13 for every code
    for c in range(6):
        a, b = flat[32 * c:32 * c + 16], flat[32 * c + 16:32 * c + 32]
        assert [x + 13 if x else 0 for x in a] == b
    # the effects: 11 / 12 riders, 5 bikes, 5 styles, 12 levels, in the order of the codes
    eff = re.findall(r"(riders_unlocked|bikes_unlocked|styles_unlocked|levels_unlocked) = (\d+);",
                     t[t.index("void frontendSelectCheats"):])
    assert eff == [("riders_unlocked", "11"), ("riders_unlocked", "12"), ("bikes_unlocked", "5"),
                   ("styles_unlocked", "5"), ("levels_unlocked", "12")]


def test_unlock_gates_read_the_save_fields(img):
    """the select pages read the career record bytes save_fields.py names (file offset = addr - 0x006955E0 + 8)"""
    from dmbmx.formats import save_fields as SF
    defs = {d.pattern: d for d in SF.FIELDS}
    rec = 0x006955E8
    checks = [(0x00445760, rec + defs["rider.<r>.levels_unlocked"].off, "levels_unlocked"),
              (0x00445D90, rec + defs["rider.<r>.styles_unlocked"].off, "styles_unlocked"),
              (0x00446730, rec + defs["rider.<r>.bikes_unlocked"].off, "bikes_unlocked"),
              (0x00444AE0, 0x006955E0 + defs["riders_unlocked"].off, "riders_unlocked"),
              (0x00440DC0, 0x006955E0 + defs["cheats_unlocked"].off, "cheats_unlocked"),
              (0x00441180, 0x006955E0 + defs["cheats_active"].off, "cheats_active")]
    rows = {int(r["va"], 16): r for r in csv_rows()}
    t = text(FRONT)
    for va, addr, field in checks:
        size = int(rows[va]["size"])
        body = img[va - B:va + size - B]
        assert struct.pack("<I", addr) in body, (hex(va), field)
        name = rows[va]["name"]
        fb = t[t.index("/* VA 0x%08x" % va):]
        assert field in fb[:fb.index("\n}\n")], (name, field)
    assert defs["rider.<r>.levels_unlocked"].off == 4 and defs["riders_unlocked"].off == 7
    # audio: MUSIC = volume_0d (channel 1), SOUND FX = volume_0c (channel 0)
    assert defs["volume_0c"].off == 4 and defs["volume_0d"].off == 5
    assert "g_saveBlockA.volume_0d = (uint8_t)(g_feMusicVol" in t and "g_saveBlockA.volume_0c = (uint8_t)(g_feSfxVol" in t
    # leaving OPTIONS writes SAVE.DAT; the main menu's first entry reads it
    th = thunk_targets(img)

    def calls(va, target):
        size = int(rows[va]["size"])
        for m in re.finditer(rb"\xe8", img[va - B:va + size - B]):
            p = va + m.start()
            tgt = p + 5 + struct.unpack_from("<i", img, p + 1 - B)[0]
            if th.get(tgt, tgt) == target:
                return True
        return False
    assert calls(0x00443830, 0x00412340)        # frontendOptionsExit -> memcard_WriteSave
    assert calls(0x0043FA20, 0x00412190)        # frontendMainMenuEnter -> memcard_LoadSave


def test_unreachable_entries(img):
    """the PSX / unused screens: no call, push or data reference to their entry (or its thunk)"""
    th = thunk_targets(img)
    rev = {}
    for p, tgt in th.items():
        rev.setdefault(tgt, []).append(p)
    text_ = img[0x1000:0x14B000]
    for va in (0x0043CD80, 0x0043F9B0, 0x00443E30, 0x00441860):   # language, title, load/save, color adjust
        refs = [va] + rev.get(va, [])
        for r in refs:
            for m in re.finditer(re.escape(struct.pack("<I", r)), img):
                assert False, (hex(va), hex(B + m.start()))
            for m in re.finditer(rb"\xe8", text_):
                p = 0x401000 + m.start()
                if 0x401005 <= p < 0x402620:
                    continue
                assert p + 5 + struct.unpack_from("<i", img, p + 1 - B)[0] != r, (hex(va), hex(p))
    # g_feAutoload (0x0056B618) is only ever cleared (mov [..], eax after 'xor eax, eax') or read
    refs = [0x401000 + m.start() for m in re.finditer(re.escape(struct.pack("<I", 0x0056B618)), text_)]
    assert refs == [0x0043F772, 0x0043F9F8, 0x0044DA6A]
    assert img[0x0043F771 - B] == img[0x0043F9F7 - B] == 0xA3
    assert img[0x0044DA69 - B] == 0xA1


# ------------------------------------------------------------------ data
@pytest.fixture(scope="module")
def groups():
    f = GAME_DIR / "assets" / "Intwrap.zal"
    if not f.is_file():
        pytest.skip("no Intwrap.zal")
    from dmbmx.formats import scrn
    return scrn.read_intwrap(f.read_bytes())


def test_screens_against_the_loader(groups):
    """guiLoadScreenFile: every sprite / selected sprite / font an EDEF names resolves in the screen's
    group, the permanent group 0xE000 or (select screen) group 0xE001 that frontendSelectEnter loads"""
    from dmbmx.formats import scrn
    perm = set(groups[0xE000].ids())
    n = 0
    for gid, g in groups.items():
        for sid in g.screens():
            s = scrn.read_screen(g.asset(sid))
            pool = set(g.ids()) | perm | (set(groups[0xE001].ids()) if sid == 0xA009 else set())
            for e in s.edefs:
                for field, on in (("sprite", not e.sprite_flags & 2), ("sel_sprite", not e.sprite_flags & 2),
                                  ("font", not e.text_flags & 2)):
                    if on and e.fields[field]:
                        assert e.fields[field] in pool, (hex(sid), hex(e.id), field)
                        n += 1
                assert e.fields["type"] == 1
    assert n == 168
    # the element ids the frontend looks up exist on its screens
    t = text(FRONT)
    ids = {sid: {e.id for e in scrn.read_screen(g.asset(sid)).edefs} for g in groups.values() for sid in g.screens()}
    for macro, scr in (("MAIN", 0xA005), ("SEL", 0xA009), ("OPT", 0xA00C), ("COOL", 0xA00D), ("CARD", 0xA012),
                       ("LANG", 0xA014)):
        used = {int(x, 0) for x in re.findall(macro + r"\((0x[0-9A-Fa-f]+|\d+)\)", t)}
        missing = sorted(n for n in used if (scr << 16 | n) not in ids[scr])
        assert not missing, (macro, [hex(m) for m in missing])


@pytest.fixture(scope="module")
def texts():
    f = GAME_DIR / "assets" / "miscdata.zal"
    if not f.is_file():
        pytest.skip("no miscdata.zal")
    z = dmparse.read_zal(f.read_bytes())
    g = {x["id"]: x["data"] for x in z["groups"]}
    out = {}
    for lang in range(4):                                # the frontend set 0x7919 + language
        s = dmparse.read_strs(g[0x7919 + lang])
        out[lang] = {s["base_id"] + i: v for i, v in enumerate(s["strings"])}
    for base in (0x7D01, 0x7D05):                         # the HUD / single-player sets (English)
        s = dmparse.read_strs(g[base])
        out[0].update({s["base_id"] + i: v for i, v in enumerate(s["strings"])})
    return out


def test_text_ids_resolve(texts):
    en = texts[0]
    for f in COUNTS:
        for m in re.finditer(r"zTextGet\((0x[0-9A-Fa-f]+|\d+)\)", text(f)):
            tid = int(m.group(1), 0)
            assert tid in en and en[tid] != "", (f, hex(tid))
    assert [en[i] for i in (0x24, 0x25, 0x26, 0x29, 0x2A, 0x2C)] == \
        ["PROQUEST", "SESSION", "FREERIDE", "COOL STUFF", "OPTIONS", "QUIT"]
    assert [en[i] for i in (0x132, 0x133, 0x139)] == ["CONTROLLER", "CHEATS", "AUDIO"]
    assert [en[i] for i in (0x2F, 0x33, 0x34, 0x35)] == ["CHALLENGES", "HIGH SCORES", "MOVIES", "CREDITS"]
    assert [en[0x19 + k] for k in (0, 1)] == ["FIRST PERSON CAM", "NIGHT VISION MODE"]
    assert en[0x3A] == "* LOCKED *" and en[0x147] == "MUSIC" and en[0x148] == "SOUND FX"
    # the language screen: option 1 is labelled DEUTSCH but loads set 2, which is the French one
    assert en[0x24D] == "DEUTSCH" and en[0x24C] == "FRANCAIS"
    assert texts[2][0x25E] == "CHARGEMENT TERMINE." and texts[3][0x25E] == "GELADEN." and texts[3][0x25D] == "LADE..."
    # the select screen ticker: rider bios are "#Y<label> #B<value>" lines
    assert en[0x158] == "#YNickname: #BMiracle Boy"

"""Decomp clean-up after the per-module passes.

1. Gap functions moved into their files: zobj.c, zobjModel.c, pcinput.c and zobjPEmit.c hold the ZglRw pass's
   drafts; the only gap files left have no known owner and say why.
2. Ghidra holes: tools/ghidra/scan_holes.py finds no executable bytes in the game/engine range outside a
   function, padding, a jump/index table or an import stub, except the allow-listed ones below.
   The 88 functions the clean-up created (CREATED) have decomp/symbols/symbols.csv rows and drafted C.
3. Signatures and names: g_pfnDebugPrintAt = zdebugPrint(x, y, fmt, ...); ZprimFont +0x02 is the line
   height (zprimGetFontHeight); 0x00439FA1 is removed from Ghidra by a `nofunc` row.
4. Consistency: every decomp/functions.csv function has exactly one `/* VA 0x..., N bytes` comment in
   its file and N is the function's size. (The syntax check of every C file is test_decomp_syntax.py.)
"""
import csv
import re
import sys
from pathlib import Path

import pytest

from conftest import GAME_DIR

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
sys.path.insert(0, str(REPO / "tools" / "ghidra"))

# holes the scanner may report, with the reason they are not functions
ALLOWED_HOLES = {
    0x00441C16: "a switch case of frontendColorAdjustHandler 0x00441BD0, reached only through its jump "
                "table 0x00441C4C, which Ghidra did not recover; the C has the case (GUI_MSG_BACK)",
}
# gap files that stay, and a phrase their header must contain (the reason)
ALLOWED_GAPS = {
    "gap_rider__guiinput.c": "none of these functions references a path string",
    "gap_zrwCol__zobjPEmit.c": "Why this stays a gap",
}
MOVED = {   # gap drafts moved into their files -> file
    "NgEngine/Zge/zobj.c": [0x00487A00, 0x00487A50],
    "NgEngine/Zge/ZgeMods/zobjModel.c": [0x00488D30] + list(range(0x00488FB0, 0x004892B0)),
    "NgEngine/Zge/Pc/pcinput.c": list(range(0x004937F0, 0x00493CDF)),
    "NgEngine/Zge/ZgeMods/zobjPEmit.c": [0x00495A10, 0x00495A60, 0x00495BD0, 0x00495C10, 0x00495D50],
}


def functions():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return {int(r["va"], 16): r for r in csv.DictReader(f)}


def symbol_rows():
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


# the functions the clean-up created (holes in the Ghidra project: no function there before)
CREATED = (
    0x00403CC0, 0x004040C0, 0x00405760, 0x00407730, 0x00407890, 0x00408390, 0x004726C0, 0x004726E0,
    0x004728C0, 0x004728E0, 0x00472960, 0x00472990, 0x004729C0, 0x00472A00, 0x00472AF0, 0x004739C0,
    0x00475140, 0x004752C0, 0x004752E0, 0x00475430, 0x00475460, 0x004754D0, 0x004754F0, 0x00475520,
    0x00475730, 0x004758F0, 0x00475920, 0x00475940, 0x00475A00, 0x00475AD0, 0x00475B10, 0x00475B50,
    0x00475B90, 0x00475BB0, 0x00475BD0, 0x00475BF0, 0x00475C20, 0x00475C40, 0x00475C60, 0x00475C80,
    0x004760B0, 0x00476190, 0x00476200, 0x00476250, 0x004762A0, 0x00476340, 0x00477DF0, 0x00477FB0,
    0x004788C0, 0x00478980, 0x00478AE0, 0x00478D00, 0x00478DC0, 0x004791C0, 0x004791E0, 0x00479210,
    0x00479A80, 0x00479AB0, 0x0047A140, 0x0047A1E0, 0x0047A300, 0x0047A330, 0x0047A5E0, 0x0047A600,
    0x0047AA60, 0x0047BD00, 0x0047C010, 0x004853A0, 0x004853B0, 0x004854C0, 0x004859B0, 0x004859C0,
    0x00495CC0, 0x0049A740, 0x0049AA00, 0x0049ABB0, 0x0049ABD0, 0x0049AD30, 0x0049C440, 0x0049C470,
    0x0049C4A0, 0x0049C4C0, 0x0049C4F0, 0x0049C520, 0x0049C550, 0x0049F490, 0x0049FA60, 0x004A4190,
)


def func_rows():
    """{va: row} of func rows in decomp/symbols/symbols.csv, later rows win (as in ApplySymbols)."""
    return {int(r["address"], 16): r for r in symbol_rows() if r["kind"] == "func"}


def test_moved_gap_functions_are_in_their_files():
    fn = functions()
    for path, vas in MOVED.items():
        mine = [v for v in vas if v in fn]
        assert mine, path
        for va in mine:
            assert fn[va]["file"] == path, (hex(va), fn[va]["file"])
            assert fn[va]["status"] == "draft"


def test_only_ownerless_gap_files_left():
    gaps = sorted(p.name for p in (SRC / "_unassigned").glob("*.c"))
    assert set(gaps) == set(ALLOWED_GAPS), gaps
    for name, phrase in ALLOWED_GAPS.items():
        assert phrase in (SRC / "_unassigned" / name).read_text(encoding="utf-8")[:2500], name
    in_csv = {r["file"] for r in functions().values() if r["file"].startswith("_unassigned/")}
    assert in_csv == {"_unassigned/" + g for g in gaps}


def test_every_function_drafted_once_with_its_size():
    fn = functions()
    assert all(r["status"] in ("draft", "reviewed") for r in fn.values())
    seen = {}
    for p in SRC.rglob("*.c"):
        rel = str(p.relative_to(SRC)).replace("\\", "/")
        for m in re.finditer(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes,", p.read_text(encoding="utf-8")):
            va = int(m.group(1), 16)
            assert va not in seen, f"0x{va:08x} in {seen.get(va)} and {rel}"
            seen[va] = rel
            assert va in fn, f"{rel}: 0x{va:08x} is not a function"
            assert fn[va]["file"] == rel, (hex(va), rel, fn[va]["file"])
            assert int(m.group(2)) == int(fn[va]["size"]), (hex(va), m.group(2), fn[va]["size"])
    missing = sorted(set(fn) - set(seen))
    assert not missing, [hex(v) for v in missing[:10]]


def test_created_functions_have_rows_names_and_c():
    fn = functions()
    rows = func_rows()
    assert len(CREATED) == len(set(CREATED)) == 88
    for va in CREATED:
        r = rows[va]
        assert re.fullmatch(r"[a-z][A-Za-z0-9]+", r["name"]) and not r["name"].startswith("FUN_"), r
        if va == 0x00403CC0:      # Ghidra keeps the lone jmp as a thunk: no decomp row
            assert va not in fn
            continue
        assert fn[va]["name"] == r["name"] and fn[va]["status"] == "draft", hex(va)
        assert fn[va]["signature"], hex(va)


def test_no_placeholder_names_left():
    for p in list(SRC.rglob("*.c")) + list((REPO / "decomp" / "include").glob("*.h")):
        t = p.read_text(encoding="utf-8")
        assert not re.search(r"\bFUN_00[0-9a-f]{6}\b", t), p.name


def test_bogus_function_removed():
    rows = [r for r in symbol_rows() if int(r["address"], 16) == 0x00439FA1]
    assert rows[-1]["kind"] == "nofunc"
    fn = functions()
    assert 0x00439FA1 not in fn
    assert int(fn[0x00439F80]["size"]) > 0x600          # the draw case is back in guiElementHandler
    java = (REPO / "tools" / "ghidra" / "ApplySymbols.java").read_text(encoding="utf-8")
    assert 'case "nofunc"' in java and "fixupFunctionBody" in java


def test_signature_and_name_fixes():
    ngif = (REPO / "decomp" / "include" / "ngif.h").read_text(encoding="utf-8")
    assert "(__cdecl *g_pfnDebugPrintAt)(int16_t x, int16_t y, const char *fmt, ...)" in ngif
    zprim = (REPO / "decomp" / "include" / "zprim.h").read_text(encoding="utf-8")
    assert "uint16_t lineHeight;" in zprim and "zprimGetFontHeight(" in zprim
    assert "zprimGetFontFlags(" not in zprim
    for p in SRC.rglob("*.c"):
        t = p.read_text(encoding="utf-8")
        assert "zprimGetFontFlags" not in t and "g_zdebugPrintf" not in t, p.name


# ------------------------------------------------------------------ against the exe

@pytest.fixture(scope="module")
def exe():
    path = GAME_DIR / "NgBMX.exe"
    if not path.is_file():
        pytest.skip("no NgBMX.exe")
    pytest.importorskip("capstone")
    return path


def test_hole_scanner(exe):
    import scan_holes
    holes = scan_holes.scan(str(exe), str(REPO / "decomp" / "symbols" / "functions.csv"))
    found = {h["va"]: h for h in holes}
    unexpected = {hex(va): (h["size"], h["prev"], h["insn"]) for va, h in found.items() if va not in ALLOWED_HOLES}
    assert not unexpected, unexpected
    # the allow list stays honest: each entry is still a hole
    assert set(found) == set(ALLOWED_HOLES)


def test_scanner_finds_a_known_hole(exe, tmp_path):
    """Drop one created function from the input: the scanner must report exactly its bytes."""
    import scan_holes
    with open(REPO / "decomp" / "symbols" / "functions.csv", encoding="utf-8", newline="") as f:
        rows = [r for r in csv.reader(f)]
    tmp = tmp_path / "functions_minus_one.csv"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        csv.writer(f, lineterminator="\n").writerows(r for r in rows if r[0] != "0x0049a740")
    holes = {h["va"]: h for h in scan_holes.scan(str(exe), str(tmp))}
    assert 0x0049A740 in holes and holes[0x0049A740]["size"] == 76 and holes[0x0049A740]["referenced"]


def test_debug_print_format_literal(exe):
    """ngappPrintVersion passes 0x00557430 as the format: it is "%s", not an RwRGBA."""
    import scan_holes
    assert scan_holes.Image(str(exe)).bytes(0x00557430, 3) == b"%s\0"

"""Self-consistency of the decompiled memcard.c / zfile.c / zrwFile.c / zramdisk.c.

No compiler on the build PC, so this checks what can be checked statically:
- every function of the four files is draft (or better) and its C definition follows its VA comment;
- no Ghidra artefacts remain (undefined4, FUN_, param_N, DAT_, LAB_, in_stack);
- zfile.h documents the 16 device slots at +0x00..+0x3C in order;
- the two device tables written in C list the same functions as the tables in NgBMX.exe;
- the SAVE.DAT layout used by memcard.c adds up with the struct sizes in ghidra_types.txt.
"""
import csv
import re
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
FILES = ["zaxis/Bmx/Src/com/action/memcard.c", "NgEngine/Zge/zfile.c",
         "NgEngine/Zge/Com/ZglRw/zrwFile.c", "NgEngine/Zge/ZgeMods/zramdisk.c"]
SLOTS = ["GetName", "MakeFileName", "CheckDevice", "Format", "CheckFormatted", "GetFreeSpace",
         "Exists", "Delete", "Open", "Close", "Read", "Write", "Seek", "Tell", "GetSize", "reserved3C"]
TABLES = {  # C variable -> (file, VA of the table in the exe)
    "zrwFileDevice": ("NgEngine/Zge/Com/ZglRw/zrwFile.c", 0x00562FF8),
    "zfileDevice2": ("NgEngine/Zge/ZgeMods/zramdisk.c", 0x00563240),
}


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return [r for r in csv.DictReader(f) if r["file"] in FILES]


def test_all_draft_and_defined():
    rows = csv_rows()
    assert len(rows) == 73   # + zfileSetHeap / ClearHeap / GetSeparator, zramdiskShutdown
    for f in FILES:
        text = (SRC / f).read_text(encoding="utf-8")
        for r in (x for x in rows if x["file"] == f):
            assert r["status"] in ("draft", "reviewed"), r
            m = re.search(r"/\* VA %s, %s bytes, [^*]*status %s \*/\n([^\n]*)\n" % (r["va"], r["size"], r["status"]), text)
            assert m, f"{f}: no VA comment for {r['va']} {r['name']}"
            assert re.search(r"\b%s\(" % re.escape(r["name"]), m.group(1)), (r["name"], m.group(1))


def test_no_ghidra_artefacts():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|in_stack_\w+|"
                     r"[iu]Var\d+|pc?Var\d+|zfile_[0-9a-f]{8})\b")
    for f in FILES:
        for n, line in enumerate((SRC / f).read_text(encoding="utf-8").splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"


def test_device_slots_documented():
    text = (INC / "zfile.h").read_text(encoding="utf-8")
    body = re.search(r"struct zfileDevice \{(.*?)\n\};", text, re.S).group(1)
    names = re.findall(r"\(__cdecl \*(\w+)\)|void \*(reserved3C);", body)
    assert [a or b for a, b in names] == SLOTS
    offs = [int(x, 16) for x in re.findall(r"/\* \+0x([0-9A-F]{2})", body)]
    assert offs == list(range(0, 0x40, 4))


def table_functions(var):
    f, _ = TABLES[var]
    text = (SRC / f).read_text(encoding="utf-8")
    init = re.search(r"zfileDevice %s = \{(.*?)\};" % var, text, re.S).group(1)
    return [x.strip() for x in init.replace("\n", " ").split(",")]


def test_device_tables_in_c():
    for var in TABLES:
        fns = table_functions(var)
        assert len(fns) == 16 and fns[-1] == "NULL"
        prefix = "zrwFile_" if var == "zrwFileDevice" else "zramdisk_"
        assert fns[:-1] == [prefix + s for s in SLOTS[:-1]]


def test_device_tables_match_exe(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    pe = pefile.PE(str(exe), fast_load=True)
    by_name = {r["name"]: int(r["va"], 16) for r in csv_rows()}
    for var, (_, va) in TABLES.items():
        ptrs = struct.unpack("<16I", pe.get_data(va - 0x400000, 0x40))
        want = [by_name[n] if n != "NULL" else 0 for n in table_functions(var)]
        assert list(ptrs) == want, var


def test_save_layout_adds_up():
    sizes = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, name, val = line.split()
            sizes[name] = int(val, 16)
    # SAVE.md: header 8, block A at 0x8, records at 0xC44, profiles at 0x22C4, total 0x235C
    assert 8 + sizes["SaveBlockA"] == 0xC44
    assert 0xC44 + 12 * 24 * sizes["SaveRecordEntry"] == 0x22C4
    assert 0x22C4 + 2 * sizes["SaveProfile"] == 0x235C
    text = (SRC / FILES[0]).read_text(encoding="utf-8")
    assert "#define SAVE_SIZE     0x235C" in text and "0x2A81511C" in text

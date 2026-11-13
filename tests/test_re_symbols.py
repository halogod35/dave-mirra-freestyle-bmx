"""The symbol pipeline files (decomp/symbols/symbols.csv, decomp/symbols/functions.csv, docs/re/module-map.md).

No Ghidra needed: these tests check the CSV contract (AD-6) and that the committed outputs are what
tools/ghidra/build_symbols.py generates from the committed decomp/symbols/functions.csv.
"""
import csv
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SYMBOLS = REPO / "decomp" / "symbols" / "symbols.csv"
FUNCTIONS = REPO / "decomp" / "symbols" / "functions.csv"
MODMAP = REPO / "docs" / "re" / "module-map.md"
HEADER = ["address", "kind", "name", "type", "source_file", "confidence"]
FUNC_HEADER = ["va", "end", "size", "name", "thunk_of", "library", "source_file", "confidence",
               "n_callers", "n_callees", "callers", "callees", "strings_referenced", "signature"]


def rows(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.reader(f))


def test_symbols_contract():
    r = rows(SYMBOLS)
    assert r[0] == HEADER
    prev = -1
    for i, row in enumerate(r[1:], 2):
        assert len(row) == 6, f"line {i}: {row}"
        assert re.fullmatch(r"0x[0-9a-f]{8}", row[0]), f"line {i}: address must be 0x%08x lowercase"
        a = int(row[0], 16)
        assert a >= prev, f"line {i}: not sorted by address"
        prev = a
        assert row[1] in ("func", "data", "label", "struct", "nofunc"), f"line {i}"
        assert row[5] in ("V", "I"), f"line {i}"


def test_functions_csv():
    r = rows(FUNCTIONS)
    assert r[0] == FUNC_HEADER
    body = r[1:]
    assert len(body) > 3000
    vas = [int(x[0], 16) for x in body]
    assert vas == sorted(vas) and len(set(vas)) == len(vas)
    libs = {x[5] for x in body}
    assert {"msvcrt", "renderware", "libpng", "zlib"} <= libs
    by = {int(x[0], 16): x for x in body}
    # the save functions (save editor names) and the entry point are present
    assert by[0x00412190][3] == "memcard_LoadSave"
    assert by[0x004a51f9][3] == "WinMainCRTStartup"
    assert by[0x0048a460][3] == "WinMain"


def test_generator_is_up_to_date(tmp_path):
    """Running build_symbols.py on the committed functions.csv reproduces the committed files."""
    sym, mm = tmp_path / "symbols.csv", tmp_path / "module-map.md"
    shutil.copy(SYMBOLS, sym)
    shutil.copy(MODMAP, mm)
    r = subprocess.run([sys.executable, str(REPO / "tools" / "ghidra" / "build_symbols.py"),
                        "--functions", str(FUNCTIONS), "--symbols", str(sym), "--module-map", str(mm)],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    assert sym.read_bytes() == SYMBOLS.read_bytes(), "decomp/symbols/symbols.csv is stale: run tools/ghidra/build_symbols.py"
    assert mm.read_bytes() == MODMAP.read_bytes(), "docs/re/module-map.md is stale"


def test_generator_keeps_hand_written_rows(tmp_path):
    sym, mm = tmp_path / "symbols.csv", tmp_path / "module-map.md"
    shutil.copy(SYMBOLS, sym)
    mm.write_text("# x\n", encoding="utf-8")
    with open(sym, "a", encoding="utf-8", newline="") as f:
        f.write("0x00403c30,func,MyName,,Bmx/Src/com/action/action.c,V\n")
    r = subprocess.run([sys.executable, str(REPO / "tools" / "ghidra" / "build_symbols.py"),
                        "--functions", str(FUNCTIONS), "--symbols", str(sym), "--module-map", str(mm)],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    got = [x for x in rows(sym)[1:] if x[0] == "0x00403c30"]
    assert ["0x00403c30", "func", "MyName", "", "Bmx/Src/com/action/action.c", "V"] in got
    assert not any(x[2] == "action_00403c30" for x in got), "auto row must not be added where a hand row names it"


@pytest.mark.skipif(shutil.which("bash") is None, reason="bash not on PATH")
def test_apply_symbols_help():
    r = subprocess.run([shutil.which("bash"), "tools/ghidra/apply_symbols.sh", "--help"], cwd=REPO,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and "Usage:" in r.stdout

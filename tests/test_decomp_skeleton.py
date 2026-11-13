"""Decomp skeleton (decomp/functions.csv, decomp/src/**, decomp/PROGRESS.md, decomp/include).

No Ghidra or C compiler needed. Checks the CSV contract, that every function has exactly one stub
with its VA comment, that the committed skeleton is what make_skeleton.py generates, and that the
struct headers and the Ghidra import manifest agree.
"""
import csv
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DECOMP = REPO / "decomp"
HEADER = ["va", "size", "name", "file", "confidence", "method", "status", "signature"]


def rows():
    with open(DECOMP / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def test_functions_csv_contract():
    with open(DECOMP / "functions.csv", encoding="utf-8", newline="") as f:
        assert next(csv.reader(f)) == HEADER
    rs = rows()
    assert len(rs) > 1300
    vas = [int(r["va"], 16) for r in rs]
    assert vas == sorted(vas) and len(set(vas)) == len(vas)
    names = [r["name"] for r in rs]
    assert len(set(names)) == len(names), "function names must be unique (they become C identifiers)"
    for r in rs:
        assert re.fullmatch(r"0x[0-9a-f]{8}", r["va"])
        assert r["status"] in ("todo", "draft", "reviewed")
        assert r["confidence"] in ("V", "I", "")
        assert (r["confidence"] == "") == r["file"].startswith("_unassigned/")
        assert r["signature"], r["va"]
        assert re.fullmatch(r"[A-Za-z_]\w*", r["name"]), r["name"]
    # well-known anchors
    by = {int(r["va"], 16): r for r in rs}
    assert by[0x00412190]["file"] == "zaxis/Bmx/Src/com/action/memcard.c"
    assert by[0x00412190]["confidence"] == "V"
    assert by[0x00484700]["file"] == "NgEngine/Zge/zfile.c"


def test_every_function_has_one_stub():
    found = {}
    for p in (DECOMP / "src").rglob("*.c"):
        rel = p.relative_to(DECOMP / "src").as_posix()
        for m in re.finditer(r"/\* VA 0x([0-9a-f]{8}),", p.read_text(encoding="utf-8")):
            va = int(m.group(1), 16)
            assert va not in found, f"0x{va:08x} in {found.get(va)} and {rel}"
            found[va] = rel
    for r in rows():
        va = int(r["va"], 16)
        assert found.get(va) == r["file"], f"{r['va']} {r['name']}: stub in {found.get(va)}, csv says {r['file']}"
    assert len(found) == len(rows())


def test_skeleton_is_up_to_date(tmp_path):
    out = tmp_path / "decomp"
    shutil.copytree(DECOMP, out)
    r = subprocess.run([sys.executable, str(REPO / "tools" / "decomp" / "make_skeleton.py"), "--out", str(out)],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr
    for rel in ["functions.csv", "PROGRESS.md"]:
        assert (out / rel).read_bytes() == (DECOMP / rel).read_bytes(), f"decomp/{rel} is stale"
    a = sorted(p.relative_to(out).as_posix() for p in (out / "src").rglob("*.c"))
    b = sorted(p.relative_to(DECOMP).as_posix() for p in (DECOMP / "src").rglob("*.c"))
    assert a == b
    for rel in b:
        assert (out / rel).read_bytes() == (DECOMP / rel).read_bytes(), f"decomp/{rel} is stale"


def test_ghidra_type_manifest():
    inc = DECOMP / "include"
    lines = [l.strip() for l in (inc / "ghidra_types.txt").read_text(encoding="utf-8").splitlines()
             if l.strip() and not l.startswith("#")]
    headers = [l for l in lines if not l.startswith("size ")]
    sizes = dict((l.split()[1], int(l.split()[2], 16)) for l in lines if l.startswith("size "))
    text = ""
    for h in headers:
        assert (inc / h).is_file(), h
        text += (inc / h).read_text(encoding="utf-8")
    for name in sizes:
        assert re.search(r"\b(struct|typedef struct)\s+%s\b" % name, text) or \
            re.search(r"}\s*%s\s*;" % name, text), name
    # every struct row in decomp/symbols/symbols.csv names a type from the manifest
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        structs = [r for r in csv.DictReader(f) if r["kind"] == "struct"]
    assert structs
    for r in structs:
        assert (REPO / r["type"]).is_file(), r
        assert re.search(r"\b%s\b" % r["name"], (REPO / r["type"]).read_text(encoding="utf-8")), r


def test_stub_types_are_declared():
    """Every type token in the stub signatures is declared in decomp/include (no compiler here)."""
    known = set()
    for h in (DECOMP / "include").glob("*.h"):
        t = h.read_text(encoding="utf-8")
        known |= set(re.findall(r"typedef\s+[^;]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;", t))
        known |= set(re.findall(r"\bstruct\s+([A-Za-z_]\w*)", t))
        # function-pointer typedefs: typedef int (__cdecl *Name)(...); 
        known |= set(re.findall(r"typedef\s+[^;()]*\(\s*(?:__\w+\s+)?\*\s*([A-Za-z_]\w*)\s*\)", t))
    kw = {"void", "int", "char", "short", "long", "float", "double", "unsigned", "signed", "const", "struct",
          "__cdecl", "__stdcall", "__fastcall", "__thiscall", "volatile"}
    missing = set()
    for r in rows():
        m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*\((.*)\)$", r["signature"])
        assert m, r["signature"]
        parts = [m.group(1)] + [p for p in m.group(3).split(",") if p.strip() not in ("void", "...", "")]
        for k, p in enumerate(parts):
            toks = re.findall(r"[A-Za-z_]\w*", p)
            if k > 0 and toks:
                toks = toks[:-1]  # parameter name
            missing |= {t for t in toks if t not in kw and t not in known}
    assert not missing, sorted(missing)


def test_generator_never_overwrites_hand_written_files(tmp_path):
    """Even if functions.csv says 'todo' everywhere (e.g. after a bad merge), files that hold
    anything but generated stubs must survive a regeneration byte for byte."""
    sys.path.insert(0, str(REPO / "tools" / "decomp"))
    import make_skeleton
    hand = [p for p in (DECOMP / "src").rglob("*.c") if make_skeleton.is_hand_written(str(p))]
    assert hand, "expected hand-written files (memcard.c, zfile.c, zasset.c, ...)"
    out = tmp_path / "decomp"
    shutil.copytree(DECOMP, out)
    rows = list(csv.reader(open(out / "functions.csv", encoding="utf-8", newline="")))
    for r in rows[1:]:
        r[6] = "todo"
    with open(out / "functions.csv", "w", encoding="utf-8", newline="") as f:
        csv.writer(f, lineterminator="\n").writerows(rows)
    r = subprocess.run([sys.executable, str(REPO / "tools" / "decomp" / "make_skeleton.py"), "--out", str(out)],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr
    for p in hand:
        rel = p.relative_to(DECOMP)
        assert (out / rel).read_bytes() == p.read_bytes(), f"{rel} was overwritten"

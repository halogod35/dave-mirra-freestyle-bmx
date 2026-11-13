"""Derive the source-file module map and library tags for NgBMX.exe from decomp/symbols/functions.csv,
merge the result into decomp/symbols/symbols.csv and regenerate the tables in docs/re/module-map.md.

    tools/testing/py.sh tools/ghidra/build_symbols.py            # update symbols.csv + module-map.md
    tools/testing/py.sh tools/ghidra/build_symbols.py --check    # report only, write nothing

Inputs are only facts that do not change when names are applied (VAs, string references, call
graph), so the script is idempotent: run -> apply_symbols.sh -> run again gives the same files.

Attribution passes (game/engine code only, i.e. below LIB_START):
  V  direct      the function references the module's __FILE__ path string
  I  strings     it references a string inside the .data string window of exactly one module,
                 and that module is the previous or next attributed module in VA order
                 (window = strings of the module's [V] functions, clipped by the neighbouring
                 modules' __FILE__ strings)
  I  between     it lies between two functions of the same module (link order = object order)
  I  callers     all its callers belong to one module, and the function next to it is in that module
Library code is tagged by VA range (see LIB_RANGES, documented in docs/re/libraries.md).

Rows in symbols.csv from any card are kept verbatim. Only DM-002 rows are (re)generated: earlier
generated rows (name "<file>_<va>" or empty name + lib: source, no type) are dropped and an
auto row is added for an attributed function that has no row yet, and DM-002 rows with an empty
source_file get it filled in.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import io
import os
import re
import sys
from collections import Counter, defaultdict

HEADER = ["address", "kind", "name", "type", "source_file", "confidence", "card"]
CARD = "DM-002"          # generated rows from the DM-002 passes
CARD40 = "DM-040"        # generated rows that need a DM-040 pass (seed/range/orphan/split)
GEN_CARDS = (CARD, CARD40)

# Statically linked code, by VA range [lo, hi). Order = link order. Evidence in docs/re/libraries.md.
LIB_RANGES = [
    # DM-061: starts at _fpmath 0x004A4A20 (fpinit.c: _fpmath, _cfltcvt_init, _set_new_mode)
    (0x004A4A20, 0x004AF9D0, "lib:msvcrt", "MSVC static C runtime (LIBC), part 1"),
    (0x004AF9D0, 0x004AFC60, "lib:imports", "import-library jmp stubs (vfw32, kernel32 RtlUnwind, dinput)"),
    (0x004AFC60, 0x004B0F20, "lib:dinput", "dinput.lib c_dfDI* DIOBJECTDATAFORMAT tables (data in .text)"),
    (0x004B0F20, 0x00506E40, "lib:renderware", "RenderWare 3.x core, world, D3D7 driver, toolkits (incl. rtpng glue at 0x5068a0)"),
    (0x00506E40, 0x0051CE10, "lib:libpng", "libpng 1.0.5 interleaved with zlib 1.1.3 (split per function below)"),
    (0x0051CE10, 0x0051D2F0, "lib:renderware", "RenderWare (late-linked object)"),
    (0x0051D2F0, 0x00520960, "lib:msvcrt", "MSVC static C runtime (LIBC), part 2"),
    (0x00520960, 0x0054AC1C, "lib:renderware", "RenderWare (late-linked object)"),
    (0x0054B000, 0x0054D000, "lib:renderware", "RenderWare _rwcseg section"),
]
LIB_START = LIB_RANGES[0][0]
CODE_START = 0x00403C30  # first game function; 0x401000.. = /INCREMENTAL thunk table + 0xCC padding

# zlib 1.1.3 inside the libpng range: these entry points and everything they call there.
ZLIB_SEEDS = [0x005113F4, 0x00511454, 0x005114B0, 0x00511B60, 0x00511B90, 0x00518B10, 0x00518B20,
              0x0051A1F0]  # adler32 (DM-030), reached through a function pointer

PATH_RE = re.compile(r"^C:\\(zaxis|NgEngine)\\", re.I)


def norm_path(p: str) -> str:
    """C:\\zaxis\\Bmx\\src\\com\\x.c -> Bmx/Src/com/x.c, C:\\NgEngine\\Zge\\x.c -> NgEngine/Zge/x.c.
    Project convention (rows on main): drop the drive and the leading 'zaxis'. Windows paths are
    case-insensitive and the binary mixes 'Src' and 'src'; we use 'Src' so both land in one directory."""
    p = p[3:].replace("\\", "/")
    p = re.sub(r"^zaxis/", "", p, flags=re.I)
    return re.sub(r"^Bmx/src/", "Bmx/Src/", p, flags=re.I)


def stem(src: str) -> str:
    return os.path.splitext(os.path.basename(src))[0]


def lib_of(va: int):
    for lo, hi, lib, _ in LIB_RANGES:
        if lo <= va < hi:
            return lib
    return None


def parse_strings(s: str):
    out = []
    if not s:
        return out
    for part in s.split(" | "):
        a, _, t = part.partition("=")
        try:
            out.append((int(a, 16), t))
        except ValueError:
            pass
    return out


def load_functions(path):
    funcs = []
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            va = int(r["va"], 16)
            funcs.append({
                "va": va,
                "size": int(r["size"]),
                "name": r["name"],
                # ILT entries below CODE_START count as thunks even when Ghidra did not mark them
                "thunk": bool(r.get("thunk_of")) or va < CODE_START,
                "callers": [int(x, 16) for x in r["callers"].split(";") if x],
                "callees": [int(x, 16) for x in r["callees"].split(";") if x],
                "strings": parse_strings(r["strings_referenced"]),
            })
    funcs.sort(key=lambda f: f["va"])
    return funcs


# Files whose __FILE__ string exists but is never referenced by code (DM-002). Their literal
# window (between the neighbouring path strings, minus the neighbours' own windows) still
# identifies their functions. Value = address of the path string in .data.
ORPHAN_PATHS = {
    "NgEngine/Zge/ZgeMods/zprop.c": 0x00560CB0,
    "NgEngine/Zge/ZgeMods/ztaginf.c": 0x005617DC,
    "NgEngine/Zge/ZgeMods/zscnpts.c": 0x00561D64,
    "NgEngine/Zge/zutil.c": 0x005637E4,
    "NgEngine/Zge/Com/ZglRw/zrwRamDisk.c": 0x005638B8,
}

# DM-040: string evidence for a file. A function that references a literal matching the regex
# belongs to the file (the original code prefixes messages with its function or module name).
# An optional (lo, hi) limits the literal addresses that count.
STRING_SEEDS = [
    (r"^bmxphys", "Bmx/Src/com/action/physics.c"),
    (r"^bmxprop", "Bmx/Src/com/action/prop.c"),
    (r"^bmxreplay", "Bmx/Src/com/action/replay.c"),
    (r"^frontend[A-Z]", "Bmx/Src/com/menu/frontend.c"),
    (r"^GUIMAIN:", "Bmx/Src/com/menu/gui/guimain.c"),
    (r"^ZPRIM:", "Bmx/Src/com/zge/zprim.c"),
    (r"^ZCOMPRES:", "NgEngine/Zge/zcompres.c"),
    (r"^RwMat:", "NgEngine/Zge/Com/ZglRw/zrwMat.c"),
    (r"^RwCam:", "NgEngine/Zge/Com/ZglRw/zrwCam.c"),
    (r"^zrwModelCreate", "NgEngine/Zge/Com/ZglRw/zrwModelCreate.c"),
    # only the literals between the zge.c and zFont.c path strings (the player object); 0x0048bed0
    # in zrwZgl.c also prints "Movie : Done Playing" etc.
    (r"^Movie : ", "NgEngine/Zge/Pc/pcmovie.c", (0x00563470, 0x00563724)),
]
# DM-040: VA ranges [lo, hi) assigned as a whole (evidence in docs/re/module-map.md).
RANGE_FILES = [
    # sin/cos/atan lookup tables + vector/matrix helpers between zasset.c and zobjCamera.c
    # (DM-045: zmath.c ends at 0x00482082; 0x00482090.. are zobjCamera vtable methods, 0x00560518)
    (0x00480D20, 0x00482090, "NgEngine/Zge/zmath.c"),
    # zcompres_* (DM-010 names, "ZCOMPRES:" messages) between zrwShadow.c and zrwModelCreate.c
    (0x0049B8E0, 0x0049C570, "NgEngine/Zge/zcompres.c"),
    # DM-042: 0x0049C570 is a list-callback dispatcher of zrwModelCreate.c, not compression code
    (0x0049C570, 0x0049C600, "NgEngine/Zge/Com/ZglRw/zrwModelCreate.c"),
    # DM-042: zanim node-table code (shares globals 0x006746C4/C8 with zanim.c), before ziff.c.
    # DM-045: the whole file is 0x00498D80-0x0049A6CB (zanim.c asserts from 0x00498DD0 on; the
    # track pool 0x006746C4 code starts at 0x00498D80, right after zrwTxr.c's load-buffer code)
    (0x00498D80, 0x0049A6D0, "NgEngine/Zge/zanim.c"),
    # DM-042: input pool code right after zinput.c, before zasset.c
    (0x00480370, 0x00480440, "NgEngine/Zge/zinput.c"),
    # DM-044: zAssertFail 0x0047ED10 moved to zdebug.c (decomp/symbols/symbols.csv rows); the debug-draw code
    # after zdebug.c stays with zrwDbg.c, as DM-040 had it (it was "between" zAssertFail and 0x0047F850)
    (0x0047EE00, 0x0047F850, "NgEngine/Zge/Com/ZglRw/zrwDbg.c"),
    # DM-044: 0x0048C110-0x0048C230 (doubly linked list) moved to zList.c; the movie / app code
    # before it stays with zrwZgl.c, as DM-040 had it
    (0x0048BC60, 0x0048C110, "NgEngine/Zge/Com/ZglRw/zrwZgl.c"),
]
# Files that exist only through DM-040 inference (no path string in the exe); names follow the
# original style but are invented.
NEW_FILES = {"Bmx/Src/com/menu/gui/guimain.c", "NgEngine/Zge/Pc/pcmovie.c", "NgEngine/Zge/zmath.c",
             "NgEngine/Zge/zcompres.c",
             # DM-043: the Panorama object (SDPA handler), between pcmovie.c and the ztoken parser
             "NgEngine/Zge/ZgeMods/zpanorama.c",
             # DM-056: the screen-file loader ("GUISFILE:" literals), between guimain.c and guitoggl.c
             "Bmx/Src/com/menu/gui/guisfile.c",
             # DM-058: the crash rag doll + limb IK (no strings), between bmxsound.c and zoverii.c
             "Bmx/Src/com/zge/bmx/dyn.c",
             # DM-061: the script tokenizer, the timer and its heap (no path strings)
             "NgEngine/Zge/ztoken.c", "NgEngine/Zge/ztimer.c", "NgEngine/Zge/zheap.c"}


def load_anchors(symbols_path):
    """{va: source_file} from hand-curated func rows of decomp/symbols/symbols.csv: any card except the
    generator's own (DM-002/DM-040, whose source_file came from this script), non-library."""
    anchors = {}
    for r in read_symbols(symbols_path):
        if r[1] != "func" or r[6].strip() in GEN_CARDS:
            continue
        src = r[4].strip()
        if src and not src.startswith("lib"):
            anchors[int(r[0], 16)] = src      # later rows win, as in ApplySymbols
    return anchors


def attribute(funcs, dm040=True, anchors=None):
    """Return {va: (source_file, confidence, method)} for game/engine functions.

    method: direct (V) | row | seed | range | strings | orphan | between | callers | split (all I).
    `row` = another card's decomp/symbols/symbols.csv row names the source file (anchors, DM-040 level)."""
    code = [f for f in funcs if not f["thunk"] and f["va"] < LIB_START]
    byva = {f["va"]: f for f in code}
    mod = {}
    # pass 1: direct path-string references
    for f in code:
        paths = sorted({norm_path(t) for _, t in f["strings"] if PATH_RE.match(t)})
        if len(paths) == 1:
            mod[f["va"]] = (paths[0], "V", "direct")
        elif len(paths) > 1:  # should not happen; keep the first, report
            print(f"warning: 0x{f['va']:08x} references several source paths: {paths}", file=sys.stderr)
            mod[f["va"]] = (paths[0], "V", "direct")

    # Module string windows. Literals of one object file are contiguous in .data and the objects
    # are in link order, so a module's window is the span of strings its [V] functions reference,
    # clipped to lie strictly between the previous and the next module's __FILE__ string (shared
    # header literals such as "Already initialized" fall outside and are ignored).
    path_addr = {}
    for f in code:
        for a, t in f["strings"]:
            if PATH_RE.match(t):
                path_addr.setdefault(norm_path(t), a)
    by_addr = sorted(path_addr.items(), key=lambda kv: kv[1])
    windows = {}
    for k, (m, pa) in enumerate(by_addr):
        lo_b = by_addr[k - 1][1] if k > 0 else 0
        hi_b = by_addr[k + 1][1] if k + 1 < len(by_addr) else 1 << 32
        addrs = [a for f in code if mod.get(f["va"], ("",))[0] == m and mod[f["va"]][1] == "V"
                 for a, _ in f["strings"] if lo_b < a < hi_b]
        if addrs:
            windows[m] = (min(addrs), max(addrs))
    # orphan files: literals between the neighbouring path strings that are not inside a
    # neighbour's window
    all_paths = sorted(list(path_addr.items()) + list(ORPHAN_PATHS.items()), key=lambda kv: kv[1])
    owin = {}
    for k, (m, pa) in enumerate(all_paths):
        if m not in ORPHAN_PATHS:
            continue
        lo = all_paths[k - 1][1] + 1 if k > 0 else 0
        hi = all_paths[k + 1][1] - 1 if k + 1 < len(all_paths) else 1 << 32
        if k > 0 and all_paths[k - 1][0] in windows:
            lo = max(lo, windows[all_paths[k - 1][0]][1] + 1)
        if k + 1 < len(all_paths) and all_paths[k + 1][0] in windows:
            hi = min(hi, windows[all_paths[k + 1][0]][0] - 1)
        owin[m] = (lo, hi)

    # seeds and ranges (strong, position-independent evidence)
    if not dm040:
        owin = {}
    for f in (code if dm040 and anchors else []):
        if f["va"] not in mod and f["va"] in anchors:
            mod[f["va"]] = (anchors[f["va"]], "I", "row")
    seeds = [] if not dm040 else [(re.compile(e[0]), e[1], e[2] if len(e) > 2 else (0, 1 << 32)) for e in STRING_SEEDS]
    for f in (code if dm040 else []):
        if f["va"] in mod:
            continue
        hit = {m for a, t in f["strings"] for rx, m, (lo, hi) in seeds if lo <= a < hi and rx.match(t)}
        if len(hit) == 1:
            mod[f["va"]] = (hit.pop(), "I", "seed")
            continue
        for lo, hi, m in RANGE_FILES:
            if lo <= f["va"] < hi:
                mod[f["va"]] = (m, "I", "range")

    order = [f["va"] for f in code]

    def neighbours(i):
        p = n = None
        for j in range(i - 1, -1, -1):
            if order[j] in mod:
                p = mod[order[j]][0]
                break
        for j in range(i + 1, len(order)):
            if order[j] in mod:
                n = mod[order[j]][0]
                break
        return p, n

    def fixpoint():
        changed = True
        while changed:
            changed = False
            # pass 2: string windows; the module must also be the previous or next attributed
            # module in VA order (keeps link order consistent, rejects far-away shared literals)
            for i, f in enumerate(code):
                if f["va"] in mod or not f["strings"]:
                    continue
                cands = set()
                for a, _ in f["strings"]:
                    cands |= {m for m, (lo, hi) in windows.items() if lo <= a <= hi}
                if len(cands) == 1 and next(iter(cands)) in neighbours(i):
                    mod[f["va"]] = (cands.pop(), "I", "strings")
                    changed = True
                    continue
                # orphan files: a literal in exactly one orphan window and in no module window
                oc = {m for a, _ in f["strings"] for m, (lo, hi) in owin.items() if lo <= a <= hi}
                if len(oc) == 1 and not cands:
                    mod[f["va"]] = (oc.pop(), "I", "orphan")
                    changed = True
            # pass 3: between two functions of the same module
            for i, va in enumerate(order):
                if va in mod:
                    continue
                p, n = neighbours(i)
                if p is not None and p == n:
                    mod[va] = (p, "I", "between")
                    changed = True
            # pass 4: all callers in one module that is adjacent in VA order
            for i, va in enumerate(order):
                if va in mod:
                    continue
                cm = {mod[c][0] for c in byva[va]["callers"] if c in mod}
                unknown = [c for c in byva[va]["callers"] if c not in mod]
                if len(cm) == 1 and not unknown:
                    m = next(iter(cm))
                    # strictly adjacent: the directly preceding or following function is in m
                    adj = {mod[order[j]][0] for j in (i - 1, i + 1) if 0 <= j < len(order) and order[j] in mod}
                    if m in adj:
                        mod[va] = (m, "I", "callers")
                        changed = True

    def split():
        """Pass 5 (DM-040): in each gap between files A and B, vote per function by call edges
        to A vs B, pick the split point that agrees with most votes, and assign the functions
        whose vote agrees with their side. Functions without a vote stay unassigned."""
        changed = False
        i = 0
        while i < len(order):
            if order[i] in mod:
                i += 1
                continue
            j = i
            while j < len(order) and order[j] not in mod:
                j += 1
            if i == 0 or j == len(order):
                i = j
                continue
            a, b = mod[order[i - 1]][0], mod[order[j]][0]
            gap = order[i:j]
            votes = []
            for va in gap:
                f = byva[va]
                edges = [c for c in f["callers"] + f["callees"] if c in mod]
                na, nb = sum(mod[c][0] == a for c in edges), sum(mod[c][0] == b for c in edges)
                votes.append("a" if na > nb else "b" if nb > na else None)
            best, bk = -1, 0
            for k in range(len(gap) + 1):
                sc = sum(v == "a" for v in votes[:k]) + sum(v == "b" for v in votes[k:])
                if sc > best:
                    best, bk = sc, k
            for k, (va, v) in enumerate(zip(gap, votes)):
                if v == "a" and k < bk:
                    mod[va] = (a, "I", "split")
                    changed = True
                elif v == "b" and k >= bk:
                    mod[va] = (b, "I", "split")
                    changed = True
            i = j
        return changed

    fixpoint()
    while dm040 and split():
        fixpoint()
    return mod, code


def split_png_zlib(funcs):
    lo, hi = 0x00506E40, 0x0051CE10
    byva = {f["va"]: f for f in funcs}
    z = set()
    todo = [v for v in ZLIB_SEEDS if v in byva]
    while todo:
        v = todo.pop()
        if v in z or not (lo <= v < hi):
            continue
        z.add(v)
        todo.extend(byva[v]["callees"])
    return z


def read_symbols(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", newline="") as f:
        rd = csv.reader(f)
        hdr = next(rd, None)
        if hdr != HEADER:
            sys.exit(f"error: {path}: header must be {','.join(HEADER)}")
        return [r + [""] * (7 - len(r)) for r in rd if r and any(c.strip() for c in r)]


def write_symbols(path, rows):
    rows = sorted(rows, key=lambda r: int(r[0], 16))  # stable: duplicate addresses keep their order
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(HEADER)
    for r in rows:
        r = list(r)
        r[0] = "0x%08x" % int(r[0], 16)
        w.writerow(r)
    return buf.getvalue()


def module_table(mod, code, funcs_by_va):
    order = [f["va"] for f in code]
    pos = {va: i for i, va in enumerate(order)}
    per = defaultdict(list)
    for va, (m, c, how) in mod.items():
        per[m].append(va)
    rows = []
    for m, vas in per.items():
        vas.sort()
        runs = []
        start = prev = vas[0]
        for v in vas[1:]:
            if pos[v] != pos[prev] + 1:
                runs.append((start, prev))
                start = v
            prev = v
        runs.append((start, prev))
        hows = Counter(mod[v][2] for v in vas)
        nbytes = sum(funcs_by_va[v]["size"] for v in vas)
        rows.append((vas[0], m, runs, len(vas), hows, nbytes))
    rows.sort()
    return rows


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--functions", default=os.path.join(root, "decomp", "symbols", "functions.csv"))
    ap.add_argument("--symbols", default=os.path.join(root, "decomp", "symbols", "symbols.csv"))
    ap.add_argument("--module-map", default=os.path.join(root, "docs", "re", "module-map.md"))
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    a = ap.parse_args()

    funcs = load_functions(a.functions)
    fby = {f["va"]: f for f in funcs}
    mod, code = attribute(funcs, anchors=load_anchors(a.symbols))
    mod02, _ = attribute(funcs, dm040=False)

    def gen_card(va):
        return CARD if mod02.get(va, (None,))[0] == mod[va][0] else CARD40
    zlib = split_png_zlib(funcs)

    # library classification of every non-thunk function
    lib = {}
    for f in funcs:
        if f["thunk"]:
            continue
        l = lib_of(f["va"])
        if l == "lib:libpng" and f["va"] in zlib:
            l = "lib:zlib"
        if l:
            lib[f["va"]] = l

    # ---- merge into symbols.csv
    rows = read_symbols(a.symbols)

    def is_auto(r):
        """A row this script generated earlier (regenerated on every run, so stale ones vanish)."""
        if r[6].strip() not in GEN_CARDS or r[1] != "func" or r[3].strip():
            return False
        name, src = r[2].strip(), r[4].strip()
        if not name:
            return src.startswith("lib:")
        return bool(src) and name == "%s_%08x" % (stem(src), int(r[0], 16))

    n_old = len(rows)
    rows = [r for r in rows if not is_auto(r)]
    n_dropped = n_old - len(rows)
    have = {int(r[0], 16) for r in rows if r[1] != "struct"}
    n_fill = n_new = 0
    for r in rows:
        va = int(r[0], 16)
        if r[6].strip() in GEN_CARDS and r[1] == "func" and not r[4].strip():
            if va in mod:
                r[4] = mod[va][0]
                n_fill += 1
            elif va in lib:
                r[4] = lib[va]
                n_fill += 1
    for va in sorted(set(mod) | set(lib)):
        if va in have:
            continue
        if va in mod:
            m, conf, _ = mod[va]
            rows.append(["0x%08x" % va, "func", "%s_%08x" % (stem(m), va), "", m, conf, gen_card(va)])
        else:
            # library: keep Ghidra's name (Function ID names like _strlen survive), tag only
            rows.append(["0x%08x" % va, "func", "", "", lib[va], "I", CARD])
        n_new += 1
    text = write_symbols(a.symbols, rows)

    # ---- report
    n_code = len([f for f in funcs if not f["thunk"]])
    n_thunk = len(funcs) - n_code
    game = [f for f in code]
    n_game_attr = sum(1 for f in game if f["va"] in mod)
    libc = Counter(lib.values())
    table = module_table(mod, code, fby)
    unassigned = [f for f in game if f["va"] not in mod]

    out = io.StringIO()
    p = lambda *x: print(*x, file=out)
    p("<!-- BEGIN GENERATED by tools/ghidra/build_symbols.py; do not edit by hand -->")
    p()
    p("### Totals [V]")
    p()
    p("| What | Functions |")
    p("|---|---:|")
    p(f"| All functions in Ghidra (incl. thunks) | {len(funcs)} |")
    ilt = [f for f in funcs if f["thunk"] and f["va"] < CODE_START]
    p(f"| jmp thunks (ILT 0x{ilt[0]['va']:08x}-0x{ilt[-1]['va'] + ilt[-1]['size'] - 1:08x}: {len(ilt)}; elsewhere: {n_thunk - len(ilt)}) | {n_thunk} |" if ilt else f"| jmp thunks | {n_thunk} |")
    p(f"| Game + engine code (below 0x{LIB_START:08x}, non-thunk) | {len(game)} |")
    p(f"| ... attributed to a source file | {n_game_attr} ({100 * n_game_attr // max(1, len(game))}%) |")
    p(f"| ... of which [V] direct path reference | {sum(1 for v in mod.values() if v[1] == 'V')} |")
    p(f"| ... unattributed | {len(unassigned)} |")
    for l, n in sorted(libc.items()):
        p(f"| Library `{l}` | {n} |")
    p()
    p("### Source file -> VA ranges")
    p()
    p("Sorted by first VA (= link order). `V` = functions that reference the path string themselves;")
    p("`I` = inferred, by method: `s` string in the file's .data string window, `b` between two functions of the file,")
    p("`c` all callers in the file (DM-002); `e` seed string naming the file/function, `r` manual range, `o` literal")
    p("in an orphan path string's window, `x` call-graph split of a gap, `w` another card's symbols.csv row names the file (DM-040). Files marked * have no path string")
    p("in the exe; their name is invented in the original style (DM-040).")
    p("Confidence: **high** = one contiguous run with >= 1 V function and at most 1/4 split/range; **medium** = has V, seed or orphan evidence; **low** = only placed by range/split/adjacency.")
    p()
    p("| # | Source file | VA range(s) | Funcs | V | I (s/b/c/e/r/o/x/w) | Bytes | Conf |")
    p("|---:|---|---|---:|---:|---|---:|---|")
    for i, (first, m, runs, n, hows, nbytes) in enumerate(table, 1):
        rs = ", ".join(f"0x{lo:08x}-0x{fby[hi]['va'] + fby[hi]['size'] - 1:08x}" if lo != hi else f"0x{lo:08x}" for lo, hi in runs)
        if len(runs) > 3:
            rs = ", ".join(f"0x{lo:08x}-0x{fby[hi]['va'] + fby[hi]['size'] - 1:08x}" for lo, hi in runs[:3]) + f", ... ({len(runs)} runs)"
        nv = hows.get("direct", 0)
        ni = "/".join(str(hows.get(k, 0)) for k in ("strings", "between", "callers", "seed", "range", "orphan", "split", "row"))
        weak = hows.get("split", 0) + hows.get("range", 0)
        if len(runs) == 1 and nv >= 1 and weak * 4 <= n:
            conf = "high"
        elif nv >= 1 or hows.get("seed", 0) or hows.get("orphan", 0):
            conf = "medium"
        else:
            conf = "low"
        star = "*" if m in NEW_FILES else ""
        p(f"| {i} | `{m}`{star} | {rs} | {n} | {nv} | {ni} | {nbytes} | {conf} |")
    p()
    # gaps
    p("### Unattributed game/engine code (gaps between modules)")
    p()
    p("Each gap lies between the last function of the previous file and the first of the next one;")
    p("its functions belong to one of the two neighbours or to a file with no path string (DM-040).")
    p()
    p("| Gap VA range | Funcs | Bytes | Previous file | Next file |")
    p("|---|---:|---:|---|---|")
    order = [f["va"] for f in code]
    i = 0
    gaps = []
    while i < len(order):
        if order[i] in mod:
            i += 1
            continue
        j = i
        while j < len(order) and order[j] not in mod:
            j += 1
        prev = mod[order[i - 1]][0] if i > 0 else "(start)"
        nxt = mod[order[j]][0] if j < len(order) else "(libraries)"
        vas = order[i:j]
        gaps.append((vas[0], vas[-1], len(vas), sum(fby[v]["size"] for v in vas), prev, nxt))
        i = j
    for lo, hi, n, b, prev, nxt in gaps:
        p(f"| 0x{lo:08x}-0x{hi + fby[hi]['size'] - 1:08x} | {n} | {b} | `{stem(prev)}` | `{stem(nxt)}` |")
    p()
    p("<!-- END GENERATED -->")
    gen = out.getvalue()

    print(f"functions={len(funcs)} thunks={n_thunk} game+engine={len(game)} attributed={n_game_attr} "
          f"(V={sum(1 for v in mod.values() if v[1] == 'V')}) modules={len(table)} gaps={len(gaps)} "
          f"libs={dict(sorted(libc.items()))}")
    print(f"symbols.csv: {len(rows)} rows ({n_new} generated, {n_dropped} previous generated rows replaced, {n_fill} filled)")
    # order check: module first-VA order should follow the path-string order in .data
    if a.check:
        return
    with open(a.symbols, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    if os.path.exists(a.module_map):
        with open(a.module_map, encoding="utf-8") as f:
            doc = f.read()
        b = doc.find("<!-- BEGIN GENERATED")
        e = doc.find("<!-- END GENERATED -->")
        if b >= 0 and e >= 0:
            doc = doc[:b] + gen.rstrip("\n") + doc[e + len("<!-- END GENERATED -->"):]
        else:
            doc = doc.rstrip("\n") + "\n\n" + gen
    else:
        doc = "# Module map\n\n" + gen
    with open(a.module_map, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
    print(f"wrote {a.symbols} and {a.module_map}")


if __name__ == "__main__":
    main()

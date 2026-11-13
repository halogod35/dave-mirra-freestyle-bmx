#!/usr/bin/env bash
# Apply decomp/symbols/symbols.csv to the canonical Ghidra project and re-export decomp/symbols/functions.csv (AD-6).
# Idempotent: running it twice gives the same project state and the same functions.csv.
#
# The Ghidra project is <main checkout>/.ghidra/project/NgBMX (gitignored, shared by worktrees);
# symbols.csv / functions.csv / scripts come from THIS checkout. Only the RE agent that owns the
# project should run this (it writes the project); everyone else just edits symbols.csv.
set -u

usage() {
    cat <<EOF
Usage: tools/ghidra/apply_symbols.sh [options]

  --csv FILE         symbols CSV to apply (default: <checkout>/decomp/symbols/symbols.csv)
  --out FILE         functions CSV to write (default: <checkout>/decomp/symbols/functions.csv)
  --no-apply         only export functions.csv (also skips the type import)
  --no-types         do not import decomp/include types (ImportTypes.java) before applying
  --no-export        only apply symbols
  --decompile [DIR]  also decompile all non-library functions to DIR (default: <checkout>/out/ghidra_c)
  --decompile-libs   with --decompile: include library functions (lib_<name>.c files)
  -h, --help         this help

Environment: DMBMX_GHIDRA_PROJECT overrides the project directory.
EOF
}

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
COMMON_GIT="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
if [ -n "$COMMON_GIT" ]; then MAIN_REPO="$(cd "$COMMON_GIT/.." && pwd)"; else MAIN_REPO="$ROOT"; fi
winpath() { cygpath -w "$1" 2>/dev/null || printf '%s\n' "$1"; }

CSV="$ROOT/decomp/symbols/symbols.csv"
OUT="$ROOT/decomp/symbols/functions.csv"
TYPES=1; APPLY=1; EXPORT=1; DECOMP=""; DECOMP_ALL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --csv) CSV="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --no-apply) APPLY=0; shift ;;
        --no-types) TYPES=0; shift ;;
        --no-export) EXPORT=0; shift ;;
        --decompile)
            if [ $# -gt 1 ] && [ "${2#-}" = "$2" ]; then DECOMP="$2"; shift 2; else DECOMP="$ROOT/out/ghidra_c"; shift; fi ;;
        --decompile-libs) DECOMP_ALL=all; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

absf() { case "$1" in /*|[A-Za-z]:*) printf '%s
' "$1" ;; *) printf '%s/%s
' "$(pwd)" "$1" ;; esac; }
CSV="$(absf "$CSV")"; OUT="$(absf "$OUT")"; [ -n "$DECOMP" ] && DECOMP="$(absf "$DECOMP")"
PROJ="${DMBMX_GHIDRA_PROJECT:-$MAIN_REPO/.ghidra/project}"
if [ ! -f "$PROJ/NgBMX.gpr" ]; then
    echo "error: no Ghidra project at $PROJ (create it per docs/re/module-map.md, section 'Rebuilding')" >&2
    exit 2
fi
[ "$APPLY" = 1 ] && [ ! -f "$CSV" ] && { echo "error: $CSV not found" >&2; exit 2; }

args=("$(winpath "$PROJ")" NgBMX -process NgBMX.exe -noanalysis -scriptPath "$(winpath "$HERE")")
INC="$ROOT/decomp/include"
[ "$APPLY" = 1 ] && [ "$TYPES" = 1 ] && [ -f "$INC/ghidra_types.txt" ] && args+=(-postScript ImportTypes.java "$(winpath "$INC")")
[ "$APPLY" = 1 ] && args+=(-postScript ApplySymbols.java "$(winpath "$CSV")")
[ "$EXPORT" = 1 ] && args+=(-postScript ExportFunctions.java "$(winpath "$OUT")")
if [ -n "$DECOMP" ]; then mkdir -p "$DECOMP"; args+=(-postScript DecompileAll.java "$(winpath "$DECOMP")" ${DECOMP_ALL:+"$DECOMP_ALL"}); fi

LOG="$ROOT/out/apply_symbols.log"
mkdir -p "$ROOT/out"
"$HERE/ghidra.sh" headless "${args[@]}" > "$LOG" 2>&1
rc=$?
# Show our scripts' output and any errors.
grep -E "ImportTypes|ApplySymbols|ExportFunctions|DecompileAll|ERROR|SCRIPT ERROR|Exception" "$LOG" | sed 's/^INFO  //'
if [ $rc -ne 0 ] || grep -q "SCRIPT ERROR\|REPORT SCRIPT ERROR" "$LOG"; then
    echo "apply_symbols: FAILED (exit $rc), full log: $LOG" >&2
    exit 1
fi
echo "apply_symbols: ok (log: $LOG)"

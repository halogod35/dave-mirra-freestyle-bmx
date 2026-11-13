#!/usr/bin/env bash
# Verify game trees against PRISTINE.sha256. Read-only: never writes anything.
set -u

usage() {
    cat <<'EOF'
Usage: tools/testing/check_pristine.sh [-q] [TARGET...]

Verify game file trees against the manifest PRISTINE.sha256 (paths in it are
relative to the tree root, e.g. "./NgBMX.exe"). Read-only.

TARGET is one or more of:
  pristine   <game>/_pristine     (default)
  install    <game>               (the user's install; work folders are ignored)
  testgame   <game>/_testgame
  all        all three
With no TARGET, checks "pristine install".

Reports per target: files that are MISSING, files whose hash DIFFERS, and EXTRA
files not in the manifest. Exit status 0 = all clean, 1 = differences, 2 = usage
or setup error.

  -q, --quiet   only print the per-target summary lines
  -h, --help    this help

Environment:
  DMBMX_GAME_DIR   game install dir (default: F:\Projects\Dave Mirra freestyle BMX)
EOF
}

GAME="${DMBMX_GAME_DIR:-F:/Projects/Dave Mirra freestyle BMX}"
MANIFEST="$GAME/PRISTINE.sha256"
QUIET=0
TARGETS=()
for a in "$@"; do
    case "$a" in
        -h|--help) usage; exit 0 ;;
        -q|--quiet) QUIET=1 ;;
        pristine|install|testgame) TARGETS+=("$a") ;;
        all) TARGETS+=(pristine install testgame) ;;
        *) echo "error: unknown argument '$a' (see --help)" >&2; exit 2 ;;
    esac
done
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(pristine install)

if [ ! -f "$MANIFEST" ]; then
    echo "error: manifest not found: $MANIFEST (set DMBMX_GAME_DIR to the game install; to create the manifest see docs/tooling.md, section Game trees)" >&2
    exit 2
fi

# Manifest paths, normalised to "./rel/path", one per line.
manifest_paths() { sed -E 's/^[0-9a-fA-F]{64} [ *]//; s/\r$//' "$MANIFEST" | sort; }

# check_tree NAME DIR [prune-dirs...]
check_tree() {
    local name="$1" dir="$2"; shift 2
    if [ ! -d "$dir" ]; then
        echo "[$name] FAIL: directory not found: $dir"
        return 1
    fi
    local out missing differ extra
    # sha256sum -c: "<path>: OK" / "<path>: FAILED" / "<path>: FAILED open or read"
    out="$(cd "$dir" && sed 's/\r$//' "$MANIFEST" | sha256sum -c - 2>/dev/null)"
    missing="$(printf '%s\n' "$out" | sed -n 's/: FAILED open or read$//p')"
    differ="$(printf '%s\n' "$out" | sed -n 's/: FAILED$//p')"
    # Extra files: everything on disk minus the manifest (minus pruned work folders).
    local prune=()
    for p in "$@"; do prune+=(-path "./$p" -prune -o); done
    extra="$(cd "$dir" && comm -23 <(find . "${prune[@]}" -type f -print | sort) <(manifest_paths))"

    local nm nd ne total
    nm=$(printf '%s' "$missing" | grep -c . ); nd=$(printf '%s' "$differ" | grep -c . ); ne=$(printf '%s' "$extra" | grep -c . )
    total=$(manifest_paths | grep -c .)
    if [ "$nm$nd$ne" = "000" ]; then
        echo "[$name] OK: $total files match ($dir)"
        return 0
    fi
    echo "[$name] FAIL: $nm missing, $nd differ, $ne extra (of $total manifest files) ($dir)"
    if [ $QUIET -eq 0 ]; then
        [ -n "$missing" ] && printf '%s\n' "$missing" | sed 's/^/    MISSING /'
        [ -n "$differ" ]  && printf '%s\n' "$differ"  | sed 's/^/    DIFFERS /'
        [ -n "$extra" ]   && printf '%s\n' "$extra"   | sed 's/^/    EXTRA   /'
    fi
    return 1
}

rc=0
for t in "${TARGETS[@]}"; do
    case "$t" in
        pristine) check_tree pristine "$GAME/_pristine" || rc=1 ;;
        install)  check_tree install "$GAME" _pristine _testgame wt dave-mirra-bmx dave-mirra-pro-bmx PRISTINE.sha256 || rc=1 ;;
        testgame) check_tree testgame "$GAME/_testgame" || rc=1 ;;
    esac
done
exit $rc

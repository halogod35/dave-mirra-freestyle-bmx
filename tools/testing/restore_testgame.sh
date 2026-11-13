#!/usr/bin/env bash
# Restore <game>/_testgame from <game>/_pristine and verify it against PRISTINE.sha256.
set -u

usage() {
    cat <<'EOF'
Usage: tools/testing/restore_testgame.sh [--check] [-h]

Make <game>/_testgame an exact copy of <game>/_pristine again:
  1. verify _pristine against PRISTINE.sha256 (abort if it is damaged),
  2. delete every file in _testgame that is not in the manifest (e.g. play-test
     extras, files the game wrote), and copy back every manifest file that is
     missing or differs,
  3. verify _testgame against the manifest.
Idempotent: on an already-clean _testgame it copies nothing.
Never touches the install itself or _pristine.

  --check     only verify _testgame (same as tools/testing/check_pristine.sh testgame)
  -h, --help  this help

Exit status 0 = _testgame is clean, 1 = restore/verify failed, 2 = setup error.

Environment:
  DMBMX_GAME_DIR   game install dir (default: F:\Projects\Dave Mirra freestyle BMX)
EOF
}

HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="${DMBMX_GAME_DIR:-F:/Projects/Dave Mirra freestyle BMX}"
MANIFEST="$GAME/PRISTINE.sha256"
SRC="$GAME/_pristine"
DST="$GAME/_testgame"
CHECK_ONLY=0
for a in "$@"; do
    case "$a" in
        -h|--help) usage; exit 0 ;;
        --check) CHECK_ONLY=1 ;;
        *) echo "error: unknown argument '$a' (see --help)" >&2; exit 2 ;;
    esac
done

[ $CHECK_ONLY -eq 1 ] && exec bash "$HERE/check_pristine.sh" testgame

for p in "$MANIFEST" "$SRC"; do
    [ -e "$p" ] || { echo "error: not found: $p (set DMBMX_GAME_DIR to the game install)" >&2; exit 2; }
done

echo "== verifying source _pristine"
bash "$HERE/check_pristine.sh" pristine || {
    echo "error: _pristine does not match the manifest; refusing to copy from it." >&2
    exit 1
}

mkdir -p "$DST"
# Manifest lines "<sha256> *./rel/path" -> rel/path
mapfile -t want < <(sed -E 's/\r$//; s/^[0-9a-fA-F]{64} [ *]\.?\/?//' "$MANIFEST")
declare -A inman=()
for f in "${want[@]}"; do inman["$f"]=1; done

removed=0
while IFS= read -r -d '' f; do
    rel="${f#./}"
    if [ -z "${inman[$rel]+x}" ]; then
        rm -f -- "$DST/$rel" && echo "   removed extra: $rel"
        removed=$((removed + 1))
    fi
done < <(cd "$DST" && find . -type f -print0)
# drop now-empty directories that pristine does not have
(cd "$DST" && find . -mindepth 1 -depth -type d -empty -print0) | while IFS= read -r -d '' d; do
    [ -d "$SRC/${d#./}" ] || rmdir -- "$DST/${d#./}"
done

# Copy files that are missing or differ (compare hashes against the manifest).
copied=0
mapfile -t bad < <(cd "$DST" && sed 's/\r$//' "$MANIFEST" | sha256sum -c - 2>/dev/null | sed -nE 's/: FAILED( open or read)?$//p')
for f in "${bad[@]}"; do
    rel="${f#./}"
    mkdir -p "$(dirname "$DST/$rel")"
    rm -f -- "$DST/$rel"
    cp -p -- "$SRC/$rel" "$DST/$rel" && echo "   restored: $rel"
    copied=$((copied + 1))
done
echo "== removed $removed extra file(s), restored $copied file(s)"

echo "== verifying _testgame"
bash "$HERE/check_pristine.sh" testgame

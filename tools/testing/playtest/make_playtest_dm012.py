"""Build out/playtest/dm-012/ (DM-012 play-test): edited saves + install.sh + MANIFEST + REQUEST.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_dm012.py [--game DIR] [--out DIR]
Reads the save from _pristine (read-only), never touches the install.
"""
import argparse
import hashlib
import os
from pathlib import Path

from dmbmx.formats import save
from dmbmx.integrity.crc import check_save

REPO = Path(__file__).resolve().parents[3]
NEW_NAME = "DM012"
NEW_SCORE = 999999


def edited(src):
    s = save.read(src)
    old_name = s.profiles[0].name
    s.profiles[0].name = NEW_NAME
    for row in s.scores:
        row[0].score = NEW_SCORE
    return s, old_name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-012"))
    a = ap.parse_args()
    src = (Path(a.game) / "_pristine" / "save" / "SAVE.DAT").read_bytes()
    out = Path(a.out)

    s, old_name = edited(src)
    good = save.write(s.resign())                      # re-signed: correct magic + size (no CRC exists)
    s.size_field = save.SIZE + 1                       # the one field the game validates
    bad = save.write(s)
    s.size_field = save.SIZE
    s.magic = 0x21444142                               # "BAD!": magic is NOT checked by the game
    nomagic = save.write(s)
    variants = {"good": good, "bad": bad, "nomagic": nomagic}
    expect = {"good": True, "bad": False, "nomagic": True}
    for k, v in variants.items():
        if check_save(v)[0] != expect[k]:
            raise SystemExit(f"internal error: variant {k} has the wrong acceptance")
        p = out / "files" / k / "save" / "SAVE.DAT"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(v)
    lines = []
    for p in sorted((out / "files").rglob("*")):
        if p.is_file():
            rel = p.relative_to(out / "files").as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(install_sh("DM-012"), newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.format(out=out.as_posix(), old=old_name, name=NEW_NAME,
                                                   score=NEW_SCORE), newline="\n")
    diff = [i for i, (x, y) in enumerate(zip(src, good)) if x != y]
    print(f"wrote {out}")
    print(f"good: {len(diff)} bytes differ from pristine (profile 0 name {old_name!r} -> {NEW_NAME!r}, "
          f"12 x score entry 0 -> {NEW_SCORE})")
    print("bad: same edits + size field 0x235D;  nomagic: same edits + magic 'BAD!'")
    print("".join(lines), end="")


def install_sh(card):
    """Generic play-test installer: install.sh <variant>|restore, where a variant is a folder under files/."""
    return INSTALL_SH.replace("@CARD@", card)


INSTALL_SH = r'''#!/usr/bin/env bash
# @CARD@ play-test installer. Usage: install.sh <variant>|restore   (--restore = restore)
# A variant is a folder under files/, laid out like the install root.
# Touches only _testgame (AD-1). Never starts the game.
set -euo pipefail
GAME="F:/Projects/Dave Mirra freestyle BMX"
TEST="$GAME/_testgame"
PRIS="$GAME/_pristine"
HERE="$(cd "$(dirname "$0")" && pwd)"
CARD="@CARD@"

case "$(cd "$TEST" && pwd -P)" in
  */_testgame) ;;
  *) echo "refusing: target is not _testgame" >&2; exit 1 ;;
esac

restore() {
  echo "Verifying _testgame against PRISTINE.sha256 ..."
  local bad
  bad="$(cd "$TEST" && sha256sum -c "$GAME/PRISTINE.sha256" 2>/dev/null | grep -v ': OK$' | sed -E 's/: FAILED.*$//' || true)"
  if [ -n "$bad" ]; then
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      echo "  restoring $f"
      mkdir -p "$(dirname "$TEST/$f")"
      cp -f "$PRIS/$f" "$TEST/$f"
    done <<< "$bad"
  fi
  if [ -f "$TEST/.playtest_added" ]; then
    while IFS= read -r f; do [ -n "$f" ] && rm -f "$TEST/$f"; done < "$TEST/.playtest_added"
    rm -f "$TEST/.playtest_added"
  fi
  (cd "$TEST" && sha256sum -c --quiet "$GAME/PRISTINE.sha256")
  echo "_testgame restored, $(wc -l < "$GAME/PRISTINE.sha256") files verified"
}

MODE="${1:-}"
case "$MODE" in
  restore|--restore) restore; rm -f "$TEST/.playtest"; exit 0 ;;
  ""|*/*|.*) MODE="" ;;
esac
if [ -z "$MODE" ] || [ ! -d "$HERE/files/$MODE" ]; then
  echo "usage: $0 <variant>|restore   variants: $(ls "$HERE/files" | tr '\n' ' ')" >&2
  exit 2
fi

restore
echo "Verifying play-test files ..."
(cd "$HERE/files" && sha256sum -c --quiet "$HERE/MANIFEST.sha256")
SRC="$HERE/files/$MODE"
(cd "$SRC" && find . -type f) | while IFS= read -r f; do
  f="${f#./}"
  if [ -e "$TEST/$f" ]; then
    old="$(sha256sum "$TEST/$f" | cut -c1-64)"
    cp -f "$SRC/$f" "$TEST/$f"
    echo "  replaced $f  old=$old  new=$(sha256sum "$TEST/$f" | cut -c1-64)"
  else
    mkdir -p "$(dirname "$TEST/$f")"
    cp -f "$SRC/$f" "$TEST/$f"
    echo "$f" >> "$TEST/.playtest_added"      # removed again by restore
    echo "  added    $f  new=$(sha256sum "$TEST/$f" | cut -c1-64)"
  fi
done
printf '%s %s %s\n' "$CARD" "$MODE" "$(date -Iseconds)" > "$TEST/.playtest"
echo "Ready. Start the game in _testgame as REQUEST.md says. Afterwards run: install.sh --restore"
'''

REQUEST = r'''### Play-test request DM-012-PT1
**Goal:** prove that the game accepts an edited SAVE.DAT (SAVE.DAT has no CRC: only the size field at offset 4 is checked), and rejects one whose size field is wrong.

Background: in the current save, profile 0 is named "{old}". The "good" save renames profile 0 to "{name}" and sets record 0 of all 12 levels (the big best-score value, normally {old}'s) to {score}. "bad" is the same file with the size field at offset 4 changed from 0x235C to 0x235D. "nomagic" is the same as good but with the 4 magic bytes changed to "BAD!" (the exe does not check them, so it should load like good).

**Prepare (Git Bash):**
    bash "{out}/install.sh" good
  Expected output ends with "Ready." It lists the replaced file: save/SAVE.DAT.

**Steps (part A, good save):**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`. Let it start up to the main menu (it loads the save at start-up).
2. Open the rider/profile or name screen (wherever you normally see the name "{old}").
3. Open the high-score / records screen for any level (any park).
4. Quit the game normally.

**Expected result (A):**
- No "LOAD FAILED." / "FILE MAY BE CORRUPTED." message at start-up.
- Step 2: the name shows **{name}** instead of "{old}".
- Step 3: the top/best score shows **{score}** (by "{old}"). Other progress (unlocked levels/riders) is unchanged.

**Steps (part B, bad save):** run `bash "{out}/install.sh" bad`, then repeat steps 1-4.

**Expected result (B):** the game rejects the file: probably a "LOAD FAILED." message (text id 0x261), or a silent fallback to default data (profile names "player", default records, no unlocks). You should NOT see {name} or {score}. The game may then overwrite SAVE.DAT with defaults when it saves (only _testgame is affected).

**Optional part C (nomagic):** `bash "{out}/install.sh" nomagic`, then repeat steps 1-4. Expected: same as A (proves the magic bytes are not checked).

**Failure looks like:** in A: "LOAD FAILED.", "FILE MAY BE CORRUPTED.", a crash, or default data (no {name}/{score}). In B: the edited data loads anyway (that would mean the size check is not on this code path).
**Please report:** pass/fail per part and step, any message text verbatim, a screenshot of steps 2-3 (Win+PrtScn), whether the game crashed, and the menu path where you found the name and the scores.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes (5 for A+B).
'''

if __name__ == "__main__":
    main()

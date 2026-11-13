"""Build out/playtest/complete-all/: the user's save + `dmbmx save complete-all`.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_complete_all.py [--game DIR] [--out DIR]
Reads the save from _pristine (read-only); writes only under out/.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path

from dmbmx.formats import progress as P
from dmbmx.formats import save
from dmbmx.integrity.crc import check_save

REPO = Path(__file__).resolve().parents[3]


def _install_sh(package):
    spec = importlib.util.spec_from_file_location("pt_save_edit", Path(__file__).with_name("make_playtest_save_edit.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.install_sh(package)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "complete-all"))
    a = ap.parse_args()
    src = (Path(a.game) / "_pristine" / "save" / "SAVE.DAT").read_bytes()
    out = Path(a.out)

    s = save.read(src)
    applied = P.complete_all(s)
    data = save.write(s.resign())
    if not check_save(data)[0] or save.write(save.read(data)) != data:
        raise SystemExit("internal error: complete-all save is not valid")
    p = out / "files" / "completeall" / "save" / "SAVE.DAT"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    rel = p.relative_to(out / "files").as_posix()
    manifest = f"{hashlib.sha256(data).hexdigest()} *./{rel}\n"
    (out / "MANIFEST.sha256").write_text(manifest, newline="\n")
    (out / "install.sh").write_text(_install_sh("complete-all"), newline="\n")
    (out / "CHECK.md").write_text(CHECK.format(out=out.as_posix()), newline="\n")
    print(f"wrote {out}: {sum(len(v) for v in applied.values())} rewards applied")
    print(manifest, end="")


CHECK = r'''### Manual in-game check: complete-all
**Goal:** prove that a save edited with `dmbmx save complete-all` loads and that the game shows every challenge as done and every competition as won (and the unlocks those give).

**Prepare (Git Bash):**
    bash "{out}/install.sh" completeall
  Expected output ends with "Ready." It lists the replaced file: save/SAVE.DAT.

**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe` and go to the main menu (no "LOAD FAILED." message).
2. Start a ProQuest game, pick **Tim Mirra** (a rider you never played), any bike/style, and go to SELECT LEVEL.
3. Pick **Greenville Park** and start; on the challenge screen look at the list (the game opens it at the start of the run).
4. Pause, choose "view challenges"; then end the run and exit to the menu.
5. Pick **Joey Garcia**, select level **Woodward Trails**, start, and look at the challenge screen again.
6. Quit the game normally.

**Expected result:**
- Step 2: all 12 levels selectable for Tim Mirra; 5 bikes, 5 styles.
- Steps 3-5: the challenge screen shows the **hardcore** challenges and "All challenges complete!" (text "#bNO #BCHALLENGES..." would mean failure); no challenge is shown as open.
- The rider list has 12 riders (Amish Boy last); the CHEATS menu lists all 8 cheats; MOVIES lists Dave Mirra, Ryan Nyquist and Amish Boy.

**Failure looks like:** "LOAD FAILED." / "FILE MAY BE CORRUPTED." at start-up, a crash on the challenge screen, challenges shown as open, or names/scores reset.
**Please report:** pass/fail per step, any message text verbatim, screenshots of steps 3-5 (Win+PrtScn), whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

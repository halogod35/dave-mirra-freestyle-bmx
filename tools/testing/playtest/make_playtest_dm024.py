"""Build out/playtest/dm-024/ (DM-024-PT2): the user's save + `dmbmx save unlock-all`.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_dm024.py [--game DIR] [--out DIR]
Reads the save from _pristine (read-only); writes only under out/.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path

from dmbmx.formats import save
from dmbmx.formats import save_fields as F
from dmbmx.integrity.crc import check_save

REPO = Path(__file__).resolve().parents[3]


def _install_sh(card):
    spec = importlib.util.spec_from_file_location("pt012", Path(__file__).with_name("make_playtest_dm012.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.install_sh(card)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-024"))
    a = ap.parse_args()
    src = (Path(a.game) / "_pristine" / "save" / "SAVE.DAT").read_bytes()
    out = Path(a.out)

    s = save.read(src)
    before = {n: F.get(s, n) for n in ("riders_unlocked", "cheats_unlocked", "rider.5.levels_unlocked",
                                       "rider.5.styles_unlocked", "rider.5.bikes_unlocked",
                                       "rider.1.styles_unlocked", "rider.3.levels_unlocked")}
    changes = F.unlock_all(s)
    data = save.write(s.resign())
    if not check_save(data)[0] or save.write(save.read(data)) != data:
        raise SystemExit("internal error: unlock-all save is not valid")
    p = out / "files" / "unlockall" / "save" / "SAVE.DAT"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    rel = p.relative_to(out / "files").as_posix()
    manifest = f"{hashlib.sha256(data).hexdigest()} *./{rel}\n"
    (out / "MANIFEST.sha256").write_text(manifest, newline="\n")
    (out / "install.sh").write_text(_install_sh("DM-024"), newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.format(out=out.as_posix(), n=len(changes), **{
        k.replace(".", "_"): v for k, v in before.items()}), newline="\n")
    print(f"wrote {out}: {len(changes)} fields changed by unlock-all")
    for n, o, v in changes:
        print(f"  {n}: {o} -> {v}")
    print(manifest, end="")


REQUEST = r'''### Play-test request DM-024-PT2
**Goal:** prove that a save edited with `dmbmx save unlock-all` loads and that the unlocks show in the menus (all riders, levels, styles, bikes, cheats and three movies).

Your save is unchanged except for {n} unlock fields (no progress, scores or names change). Before → after:
- selectable riders: {riders_unlocked} → 12 (**Amish Boy** added after Slim Jim Guy)
- Tim Mirra: levels {rider_5_levels_unlocked}, styles {rider_5_styles_unlocked}, bikes {rider_5_bikes_unlocked} → 12 / 5 / 5
- Mike Laird: levels {rider_3_levels_unlocked} → 12; Ryan Nyquist: styles {rider_1_styles_unlocked} → 5
- every rider: 12 levels, 5 styles, 5 bikes (the 5th bike is the "SECRET BIKE")
- unlocked cheats: {cheats_unlocked} → all 8 (First Person Cam, Night Vision, Super Crash, Exorcist, Suspension, Silly Grunt, Ghost Rider, Sticky Crash)
- MOVIES menu: Dave Mirra, Ryan Nyquist and Amish Boy movies added

**Prepare (Git Bash):**
    bash "{out}/install.sh" unlockall
  Expected output ends with "Ready." It lists the replaced file: save/SAVE.DAT.

**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe` and go to the main menu (no "LOAD FAILED." message).
2. Start a single-player career/ProQuest game and go to SELECT RIDER. Scroll to the end of the rider list.
3. Pick **Tim Mirra** (a rider you never played). On SELECT BIKE scroll through all bikes; on SELECT STYLE through all 5 styles; on SELECT LEVEL through all 12 levels. Then back out (no need to ride).
4. Back in the main menu, open **MOVIES**.
5. Open the **CHEATS** menu (in the options/extras menu).
6. Quit the game normally.

**Expected result:**
- Step 2: the rider list has 12 riders, ending with Slim Jim Guy and **Amish Boy**.
- Step 3: no "* LOCKED *" / "Play more to unlock this bike !" / "Play more to unlock this level !" anywhere: 5 bikes (the last one "SECRET BIKE"), STYLE 1-5, all 12 levels (Greenville Park … Acclaim Max Games Vert).
- Step 4: MOVIES lists the intro plus **DAVE MIRRA**, **RYAN NYQUIST** and **AMISH BOY**.
- Step 5: all 8 cheats are listed and can be toggled.

**Failure looks like:** "LOAD FAILED." / "FILE MAY BE CORRUPTED." at start-up, a crash when selecting Amish Boy / the secret bike / a style, locked entries still shown, or your progress (names, scores) reset to defaults.
**Please report:** pass/fail per step, any message text verbatim, screenshots of steps 2-5 (Win+PrtScn), whether the game crashed, and the exact menu path to CHEATS.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

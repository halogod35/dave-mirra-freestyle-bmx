"""Build out/playtest/menu-edit/: a main-menu label and a recoloured menu sprite.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_menu_edit.py [--game DIR] [--out DIR]
Reads assets/Intwrap.zal and assets/miscdata.zal from _pristine (read-only), never touches the install.

The edits go through the same paths a modder uses (docs/tools/menu-modding.md):
  * miscdata.zal: text 0x24 "PROQUEST" -> "MOD QUEST" in the four front-end tables 0x7919-0x791C
    (`dmbmx gui text-set`), so it shows whatever the save's language is.
  * Intwrap.zal: `dmbmx gui export --screen 0xA004` (group 0xE004, the rider portraits) into
    out/playtest/menu-edit/gui-edit/, the PNG of sprite 0x40B6 (Dave Mirra's portrait on SELECT RIDER,
    g_feRiderPortraits[0] 0x0054D23C) gets red and blue swapped in its palette, then `dmbmx gui import`.
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
from pathlib import Path

from dmbmx.convert import imaging
from dmbmx.formats import gui_dir, scrn, strs, zal, zprim

REPO = Path(__file__).resolve().parents[3]
PACKAGE = "menu-edit"
LABEL_ID, LABEL = 0x24, "MOD QUEST"
SPRITE, SCREEN = 0x40B6, 0xA004


def make_misc(src):
    out = gui_dir.set_texts(src, {t: {LABEL_ID: LABEL} for t in strs.FRONTEND_TABLES})
    arc, old = zal.ZalArchive.read(out), zal.ZalArchive.read(src)
    for e in arc.entries:
        if e.id in strs.FRONTEND_TABLES:
            t = strs.read(e.data())
            assert t.get(LABEL_ID) == LABEL and len(e.data()) <= strs.FRONTEND_MAX
            o = strs.read(old.find(e.id).data())
            assert [t.get(i) for i in t.ids() if i != LABEL_ID] == [o.get(i) for i in o.ids() if i != LABEL_ID]
        else:
            assert e.data() == old.find(e.id).data()
    return out


def swap_red_blue(png):
    img = imaging.png_decode(png)
    pal = [(b, g, r) for r, g, b in img["palette"]]
    return imaging.png_encode(img["width"], img["height"], img["rows"], color_type=3, bit_depth=img["bit_depth"],
                              palette=pal, trns=img["trns"])


def make_intwrap(src, edit_dir):
    if edit_dir.exists():
        shutil.rmtree(edit_dir)
    gui_dir.export_gui(src, edit_dir, screens=[SCREEN])
    p = edit_dir / "sprites" / f"{SPRITE:04X}_0.png"
    p.write_bytes(swap_red_blue(p.read_bytes()))
    report = []
    out = gui_dir.import_gui(src, edit_dir, report=report)
    assert [r[0] for r in report] == [SPRITE], report
    old, new = scrn.read_intwrap(src), scrn.read_intwrap(out)
    for gid, g in new.items():
        for aid, d in g.entries:
            if aid != SPRITE:
                assert d == old[gid].asset(aid), hex(aid)
    a, b = zprim.read_image(old[0xE004].asset(SPRITE)), zprim.read_image(new[0xE004].asset(SPRITE))
    assert a.frames[0].pixels == b.frames[0].pixels
    for c, d in zip(a.palette.colours, b.palette.colours):
        if c != zprim.TRANSPARENT:
            assert d == ((c & 0x1F) << 10 | (c & 0x3E0) | (c >> 10 & 0x1F)), (hex(c), hex(d))
    assert zal.ZalArchive.read(out).write() == out
    return out


def install_sh():
    spec = importlib.util.spec_from_file_location("pt_zal_repack", Path(__file__).with_name("make_playtest_zal_repack.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    s = mod.INSTALL_SH
    for a, b in (('PACKAGE="zal-repack"', f'PACKAGE="{PACKAGE}"'),
                 ("# zal-repack installer. Usage: install.sh intwrap|material|both|--restore",
                  f"# {PACKAGE} installer. Usage: install.sh [menu]|--restore"),
                 ('MODE="${1:-}"', 'MODE="${1:-menu}"'),
                 ("  intwrap|material|both) ;;", "  menu) ;;"),
                 ('usage: $0 intwrap|material|both|--restore', 'usage: $0 [menu]|--restore')):
        assert a in s, a
        s = s.replace(a, b)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "menu-edit"))
    a = ap.parse_args()
    pris = Path(a.game) / "_pristine" / "assets"
    out = Path(a.out)
    names = {p.name.lower(): p.name for p in pris.iterdir()}
    files = {
        f"assets/{names['miscdata.zal']}": make_misc((pris / names["miscdata.zal"]).read_bytes()),
        f"assets/{names['intwrap.zal']}": make_intwrap((pris / names["intwrap.zal"]).read_bytes(), out / "gui-edit"),
    }
    if (out / "files").exists():
        shutil.rmtree(out / "files")
    for rel, data in files.items():
        p = out / "files" / "menu" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    lines = []
    for p in sorted((out / "files").rglob("*")):
        if p.is_file():
            rel = p.relative_to(out / "files").as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(install_sh(), newline="\n")
    (out / "CHECK.md").write_text(CHECK.replace("{out}", out.as_posix()), newline="\n")
    print(f"wrote {out}")
    for rel, data in files.items():
        print(f"  {rel}: {len(data)} bytes (pristine {(pris / Path(rel).name).stat().st_size})")
    print("".join(lines), end="")


CHECK = r'''### Manual in-game check: menu-edit
**Goal:** prove that the game accepts menu mods made with `dmbmx gui`: a changed main-menu label (miscdata.zal text table) and a recoloured menu sprite (Intwrap.zal, re-encoded zprim sprite in a repacked nested archive).
**Prepare (Git Bash):**
    bash "{out}/install.sh"
  Expected output ends with "Ready." It lists the replaced files: assets/miscdata.zal and assets/Intwrap.zal.
**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Look at the main menu (the scrolling list PROQUEST / SESSION / FREERIDE / COOL STUFF / OPTIONS / QUIT).
3. Accept the first entry (where PROQUEST used to be). The SELECT screen opens on the rider page.
4. Scroll the riders with Left / Right until Dave Mirra is in the middle, then back out (Esc) to the main menu.
5. Open OPTIONS and COOL STUFF briefly and go back, then quit the game.
**Expected result:**
- Step 2: the entry PROQUEST reads **MOD QUEST**; all other entries are unchanged.
- Step 3: the select screen works normally (session/proquest mode as before).
- Step 4: **Dave Mirra's portrait** has red and blue swapped (bluish skin, orange/red where it was blue); the other riders' portraits look normal.
- Step 5: the other menus look and behave normally. No error message, no crash.
**Failure looks like:** "FRONTEND: Macro data too big or too small", "String(0x24) not found", "Interface asset wrapper does not exist!", "Failed to load interface assets", "ZPRIM: Failed to find sprite", garbled or missing portrait, a black screen, or a crash to desktop.
**Please report:** pass/fail per step, any message text verbatim, a screenshot of the main menu and of the rider page with Dave Mirra (Win+PrtScn), and whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~5 minutes.
'''

if __name__ == "__main__":
    main()

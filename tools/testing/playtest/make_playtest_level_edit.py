"""Build out/playtest/level-edit/: the three Woodward levels edited through dmbmx.level.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_level_edit.py [--game DIR] [--out DIR]
Reads assets/Woodward{Dirt,Street,Vert}/ from _pristine (read-only), never touches the install.

Per level (all edits go through dmbmx.formats.level; the writer re-signs the ZSD SCFI sizes):
  * Prop._sd: every prop raised by 3.0 RW units (~3 m), and one extra copy of the first prop
    placed 4 units beside it (Prop._sd grows by 88 bytes -> its SCFI size changes).
  * Mat._sd: the world texture used by the most BSP triangles (ramps/ground) replaced by a
    magenta/yellow 16-px checkerboard of the same size (24-bit BMP; Mat._sd size changes).
  * BSP/PropDef/Scene/Tag._sd: rewritten through the object model, byte-identical.
"""
import argparse
import collections
import hashlib
import importlib.util
import os
import shutil
from pathlib import Path

from dmbmx.formats import level as lvl
from dmbmx.formats import texture as T
from dmbmx.integrity import zsd as zsdcheck

REPO = Path(__file__).resolve().parents[3]
LEVELS = ("WoodwardDirt", "WoodwardStreet", "WoodwardVert")
RAISE = 3.0          # RW units; stored ZGE/100 y is "down", so y -= RAISE lifts the prop


def checker(w, h, cell=16, a=(255, 0, 200), b=(255, 230, 0)):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((a if ((x // cell) + (y // cell)) % 2 else b) + (255,))
    return T.Image(w, h, px)


def edit_level(src_dir):
    lv = lvl.Level.read_dir(src_dir)
    info = {}
    # 1. props
    props = lv.props.props
    first = props[0]
    for p in props:
        p.pos = (p.pos[0], p.pos[1] - RAISE, p.pos[2])
    dup = lvl.Prop(first.def_id, first.name, first.matrix, (first.pos[0] + 4.0, first.pos[1], first.pos[2]))
    lv.props.items.append(dup)
    info["props"] = len(props) + 1
    info["dup"] = first.name
    # 2. ground texture
    w = lv.world
    names = w.texture_names()
    use = collections.Counter()
    for s in w.sectors():
        for m, *_ in s.triangles:
            use[names[s.mat_base + m]] += 1
    tex, ntri = next((n, c) for n, c in use.most_common() if n and lv.texture(n) is not None)
    mati = lv.texture(tex)
    ti = T.info(mati.image)
    if mati.kind == "bmp":
        new, _ = T.prepare_replacement(T.encode_bmp(checker(ti["width"], ti["height"])), "bmp",
                                       size=(ti["width"], ti["height"]))
    else:
        new = T.encode_png(checker(ti["width"], ti["height"]))
    mati.set_image(new, mati.kind)
    info["texture"] = f"{tex} ({ti['width']}x{ti['height']}, {ntri} of {w.num_triangles} world triangles)"
    files = lv.files()
    # self-checks: re-read, sizes re-signed, untouched components byte-identical
    return lv, files, info


def _installer():
    spec = importlib.util.spec_from_file_location("pt_zal_repack", Path(__file__).with_name("make_playtest_zal_repack.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    s = mod.INSTALL_SH.replace('PACKAGE="zal-repack"', 'PACKAGE="level-edit"').replace("zal-repack installer", "level-edit installer")
    s = s.replace("intwrap|material|both|--restore", "woodward|--restore").replace("intwrap|material|both) ;;", "woodward) ;;")
    s = s.replace('if [ "$MODE" = both ]; then install_variant intwrap; install_variant material; else install_variant "$MODE"; fi',
                  'install_variant "$MODE"')
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "level-edit"))
    a = ap.parse_args()
    out = Path(a.out)
    lines, report = [], []
    for name in LEVELS:
        src = Path(a.game) / "_pristine" / "assets" / name
        lv, files, info = edit_level(src)
        dst = out / "files" / "woodward" / "assets" / name
        dst.mkdir(parents=True, exist_ok=True)
        changed = []
        for fn, data in files.items():
            if (src / fn).read_bytes() == data:
                continue                       # only ship what changed
            (dst / fn).write_bytes(data)
            changed.append(fn)
            lines.append(f"{hashlib.sha256(data).hexdigest()} *./woodward/assets/{name}/{fn}\n")
        # the shipped ZSD must match the shipped + untouched component sizes
        tmp = out / "check" / name
        tmp.mkdir(parents=True, exist_ok=True)
        for fn, data in files.items():
            (tmp / fn).write_bytes(data)
        assert all(ok for _e, _r, ok in zsdcheck.check(tmp / lv.zsd_name)), name
        again = lvl.Level.read_dir(tmp)
        assert again.files() == files
        shutil.rmtree(tmp)
        report.append(f"{name}: changed {', '.join(sorted(changed))}; props {info['props']} (extra copy of "
                      f"{info['dup']}); ground texture {info['texture']}")
        (out / f"preview_{name}.png").write_bytes(T.encode_png(T.decode(lv.texture(info['texture'].split()[0]).image)))
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(_installer(), newline="\n")
    (out / "CHECK.md").write_text(CHECK.replace("{out}", out.as_posix()).replace("{report}", "\n".join(
        f"- {r}" for r in report)), newline="\n")
    print(f"wrote {out}")
    print("\n".join(report))


CHECK = r'''### Manual in-game check: level-edit
**Goal:** prove that the game loads level files rewritten by `dmbmx.formats.level`: props moved and added (Prop._sd grows), a world texture replaced (Mat._sd changes size), and the `.ZSD` component sizes re-signed automatically.

**Prepare (Git Bash):**
    bash "{out}/install.sh" woodward
  Expected output ends with "Ready." It lists the replaced files: for each of `assets/WoodwardDirt`, `WoodwardStreet`, `WoodwardVert` the `.ZSD`, `Mat._sd` and `Prop._sd`.
  What was changed:
{report}
  Previews of the new textures: `{out}/preview_Woodward*.png`.

**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Start a session in **Woodward** (any mode that lets you pick the level: Session/Free ride or Proquest level 1). If you can choose, do it once each for the Dirt, Street and Vert variants; one variant is enough if only one is available.
3. Ride around for ~1 minute: look at the ramps/ground and at the props (rails, boxes, machines, signs, grass patches, fences that are separate objects).
4. Ride into the area where a prop now floats and check whether you collide with it in the air or at its old place.
5. Quit to the menu and quit the game.

**Expected result:**
- Step 2: the level loads with no error message (no "Scene component file not found" assert).
- Step 3: every surface that uses the texture named in "What was changed" (the level's most used world texture: wooden ramps in Dirt, quarter-pipe concrete in Street, the pipe transition strips in Vert) shows a **magenta/yellow checkerboard**. Every prop floats **about 3 m (rider height x1.5) above** where it normally stands, and the prop named in "What was changed" appears **twice**, the copy about 4 m to the side of the original.
- Step 4: please just describe what happens (collision with floating props is unknown).
- The rest of the level (geometry, other textures) looks normal.

**Failure looks like:** an assert box ("Scene component file not found", "Prop header size invalid", "No prop def corresponds to prop"), a crash to desktop while loading, the changed texture white/black/missing, props missing entirely, or props in the wrong place/orientation (rotated, mirrored).
**Please report:** pass/fail per step, a screenshot of the checkerboard surface and of a floating prop (Win+PrtScn), which Woodward variant(s) you tested, any message text verbatim, and whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

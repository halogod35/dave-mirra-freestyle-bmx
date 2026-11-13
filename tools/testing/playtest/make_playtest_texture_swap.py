"""Build out/playtest/texture-swap/: Dave Mirra's bike 1 with two swapped textures.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_texture_swap.py [--game DIR] [--out DIR]
Reads assets/DMbike01.zmd from _pristine (read-only), never touches the install.

Variant "bike" (assets/DMbike01.zmd):
  NAME_801_1 (128x128 24-bit BMP: frame colour strip + chrome parts) -> green/orange checkerboard
  TIRE       (64x64 RGBA PNG)                                          -> red-tinted tyre, alpha kept
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path

from dmbmx.formats import texture as T
from dmbmx.formats import zmd

REPO = Path(__file__).resolve().parents[3]


def checker(w, h, cell=16, a=(0, 220, 0), b=(255, 140, 0)):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((a if ((x // cell) + (y // cell)) % 2 else b) + (255,))
    return T.Image(w, h, px)


def red_tint(img):
    b = bytearray(img.rgba)
    for o in range(0, len(b), 4):
        lum = (b[o] * 3 + b[o + 1] * 6 + b[o + 2]) // 10
        b[o], b[o + 1], b[o + 2] = min(255, 80 + lum), lum // 4, lum // 4
    return T.Image(img.width, img.height, b)


def make_bike(src):
    z = zmd.read(src)
    frame = z.texture("NAME_801_1")
    fi = T.info(frame.image)
    new_frame, _ = T.prepare_replacement(T.encode_bmp(checker(fi["width"], fi["height"])), "bmp",
                                         size=(fi["width"], fi["height"]))
    frame.set_image(new_frame, "bmp")
    tire = z.texture("TIRE")
    new_tire = T.encode_png(red_tint(T.decode(tire.image)), alpha=True)
    assert not T.check(new_tire, "png")[0]
    tire.set_image(new_tire, "png")
    out = z.write()
    chk = zmd.read(out)
    assert chk.write() == out
    assert chk.texture("NAME_801_1").image == new_frame and chk.texture("TIRE").image == new_tire
    return out


def _installer():
    spec = importlib.util.spec_from_file_location("pt_zal_repack", Path(__file__).with_name("make_playtest_zal_repack.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    s = mod.INSTALL_SH.replace('PACKAGE="zal-repack"', 'PACKAGE="texture-swap"').replace("zal-repack installer", "texture-swap installer")
    s = s.replace("intwrap|material|both|--restore", "bike|--restore").replace("intwrap|material|both) ;;", "bike) ;;")
    s = s.replace('if [ "$MODE" = both ]; then install_variant intwrap; install_variant material; else install_variant "$MODE"; fi',
                  'install_variant "$MODE"')
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "texture-swap"))
    a = ap.parse_args()
    src = (Path(a.game) / "_pristine" / "assets" / "DMbike01.zmd").read_bytes()
    out = Path(a.out)
    data = make_bike(src)
    p = out / "files" / "bike" / "assets" / "DMbike01.zmd"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    # preview of the two new textures for the requester
    z = zmd.read(data)
    (out / "preview_frame.png").write_bytes(T.encode_png(T.decode(z.texture("NAME_801_1").image)))
    (out / "preview_tire.png").write_bytes(z.texture("TIRE").image)
    lines = [f"{hashlib.sha256(p.read_bytes()).hexdigest()} *./bike/assets/DMbike01.zmd\n"]
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(_installer(), newline="\n")
    (out / "CHECK.md").write_text(CHECK.replace("{out}", out.as_posix()), newline="\n")
    print(f"wrote {out}: DMbike01.zmd {len(data)} bytes (pristine {len(src)})")
    print("".join(lines), end="")


CHECK = r'''### Manual in-game check: texture-swap
**Goal:** prove that the game loads a ZMD whose textures were replaced by `dmbmx tex replace`: a 24-bit BMP slot (bike frame) and an RGBA PNG slot (tyre).

**Prepare (Git Bash):**
    bash "{out}/install.sh" bike
  Expected output ends with "Ready." It lists the replaced file: assets/DMbike01.zmd.
  Previews of the new textures: `{out}/preview_frame.png` and `preview_tire.png`.

**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Pick **Dave Mirra** as the rider, then look at the bike selection and choose his **first** bike (DMbike01).
3. Start any level and ride for ~30 seconds, looking at the bike close up (pause/replay camera helps).
4. Quit the game.

**Expected result:**
- At steps 2-3 the bike frame (and the other parts that use the same texture sheet: chrome bits, bars, cranks) shows a **green/orange checkerboard** instead of its normal colour.
- Both tyres look **red** instead of black/grey; the spokes/transparent parts of the wheel stay see-through (the tyre PNG keeps its alpha).
- Dave Mirra's other bikes (2-4) and all other riders' bikes look normal.
- No error message, no crash.

**Failure looks like:** crash to desktop when the bike loads, a white/black/missing texture on the frame or tyres, or the tyre's transparent areas drawn as solid colour.
**Please report:** pass/fail per step, a screenshot of the bike (Win+PrtScn), any message text verbatim, and which bike number shows the change if it isn't bike 1.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~5 minutes.
'''

if __name__ == "__main__":
    main()

"""Build out/playtest/dm-023/ (DM-023 play-test): repacked ZALs with recoloured images + install.sh.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_dm023.py [--game DIR] [--out DIR]
Reads the archives from _pristine (read-only), never touches the install.

Variants (each replaces one file in _testgame/assets):
  intwrap   Intwrap.zal: the four 512x512 start-up/loading screens (groups 1-4) get an inverted
            palette, and all 35 assets of the nested GUI archive (group 0) are re-compressed with
            dmbmx's LZO1X encoder (same pixels, new compressed bytes).
  material  material.zal: all 1262 BMPs get an inverted palette and are re-compressed by dmbmx.
"""
import argparse
import hashlib
import os
import struct
from pathlib import Path

from dmbmx.formats import zal

REPO = Path(__file__).resolve().parents[3]


def invert_palette(bmp):
    """Invert the RGB of every palette entry of a palettised (<= 8 bpp) BMP; pixels untouched."""
    b = bytearray(bmp)
    if b[:2] != b"BM":
        raise ValueError("not a BMP")
    hsize = struct.unpack_from("<I", b, 14)[0]
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp > 8:
        raise ValueError(f"{bpp}-bit BMP has no palette")
    used = struct.unpack_from("<I", b, 46)[0] or (1 << bpp)
    start = 14 + hsize
    if start + 4 * used > struct.unpack_from("<I", b, 10)[0]:
        raise ValueError("palette overlaps pixel data")
    for i in range(used):
        p = start + 4 * i
        b[p:p + 3] = bytes(255 - x for x in b[p:p + 3])
    return bytes(b)


def make_intwrap(src):
    arc = zal.read(src)
    expect = {}
    for gid in (1, 2, 3, 4):
        new = invert_palette(arc.find(gid).data())
        arc.set_data(gid, new)
        expect[gid] = new
    inner = zal.read(arc.find(0).data())
    inner_expect = {}
    for e in inner.entries:
        d = e.data()
        inner.set_data(e.id, d)          # force re-encoding with dmbmx's compressor
        inner_expect[e.id] = d
    arc.set_data(0, inner.write())
    out = arc.write()
    # verify by re-reading everything
    chk = zal.read(out)
    for gid, d in expect.items():
        assert chk.find(gid).data() == d
    chk_inner = zal.read(chk.find(0).data())
    for gid, d in inner_expect.items():
        assert chk_inner.find(gid).data() == d
    orig_inner = zal.read(zal.read(src).find(0).data())
    assert all(a.raw != b.raw for a, b in zip(orig_inner.entries, chk_inner.entries)), \
        "expected every nested stream to be re-encoded"
    return out


def make_material(src):
    arc = zal.read(src)
    expect = {}
    for e in arc.entries:
        new = invert_palette(e.data())
        arc.set_data(e.id, new)
        expect[e.id] = new
    out = arc.write()
    chk = zal.read(out)
    for e in chk.entries:
        assert e.data() == expect[e.id]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-023"))
    a = ap.parse_args()
    pris = Path(a.game) / "_pristine" / "assets"
    out = Path(a.out)
    variants = {
        "intwrap": ("assets/Intwrap.zal", make_intwrap((pris / "Intwrap.zal").read_bytes())),
        "material": ("assets/material.zal", make_material((pris / "material.zal").read_bytes())),
    }
    for k, (rel, data) in variants.items():
        p = out / "files" / k / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    lines = []
    for p in sorted((out / "files").rglob("*")):
        if p.is_file():
            rel = p.relative_to(out / "files").as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(INSTALL_SH, newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.replace("{out}", out.as_posix()), newline="\n")
    print(f"wrote {out}")
    for k, (rel, data) in variants.items():
        print(f"  {k}: {rel} {len(data)} bytes (pristine {(pris / Path(rel).name).stat().st_size})")
    print("".join(lines), end="")


INSTALL_SH = r'''#!/usr/bin/env bash
# DM-023 play-test installer. Usage: install.sh intwrap|material|both|--restore
# Touches only _testgame (AD-1). Never starts the game.
set -euo pipefail
GAME="F:/Projects/Dave Mirra freestyle BMX"
TEST="$GAME/_testgame"
PRIS="$GAME/_pristine"
HERE="$(cd "$(dirname "$0")" && pwd)"
CARD="DM-023"

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

install_variant() {
  local SRC="$HERE/files/$1"
  (cd "$SRC" && find . -type f) | while IFS= read -r f; do
    f="${f#./}"
    old="$(sha256sum "$TEST/$f" | cut -c1-64)"
    cp -f "$SRC/$f" "$TEST/$f"
    new="$(sha256sum "$TEST/$f" | cut -c1-64)"
    echo "  replaced $f  old=$old  new=$new"
  done
}

MODE="${1:-}"
case "$MODE" in
  restore|--restore) restore; rm -f "$TEST/.playtest"; exit 0 ;;
  intwrap|material|both) ;;
  *) echo "usage: $0 intwrap|material|both|--restore" >&2; exit 2 ;;
esac

restore
echo "Verifying play-test files ..."
(cd "$HERE/files" && sha256sum -c --quiet "$HERE/MANIFEST.sha256")
if [ "$MODE" = both ]; then install_variant intwrap; install_variant material; else install_variant "$MODE"; fi
printf '%s %s %s\n' "$CARD" "$MODE" "$(date -Iseconds)" > "$TEST/.playtest"
echo "Ready. Start _testgame/NgBMX.exe and follow REQUEST.md. Afterwards run: install.sh --restore"
'''

REQUEST = r'''### Play-test request DM-023-PT1
**Goal:** prove that the game loads ZAL archives rebuilt by dmbmx: (A) replaced uncompressed images in Intwrap.zal plus a nested archive whose 35 assets were all re-compressed by our LZO1X encoder; (B) material.zal with all 1262 textures replaced and re-compressed.

**Part A: Intwrap.zal (start-up screens)**
Prepare (Git Bash):
    bash "{out}/install.sh" intwrap
  Expected output ends with "Ready." It lists the replaced file: assets/Intwrap.zal.

Steps:
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Watch the start-up screens: the RenderWare logo, the Acclaim/Z-Axis legal text screen, and the "LOADING" screen with Dave Mirra's face and the orange logo.
3. In the main menu, move through a few menus (e.g. Proquest -> rider select, Options) and back.
4. Start any level, then quit back to the menu (you will see the "LOADING" screen again). Quit the game.

Expected result (A):
- Step 2: those three screens appear in **inverted colours** (photo-negative): the RenderWare screen has a **white** background, the legal screen has light text areas with **dark text**, the loading screen shows a **blue** "Dave Mirra" logo (instead of orange) on a light background.
- Step 3: all menus look and behave **normally** (they come from the re-compressed nested archive; no change is expected).
- No error message, no crash.

**Part B: material.zal**
Prepare: `bash "{out}/install.sh" material`, then repeat steps 1-4.
Expected result (B): no crash and no error at start-up or when loading a level. Wherever the game uses material.zal textures, they appear in inverted colours. **We don't know where these 1262 small textures are used yet** (likely level/prop/effect textures or menu icons), so please note every place where colours look inverted (a screenshot helps). If nothing looks different, say so, since that's also useful.

**Failure looks like:** "Could not open ZAL", "Interface asset wrapper does not exist!", "Failed to load interface assets", "ERROR - Size mismatch in decompressed data", "Could not load menu art asset file", garbled or black screens, or a crash to desktop.
**Please report:** pass/fail for A and B per step, any message text verbatim, screenshots of the start-up screens (A) and of anything inverted (B) (Win+PrtScn), and whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

"""Build out/playtest/gltf-bikes/ (the M2 model proof): bikes edited through glTF.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_gltf_bikes.py [--game DIR] [--out DIR]
Reads the bikes from _pristine (read-only), never touches the install.

Every in-game bike (each assets/*.zmd whose root frame is "Hipsdeck", shadows excluded) goes through
the real edit path: ZMD -> `export_glb` -> scripted glTF edit -> `import_glb` -> ZMD.

Variants (each a full set of bike files under files/<variant>/assets/):
  wheels   FrontWheel + BackWheel meshes scaled x1.5 about their hubs (geometry path: positions and
           bounding sphere change; topology, BinMesh strips and everything else stay identical).
  paint    the bike frame's texture (the Hipsdeck part, material slot 0) colour-inverted
           (texture path: TBMP re-encoded at its original depth / TPNG re-encoded).
  both     wheels + paint.
"""
import argparse
import hashlib
import os
from pathlib import Path

from dmbmx.convert import gltf, imaging
from dmbmx.convert.glb_edit import GlbDoc
from dmbmx.convert.gltf_import import import_glb
from dmbmx.formats import zmd

REPO = Path(__file__).resolve().parents[3]
SCALE = 1.5


def is_bike(m):
    return (not m.empty and m.lods and m.lods[0].clump is not None and m.lods[0].clump.frames
            and m.skeleton()[0]["name"] == "Hipsdeck")


def frame_texture(m):
    cl = m.lods[0].clump
    root_atomic = next(a for a in cl.atomics if a.frame == 0)
    return root_atomic.geometry.material_list()[0].texture.name


def invert_png(png):
    img = imaging.png_decode(png)
    rows = imaging.png_to_rgba_rows(img)
    inv = [bytes((255 - b) if k % 4 != 3 else b for k, b in enumerate(r)) for r in rows]
    return imaging.png_encode(img["width"], img["height"], inv, color_type=6)


def edit(src_bytes, name, wheels, paint):
    m = zmd.read(src_bytes)
    doc = GlbDoc(gltf.export_glb(m, source=src_bytes, source_name=name))
    if wheels:
        for fn in ("FrontWheel", "BackWheel"):
            doc.map_positions(doc.atomic_node(frame_name=fn), lambda p: (p[0] * SCALE, p[1] * SCALE, p[2] * SCALE))
    tex = frame_texture(m)
    if paint:
        ii = doc.image_index(tex)
        doc.set_image(ii, invert_png(gltf.image_bytes(doc.js, bytes(doc.bin), ii)))
    out, rep = import_glb(doc.to_bytes(), base=src_bytes, strict=True)
    verify(src_bytes, out, wheels, paint, tex)
    return out, rep, tex


def verify(src, out, wheels, paint, tex):
    """Re-read the result and check that exactly the intended parts changed."""
    a, b = zmd.read(src), zmd.read(out)
    assert zmd.write(b) == out
    sk = a.skeleton()
    ver = a.lods[0].rw_version
    for x, y in zip(a.lods[0].clump.atomics, b.lods[0].clump.atomics):
        gx, gy = x.geometry, y.geometry
        if wheels and sk[x.frame]["name"] in ("FrontWheel", "BackWheel"):
            assert gx.triangles == gy.triangles and gx.uv_sets == gy.uv_sets
            assert [c.to_bytes() for c in gx.ext] == [c.to_bytes() for c in gy.ext]  # BinMesh strips kept
            for p, q in zip(gx.morph_targets[0].vertices, gy.morph_targets[0].vertices):
                assert all(abs(q[k] - p[k] * SCALE) < 1e-5 for k in range(3))
        else:
            assert gx.to_chunk(ver).to_bytes() == gy.to_chunk(ver).to_bytes()
    for s, t in zip(a.materials(), b.materials()):
        if paint and s.name.upper() == tex.upper():
            assert s.image != t.image and s.kind == t.kind
            if s.kind == "bmp":
                assert imaging.bmp_info(s.image).bpp == imaging.bmp_info(t.image).bpp
        else:
            assert s.to_chunk().to_bytes() == t.to_chunk().to_bytes()
    assert a.mdef.encode() == b.mdef.encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "gltf-bikes"))
    a = ap.parse_args()
    pris = Path(a.game) / "_pristine" / "assets"
    out = Path(a.out)
    bikes = []
    for f in sorted(pris.iterdir()):
        if f.suffix.lower() != ".zmd" or "shadow" in f.name.lower():
            continue
        data = f.read_bytes()
        if data and is_bike(zmd.read(data)):
            bikes.append((f, data))
    variants = {"wheels": (True, False), "paint": (False, True), "both": (True, True)}
    summary = []
    for v, (wh, pa) in variants.items():
        for f, data in bikes:
            new, rep, tex = edit(data, f.name, wh, pa)
            p = out / "files" / v / "assets" / f.name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(new)
            if v == "both":
                summary.append(f"  {f.name:<20s} frame texture {tex:<12s} {len(rep.changes)} changes")
    lines = []
    for p in sorted((out / "files").rglob("*")):
        if p.is_file():
            rel = p.relative_to(out / "files").as_posix()
            lines.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(INSTALL_SH, newline="\n")
    names = ", ".join(f.name for f, _ in bikes)
    (out / "CHECK.md").write_text(CHECK.replace("{out}", out.as_posix()).replace("{n}", str(len(bikes)))
                                    .replace("{names}", names), newline="\n")
    print(f"wrote {out}: {len(bikes)} bikes x {len(variants)} variants, all verified")
    print("\n".join(summary))


INSTALL_SH = r'''#!/usr/bin/env bash
# gltf-bikes installer. Usage: install.sh wheels|paint|both|--restore
# Touches only _testgame (AD-1). Never starts the game.
set -euo pipefail
GAME="F:/Projects/Dave Mirra freestyle BMX"
TEST="$GAME/_testgame"
PRIS="$GAME/_pristine"
HERE="$(cd "$(dirname "$0")" && pwd)"
PACKAGE="gltf-bikes"

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
  wheels|paint|both) ;;
  *) echo "usage: $0 wheels|paint|both|--restore" >&2; exit 2 ;;
esac

restore
echo "Verifying package files ..."
(cd "$HERE/files" && sha256sum -c --quiet "$HERE/MANIFEST.sha256")
SRC="$HERE/files/$MODE"
n=0
while IFS= read -r f; do
  f="${f#./}"
  [ -f "$TEST/$f" ] || { echo "refusing: $f is not a stock file" >&2; exit 1; }
  old="$(sha256sum "$TEST/$f" | cut -c1-64)"
  cp -f "$SRC/$f" "$TEST/$f"
  new="$(sha256sum "$TEST/$f" | cut -c1-64)"
  echo "  replaced $f  old=$old  new=$new"
  n=$((n + 1))
done < <(cd "$SRC" && find . -type f | sort)
printf '%s %s %s\n' "$PACKAGE" "$MODE" "$(date -Iseconds)" > "$TEST/.playtest"
echo "Replaced $n bike files ($MODE)."
echo "Ready. Start _testgame/NgBMX.exe and follow CHECK.md. Afterwards run: install.sh --restore"
'''

CHECK = r'''### Manual in-game check: gltf-bikes (M2: an edited model loads in game)
**Goal:** prove the game accepts bike models rebuilt by dmbmx from an edited glTF: (A) bigger wheels (geometry edit), (B) a recoloured bike frame (texture edit).
All {n} bikes are edited, so it does not matter which rider or bike you pick.

**Part A: wheels (~5 min)**
Prepare (Git Bash):
    bash "{out}/install.sh" wheels
  Expected output: "Replaced {n} bike files (wheels)." then "Ready."
Steps:
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Main menu -> Session (free ride) or Proquest -> pick **Dave Mirra** (default rider) and his default bike -> start any level.
3. Look at the bike while riding for ~30 s (pedal, jump once); open the rider/bike select screen too if it shows the bike.
4. Quit to the main menu, then quit the game.
Expected (A):
- Step 2/3: both **wheels are about 1.5x their normal size** (they stick out past the frame and dip into the ground a bit; the hubs stay where they were). The frame, handlebars, seat, pedals and colours look **normal**.
- No error message, no crash. (Riding physics should be unchanged; the edit is visual only.)

**Part B: frame paint (~5 min)**
Prepare: `bash "{out}/install.sh" paint`, then repeat steps 1-4.
Expected (B): wheels **normal size**; the bike **frame** (and the parts sharing its texture, usually handlebars/sprocket) is shown in **inverted colours** (photo-negative, e.g. a green frame turns magenta/pink). Tyres and pedals look normal. No error, no crash.

**Failure looks like:** a crash to desktop when the level loads, an error dialog, an invisible or exploded bike, wheels at normal size in A (edit not picked up), garbled/black frame texture in B.
**Please report:** pass/fail for A and B, which rider/bike you picked, any message text verbatim, one screenshot of each (Win+PrtScn), and whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.

Edited files ({n}): {names}
'''

if __name__ == "__main__":
    main()

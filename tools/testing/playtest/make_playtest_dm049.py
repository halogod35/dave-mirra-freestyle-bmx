"""Build out/playtest/dm-049/ (DM-049 play-test): new world geometry in WoodwardDirt, with and
without the rebuilt RW collision tree.

Usage (repo root or worktree):  tools/testing/py.sh tools/testing/playtest/make_playtest_dm049.py [--game DIR] [--out DIR]
Reads assets/WoodwardDirt/ from _pristine (read-only), never touches the install.

The edit: a solid wooden block ("table") is added to the world sector the rider starts in front of:
footprint 4 m x 10 m across the start line, about 10 m ahead of the start point, its flat top 1 m
above the ground there (5 faces, 10 triangles, 20 vertices, material Woodward_wood).

Two variants:
  * ``rebuilt``: written by dmbmx as an editor would (the sector's collision tree is rebuilt and
    its triangles re-sorted, colSectorSize and the header counts recomputed, BinMesh rebuilt).
  * ``stale`` (control): the same geometry, but the old collision tree is kept (it does not
    reference the 10 new triangles), so the block should be visible but not solid.
Only BSP._sd and the .ZSD (re-signed size) change.
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
from collections import Counter
from pathlib import Path

from dmbmx.formats import level as lvl
from dmbmx.formats import rwcoll
from dmbmx.integrity import zsd as zsdcheck

REPO = Path(__file__).resolve().parents[3]
LEVEL = "WoodwardDirt"
SECTOR_PROBE = (278.0, 57.0, 226.9)       # a point above the ground the block stands on (RW units)
BOX_X = (276.0, 280.0)                    # along the start heading (+x)
BOX_Z = (222.0, 232.0)                    # across it; the start point is at z = 226.9
GROUND = 52.74                            # ground height there [V: probe below]
TOP = GROUND + 1.0                        # 1 m (RW unit = 1 m; ZGE = cm)
BOTTOM = 52.3                             # below the ground, inside the sector's region (y >= 52.18)
MATERIAL = "Woodward_wood"


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def find_sector(world, p):
    """The sector whose box holds p and that has the most triangles (the ground sector)."""
    cands = [s for s in world.sectors() if s.triangles and all(s.inf[i] <= p[i] <= s.sup[i] for i in (0, 2))]
    return max(cands, key=lambda s: len(s.triangles))


def winding_sign(s):
    """+1 if the corpus winding has cross(b-a, c-a) along the vertex normals, else -1 [V per sector]."""
    vote = Counter()
    nv = s.normal_vectors()
    for _m, a, b, c in s.triangles:
        n = _cross(_sub(s.vertices[b], s.vertices[a]), _sub(s.vertices[c], s.vertices[a]))
        avg = tuple(sum(nv[i][k] for i in (a, b, c)) for k in range(3))
        vote[_dot(n, avg) > 0] += 1
    return 1 if vote[True] >= vote[False] else -1


def add_block(world, s):
    names = world.texture_names()
    mat = names.index(MATERIAL) - s.mat_base
    used = [v for m, *vv in s.triangles if m == mat for v in vv]
    prelit = tuple(round(sum(s.prelit[v][k] for v in used) / len(used)) for k in range(4)) if used else (200, 200, 200, 255)
    sign = winding_sign(s)
    x0, x1 = BOX_X
    z0, z1 = BOX_Z
    y0, y1 = BOTTOM, TOP
    # quads as (corner list in any order around the face, outward normal, uv axes)
    faces = [
        ([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0), (0, 2)),   # top
        ([(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], (-1, 0, 0), (2, 1)),  # front (faces the start)
        ([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0), (2, 1)),   # back
        ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1), (0, 1)),  # side
        ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1), (0, 1)),   # side
    ]
    first_tri = len(s.triangles)
    for quad, n, (ua, va) in faces:
        base = len(s.vertices)
        for p in quad:
            s.vertices.append(p)
            s.normals.append(bytes((n[0] * 127 & 0xFF, n[1] * 127 & 0xFF, n[2] * 127 & 0xFF, 0xCD)))
            s.prelit.append(prelit)
            s.uvs.append((p[ua] / 2.0, p[va] / 2.0))
        for tri in ((0, 1, 2), (0, 2, 3)):
            a, b, c = (base + i for i in tri)
            cr = _cross(_sub(s.vertices[b], s.vertices[a]), _sub(s.vertices[c], s.vertices[a]))
            if (_dot(cr, n) > 0) != (sign > 0):
                b, c = c, b
            s.triangles.append((mat, a, b, c))
    s.rebuild_bin_mesh()
    return list(range(first_tri, len(s.triangles))), {"material": f"{MATERIAL} (sector-relative {mat})",
                                                       "prelit": prelit, "winding": sign}


def build(game):
    src = Path(game) / "_pristine" / "assets" / LEVEL
    variants, report = {}, []
    for variant in ("rebuilt", "stale"):
        lv = lvl.Level.read_dir(src)
        w = lv.world
        s = find_sector(w, SECTOR_PROBE)
        nt0, nv0 = len(s.triangles), len(s.vertices)
        new_tris, info = add_block(w, s)
        new_set = {s.triangles[t] for t in new_tris}
        name = lv.zsd.entry(4).name
        if variant == "stale":
            data = w.write(rebuild_collision=False)
            lv.components[name] = data
        files = lv.files()
        # checks through the Python ports of the game's walks
        w2 = lvl.read_component(name, files[name])
        s2 = find_sector(w2, SECTOR_PROBE)
        probs = rwcoll.check(s2)
        mid = ((BOX_X[0] + BOX_X[1]) / 2, (BOX_Z[0] + BOX_Z[1]) / 2)
        down = w2.line_hits((mid[0], TOP + 2.0, mid[1]), (mid[0], GROUND - 1.0, mid[1]))
        top_found = any(sec is s2 and sec.triangles[t] in new_set for sec, t in down)
        ahead = w2.line_hits((BOX_X[0] - 2.0, GROUND + 0.5, 226.9), (BOX_X[0] + 1.0, GROUND + 0.5, 226.9))
        front_found = any(sec is s2 and sec.triangles[t] in new_set for sec, t in ahead)
        if variant == "rebuilt":
            assert not probs and top_found and front_found, (probs, top_found, front_found)
        else:
            assert probs and not top_found and not front_found, (probs[:2], top_found, front_found)
        report.append(f"{variant}: sector {nt0} -> {len(s2.triangles)} triangles, {nv0} -> {len(s2.vertices)} "
                      f"vertices; check() {'clean' if not probs else probs[0]}; Python port of the game's "
                      f"line test: block top {'FOUND' if top_found else 'not found'}, front face "
                      f"{'FOUND' if front_found else 'not found'}; colSectorSize {w2.col_sector_size}; "
                      f"{info['material']}, prelit {info['prelit']}")
        variants[variant] = (lv, files)
    return src, variants, report


def _installer():
    spec = importlib.util.spec_from_file_location("pt23", Path(__file__).with_name("make_playtest_dm023.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    s = mod.INSTALL_SH.replace('CARD="DM-023"', 'CARD="DM-049"').replace("DM-023 play-test", "DM-049 play-test")
    s = s.replace("intwrap|material|both|--restore", "rebuilt|stale|--restore").replace(
        "intwrap|material|both) ;;", "rebuilt|stale) ;;")
    s = s.replace('if [ "$MODE" = both ]; then install_variant intwrap; install_variant material; else install_variant "$MODE"; fi',
                  'install_variant "$MODE"')
    assert "intwrap" not in s and "both" not in s
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-049"))
    a = ap.parse_args()
    out = Path(a.out)
    if (out / "files").exists():
        shutil.rmtree(out / "files")
    src, variants, report = build(a.game)
    lines = []
    for variant, (lv, files) in variants.items():
        dst = out / "files" / variant / "assets" / LEVEL
        dst.mkdir(parents=True, exist_ok=True)
        changed = []
        for fn, data in files.items():
            if (src / fn).read_bytes() == data:
                continue
            (dst / fn).write_bytes(data)
            changed.append(fn)
            lines.append(f"{hashlib.sha256(data).hexdigest()} *./{variant}/assets/{LEVEL}/{fn}\n")
        assert sorted(changed) == sorted(["BSP._sd", lv.zsd_name]), changed
        tmp = out / "check" / variant
        tmp.mkdir(parents=True, exist_ok=True)
        for fn, data in files.items():
            (tmp / fn).write_bytes(data)
        assert all(ok for _e, _r, ok in zsdcheck.check(tmp / lv.zsd_name)), variant
        shutil.rmtree(out / "check")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(_installer(), newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.replace("{out}", out.as_posix()).replace(
        "{report}", "\n".join(f"  - {r}" for r in report)), newline="\n")
    print(f"wrote {out}")
    print("\n".join(report))


REQUEST = r'''### Play-test request DM-049-PT1
**Goal:** prove that world geometry added by dmbmx is solid in the game once the RW collision tree is rebuilt (and, as a control, not solid when the old tree is kept).

**Prepare (Git Bash), part A:**
    bash "{out}/install.sh" rebuilt
  Expected output ends with "Ready." It lists 2 replaced files: `assets/WoodwardDirt/BSP._sd` and `assets/WoodwardDirt/WoodDirt.ZSD` (re-signed size).
  What was changed (both variants): a wooden block, 4 m deep x 10 m wide, its flat top 1 m above the ground, stands across the start line about 10 m straight ahead of the WoodwardDirt start point (the rider faces it at the start).
{report}

**Steps (part A, `rebuilt`):**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. Start a session in **Woodward, Dirt** (Session/Free ride, or Proquest if that is the only way in).
3. At the start, look straight ahead: a new wooden block stands on the ground about 10 m in front of you (if it is not straight ahead, look around the start; it is within about 15 m).
4. Ride straight into the block's front face at moderate speed.
5. Ride back, take speed and jump (or ride off the start platform) so that you land **on top** of the block. Roll across the top and off the far edge.
6. Quit to the menu.

**Part B (control, `stale`):**
7. Quit the game. Run `bash "{out}/install.sh" stale` (restores first, then installs the control; same 2 files).
8. Repeat steps 1-5.

**Expected result:**
- Step 3: the block is visible with a wood texture and no holes; the rest of the level looks normal.
- Step 4 (A): the rider **hits** the front face (bounces off or crashes); he does not pass through it.
- Step 5 (A): the rider **lands on and rolls across the top** of the block, 1 m above the ground, and drops off the far edge.
- Step 8 (B, control): the block looks the same, but the rider **passes through** its front face and **falls through** its top to the ground (the old collision tree does not list the new triangles).

**Failure looks like:** an assert or crash while loading WoodwardDirt (either variant); in A: riding through the block or falling through its top; in A or B: the block missing, black, or inside-out (only visible from inside); other parts of the level becoming non-solid or the rider falling through the ground anywhere.
**Please report:** pass/fail per step for A and B, a screenshot of the rider on top of the block in A (Win+PrtScn), any message text verbatim, and whether the game crashed.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

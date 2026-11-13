"""dmbmx import: glTF (.glb) -> ZMD (DM-021). See docs/tools/import.md.

(The module is called ``import_`` because ``import`` is a Python keyword; the command is ``import``.)
"""
import hashlib
import os
from pathlib import Path

from dmbmx.convert.gltf_import import BINMESH_MODES, PALETTISED_MODES, import_glb
from dmbmx.core.errors import FormatError
from dmbmx.formats import zmd

DEFAULT_GAME_DIR = r"F:\Projects\Dave Mirra freestyle BMX"
_WRITABLE = {"_testgame", "dave-mirra-bmx", "dave-mirra-pro-bmx", "wt"}


def register(sub):
    p = sub.add_parser("import", help="rebuild a ZMD from an (edited) .glb written by `dmbmx export`",
                       description="dmbmx import <in.glb> <out.zmd> [--base original.zmd] [--binmesh auto|strips|lists] "
                                   "[--strict]")
    p.add_argument("glb")
    p.add_argument("out", help="output .zmd (never inside the game install or _pristine)")
    p.add_argument("--base", help="the original .zmd: checked against the SHA-256 in the .glb, and supplies "
                                  "the LODs a --lod export left out")
    p.add_argument("--binmesh", choices=BINMESH_MODES, default="auto",
                   help="form of regenerated BinMesh chunks (default auto = same as the original geometry)")
    p.add_argument("--palettised", choices=PALETTISED_MODES, default="promote",
                   help="edited 4/8-bit BMP textures: promote to 24-bit (default; DM-014: any depth loads) "
                        "or quantize to the original palette")
    p.add_argument("--strict", action="store_true", help="treat warnings as errors")
    p.add_argument("--game-dir", default=os.environ.get("DMBMX_GAME_DIR", DEFAULT_GAME_DIR), help=__import__("argparse").SUPPRESS)
    p.set_defaults(func=cmd_import)


def protected(path, game_dir):
    """True if ``path`` is inside the read-only install (AD-1): anything under the game dir except
    _testgame and the repo/worktrees."""
    try:
        rel = Path(path).resolve().relative_to(Path(game_dir).resolve())
    except ValueError:
        return False
    return not rel.parts or rel.parts[0] not in _WRITABLE


def cmd_import(args):
    if protected(args.out, args.game_dir):
        print(f"refusing to write {args.out}: inside the game install / _pristine (read-only, AD-1). "
              "Write to out/ and install into _testgame with a play-test script.")
        return 2
    try:
        data = Path(args.glb).read_bytes()
        base = Path(args.base).read_bytes() if args.base else None
        out, rep = import_glb(data, base=base, binmesh=args.binmesh, strict=args.strict,
                              palettised=args.palettised)
    except (FormatError, OSError) as e:
        print(f"error: {e}")
        return 1
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(out)
    m = zmd.read(out)
    nv = sum(len(ms["positions"]) for li in range(len(m.lods)) for ms in m.meshes(li))
    nt = sum(len(ms["material_ids"]) for li in range(len(m.lods)) for ms in m.meshes(li))
    for c in rep.changes:
        print(f"  change: {c}")
    for w in rep.warnings:
        print(f"  WARNING: {w}")
    regen = {k: v for k, v in rep.binmesh.items() if v != "kept"}
    if regen:
        print("  BinMesh regenerated: " + ", ".join(f"{k} as {v}" for k, v in regen.items()))
    same = base is not None and out == base
    print(f"{args.out}: {len(out)} bytes, {nv} vertices, {nt} triangles, sha256 {hashlib.sha256(out).hexdigest()[:16]}"
          + (" (identical to --base)" if same else "") + (" (no changes)" if not rep.changes else ""))
    return 0

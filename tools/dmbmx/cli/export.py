"""dmbmx export: ZMD -> glTF binary (.glb) [+ OBJ] (DM-020). See docs/tools/export.md."""
import os
from pathlib import Path

from dmbmx.convert import gltf, obj
from dmbmx.core.errors import FormatError
from dmbmx.formats import zmd

DEFAULT_GAME_DIR = r"F:\Projects\Dave Mirra freestyle BMX"
_SKIP_DIRS = {"_pristine", "_testgame", "wt", "dave-mirra-bmx", "dave-mirra-pro-bmx"}


def register(sub):
    p = sub.add_parser("export", help="export ZMD models to glTF .glb (+ optional OBJ)",
                       description="dmbmx export <file.zmd> <out.glb> [--lod N] [--obj]\n"
                                   "dmbmx export --all <outdir> [--game-dir DIR] [--obj]",
                       formatter_class=__import__("argparse").RawDescriptionHelpFormatter)
    p.add_argument("paths", nargs="+", metavar="PATH", help="<file.zmd> <out.glb>, or <outdir> with --all")
    p.add_argument("--all", action="store_true", help="export every .zmd under the game install")
    p.add_argument("--game-dir", default=os.environ.get("DMBMX_GAME_DIR", DEFAULT_GAME_DIR),
                   help="install to scan with --all (default: $DMBMX_GAME_DIR or the project install)")
    p.add_argument("--lod", type=int, default=None,
                   help="export only this LOD (default: all; a partial export can't be restored losslessly)")
    p.add_argument("--obj", action="store_true", help="also write <out>.obj/.mtl/.png (LOD --lod or 0)")
    p.add_argument("--no-upright", action="store_true",
                   help="don't add the rider upright root rotation (raw RW model space)")
    p.add_argument("--anims", default=None, metavar="IDS",
                   help="add motion.zal animations as glTF animations: 'all' or ids like 1,5,10-12 "
                        "(rider/bike channels; removes the rider upright root rotation, see ANIM.md)")
    p.add_argument("--motion-zal", default=None,
                   help="motion.zal to read (default: <game-dir>/assets/motion.zal)")
    p.set_defaults(func=cmd_export)


def _parse_ids(spec, available):
    if spec.strip().lower() == "all":
        return list(available)
    out = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-", 1)
            out += list(range(int(a, 0), int(b, 0) + 1))
        elif part:
            out.append(int(part, 0))
    missing = [i for i in out if i not in available]
    if missing:
        raise ValueError(f"no motion ids {missing} (bank has {min(available)}..{max(available)})")
    return out


_BANK = {}


def _motions(args):
    from dmbmx.formats import motion
    path = Path(args.motion_zal or Path(args.game_dir) / "assets" / "motion.zal")
    if path not in _BANK:
        _BANK[path] = motion.read_bank(path.read_bytes())
    bank = _BANK[path]
    return [(f"motion_{i:03d}", bank.motion(i)) for i in _parse_ids(args.anims, bank.ids)]


def _export_one(src, out, args):
    data = Path(src).read_bytes()
    m = zmd.read(data)
    if m.empty:
        print(f"skip {src}: empty file (0 bytes)")
        return None
    lods = None if args.lod is None else [args.lod]
    glb = gltf.export_glb(m, source=data, source_name=Path(src).name, lods=lods, upright=not args.no_upright)
    if getattr(args, "anims", None):
        from dmbmx.convert import anim_gltf
        glb = anim_gltf.add_animations(glb, _motions(args))
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(glb)
    nv = sum(len(ms["positions"]) for li in (lods or range(len(m.lods))) for ms in m.meshes(li))
    nt = sum(len(ms["material_ids"]) for li in (lods or range(len(m.lods))) for ms in m.meshes(li))
    msg = f"{out}: {nv} vertices, {nt} triangles, {len(m.materials())} textures, {len(glb)} bytes"
    if args.obj:
        ov, ot, om = obj.export_obj(m, out.with_suffix(".obj"), lod=args.lod or 0, upright=not args.no_upright,
                                    name=Path(src).stem)
        msg += f" (+ {out.with_suffix('.obj').name})"
    print(msg)
    return out


def _corpus(game_dir):
    root = Path(game_dir)
    found = []
    for d, dirs, files in os.walk(root):
        if Path(d) == root:
            dirs[:] = [x for x in dirs if x not in _SKIP_DIRS]
        found += [Path(d) / f for f in files if f.lower().endswith(".zmd")]
    return sorted(found)


def cmd_export(args):
    if args.all:
        if len(args.paths) != 1:
            print("usage: dmbmx export --all <outdir>")
            return 2
        outdir = Path(args.paths[0])
        files = _corpus(args.game_dir)
        if not files:
            print(f"no .zmd files under {args.game_dir}")
            return 1
        ok = skipped = failed = 0
        for f in files:
            try:
                r = _export_one(f, outdir / (f.stem + ".glb"), args)
            except (FormatError, ValueError) as e:
                print(f"FAIL {f}: {e}")
                failed += 1
                continue
            if r is None:
                skipped += 1
            else:
                ok += 1
        print(f"exported {ok}, skipped {skipped} (empty), failed {failed}, of {len(files)} files -> {outdir}")
        return 1 if failed else 0
    if len(args.paths) != 2:
        print("usage: dmbmx export <file.zmd> <out.glb> [--lod N] [--obj]")
        return 2
    try:
        r = _export_one(args.paths[0], args.paths[1], args)
    except (FormatError, ValueError) as e:
        print(f"error: {e}")
        return 1
    return 0 if r is not None else 1

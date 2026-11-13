"""dmbmx export-combo: rider + bike in one animated .glb. See docs/tools/export.md section 6."""
import os
from pathlib import Path

from dmbmx.convert import combo
from dmbmx.formats import motion, zmd

DEFAULT_GAME_DIR = r"F:\Projects\Dave Mirra freestyle BMX"


def register(sub):
    p = sub.add_parser("export-combo", help="export a rider on a bike as one animated glTF (.glb)")
    p.add_argument("rider", help="rider .zmd (e.g. DaveMirra1.ZMD)")
    p.add_argument("bike", help="bike .zmd (e.g. DMbike01.zmd)")
    p.add_argument("out", help="output .glb")
    p.add_argument("--anims", default="8", metavar="IDS",
                   help="motion ids: 'all', '8', '1,5,10-12' (default 8, the pedalling clip); 'none' = static")
    p.add_argument("--motion-zal", default=None, help="motion.zal (default: <game-dir>/assets/motion.zal)")
    p.add_argument("--game-dir", default=os.environ.get("DMBMX_GAME_DIR", DEFAULT_GAME_DIR))
    p.add_argument("--no-wheel-spin", action="store_true", help="don't derive wheel rotation from the crank")
    p.add_argument("--gear-ratio", type=float, default=combo.GEAR_RATIO,
                   help=f"wheel turns per crank turn (default {combo.GEAR_RATIO:.4g}, [I] 44/16)")
    p.set_defaults(func=run)


def _ids(spec, available):
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
    bad = [i for i in out if i not in available]
    if bad:
        raise SystemExit(f"no motion ids {bad}")
    return out


def run(args):
    rdata, bdata = Path(args.rider).read_bytes(), Path(args.bike).read_bytes()
    r, b = zmd.read(rdata), zmd.read(bdata)
    motions = []
    if args.anims.strip().lower() != "none":
        mz = Path(args.motion_zal or Path(args.game_dir) / "assets" / "motion.zal")
        bank = motion.read_bank(mz.read_bytes())
        motions = [(f"motion_{i:03d}", bank.motion(i)) for i in _ids(args.anims, bank.ids)]
    glb = combo.export_combo(r, b, motions, rider_source=rdata, bike_source=bdata,
                             rider_name=Path(args.rider).stem, bike_name=Path(args.bike).stem,
                             spin_wheels=not args.no_wheel_spin, ratio=args.gear_ratio)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(glb)
    print(f"{out}: {Path(args.rider).name} on {Path(args.bike).name}, {len(motions)} clip(s), {len(glb)} bytes")
    return 0

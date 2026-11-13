"""dmbmx motion list|dump: inspect the motion bank in motion.zal. glTF output: dmbmx export --anims."""
import json
import os
from pathlib import Path

from ..formats import motion as M

DEFAULT_GAME_DIR = r"F:\Projects\Dave Mirra freestyle BMX"


def register(sub):
    p = sub.add_parser("motion", help="list / dump motions from motion.zal")
    ms = p.add_subparsers(dest="motion_cmd", metavar="<action>")
    pl = ms.add_parser("list", help="one line per motion: frames, duration, sections, records")
    pl.add_argument("file", nargs="?", default=None, help="motion.zal (default: install assets/motion.zal)")
    pl.set_defaults(func=run_list)
    pd = ms.add_parser("dump", help="decoded per-frame channel values of one motion as JSON")
    pd.add_argument("id", type=lambda s: int(s, 0))
    pd.add_argument("file", nargs="?", default=None)
    pd.add_argument("--quat", action="store_true", help="also give the local rotation quaternions")
    pd.set_defaults(func=run_dump)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _bank(path):
    if path is None:
        path = Path(os.environ.get("DMBMX_GAME_DIR", DEFAULT_GAME_DIR)) / "assets" / "motion.zal"
    return M.read_bank(Path(path).read_bytes())


def run_list(args):
    b = _bank(args.file)
    print(f"{'id':>4} {'frames':>6} {'ms':>6} {'rider':>5} {'bike':>4} {'records':>7} {'bytes':>6}")
    for mid in b.ids:
        raw = b.bank.find(mid).data()
        m = M.Motion.read(raw)
        bike = next((c for c, _, k in m.sections if k != 0), 0)
        rider = next((c for c, _, k in m.sections if k == 0), 0)
        print(f"{mid:4d} {m.nframes:6d} {m.duration_ms:6d} {rider:5d} {bike:4d} {len(m.records):7d} {len(raw):6d}")
    print(f"{len(b.ids)} motions")
    return 0


def run_dump(args):
    m = _bank(args.file).motion(args.id)
    frames = []
    for p in m.sample():
        f = {"time_ms": p.time_ms, "rider_trans": p.rider_trans, "bike_trans": p.bike_trans, "value": p.value,
             "rider": dict(zip(M.RIDER_CHANNELS, p.rider or [])),
             "bike": dict(zip(M.BIKE_CHANNELS, p.bike or []))}
        if args.quat:
            f["rider_quat"] = {n: [round(x, 6) for x in M.channel_rotation(n, e)] for n, e in f["rider"].items()}
            f["bike_quat"] = {n: [round(x, 6) for x in M.channel_rotation(n, e)] for n, e in f["bike"].items()}
        frames.append(f)
    print(json.dumps({"id": args.id, "frames": m.nframes, "frame_ms": m.frame_ms, "sections": m.sections,
                      "poses": frames}, indent=1))
    return 0

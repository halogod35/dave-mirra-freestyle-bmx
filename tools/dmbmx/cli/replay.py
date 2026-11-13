"""dmbmx replay list|dump: inspect the demo replays in replay.zal (DM-052, docs/formats/REPLAY.md)."""
import json
import os
from pathlib import Path

from ..formats import replay as R
from ..formats import zal as Z

DEFAULT_GAME_DIR = r"F:\Projects\Dave Mirra freestyle BMX"
# record id = rider + 2 * level (action.c); the attract demo cycles g_actionDemoRuns {rider, level}:
# {0, 4}, {1, 0}, {1, 1}. Level folders in g_currentLevel order (docs/formats/LEVEL.md section 1).
LEVELS = ("GreenvilleDirt", "GreenvilleStreet", "GreenvilleVert", "WoodwardDirt", "WoodwardStreet",
          "WoodwardVert", "ProTourDirt", "ProTourStreet", "ProTourVert", "AcclaimDirt", "AcclaimStreet",
          "AcclaimVert")
DEMO_IDS = (8, 1, 3)


def register(sub):
    p = sub.add_parser("replay", help="list / dump the replay.zal demo records")
    ms = p.add_subparsers(dest="replay_cmd", metavar="<action>")
    pl = ms.add_parser("list", help="one line per record: layout, size, frames, events")
    pl.add_argument("file", nargs="?", default=None, help="replay.zal (default: install assets/replay.zal)")
    pl.set_defaults(func=run_list)
    pd = ms.add_parser("dump", help="header, events and per-frame input of one record as JSON")
    pd.add_argument("id", type=lambda s: int(s, 0))
    pd.add_argument("file", nargs="?", default=None)
    pd.add_argument("--frames", action="store_true", help="also the per-frame input (playback)")
    pd.set_defaults(func=run_dump)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _records(path):
    if path is None:
        path = Path(os.environ.get("DMBMX_GAME_DIR", DEFAULT_GAME_DIR)) / "assets" / "replay.zal"
    return R.records(Z.read(Path(path).read_bytes()))


def run_list(args):
    print(f"{'id':>3} {'layout':6} {'bytes':>6} {'frames':>6} {'seconds':>7} {'events':>6}  rider level")
    for rid, r in sorted(_records(args.file).items()):
        size = len(r.write())
        if r.layout == "pc":
            frames, events = r.fields["frames"], len(r.events())
        elif r.layout == "psx":
            frames, events = int.from_bytes(r.header[8:12], "little"), r.used_length() // 3
        else:
            frames = events = 0
        demo = "  (attract demo)" if rid in DEMO_IDS else ""
        print(f"{rid:3d} {r.layout:6} {size:6d} {frames:6d} {frames / 30:7.1f} {events:6d}  "
              f"{rid % 2:5d} {LEVELS[rid // 2]}{demo}")
    return 0


def run_dump(args):
    recs = _records(args.file)
    if args.id not in recs:
        print(f"no record {args.id}")
        return 1
    r = recs[args.id]
    out = {"id": args.id, "layout": r.layout}
    if r.layout == "pc":
        out["fields"] = r.fields
        out["flags"] = r.flags()
        out["events"] = [e._asdict() for e in r.events()]
        if args.frames:
            out["frames"] = [f._asdict() for f in r.play()]
    elif r.layout == "psx":
        out["header"] = r.header.hex()
        out["used"] = r.used_length()
    print(json.dumps(out, indent=1))
    return 0

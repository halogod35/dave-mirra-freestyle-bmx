"""dmbmx zal list|extract|pack|replace: inspect, unpack and rebuild .zal asset libraries (DM-010, DM-023)."""
import os
from pathlib import Path

from ..core.errors import FormatError
from ..core.hash import zhash
from ..formats import zal, zal_dir


def register(sub):
    p = sub.add_parser("zal", help="list / extract / pack / replace .zal asset libraries")
    zs = p.add_subparsers(dest="zal_cmd", metavar="<action>")
    pl = zs.add_parser("list", help="print the group table")
    pl.add_argument("file")
    pl.add_argument("-r", "--recursive", action="store_true", help="also list nested .zal entries")
    pl.set_defaults(func=run_list)
    pe = zs.add_parser("extract", help="write every asset (decompressed) to a directory")
    pe.add_argument("file")
    pe.add_argument("outdir", help="output directory (use out/..., never the game install)")
    pe.add_argument("-r", "--recursive", action="store_true", help="also unpack nested .zal entries")
    pe.add_argument("--raw", action="store_true", help="write stored (still compressed) bytes; not packable")
    pe.set_defaults(func=run_extract)
    pp = zs.add_parser("pack", help="rebuild a .zal from an `extract` directory (edited files are re-encoded)")
    pp.add_argument("dir")
    pp.add_argument("out", help="output .zal (never inside the game install)")
    pp.add_argument("-v", "--verbose", action="store_true", help="list every asset and whether it was kept")
    pp.set_defaults(func=run_pack)
    pr = zs.add_parser("replace", help="replace one asset's payload and write a new .zal")
    pr.add_argument("file")
    pr.add_argument("id", help="group id (0x7531, 30001), a name (hashed with zhash), or a nested path "
                               "like 0/0xe003 (outer group 0, inner group 0xE003)")
    pr.add_argument("payload", help="new (uncompressed) payload file, e.g. an edited .bmp")
    pr.add_argument("out", help="output .zal (never inside the game install)")
    pr.set_defaults(func=run_replace)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _load(path):
    with open(path, "rb") as f:
        return zal.read(f.read())


def _list(arc, recursive=False, indent=""):
    print(f"{indent}{arc!r}")
    print(f"{indent}{'id':>10} {'offset':>10} {'size':>10} {'stored':>10}  type")
    for e in arc.entries:
        for i, a in enumerate(e.assets):
            try:
                payload = arc.asset_data(a)
                kind = zal_dir.kind_of(payload)
            except FormatError as ex:
                payload, kind = b"", f"<{ex}>"
            sub = f"[{i}]" if len(e.assets) > 1 else ""
            print(f"{indent}0x{e.id:08x}{sub} 0x{a.offset + zal.HEADER_SIZE:08x} 0x{a.size:08x} "
                  f"0x{len(a.stored):08x}  {kind}")
            if recursive and kind == "ZAL" and len(indent) < 16:
                _list(zal.read(payload), recursive, indent + "    ")


def run_list(args):
    _list(_load(args.file), args.recursive)
    return 0


def _guard_outdir(outdir):
    """AD-1: refuse any output path inside the game install (except the repo / worktrees)."""
    game = os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX")
    out = Path(outdir).resolve()
    g = Path(game).resolve()
    allowed = g / "dave-mirra-bmx", g / "dave-mirra-pro-bmx", g / "wt"
    if (out == g or g in out.parents) and not any(out == a or a in out.parents for a in allowed):
        raise SystemExit(f"refusing to write into the game install ({out}); use out/ (AD-1)")


def run_extract(args):
    _guard_outdir(args.outdir)
    n = zal_dir.extract_dir(_load(args.file), Path(args.outdir), args.recursive, args.raw)
    print(f"extracted {n} assets from {args.file} to {args.outdir}")
    return 0


def run_pack(args):
    _guard_outdir(args.out)
    report = []
    data = zal_dir.pack_dir(args.dir, report=report)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(data)
    changed = [r for r in report if r[2] != "kept"]
    if args.verbose:
        for d, gid, action in report:
            print(f"  {d}: 0x{gid:08x} {action}")
    print(f"packed {len(report)} assets ({len(changed)} re-encoded) -> {args.out} ({len(data)} bytes)")
    return 0


def parse_id_path(text):
    """'0x7531' / '30001' / 'Some Name' / '0/0xe003' -> list of int group ids."""
    out = []
    for part in text.split("/"):
        part = part.strip()
        try:
            out.append(int(part, 0))
        except ValueError:
            out.append(zhash(part))
    return out


def run_replace(args):
    _guard_outdir(args.out)
    arc = _load(args.file)
    ids = parse_id_path(args.id)
    payload = Path(args.payload).read_bytes()
    try:
        zal_dir.replace(arc, ids, payload)
    except KeyError as e:
        raise SystemExit(f"no group {e} in {args.file} (path {'/'.join(hex(i) for i in ids)})")
    data = arc.write()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(data)
    print(f"replaced {'/'.join(hex(i) for i in ids)} ({len(payload)} bytes) -> {args.out} ({len(data)} bytes)")
    return 0

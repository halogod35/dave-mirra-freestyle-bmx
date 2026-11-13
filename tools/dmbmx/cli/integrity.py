"""dmbmx integrity check|fix-png|resign-zsd: the game's file checks (docs/re/integrity-checks.md)."""
from pathlib import Path

from dmbmx.core.errors import FormatError
from dmbmx.integrity import png, zsd
from dmbmx.integrity.crc import check_save


def register(sub):
    p = sub.add_parser("integrity", help="check / re-sign files against the game's load-time checks")
    ss = p.add_subparsers(dest="integrity_cmd", metavar="<integrity command>")
    pc = ss.add_parser("check", help="would the game accept these files? (SAVE.DAT, .ZSD, embedded PNG CRCs)")
    pc.add_argument("paths", nargs="+", help="files or folders (folders are walked)")
    pc.set_defaults(func=run_check)
    pf = ss.add_parser("fix-png", help="recompute the chunk CRCs of every PNG embedded in a file (length unchanged)")
    pf.add_argument("input")
    _out(pf)
    pf.set_defaults(func=run_fix_png)
    pz = ss.add_parser("resign-zsd", help="set the SCFI component sizes of a .ZSD to the files next to it")
    pz.add_argument("input")
    _out(pz)
    pz.set_defaults(func=run_resign_zsd)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _out(q):
    g = q.add_mutually_exclusive_group(required=True)
    g.add_argument("-o", "--output")
    g.add_argument("--in-place", action="store_true")


def _target(args):
    out = Path(args.input if args.in_place else args.output)
    if not args.in_place and out.resolve() == Path(args.input).resolve():
        raise SystemExit("error: output equals input; use --in-place")
    return out


# Folder walks only look at game file types (explicit file arguments are always checked).
GAME_EXTS = {".zmd", ".zal", ".zsd", "._sd", ".dff", ".png", ".bmp"}


def _files(paths):
    for p in map(Path, paths):
        if p.is_dir():
            yield from sorted(x for x in p.rglob("*") if x.is_file()
                              and (x.suffix.lower() in GAME_EXTS or x.name.lower() == "save.dat"))
        else:
            yield p


def check_one(path):
    """Return (ok, [messages]) for one file."""
    name = path.name.lower()
    data = path.read_bytes()
    msgs = []
    if name == "save.dat":
        ok, why = check_save(data)
        return ok, [f"save: {why}"]
    if name.endswith(".zsd") and data[:4] == b"ZSND":
        return True, ["audio bank (ZSND): no load-time checks"]
    if name.endswith(".zsd"):
        try:
            res = zsd.check(path)
        except FormatError as e:
            return False, [f"zsd: {e}"]
        bad = [f"SCFI {e.name}: stored {e.size}, file {real}" for e, real, ok in res if not ok]
        return not bad, bad or [f"zsd: {len(res)} SCFI sizes match"]
    bad = png.check(data)
    for pofs, cofs, t, st, cp, crit in bad:
        if cofs is None:
            msgs.append(f"png@0x{pofs:x}: {t}")
        else:
            msgs.append(f"png@0x{pofs:x} {t}@0x{cofs:x}: CRC {st:08x} != {cp:08x} ({'critical: texture fails' if crit else 'ancillary: warning only'})")
    n = len(png.find(data))
    return not any(b[5] for b in bad), msgs or ([f"{n} embedded PNG(s), CRCs ok"] if n else [])


def run_check(args):
    worst = 0
    for f in _files(args.paths):
        ok, msgs = check_one(f)
        if msgs:
            print(f"{'OK  ' if ok else 'FAIL'} {f}: " + "; ".join(msgs))
        worst = worst or (0 if ok else 1)
    return worst


def run_fix_png(args):
    out = _target(args)
    data = Path(args.input).read_bytes()
    fixed = png.fix(data)
    out.write_bytes(fixed)
    n = sum(1 for a, b in zip(data, fixed) if a != b)
    print(f"wrote {out}: {n} CRC byte(s) changed, {len(png.check(fixed))} problem(s) left")
    return 0


def run_resign_zsd(args):
    out = _target(args)
    try:
        new = zsd.resign_file(args.input)
    except FormatError as e:
        print(f"error: {e}")
        return 2
    old = Path(args.input).read_bytes()
    out.write_bytes(new)
    for e in zsd.entries(new):
        print(f"SCFI type {e.type} {e.name}: size {e.size}")
    print(f"wrote {out} ({'unchanged' if new == old else 'sizes updated'})")
    return 0

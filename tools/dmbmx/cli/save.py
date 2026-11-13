"""dmbmx save info|fix-crc|list-fields|get|set|unlock-all|complete-all: inspect and edit SAVE.DAT."""
from pathlib import Path

from dmbmx.core.errors import FormatError
from dmbmx.formats import save as fmt
from dmbmx.integrity.crc import check_save, resign_save


def register(sub):
    p = sub.add_parser("save", help="SAVE.DAT tools (info, fix-crc)")
    ss = p.add_subparsers(dest="save_cmd", metavar="<save command>")
    pi = ss.add_parser("info", help="decode a SAVE.DAT and say whether the game would accept it")
    pi.add_argument("file")
    pi.add_argument("--scores", action="store_true", help="list all 12x24 high-score entries")
    pi.set_defaults(func=run_info)
    pf = ss.add_parser("fix-crc", aliases=["resign"],
                       help="fix header (magic + size) so the game accepts an edited save; SAVE.DAT has no CRC")
    pf.add_argument("input")
    pf.add_argument("output")
    pf.set_defaults(func=run_fix)

    pl = ss.add_parser("list-fields", help="list editable field patterns (type, range, evidence)")
    pl.add_argument("file", nargs="?", help="also show values (expands patterns with at most --max names)")
    pl.add_argument("--grep", help="only fields whose pattern contains this text")
    pl.add_argument("--max", type=int, default=24, help="expand patterns up to this many names when a file is given")
    pl.set_defaults(func=run_list)
    pg = ss.add_parser("get", help="print field values")
    pg.add_argument("file")
    pg.add_argument("field", nargs="+")
    pg.set_defaults(func=run_get)
    for name, hlp in (("set", "set fields: set <file> <field> <value> [<field> <value> ...] -o OUT"),
                      ("unlock-all", "unlock all riders, levels, styles, bikes, cheats and movies"),
                      ("complete-all", "complete every challenge and win every competition, for every rider, "
                                       "with the game's own rewards (raise-only)")):
        q = ss.add_parser(name, help=hlp)
        q.add_argument("file")
        if name == "set":
            q.add_argument("pairs", nargs="+", metavar="field value")
        out = q.add_mutually_exclusive_group(required=True)
        out.add_argument("-o", "--output", help="write the edited save here (a new file)")
        out.add_argument("--in-place", action="store_true", help="overwrite the input file")
        q.add_argument("--lenient", action="store_true", help="accept a save with a bad magic/size header")
        q.set_defaults(func={"set": run_set, "unlock-all": run_unlock_all, "complete-all": run_complete_all}[name])
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _load(path, lenient=False):
    try:
        return fmt.read(Path(path).read_bytes(), lenient=lenient)
    except FormatError as e:
        raise SystemExit(f"error: {e} (use --lenient to edit anyway)")


def _write(s, args):
    out = Path(args.file if args.in_place else args.output)
    if not args.in_place and out.resolve() == Path(args.file).resolve():
        raise SystemExit("error: output equals input; use --in-place to overwrite")
    data = fmt.write(s.resign())          # header always valid (no CRC exists)
    out.write_bytes(data)
    ok, why = check_save(data)
    print(f"wrote {out} ({len(data)} bytes); game accepts: {'yes' if ok else 'NO'} ({why})")


def _show(f, name, v):
    from dmbmx.formats import save_fields as F
    lab = F.label(f, v)
    shown = repr(v) if isinstance(v, str) else (f"{v} (0x{v:X})" if f.kind in ("u16", "u32") or "flags" in name else str(v))
    return f"{name} = {shown}" + (f"  [{lab}]" if lab else "")


def run_list(args):
    from dmbmx.formats import save_fields as F
    s = _load(args.file, lenient=True) if args.file else None
    for f in F.FIELDS:
        if args.grep and args.grep not in f.pattern:
            continue
        rng = f"len<={f.hi}" if f.kind == "str" else f"{f.lo}..{f.hi}" if f.hi < 0x10000 else f"0..0x{f.hi:X}"
        idx = " ".join(f"{r.start}..{r.stop - 1}" for r in f.idx)
        print(f"{f.pattern:52} {f.kind:6} {rng:12} [{f.tag}] {('idx ' + idx) if idx else ''}")
        if f.doc or f.va:
            print(f"    {f.doc}{'  (' + f.va + ')' if f.va else ''}")
        if f.choices:
            print(f"    values: " + ", ".join(f"{i}={c}" for i, c in enumerate(f.choices)))
        if s is not None:
            names = list(F.concrete_names(f))
            for n in names[:args.max]:
                print("    " + _show(f, n, F.get(s, n)))
            if len(names) > args.max:
                print(f"    ... {len(names) - args.max} more")
    return 0


def run_get(args):
    from dmbmx.formats import save_fields as F
    s = _load(args.file, lenient=True)
    rc = 0
    for name in args.field:
        try:
            f, _ = F.resolve(name)
            print(_show(f, name, F.get(s, name)))
        except F.FieldError as e:
            print(f"error: {e}")
            rc = 2
    return rc


def run_set(args):
    from dmbmx.formats import save_fields as F
    if len(args.pairs) % 2:
        raise SystemExit("error: set needs <field> <value> pairs")
    s = _load(args.file, args.lenient)
    for name, value in zip(args.pairs[::2], args.pairs[1::2]):
        try:
            f, _ = F.resolve(name)
            old = F.get(s, name)
            F.set(s, name, value)
        except F.FieldError as e:
            raise SystemExit(f"error: {e}")
        print(f"{_show(f, name, old)}  ->  {F.get(s, name)!r}")
    _write(s, args)
    return 0


def run_unlock_all(args):
    from dmbmx.formats import save_fields as F
    s = _load(args.file, args.lenient)
    changes = F.unlock_all(s)
    for name, old, new in changes:
        print(f"{name}: {old} -> {new}")
    print(f"{len(changes)} fields changed")
    _write(s, args)
    return 0


def run_complete_all(args):
    from dmbmx.formats import progress as P
    from dmbmx.formats import save_fields as F
    s = _load(args.file, args.lenient)
    before = {n: F.get(s, n) for n in _summary_fields()}
    applied = P.complete_all(s)
    for name, old in before.items():
        new = F.get(s, name)
        if new != old:
            print(f"{name}: {old} -> {new}")
    print(f"{sum(len(v) for v in applied.values())} rewards applied for {sum(1 for v in applied.values() if v)} riders")
    _write(s, args)
    return 0


def _summary_fields():
    names = ["riders_unlocked", "cheats_unlocked"]
    for r in range(fmt.RIDER_COUNT):
        names += [f"rider.{r}.flags", f"rider.{r}.levels_unlocked", f"rider.{r}.styles_unlocked",
                  f"rider.{r}.bikes_unlocked"]
        names += [f"rider.{r}.level.{L}.tier" for L in range(6)]
        names += [f"rider.{r}.comp.{L}" for L in range(6, 12)]
    return names


def run_info(args):
    data = Path(args.file).read_bytes()
    ok, why = check_save(data)
    print(f"{args.file}: {len(data)} bytes; game accepts: {'yes' if ok else 'NO'} ({why})")
    try:
        s = fmt.read(data, lenient=True)
    except FormatError as e:
        print(f"cannot decode: {e}")
        return 2
    print(f"magic 0x{s.magic:08X}{'' if s.magic == fmt.MAGIC else ' (unexpected; the game ignores it)'}"
          f"  size field 0x{s.size_field:X}  integrity: none (no CRC in SAVE.DAT)")
    print(f"options @0x08: {s.options.hex(' ')}  (word_08={s.word_08} word_0a={s.word_0a} "
          f"vol_0c={s.volume_0c} vol_0d={s.volume_0d})  current_profile={s.current_profile}")
    for i, p in enumerate(s.profiles):
        print(f"profile {i}: name={p.name!r} head={p.raw[:7].hex(' ')} controls={[hex(c) for c in p.controls]}")
    for lv, row in enumerate(s.scores):
        top = row[0]
        print(f"level {lv:2d}: top record {top.score} by {top.name!r}; rider {lv:2d} record head={s.riders[lv][:8].hex(' ')}")
        if args.scores:
            for e, sc in enumerate(row):
                print(f"    {e:2d} {sc.score:8d} {sc.name!r:16} b4..b6={sc.b4},{sc.b5},{sc.b6}")
    if s.extra:
        print(f"extra: {len(s.extra)} trailing bytes (ignored by the game)")
    return 0 if ok else 1


def run_fix(args):
    data = Path(args.input).read_bytes()
    try:
        out = resign_save(data)
    except FormatError as e:
        print(f"error: {e}")
        return 2
    Path(args.output).write_bytes(out)
    ok, why = check_save(out)
    print(f"wrote {args.output} ({len(out)} bytes); {'unchanged' if out == data else 'header fixed'}; "
          f"game accepts: {'yes' if ok else 'NO'} ({why})")
    return 0

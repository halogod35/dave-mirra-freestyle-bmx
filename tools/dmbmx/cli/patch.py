"""dmbmx patch list|apply|nocrc: write a patched COPY of NgBMX.exe from tools/dmbmx/patch/patches.json."""
from dmbmx.patch import patcher


def register(sub):
    p = sub.add_parser("patch", help="write a patched copy of NgBMX.exe (patches.json)")
    ss = p.add_subparsers(dest="patch_cmd", metavar="<patch command>")
    pl = ss.add_parser("list", help="list patches and groups")
    pl.set_defaults(func=run_list)
    pa = ss.add_parser("apply", help="apply selected patches: apply IN OUT --id X [--id Y] | --group G")
    pa.add_argument("input")
    pa.add_argument("output")
    pa.add_argument("--id", action="append", dest="ids", default=[])
    pa.add_argument("--group")
    pa.set_defaults(func=run_apply)
    pn = ss.add_parser("nocrc", help="apply the 'nocrc' group (the save size-field check; the exe has no CRC check)")
    pn.add_argument("input")
    pn.add_argument("output")
    pn.set_defaults(func=lambda a: run_apply(a, group="nocrc"))
    p.set_defaults(func=lambda a: p.print_help() or 1)


def run_list(args):
    spec = patcher.load()
    print(f"{spec['exe']} sha256 {spec['sha256']}\n{spec['note']}\n")
    for p in spec["patches"]:
        print(f"{p['id']:18} VA {p['va']} file {p['file_offset']}  {p['orig']} -> {p['new']}\n    {p['reason']}")
    print("\ngroups: " + "; ".join(f"{k} = {', '.join(v)}" for k, v in spec["groups"].items()))
    return 0


def run_apply(args, group=None):
    try:
        patches, sha = patcher.apply_file(args.input, args.output, getattr(args, "ids", None),
                                          group or getattr(args, "group", None))
    except (patcher.PatchError, OSError) as e:
        print(f"error: {e}")
        return 2
    for p in patches:
        print(f"applied {p['id']} at VA {p['va']} (file {p['file_offset']}): {p['orig']} -> {p['new']}")
    print(f"wrote {args.output} sha256 {sha}")
    return 0

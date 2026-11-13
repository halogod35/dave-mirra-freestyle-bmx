"""CLI plugin discovery: every module tools/dmbmx/cli/<name>.py exposing register(subparsers) is loaded.

A plugin does:  def register(sub): p = sub.add_parser("zmd", help=...); ...; p.set_defaults(func=handler)
"""
import argparse, importlib, pkgutil


def build_parser():
    ap = argparse.ArgumentParser(prog="dmbmx", description="Dave Mirra Freestyle BMX file tools")
    sub = ap.add_subparsers(dest="command", metavar="<command>")
    for m in sorted(pkgutil.iter_modules(__path__), key=lambda m: m.name):
        mod = importlib.import_module(f"{__name__}.{m.name}")
        if hasattr(mod, "register"):
            mod.register(sub)
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 1
    return args.func(args) or 0

"""dmbmx gui: menu modding. Menu art, screens and fonts of Intwrap.zal, the menu / HUD texts of
MiscData.ZAL and the HUD fonts. Usage notes: docs/tools/menu-modding.md.

  gui list <Intwrap.zal> [--screen ID]            groups and assets; with --screen, its elements and texts
  gui export <Intwrap.zal> <dir> [--screen ID]    PNG + JSON for every asset (or one screen's group)
  gui import <Intwrap.zal> <dir> <out.zal>        rebuild Intwrap.zal from an edited export
  gui texts <MiscData.ZAL> [--table ID] [--json F] print the STRS texts, or export them to JSON
  gui text-set <MiscData.ZAL> <text id> <text> <out.zal> [--table ID]  change one text
  gui text-import <MiscData.ZAL> <texts.json> <out.zal>
  gui hud-export <assets dir> <dir>               HUD fonts: metrics JSON + glyph sheet PNG
  gui hud-import <assets dir> <dir> <outdir>      writes MiscData.ZAL / Material.ZAL that changed
"""
import sys
from pathlib import Path

from ..core.errors import FormatError
from ..formats import gui_dir, scrn, strs, zprim
from .zal import _guard_outdir


def _int(s):
    """0xA005, 40965, or bare hex such as A005."""
    try:
        return int(s, 0)
    except ValueError:
        return int(s, 16)


def register(sub):
    p = sub.add_parser("gui", help="menu modding: GUI art / screens / fonts, menu and HUD texts")
    gs = p.add_subparsers(dest="gui_cmd", metavar="<action>")
    a = gs.add_parser("list", help="list the GUI groups and assets, or one screen's elements")
    a.add_argument("intwrap")
    a.add_argument("--screen", type=_int, help="screen id, e.g. 0xA005 (main menu)")
    a.set_defaults(func=run_list)
    a = gs.add_parser("export", help="export art (PNG) and screens / fonts (JSON) to a directory")
    a.add_argument("intwrap")
    a.add_argument("outdir", help="output directory (use out/..., never the game install)")
    a.add_argument("--screen", type=_int, action="append", help="only this screen's group (repeatable)")
    a.set_defaults(func=run_export)
    a = gs.add_parser("import", help="rebuild Intwrap.zal from an edited export directory")
    a.add_argument("intwrap")
    a.add_argument("dir")
    a.add_argument("out", help="output Intwrap.zal (never inside the game install)")
    a.set_defaults(func=run_import)
    a = gs.add_parser("texts", help="print the STRS text tables of MiscData.ZAL, or export them")
    a.add_argument("misc")
    a.add_argument("--table", type=_int, action="append", help="table id, e.g. 0x7919 (repeatable)")
    a.add_argument("--json", help="write the tables to this JSON file instead of printing")
    a.set_defaults(func=run_texts)
    a = gs.add_parser("text-set", help="change one text and write a new MiscData.ZAL")
    a.add_argument("misc")
    a.add_argument("text_id", type=_int, help="text id, e.g. 0x24 (PROQUEST)")
    a.add_argument("text")
    a.add_argument("out")
    a.add_argument("--table", type=_int, action="append",
                   help="table(s) to change; default: every table (all languages) whose ids include it")
    a.set_defaults(func=run_text_set)
    a = gs.add_parser("text-import", help="apply an edited `texts --json` file")
    a.add_argument("misc")
    a.add_argument("json")
    a.add_argument("out")
    a.set_defaults(func=run_text_import)
    a = gs.add_parser("hud-export", help="export the 4 HUD fonts (metrics JSON + glyph sheet PNG)")
    a.add_argument("assets", help="the game's assets folder (MiscData.ZAL, Material.ZAL)")
    a.add_argument("outdir")
    a.set_defaults(func=run_hud_export)
    a = gs.add_parser("hud-import", help="rebuild MiscData.ZAL / Material.ZAL from an edited hud-export")
    a.add_argument("assets")
    a.add_argument("dir")
    a.add_argument("outdir", help="MiscData.ZAL and/or Material.ZAL are written here (only the changed ones)")
    a.set_defaults(func=run_hud_import)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _asset_desc(aid, d):
    kind = scrn.asset_type(aid)
    try:
        if kind in ("picture", "sprite", "sprite2"):
            img = zprim.read_image(d)
            sizes = sorted({(f.width, f.height) for f in img.frames})
            sz = ", ".join(f"{w}x{h}" for w, h in sizes)
            return f"{kind:<8} {len(img.frames)} frame(s) {sz} {img.bpp}bpp {len(img.palette.colours)} colours"
        if kind == "font":
            f = zprim.read_font(d)
            return f"font     {len(f.glyphs)} glyphs, line {f.line_height}, sheet {f.width}x{f.height}"
        if kind == "screen":
            s = scrn.read_screen(d)
            return f"screen   {len(s.edefs)} elements, backdrop 0x{s.header['backdrop']:04x}"
    except FormatError as e:
        return f"{kind:<8} <{e}>"
    return kind


def run_list(args):
    iw = gui_dir.Intwrap(Path(args.intwrap).read_bytes())
    if args.screen is None:
        for gid, g in sorted(iw.groups.items()):
            print(f"group 0x{gid:04x}: {len(g.entries)} assets")
            for aid, d in g.entries:
                print(f"  0x{aid:04x} {len(d):>7}  {_asset_desc(aid, d)}")
        return 0
    sid = 0xA000 | (args.screen & 0xFFF)
    gid = 0xE000 | (args.screen & 0xFFF)
    if gid not in iw.groups or sid not in iw.groups[gid].ids():
        raise SystemExit(f"no screen 0x{sid:04x}")
    s = scrn.read_screen(iw.groups[gid].asset(sid))
    h = s.header
    print(f"screen 0x{sid:04x} (group 0x{gid:04x}): backdrop 0x{h['backdrop']:04x}, focus font "
          f"0x{h['focus_font']:04x} rgb {h['focus_rgb']}, blur font 0x{h['blur_font']:04x} rgb {h['blur_rgb']}")
    print(f"{'#':>3} {'id':>10} {'x':>5} {'y':>5} {'sprite':>6} {'frame':>5} {'font':>6} {'prio':>4}  text")
    for k, e in enumerate(s.edefs):
        t = s.string_at(e.text)
        g = s.string_at(e.toggle_text)
        txt = "" if t is None else repr(t)
        if g is not None:
            txt += f"  toggle {g!r}"
        print(f"{k:>3} 0x{e.id:08x} {e.x:>5} {e.y:>5} 0x{e.sprite:04x} {e.frame:>5} 0x{e.font:04x} "
              f"{e.priority:>4}  {txt}")
    print("art in the group: " + ", ".join(f"0x{a:04x}" for a in iw.groups[gid].ids() if a != sid))
    return 0


def run_export(args):
    _guard_outdir(args.outdir)
    n = gui_dir.export_gui(Path(args.intwrap).read_bytes(), Path(args.outdir), args.screen)
    print(f"wrote {n} files to {args.outdir}")
    return 0


def _write(path, data):
    _guard_outdir(Path(path).resolve().parent)
    _guard_outdir(path)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data)


def _warn(warnings):
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)


def run_import(args):
    warnings, report = [], []
    src = Path(args.intwrap).read_bytes()
    out = gui_dir.import_gui(src, Path(args.dir), warnings, report)
    _warn(warnings)
    for aid, kind, a, b, groups in report:
        print(f"  0x{aid:04x} {kind:<7} {a} -> {b} bytes, groups {', '.join(f'0x{g:04x}' for g in groups)}")
    _write(args.out, out)
    print(f"{len(report)} asset(s) changed; wrote {args.out} ({len(out)} bytes, was {len(src)})")
    return 0


def run_texts(args):
    data = Path(args.misc).read_bytes()
    if args.json:
        _guard_outdir(Path(args.json).resolve().parent)
        n = gui_dir.export_texts(data, args.json, args.table)
        print(f"wrote {n} table(s) to {args.json}")
        return 0
    from ..formats import zal
    tabs = gui_dir.strs_tables(zal.ZalArchive.read(data))
    for tid, e in sorted(tabs.items()):
        if args.table and tid not in args.table:
            continue
        t = strs.read(e.data())
        print(f"table 0x{tid:04x}: ids 0x{t.base_id:04x}-0x{t.base_id + len(t) - 1:04x}")
        for i in t.ids():
            print(f"  0x{i:04x} {t.get(i)!r}")
    return 0


def run_text_set(args):
    from ..formats import zal
    data = Path(args.misc).read_bytes()
    tabs = gui_dir.strs_tables(zal.ZalArchive.read(data))
    targets = args.table or [tid for tid, e in sorted(tabs.items())
                             if args.text_id in strs.read(e.data()).ids()]
    if not targets:
        raise SystemExit(f"no table holds text 0x{args.text_id:x}")
    report = []
    out = gui_dir.set_texts(data, {t: {args.text_id: args.text} for t in targets}, report)
    for tid, n, size in report:
        print(f"  table 0x{tid:04x}: text 0x{args.text_id:x} = {args.text!r} ({size} bytes)")
    _write(args.out, out)
    print(f"wrote {args.out}")
    return 0


def run_text_import(args):
    report = []
    out = gui_dir.import_texts(Path(args.misc).read_bytes(), args.json, report)
    for tid, n, size in report:
        print(f"  table 0x{tid:04x}: {n} text(s) changed ({size} bytes)")
    _write(args.out, out)
    print(f"wrote {args.out}")
    return 0


def _assets_file(folder, name):
    """The install's file name case varies (MiscData.ZAL / miscdata.zal): find it."""
    for p in Path(folder).iterdir():
        if p.name.lower() == name.lower():
            return p
    raise SystemExit(f"no {name} in {folder}")


def run_hud_export(args):
    _guard_outdir(args.outdir)
    misc = _assets_file(args.assets, "MiscData.ZAL").read_bytes()
    mat = _assets_file(args.assets, "Material.ZAL").read_bytes()
    n = gui_dir.export_hud(misc, mat, args.outdir)
    print(f"wrote {n} fonts to {args.outdir}")
    return 0


def run_hud_import(args):
    _guard_outdir(args.outdir)
    mp = _assets_file(args.assets, "MiscData.ZAL")
    tp = _assets_file(args.assets, "Material.ZAL")
    warnings, report = [], []
    misc, mat = gui_dir.import_hud(mp.read_bytes(), tp.read_bytes(), args.dir, warnings, report)
    _warn(warnings)
    for f, aid, a, b in report:
        print(f"  {f} 0x{aid:04x}: {a} -> {b} bytes")
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    for data, p in ((misc, mp), (mat, tp)):
        if data is not None:
            (out / p.name).write_bytes(data)
            print(f"wrote {out / p.name}")
    if misc is None and mat is None:
        print("nothing changed")
    return 0

"""dmbmx tex list|export|replace: embedded textures in .zmd, level Mat._sd and .zal (DM-014)."""
import json
import os
from pathlib import Path

from ..core.errors import FormatError
from ..formats import texture as T
from ..formats import zal, zmd
from ..formats.mati import SURFACE_NAMES, MatSd


def register(sub):
    p = sub.add_parser("tex", help="list / export / replace textures (ZMD, Mat._sd, material.zal)")
    ts = p.add_subparsers(dest="tex_cmd", metavar="<action>")
    pl = ts.add_parser("list", help="print every texture with format, size and MADF fields")
    pl.add_argument("file")
    pl.set_defaults(func=run_list)
    pe = ts.add_parser("export", help="write every texture as-is (and optionally as PNG)")
    pe.add_argument("file", help=".zmd, Mat._sd or .zal")
    pe.add_argument("outdir", help="output directory (use out/..., never the game install)")
    pe.add_argument("--png", action="store_true", help="also write <name>.png (BMP converted, RGBA)")
    pe.add_argument("--colorkey", action="store_true",
                    help="with --png: make 255,0,255 pixels of BMPs transparent, as the game does")
    pe.set_defaults(func=run_export)
    pr = ts.add_parser("replace", help="replace one texture's image and write a new file")
    pr.add_argument("file", help=".zmd or Mat._sd")
    pr.add_argument("name", help="texture name (case-insensitive), e.g. NAME_801_1")
    pr.add_argument("image", help=".bmp or .png; converted to the slot's type if needed")
    pr.add_argument("out", help="output file (never inside the game install)")
    pr.add_argument("--resize", action="store_true", help="scale to the original texture's size")
    pr.add_argument("--keep-size", action="store_true", help="allow a size different from the original")
    pr.set_defaults(func=run_replace)
    p.set_defaults(func=lambda a: p.print_help() or 1)


class Source:
    """Uniform view: ``textures`` = list of (name, kind, image bytes, mati or None)."""

    def __init__(self, path):
        self.path = Path(path)
        data = self.path.read_bytes()
        low = self.path.name.lower()
        self.zal = None
        if low.endswith(".zmd"):
            self.obj = zmd.read(data)
            self.textures = [(m.name, m.kind, m.image, m) for m in self.obj.materials()]
        elif low.endswith(".zal"):
            self.obj = zal.read(data)
            self.textures = []
            for e in self.obj.entries:
                d = e.data()
                k = T.sniff(d)
                if k in ("bmp", "png"):
                    self.textures.append((f"{e.id:08x}", k, d, None))
        elif data[:4] == b"MATI":
            self.obj = MatSd.read(data)
            self.textures = [(m.name, m.kind, m.image, m) for m in self.obj.materials()]
        else:
            raise SystemExit(f"{path}: not a .zmd, .zal or Mat._sd file")

    def write(self):
        return self.obj.write()


def _guard_out(path):
    game = Path(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX")).resolve()
    out = Path(path).resolve()
    allowed = game / "dave-mirra-bmx", game / "dave-mirra-pro-bmx", game / "wt"
    if (out == game or game in out.parents) and not any(out == a or a in out.parents for a in allowed):
        raise SystemExit(f"refusing to write into the game install ({out}); use out/ (AD-1)")


def _desc(name, kind, image, m):
    try:
        i = T.info(image)
        fmt = (f"{i['width']}x{i['height']} " +
               (f"BMP{i['bpp']}" if kind == "bmp" else f"PNG ct{i['color_type']}"))
    except FormatError as e:
        fmt = f"<{e}>"
    extra = ""
    if m is not None:
        sw = m.surface_word[0]
        extra = f" surface={m.surface}:{SURFACE_NAMES.get(m.surface, '?')}"
        if sw >> 4 or m.anim1[0] or m.anim2[0]:
            extra += f" word38=0x{sw:x} anim=0x{m.anim1[0]:x}/0x{m.anim2[0]:x}"
    return f"{name:24s} {kind:4s} {len(image):7d}  {fmt}{extra}"


def run_list(args):
    s = Source(args.file)
    for t in s.textures:
        print(_desc(*t))
    print(f"{len(s.textures)} textures")
    return 0


def run_export(args):
    _guard_out(args.outdir)
    s = Source(args.file)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for name, kind, image, m in s.textures:
        (out / f"{name}.{kind}").write_bytes(image)
        rec = {"name": name, "kind": kind, "file": f"{name}.{kind}", "bytes": len(image)}
        try:
            rec.update({k: v for k, v in T.info(image).items() if k in ("width", "height", "bpp",
                                                                         "color_type")})
        except FormatError:
            pass
        if m is not None:
            rec.update({"name_id": m.name_id, "surface": m.surface, "madf_head": m.madf_head.hex()})
        if args.png and kind == "bmp":
            if args.colorkey:
                png = T.encode_png(T.apply_color_key(T.decode(image)))
            else:
                png = T.bmp_to_png(image)          # lossless, palettised stays palettised
            (out / f"{name}.png").write_bytes(png)
            rec["png"] = f"{name}.png"
        index.append(rec)
    (out / "_textures.json").write_text(json.dumps(index, indent=1), newline="\n")
    print(f"exported {len(index)} textures from {args.file} to {out}")
    return 0


def run_replace(args):
    _guard_out(args.out)
    s = Source(args.file)
    if s.zal is None and isinstance(s.obj, zal.ZalArchive):
        raise SystemExit("replace in .zal archives: use `dmbmx zal replace` (DM-023)")
    m = s.obj.texture(args.name)
    if m is None:
        names = ", ".join(t[0] for t in s.textures)
        raise SystemExit(f"no texture {args.name!r} in {args.file}; have: {names}")
    old = T.info(m.image)
    size = None if args.keep_size else (old["width"], old["height"])
    new = Path(args.image).read_bytes()
    try:
        data, notes = T.prepare_replacement(new, m.kind, size=size, resize=args.resize)
    except FormatError as e:
        raise SystemExit(f"cannot use {args.image}: {e}")
    _, warns = T.check(data, m.kind)
    m.set_image(data, m.kind)
    out = s.write()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(out)
    for n in notes:
        print(f"  note: {n}")
    for w in warns:
        print(f"  warning: {w}")
    print(f"replaced {m.name} ({m.kind}, {len(data)} bytes) -> {args.out} ({len(out)} bytes)")
    return 0

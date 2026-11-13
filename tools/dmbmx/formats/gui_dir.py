"""Menu-modding directories: export the GUI art / screens / texts / HUD fonts to PNG + JSON and back (DM-066).

Intwrap.zal (``export_gui`` / ``import_gui``)::

    <dir>/_gui.json                index (format "dmbmx-gui/1"): every asset, the groups that hold it
    <dir>/pictures/0009.png        + 0009.json   (backdrops, 512x512 8-bit)
    <dir>/sprites/40C7_0.png ...   + 40C7.json   (one PNG per frame; hot spots, palette in the JSON)
    <dir>/fonts/8002.png           + 8002.json   (glyph sheet; glyph metrics and the 16 colour banks)
    <dir>/screens/A005.json        the SCRN header, every element's fields and its texts

An asset id that appears in several groups (fonts 0x8001, ...) is exported once and imported into
every group that holds it ([V] equal ids hold equal bytes in the shipped archive).

MiscData.ZAL texts (``export_texts`` / ``import_texts``): one JSON with the STRS tables.
HUD fonts (``export_hud`` / ``import_hud``): MiscData.ZAL 0x7531.. metrics + their Material.ZAL sheets.

Unedited files import back to the original bytes (tests/test_zprim_corpus.py).
"""
import hashlib
import json
from pathlib import Path

from ..convert import imaging
from ..core.errors import FormatError
from . import hudfont, scrn, strs, zal, zprim

GUI_FORMAT = "dmbmx-gui/1"
TEXT_FORMAT = "dmbmx-strs/1"
HUD_FORMAT = "dmbmx-hudfont/1"
_DIRS = {"picture": "pictures", "sprite": "sprites", "sprite2": "sprites", "font": "fonts"}


class GuiDirError(FormatError):
    def __init__(self, msg):
        super().__init__(msg, fmt="GUIDIR")


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def _hx(v, w=4):
    return f"0x{v:0{w}x}"


def _scalar(v):
    return v is None or isinstance(v, (str, int, float, bool))


def _dumps(v, ind=""):
    """JSON with lists of scalars and dicts of scalars on one line (glyphs, coordinates)."""
    one = lambda x: json.dumps(x, ensure_ascii=False)          # noqa: E731
    if isinstance(v, list):
        if all(_scalar(x) for x in v) and not any(isinstance(x, str) and len(x) > 12 for x in v):
            return one(v)
        inner = ind + " "
        return "[\n" + ",\n".join(inner + _dumps(x, inner) for x in v) + "\n" + ind + "]"
    if isinstance(v, dict):
        if all(_scalar(x) or (isinstance(x, list) and all(_scalar(y) for y in x) and len(x) <= 4)
               for x in v.values()) and len(v) <= 12:
            return one(v)
        inner = ind + " "
        return "{\n" + ",\n".join(f"{inner}{one(k)}: {_dumps(x, inner)}" for k, x in v.items()) + "\n" + ind + "}"
    return one(v)


def _write_json(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_dumps(obj) + "\n", encoding="utf-8", newline="\n")


def _read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise GuiDirError(f"{path}: {e}") from e


def _colours_json(colours):
    """Palette rows of 16 space-separated 15-bit values (a "#rrggbb" token is accepted on import)."""
    return [" ".join(f"{c:04X}" for c in colours[i:i + 16]) for i in range(0, len(colours), 16)]


def _colours_parse(rows):
    return [zprim.parse_colour(t) for r in rows for t in r.split()]


# ------------------------------------------------------------------ Intwrap.zal

class Intwrap:
    """Intwrap.zal opened for editing: ``outer`` / ``inner`` archives, ``groups`` {id: Group}."""

    def __init__(self, data):
        self.outer = zal.ZalArchive.read(data)
        self.inner = zal.ZalArchive.read(self.outer.find(0).data())
        self.groups = {e.id: scrn.read_group(e.data()) for e in self.inner.entries}
        self.changed = set()

    def assets(self):
        """{asset id: (bytes, [group ids])} over all groups (first copy's bytes)."""
        out = {}
        for gid, g in self.groups.items():
            for aid, d in g.entries:
                if aid in out:
                    out[aid][1].append(gid)
                else:
                    out[aid] = (d, [gid])
        return out

    def set_asset(self, aid, data):
        """Put ``data`` into every group that holds ``aid``; returns the groups changed."""
        hit = []
        for gid, g in self.groups.items():
            if aid in g.ids() and g.asset(aid) != data:
                g.set_asset(aid, data)
                hit.append(gid)
        self.changed.update(hit)
        return hit

    def write(self):
        for gid in sorted(self.changed):
            self.inner.set_data(gid, self.groups[gid].write())
        if self.changed:
            self.outer.set_data(0, self.inner.write())
        return self.outer.write()


def _selected(iw, screens):
    """Asset ids to export: all, or the groups of the given screens (0xA000 | n -> group 0xE000 | n)."""
    allx = iw.assets()
    if not screens:
        return allx
    want = {0xE000 | (s & 0xFFF) for s in screens}
    missing = want - set(iw.groups)
    if missing:
        raise GuiDirError(f"no group for screen(s) {', '.join(_hx(0xA000 | (g & 0xFFF)) for g in sorted(missing))}")
    return {a: v for a, v in allx.items() if want & set(v[1])}


def image_json(aid, img, kind, pngs):
    return {"id": _hx(aid), "type": kind, "bpp": img.bpp, "flags": img.flags, "width": img.width,
            "height": img.height, "pad_12": img.pad_12,
            "frames": [{"png": p, "width": f.width, "height": f.height, "hot_x": f.hot_x, "hot_y": f.hot_y,
                        "material": f.material} for p, f in zip(pngs, img.frames)],
            "palette": {"unk": img.palette.unk, "colours": _colours_json(img.palette.colours),
                        "tail": img.palette.tail.hex()}}


def font_json(aid, font, png):
    gl = []
    for k, g in enumerate(font.glyphs):
        d = g.as_dict()
        ch = font.first + k if font.indexed else g.code
        d = {"char": chr(ch) if 0x20 <= ch < 0x7F or 0xA0 <= ch <= 0xFF else None, **d}
        gl.append(d)
    return {"id": _hx(aid), "type": "font", "png": png, "line_height": font.line_height, "bpp": font.bpp,
            "width": font.width, "height": font.height, "materials": font.materials, "unk_16": font.unk_16,
            "pad": font.pad.hex(), "indexed": font.indexed, "first": font.first,
            "palette": {"unk": font.palette.unk, "banks": _colours_json(font.palette.colours),
                        "tail": font.palette.tail.hex()},
            "glyphs": gl}


def screen_json(sid, scr):
    h = {k: scr.header[k] for k in ("backdrop", "unk_0e", "focus_font", "focus_rgb", "blur_font", "blur_rgb")}
    els = []
    for k, e in enumerate(scr.edefs):
        f = {n: v for n, v in e.fields.items() if n not in ("text", "toggle_text")}
        f["id"] = _hx(f["id"], 8)
        for n in ("sprite", "sel_sprite", "font", "toggle_font"):
            f[n] = _hx(f[n])
        els.append({"index": k, **f, "text": scr.string_at(e.text),
                    "toggle_text": scr.string_at(e.toggle_text)})
    return {"id": _hx(sid), "header": h, "elements": els}


def export_gui(data, outdir, screens=None):
    """Write the export directory; returns the number of files written."""
    iw = Intwrap(data)
    out = Path(outdir)
    index = {"format": GUI_FORMAT, "source_sha256": _sha(data), "assets": {}}
    n = 0
    for aid, (d, gids) in sorted(_selected(iw, screens).items()):
        kind = scrn.asset_type(aid)
        rec = {"type": kind, "groups": [_hx(g) for g in gids], "sha256": _sha(d)}
        stem = f"{aid:04X}"
        if kind in ("picture", "sprite", "sprite2"):
            img = zprim.read_image(d)
            sub = _DIRS[kind]
            pngs = [f"{stem}_{k}.png" if len(img.frames) > 1 or kind != "picture" else f"{stem}.png"
                    for k in range(len(img.frames))]
            for k, p in enumerate(pngs):
                (out / sub).mkdir(parents=True, exist_ok=True)
                (out / sub / p).write_bytes(zprim.frame_png(img, k))
                n += 1
            _write_json(out / sub / f"{stem}.json", image_json(aid, img, kind, pngs))
            rec["json"] = f"{sub}/{stem}.json"
        elif kind == "font":
            font = zprim.read_font(d)
            (out / "fonts").mkdir(parents=True, exist_ok=True)
            (out / "fonts" / f"{stem}.png").write_bytes(zprim.font_png(font))
            n += 1
            _write_json(out / "fonts" / f"{stem}.json", font_json(aid, font, f"{stem}.png"))
            rec["json"] = f"fonts/{stem}.json"
        elif kind == "screen":
            _write_json(out / "screens" / f"{stem}.json", screen_json(aid, scrn.read_screen(d)))
            rec["json"] = f"screens/{stem}.json"
        else:
            raise GuiDirError(f"asset {_hx(aid)}: unknown type {kind}")
        n += 1
        index["assets"][_hx(aid)] = rec
    _write_json(out / "_gui.json", index)
    return n + 1


def _load_image(js, base, orig, warnings):
    img = zprim.read_image(orig)
    try:
        json_cols = _colours_parse(js["palette"]["colours"])
        pal = zprim.Palette(json_cols, int(js["palette"]["unk"]), bytes.fromhex(js["palette"]["tail"]))
        frames = []
        for f, fj in zip(img.frames, js["frames"]):
            frames.append(zprim.Frame(f.width, f.height, f.pixels, int(fj["hot_x"]), int(fj["hot_y"]),
                                      int(fj["material"])))
        if len(js["frames"]) != len(img.frames):
            raise GuiDirError(f"{js['id']}: {len(js['frames'])} frames in the JSON, the asset has "
                              f"{len(img.frames)} (adding / removing frames is not supported)")
        base_img = zprim.Image(frames, pal, img.bpp, int(js["flags"]), int(js["width"]), int(js["height"]),
                               int(js["pad_12"]))
    except (KeyError, TypeError, ValueError) as e:
        raise GuiDirError(f"{js.get('id', '?')}: bad JSON ({e})") from e
    pngs = [(base / fj["png"]).read_bytes() for fj in js["frames"]]
    return zprim.image_from_png(base_img, pngs, warnings, reference=img.palette.colours)


def _load_font(js, base, orig, warnings):
    font = zprim.read_font(orig)
    try:
        cols = _colours_parse(js["palette"]["banks"])
        pal = zprim.Palette(cols, int(js["palette"]["unk"]), bytes.fromhex(js["palette"]["tail"]))
        glyphs = [zprim.Glyph(*(int(g[k]) for k in zprim.GLYPH_FIELDS)) for g in js["glyphs"]]
        base_font = zprim.Font(glyphs, font.width, font.height, font.pixels, pal, int(js["line_height"]),
                               font.bpp, int(js["materials"]), int(js["unk_16"]), bytes.fromhex(js["pad"]))
    except (KeyError, TypeError, ValueError) as e:
        raise GuiDirError(f"{js.get('id', '?')}: bad JSON ({e})") from e
    if len(glyphs) < 0x100:
        first = glyphs[0].code
        bad = [k for k, g in enumerate(glyphs) if g.code not in (0, first + k) and k]
        if bad:
            warnings.append(f"{js['id']}: glyph(s) {bad[:5]} have a code that is not first + index; "
                            "with fewer than 256 glyphs the game finds a glyph by its index")
    for k, g in enumerate(glyphs):
        if g.x + g.w > 255 + 1 or g.y + g.h > 255 + 1:
            warnings.append(f"{js['id']}: glyph {k} reaches past the 256x256 a byte can address")
    return zprim.font_from_png(base_font, (base / js["png"]).read_bytes(), warnings, reference=font.palette.colours)


def _load_screen(js, orig):
    scr = scrn.read_screen(orig)
    try:
        for k in ("backdrop", "unk_0e", "focus_font", "focus_rgb", "blur_font", "blur_rgb"):
            scr.header[k] = js["header"][k]
        els = js["elements"]
        if len(els) != len(scr.edefs):
            raise GuiDirError(f"{js['id']}: {len(els)} elements, the screen has {len(scr.edefs)} "
                              "(adding / removing elements is not supported)")
        texts = {}
        for k, (e, ej) in enumerate(zip(scr.edefs, els)):
            for name in e.fields:
                if name in ("text", "toggle_text"):
                    continue
                v = ej[name]
                e.fields[name] = int(v, 0) if isinstance(v, str) else v
            texts[(k, "text")] = ej["text"]
            texts[(k, "toggle_text")] = ej["toggle_text"]
    except (KeyError, TypeError, ValueError) as e:
        raise GuiDirError(f"{js.get('id', '?')}: bad JSON ({e})") from e
    scr.rebuild_strs(texts)
    return scr.write()


def import_gui(data, indir, warnings=None, report=None):
    """Apply an export directory to Intwrap.zal ``data``; returns the new archive bytes."""
    warnings = [] if warnings is None else warnings
    base = Path(indir)
    index = _read_json(base / "_gui.json")
    if index.get("format") != GUI_FORMAT:
        raise GuiDirError(f"{base / '_gui.json'}: not a {GUI_FORMAT} index")
    iw = Intwrap(data)
    have = iw.assets()
    for key, rec in index["assets"].items():
        aid = int(key, 16)
        if aid not in have:
            raise GuiDirError(f"asset {key} is not in this Intwrap.zal")
        orig = have[aid][0]
        path = base / rec["json"]
        js = _read_json(path)
        kind = rec["type"]
        if kind in ("picture", "sprite", "sprite2"):
            new = _load_image(js, path.parent, orig, warnings).write()
        elif kind == "font":
            new = _load_font(js, path.parent, orig, warnings).write()
        elif kind == "screen":
            new = _load_screen(js, orig)
        else:
            raise GuiDirError(f"asset {key}: unknown type {kind}")
        if new != orig:
            groups = iw.set_asset(aid, new)
            if report is not None:
                report.append((aid, kind, len(orig), len(new), groups))
    out = iw.write()
    # the result must parse again, asset by asset
    chk = Intwrap(out)
    for aid, (d, _) in chk.assets().items():
        kind = scrn.asset_type(aid)
        if kind == "screen":
            scrn.read_screen(d)
        elif kind == "font":
            zprim.read_font(d)
        elif kind in ("picture", "sprite", "sprite2"):
            zprim.read_image(d)
    return out


# ------------------------------------------------------------------ texts (MiscData.ZAL STRS)

def strs_tables(misc):
    return {e.id: e for e in misc.entries if e.data()[:4] == b"STRS"}


def export_texts(misc_data, path, tables=None):
    misc = zal.ZalArchive.read(misc_data)
    out = {"format": TEXT_FORMAT, "source_sha256": _sha(misc_data), "tables": {}}
    for tid, e in sorted(strs_tables(misc).items()):
        if tables and tid not in tables:
            continue
        t = strs.read(e.data())
        out["tables"][_hx(tid)] = {"base": _hx(t.base_id),
                                   "strings": {_hx(i, 4): t.get(i) for i in t.ids()}}
    _write_json(Path(path), out)
    return len(out["tables"])


def check_table(tid, data):
    if tid in strs.FRONTEND_TABLES and len(data) > strs.FRONTEND_MAX:
        raise GuiDirError(f"table {_hx(tid)} would be {len(data)} bytes; the front end loads at most "
                          f"{strs.FRONTEND_MAX} ('Macro data too big or too small')")


def set_texts(misc_data, edits, report=None):
    """``edits`` = {table id: {text id: string}} -> new MiscData.ZAL bytes."""
    misc = zal.ZalArchive.read(misc_data)
    tabs = strs_tables(misc)
    for tid, strings in edits.items():
        if tid not in tabs:
            raise GuiDirError(f"no STRS table {_hx(tid)}")
        t = strs.read(tabs[tid].data())
        n = 0
        for text_id, s in strings.items():
            try:
                n += t.set(text_id, s)
            except KeyError as e:
                raise GuiDirError(f"table {_hx(tid)}: {e.args[0]}") from e
        if n:
            new = t.write()
            check_table(tid, new)
            misc.set_data(tid, new)
            if report is not None:
                report.append((tid, n, len(new)))
    return misc.write()


def import_texts(misc_data, path, report=None):
    js = _read_json(path)
    if js.get("format") != TEXT_FORMAT:
        raise GuiDirError(f"{path}: not a {TEXT_FORMAT} file")
    edits = {int(k, 16): {int(i, 16): s for i, s in v["strings"].items()} for k, v in js["tables"].items()}
    return set_texts(misc_data, edits, report)


# ------------------------------------------------------------------ HUD fonts

def hud_json(fid, font, png):
    name, image, res = hudfont.FONTS[fid]
    return {"format": HUD_FORMAT, "id": _hx(fid), "name": name, "image": _hx(image), "resource": _hx(res),
            "png": png, "height": font.height, "header": bytes(font.header).hex(),
            "glyphs": [{"char": chr(g.char), "u": g.u, "v": g.v, "w": g.w,
                        **({"pads": g.pads.hex()} if g.pads.strip(b"\0") else {})} for g in font.glyphs]}


def export_hud(misc_data, mat_data, outdir):
    misc = zal.ZalArchive.read(misc_data)
    mat = zal.ZalArchive.read(mat_data)
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    for fid, (name, image, _) in hudfont.FONTS.items():
        font = hudfont.read(misc.find(fid).data())
        png = f"{name}.png"
        (out / png).write_bytes(imaging.bmp_to_png(mat.find(image).data()))
        _write_json(out / f"{name}.json", hud_json(fid, font, png))
    return len(hudfont.FONTS)


def sheet_from_png(bmp, png, warnings=None):
    """A Material.ZAL BMP with its pixels (and palette) from ``png``: a palette PNG of the same depth
    keeps its indices and its colours replace the BMP's; any other PNG is mapped to the BMP palette."""
    info = imaging.bmp_info(bmp)
    img = imaging.png_decode(png)
    if (img["width"], img["height"]) != (info.width, info.height):
        raise GuiDirError(f"sheet is {img['width']}x{img['height']}, the image is {info.width}x{info.height} "
                          "(the glyph positions refer to it)")
    head = bytearray(info.header)
    if img["color_type"] == 3 and img["bit_depth"] == info.bpp:
        hs = int.from_bytes(head[14:18], "little")
        for i, rgb in enumerate(img["palette"][:len(info.palette)]):
            p = 14 + hs + 4 * i
            head[p:p + 3] = bytes((rgb[2], rgb[1], rgb[0]))
        return imaging.png_to_bmp(png, bytes(head), info.tail)
    if info.bpp > 8:
        return imaging.png_to_bmp(png, bytes(head), info.tail)
    rows = imaging.quantize_rows(imaging.png_to_rgba_rows(img), info.width, info.palette, info.bpp)
    if warnings is not None:
        warnings.append("sheet is not a palette PNG of the image's depth: colours mapped to the nearest palette entry")
    tmp = imaging.png_encode(info.width, info.height, rows, color_type=3, bit_depth=info.bpp,
                             palette=list(info.palette) + [(0, 0, 0)] * ((1 << info.bpp) - len(info.palette)))
    return imaging.png_to_bmp(tmp, bytes(head), info.tail)


def import_hud(misc_data, mat_data, indir, warnings=None, report=None):
    """-> (new MiscData.ZAL bytes or None, new Material.ZAL bytes or None) (None = unchanged)."""
    warnings = [] if warnings is None else warnings
    misc = zal.ZalArchive.read(misc_data)
    mat = zal.ZalArchive.read(mat_data)
    base = Path(indir)
    misc_changed = mat_changed = False
    for fid, (name, image, _) in hudfont.FONTS.items():
        path = base / f"{name}.json"
        if not path.is_file():
            continue
        js = _read_json(path)
        if js.get("format") != HUD_FORMAT:
            raise GuiDirError(f"{path}: not a {HUD_FORMAT} file")
        orig = misc.find(fid).data()
        try:
            glyphs = []
            for g in js["glyphs"]:
                ch = g["char"]
                c = ord(ch) if isinstance(ch, str) and len(ch) == 1 else int(ch)
                glyphs.append(hudfont.Glyph(c, int(g["u"]), int(g["v"]), int(g["w"]),
                                            bytes.fromhex(g.get("pads", "00" * 6))))
            font = hudfont.HudFont(glyphs, int(js["height"]), bytes.fromhex(js["header"]))
        except (KeyError, TypeError, ValueError) as e:
            raise GuiDirError(f"{path}: bad JSON ({e})") from e
        chars = [g.char for g in glyphs]
        if len(set(chars)) != len(chars):
            warnings.append(f"{name}: a character has two glyphs (the last one wins)")
        new = font.write()
        if new != orig:
            misc.set_data(fid, new)
            misc_changed = True
            if report is not None:
                report.append(("MiscData.ZAL", fid, len(orig), len(new)))
        bmp = mat.find(image).data()
        new_bmp = sheet_from_png(bmp, (base / js["png"]).read_bytes(), warnings)
        if new_bmp != bmp:
            mat.set_data(image, new_bmp)
            mat_changed = True
            if report is not None:
                report.append(("Material.ZAL", image, len(bmp), len(new_bmp)))
    return (misc.write() if misc_changed else None, mat.write() if mat_changed else None)

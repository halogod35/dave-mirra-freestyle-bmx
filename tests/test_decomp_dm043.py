"""DM-043 self-consistency: decomp of zScene.c, zscnpts.c, zpanorama.c, zprop.c, ztaginf.c, zrwWorld.c.

No C compiler on the build PC, so the C is checked two ways (as DM-041/DM-042 do):
1. structure: every function of the six files is drafted, its C definition follows its VA comment,
   the names agree with decomp/functions.csv and decomp/symbols/symbols.csv, no Ghidra artefacts remain, and the
   struct layouts in the new headers agree with their "+0x.." comments and the ghidra_types.txt sizes;
2. behaviour: line-for-line Python ports of the C below run over the whole level corpus and are
   compared with dmbmx.formats.level (the format reference): zrwWorldFindScfi on every ZSD, the
   zsceneLoadComponent dispatch with the zpropLoadDef / zpropLoad / zscnptsLoad / zpanoramaLoad /
   ztaginfLoad(+LoadNode) handlers on every component file, ztaginfFindLeaf on every tag face, the
   zpropLoad rotation chain (matrix -> euler -> quaternion -> matrix) on every prop, and
   zsceneLoadDatabase + the zsceneOpen lookup on Scene.Txt with the exe's level-name table.
"""
import csv
import math
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR, corpus_files
from dmbmx.core.hash import zhash
from dmbmx.formats import level

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src" / "NgEngine" / "Zge"
INC = REPO / "decomp" / "include"
FILES = {
    "NgEngine/Zge/zScene.c": 39, "NgEngine/Zge/ZgeMods/zscnpts.c": 11, "NgEngine/Zge/ZgeMods/zpanorama.c": 5,
    "NgEngine/Zge/ZgeMods/zprop.c": 8, "NgEngine/Zge/ZgeMods/ztaginf.c": 28,
    "NgEngine/Zge/Com/ZglRw/zrwWorld.c": 33,   # DM-059 added the two zfx affectors (4 functions)
}
HEADERS = ["zscene.h", "zprop.h", "ztaginf.h", "zscnpts.h", "zrwworld.h"]


def src(f):
    return (REPO / "decomp" / "src" / f).read_text(encoding="utf-8")


def c_functions():
    """{va: (file, status, name)} for every VA comment of FILES."""
    out = {}
    for f in FILES:
        for m in re.finditer(r"/\* VA 0x([0-9a-f]{8}), \d+ bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n",
                             src(f)):
            name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(3)).group(1)
            out[int(m.group(1), 16)] = (f, m.group(2), name)
    return out


# ------------------------------------------------------------------ structure
def test_all_functions_drafted_and_named_consistently():
    fns = c_functions()
    assert len(fns) == sum(FILES.values()) == 124
    rows = {int(r["va"], 16): r for r in csv.DictReader(open(REPO / "decomp" / "functions.csv", encoding="utf-8"))}
    in_files = {va for va, r in rows.items() if r["file"] in FILES}
    assert in_files == set(fns)
    for f, n in FILES.items():
        assert sum(1 for x in fns.values() if x[0] == f) == n, f
    sym = {}
    for r in csv.DictReader(open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8")):
        if r["kind"] == "func":
            sym.setdefault(int(r["address"], 16), []).append(r)
    for va, (f, status, name) in fns.items():
        r = rows[va]
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert r["name"] == name, (hex(va), r["name"], name)
        card = "DM-059" if 0x00497640 <= va < 0x00497780 else "DM-043"   # DM-059 added the zfx affectors
        hand = [s for s in sym[va] if s["card"] == card]
        assert [s["name"] for s in hand] == [name] and hand[0]["source_file"] == f, hex(va)
    names = {n for _f, _s, n in fns.values()}
    assert {"zsceneOpen", "zsceneLoadComponent", "zrwWorldFindScfi", "zrwWorldOpen", "zrwWorldHookRpMaterials",
            "ztaginfLoad", "zscnptsLoad", "zpropLoad", "zpanoramaLoad"} <= names


def test_no_ghidra_artefacts():
    # FUN_xxxxxxxx stays allowed for *calls* into unnamed functions of other modules (their current
    # names); none of this card's definitions may use one.
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|PTR_\w+|in_stack_\w+|unaff_\w+|"
                     r"[iu]Var\d+|p[A-Z]\w*Var\d+|[a-z]Stack_\w+|TODO\(decomp\))\b")
    for f in FILES:
        for n, line in enumerate(src(f).splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
    for _f, _s, name in c_functions().values():
        assert not name.startswith("FUN_") and not re.search(r"_[0-9a-f]{8}$", name), name


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4, "unsigned": 4, "ZsceneLoadFn": 4, "ZsceneChunkFn": 4, "ZiffHandlerFn": 4}


def layouts(text, known):
    """{struct: (size, align, [(field, offset, commented_offset)])}, x86 natural alignment."""
    text = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", text, flags=re.S)
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\n\} (\w+);", text, re.S):
        off, fields, salign = 0, [], 1
        for line in m.group(2).splitlines():
            body = re.sub(r"/\*.*?\*/", "", line).strip()
            if not body:
                continue
            comment = re.search(r"/\*\s*\+0x([0-9A-Fa-f]+)", line)
            fp = re.search(r"\(\s*(?:__cdecl\s*)?\*\s*(\w+)\s*\)\s*\(", body)
            if fp:
                name, size, align, count = fp.group(1), 4, 4, 1
            else:
                d = re.match(r"(?:const\s+)?((?:struct\s+|unsigned\s+)?\w+)\s*(\**)\s*(\w+)(?:\[(\w+)\])?;$", body)
                assert d, (m.group(3), line)
                typ, ptr, name, count = d.groups()
                typ = typ.replace("struct ", "").replace("unsigned ", "")
                if ptr:
                    size = align = 4
                elif typ in SIZES:
                    size = align = SIZES[typ]
                else:
                    size, align = known[typ][0], known[typ][1]
                count = int(count, 0) if count else 1
            off = (off + align - 1) // align * align
            fields.append((name, off, int(comment.group(1), 16) if comment else None))
            off += size * count
            salign = max(salign, align)
        known[m.group(3)] = ((off + salign - 1) // salign * salign, salign, fields)
    return known


def all_layouts():
    known = {}
    layouts((INC / "rwtypes.h").read_text(encoding="utf-8"), known)
    for h in ["zlist.h", "zfile.h", "ziff.h"] + HEADERS:
        layouts((INC / h).read_text(encoding="utf-8"), known)
    return known


def test_struct_layouts():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    known = all_layouts()
    new = set()
    for h in HEADERS:
        text = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", (INC / h).read_text(encoding="utf-8"),
                      flags=re.S)
        new |= set(re.findall(r"typedef struct (\w+) \{", text))
    new |= {"RwV3d", "RwMatrix"}
    assert len(new) >= 34      # (the zprop lists use DM-044's zList)
    for name in sorted(new):
        size, _align, fields = known[name]
        assert size == want[name], (name, hex(size), hex(want[name]))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))


def test_headers_listed_for_ghidra():
    listed = (INC / "ghidra_types.txt").read_text(encoding="utf-8")
    umbrella = (INC / "decomp.h").read_text(encoding="utf-8")
    for h in HEADERS:
        assert f"\n{h}\n" in listed and f'#include "{h}"' in umbrella, h


# ------------------------------------------------------------------ the IFF reader as the C uses it
class Iff:
    """ziff semantics needed here: a form's children, FindFirst (scan from the start) and FindNext
    (continue after the current child), ChunkReadData (sequential reads inside the current child)."""

    def __init__(self, data, start, end):
        self.d, self.kids = data, []
        o = start
        while o + 8 <= end:
            tag, size = data[o:o + 4], struct.unpack_from("<I", data, o + 4)[0]
            assert size % 4 == 0 and o + 8 + size <= end      # "Data length not word size"
            self.kids.append((tag, o + 8, size))
            o += 8 + size
        self.cur, self.pos = -1, 0

    def find_first(self, tag):
        self.cur = -1
        return self.find_next(tag)

    def find_next(self, tag):
        for i in range(self.cur + 1, len(self.kids)):
            if self.kids[i][0] == tag:
                self.cur, self.pos = i, self.kids[i][1]
                return True
        self.cur = len(self.kids)
        return False

    def read(self, n):
        tag, off, size = self.kids[self.cur]
        n = min(n, off + size - self.pos)                   # ziffChunkReadData clamps
        b = self.d[self.pos:self.pos + n]
        self.pos += n
        return b

    def size(self):
        return self.kids[self.cur][2]

    def open(self):
        tag, off, size = self.kids[self.cur]
        return Iff(self.d, off, off + size)


def c_find_scfi(zsd, typ):
    """Port of zrwWorldFindScfi: (size, name) of the first SCFI of the type, or None."""
    scen = Iff(zsd, 8, 8 + struct.unpack_from("<I", zsd, 4)[0])
    ok = scen.find_first(b"SCFI")
    while ok:
        assert scen.size() == 0x28                          # "Chunk out of sync"
        rec = scen.read(0x28)
        if rec[0] == typ:
            return struct.unpack_from("<I", rec, 4)[0], rec[8:40].split(b"\0")[0].decode("latin-1")
        ok = scen.find_next(b"SCFI")
    return None


def read_obji(form):
    if not form.find_first(b"OBJI"):
        return None
    _hdr, n = struct.unpack("<II", form.read(8))
    return form.read(n)


class CScene:
    """Ports of the component handlers; state as in the exe's globals."""

    def __init__(self):
        self.defs, self.props, self.points, self.panorama = [], [], [], False
        self.faces, self.tag_data, self.root, self.extra_nodes = [], b"", None, False

    def prop_load_def(self, form):                           # zpropLoadDef 0x00486A20
        assert form.find_first(b"PDIN")
        rec = form.read(0x38)
        size, ident = struct.unpack_from("<II", rec)
        assert size == 0x38
        name = rec[8:28].split(b"\0")[0]                   # strcpy
        extents = struct.unpack_from("<3f", rec, 0x1C)
        extra = rec[0x28:0x38]
        assert form.find_first(b"MODL")
        self.defs.append(dict(id=ident, name=name, extents=extents, extra=extra, obji=read_obji(form)))
        return True

    def prop_load(self, form):                               # zpropLoad 0x00486BE0
        assert form.find_first(b"PRBL")
        rec = form.read(0x48)
        size, def_id = struct.unpack_from("<II", rec)
        assert size == 0x48
        d = next(x for x in self.defs if x["id"] == def_id)   # "No prop def corresponds..."
        m = struct.unpack_from("<9f", rec, 0x18)
        pos = [v * 100.0 for v in struct.unpack_from("<3f", rec, 0x3C)]
        self.props.append(dict(def_id=def_id, name=rec[8:24], matrix=m, pos=pos, model=d["name"],
                               obji=read_obji(form)))
        return True

    def scnpts_load(self, form):                             # zscnptsLoad 0x00491F50
        assert form.find_first(b"SDPT")
        rec = form.read(0x4C)
        size, ident, typ = struct.unpack_from("<III", rec)
        assert size == 0x4C
        m = struct.unpack_from("<9f", rec, 0x1C)
        right, up, at = [-v for v in m[0:3]], list(m[3:6]), [-v for v in m[6:9]]
        pos = [v * 100.0 for v in struct.unpack_from("<3f", rec, 0x40)]
        self.points.insert(0, dict(id=ident, type=typ, right=right, up=up, at=at, pos=pos, obji=read_obji(form)))
        return True

    def panorama_load(self, form):                           # zpanoramaLoad 0x004A10A0
        if not form.find_first(b"MODL"):
            return False
        self.panorama = True
        return True

    def taginf_load(self, form):                             # ztaginfLoad 0x0048DAE0
        assert form.find_first(b"TAGD")
        hdr, count = struct.unpack("<II", form.read(8))
        assert hdr == 8
        if count:
            if form.find_first(b"TAGI"):
                form.read(8)
                n = form.size() - 8
                self.tag_data = form.read(n) if n else b""
            ok = form.find_first(b"TAGF")
            while ok:
                rec = form.read(0x50)
                assert struct.unpack_from("<I", rec)[0] == 0x50
                v = struct.unpack_from("<12f", rec, 4)
                tags = struct.unpack_from("<7i", rec, 0x34)
                self.faces.append(dict(v=[v[0:3], v[3:6], v[6:9]], normal=v[9:12],
                                       face=tags[0], edges=tags[1:4], verts=tags[4:7]))
                ok = form.find_next(b"TAGF")
            assert len(self.faces) == count                 # (the exe would overrun otherwise)
        if form.find_first(b"TAGN"):
            self.root = self.load_node(form)
            self.extra_nodes = form.find_next(b"TAGN")      # "Extra nodes in tree???"
        return True

    def load_node(self, form):                               # ztaginfLoadNode 0x0048D9A0
        rec = form.read(0x28)
        kind = rec[4]
        split, = struct.unpack_from("<f", rec, 8)
        a, b = struct.unpack_from("<3f", rec, 0x0C), struct.unpack_from("<3f", rec, 0x18)
        count = struct.unpack_from("<I", rec, 0x24)[0] & 0xFFFF
        faces = list(struct.unpack(f"<{count}I", form.read(4 * count))) if count else []
        node = dict(kind=kind, split=split, a=a, b=b, faces=faces, low=None, high=None)
        if kind != 1:
            form.find_next(b"TAGN")
            node["low"] = self.load_node(form)
            form.find_next(b"TAGN")
            node["high"] = self.load_node(form)
        return node


def c_load_component(scene, zsd, directory, typ, handlers):
    """Port of zsceneLoadComponent: SCFI -> exact-size load -> headerless IFF -> per-handler walk."""
    hit = c_find_scfi(zsd, typ)
    if hit is None:
        return 0
    size, name = hit
    if size == 0:
        return 1
    path = next(p for p in directory.iterdir() if p.name.lower() == name.lower())
    data = path.read_bytes()
    assert len(data) >= size                                 # zfileLoad: shorter -> NULL
    data = data[:size]
    root = Iff(data, 0, size)
    for tag, fn in handlers:
        ok = root.find_first(tag)
        while ok:
            if not fn(root.open()):
                return 0
            ok = root.find_next(tag)
    return 1


def level_dirs():
    return sorted({p.parent for p in corpus_files(".zsd") if p.parent.name.lower() != "audio"})


LEVELS = [pytest.param(d, id=d.name) for d in level_dirs()] or [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))]


def load_level(d):
    zsd_path = next(p for p in d.iterdir() if p.suffix.lower() == ".zsd")
    zsd = zsd_path.read_bytes()
    s = CScene()
    # the order of zsceneOpen: Tag, Scene (SCPT, SDPA), PropDef, Prop
    assert c_load_component(s, zsd, d, 3, [(b"TAGB", s.taginf_load)]) == 1
    assert c_load_component(s, zsd, d, 6, [(b"SCPT", s.scnpts_load), (b"SDPA", s.panorama_load)]) == 1
    assert c_load_component(s, zsd, d, 2, [(b"PDBL", s.prop_load_def)]) == 1
    assert c_load_component(s, zsd, d, 5, [(b"PRBL", s.prop_load)]) == 1
    return zsd, s, level.Level.read_dir(d)


def test_level_corpus_count():
    if not GAME_DIR.exists():
        pytest.skip("no corpus")
    assert len(level_dirs()) == 12


@pytest.mark.parametrize("d", LEVELS)
def test_find_scfi_port_agrees_with_zsd(d):
    zsd_path = next(p for p in d.iterdir() if p.suffix.lower() == ".zsd")
    zsd = zsd_path.read_bytes()
    ref = level.Zsd.read(zsd)
    for t in range(1, 7):
        e = ref.entry(t)
        assert c_find_scfi(zsd, t) == (e.size, e.name)
        f = next(p for p in d.iterdir() if p.name.lower() == e.name.lower())
        assert f.stat().st_size == e.size
    assert c_find_scfi(zsd, 7) is None


@pytest.mark.parametrize("d", LEVELS)
def test_component_handler_ports_agree_with_level_py(d):
    _zsd, s, lv = load_level(d)
    comp = {type(c).__name__: c for c in lv.components.values()}
    # PropDef._sd
    ref_defs = comp["PropDefSd"].defs
    assert [x["id"] for x in s.defs] == [p.id for p in ref_defs]
    for x, p in zip(s.defs, ref_defs):
        assert x["name"].decode("latin-1") == p.name and x["extents"] == tuple(p.extents)
        assert x["extra"] == p.junk                          # the 16 "junk" bytes = ZpropDefInfo.extra
        assert (x["obji"] is None) == (p.obji is None)
        if p.obji is not None:
            assert x["obji"] == p.obji.data
            assert any(x["extents"])                         # extents only with OBJI ...
        else:
            assert not any(x["extents"])                     # ... and never without (LEVEL.md 5.1)
    # Prop._sd
    ref_props = comp["PropSd"].props
    assert len(s.props) == len(ref_props)
    for x, p in zip(s.props, ref_props):
        assert x["def_id"] == p.def_id and x["matrix"] == tuple(p.matrix)
        assert x["pos"] == pytest.approx([v * 100.0 for v in p.pos])
        assert (x["obji"] is None) == (p.obji is None)
    # Scene._sd: the list is newest first
    ref_pts = comp["SceneSd"].points
    assert [(x["id"], x["type"]) for x in reversed(s.points)] == [(p.id, p.type) for p in ref_pts]
    for x, p in zip(reversed(s.points), ref_pts):
        m = p.matrix
        assert x["right"] == [-m[0], -m[1], -m[2]] and x["up"] == list(m[3:6]) and x["at"] == [-m[6], -m[7], -m[8]]
        assert x["pos"] == pytest.approx([v * 100.0 for v in p.pos])
    assert s.panorama == (comp["SceneSd"].panorama is not None)
    # Tag._sd
    tags = comp["TagSd"]
    assert len(s.faces) == len(tags.faces) and not s.extra_nodes
    for x, f in zip(s.faces, tags.faces):
        assert [tuple(v) for v in x["v"]] == [tuple(v) for v in f.verts]
        assert (x["face"] != -1) == (f.tag is not None)
        if f.tag is not None:                                 # the pointer into TAGI reads the record
            assert struct.unpack_from("<H", s.tag_data, x["face"])[0] == f.tag.code
        for off, et in zip(x["edges"], f.edge_tags):
            assert (off != -1) == (et is not None)
            if et is not None:
                assert struct.unpack_from("<HB", s.tag_data, off) == (et.code, et.kind)
    n_ref = sum(1 for _ in tags.root.walk())
    n_c, stack = 0, [s.root]
    while stack:
        n = stack.pop()
        n_c += 1
        if n["kind"] != 1:
            stack += [n["high"], n["low"]]
    assert n_c == n_ref


def c_find_leaf(root, p):
    """Port of ztaginfFindLeaf 0x0048E400."""
    n = root
    while n["kind"] != 1:
        if n["kind"] == 2:
            n = n["low"] if p[0] < n["split"] else n["high"]
        elif n["kind"] == 4:
            n = n["low"] if n["split"] < p[1] else n["high"]
        elif n["kind"] == 8:
            n = n["low"] if p[2] < n["split"] else n["high"]
        else:
            raise AssertionError("unknown node kind loops forever in the exe")
    return n


@pytest.mark.parametrize("d", LEVELS)
def test_find_leaf_port_finds_each_face(d):
    """Every face is listed in the leaf that ztaginfFindLeaf picks for its centroid."""
    _zsd, s, _lv = load_level(d)
    missing = 0
    for i, f in enumerate(s.faces):
        c = [sum(v[k] for v in f["v"]) / 3.0 for k in range(3)]
        if i not in c_find_leaf(s.root, c)["faces"]:
            missing += 1
    assert missing == 0, f"{missing} of {len(s.faces)} faces not in their centroid's leaf"


# ------------------------------------------------------------------ the prop rotation chain
TWO_PI = 2.0 * math.pi


def c_atan2_angle(y, x):
    """zmath_00480FF0: atan2 as a 16-bit angle (65536 = 360 degrees) [I: table-based in the exe]."""
    return int(round(math.atan2(y, x) * 65536.0 / TWO_PI)) & 0xFFFF


def s16(v):
    return v - 0x10000 if v >= 0x8000 else v


def c_matrix_to_euler(m):
    """Port of zmath_00481FC0 (m = row-major 4x4 as 16 floats)."""
    r = math.sqrt(m[1] * m[1] + m[0] * m[0])
    if r > 0.001:
        return [s16(c_atan2_angle(m[6], m[10])), s16(c_atan2_angle(-m[2], r)), s16(c_atan2_angle(m[1], m[0]))]
    return [s16(c_atan2_angle(-m[9], m[5])), s16(c_atan2_angle(-m[2], r)), 0]


def c_quat_from_euler(e):
    """Port of zquat_FromEuler 0x004832B0: half angles, q = qz * qy * qx."""
    h = [int(a * 9.58738019107841e-05 * 0.5 * 10430.3779296875) for a in e]   # __ftol (truncate)
    c = [math.cos(a * TWO_PI / 65536.0) for a in h]
    s = [math.sin(a * TWO_PI / 65536.0) for a in h]
    c0, c1, c2 = c
    s0, s1, s2 = s
    return [s0 * c2 * c1 - s2 * c0 * s1, s2 * s0 * c1 + c2 * c0 * s1,
            s2 * c0 * c1 - s0 * c2 * s1, c2 * c0 * c1 + s2 * s0 * s1]


def c_quat_to_matrix(q):
    """Port of zquat_ToMatrix 0x00483470 (3x3 part, row-major)."""
    n = math.sqrt(sum(v * v for v in q))
    x, y, z, w = (v / n for v in q)
    k = 2.0 / n if n > 0 else 0.0
    return [1 - (z * z * k + y * y * k), x * y * k + w * z * k, x * z * k - w * y * k,
            x * y * k - w * z * k, 1 - (z * z * k + x * x * k), y * z * k + w * x * k,
            x * z * k + w * y * k, y * z * k - w * x * k, 1 - (y * y * k + x * x * k)]


def test_rotation_ports_round_trip_synthetic():
    for e in ([0, 0, 0], [1000, -2000, 3000], [-16000, 5000, 12345], [8192, 0, -8192]):
        m3 = c_quat_to_matrix(c_quat_from_euler(e))
        m = [m3[0], m3[1], m3[2], 0, m3[3], m3[4], m3[5], 0, m3[6], m3[7], m3[8], 0, 0, 0, 0, 1]
        e2 = c_matrix_to_euler(m)
        assert all(abs(a - b) <= 2 for a, b in zip(e, e2)), (e, e2)


def test_prop_rotation_chain_is_identity():
    """LEVEL.md open question 6: zpropLoad copies the stored rows into a 4x4 *without* transposing,
    and matrix -> euler (0x00481FC0) -> quaternion (zquat_FromEuler) -> zquat_ToMatrix gives the
    stored rows back (to the 16-bit angle quantisation), not their transpose."""
    dirs = level_dirs()
    if not dirs:
        pytest.skip("no corpus")
    n = asym = 0
    for d in dirs:
        data = next(p for p in d.iterdir() if p.name.lower() == "prop._sd").read_bytes()
        for p in level.PropSd.read(data).props:
            m9 = p.matrix
            m = [m9[0], m9[1], m9[2], 0, m9[3], m9[4], m9[5], 0, m9[6], m9[7], m9[8], 0, 0, 0, 0, 1]
            r = c_quat_to_matrix(c_quat_from_euler(c_matrix_to_euler(m)))
            assert max(abs(a - b) for a, b in zip(r, m9)) < 2e-3, (d.name, m9, r)
            t = [m9[0], m9[3], m9[6], m9[1], m9[4], m9[7], m9[2], m9[5], m9[8]]
            if max(abs(a - b) for a, b in zip(t, m9)) > 0.1:
                asym += 1
                assert max(abs(a - b) for a, b in zip(r, t)) > 0.05
            n += 1
    assert n == 927 and asym > 100


# ------------------------------------------------------------------ Scene.Txt
def c_tokens(text):
    """Stand-in for the ztoken parser (not decompiled): // comments, "quoted" strings,
    #define NAME VALUE and $NAME substitution."""
    defines, out = {}, []
    toks = re.findall(r'"[^"]*"|\S+', re.sub(r"//[^\n]*", "", text))
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "#define":
            defines[toks[i + 1]] = toks[i + 2]
            i += 3
            continue
        if t.startswith("$"):
            t = defines[t[1:]]
        out.append(t.strip('"'))
        i += 1
    return out


class CDatabase:
    """Ports of zsceneParseObject / zsceneLoadDatabase / zsceneFindDefRecurse."""

    def __init__(self, tokens):
        self.t, self.i = tokens, 0
        self.common, self.scenes = self.new(0), None

    @staticmethod
    def new(kind):
        return dict(name="", file="", kind=kind, id=0, type=0, subtype=0, next=None, child=None)

    def next_tok(self):
        if self.i >= len(self.t):
            return None
        self.i += 1
        return self.t[self.i - 1]

    def parse_object(self, d):
        if self.next_tok() != "{":
            return False
        while True:
            tok = self.next_tok()
            if tok is None:
                return False
            if tok == "NAME":
                d["name"] = self.next_tok()
                assert len(d["name"]) <= 31
                d["id"] = zhash(d["name"])
            elif tok == "FILE":
                d["file"] = self.next_tok()
                assert len(d["file"]) <= 63
            elif tok in ("TYPE", "SUBTYPE"):
                d[tok.lower()] = int(self.next_tok(), 0)
            elif tok in ("ANIM", "MODEL"):
                c = self.new(2 if tok == "ANIM" else 1)
                c["next"], d["child"] = d["child"], c
                self.parse_object(c)
            elif tok == "}":
                return True
            else:
                raise AssertionError(f"**ERROR** Unknown token:'{tok}'")

    def load(self):
        assert self.next_tok() == "SCENE_COMMON" and self.parse_object(self.common)
        tok = self.next_tok()
        while tok == "SCENE":
            d = self.new(0)
            if self.scenes is None:
                self.scenes = d
            else:
                d["next"], self.scenes["next"] = self.scenes["next"], d
            assert self.parse_object(d)
            tok = self.next_tok()
        return tok == "END"

    def find(self, d, key):
        if d["kind"] == key["kind"]:
            if key["id"] == 0:
                key["id"], key["found"] = 0xFFFFFFFF, d
                return None
            if d["id"] == key["id"]:
                key["found"] = d
                return None
        if d["child"] and self.find(d["child"], key) is None:
            return None
        if d["next"] and self.find(d["next"], key) is None:
            return None
        return d

    def open_scene(self, name):
        """the def search of zsceneOpen"""
        key = dict(kind=0, id=zhash(name), found=None)
        s = self.scenes
        if s["kind"] == 0 and s["id"] == key["id"]:
            return s
        if not s["child"] or self.find(s["child"], key) is not None:
            if s["next"]:
                self.find(s["next"], key)
        return key["found"]


def test_scene_txt_database_port_and_level_table():
    scene_txt = GAME_DIR / "Scene.Txt"
    exe = GAME_DIR / "NgBMX.exe"
    if not scene_txt.exists() or not exe.exists():
        pytest.skip("no game install")
    db = CDatabase(c_tokens(scene_txt.read_text(encoding="latin-1")))
    assert db.load()
    models = []
    c = db.common["child"]
    while c:
        models.append(c)
        c = c["next"]
    assert any(m["kind"] == 2 and m["file"].lower() == "assets\\motion.zal" for m in models)
    # the 14 level names the game passes to zsceneOpen (table 0x0055BC44, indexed by g_currentLevel)
    img = exe.read_bytes()
    import pefile                                          # in the venv (DM-002)
    pe = pefile.PE(data=img, fast_load=True)
    rd = lambda va, n: pe.get_data(va - 0x400000, n)
    names = [rd(p, 40).split(b"\0")[0].decode() for p in struct.unpack("<14I", rd(0x0055BC44, 56))]
    assert names[0] == "Greenville Dirt" and names[11] == "Acclaim Vert"
    files = set()
    for n in names:
        s = db.open_scene(n)
        assert s is not None and s["name"] == n, n
        f = GAME_DIR / s["file"].replace("\\", "/")
        hit = [p for p in f.parent.iterdir() if p.name.lower() == f.name.lower()]
        assert hit, s["file"]
        files.add(hit[0])
    assert len(files) == 12                                 # "Test A"/"Test B" reuse GreenVilleVert
    assert db.open_scene("No such scene") is None

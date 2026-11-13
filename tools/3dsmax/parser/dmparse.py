#!/usr/bin/env python3
"""dmparse: a simple, single-file reference parser for Dave Mirra Freestyle BMX (PC, 2000) files.

Stdlib only, independent of the `dmbmx` library. The format specification is ../SPEC.md; section
numbers in comments (e.g. "SPEC 3.2") point there. The MaxScript importer
(../maxscript/DaveMirraImport.ms) mirrors the ZMD/MATI functions here line by line.

    python dmparse.py tree  <file>          chunk tree of any supported file
    python dmparse.py dump  <file> [--full] decoded fields as JSON (big arrays summarised unless --full)
    python dmparse.py textures <file> <dir> write embedded TBMP/TPNG images (.bmp/.png)

Library use (all return plain dicts / lists; byte blobs are `bytes`):
    read_zmd(data)        models: riders, bikes, shadows, sfx            (SPEC 3)
    read_mati(data)       one MATI chunk (texture + material def)        (SPEC 4)
    read_mat_sd(data)     a level's Mat._sd (MATI sequence)              (SPEC 4.3)
    read_zsd(data)        a level's .ZSD include file (SCEN/SCDF/SCFI)   (SPEC 5)
    read_zsd_level(dir)   a level folder: ZSD + Mat._sd + component list (SPEC 5)
    read_level(dir)       a whole level: world, props, prop defs, scene points, panorama, tags (SPEC 5)
    read_bsp / read_propdef_sd / read_prop_sd / read_scene_sd / read_tag_sd   one level file each
    read_zal(data)        a ZAL archive, assets LZO1X-decoded            (SPEC 6)
    read_form(data)       an IFF FORM (PLHD/PLYR player table, ZMHD/ZMAP level table)  (SPEC 7)
    read_plyr(data)       the player table (miscdata.zal id 1, or the whole miscdata.zal) (SPEC 7.1)
    read_strs(data)       a STRS string table                            (SPEC 7.3)
    read_motion_bank(d)   motion.zal -> clips; sample_motion(clip) -> per-frame poses   (SPEC 8)
    read_zsnd(data)       a sound bank header + tables                   (SPEC 9)
    read_save(data)       SAVE.DAT                                       (SPEC 10)
Helpers: zhash(name), skeleton(zmd, lod), model_meshes(zmd), prop_instances(level), zge_to_rw /
pos_to_rw / matrix_to_rw / prop_matrix_to_rw (level coordinates), color_key_png (the game's magenta key), quat helpers.

All integers little-endian. Parsers raise ParseError on malformed input (bounds-checked).
"""
import json
import math
import os
import struct
import sys

__version__ = "1.0"


class ParseError(ValueError):
    pass


# ============================================================================ byte reader

class Buf:
    """Bounds-checked little-endian reader over ``data[start:end]``."""

    def __init__(self, data, pos=0, end=None):
        self.d, self.p = data, pos
        self.end = len(data) if end is None else end

    def need(self, n, what="data"):
        if n < 0 or self.p + n > self.end:
            raise ParseError(f"{what}: need {n} bytes at 0x{self.p:x}, only {self.end - self.p} left")

    def take(self, fmt, what="value"):
        n = struct.calcsize("<" + fmt)
        self.need(n, what)
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += n
        return v

    def u8(self):
        return self.take("B")[0]

    def u16(self):
        return self.take("H")[0]

    def u32(self):
        return self.take("I")[0]

    def i32(self):
        return self.take("i")[0]

    def f32(self):
        return self.take("f")[0]

    def raw(self, n, what="bytes"):
        self.need(n, what)
        b = bytes(self.d[self.p:self.p + n])
        self.p += n
        return b

    def left(self):
        return self.end - self.p


def cstr(b):
    """NUL-terminated string (anything after the NUL is padding/garbage)."""
    return bytes(b).split(b"\0", 1)[0].decode("latin-1")


# ============================================================================ name hash (SPEC 2.4)

def zhash(name):
    """Z-Axis name hash (zHashName 0x004828C0): upper-cased signed chars c[1..n],
    h = sum(i * c[i]) + (c[n] << 16), mod 2**32."""
    if isinstance(name, str):
        name = name.encode("latin-1")
    h = 0
    last = 0
    for i, c in enumerate(name.upper(), 1):
        c = c - 256 if c >= 128 else c
        h += i * c
        last = c
    return (h + (last << 16)) & 0xFFFFFFFF


KNOWN_NAMES = ("Skin", "Hips", "_Abdomen", "Chest", "Chestx", "Shoulders", "_LeftShoulder", "_RightShoulder",
               "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm", "RightHand",
               "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot",
               "Hipsdeck", "BackWheel", "FrontWheel", "Sprocket", "Handlebars", "SeatGrip", "LeftGrip",
               "RightGrip", "LeftPedal", "RightPedal", "SFXCube", "Cone01", "wheel")
_KNOWN = {}
for _n in KNOWN_NAMES:
    _KNOWN.setdefault(zhash(_n), _n)


# ============================================================================ chunk layers (SPEC 2)

ZAXIS_CONTAINERS = {b"MODL", b"MLOD", b"MATI", b"FORM", b"SCEN", b"TAGB", b"PDBL", b"SCPT", b"SDPA"}
RW_NAMES = {0x01: "Struct", 0x02: "String", 0x03: "Extension", 0x06: "Texture", 0x07: "Material",
            0x08: "MaterialList", 0x09: "AtomicSector", 0x0A: "PlaneSector", 0x0B: "World", 0x0E: "FrameList",
            0x0F: "Geometry", 0x10: "Clump", 0x14: "Atomic", 0x105: "Morph", 0x108: "Animation",
            0x10E: "Bone", 0x50E: "BinMesh"}
RW_CONTAINERS = {0x03, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0E, 0x0F, 0x10, 0x14}


def parse_4cc(data, start=0, end=None, depth=0):
    """Z-Axis chunks: char tag[4]; u32 size (excludes the 8-byte header); payload. Returns
    [{'tag', 'off', 'size', 'children'?}] (children for container tags)."""
    end = len(data) if end is None else end
    if depth > 16:
        raise ParseError("chunk nesting too deep")
    out, p = [], start
    while p < end:
        b = Buf(data, p, end)
        tag = b.raw(4, "chunk tag")
        size = b.u32()
        if size > end - b.p:
            raise ParseError(f"chunk {tag!r} at 0x{p:x}: size {size} runs past its parent")
        c = {"tag": tag.decode("latin-1"), "off": p, "size": size}
        if tag in ZAXIS_CONTAINERS or (tag == b"PRBL" and depth == 0):   # inner PRBL is the record
            c["children"] = parse_4cc(data, b.p, b.p + size, depth + 1)
        out.append(c)
        p = b.p + size
    return out


def parse_rw(data, start=0, end=None, depth=0):
    """RenderWare chunks: u32 type; u32 size; u32 version; payload (SPEC 2.2)."""
    end = len(data) if end is None else end
    if depth > 64:                                   # BSP sector trees nest deeper than models
        raise ParseError("RW nesting too deep")
    out, p = [], start
    while p < end:
        b = Buf(data, p, end)
        typ, size, ver = b.take("III", "RW header")
        if size > end - b.p:
            raise ParseError(f"RW chunk 0x{typ:x} at 0x{p:x}: size {size} runs past its parent")
        c = {"type": typ, "name": RW_NAMES.get(typ, f"0x{typ:X}"), "off": p, "size": size, "version": ver}
        if typ in RW_CONTAINERS:
            c["children"] = parse_rw(data, b.p, b.p + size, depth + 1)
        out.append(c)
        p = b.p + size
    return out


def body(data, c, rw=False):
    """Payload bytes of a chunk dict."""
    s = c["off"] + (12 if rw else 8)
    return data[s:s + c["size"]]


def kids(c, tag):
    return [k for k in c.get("children", []) if k.get("tag") == tag or k.get("type") == tag]


# ============================================================================ ZMD (SPEC 3)

GEO_TRISTRIP, GEO_POSITIONS, GEO_TEXTURED, GEO_PRELIT, GEO_NORMALS = 0x01, 0x02, 0x04, 0x08, 0x10
GEO_TEXTURED2 = 0x80


def read_zmd(data):
    """Decode a .zmd. Coordinates are raw RW model space (SPEC 2.5). Triangles are returned CCW as
    (a, b, c, slot) = file (v1, v0, v2, material)."""
    if not data:
        return {"type": "zmd", "empty": True, "lods": [], "mdef": None, "textures": []}
    if data[:4] != b"MODL":
        raise ParseError("not a ZMD (no MODL)")
    modl = parse_4cc(data)[0]
    z = {"type": "zmd", "empty": False, "lods": [], "mdef": None, "textures": []}
    for c in modl["children"]:
        if c["tag"] == "MLOD":
            z["lods"].append(_read_mlod(data, c))
        elif c["tag"] == "MDEF":
            z["mdef"] = _read_mdef(body(data, c))
        elif c["tag"] == "MATI":
            z["textures"].append(_read_mati_chunk(data, c))
    return z


def _read_mlod(data, c):
    mldf = kids(c, "MLDF")[0]
    mdff = kids(c, "MDFF")[0]
    flags, unk = struct.unpack("<II", body(data, mldf)[:8])
    s = mdff["off"] + 8
    clump = parse_rw(data, s, s + mdff["size"])[0]
    if clump["type"] != 0x10:
        raise ParseError("MDFF does not hold a Clump")
    lod = {"mldf": [flags, unk], "rw_version": clump["version"], "frames": [], "atomics": []}
    ch = clump["children"]
    lod["frames"] = _read_framelist(data, ch[1])
    for a in ch[2:]:
        if a["type"] == 0x14:
            lod["atomics"].append(_read_atomic(data, a))
    return lod


def _read_framelist(data, fl):
    st = fl["children"][0]
    b = Buf(data, st["off"] + 12, st["off"] + 12 + st["size"])
    n = b.u32()
    frames = []
    for i in range(n):
        v = b.take("12fiI", "frame")
        frames.append({"right": list(v[0:3]), "up": list(v[3:6]), "at": list(v[6:9]), "pos": list(v[9:12]),
                       "parent": v[12], "bone_id": None})
    for i, ext in enumerate(fl["children"][1:1 + n]):
        for e in ext.get("children", []):
            if e["type"] == 0x108 and e["size"] == 8:          # Animation plugin: u32 bone id, u32 0
                frames[i]["bone_id"] = struct.unpack("<I", body(data, e, True)[:4])[0]
    return frames


def _read_atomic(data, a):
    st, geo = a["children"][0], a["children"][1]
    frame, flags, _ = struct.unpack("<III", body(data, st, True))
    return {"frame": frame, "flags": flags, "geometry": _read_geometry(data, geo)}


def _read_geometry(data, geo):
    st, ml, ex = geo["children"][0], geo["children"][1], geo["children"][2]
    b = Buf(data, st["off"] + 12, st["off"] + 12 + st["size"])
    flags, nt, nv, nm = b.take("IIII", "geometry header")
    b.take("3f")                                                    # ambient, specular, diffuse
    g = {"flags": flags, "num_vertices": nv, "colors": None, "uvs": [], "positions": None, "normals": None}
    if flags & GEO_PRELIT:
        g["colors"] = [list(b.take("4B")) for _ in range(nv)]
    for _ in range(2 if flags & GEO_TEXTURED2 else (1 if flags & GEO_TEXTURED else 0)):
        g["uvs"].append([list(b.take("2f")) for _ in range(nv)])
    tris = []
    for _ in range(nt):
        v0, v1, mat, v2 = b.take("4H", "triangle")
        tris.append([v1, v0, v2, mat])                              # CCW (SPEC 3.4)
    g["triangles"] = tris
    if nm < 1:
        raise ParseError("geometry without morph target")
    sphere = list(b.take("4f"))
    has_v, has_n = b.take("II")
    if has_v:
        g["positions"] = [list(b.take("3f")) for _ in range(nv)]
    if has_n:
        g["normals"] = [list(b.take("3f")) for _ in range(nv)]
    g["sphere"] = sphere
    for _ in range(nm - 1):                                         # never in the corpus
        b.take("4f")
        hv, hn = b.take("II")
        b.raw(12 * nv * (hv + hn))
    for t in tris:
        if max(t[:3]) >= nv:
            raise ParseError("triangle index out of range")
    g["material_slots"] = _read_matlist(data, ml)
    g["bones"] = None
    for e in ex.get("children", []):
        if e["type"] == 0x10E:                                      # Bone plugin (SPEC 3.6)
            raw = body(data, e, True)
            g["bones"] = [list(struct.unpack_from("<III", raw, 12 * k)) for k in range(len(raw) // 12)]
    return g


def _read_matlist(data, ml):
    """MaterialList -> one {color, texture} per slot (-1 = next new material, k = reuse slot k) (SPEC 3.6)."""
    mst = ml["children"][0]
    mb = Buf(data, mst["off"] + 12, mst["off"] + 12 + mst["size"])
    n = mb.u32()
    idx = [mb.i32() for _ in range(n)]
    mats = [_read_material(data, m) for m in ml["children"][1:]]
    slots, it = [], iter(mats)
    for i in idx:
        if i < 0:
            slots.append(next(it))
        elif i < len(slots):
            slots.append(slots[i])
        else:
            raise ParseError("material list: bad reuse index")
    return slots


def _read_material(data, m):
    st = m["children"][0]
    b = Buf(data, st["off"] + 12, st["off"] + 12 + st["size"])
    b.u32()
    color = list(b.take("4B"))
    tex = None
    for k in m["children"][1:]:
        if k["type"] == 0x06:                                       # Texture: Struct, String name, String mask, Ext
            tex = cstr(body(data, k["children"][1], True))
    return {"color": color, "texture": tex}


def _read_mdef(raw):
    b = Buf(raw)
    kind = b.u32()
    name = cstr(b.raw(16))
    name_id = b.u32()
    params = list(b.take("4f"))
    count, unk = b.take("HH")
    entries = []
    for _ in range(count):
        n = cstr(b.raw(16))
        entries.append({"name": n, "id": b.u32()})
    return {"kind": kind, "name": name, "name_id": name_id, "params": params, "unk": unk, "entries": entries}


# ============================================================================ MATI (SPEC 4)

def _read_mati_chunk(data, c):
    madf = kids(c, "MADF")[0]
    img = [k for k in c["children"] if k["tag"] in ("TBMP", "TPNG", "TDDS")][0]
    raw = body(data, madf)
    if len(raw) < 72 or struct.unpack_from("<I", raw, 0)[0] != 0x48:
        raise ParseError("MADF: bad header")
    words = struct.unpack_from("<4I10f4I", raw, 0)
    image = bytes(body(data, img))
    kind = {"TBMP": "bmp", "TPNG": "png", "TDDS": "dds"}[img["tag"]]
    n = _image_length(image, kind)
    return {"name": cstr(raw[72:]), "name_id": words[17], "kind": kind, "data": image[:n],
            "pad": len(image) - n, "flags": words[1], "surface": words[14] & 0xF, "surface_word": words[14],
            "anim": [words[15], words[16]], "colors": [list(words[4:7]), list(words[7:10]), list(words[10:13])]}


def _image_length(d, kind):
    """Length of the image file inside its zero-padded chunk (SPEC 4.2)."""
    if kind == "bmp" and d[:2] == b"BM" and len(d) >= 6:
        n = struct.unpack_from("<I", d, 2)[0]
        if n <= len(d):
            return n
    if kind == "png":
        i = d.rfind(b"IEND")
        if i >= 4:
            return min(len(d), i + 8)
    return len(d)


def read_mati(data):
    """One MATI chunk (bytes starting with b'MATI')."""
    return _read_mati_chunk(data, parse_4cc(data)[0])


def read_mat_sd(data):
    """A level Mat._sd: a flat sequence of MATI chunks."""
    return [_read_mati_chunk(data, c) for c in parse_4cc(data) if c["tag"] == "MATI"]


# ============================================================================ level ZSD (SPEC 5)

LOAD_ORDER = {1: "Mat", 2: "PropDef", 3: "Tag", 4: "BSP", 5: "Prop", 6: "Scene"}


def read_zsd(data):
    """Level .ZSD include file: SCEN{ SCDF(16), SCFI(40) * N }. SCDF's size covers only its own 16
    bytes, so the SCFI blocks are its siblings; N comes from the SCEN size."""
    if data[:4] != b"SCEN":
        raise ParseError("not a level ZSD (no SCEN)")
    scen = parse_4cc(data)[0]
    files = []
    for c in scen["children"]:
        if c["tag"] == "SCFI":
            raw = body(data, c)
            if len(raw) < 9:
                raise ParseError("SCFI too short")
            files.append({"order": raw[0], "kind": LOAD_ORDER.get(raw[0], "?"), "junk": raw[1:4].hex(),
                          "size": struct.unpack_from("<I", raw, 4)[0], "name": cstr(raw[8:])})
    scdf = [c for c in scen["children"] if c["tag"] == "SCDF"]
    return {"type": "zsd", "scdf": body(data, scdf[0]).hex() if scdf else None, "files": files}


def read_zsd_level(folder):
    """A level folder (e.g. assets/AcclaimDirt): the .ZSD, its Mat._sd textures and the component
    list with real file sizes (read_level decodes everything)."""
    zsd = [f for f in os.listdir(folder) if f.lower().endswith(".zsd")]
    if not zsd:
        raise ParseError(f"no .ZSD in {folder}")
    z = read_zsd(open(os.path.join(folder, zsd[0]), "rb").read())
    z["zsd"] = zsd[0]
    for f in z["files"]:
        p = os.path.join(folder, f["name"])
        f["exists"] = os.path.isfile(p)
        f["actual_size"] = os.path.getsize(p) if f["exists"] else None
    mat = next((f for f in z["files"] if f["kind"] == "Mat" and f["exists"]), None)
    z["textures"] = read_mat_sd(open(os.path.join(folder, mat["name"]), "rb").read()) if mat else []
    return z


# ---------------------------------------------------------------------------- level coordinates (SPEC 2.6)

ZGE_SCALE = 100.0


def zge_to_rw(p, origin):
    """Engine (ZGE) units -> RW world: o + (x, -y, -z) / 100. Used by tag faces."""
    return (origin[0] + p[0] / ZGE_SCALE, origin[1] - p[1] / ZGE_SCALE, origin[2] - p[2] / ZGE_SCALE)


def pos_to_rw(p, origin):
    """Prop / scene-point position (stored as ZGE/100) -> RW world: o + (x, -y, -z)."""
    return (origin[0] + p[0], origin[1] - p[1], origin[2] - p[2])


def matrix_to_rw(m9):
    """Scene-point rotation rows (right, up, at; ZGE axes) -> RW rows: F M F with F = diag(1, -1, -1)
    (the rows taken as ZGE direction vectors). Props use prop_matrix_to_rw instead (SPEC 2.6)."""
    f = (1.0, -1.0, -1.0)
    return [tuple(m9[3 * r + c] * f[r] * f[c] for c in range(3)) for r in range(3)]


def prop_matrix_to_rw(m9):
    """Prop rotation rows (PRBL m[9]) -> RW rows: the stored rows unchanged (SPEC 2.6, DM-065). The game
    renders props through its mirrored quaternion converter, D (F M F) D with D = diag(-1, 1, 1) = M."""
    return [tuple(float(m9[3 * r + c]) for c in range(3)) for r in range(3)]


# ---------------------------------------------------------------------------- BSP._sd world (SPEC 5.4)

def read_bsp(data):
    """RW World (BSP._sd): origin, material slots and the sectors in depth-first (left first) order.
    Sector triangles are (slot, a, b, c), CCW; the 384-byte collision block is skipped."""
    root = parse_rw(data)
    if len(root) != 1 or root[0]["type"] != 0x0B:
        raise ParseError("not a RW World (0x0B)")
    w = root[0]
    ch = w["children"]
    b = Buf(data, ch[0]["off"] + 12, ch[0]["off"] + 12 + ch[0]["size"])
    v = b.take("I3f3f6I", "World struct")
    flags = v[12]
    out = {"type": "bsp", "inv_origin": list(v[1:4]), "origin": [-c for c in v[1:4]], "flags": flags,
           "counts": {"triangles": v[7], "vertices": v[8], "planes": v[9], "sectors": v[10]},
           "slots": _read_matlist(data, ch[1]), "sectors": []}
    stack = [(ch[2], 0)]
    while stack:
        c, depth = stack.pop()
        if depth > 64:
            raise ParseError("sector tree too deep")
        if c["type"] == 0x0A:                                   # PlaneSect: Struct, left, right
            if len(c["children"]) != 3:
                raise ParseError("PlaneSect needs 3 children")
            stack += [(c["children"][2], depth + 1), (c["children"][1], depth + 1)]
        elif c["type"] == 0x09:
            out["sectors"].append(_read_sector(data, c["children"][0], flags))
        else:
            raise ParseError(f"unexpected chunk 0x{c['type']:x} in the sector tree")
    return out


def _read_sector(data, st, flags):
    b = Buf(data, st["off"] + 12, st["off"] + 12 + st["size"])
    base, nt, nv = b.take("3I")
    inf, sup = list(b.take("3f")), list(b.take("3f"))
    b.take("2I")
    if nv > 256:
        raise ParseError("sector with more than 256 vertices")
    sec = {"inf": inf, "sup": sup, "vertices": [list(b.take("3f")) for _ in range(nv)],
           "normals": None, "colors": None, "uvs": None}
    if flags & 0x10:
        sec["normals"] = []
        for _ in range(nv):
            x, y, z, _pad = b.take("4b")
            sec["normals"].append([x / 127.0, y / 127.0, z / 127.0])
    if flags & 0x08:
        sec["colors"] = [list(b.take("4B")) for _ in range(nv)]
    if flags & (0x04 | 0x80):
        sec["uvs"] = [list(b.take("2f")) for _ in range(nv)]
    if flags & 0x80:
        b.raw(8 * nv)
    tris = []
    for _ in range(nt):
        m, x, y, z = b.take("4B")
        if max(x, y, z) >= nv:
            raise ParseError("sector triangle index out of range")
        tris.append([base + m, x, y, z])
    sec["triangles"] = tris
    return sec


# ---------------------------------------------------------------------------- PropDef / Prop / Scene (SPEC 5.5)

def _forms(data, tag):
    return [c for c in parse_4cc(data) if c["tag"] == tag]


def _obji(data, c):
    """OBJI: u32 8; u32 n; data[n] (+ pad to 4) -> the data bytes."""
    raw = body(data, c)
    if len(raw) < 8:
        raise ParseError("OBJI too short")
    n = struct.unpack_from("<I", raw, 4)[0]
    if 8 + n > len(raw):
        raise ParseError("OBJI data past its chunk")
    return bytes(raw[8:8 + n])


def _modl(data, c):
    return read_zmd(bytes(data[c["off"]:c["off"] + 8 + c["size"]]))


def read_propdef_sd(data):
    """PropDef._sd: PDBL{ PDIN(0x38), [OBJI], MODL } -> [{id, name, extents, obji, model}]."""
    out = []
    for f in _forms(data, "PDBL"):
        d = {"id": None, "name": None, "extents": None, "obji": None, "model": None}
        for k in f["children"]:
            if k["tag"] == "PDIN":
                raw = body(data, k)
                if len(raw) != 0x38 or struct.unpack_from("<I", raw, 0)[0] != 0x38:
                    raise ParseError("PDIN must be 0x38 bytes")
                d["id"] = struct.unpack_from("<I", raw, 4)[0]
                d["name"] = cstr(raw[8:28])
                d["extents"] = list(struct.unpack_from("<3f", raw, 28))
            elif k["tag"] == "OBJI":
                d["obji"] = _obji(data, k)
            elif k["tag"] == "MODL":
                d["model"] = _modl(data, k)
        if d["id"] is None or d["model"] is None:
            raise ParseError("PDBL without PDIN or MODL")
        out.append(d)
    return out


def _placed(raw, size, with_type):
    """Shared layout of the PRBL (0x48) and SDPT (0x4C) records."""
    if len(raw) != size or struct.unpack_from("<I", raw, 0)[0] != size:
        raise ParseError(f"record must be 0x{size:X} bytes")
    o = 12 if with_type else 8
    r = {"id": struct.unpack_from("<I", raw, 4)[0], "name": cstr(raw[o:o + 16]),
         "matrix": list(struct.unpack_from("<9f", raw, o + 16)), "pos": list(struct.unpack_from("<3f", raw, o + 52))}
    if with_type:
        r["type"] = struct.unpack_from("<I", raw, 8)[0]
    return r


def read_prop_sd(data):
    """Prop._sd: PRBL{ PRBL(0x48), [OBJI] } -> [{def_id, name, matrix (9, ZGE rows), pos (ZGE/100), obji}]."""
    out = []
    for f in _forms(data, "PRBL"):
        rec = [k for k in f["children"] if k["tag"] == "PRBL"]
        if len(rec) != 1:
            raise ParseError("PRBL form needs one PRBL record")
        r = _placed(body(data, rec[0]), 0x48, False)
        r["def_id"] = r.pop("id")
        r["obji"] = next((_obji(data, k) for k in f["children"] if k["tag"] == "OBJI"), None)
        out.append(r)
    return out


def read_scene_sd(data):
    """Scene._sd: SCPT{ SDPT(0x4C), [OBJI] } x n + SDPA{ MODL } -> {points, panorama}."""
    pts = []
    for f in _forms(data, "SCPT"):
        rec = [k for k in f["children"] if k["tag"] == "SDPT"]
        if len(rec) != 1:
            raise ParseError("SCPT needs one SDPT")
        r = _placed(body(data, rec[0]), 0x4C, True)
        r["obji"] = next((_obji(data, k) for k in f["children"] if k["tag"] == "OBJI"), None)
        pts.append(r)
    pano = None
    for f in _forms(data, "SDPA"):
        m = [k for k in f["children"] if k["tag"] == "MODL"]
        if m:
            pano = _modl(data, m[0])
    return {"type": "scene", "points": pts, "panorama": pano}


# ---------------------------------------------------------------------------- Tag._sd (SPEC 5.6)

def read_tag_sd(data):
    """Tag._sd: TAGB{ TAGD, TAGN*, TAGF*, TAGI } -> faces (ZGE vertices/normal + face/edge tag codes)."""
    top = _forms(data, "TAGB")
    if len(top) != 1:
        raise ParseError("expected one TAGB form")
    kids_ = top[0]["children"]
    tagd = [k for k in kids_ if k["tag"] == "TAGD"]
    tagi = [k for k in kids_ if k["tag"] == "TAGI"]
    if len(tagd) != 1 or len(tagi) != 1:
        raise ParseError("TAGB needs one TAGD and one TAGI")
    count = struct.unpack_from("<I", body(data, tagd[0]), 4)[0]
    blob = body(data, tagi[0])[8:]
    faces = []
    for k in kids_:
        if k["tag"] != "TAGF":
            continue
        raw = body(data, k)
        if len(raw) != 0x50:
            raise ParseError("TAGF must be 0x50 bytes")
        v = struct.unpack("<I12f7i", raw)
        f = {"verts": [list(v[1:4]), list(v[4:7]), list(v[7:10])], "normal": list(v[10:13]),
             "code": None, "kind": None, "flags": None, "param": None, "edges": [None, None, None]}
        if v[13] >= 0:
            if v[13] + 12 > len(blob):
                raise ParseError("face tag past the TAGI blob")
            f["code"], f["kind"], f["flags"], f["param"], _x = struct.unpack_from("<HBBII", blob, v[13])
        for e in range(3):
            if v[14 + e] >= 0:
                if v[14 + e] + 3 > len(blob):
                    raise ParseError("edge tag past the TAGI blob")
                f["edges"][e] = struct.unpack_from("<H", blob, v[14 + e])[0]
        faces.append(f)
    if count != len(faces):
        raise ParseError("TAGD face count does not match the TAGF chunks")
    nodes = [k for k in kids_ if k["tag"] == "TAGN"]
    leaves = sum(1 for k in nodes if body(data, k)[4] == 1)
    return {"type": "tags", "faces": faces, "nodes": len(nodes), "leaves": leaves}


# ---------------------------------------------------------------------------- whole level (SPEC 5)

def read_level(folder):
    """A level folder -> {name, files, origin, textures, world, prop_defs, props, scene, tags}."""
    lv = read_zsd_level(folder)
    names = {f.lower(): f for f in os.listdir(folder)}

    def load(kind):
        e = next((f for f in lv["files"] if f["kind"] == kind and f["exists"]), None)
        return open(os.path.join(folder, names[e["name"].lower()]), "rb").read() if e else None

    world = read_bsp(load("BSP"))
    out = {"type": "level", "name": os.path.basename(os.path.normpath(folder)), "files": lv["files"],
           "textures": lv["textures"], "world": world, "origin": world["origin"]}
    d = load("PropDef")
    out["prop_defs"] = read_propdef_sd(d) if d else []
    d = load("Prop")
    out["props"] = read_prop_sd(d) if d else []
    d = load("Scene")
    out["scene"] = read_scene_sd(d) if d else {"points": [], "panorama": None}
    d = load("Tag")
    out["tags"] = read_tag_sd(d) if d else {"faces": [], "nodes": 0, "leaves": 0}
    return out


def prop_instances(level):
    """[(prop, prop_def or None, RW position, RW rotation rows)] (SPEC 2.6)."""
    defs = {d["id"]: d for d in level["prop_defs"]}
    o = level["origin"]
    return [(p, defs.get(p["def_id"]), pos_to_rw(p["pos"], o), prop_matrix_to_rw(p["matrix"])) for p in level["props"]]


def model_meshes(z, lod=0):
    """Atomics of a model with their frame matrices baked in (positions/normals in model space):
    [{positions, normals, uvs, colors, triangles (a, b, c, slot), slots}]. Used for props/panorama."""
    sk = skeleton(z, lod)
    out = []
    for a in z["lods"][lod]["atomics"]:
        g = a["geometry"]
        w = sk[a["frame"]]["world"]

        def xf(p, t):
            return [w[i][0] * p[0] + w[i][1] * p[1] + w[i][2] * p[2] + (w[i][3] if t else 0.0) for i in range(3)]

        out.append({"positions": [xf(p, True) for p in g["positions"] or []],
                    "normals": [xf(n, False) for n in g["normals"]] if g["normals"] else None,
                    "uvs": g["uvs"][0] if g["uvs"] else None, "colors": g["colors"],
                    "triangles": g["triangles"], "slots": g["material_slots"]})
    return out


# ============================================================================ LZO1X (SPEC 6.3)

def lzo1x_decompress(src, out_len):
    """Standard LZO1X decompression (as minilzo), bounds-checked; returns ``out_len`` bytes."""
    out = bytearray()
    n = len(src)
    ip = 0

    def need(k):
        if ip + k > n:
            raise ParseError("LZO: input overrun")

    def copy_match(dist, length):
        start = len(out) - dist
        if start < 0:
            raise ParseError("LZO: lookbehind overrun")
        for k in range(length):                                   # may overlap: byte by byte
            out.append(out[start + k])

    def run_len(t, base):
        nonlocal ip
        if t == 0:
            while True:
                need(1)
                if src[ip] != 0:
                    break
                t += 255
                ip += 1
            t += base + src[ip]
            ip += 1
        return t

    need(1)
    state = "loop"
    t = 0
    if src[0] > 17:
        t = src[0] - 17
        ip = 1
        need(t)
        out += src[ip:ip + t]
        ip += t
        state = "first"
        if t < 4:                                   # short initial run: continue as after a match
            need(1)
            t = src[ip]
            ip += 1
            state = "match"
    while True:
        if state == "loop":
            need(1)
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            t = run_len(t, 15)
            need(t + 3)
            out += src[ip:ip + t + 3]
            ip += t + 3
            state = "first"
        if state == "first":
            need(1)
            t = src[ip]
            ip += 1
            if t < 16:
                need(1)
                dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 3)
                state = "done"
            else:
                state = "match"
        if state == "match":
            if t >= 64:
                need(1)
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
                ip += 1
                copy_match(dist, (t >> 5) - 1 + 2)
            elif t >= 32:
                t = run_len(t & 31, 31)
                need(2)
                dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                copy_match(dist, t + 2)
            elif t >= 16:
                hi = (t & 8) << 11
                t = run_len(t & 7, 7)
                need(2)
                dist = hi + (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if dist == 0:                                     # end of stream
                    if len(out) != out_len:
                        raise ParseError(f"LZO: decoded {len(out)} bytes, expected {out_len}")
                    return bytes(out)
                copy_match(dist + 0x4000, t + 2)
            else:
                need(1)
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 2)
            state = "done"
        if state == "done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "loop"
                continue
            need(t + 1)
            out += src[ip:ip + t]
            ip += t
            t = src[ip]
            ip += 1
            state = "match"
        if len(out) > out_len:
            raise ParseError("LZO: output overrun")


# ============================================================================ ZAL (SPEC 6)

ZAL_MAGIC = 0x2A81511C


def read_zal(data, headerless=False):
    """ZAL archive -> {'flags', 'count', 'compressed', 'codec', 'groups': [{'id', 'size', 'data'}]}.
    ``headerless``: the in-memory form (motion bank) without magic + 12 reserved bytes."""
    base = 0 if headerless else 0x10
    b = Buf(data)
    if not headerless:
        if b.u32() != ZAL_MAGIC:
            raise ParseError("not a ZAL (bad magic)")
        b.raw(12)
    flags, count = b.u16(), b.u16()
    per = flags & 0x1FFF
    ngroups = count & 0x3FFF
    comp = (flags >> 15) * 2 + (count >> 15)
    codec = 2 if count & 0x4000 else 1
    groups = []
    for _ in range(ngroups):
        gid = b.u32()
        for _a in range(per):
            off, size = b.u32(), b.u32()
            stored = b.u32() if comp else size & 0xFFFFFF
            p = base + off
            if p + stored > len(data):
                raise ParseError(f"ZAL asset {gid}: data past end of file")
            raw = data[p:p + stored]
            if comp:
                if codec != 2:
                    raise ParseError("ZAL codec 1 is not supported (unused by the game files)")
                payload = lzo1x_decompress(raw, size & 0xFFFFFF)
            else:
                payload = bytes(raw)
            groups.append({"id": gid, "size": size & 0xFFFFFF, "stored": stored, "data": payload})
    return {"type": "zal", "flags": flags, "count": count, "compressed": bool(comp),
            "codec": codec if comp else None, "groups": groups}


# ============================================================================ FORM / PLYR / ZMAP / STRS (SPEC 7)

def read_form(data):
    """IFF-like FORM (sizes little-endian, no form type): returns {tag: payload bytes} in order."""
    if data[:4] != b"FORM":
        raise ParseError("not a FORM")
    form = parse_4cc(data)[0]
    return [(c["tag"], bytes(body(data, c))) for c in form["children"]]


RIDERS = ("Dave Mirra", "Ryan Nyquist", "Troy McMurray", "Mike Laird", "Chad Kagy", "Tim Mirra", "Kenan Harkin",
          "Shaun Butler", "Leigh Ramsdell", "Joey Garcia", "Slim Jim Guy", "Amish Boy")


def read_plyr(data):
    """Player table: miscdata.zal group 1 = FORM{ PLHD(8), PLYR(count * 24) }. Accepts the FORM bytes
    or the whole miscdata.zal. Entry index = rider * 5 + style (SPEC 7.1)."""
    if data[:4] != b"FORM":
        data = next(g["data"] for g in read_zal(data)["groups"] if g["id"] == 1)
    chunks = dict(read_form(data))
    count, esize, unk1, unk2 = struct.unpack("<4H", chunks["PLHD"][:8])
    if esize != 24:
        raise ParseError("PLHD entry size is not 24 (the game asserts 'Out of sync player data')")
    raw = chunks["PLYR"]
    if len(raw) < count * esize:
        raise ParseError("PLYR shorter than PLHD count * size")
    entries = []
    for i in range(count):
        e = raw[i * esize:(i + 1) * esize]
        off, ln, fl = struct.unpack_from("<HBB", e, 0)
        entries.append({"index": i, "rider": i // 5, "style": i % 5, "ref_offset": off, "ref_length": ln,
                        "ref_flags": fl, "id": struct.unpack_from("<I", e, 4)[0], "name": cstr(e[8:24])})
    return {"type": "plyr", "count": count, "entry_size": esize, "plhd_rest": [unk1, unk2], "entries": entries}


def read_zmap(data):
    """Level table: tables.zal group 0 = FORM{ ZMHD(u32 count, u32 record size), ZMAP(count * 100) }."""
    if data[:4] != b"FORM":
        data = read_zal(data)["groups"][0]["data"]
    chunks = dict(read_form(data))
    count, rsize = struct.unpack("<II", chunks["ZMHD"][:8])
    raw = chunks["ZMAP"]
    levels = []
    for i in range(count):
        r = raw[i * rsize:(i + 1) * rsize]
        levels.append({"index": i, "head": list(struct.unpack_from("<4H", r, 0)),
                       "values": list(struct.unpack_from("<13I", r, 8)), "name": cstr(r[60:80]), "zal": cstr(r[80:100])})
    return {"type": "zmap", "count": count, "record_size": rsize, "levels": levels}


def read_strs(data):
    """String table: 'STRS', u16 base text id, u16 count, u16 offset[count] (from the chunk start),
    NUL-terminated latin-1 strings. Text id = base + index."""
    if data[:4] != b"STRS":
        raise ParseError("not a STRS table")
    base, n = struct.unpack_from("<HH", data, 4)
    b = Buf(data, 8)
    offs = [b.u16() for _ in range(n)]
    out = []
    for o in offs:
        if o >= len(data):
            raise ParseError("STRS offset past end")
        out.append(cstr(data[o:]))
    return {"type": "strs", "base_id": base, "strings": out}


# ============================================================================ motion bank (SPEC 8)

RIDER_CHANNELS = ("Hips", "Chest", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm",
                  "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot")
BIKE_CHANNELS = ("Hipsdeck", "Handlebars", "Sprocket", "RightPedal", "LeftPedal", "BackWheel", "FrontWheel",
                 "LeftGrip", "RightGrip", "SeatGrip")


def read_motion_bank(data):
    """motion.zal -> {id: clip}. Group 0 holds a headerless ZAL of 238 clips (ids 1..238)."""
    outer = read_zal(data)
    inner = read_zal(outer["groups"][0]["data"], headerless=True)
    return {g["id"]: read_motion(g["data"]) for g in inner["groups"]}


def read_motion(data):
    """One motion clip (decoder zactor_00472b10)."""
    b = Buf(data)
    frames_word, flags, frame_ms, extra = b.take("HHII")
    wide = bool(flags & 0x8000)
    sections = [list(b.take("BBH")) for _ in range((flags & 0xFF) >> 4)]
    if frames_word & 0x8000:
        b.raw(100)
    vfmt_init, vfmt = ("3h", "3h") if wide else ("3B", "3b")

    def rec(init, si):
        start = b.p
        mask, t = b.take("IH")
        h, value = (b.take("Hf") if mask & 1 else (0, None))
        trans = list(b.take("3h")) if mask & 2 else None
        cnt = sections[si][0]
        rots = {}
        if init:
            if mask & 4:
                for i in range(cnt):
                    rots[i] = list(b.take(vfmt_init))
        else:
            for i in range(cnt):
                if mask & (4 << i):
                    rots[i] = list(b.take(vfmt))
        b.raw((-(b.p - start)) % 4)                                  # pad to 4
        return {"section": si, "time": t, "value": value, "trans": trans, "rots": rots}

    inits = []
    for si, (cnt, sflags, kind) in enumerate(sections):
        if kind == 0:                                                # fixed rider init record
            start = b.p
            mask, t, h, value = b.take("IHHf")
            trans = list(b.take("3h"))
            rots = {i: list(b.take(vfmt_init)) for i in range(cnt)}
            b.raw((-(b.p - start)) % 4)
            inits.append({"section": si, "time": t, "value": value, "trans": trans, "rots": rots})
        elif sflags & 2:
            inits.append(None)
        else:
            inits.append(rec(True, si))
    records = []
    while True:
        mask = struct.unpack_from("<I", data, b.p)[0] if b.left() >= 4 else None
        if mask is None:
            raise ParseError("motion: missing terminator")
        if mask == 0:
            b.u32()
            end_time = b.u32()
            break
        si = mask >> 28
        if si >= len(sections):
            raise ParseError("motion: record for a missing section")
        records.append(rec(False, si))
    return {"frames": frames_word & 0x7FFF, "frame_ms": frame_ms, "wide": wide, "sections": sections,
            "inits": inits, "records": records, "end_time": end_time}


def _s16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


def sample_motion(clip):
    """Per-frame poses: [{'time', 'rider': [euler]*15|None, 'bike': [euler]*n|None, 'rider_trans', 'bike_trans'}].
    Init values are absolute; each frame, records at that time replace per-channel deltas, then every
    channel adds its current delta (deltas persist)."""
    st = []
    for si, (cnt, _, kind) in enumerate(clip["sections"]):
        r = clip["inits"][si]
        rot = [[0, 0, 0] for _ in range(cnt)]
        trans = [0.0, 0.0, 0.0]
        if r is not None:
            for i, v in r["rots"].items():
                rot[i] = list(v) if clip["wide"] else [x << 8 for x in v]
            if r["trans"] is not None:
                trans = [c * 0.125 for c in r["trans"]]
        st.append({"kind": kind, "rot": rot, "drot": [[0, 0, 0] for _ in range(cnt)], "trans": trans,
                   "dtrans": [0.0, 0.0, 0.0]})
    by_time = {}
    for r in clip["records"]:
        by_time.setdefault(r["time"], []).append(r)
    root = [0.0, 0.0, 0.0]
    poses = []
    for k in range(clip["frames"]):
        if k:
            for r in by_time.get(k * clip["frame_ms"], ()):
                s = st[r["section"]]
                if r["trans"] is not None:
                    s["dtrans"] = [c / 256.0 for c in r["trans"]]
                for i, v in r["rots"].items():
                    s["drot"][i] = list(v) if clip["wide"] else [x << 7 for x in v]
            for s in st:
                for rr, dd in zip(s["rot"], s["drot"]):
                    for i in range(3):
                        rr[i] = _s16(rr[i] + dd[i])
                if s["kind"] == 0:
                    root[0] += s["dtrans"][0]
                    root[2] += s["dtrans"][2]
                    s["trans"][1] += s["dtrans"][1]
                else:
                    for i in range(3):
                        s["trans"][i] += s["dtrans"][i]
        pose = {"time": k * clip["frame_ms"], "rider": None, "bike": None, "rider_trans": None, "bike_trans": None}
        for s in st:
            if s["kind"] == 0:
                pose["rider"] = [tuple(r) for r in s["rot"]]
                pose["rider_trans"] = (s["trans"][0] + root[0], s["trans"][1], s["trans"][2] + root[2])
            else:
                pose["bike"] = [tuple(r) for r in s["rot"]]
                pose["bike_trans"] = tuple(s["trans"])
        poses.append(pose)
    return poses


def euler_to_quat(a):
    """s16 binary angles (65536 = 360 deg) -> quaternion (x, y, z, w) = qz * qy * qx (SPEC 8.3)."""
    hx, hy, hz = (v * math.pi / 65536.0 for v in a)
    cx, sx, cy, sy, cz, sz = math.cos(hx), math.sin(hx), math.cos(hy), math.sin(hy), math.cos(hz), math.sin(hz)
    return (sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz)


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


_ZP, _ZM = euler_to_quat((0, 0, 0x4000)), euler_to_quat((0, 0, -0x4000))
# (pre, post): q' = pre * q * post. NgIFzactor's literal arm table (SPEC 8.3): upper arms q*Cz(+-90),
# lower arms / hands Cz(-+90)*q*Cz(+-90).
ARM_FIX = {"LeftUpArm": (None, _ZP), "LeftLowArm": (_ZM, _ZP), "LeftHand": (_ZM, _ZP),
           "RightUpArm": (None, _ZM), "RightLowArm": (_ZP, _ZM), "RightHand": (_ZP, _ZM)}


def channel_rotation(name, euler):
    """Local frame rotation (model axes) the game gives a channel's frame, rider or bike (SPEC 8.3):
    Euler -> quaternion, the arm table, then the engine -> model conversion (x, y, z, w) -> (x, -y, -z, w),
    i.e. F R F with F = diag(1, -1, -1) (zrwModelQuatToRwMatrix 0x00487E70)."""
    q = euler_to_quat(euler)
    pre, post = ARM_FIX.get(name, (None, None))
    if pre is not None:
        q = quat_mul(pre, q)
    if post is not None:
        q = quat_mul(q, post)
    return (q[0], -q[1], -q[2], q[3])


# ============================================================================ ZSND (SPEC 9)

def read_zsnd(data):
    """Sound bank header + tables (samples are not decoded: PCM16 / IMA-ADPCM, see SPEC 9)."""
    if data[:4] != b"ZSND":
        raise ParseError("not a ZSND sound bank")
    h = struct.unpack_from("<4s4sII12I", data, 0)
    tables = {k: (h[4 + 2 * i], h[5 + 2 * i]) for i, k in enumerate("ABCDEF")}
    n_c, off_c = tables["C"]
    samples = [dict(zip(("offset", "size"), struct.unpack_from("<II", data, off_c + 8 * i))) for i in range(n_c)]
    n_b, off_b = tables["B"]
    defs = []
    for i in range(n_b):
        f, fl, rate, _, ls, le = struct.unpack_from("<HHIIII", data, off_b + 24 * i)
        defs.append({"sample": f, "flags": fl, "rate": rate, "loop_start": ls, "loop_end": le})
    return {"type": "zsnd", "platform": h[1].decode("latin-1"), "size": h[2], "header_end": h[3],
            "counts": {k: v[0] for k, v in tables.items()}, "samples": samples, "sample_defs": defs}


# ============================================================================ SAVE.DAT (SPEC 10)

def read_save(data):
    """SAVE.DAT: 8-byte header + 3 raw memory blocks. Decodes the header, options and profiles."""
    if len(data) != 0x235C or struct.unpack_from("<I", data, 4)[0] != 0x235C:
        raise ParseError("not a SAVE.DAT (size field must be 0x235C)")
    o = {"type": "save", "magic": hex(struct.unpack_from("<I", data, 0)[0]),
         "cheats_unlocked": struct.unpack_from("<H", data, 8)[0], "music_track": data[0x0E],
         "riders_unlocked": data[0x0F], "careers": [], "profiles": []}
    for r in range(12):
        p = 0x10 + r * 0x104
        o["careers"].append({"rider": RIDERS[r], "flags": struct.unpack_from("<I", data, p)[0],
                             "levels": data[p + 4], "styles": data[p + 5], "bikes": data[p + 6]})
    for k in range(2):
        p = 0x22C4 + k * 0x4C
        o["profiles"].append({"rider": data[p + 3], "style": data[p + 4], "bike": data[p + 5],
                              "camera": data[p + 6], "name": cstr(data[p + 7:p + 0x24])})
    return o


# ============================================================================ skeleton helper

def _mat(f):
    """Frame -> 4x4 column-vector matrix (columns right, up, at, pos). RW uses row vectors:
    p' = p * [right; up; at] + pos."""
    r, u, a, p = f["right"], f["up"], f["at"], f["pos"]
    return [[r[0], u[0], a[0], p[0]], [r[1], u[1], a[1], p[1]], [r[2], u[2], a[2], p[2]], [0, 0, 0, 1]]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def skeleton(z, lod=0):
    """Frames of a LOD with names (MDEF entry by bone id, else a known name with that zhash, else
    'bone_0x<id>'; 'frame_<i>' when the frame has no bone id) and local/world matrices."""
    lo = z["lods"][lod]
    names = {e["id"]: e["name"] for e in (z["mdef"] or {}).get("entries", [])}
    out = []
    for i, f in enumerate(lo["frames"]):
        bid = f["bone_id"]
        if bid is None or bid == 0xFFFFFFFF:                       # frame without a bone id
            name = f"frame_{i}"
        else:
            name = names.get(bid) or _KNOWN.get(bid) or f"bone_0x{bid:08X}"
        local = _mat(f)
        world = mat_mul(out[f["parent"]]["world"], local) if 0 <= f["parent"] < i else local
        out.append({"index": i, "parent": f["parent"], "bone_id": bid, "name": name, "local": local, "world": world})
    return out


def vertex_frames(z, lod, atomic):
    """For a skinned atomic: frame index per vertex (Bone plugin ranges + frame bone ids), else None."""
    lo = z["lods"][lod]
    a = lo["atomics"][atomic]
    g = a["geometry"]
    if not g["bones"]:
        return None
    id2frame = {}
    for i, f in enumerate(lo["frames"]):
        if f["bone_id"] is not None:
            id2frame.setdefault(f["bone_id"], i)
    out = [a["frame"]] * g["num_vertices"]
    for first, count, bid in g["bones"]:
        fi = id2frame.get(bid)
        if count and fi is not None:
            for v in range(first, min(first + count, g["num_vertices"])):
                out[v] = fi
    return out


def upright_correction(z, lod=0):
    """'rx-90' if the skinned skeleton lies along +Z (in-game riders), else None (SPEC 2.5)."""
    lo = z["lods"][lod]
    if not any(a["geometry"]["bones"] for a in lo["atomics"]):
        return None
    sk = skeleton(z, lod)
    roots = [s for s in sk if s["parent"] < 0]
    kids_ = [s for s in sk if roots and s["parent"] == roots[0]["index"]]
    if not kids_:
        return None
    up_z = kids_[0]["world"][2][1]
    return "rx-90" if up_z > 0.9 else ("rx+90" if up_z < -0.9 else None)


# ============================================================================ images: colour key (SPEC 4.2)

def decode_bmp(d):
    """Uncompressed 1/4/8/24/32-bit BMP -> (width, height, RGBA bytes top-down)."""
    if d[:2] != b"BM" or len(d) < 54:
        raise ParseError("not a BMP")
    off = struct.unpack_from("<I", d, 10)[0]
    hs, w, h, _pl, bpp, comp = struct.unpack_from("<IiiHHI", d, 14)
    clr = struct.unpack_from("<I", d, 46)[0]
    if comp != 0 or bpp not in (1, 4, 8, 24, 32) or not (0 < w <= 8192 and 0 < abs(h) <= 8192):
        raise ParseError("unsupported BMP")
    pal = []
    if bpp <= 8:
        p0 = 14 + hs
        for i in range(clr or (1 << bpp)):
            b_, g_, r_ = d[p0 + 4 * i:p0 + 4 * i + 3]
            pal.append(bytes((r_, g_, b_, 255)))
    stride = ((w * bpp + 31) // 32) * 4
    if off + stride * abs(h) > len(d):
        raise ParseError("BMP pixel data past the end")
    rows = []
    for y in range(abs(h)):
        r = d[off + y * stride:off + (y + 1) * stride]
        px = bytearray()
        for x in range(w):
            if bpp == 24:
                px += bytes((r[3 * x + 2], r[3 * x + 1], r[3 * x], 255))
            elif bpp == 32:
                px += bytes((r[4 * x + 2], r[4 * x + 1], r[4 * x], 255))
            else:
                per = 8 // bpp
                i = (r[x // per] >> (8 - bpp * (x % per + 1))) & ((1 << bpp) - 1)
                px += pal[i] if i < len(pal) else b"\0\0\0\xff"
        rows.append(bytes(px))
    if h > 0:
        rows.reverse()
    return w, abs(h), b"".join(rows)


def encode_png(w, h, rgba):
    """8-bit RGBA PNG (filter 0), valid CRCs."""
    import zlib

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_may_have_key(d):
    """Cheap test: can this BMP contain a magenta pixel? 4/8-bit: some palette entry is (255, 0, 255);
    24/32-bit: some pixel is. (The MAXScript importer uses the same test.)"""
    if d[:2] != b"BM" or len(d) < 54:
        return False
    off = struct.unpack_from("<I", d, 10)[0]
    hs, w, h, _pl, bpp = struct.unpack_from("<IiiHH", d, 14)
    if bpp <= 8:
        n = struct.unpack_from("<I", d, 46)[0] or (1 << bpp)
        p0 = 14 + hs
        return any(d[p0 + 4 * i:p0 + 4 * i + 3] == b"\xff\x00\xff" for i in range(n))
    k = bpp // 8
    stride = ((w * bpp + 31) // 32) * 4
    for y in range(abs(h)):
        r = d[off + y * stride:off + y * stride + w * k]
        if any(r[x:x + 3] == b"\xff\x00\xff" for x in range(0, len(r), k)):
            return True
    return False


def color_key_png(bmp):
    """The game's colour key for a BMP texture: pixels of exactly RGB (255, 0, 255) become fully
    transparent (RGBA 0,0,0,0). Returns PNG bytes, or None when the image has no magenta pixel."""
    if not bmp_may_have_key(bmp):
        return None
    w, h, rgba = decode_bmp(bmp)
    key = b"\xff\x00\xff\xff"
    if not any(rgba[i:i + 4] == key for i in range(0, len(rgba), 4)):
        return None
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        if out[i:i + 4] == key:
            out[i:i + 4] = b"\0\0\0\0"
    return encode_png(w, h, bytes(out))


# ============================================================================ detection / CLI

def sniff(data, name=""):
    n = name.lower()
    if data[:4] == b"MODL" or (not data and n.endswith(".zmd")):
        return "zmd"
    if data[:4] == b"SCEN":
        return "zsd"
    if data[:4] == b"MATI":
        return "mat_sd"
    if data[:4] in (b"PDBL", b"PRBL", b"SCPT", b"SDPA", b"TAGB"):
        return "level_" + data[:4].decode()
    if data[:4] == b"FORM":
        return "form"
    if data[:4] == b"STRS":
        return "strs"
    if data[:4] == b"ZSND":
        return "zsnd"
    if len(data) == 0x235C and data[4:8] == struct.pack("<I", 0x235C):
        return "save"
    if data[:4] == struct.pack("<I", ZAL_MAGIC):
        return "zal"
    if len(data) >= 12 and struct.unpack_from("<I", data, 0)[0] in (0x0B, 0x10):
        return "rw"
    return None


def read_any(data, name=""):
    kind = sniff(data, name)
    if kind == "zmd":
        return read_zmd(data)
    if kind == "zsd":
        return read_zsd(data)
    if kind == "mat_sd":
        return {"type": "mat_sd", "textures": read_mat_sd(data)}
    if kind == "form":
        tags = [t for t, _ in read_form(data)]
        if "PLYR" in tags:
            return read_plyr(data)
        if "ZMAP" in tags:
            return read_zmap(data)
        return {"type": "form", "chunks": tags}
    if kind == "strs":
        return read_strs(data)
    if kind == "zsnd":
        return read_zsnd(data)
    if kind == "save":
        return read_save(data)
    if kind == "zal":
        z = read_zal(data)
        if name.lower().endswith("motion.zal"):
            z["motions"] = read_motion_bank(data)
        return z
    if kind == "rw":
        if struct.unpack_from("<I", data, 0)[0] == 0x0B:
            return read_bsp(data)
        return {"type": "rw", "chunks": parse_rw(data)}
    if kind == "level_PDBL":
        return {"type": "propdefs", "defs": read_propdef_sd(data)}
    if kind == "level_PRBL":
        return {"type": "props", "props": read_prop_sd(data)}
    if kind in ("level_SCPT", "level_SDPA"):
        return read_scene_sd(data)
    if kind == "level_TAGB":
        return read_tag_sd(data)
    raise ParseError("unknown file type")


def print_tree(data, name="", out=None):
    out = out or sys.stdout
    kind = sniff(data, name)

    def rw(chunks, d):
        for c in chunks:
            out.write(f"{'  ' * d}{c['name']} size={c['size']} ver=0x{c['version']:X}\n")
            rw(c.get("children", []), d + 1)

    def zx(chunks, d):
        for c in chunks:
            out.write(f"{'  ' * d}{c['tag']} size={c['size']}\n")
            if c["tag"] == "MDFF":
                rw(parse_rw(data, c["off"] + 8, c["off"] + 8 + c["size"]), d + 1)
            zx(c.get("children", []), d + 1)

    if kind in ("zmd", "zsd", "mat_sd", "form") or (kind or "").startswith("level_"):
        if data:
            zx(parse_4cc(data), 0)
    elif kind == "rw":
        rw(parse_rw(data), 0)
    elif kind == "zal":
        z = read_zal(data)
        out.write(f"ZAL flags=0x{z['flags']:04X} count=0x{z['count']:04X} groups={len(z['groups'])}\n")
        for g in z["groups"]:
            out.write(f"  group 0x{g['id']:X} size={g['size']} stored={g['stored']} starts={g['data'][:4]!r}\n")
    elif kind == "strs":
        s = read_strs(data)
        out.write(f"STRS base=0x{s['base_id']:X} strings={len(s['strings'])}\n")
    elif kind == "zsnd":
        s = read_zsnd(data)
        out.write(f"ZSND {s['platform']} size={s['size']} tables={s['counts']}\n")
    elif kind == "save":
        out.write("SAVE.DAT header(8) blockA(0xC3C) records(0x1680) profiles(0x98)\n")
    else:
        raise ParseError("unknown file type")


def _jsonable(o, full):
    if isinstance(o, (bytes, bytearray)):
        return {"bytes": len(o), "head": bytes(o[:16]).hex()}
    if isinstance(o, dict):
        return {str(k): _jsonable(v, full) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        if not full and len(o) > 8 and all(isinstance(x, (list, tuple)) for x in o):
            return {"count": len(o), "first": _jsonable(o[:3], full)}
        return [_jsonable(x, full) for x in o]
    return o


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Dave Mirra Freestyle BMX reference parser (see ../SPEC.md)")
    sp = ap.add_subparsers(dest="cmd", required=True)
    for c in ("tree", "dump"):
        p = sp.add_parser(c)
        p.add_argument("file")
        if c == "dump":
            p.add_argument("--full", action="store_true", help="write every array element")
    p = sp.add_parser("textures")
    p.add_argument("file")
    p.add_argument("outdir")
    a = ap.parse_args(argv)
    if os.path.isdir(a.file):
        data = None
    else:
        data = open(a.file, "rb").read()
    try:
        if a.cmd == "tree":
            print_tree(data, a.file)
        elif a.cmd == "dump":
            obj = read_level(a.file) if data is None else read_any(data, a.file)
            json.dump(_jsonable(obj, a.full), sys.stdout, indent=1)
            print()
        else:
            obj = read_zsd_level(a.file) if data is None else read_any(data, a.file)
            os.makedirs(a.outdir, exist_ok=True)
            for t in obj.get("textures", []):
                path = os.path.join(a.outdir, f"{t['name']}.{t['kind']}")
                open(path, "wb").write(t["data"])
                print(path)
    except ParseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

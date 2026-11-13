"""DM-059 self-consistency: decomp of the ZglRw layer (zrw*.c, the ZGL driver over RenderWare 3.1)
and the gaps between its files.

No C compiler on the build PC, so the C is checked the way DM-041..DM-057 do it:
1. structure: every function of the DM-059 files is drafted; names agree with decomp/functions.csv and
   decomp/symbols/symbols.csv (the last row wins); no Ghidra artefacts or placeholder names; the zglrw.h types are
   size-checked in C, listed in ghidra_types.txt and have struct rows;
2. exe facts: the nine driver tables (texture, material, model, light, collision, primitives, camera,
   system, debug) read from NgBMX.exe point at the functions the C tables name; every assert (file,
   line) of the C is a call site; constants (LOD distances, light types, shadow corners, colour key,
   clear colour, default fov, the CD-check strings);
3. behaviour: Python ports agree with dmbmx on the corpus: the colour key of zrwTxr_ApplyColorKey
   (palette-level for 4/8-bit, pixel-level for 24-bit) vs dmbmx.formats.texture on every TBMP; the
   MLOD / MDEF reading of zrwModelCreate (LOD distances, skinned flag, entry table) vs dmbmx.formats.zmd
   on every ZMD; the nearest-hit line test of zrwColLineTest through dmbmx.formats.rwcoll vs brute
   force on a level.
"""
import csv
import random
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR, corpus_files

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
ZRW = "NgEngine/Zge/Com/ZglRw/"
FILES = {  # DM-059's files and their function counts
    ZRW + "zrwTxr.c": 49, ZRW + "zrwMat.c": 25, ZRW + "zrwModel.c": 47, ZRW + "zrwModelCreate.c": 51,
    ZRW + "zrwModelGeom.c": 9, ZRW + "zrwModelRender.c": 19, ZRW + "zrwLight.c": 10, ZRW + "zrwCam.c": 33,
    ZRW + "zrwCol.c": 21, ZRW + "zrwPrim.c": 12, ZRW + "zrwShadow.c": 5, ZRW + "zrwDbg.c": 32,
    ZRW + "zrwZgl.c": 43,
    # DM-064 moved the other gap files' functions into zobj.c / zobjModel.c / pcinput.c / zobjPEmit.c
    # (test_decomp_dm064.py) and created 7 more in zrwModelCreate.c (card DM-064)
    "_unassigned/gap_zrwCol__zobjPEmit.c": 14,
}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, ([^,\n]*), status (\w+)[^*]*(?:\*(?!/)[^*]*)*\*/\s*\n"
                   r"([A-Za-z_][^;{]*?)\(")
B = 0x400000


def text(f):
    return (SRC / f).read_text(encoding="utf-8")


def c_functions():
    out = {}
    for f in FILES:
        for m in VA_RE.finditer(text(f)):
            out[int(m.group(1), 16)] = (f, m.group(4), m.group(5).split()[-1].lstrip("*"))
    return out


def fcsv():
    return {int(r["va"], 16): r for r in csv.DictReader(open(REPO / "decomp" / "functions.csv", encoding="utf-8"))}


def symbols():
    out = {}
    for r in csv.DictReader(open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8")):
        if r["kind"] == "func":
            out[int(r["address"], 16)] = r       # the last row wins, as in ApplySymbols
    return out


# ------------------------------------------------------------------ 1. structure
def test_all_functions_drafted_and_named_consistently():
    fns = c_functions()
    for f, n in FILES.items():
        assert sum(1 for x in fns.values() if x[0] == f) == n, f
    rows, sym = fcsv(), symbols()
    for va, (f, status, name) in fns.items():
        r = rows[va]
        assert r["file"] == f, (hex(va), r["file"], f)
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert r["name"] == name, (hex(va), r["name"], name)
        assert sym[va]["name"] == name, (hex(va), sym[va]["name"], name)
    # every function of the ZglRw files (zrwFile.c / zrwWorld.c / zrwRamDisk.c belong to other cards)
    for va, r in rows.items():
        if r["file"] in FILES:
            assert va in fns, (hex(va), r["file"])


def test_no_ghidra_artefacts_or_placeholders():
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|PTR_\w+|in_stack_\w+|"
                     r"unaff_\w+|[iu]Var\d+|p[A-Z]\w*Var\d+|f\w*Var\d+|[a-z]Stack_\w+|float10|CONCAT\d+|"
                     r"FUN_[0-9a-f]{8}|TODO\(decomp\)|zrw[A-Za-z]*_[0-9a-f]{8})\b")
    for f in FILES:
        for n, line in enumerate(text(f).splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
    # nobody calls the placeholder names of these functions any more
    old = re.compile(r"\b(zrw(Dbg|Model|ModelCreate|ModelGeom|ModelRender|Mat|Zgl|Prim|Cam|Col|Txr|Shadow|Light)"
                     r"_[0-9a-f]{8})\b")
    for p in SRC.rglob("*.c"):
        assert not old.search(p.read_text(encoding="utf-8")), p


def test_zglrw_types_size_checked():
    h = (INC / "zglrw.h").read_text(encoding="utf-8")
    names = re.findall(r"\}\s*([A-Z]\w+);\s*/\* (0x[0-9A-F]+) \*/", h)
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8")
    assert "\nzglrw.h\n" in types
    sizes = dict(re.findall(r"^size (\w+) (0x[0-9A-Fa-f]+)$", types, re.M))
    skip = h[h.index("GHIDRA-SKIP-BEGIN"):]
    rows = {r["name"] for r in csv.DictReader(open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8"))
            if r["kind"] == "struct" and r["card"] == "DM-059"}
    assert len(names) >= 26
    for n, sz in names:
        assert int(sizes[n], 16) == int(sz, 16), n
        assert re.search(r"sizeof\(%s\) == 0x0*%X\b" % (n, int(sz, 16)), skip), n
        assert n in rows, n


# ------------------------------------------------------------------ 2. exe facts
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def u32(img, va, n=1):
    return list(struct.unpack_from("<%dI" % n, img, va - B))


TABLES = {  # C table name -> (file, VA)
    "g_texDriverVtbl": (ZRW + "zrwTxr.c", 0x005623F0), "g_zglMaterialApi": (ZRW + "zrwMat.c", 0x00561018),
    "g_zglLightApi": (ZRW + "zrwLight.c", 0x00563420), "g_zglColApi": (ZRW + "zrwCol.c", 0x00562060),
    "g_zglPrimApi": (ZRW + "zrwPrim.c", 0x00561720), "g_zglCameraApi": (ZRW + "zrwCam.c", 0x00561D88),
    "g_zglSystemApi": (ZRW + "zrwZgl.c", 0x00561468), "g_zrwDbgDriver": (ZRW + "zrwDbg.c", 0x00560030),
}
MODEL_TABLE = [  # 0x00560E78, 41 slots (the table lives in zrwModel.c's data; the functions in 4 files)
    "zrwModelInit", "zrwModelShutdown", "zrwModelLoadMemory", "zrwModelLoadFile", "zrwModelSave",
    "zrwModelLoadIff", "zrwModelCreateInstance", "zrwModelRelease", "zrwModelUpdate", "zrwModelSetPosition",
    "zrwModelSetRotation", "zrwModelSetScale", "zrwModelGetNumParts", "zrwModelFindPart",
    "zrwModelPartToWorld", "zrwModelPartBindOffset", "zrwModelGetPartMatrix", "zrwModelGetPartParent",
    "zrwModelGetNameHash", "zrwModelGetDefEntry", "zrwModelGetPartIds", "zrwModelBindParts", "zrwModelRender",
    "zrwModelRenderSkeleton", "zrwModelSetFlags", "zrwModelSetUser", "zrwModelGetUser", "zrwModelGetClump",
    "zrwModelFindMaterial", "zrwModelGetNumMaterials", "zrwModelGetMaterial", "zrwModelSetColour",
    "zrwModelSetUpdateCallback", "zrwModelGeomNotCoded", "zrwModelGeomVertexNumber", "zrwModelGeomLock",
    "zrwModelGeomUnlock", "zrwModelGeomGetVertex", "zrwModelGeomSetVertex", "zrwModelPrintInfo",
    "zrwModelPrintStats"]


def test_driver_tables_match_the_exe(img):
    by_name = {r["name"]: va for va, r in fcsv().items()}
    by_name.update({"zAssertMessageBox": 0x00489CF0, "timeGetTime": 0x00489CB0})
    for table, (f, va) in TABLES.items():
        m = re.search(r"\b%s(?:\[\d+\])? = \{(.*?)\};" % table, text(f), re.S)
        assert m, table
        body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        slots = [re.findall(r"\w+", s)[-1] for s in body.split(",") if s.strip()]
        got = u32(img, va, len(slots))
        assert got == [by_name[s] for s in slots], table
    assert u32(img, 0x00560E78, len(MODEL_TABLE)) == [by_name[s] for s in MODEL_TABLE]
    # zrwZglInit installs the drivers in this order: texture, material, model, light, collision, prims
    code = img[0x0048AFE0 - B:0x0048B110 - B]
    order = [code.find(struct.pack("<I", t)) for t in (0x005623F0, 0x00561018, 0x00560E78, 0x00563420,
                                                       0x00562060, 0x00561720)]
    assert all(o > 0 for o in order) and order == sorted(order)


REGISTER_LINES = {(ZRW + "zrwTxr.c", 0x2B2), (ZRW + "zrwTxr.c", 0x2CB)}


def test_asserts_are_call_sites(img):
    """Every zAssertFail(msg, <FILE>_SRC, line) of the ZglRw files pushes that line and that file's
    path string in the exe."""
    n = 0
    for f in FILES:
        if not f.startswith(ZRW):
            continue
        t = text(f)
        mac = re.search(r'#define (\w+_SRC) "(.*?)"', t)
        path = mac.group(2).replace("\\\\", "\\")
        addrs = [m.start() + B for m in re.finditer(re.escape(path.encode() + b"\0"), img)]
        assert addrs, path
        for line in re.findall(r"zAssertFail\([^;]*?%s, (0x[0-9A-Fa-f]+|\d+), 0\)" % mac.group(1), t, re.S):
            line = int(line, 0)
            if (f, line) in REGISTER_LINES:     # the line number goes through a register (a shared tail)
                assert struct.pack("<I", line) in img
                n += 1
                continue
            pl = bytes([0x6A, line]) if line < 0x80 else b"\x68" + struct.pack("<I", line)
            # push line, then (at most one short instruction later: zstringFormat messages) push file
            assert any(re.search(re.escape(pl) + b".{0,9}?" + re.escape(b"\x68" + struct.pack("<I", a)), img, re.S)
                       for a in addrs), (f, hex(line))
            n += 1
    assert n >= 50


def test_constants(img):
    assert struct.unpack_from("<4f", img, 0x00562CC4 - B) == (0.0, 50.0, 100.0, 150.0)   # LOD distances
    assert u32(img, 0x00563408, 5) == [1, 2, 0x80, 0x81, 0x82]                            # RpLight types
    assert u32(img, 0x0054E104, 8) == [0, 0, 1, 0, 1, 1, 0, 1]                            # shadow corners
    # zrwTxr_Init / zrwMat_Init / zrwModelCreateInit register their IFF chunk handlers
    for va, end, tags in ((0x00498660, 0x00498790, (b"TBMP", b"TPNG", b"TDDS")),
                          (0x00489440, 0x004894D0, (b"MATI", b"MADF")),
                          (0x0049D9D0, 0x0049DA60, (b"MODL", b"MLOD", b"MCOL"))):
        code = img[va - B:end - B]
        for t in tags:
            assert b"\x68" + t in code, t
    # zrwZglInit: the clear colour 7F AE DD 00 is built byte by byte on the stack
    zinit = img[0x0048AFE0 - B:0x0048B110 - B]
    for k, v in enumerate((0x7F, 0xAE, 0xDD, 0x00)):
        assert bytes([0xC6, 0x44, 0x24, 0x10 + k, v]) in zinit
    # zrwCamCreate: fov 0x271C, near 0.5, far 75
    cam = img[0x004925F0 - B:0x00492970 - B]
    assert struct.pack("<H", 0x271C) in cam and struct.pack("<f", 75.0) in cam and struct.pack("<f", 0.5) in cam
    # app_CheckGameCD / zrwShadowInit strings
    for s in (b"Software\\Acclaim Entertainment\\Dave Mirra Freestyle BMX\\1.00.000\\\0", b"SRCDISK\0",
              b"\\bin\\movie\\acclaim.avi\0", b"assets\\shadowMap.Bmp\0", b"ShadowMap\0"):
        assert s in img, s


# ------------------------------------------------------------------ 3. behaviour (ports vs dmbmx)
def game_key_bmp(data):
    """Port of zrwTxr_ReadBmp + zrwTxr_ApplyColorKey on a .bmp: 4/8-bit images key their palette
    entries, truecolour images their pixels; RGB (255, 0, 255) -> RGBA 0, 0, 0, 0. Returns top-down
    RGBA bytes."""
    from dmbmx.convert import imaging
    i = imaging.bmp_info(data)
    w, h, bpp = i.width, i.height, i.bpp
    out = bytearray()
    if bpp in (4, 8):
        pal = []
        for c in i.palette:
            rgb = bytes(c)[:3]
            pal.append(b"\0\0\0\0" if rgb == b"\xff\x00\xff" else rgb + b"\xff")
    for row in range(h):
        y = row if i.top_down else h - 1 - row
        r = data[i.off_bits + y * i.row_bytes: i.off_bits + (y + 1) * i.row_bytes]
        for x in range(w):
            if bpp == 24:
                px = bytes((r[3 * x + 2], r[3 * x + 1], r[3 * x])) + b"\xff"
                out += b"\0\0\0\0" if px[:3] == b"\xff\x00\xff" else px
            elif bpp == 8:
                out += pal[r[x]] if r[x] < len(pal) else b"\0\0\0\xff"
            else:
                v = (r[x >> 1] >> (4 * (1 - (x & 1)))) & 0xF
                out += pal[v] if v < len(pal) else b"\0\0\0\xff"
    return bytes(out)


def test_colour_key_port_agrees_with_dmbmx():
    from dmbmx.formats import mati, texture, zmd
    files = corpus_files(".zmd") + [p for p in corpus_files("._sd") if p.name.lower() == "mat._sd"]
    if not files:
        pytest.skip("no corpus")
    n = 0
    for p in files:
        data = p.read_bytes()
        if not data:
            continue
        mats = zmd.read(data).materials() if p.suffix.lower() == ".zmd" else mati.MatSd.read(data).items
        for m in mats:
            if m.kind != "bmp":
                continue
            ours = game_key_bmp(m.image)
            theirs = texture.apply_color_key(texture.decode_bmp(m.image)).rgba
            assert ours == theirs, (p.name, m.name)
            n += 1
    assert n > 800


def test_model_load_port_agrees_with_dmbmx():
    """zrwModelChunkMLOD / zrwModelReadMDEF / zrwModelLoadClump on every ZMD: at most 4 LODs, the LOD
    distance is MLDF word 1 as a float or the default, MDEF flag 2 (skinned: RpBone) is set exactly
    for the models whose geometries carry the Bone plugin, and the entry table is (name[16], id)."""
    from dmbmx.formats import zmd
    files = corpus_files(".zmd")
    if not files:
        pytest.skip("no corpus")
    defaults = (0.0, 50.0, 100.0, 150.0)
    n = long16 = 0
    for p in files:
        data = p.read_bytes()
        if not data:
            continue
        z = zmd.read(data)
        assert 1 <= len(z.lods) <= 4, p.name
        for k, lod in enumerate(z.lods):
            dist = struct.unpack("<f", struct.pack("<I", lod.mldf[1]))[0]
            assert (defaults[k] if dist == 0.0 else dist) == defaults[k], p.name   # the corpus uses the defaults
        skinned = any(a.geometry.bones for a in z.lods[0].clump.atomics)
        assert bool(z.mdef.kind & 2) == skinned, p.name
        assert z.mdef.kind & ~0x02 == 0, p.name                                    # no other MDEF flag is used
        for e in z.mdef.entries:   # zrwModelReadMDEF forces name[15] = 0: 16-character names lose a letter
            long16 += len(e.name.encode("latin-1")) == 16
            assert len(e.name.encode("latin-1")) <= 16
        n += 1
    assert n == 115 and long16 > 0


def test_line_test_port_agrees_with_brute_force():
    """zrwColLineTest on the world sectors: ZGE -> RW (x * s + o.x, o.y - y * s, o.z - z * s), walk every
    sector's collision tree (dmbmx.formats.rwcoll.line_test = RW 0x004BE560), keep the nearest hit
    (zrwColWorldLineCB). Same nearest triangle and t as brute force, on vertical drops over a level."""
    from dmbmx.formats import rwcoll, rwworld
    bsp = GAME_DIR / "assets" / "WoodwardDirt" / "BSP._sd"
    if not bsp.is_file():
        pytest.skip("no corpus")
    w = rwworld.World.read(bsp.read_bytes())
    secs = [sec for sec in w.sectors() if sec.coll]     # RW walks the sectors that have a tree
    o, s = w.origin, 1.0
    pts = [v for sec in secs for v in sec.vertices]
    lo = [min(p[i] for p in pts) for i in range(3)]
    hi = [max(p[i] for p in pts) for i in range(3)]

    def t_of(sec, t, a, b):
        _, i, j, k = sec.triangles[t]
        V = sec.vertices
        e1 = [V[j][q] - V[i][q] for q in range(3)]
        e2 = [V[k][q] - V[i][q] for q in range(3)]
        d = [b[q] - a[q] for q in range(3)]
        pv = (d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0])
        det = sum(e1[q] * pv[q] for q in range(3))
        tv = [a[q] - V[i][q] for q in range(3)]
        qv = (tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0])
        return sum(e2[q] * qv[q] for q in range(3)) / det

    def nearest(cands):
        best = None
        for sec, a, b, tris in cands:
            for t in rwcoll.segment_hits(sec, a, b, tris):
                f = t_of(sec, t, a, b)
                if best is None or f < best[0]:
                    best = (f, frozenset(sec.vertices[v] for v in sec.triangles[t][1:]))
        return best

    rng = random.Random(59)
    hits = 0
    for _ in range(40):
        zx, zz = rng.uniform(lo[0] - o[0], hi[0] - o[0]), -rng.uniform(lo[2] - o[2], hi[2] - o[2])
        za, zb = (zx, -(hi[1] - o[1]) - 1.0, zz), (zx, -(lo[1] - o[1]) + 1.0, zz)   # top -> bottom, ZGE y down
        a = (za[0] * s + o[0], o[1] - za[1] * s, o[2] - za[2] * s)
        b = (zb[0] * s + o[0], o[1] - zb[1] * s, o[2] - zb[2] * s)
        walked = nearest([(sec, a, b, rwcoll.line_test(sec, a, b)) for sec in secs])
        brute = nearest([(sec, a, b, None) for sec in secs])
        assert (walked is None) == (brute is None)
        if walked:
            assert walked[1] == brute[1] and abs(walked[0] - brute[0]) < 1e-6
            hits += 1
    assert hits > 10

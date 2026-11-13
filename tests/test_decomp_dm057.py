"""DM-057 self-consistency: decomp of the NgIF layer (NgApp, NgIFfx, NgIFpsx, NgIFzactor, NgIFzbsp,
NgIFzcollide, NgIFzres, NgIFzroller, MissingZGE, the rest of NgIfzmemDMM).

No C compiler on the build PC, so the C is checked the way DM-041..DM-055 do it:
1. structure: every function of the ten files drafted, names agree with decomp/functions.csv and
   decomp/symbols/symbols.csv (the last row wins), no Ghidra artefacts, no NgIF*_xxxxxxxx / FUN_ names of
   these VAs left anywhere in decomp/src, the ngif.h types are
   size-checked in C, in ghidra_types.txt and have struct rows; NgColFace / NgPropCol offsets;
2. exe facts: every assert (message, file, line) and every "%s %d %s" stub print (text, file,
   line) of the C is a call site in NgBMX.exe; the arm table read from NgIFzactor_ApplyMotionPose
   equals dmbmx.formats.motion.ARM_FIX; the channel lists and mirror table equal motion.py; the
   zfx type tables and the rail / image tables match the C;
3. behaviour: a Python port of the collision layer's tag lookup (code rule, flag remap, the
   spine-face edge link, the tagged-edge queries) agrees with dmbmx on all 12 levels' Tag._sd.
"""
import csv
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR, corpus_files
from dmbmx.formats import motion as M
from dmbmx.formats import physics as P

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
NG = "zaxis/Bmx/Src/com/zge/NgIF/"
COUNTS = {"MissingZGE.c": 8, "NgApp.c": 28, "NgIFfx.c": 31, "NgIFpsx.c": 39, "NgIFzactor.c": 12,
          "NgIFzbsp.c": 137, "NgIFzcollide.c": 17, "NgIfzmemDMM.c": 11, "NgIFzres.c": 19, "NgIFzroller.c": 92}
MACROS = {"MissingZGE.c": "MISSINGZGE_SRC", "NgApp.c": "NGAPP_SRC", "NgIFfx.c": "NGIFFX_SRC",
          "NgIFpsx.c": "NGIFPSX_SRC", "NgIFzactor.c": "NGIFZACTOR_SRC", "NgIFzbsp.c": "NGIFZBSP_SRC",
          "NgIFzcollide.c": "NGIFZCOLLIDE_SRC", "NgIFzres.c": "NGIFZRES_SRC", "NgIFzroller.c": "NGIFZROLLER_SRC"}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")
B = 0x400000


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    out = []
    for m in VA_RE.finditer((SRC / NG / f).read_text(encoding="utf-8")):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == kind:
                out[int(r["address"], 16) if kind != "struct" else r["name"]] = r
    return out


# ------------------------------------------------------------------ structure
@pytest.mark.parametrize("f", sorted(COUNTS))
def test_every_function_drafted_and_named_consistently(f):
    rows = [r for r in csv_rows() if r["file"] == NG + f]
    assert len(rows) == COUNTS[f], (f, len(rows))
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    for va, size, status, name in cf:
        r = by_va[va]
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, (hex(va), syms[va]["name"], name)
        if f != "NgIfzmemDMM.c" or name == "zmemDMMStats":
            assert syms[va]["source_file"] == "Bmx/Src/com/zge/NgIF/" + f, hex(va)
    t = (SRC / NG / f).read_text(encoding="utf-8")
    assert "TODO(decomp)" not in t
    # the header comment states the function count
    if f != "NgIfzmemDMM.c":
        assert re.search(r"\* %d functions, VA 0x" % COUNTS[f], t), f


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+|h13_[0-9a-f]{8})\b")
    for f in COUNTS:
        for n, line in enumerate((SRC / NG / f).read_text(encoding="utf-8").splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"
    # no old-style names of this layer anywhere in decomp/src (the front-end files included)
    old = re.compile(r"\b(NgIF\w*_[0-9a-fA-F]{8}|NgApp_[0-9a-fA-F]{8}|MissingZGE_[0-9a-fA-F]{8}|"
                     r"FUN_004(5[a-f]|6[0-9a-d])[0-9a-fA-F]{4}|game_OpenAssetLibraries)\b")
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        m = old.search(p.read_text(encoding="utf-8"))
        assert m is None, (p, m and m.group(0))


def test_gap_files_absorbed():
    for g in ("gap_zoverii__MissingZGE", "gap_MissingZGE__NgApp", "gap_NgApp__NgIFfx", "gap_NgIFfx__NgIFpsx",
              "gap_NgIFpsx__NgIFzactor", "gap_NgIFzactor__NgIFzbsp", "gap_NgIFzres__NgIFzroller",
              "gap_NgIFzroller__winChan"):
        assert not (SRC / "_unassigned" / (g + ".c")).exists(), g


def ngif_structs():
    t = (INC / "ngif.h").read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    return re.findall(r"\} (\w+);\s+/\* 0x([0-9A-F]+)", t) + re.findall(r"\} (\w+);\s+/\* (\d+)[ ,]", t)


def test_ngif_types_checked_everywhere():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert "ngif.h" in types and types.index("ngif.h") > max(types.index(h) for h in
                                                             ("fx.h", "physics.h", "bmxprop.h", "zactor.h",
                                                              "zoverii.h", "zprim.h", "ztaginf.h"))
    t = (INC / "ngif.h").read_text(encoding="utf-8")
    structs = last_symbols("struct")
    names = re.findall(r"^\} (\w+);", re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t,
                                                flags=re.S), flags=re.M)
    assert len(names) == 31
    for n in names:
        assert n in want, n
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n
        assert structs[n]["card"] == "DM-057" and structs[n]["type"] == "decomp/include/ngif.h", n
        # the size comment after the typedef agrees
        m = re.search(r"\} %s;\s+/\* (0x[0-9A-F]+|\d+)\b" % n, t)
        assert m and int(m.group(1), 0) == want[n], n


def offsets(header, struct_name):
    """+0x.. comments of a struct's fields (the authors' layout claims)."""
    t = (INC / header).read_text(encoding="utf-8")
    body = re.search(r"typedef struct %s \{(.*?)\} %s;" % (struct_name, struct_name), t, re.S).group(1)
    return {m.group(2): int(m.group(3), 16)
            for m in re.finditer(r"^\s*[\w\s\*]+?[\s\*](\*?)(\w+)(?:\[[^\]]*\])*;\s*/\* \+0x([0-9A-Fa-f]+)", body, re.M)}


def test_completed_offsets():
    face = offsets("physics.h", "NgColFace")
    assert (face["vtx"], face["tagFace"], face["prop"], face["v0"], face["ivtx"], face["rwTri"]) == \
        (0x1C, 0x28, 0x2C, 0x30, 0x60, 0x84)
    col = offsets("bmxprop.h", "NgPropCol")
    assert (col["defIndex"], col["flags"], col["rails"], col["unk_28"], col["game"], col["prop"]) == \
        (0, 2, 0x24, 0x28, 0x1EC, 0x1F0)
    t = (INC / "bmxprop.h").read_text(encoding="utf-8")
    flags = dict(re.findall(r"#define (NGPROPCOL_\w+)\s+(0x[0-9A-F]+)u", t))
    assert {k: int(v, 16) for k, v in flags.items()} == {
        "NGPROPCOL_GROUP": 0x1F, "NGPROPCOL_MOVING": 0x40, "NGPROPCOL_OFF": 0x80, "NGPROPCOL_FLAG100": 0x100,
        "NGPROPCOL_HIDDEN": 0x200, "NGPROPCOL_DRIVEN": 0x400, "NGPROPCOL_OBJI4": 0x800, "NGPROPCOL_OBJI5": 0x1000}


# ------------------------------------------------------------------ exe
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def str_addrs(img, s):
    return [m.start() + B for m in re.finditer(re.escape(s.encode("latin-1") + b"\0"), img)]


def c_string(s):
    return s.encode("latin-1").decode("unicode_escape")


def file_range(f):
    rows = [r for r in csv_rows() if r["file"] == NG + f]
    lo = min(int(r["va"], 16) for r in rows)
    hi = max(int(r["va"], 16) + int(r["size"]) for r in rows)
    return lo, hi


@pytest.mark.parametrize("f", sorted(MACROS))
def test_assert_and_stub_sites_exist_in_exe(img, f):
    t = (SRC / NG / f).read_text(encoding="utf-8")
    path = c_string(re.search(r'#define %s "([^"]+)"' % MACROS[f], t).group(1))
    p_addrs = str_addrs(img, path)
    assert p_addrs, path
    lo, hi = file_range(f)
    code = img[lo - B:hi - B]
    fmt = struct.pack("<I", str_addrs(img, "%s %d %s\n")[0])
    asserts = [(c_string(m.group(1)), int(m.group(2), 16))
               for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", %s, (0x[0-9A-Fa-f]+)' % MACROS[f], t)]
    stubs = [(int(m.group(1), 0), c_string(m.group(2)))
             for m in re.finditer(r'NGIF_MISSING\(%s, (0x[0-9A-Fa-f]+|\d+), "((?:[^"\\]|\\.)*)"\)' % MACROS[f], t)]
    assert asserts or stubs
    for msg, line in asserts:
        pats = [push(line) + b"\x68" + struct.pack("<I", pa) + b"\x68" + struct.pack("<I", a)
                for a in str_addrs(img, msg) for pa in p_addrs]
        assert any(p in code for p in pats), (f, msg, hex(line))
    for line, msg in stubs:
        pats = [b"\x68" + struct.pack("<I", a) + push(line) + b"\x68" + struct.pack("<I", pa) + b"\x68" + fmt
                for a in str_addrs(img, msg) for pa in p_addrs]
        assert any(p in code for p in pats), (f, msg, line)
    if f == "NgIFzbsp.c":
        assert len(stubs) == 113
    if f == "NgIFzroller.c":
        assert len(stubs) >= 48


def disasm(img, va, size):
    cs = pytest.importorskip("capstone")
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    return list(md.disasm(img[va - B:va - B + size], va))


def c_table(name, f):
    t = (SRC / NG / f).read_text(encoding="utf-8")
    body = re.search(r"%s[^=]*= \{(.*?)\};" % re.escape(name), t, re.S).group(1)
    return body


def test_arm_table_matches_motion(img):
    """NgIFzactor_ApplyMotionPose builds its arm table on the stack: emulate the stores."""
    regs, sp, words = {}, 0, {}
    for i in disasm(img, 0x004654B0, 0x90):
        ops = i.op_str
        if i.mnemonic == "call":
            break
        if i.mnemonic == "sub" and ops.startswith("esp, "):
            sp -= int(ops.split(", ")[1], 0)
        elif i.mnemonic == "push":
            sp -= 4
        elif i.mnemonic == "mov" and re.match(r"e?[a-d]x|e[sd]i|ebp|[a-d]x|bp|si|di", ops.split(",")[0]) and \
                re.match(r"^\w+, (0x[0-9a-f]+|\d+)$", ops):
            r, v = ops.split(", ")
            regs[r[-2:]] = int(v, 0) & 0xFFFF
        elif i.mnemonic == "xor":
            a, b = ops.split(", ")
            if a == b:
                regs[a[-2:]] = 0
        m = re.match(r"word ptr \[esp \+ (0x[0-9a-f]+|\d+)\], (\w+)$", ops)
        if i.mnemonic == "mov" and m:
            src = m.group(2)
            v = int(src, 0) if re.match(r"0x|\d", src) else regs[src[-2:]]
            words[sp + int(m.group(1), 0)] = v
    nz = sorted(a for a, v in words.items() if v)
    assert len(nz) == 4
    base = nz[0] - 4                              # the first non-zero is index 2
    exe = [tuple(words.get(base + 8 * k + 2 * j, 0) for j in range(3)) for k in range(4)]
    assert all(words.get(base + 2 * i) == 0 for i in range(16) if i % 4 != 2 and i % 4 != 3), words
    body = c_table("s_armEuler[4][4]", "NgIFzactor.c")
    c = [tuple(int(x, 0) & 0xFFFF for x in r.split(",")[:3]) for r in re.findall(r"\{([^{}]*)\}", body)]
    assert exe == c == [(0, 0, 0x4000), (0, 0, 0xC000), (0, 0, 0xC000), (0, 0, 0x4000)]
    fix = re.findall(r'\{ "(\w+)",\s*(-?\d+), (\d+) \}', c_table("s_armFix[6]", "NgIFzactor.c"))
    assert len(fix) == 6
    euler = [(e[0], e[1], e[2] - 0x10000 if e[2] >= 0x8000 else e[2]) for e in c]
    for name, pre, post in fix:
        want_pre, want_post = M.ARM_FIX[name]
        got_post = M.euler_to_quat(euler[int(post)])
        assert got_post == pytest.approx(want_post, abs=1e-6), name
        if int(pre) < 0:
            assert want_pre is None, name
        else:
            assert M.euler_to_quat(euler[int(pre)]) == pytest.approx(want_pre, abs=1e-6), name
    assert set(n for n, _, _ in fix) == set(M.ARM_FIX)


def pushed_strings(img, va, size):
    out = []
    for i in disasm(img, va, size):
        if i.mnemonic == "push" and i.op_str.startswith("0x"):
            a = int(i.op_str, 16)
            if 0x00550000 <= a < 0x00570000:
                s = img[a - B:a - B + 32]
                out.append(s[:s.index(b"\0")].decode())
    return out


def test_channel_lists_match_motion(img):
    assert tuple(pushed_strings(img, 0x00465130, 410)) == M.RIDER_CHANNELS
    assert tuple(pushed_strings(img, 0x00465340, 280)) == M.BIKE_CHANNELS
    assert tuple(pushed_strings(img, 0x00464740, 477)) == M.RIDER_CHANNELS       # part frames, same order
    assert tuple(pushed_strings(img, 0x004649A0, 326)) == M.BIKE_CHANNELS
    mirror = {}
    for i in disasm(img, 0x00465130, 410):
        m = re.match(r"dword ptr \[esi \+ (0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$", i.op_str)
        if i.mnemonic == "mov" and m and 0x228 <= int(m.group(1), 16) < 0x274:
            mirror[(int(m.group(1), 16) - 0x228) // 4] = int(m.group(2), 0)
    assert tuple(mirror[k] for k in sorted(mirror)) == M.MIRROR
    t = (SRC / NG / "NgIFzactor.c").read_text(encoding="utf-8")
    names = re.findall(r'"(\w+)"', re.search(r"s_riderParts\[RIDER_PARTS\] = \{(.*?)\};", t, re.S).group(1))
    assert tuple(names) == M.RIDER_CHANNELS
    names = re.findall(r'"(\w+)"', re.search(r"s_bikeParts\[BIKE_PARTS\] = \{(.*?)\};", t, re.S).group(1))
    assert tuple(names) == M.BIKE_CHANNELS
    mm = re.search(r"s_riderMirror\[RIDER_PARTS\] = \{([^}]*)\}", t).group(1)
    assert tuple(int(x) for x in mm.split(",")) == M.MIRROR


def test_tables_match_exe(img):
    names = {int(r["va"], 16): r["name"] for r in csv_rows()}
    thunks = {}
    a = 0x00401005
    while img[a - B] == 0xE9:
        thunks[a] = a + 5 + struct.unpack_from("<i", img, a - B + 1)[0]
        a += 5
    # the zfx type tables: spawn / update / draw slots point at the NgIFfx functions
    want = {0x0055C278: ("zfxSparkSpawn", None, "zfxSparkDraw"), 0x0055C2B0: ("zfxShardSpawn", None, "zfxShardDraw"),
            0x0055C2E8: ("zfxSparkSpawn", None, "zfxSplashDraw"), 0x0055C320: ("zfxDustSpawn", None, "zfxDustDraw"),
            0x0055C358: ("zfxPosterSpawn", None, "zfxPosterDraw"),
            0x0055C390: ("zfxRippleSpawn", "zfxRippleUpdate", "zfxRippleDraw"),
            0x0055C3C8: ("zfxLightningSpawn", None, "zfxLightningDraw")}
    for va, (spawn, update, draw) in want.items():
        slots = [thunks.get(v, v) for v in struct.unpack_from("<14I", img, va - B)]
        assert names[slots[6]] == spawn and names[slots[8]] == draw, hex(va)
        assert (names.get(slots[7]) if update else slots[7]) == (update or 0x004954B0), hex(va)
    assert names[thunks[struct.unpack_from("<I", img, 0x0055C358 + 0x10 - B)[0]]] == "zfxPosterCreate"
    # the box rails and the edge rail
    body = c_table("s_boxRails[4][3]", "NgIFzcollide.c")
    rails = [tuple(int(x) for x in r.split(",")) for r in re.findall(r"\{([^{}]*)\}", body)]
    assert [tuple(img[0x0055D4F8 - B + 3 * k:0x0055D4F8 - B + 3 * k + 3]) for k in range(4)] == rails
    t = (SRC / NG / "NgIFzcollide.c").read_text(encoding="utf-8")
    edge = tuple(int(x) for x in re.search(r"s_edgeRail\[3\] = \{([^}]*)\}", t).group(1).split(","))
    assert tuple(img[0x0055D504 - B:0x0055D507 - B]) == edge == (0, 1, 1)
    # the overlay image resources
    body = c_table("g_zresImages[11]", "NgIFzres.c")
    defs = [tuple(int(x, 0) for x in r.split(",")) for r in re.findall(r"\{([^{}]*)\}", body)]
    assert [struct.unpack_from("<iiHHI", img, 0x0055D978 + 16 * k - B)[:3] + struct.unpack_from(
        "<I", img, 0x0055D978 + 16 * k + 12 - B) for k in range(11)] == [(d[0], d[1], d[2], d[3]) for d in defs]
    # the bike models
    body = c_table("g_zrollerBikeModels[49]", "NgIFzroller.c")
    models = re.findall(r'"(\w+)"', body)
    exe = []
    for k in range(49):
        p = struct.unpack_from("<I", img, 0x0055DC80 + 4 * k - B)[0]
        s = img[p - B:p - B + 16]
        exe.append(s[:s.index(b"\0")].decode())
    assert models == exe
    # the PSX-compat memory-card stubs: 17 functions 0x20 apart, "ret" / "xor eax, eax; ret"
    for k in range(17):
        va = 0x0046D720 + 0x20 * k
        assert img[va - B:va - B + 1] == b"\xc3" or img[va - B:va - B + 3] == b"\x33\xc0\xc3", hex(va)
        assert names[va].startswith("zcard"), hex(va)


def test_propcol_flag_readers(img):
    """The NgPropCol flag answers: the knocked flag (0x80) is set only by zbspPropSetMode; the
    OBJI flags 0x400 / 0x800 / 0x1000 have no reader (only bmxpropInit's set of 0x400)."""
    rows = [r for r in csv_rows() if r["file"]]
    seen = []
    for r in rows:
        va, n = int(r["va"], 16), int(r["size"])
        for i in disasm(img, va, n):
            s = "%s %s" % (i.mnemonic, i.op_str)
            if re.match(r"(test|and|or) byte ptr \[e\w\w \+ 3\], (4|8|0x10)$", s) or \
                    re.match(r"(test|or) byte ptr \[e\w\w \+ 2\], 0x80$", s):
                seen.append((r["name"], s))
    names = {n for n, _ in seen}
    assert ("zbspPropSetMode", "or byte ptr [esi + 2], 0x80") in seen
    assert ("bmxpropInit", "or byte ptr [edi + 3], 4") in seen
    # the GUI hits are GuiElement flags (DM-056), not NgPropCol
    assert names <= {"zbspPropSetMode", "bmxpropInit", "guiSetHighlight", "guiElementDraw",
                     "guiElementHandler"}   # DM-064: 0x00439FA1 is part of guiElementHandler, seen


# ------------------------------------------------------------------ behaviour
def tag_files():
    files = [p for p in corpus_files("._sd") if p.name.lower() == "tag._sd"]
    if not files:
        pytest.skip("no Tag._sd corpus")
    return files


def collide_code(tag):
    """Port of zcolSegmentTest / zbspColSphere: the face code physics sees."""
    code = tag.code
    if tag.extra == 0 and (code & 0xF000) == 0x1000:
        code &= 0xFFF
    return code


def collide_flags(b):
    """Port of the C's flag remap (zcolSegmentTest)."""
    return ((((b & 1) << 1 | (b & 0xC)) << 1 | (b & 2)) << 2) & 0xFF


def test_tag_lookup_port_agrees_with_dmbmx():
    from dmbmx.formats.level import TagSd

    files = tag_files()
    assert len(files) == 12
    spines = classes = 0
    for p in files:
        tag = TagSd.read(p.read_bytes())
        faces = tag.faces
        edges = [(i, k, e) for i, f in enumerate(faces) for k, e in enumerate(f.edge_tags) if e is not None]
        for f in faces:
            if f.tag is None:
                continue
            code = collide_code(f.tag)
            assert collide_flags(f.tag.flags) == P.face_flags(f.tag.flags)
            cls = P.tag_class(code)
            assert cls == 0 or cls in P.FACE_CLASSES or cls >= 11
            if (f.tag.code & 0xF000) == 0x1000:
                assert code == P.tag_id(f.tag.code)          # extra is 0 in the corpus
            # ngappLinkSpineFaces / zbspGetTaggedEdge: class 2/3 ids name an edge code
            if cls in (2, 3):
                assert any(e.code == P.tag_id(code) for _, _, e in edges), (p, hex(code))
                spines += 1
            classes += 1
        # zbspFindTaggedEdges(0, 0xFFFF) keeps the edges with a kind
        port = [e for _, _, e in edges if e.kind != 0]
        assert len(port) == sum(1 for f in faces for e in f.edge_tags if e is not None and e.kind)
    assert spines == 1591 + 106 and classes > 0

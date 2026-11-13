"""Self-consistency checks for the decomp of action.c, contrler.c and camera.c (controls, game loop, camera).

1. structure: the 27 functions are drafted (VA order, names = functions.csv = symbols.csv), the camera
   gap is gone, no Ghidra artefacts or old names, struct layouts (contrler.h, camera.h) agree with
   their "+0x.." comments and ghidra_types.txt, the new cameraDrawNightVision is the callback
   cameraInit registers;
2. against the exe: every assert (message, file, line) is a call site, the debug messages exist, the
   action binding table, keyboard layouts, button names and camera tables equal what the C / docs say;
3. the control masks: contrlerUpdate's remap (read from its code) = contrler.c = comdata.c defaults =
   save_fields.py (offsets, bits) = the real save; the rider.c (action, status) pairs.
"""
import csv
import re
import struct
from pathlib import Path

import pytest

from conftest import corpus_files
from dmbmx.formats import save
from dmbmx.formats import save_fields as F

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
ACTION = "zaxis/Bmx/Src/com/action/"
# + 1 + 3 + 1 functions created later (thunk-only dead code)
FILES = {"action.c": (8, 5072, "action"), "camera.c": (13, 7160, "camera"), "contrler.c": (11, 1702, "contrler")}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")
B = 0x400000


def text(name):
    return (SRC / (ACTION + name)).read_text(encoding="utf-8")


def c_functions(name):
    out = []
    for m in VA_RE.finditer(text(name)):
        fn = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), fn))
    return out


def symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == kind:
                out[int(r["address"], 16) if kind != "struct" else r["name"]] = r     # later rows win
    return out


# ------------------------------------------------------------------ structure
def test_every_function_drafted_and_named_consistently():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    syms = symbols("func")
    for name, (count, size, prefix) in FILES.items():
        mine = [r for r in rows if r["file"] == ACTION + name]
        cf = c_functions(name)
        assert len(mine) == len(cf) == count, name
        assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in mine)
        by_va = {int(r["va"], 16): r for r in mine}
        for va, sz, status, fn in cf:
            r = by_va[va]
            assert status == "draft" and r["status"] == "draft", hex(va)
            assert int(r["size"]) == sz and r["name"] == fn, (hex(va), r["name"], fn)
            assert syms[va]["name"] == fn, hex(va)
            assert syms[va]["source_file"] == "Bmx/Src/com/action/" + name, hex(va)
            assert fn.startswith(prefix), fn
        assert sum(sz for _, sz, _, _ in cf) == size, name
        assert re.search(r"\* %d functions, %d bytes" % (count, size), text(name)), name
    assert not (SRC / "_unassigned" / "gap_camera__comdata.c").exists()


def test_no_ghidra_artefacts_and_old_names_gone():
    bad = re.compile(r"\b(undefined[1-8]?|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|[iu]Var\d+|pc?Var\d+|"
                     r"extraout_\w+|CONCAT\d+|thunk_\w+|switchD_\w+|FUN_004078c0|FUN_00407620|FUN_00407b70)\b")
    for name in FILES:
        t = text(name)
        assert "TODO(decomp)" not in t
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{name}:{n}: {line.strip()}"
    old = re.compile(r"\b(action_[0-9a-f]{8}|camera_[0-9a-f]{8}|contrler_[0-9a-f]{8})\b")
    for p in SRC.rglob("*.c"):
        code = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        assert not old.search(code), p


SIZES = {"uint8_t": (1, 1), "int8_t": (1, 1), "char": (1, 1), "uint16_t": (2, 2), "int16_t": (2, 2),
         "uint32_t": (4, 4), "int32_t": (4, 4), "int": (4, 4), "float": (4, 4),
         "RwV3d": (0xC, 4), "RwMatrix": (0x40, 4), "BmxInput": (0x28, 4)}


def layouts(*headers):
    """{struct: (size, [(field, offset, commented_offset)])} (x86, natural alignment)."""
    known = dict(SIZES)
    out = {}
    for header in headers:
        t = (INC / header).read_text(encoding="utf-8")
        t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
        for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
            off, fields, maxal = 0, [], 1
            for line in m.group(2).splitlines():
                d = re.match(r"\s*(?:const\s+)?(\w+)\s*(\*?)\s*(\w+)((?:\[\w+\])*);\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?",
                             line)
                if not d:
                    continue
                typ, ptr, name, dims, comment = d.groups()
                size, align = (4, 4) if ptr else known[typ]
                count = 1
                for k in re.findall(r"\[(\w+)\]", dims):
                    count *= int(k, 0)
                off = (off + align - 1) // align * align
                fields.append((name, off, int(comment, 16) if comment else None))
                off += size * count
                maxal = max(maxal, align)
            size = (off + maxal - 1) // maxal * maxal
            out[m.group(3)] = (size, fields)
            known[m.group(3)] = (size, maxal)
    return out


def test_struct_layouts():
    lay = layouts("contrler.h", "camera.h")
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    names = ("BmxActionBinding", "ZInputDevice", "BmxPad", "BmxCamParams", "BmxCamera", "BmxCamView")
    assert set(names) <= set(lay)
    for n in names:
        size, fields = lay[n]
        assert size == want[n], (n, hex(size))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (n, fname, hex(off), hex(comment))
        h = "contrler.h" if n in names[:3] else "camera.h"
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), (INC / h).read_text(encoding="utf-8")), n
        assert symbols("struct")[n]["type"] == "decomp/include/" + h
    types = (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    assert types.index("rider.h") < types.index("contrler.h") < types.index("camera.h")
    # the pad holds the rider's input block at +0x10 (riderReadInput copies it)
    assert dict((f, o) for f, o, _ in lay["BmxPad"][1])["input"] == 0x10
    cam = dict((f, o) for f, o, _ in lay["BmxCamera"][1])
    assert (cam["view"], cam["shot"], cam["flags"], cam["pos"], cam["wantPos"], cam["look"], cam["fov"],
            cam["matrix"]) == (2, 3, 0xB, 0x10, 0x20, 0x40, 0x70, 0x78)


# ------------------------------------------------------------------ against the exe
def load_image(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def u32(img, va):
    return struct.unpack_from("<I", img, va - B)[0]


def cstr(img, va):
    e = img.index(b"\0", va - B)
    return img[va - B:e].decode("latin-1")


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def test_assert_sites_exist_in_exe(game_dir):
    img = load_image(game_dir)
    n = 0
    for name, macro, path_va in (("action.c", "ACTION_SRC", 0x00551A64), ("camera.c", "CAMERA_SRC", 0x00551F88),
                                 ("contrler.c", "CONTRLER_SRC", 0x00554624)):
        t = text(name)
        path = re.search(r'#define %s "([^"]+)"' % macro, t).group(1).replace("\\\\", "\\")
        assert img.find(path.encode() + b"\0") + B == path_va, name
        for msg, line in re.findall(r'zAssertFail\("([^"]+)", %s, (0x[0-9A-Fa-f]+)' % macro, t):
            m_addr = img.find(msg.encode() + b"\0") + B
            assert m_addr > B, msg
            pat = push(int(line, 16)) + b"\x68" + struct.pack("<I", path_va) + b"\x68" + struct.pack("<I", m_addr)
            assert pat in img, (name, msg, line)
            n += 1
    assert n == 5 + 1 + 7


def test_debug_messages_exist_in_exe(game_dir):
    img = load_image(game_dir)
    msgs = re.findall(r'(?:g_pfnDebugPrintf|zconsoleLog)\("((?:[^"\\]|\\.)*)"', text("action.c"))
    assert len(msgs) == 6
    for m in msgs:
        a = img.find(m.replace("\\n", "\n").encode() + b"\0")
        assert a > 0 and struct.pack("<I", a + B) in img, m


def expected_actions():
    """g_contrlerActions [config][action][status] = (mask, test), as docs/re/input-camera.md 3 describes."""
    X, SQ, CI, TRI, UP, DOWN = 0x80, 0x20, 0x40, 0x10, 0x1, 0x2
    NEVER, ALWAYS = (0, 7), (0, 6)

    def btn(m, held=2):
        return [(m, 0), (m, held), (m, 4)]
    base = {0: [(X, 4), NEVER, NEVER], 2: btn(DOWN), 3: btn(SQ), 4: btn(CI), 5: [(X, 2), (X, 2), (X, 4)],
            6: btn(X), 7: [(TRI, 0), (TRI, 2), NEVER], 8: [(X, 0), ALWAYS, NEVER], 9: [(UP, 0), NEVER, NEVER],
            10: btn(0x500, 3), 11: btn(0xA00, 3), 16: [(0xF0, 1), ALWAYS, NEVER], 17: [NEVER] * 3,
            18: btn(CI), 19: btn(TRI), 20: btn(SQ)}
    for a in (12, 13, 14, 15):
        base[a] = [NEVER] * 3
    out = []
    for cfg in range(3):
        t = {a: list(v) for a, v in base.items()}
        if cfg == 0:
            t[1] = [(X, 0), (UP, 2), NEVER]
        elif cfg == 1:
            t[1] = btn(X)
            t[6] = [(X, 4)] * 3
        else:
            t[1] = btn(UP)
            t[10], t[11] = btn(0x100), btn(0x200)
            for a in (12, 13, 14, 15):
                t[a] = btn(0)
        out.append([t[a] for a in range(21)])
    return out


def test_action_binding_table(game_dir):
    img = load_image(game_dir)
    got = [[[struct.unpack_from("<II", img, 0x00553EE0 - B + ((c * 21 + a) * 3 + s) * 8) for s in range(3)]
            for a in range(21)] for c in range(3)]
    assert got == expected_actions()
    assert u32(img, 0x00553EE0 + 3 * 21 * 3 * 8) == 0          # the table ends there
    hdr = (INC / "contrler.h").read_text(encoding="utf-8")
    ids = {k: int(v) for k, v in re.findall(r"\b(ACT_[A-Z_0-9]+)\s*=\s*(\d+)", hdr)}
    assert (ids["ACT_JUMP"], ids["ACT_GRIND"], ids["ACT_TURNAROUND"], ids["ACT_COUNT"], ids["ACT_RELEASED"]) == (6, 7, 9, 21, 2)
    # contrlerTestAction's asserts: action <= 20, status <= 2, config <= 2
    assert "if (action > 20)" in text("contrler.c") and "if (status > 2)" in text("contrler.c")


def test_pad_button_names_and_keymaps(game_dir):
    img = load_image(game_dir)
    names = tuple(cstr(img, u32(img, 0x0055A3F4 + 4 * i)) for i in range(18))
    assert names == F.PAD_BUTTON_NAMES
    hdr = (INC / "contrler.h").read_text(encoding="utf-8")
    for bit, name in enumerate(names):
        assert re.search(r'= 0x%05X,\s*/\* "%s"' % (1 << bit, re.escape(name)), hdr), name
    layouts = [cstr(img, u32(img, 0x0055A43C + 4 * i)) for i in range(5)]
    assert layouts == [f"< KEYBOARD LAYOUT {i} >" for i in range(1, 6)]

    def keymap(va):
        out, p = [], va
        while img[p - B] != 0xFF:
            out.append((img[p - B], img[p - B + 1]))
            p += 2
        return out
    pad0 = keymap(0x00561E68)
    lay1 = keymap(0x0055BB98)
    # layout 1 = pcinput's default pad-0 map without the L2 / R2 keys ('3' -> ch 16, '4' -> ch 17)
    assert lay1 == [kc for kc in pad0 if kc[1] not in (16, 17)]
    assert dict(pad0)[0x12] == 8 and dict(pad0)[0x1F] == 9 and dict(pad0)[0x21] == 10 and dict(pad0)[0x20] == 11
    for k in range(5):                                          # 5 layouts of 32 bytes, all terminated
        assert 0xFF in img[0x0055BB98 - B + 32 * k:0x0055BB98 - B + 32 * (k + 1)]


def test_camera_tables(game_dir):
    img = load_image(game_dir)

    def floats(va, n):
        return [round(x, 4) for x in struct.unpack_from("<%df" % n, img, va - B)]
    assert [floats(0x00551E6C + 24 * v, 6) for v in range(4)] == [
        [0.06, 0.45, 0.7, 0.35, 0.12, 50.0], [0.06, 0.45, 0.75, 0.5, 0.12, 60.0],
        [0.02, 0.3, 1.0, 0.25, 0.12, 60.0], [0.16, 0.4, 1.0, 0.5, 0.12, 70.0]]
    assert floats(0x00551E0C, 6) == [0.008, 0.6, 1.0, 0.13, 0.12, 60.0]
    assert floats(0x00551ECC, 6) == [5.0, 250.0, 170.0, 275.0, 160.0, 125.0]
    offs = [floats(0x00551C54 + 16 * s, 3) for s in range(11)]
    assert {s for s in range(11) if any(offs[s])} == {2, 4, 6}
    assert (offs[2], offs[4], offs[6]) == ([-200.0, 100.0, 0.0], [-100.0, 700.0, 0.0], [150.0, 100.0, -200.0])
    # cameraSetShot's jump table: shots 1..10 -> case 0 (place the camera) for 1, 3, 5, 7-10
    jt = [u32(img, 0x00407AC8 + 4 * i) for i in range(2)]
    sel = list(img[0x00407AD0 - B:0x00407AD0 - B + 10])
    assert jt[1] == 0x00407AB5 and {s for s in range(1, 11) if sel[s - 1] == 0} == {1, 3, 5, 7, 8, 9, 10}
    # header comments carry the same numbers
    hdr = (INC / "camera.h").read_text(encoding="utf-8")
    for want in ("0x00551ECC 5:", "0x00551ED0 250:", "0x00551ED4 170:", "0x00551ED8 275:", "0x00551EDC 160:",
                 "0x00551EE0 125:"):
        assert want in hdr, want


def test_action_tables_and_new_function(game_dir):
    img = load_image(game_dir)
    t = text("action.c")
    runs = [tuple(img[0x00551A34 - B + 2 * k:0x00551A34 - B + 2 * k + 2]) for k in range(3)]
    assert runs == [(0, 4), (1, 0), (1, 1)] and "{ 0, 4 }, { 1, 0 }, { 1, 1 }" in t
    grp = list(img[0x00551FC4 - B:0x00551FC4 - B + 12])
    assert "{ %s }" % ", ".join(map(str, grp)) in t
    assert struct.unpack_from("<ii", img, 0x00551A3C - B) == (1, 1)
    # cameraDrawNightVision 0x004074C0: pushed by cameraInit (directly or through its thunk)
    targets = {0x004074C0}
    va = 0x00401005
    while img[va - B] == 0xE9:
        if va + 5 + struct.unpack_from("<i", img, va - B + 1)[0] == 0x004074C0:
            targets.add(va)
        va += 5
    init = img[0x004056C0 - B:0x004056C0 - B + 121]
    assert any(b"\x68" + struct.pack("<I", x) in init for x in targets)
    # its body: cheats_active bit 1 (byte 0x006955E2), 640 x 480, colour 0x70007000
    body = img[0x004074C0 - B:0x004074C0 - B + 68]
    assert b"\xf6\x05\xe2\x55\x69\x00\x02" in body and struct.pack("<I", 0x280) in body and \
        struct.pack("<I", 0x1E0) in body and struct.pack("<I", 0x70007000) in body


# ------------------------------------------------------------------ control masks <-> save_fields
def remap_from_code(img):
    """(profile offset, logical bit) pairs of contrlerUpdate's remap switch (0x004080F0)."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    ins = list(md.disasm(img[0x004080F0 - B:0x004080F0 - B + 519], 0x004080F0))
    base = None
    pairs = []
    for i, x in enumerate(ins):
        if x.mnemonic == "mov" and x.op_str.startswith("ebx, 0x6952"):
            base = int(x.op_str.split(",")[1], 16) - 0x006952A0     # profile + base
        if x.mnemonic == "or" and x.op_str.startswith("ebp, 0x"):
            m = re.match(r"eax, dword ptr \[ebx \+ (0x[0-9a-f]+)\]", ins[i - 4].op_str)
            if m:
                pairs.append((base + int(m.group(1), 16), int(x.op_str.split(",")[1], 16)))
    return sorted(pairs)


def test_control_masks_match_save_fields(game_dir):
    img = load_image(game_dir)
    pytest.importorskip("capstone")
    pairs = remap_from_code(img)
    want = sorted((save.PROFILE_CONTROLS_OFF + 4 * k, bit) for k, bit in enumerate(F.CONTROL_BITS))
    assert pairs == want                                        # control[k] feeds logical bit CONTROL_BITS[k]
    # contrler.c and comdata.c say the same
    m = re.search(r"contrlerRemapBits\[CONTRLER_REMAP_COUNT\] = \{\s*([^}]*)\}", text("contrler.c")).group(1)
    pad = {k: int(v, 16) for k, v in re.findall(r"\b(PAD_\w+)\s*=\s*(0x[0-9A-Fa-f]+)", (INC / "contrler.h").read_text(encoding="utf-8"))}
    assert tuple(pad[n.strip()] for n in m.split(",")) == F.CONTROL_BITS
    d = re.search(r"comdataDefaultControls\[7\] = \{([^}]*)\}", text("comdata.c")).group(1)
    assert tuple(int(v, 0) for v in d.split(",")) == F.CONTROL_BITS
    # save_fields: offsets and names
    for k in range(7):
        f, idx = F.resolve(f"profile.0.control.{k}")
        assert f.off(*idx) == 0x24 + 4 * k and f.tag == "V"
        assert F.label(f, F.CONTROL_BITS[k]) == F.PAD_BUTTON_NAMES[F.CONTROL_BITS[k].bit_length() - 1]
    for k in range(4):
        f, idx = F.resolve(f"profile.1.dpad.{k}")
        assert f.off(*idx) == 0x14 + 4 * k
    name = F.resolve("profile.0.name")[0]
    assert name.off + name.hi + 1 == 0x14                         # name (12 + NUL) ends where the bindings start
    assert F.resolve("profile.0.controller_config")[0].off == 2
    assert F.resolve("profile.0.keyboard_layout")[0].off == 0x4B
    # the CONTROLLER screen stores 11 dwords from profile + 0x14 (0x0044256B: mov ecx, 0x5745B4 / mov edx, 0xB)
    assert b"\xb9\xb4\x45\x57\x00" in img[0x0044256B - B:0x0044256B - B + 5]
    acc = img[0x0044256B - B:0x0044256B - B + 0x30]
    assert b"\xba\x0b\x00\x00\x00" in acc and b"\x88\x4e\x37" in acc    # 11 dwords; layout byte at +0x14 + 0x37 = +0x4B
    assert b"\x8d\x34\x95\xb4\x52\x69\x00" in img[0x004420A0 - B:0x004420A0 - B + 0x20]   # esi = profile + 0x14
    # contrlerTestAction bound check reads config from profile +2 (riderCreate 0x004316F2)
    assert b"\x8a\x04\x95\xa2\x52\x69\x00" in img[0x004316F2 - B:0x004316F2 - B + 7]


def test_real_save_controls(game_dir):
    files = corpus_files(".dat")
    if not files:
        pytest.skip("no save")
    s = save.read(files[0].read_bytes())
    for p in range(2):
        assert tuple(F.get(s, f"profile.{p}.control.{k}") for k in range(7)) == F.CONTROL_BITS
        assert F.get(s, f"profile.{p}.controller_config") in (0, 1, 2)
        assert [F.get(s, f"profile.{p}.dpad.{k}") for k in range(4)] == [0, 0, 0, 0]
        assert F.get(s, f"profile.{p}.keyboard_layout") in range(5)


# ------------------------------------------------------------------ rider.c actions
RIDER_ACTION_PAIRS = {  # (action, status): call sites in the exe (Ghidra, rider.c 0x00431xxx-0x00435xxx)
    (0, 0): 1, (1, 1): 2, (3, 0): 1, (3, 1): 1, (3, 2): 1, (4, 0): 2, (4, 1): 2, (4, 2): 2, (6, 1): 5, (6, 2): 5,
    (7, 1): 3, (8, 0): 1, (8, 2): 1, (9, 0): 1, (10, 1): 3, (11, 1): 1, (16, 0): 1, (17, 0): 1, (18, 1): 1,
    (19, 1): 2}


def test_rider_action_pairs(game_dir):
    hdr = (INC / "contrler.h").read_text(encoding="utf-8")
    ids = {k: int(v) for k, v in re.findall(r"\b(ACT_[A-Z_0-9]+)\s*=\s*(\d+)", hdr)}
    got = {}
    for a, s in re.findall(r"act\(r, (ACT_\w+), (ACT_\w+)\)", text("rider.c")):
        key = (ids[a], ids[s])
        got[key] = got.get(key, 0) + 1
    assert got == RIDER_ACTION_PAIRS
    img = load_image(game_dir)
    # the corrected site: push 0 (pressed); push 9 (TURNAROUND), then push 1; push 6 (JUMP held)
    lo, hi = 0x00430E70 - B, 0x004364FC - B                       # rider.c
    i = img.find(b"\x6a\x00\x6a\x09", lo, hi)
    assert i > 0 and img.count(b"\x6a\x00\x6a\x09", lo, hi) == 1
    assert img.find(b"\x6a\x01", i, i + 0x100) > 0 and img.find(b"\x6a\x06", i, i + 0x100) > 0
    assert img.find(b"\x6a\x01\x6a\x09", lo, hi) < 0                 # never "TURNAROUND held"


def test_camera_mode_callers():
    modes = set()
    for p in (SRC / ACTION.rstrip("/")).glob("*.c"):
        for m in re.findall(r"cameraSetMode\([^,]+, (\w+)\)", p.read_text(encoding="utf-8")):
            modes.add(m)
    hdr = (INC / "camera.h").read_text(encoding="utf-8")
    vals = {k: int(v) for k, v in re.findall(r"\b(CAM_MODE_\w+)\s*=\s*(\d+)", hdr)}
    used = {vals.get(m, int(m, 0) if m[0].isdigit() else None) for m in modes}
    assert used == {0, 1, 3, 4}                                # mode 2 is never set

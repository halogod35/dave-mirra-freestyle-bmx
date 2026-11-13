"""Self-consistency checks for the decomp of zactor.c (actors, motion bank, the motion.zal clip decoder)
and zprim.c (2D primitives).

No C compiler on the build PC, so the C is checked the way the other decomp tests do it:
1. structure: all 44 + 50 functions drafted, names agree with decomp/functions.csv and
   decomp/symbols/symbols.csv (the last row wins), no Ghidra artefacts and no old zactor_ names anywhere in
   decomp/src, the zactor.h / zprim.h layouts agree with their "+0x.." comments and the
   ghidra_types.txt sizes, every assert (message, file, line) is a call site in NgBMX.exe;
2. exe facts the C relies on: tables and constants, the decoder's reads of the clip header
   (bits it tests, fields it never reads), field displacements, globals private to the module;
3. behaviour: a literal Python port of zactorDecodeMotion (state blocks, byte-level record
   parsing, 34 ms clock, one record group per frame step, mirror mode) against
   dmbmx.formats.motion on every frame of all 238 clips, plain and mirrored, and on synthetic
   clips for the cases the bank does not exercise (8-bit angles, odd record times, a time-0 delta
   group, a section with flag 0x02, a stream that ends early).
"""
import csv
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR
from dmbmx.formats import motion as M

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
ZACTOR = "zaxis/Bmx/Src/com/zge/zactor.c"
ZPRIM = "zaxis/Bmx/Src/com/zge/zprim.c"
COUNTS = {ZACTOR: 85, ZPRIM: 73}   # 40 / 21 of them created by the clean-up
# zactorCloseMotionLibrary 0x00475000 (assert "Motions not opened") and the two bare rets
# 0x00479240 / 0x00479260 in zprim.c were created with the ZGE object model
ZOBJ_PASS_CREATED = {0x00475000, 0x00479240, 0x00479260}
RANGES = {ZACTOR: (0x00472630, 0x0047785D), ZPRIM: (0x004779E0, 0x0047C3C3)}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n((?:[^\n]+\n)+?)\{\n")
B = 0x400000


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    out = []
    for m in VA_RE.finditer((SRC / f).read_text(encoding="utf-8")):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbols(kind):
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == kind:
                out[int(r["address"], 16)] = r
    return out


# ------------------------------------------------------------------ structure
@pytest.mark.parametrize("f", [ZACTOR, ZPRIM])
def test_every_function_drafted_and_named_consistently(f):
    rows = [r for r in csv_rows() if r["file"] == f]
    assert len(rows) == COUNTS[f]
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    prefix = "zactor" if f == ZACTOR else "zprim"
    lo, hi = RANGES[f]
    for va, size, status, name in cf:
        r = by_va[va]
        assert lo <= va < hi
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name, hex(va)
        assert syms[va]["source_file"] == "Bmx/Src/com/zge/%s.c" % prefix
        assert re.match(prefix + r"[A-Z]\w+$", name), name
    # zactorInit came over from the winStrm / zactor gap
    assert not (SRC / "_unassigned" / "gap_winStrm__zactor.c").exists()


OLD_NAMES = re.compile(r"\b(zactor_[0-9a-fA-F]{8}|zprim_[0-9a-f]{8}|zactor_DecodeMotion|zactor_OpenMotionLibrary|"
                       r"zactor_LoadMotionBank|zasset_OpenMemLibrary|FUN_00472630)\b")


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+)\b")
    allowed_fun = {"psx2DSetClip", "psx2DDrawSprite", "psx2DFillRect", "psx2DDrawLine"}   # NgIFpsx, not named yet
    for f in (ZACTOR, ZPRIM):
        t = (SRC / f).read_text(encoding="utf-8")
        assert "TODO(decomp)" not in t
        for n, line in enumerate(t.splitlines(), 1):
            for m in bad.finditer(line):
                assert m.group(0) in allowed_fun, f"{f}:{n}: {line.strip()}"
    for p in list(SRC.rglob("*.c")) + list(INC.glob("*.h")):
        code = re.sub(r"/\*.*?\*/", "", p.read_text(encoding="utf-8"), flags=re.S)
        assert not OLD_NAMES.search(code), (p, OLD_NAMES.search(code).group(0))


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4, "RwV3d": 12}
ALIGN = {"RwV3d": 4}


def layouts(header):
    out = {}
    t = (INC / header).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields, maxal = 0, {}, 1
        for line in m.group(2).splitlines():
            d = re.match(r"\s*(?:const\s+)?(?:struct\s+)?(\w+)\s*(\**)\s*(\w+)((?:\[\w+\])*);\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?",
                         line)
            if not d:
                continue
            typ, ptr, name, dims, comment = d.groups()
            if ptr:
                size = align = 4
            elif typ in SIZES:
                size, align = SIZES[typ], ALIGN.get(typ, SIZES[typ])
            else:
                size, align = out[typ][0], out[typ][2]
            count = 1
            for k in re.findall(r"\[(\w+)\]", dims):
                count *= int(k, 0)
            off = (off + align - 1) // align * align
            assert comment is None or int(comment, 16) == off, (m.group(3), name, hex(off))
            fields[name] = off
            off += size * count
            maxal = max(maxal, align)
        out[m.group(3)] = ((off + maxal - 1) // maxal * maxal, fields, maxal)
    return out


def test_struct_layouts():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    lay = {**layouts("zactor.h"), **layouts("zprim.h")}
    sized = {"ZMotionSection": 4, "ZMotionState": 0x17C, "ZActorAnim": 0xD60, "ZactorFrame": 0x18,
             "ZactorBikeOffset": 0x18, "ZActor": 0x278, "ZactorBank": 0x14, "ZactorBankEntry": 8,
             "ZprimAsset": 0x10, "ZprimState": 0x54, "ZprimFrame": 0x10, "ZprimGlyph": 0x0A}
    for n, size in sized.items():
        assert lay[n][0] == size == want[n], n
    assert lay["ZprimImage"][1]["frame"] == 0x14 and lay["ZprimFont"][1]["glyph"] == 0x18
    assert lay["ZMotionHeader"][1]["section"] == 0x0C
    # rider.c reads anim->block[1].rec (ZActor +0x48 -> +0x18C)
    assert lay["ZActor"][1]["anim"] == 0x48 and 0x17C + lay["ZMotionState"][1]["rec"] == 0x18C
    for h, names in (("zactor.h", [n for n in sized if n.startswith("Z") and not n.startswith("Zprim")]),
                     ("zprim.h", [n for n in sized if n.startswith("Zprim")])):
        t = (INC / h).read_text(encoding="utf-8")
        for n in names:
            assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, want[n]), t), n
        assert h in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as fh:
        snames = {r["name"] for r in csv.DictReader(fh) if r["kind"] == "struct" and r["type"] in ("decomp/include/zactor.h", "decomp/include/zprim.h")}
    assert snames == set(sized) | {"ZMotionHeader", "ZprimImage", "ZprimPalette", "ZprimFont"}


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


@pytest.mark.parametrize("f,macro,count", [(ZACTOR, "ZACTOR_FILE", 9), (ZPRIM, "ZPRIM_FILE", 7)])  # clean-up: +4 / +1
def test_assert_sites_exist_in_exe(img, f, macro, count):
    t = (SRC / f).read_text(encoding="utf-8")
    path = re.search(r'#define %s "([^"]+)"' % macro, t).group(1).replace("\\\\", "\\")
    sites = {(m.group(1), int(m.group(2), 16))
             for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", %s, (0x[0-9A-Fa-f]+)' % macro, t)}
    assert len(sites) == count
    p_addr = img.find(path.encode() + b"\0") + B
    lo, hi = RANGES[f]
    for msg, line in sites:
        raw = msg.encode().decode("unicode_escape").encode("latin-1")   # C escapes such as \n
        addrs = [m.start() + B for m in re.finditer(re.escape(raw + b"\0"), img)]
        pats = [push(line) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", a)
                for a in addrs]
        assert any(p in img[lo - B:hi - B] for p in pats), (msg, hex(line))


def test_tables_match_exe(img):
    f32 = lambda va: struct.unpack_from("<f", img, va - B)[0]  # noqa: E731
    assert f32(0x0055F35C) == 34.0
    assert struct.unpack_from("<i", img, 0x0055F360 - B)[0] == 64
    assert struct.unpack_from("<i", img, 0x0055F38C - B)[0] == -1
    assert struct.unpack_from("<19H", img, 0x0055F364 - B) == M.MIRROR + (0,) * 4
    t = (SRC / ZACTOR).read_text(encoding="utf-8")
    # g_motionMirrorMap initializer
    mm = re.search(r"g_motionMirrorMap\[ZACTOR_MAX_CHANNELS\] = \{[^\n]*\n\s*([^}]*)\}", t).group(1)
    assert tuple(int(x) for x in mm.replace(",", " ").split()) == M.MIRROR
    # bone paths: rows of 8 signed bytes, 0xFF-terminated
    body = re.search(r"g_zactorBonePath\[2\]\[ZACTOR_MAX_CHANNELS\]\[8\] = \{(.*?)\};", t, re.S).group(1)
    rows = [[int(x) for x in r.split(",")] for r in re.findall(r"\{\s*(-?\d+(?:\s*,\s*-?\d+)*)\s*\}", body)]
    assert len(rows) == 15
    for b, r in enumerate(rows):
        exe = struct.unpack_from("<8b", img, 0x0055F390 + 8 * b - B)
        assert list(exe[:len(r)]) == r and all(v == 0 for v in exe[len(r):]), b
    assert img[0x0055F390 + 15 * 8 - B:0x0055F4C0 - B] == bytes(0x0055F4C0 - 0x0055F390 - 15 * 8)
    body = re.search(r"g_zactorBoneOffset\[2\]\[ZACTOR_MAX_CHANNELS\]\[4\] = \{(.*?)\};", t, re.S).group(1)
    offs = [[float(x.rstrip("f")) for x in r.split(",")] for r in re.findall(r"\{\s*(-?[\d.]+f(?:\s*,\s*-?[\d.]+f)*)\s*\}", body)]
    assert len(offs) == 15
    for b, o in enumerate(offs):
        exe = struct.unpack_from("<4f", img, 0x0055F4C0 + 16 * b - B)
        assert exe[3] == 0.0 and exe[:3] == pytest.approx(o, abs=1e-5), b
    assert img[0x0055F4C0 + 15 * 16 - B:0x0055F720 - B] == bytes(0x0055F720 - 0x0055F4C0 - 15 * 16)
    # zactorAngleMix25: the wrap test compares |a - b| with -180 (never true), 0.25 / 0.75 weights
    assert f32(0x0054DCA4) == -180.0 and f32(0x0054D164) == 0.75 and f32(0x0054D0A0) == 0.25
    assert img[0x00474B50 - B:0x00474B56 - B] == b"\xd8\x1d" + struct.pack("<I", 0x0054DCA4)
    # zactorVecMix25: 1.5 and 0.5
    assert b"\xd8\x0d" in img[0x00474D70 - B:0x00474DC0 - B]


def disasm(img, va, size):
    cs = pytest.importorskip("capstone")
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    return list(md.disasm(img[va - B:va - B + size], va))


def code_of(img, f):
    out = []
    for r in csv_rows():
        if r["file"] == f:
            out += disasm(img, int(r["va"], 16), int(r["size"]))
    return out


def test_decoder_reads_of_the_clip(img):
    """The open questions of ANIM.md section 6.3: what the decoder reads."""
    ins = disasm(img, 0x00472B10, 3004)
    ops = ["%s %s" % (i.mnemonic, i.op_str) for i in ins]
    # the u16 after the record time ('h') is never read: no memory operand at +6
    assert not [o for o in ops if re.search(r"(word|dword|byte) ptr \[e\w\w \+ 6\]", o)]
    # bit tests with 0x80: exactly the header's frames bit 15 (+1) and flags bit 15 (+3)
    t80 = [o for o in ops if o.startswith("test") and o.endswith("0x80")]
    assert t80 == ["test dl, 0x80", "test byte ptr [eax + 1], 0x80"]
    assert "mov dl, byte ptr [eax + 3]" in ops and "mov dl, byte ptr [eax + 2]" in ops
    # section flags: only bit 1 is tested (init record + stepping); the clock uses 34.0 * rate
    assert "test byte ptr [edi + 1], 2" in ops and "mov al, byte ptr [ecx + 1]" in ops
    assert "fld dword ptr [0x55f35c]" in ops
    # the record's f32 goes to the state (+0x1C) and the frame (+0x14), nowhere else
    assert "mov dword ptr [esi + 0x1c], eax" in ops and "mov dword ptr [ebx + 0x14], eax" in ops
    # nothing outside zactor.c reads the frame's yaw / value or the crossfade frame (0x00601D48..)
    for r in csv_rows():
        if r["file"] == ZACTOR or not r["file"]:
            continue
        va, n = int(r["va"], 16), int(r["size"])
        blob = img[va - B:va - B + n]
        for g in range(0x00601D30, 0x00601E1C, 4):
            assert struct.pack("<I", g) not in blob, (r["name"], hex(g))


def refs(img, lo, hi):
    """Addresses of 32-bit immediates/displacements in [lo, hi) in the game/engine code (below the
    statically linked libraries at 0x004A4A80)."""
    out = set()
    text = img[0x1000:0x004A4A80 - B]
    for g in range(lo, hi):
        for m in re.finditer(re.escape(struct.pack("<I", g)), text):
            out.add(0x401000 + m.start())
    return out


def test_module_globals_are_private(img):
    for f, (lo, hi) in ((ZACTOR, (0x00641E1C, 0x00641E34)), (ZACTOR, (0x00601DC8, 0x00601DE0)),
                        (ZPRIM, (0x006783FC, 0x00678434))):
        for a in refs(img, lo, hi):
            assert RANGES[f][0] <= a < RANGES[f][1], (f, hex(a))


def test_field_displacements(img):
    """The named fields are accessed at these displacements by their users."""
    lay = {**layouts("zactor.h"), **layouts("zprim.h")}

    def disps(ins):
        s = set()
        for i in ins:
            for m in re.finditer(r"\[e\w\w(?: \+ e\w\w(?:\*\d)?)? \+ (0x[0-9a-f]+|\d+)\]", i.op_str):
                s.add(int(m.group(1), 0))
        return s

    za = disps(code_of(img, ZACTOR))
    ng = disps(code_of(img, "zaxis/Bmx/Src/com/zge/NgIF/NgIFzactor.c"))
    a = lay["ZActor"][1]
    for n in ("flags", "skeleton", "heading", "anim", "radius", "model", "unk_18e"):
        assert a[n] in za, n
    for n in ("channelHash", "channelMirror", "numChannels", "model2"):
        assert a[n] in ng, n
    s = lay["ZMotionState"][1]
    dec = disps(disasm(img, 0x00472B10, 3004))
    for n in ("frameMs", "duration", "nextFrame", "rec", "value", "extra", "sections", "block", "transDelta",
              "bikeTransDelta", "motion", "playFlags", "decodeFlags", "lastDelta", "numSections", "numChannels",
              "bikeTrans", "bikeRoot", "height", "rot", "headYaw", "frameCount"):
        assert s[n] in dec, n
    assert 0x17C * 1 + s["rot"] + 2 * 6 + 2 == 0x17C + 0x112         # head yaw = rot[2][1]
    assert lay["ZActorAnim"][1]["fade"] in za
    st = lay["ZprimState"][1]
    zp = disps(code_of(img, ZPRIM))
    for n in ("bgType", "colour", "primFlags", "sprite", "spriteFlags", "spriteArg20", "spriteArg24", "scaleX",
              "scaleY", "spriteArg30", "clut34", "clut38", "font", "fontArg40", "fontFlags", "fontArg48",
              "textBg", "spacing"):
        assert st[n] in zp, n


# ------------------------------------------------------------------ the decoder port
MIRROR = M.MIRROR + (0, 0, 0, 0)


def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _s16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


class Block:
    """ZMotionState (only the fields the decoder uses)."""

    def __init__(self):
        self.time = self.frameMs = self.duration = self.nextFrame = 0
        self.rec = None
        self.value = 0.0
        self.transDelta = [0.0] * 3
        self.bikeTransDelta = [0.0] * 3
        self.rotDelta = [[0, 0, 0] for _ in range(19)]
        self.decodeFlags = 0
        self.lastDelta = [0.0] * 3
        self.numSections = self.numChannels = 0
        self.bikeTrans = [0.0] * 3
        self.bikeRoot = [0, 0, 0]
        self.height = 0.0
        self.rot = [[0, 0, 0] for _ in range(19)]
        self.frameCount = 0


class Decoder:
    """zactorDecodeMotion 0x00472B10 for one slot: st = block[1 + slot], bk = block[5 + slot]."""

    def __init__(self, data):
        self.d = bytes(data)
        self.st, self.bk = Block(), Block()

    def _h(self, o):
        return struct.unpack_from("<h", self.d, o)[0]

    def _H(self, o):
        return struct.unpack_from("<H", self.d, o)[0]

    def _I(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def _angles(self, q, init, mirror):
        d = self.d
        if self.st.decodeFlags & 0x10:
            v = [self._h(q), self._h(q + 2), self._h(q + 4)]
            n = 6
        elif init:
            v = [d[q] << 8, d[q + 1] << 8, d[q + 2] << 8]
            n = 3
        else:
            v = [struct.unpack_from("<b", d, q + i)[0] << 7 for i in range(3)]
            n = 3
        if mirror:
            v = [v[0], -v[1], -v[2]]
        return [_s16(x) for x in v], n

    def decode(self, rate=1.0, flags=0):
        """One call; returns (dx, dz) root delta or None (clip over)."""
        st, bk, d = self.st, self.bk, self.d
        mirror = bool(flags & 8)
        out = [0.0, 0.0, 0.0]
        nsec = d[2] >> 4
        if flags & 2:
            out[0], out[2] = st.lastDelta[0], st.lastDelta[2]
            st.bikeTrans = [0.0] * 3
            st.frameMs = self._I(4)
            st.duration = (self._H(0) & 0x7FFF) * st.frameMs
            st.decodeFlags &= 0x21
            if d[3] & 0x80:
                st.decodeFlags |= 0x10
            st.time = st.nextFrame = st.frameCount = 0
            st.numSections, st.numChannels = nsec, d[12]
            st.rec = 12 + 4 * nsec + (100 if d[1] & 0x80 else 0)
            for s in range(nsec):
                so, p = 12 + 4 * s, st.rec
                if self._H(so + 2) == 0:
                    st.value = struct.unpack_from("<f", d, p + 8)[0]
                    x = self._h(p + 12) * 0.125
                    out[0] = -x if mirror else x
                    out[2] = self._h(p + 16) * 0.125
                    st.height = _f32(self._h(p + 14) * 0.125)
                    q = p + 0x12
                    for b in range(d[so]):
                        v, n = self._angles(q, True, mirror)
                        st.rot[MIRROR[b] if mirror else b] = v
                        q += n
                elif not d[so + 1] & 2:
                    mask = self._I(p)
                    q = p + (12 if mask & 1 else 6)
                    if mask & 2:
                        x = _f32(self._h(q) * 0.125)
                        st.bikeTrans = [-x if mirror else x, _f32(self._h(q + 2) * 0.125), _f32(self._h(q + 4) * 0.125)]
                        q += 6
                    if not mask & 4:
                        st.bikeRoot = [0, 0, 0]
                    else:
                        for b in range(d[so]):
                            bk.rot[b], n = self._angles(q, True, mirror)
                            q += n
                        st.bikeRoot = list(bk.rot[0])
                else:
                    continue
                st.rec = p + ((q - p + 3) & ~3)
            st.nextFrame += st.frameMs
        else:
            st.time += int(_f32(34.0 * rate))
            if st.rec is None or self._I(st.rec) == 0:
                return None
            while st.nextFrame <= st.time:
                if self._I(st.rec) != 0:
                    t0 = self._H(st.rec + 4)
                    if t0 <= st.time:
                        while True:
                            self._record(st.rec, mirror)
                            if self._H(st.rec + 4) != t0:
                                break
                    for s in range(nsec):
                        so = 12 + 4 * s
                        if self._H(so + 2) == 0:
                            st.frameCount += 1
                            st.height = _f32(st.height + st.transDelta[1])
                            out[0], out[2] = st.transDelta[0], st.transDelta[2]
                            for b in range(d[so]):
                                st.rot[b] = [_s16(st.rot[b][i] + st.rotDelta[b][i]) for i in range(3)]
                        elif not d[so + 1] & 2:
                            st.bikeTrans = [_f32(st.bikeTrans[i] + st.bikeTransDelta[i]) for i in range(3)]
                            for b in range(d[so]):
                                bk.rot[b] = [_s16(bk.rot[b][i] + bk.rotDelta[b][i]) for i in range(3)]
                            st.bikeRoot = list(bk.rot[0])
                st.nextFrame += st.frameMs
        if st.nextFrame >= st.duration:
            st.lastDelta = list(st.transDelta)
        return out[0], out[2]

    def _record(self, p, mirror):
        st, bk, d = self.st, self.bk, self.d
        mask = self._I(p)
        so = 12 + 4 * (mask >> 28)
        rider = self._H(so + 2) == 0
        q = p + 6
        if mask & 1:
            if rider:
                st.value = struct.unpack_from("<f", d, p + 8)[0]
            q = p + 12
        if mask & 2:
            x = _f32(self._h(q) / 256.0)
            t = [-x if mirror else x, _f32(self._h(q + 2) / 256.0), _f32(self._h(q + 4) / 256.0)]
            if rider:
                st.transDelta = t
            else:
                st.bikeTransDelta = t
            q += 6
        for b in range(d[so]):
            if mask & (4 << b):
                v, n = self._angles(q, False, mirror)
                if rider:
                    st.rotDelta[MIRROR[b] if mirror else b] = v
                else:
                    bk.rotDelta[b] = v
                q += n
        st.rec = p + ((q - p + 3) & ~3)


def play(data, flags=0):
    """Frames the game shows for one clip (rate 1): rider angles, height, summed root x/z, bike."""
    dec = Decoder(data)
    frames, root = [], [0.0, 0.0]
    r = dec.decode(1.0, flags | 2)
    while r is not None:
        root[0] += r[0]
        root[1] += r[1]
        st = dec.st
        frames.append({"rider": [tuple(x) for x in st.rot[:st.numChannels]], "bike": [tuple(x) for x in dec.bk.rot],
                       "height": st.height, "root": tuple(root), "bikeTrans": tuple(st.bikeTrans)})
        r = dec.decode(1.0, flags)
    return frames


def same(frame, pose, mirror=False):
    if mirror:
        exp = [None] * len(pose.rider)
        for b, v in enumerate(pose.rider):
            exp[MIRROR[b]] = (v[0], _s16(-v[1]), _s16(-v[2]))
        bike = [(v[0], _s16(-v[1]), _s16(-v[2])) for v in pose.bike or []]
        sx = -1
    else:
        exp, bike, sx = pose.rider, pose.bike or [], 1
    return (frame["rider"] == exp and frame["bike"][:len(bike)] == bike
            and abs(frame["height"] - pose.rider_trans[1]) < 1e-3
            and abs(frame["root"][0] - sx * pose.rider_trans[0]) < 1e-3
            and abs(frame["root"][1] - pose.rider_trans[2]) < 1e-3
            and (pose.bike_trans is None
                 or (abs(frame["bikeTrans"][0] - sx * pose.bike_trans[0]) < 1e-3
                     and all(abs(a - b) < 1e-3 for a, b in zip(frame["bikeTrans"][1:], pose.bike_trans[1:])))))


@pytest.fixture(scope="module")
def bank():
    f = GAME_DIR / "assets" / "motion.zal"
    if not f.is_file():
        pytest.skip("no motion.zal")
    return M.read_bank(f.read_bytes())


@pytest.mark.parametrize("mirror", [False, True])
def test_decoder_port_agrees_with_motion_py(bank, mirror):
    """Every frame of all 238 clips: the game's decoder (port) == dmbmx.formats.motion.sample()."""
    clips = frames = 0
    for mid in bank.ids:
        raw = bank.bank.find(mid).data()
        mo = M.Motion.read(raw)
        poses = mo.sample()
        game = play(raw, 8 if mirror else 0)
        assert len(game) == len(poses) == mo.nframes, mid       # the stream ends exactly at the last frame
        for k, (f, p) in enumerate(zip(game, poses)):
            assert same(f, p, mirror), (mid, k)
        clips += 1
        frames += len(game)
    assert (clips, frames) == (238, 2301)


def test_bank_facts_behind_the_answers(bank):
    """ANIM.md 6.3 and the decoder's persistence rule, over the whole bank."""
    for mid in bank.ids:
        mo = M.Motion.read(bank.bank.find(mid).data())
        assert (mo.flags, mo.extra, mo.frames_word & 0x8000) == (0x9823, 0, 0)
        assert all(sflags == 0x80 for _, sflags, _ in mo.sections)
        assert all(r.h == 0 and r.value == 0.0 for r in [i for i in mo.inits if i] + mo.records if r.mask & 1 or r.init_rider)
        times = [r.time for r in mo.records]
        assert times == sorted(times) and all(t % 34 == 0 and t > 0 for t in times)
        assert times[-1] == (mo.nframes - 1) * 34
        # frame 1 sets every channel's delta and every translation delta: the deltas the decoder
        # keeps from the previous clip (it never clears them) are always overwritten
        for si, (cnt, _, _) in enumerate(mo.sections):
            first = [r for r in mo.records if r.time == 34 and r.section == si]
            assert set().union(*(set(r.rots) for r in first)) == set(range(cnt)), (mid, si)
            assert any(r.trans is not None for r in first), (mid, si)


# ------------------------------------------------------------------ synthetic clips (no game data)
def _clip(wide=True, bike_flags=0x80, records=None, frames=4, bike_init_mask=0x10000006):
    m = M.Motion()
    m.frames_word, m.flags, m.frame_ms, m.extra = frames, 0x0023 | (0x8000 if wide else 0), 34, 0
    m.sections = [(2, 0x80, 0), (2, bike_flags, 1)]
    big = (200, 0x7F, 0x90) if not wide else (-30000, 1200, 5)
    m.inits = [M.Record(0, 0x1FFFF, 0, 0, 0.5, (8, 16, -8), {0: big, 1: (1, 2, 3)}, init_rider=True),
               None if bike_flags & 2 else M.Record(1, bike_init_mask, 0, 0, None, (80, 0, -8), {0: big, 1: (4, 5, 6)})]
    m.records = records or []
    m.end_time = frames * 34
    return M.Motion.read(m.write())


def _check(m, mirror=False):
    data = m.write()
    poses = M.Motion.read(data).sample()
    game = play(data, 8 if mirror else 0)
    assert 1 <= len(game) <= len(poses)
    for k, (f, p) in enumerate(zip(game, poses)):
        assert same(f, p, mirror), k
    for p in poses[len(game):]:                  # after the stream ends motion.py holds the last pose
        assert same(game[-1], p, mirror)
    return game, poses


def test_synthetic_8bit_angles():
    recs = [M.Record(0, 0x0000000E, 34, 0, None, (256, 512, 0), {0: (-128, 127, 3), 1: (1, -1, 0)}),
            M.Record(1, 0x1000000E, 34, 0, None, (0, 256, 0), {0: (5, 0, -5), 1: (0, 0, 1)}),
            M.Record(0, 0x00000004, 102, 0, None, None, {0: (0, 0, 0)})]
    for mirror in (False, True):
        game, poses = _check(_clip(wide=False, records=recs), mirror)
        assert len(game) == 4
    # u8 << 8 init values wrap to s16 (200 << 8 = 0xC800 -> -14336)
    assert M.Motion.read(_clip(wide=False).write()).sample()[0].rider[0] == (-14336, 0x7F00, -28672)


def test_synthetic_odd_times_time0_and_gaps():
    """One record group per frame step, applied when due (time <= frame time)."""
    recs = [M.Record(0, 0x00000004, 0, 0, None, None, {0: (10, 0, 0)}),       # time 0: applied at frame 1
            M.Record(0, 0x00000004, 50, 0, None, None, {0: (0, 10, 0)}),      # odd time: frame 2
            M.Record(1, 0x10000004, 50, 0, None, None, {0: (0, 0, 7)}),
            M.Record(0, 0x00000006, 170, 0, None, (256, 0, 0), {0: (0, 0, 1)})]  # frame 5 (frames 3, 4 just step)
    game, poses = _check(_clip(records=recs, frames=8))
    assert len(game) == 6                       # the stream ends at frame 5
    # frame 2: the time-50 group replaces channel 0's delta (10, 0, 0) by (0, 10, 0)
    assert [p.rider[0] for p in poses[:3]] == [(-30000, 1200, 5), (-29990, 1200, 5), (-29990, 1210, 5)]
    assert poses[2].bike[0] == (-30000, 1200, 12)
    assert poses[7].rider == poses[5].rider


def test_synthetic_section_flag_2_and_bike_init_without_angles():
    recs = [M.Record(1, 0x1000000E, 34, 0, None, (256, 0, 0), {0: (9, 9, 9), 1: (1, 1, 1)}),
            M.Record(0, 0x00000004, 68, 0, None, None, {0: (1, 0, 0)})]
    game, poses = _check(_clip(bike_flags=0x82, records=recs))
    assert poses[2].bike == [(0, 0, 0), (0, 0, 0)] and poses[2].bike_trans == (0.0, 0.0, 0.0)  # never stepped
    game, poses = _check(_clip(records=recs, bike_init_mask=0x10000002))
    assert poses[2].bike[0] == (18, 18, 18)

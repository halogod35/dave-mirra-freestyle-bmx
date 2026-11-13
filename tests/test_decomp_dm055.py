"""DM-055 self-consistency: decomp of the sound system (bmxsound.c, zsndseq.c, zsndsrc.c and the PC
backend winChan.c / winLoad.c / winStrm.c) and the audio.py answers that came out of it.

No C compiler on the build PC, so the C is checked the way DM-041..DM-053 do it:
1. structure: all 129 functions drafted, names agree with decomp/functions.csv and decomp/symbols/symbols.csv
   (last row wins), no Ghidra artefacts, no old names left in decomp/src, zsound.h layouts agree
   with their "+0x.." comments and the ghidra_types.txt sizes, every assert (message, line) of the
   six files is a call site in its file's code in NgBMX.exe and vice versa;
2. exe facts the C and audio.py rely on: tables (pitch, gain, envelope times, stream attenuation,
   IMA, the level -> music track table), the ZTRK decoder's jump tables (opcode -> handler,
   form -> delta / length layout), the event codes the game passes to bmxsoundEvent;
3. behaviour: a literal port of zsndSeqDecodeEvent / zsndSeqPeekDelta against audio.ztrk_decode on
   every ZTRK track, which also round-trips through ztrk_encode; a literal port of zsndStreamRead +
   zsndImaDecode (0x4000-byte chunks, state carried) against audio.ima_decode on the music tracks
   (the first 128 KB of each by default; every whole track with DMBMX_SLOW=1), plus an exhaustive check
   of the decoder's state transition.
"""
import csv
import os
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR
from fuzz import fuzz_parser
from dmbmx.cli import main
from dmbmx.core.errors import FormatError
from dmbmx.formats import audio

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
B = 0x400000
BMXSOUND = "zaxis/Bmx/Src/com/zge/bmx/bmxsound.c"
WINCHAN = "zaxis/Bmx/Src/Pc/zge/sound/winChan.c"
WINLOAD = "zaxis/Bmx/Src/Pc/zge/sound/winLoad.c"
WINSTRM = "zaxis/Bmx/Src/Pc/zge/sound/winStrm.c"
ZSNDSEQ = "zaxis/Bmx/Src/com/zge/zsndseq.c"
ZSNDSRC = "zaxis/Bmx/Src/com/zge/zsndsrc.c"
FILES = {BMXSOUND: (10, 0x0044DD40, 0x00450044, "bmxsound", "Bmx/Src/com/zge/bmx/bmxsound.c"),
         WINCHAN: (21, 0x0046D940, 0x0046FA0B, "zsndc", "Bmx/Src/Pc/zge/sound/winChan.c"),
         WINLOAD: (17, 0x0046FB20, 0x00470A48, "zsnd", "Bmx/Src/Pc/zge/sound/winLoad.c"),
         WINSTRM: (27, 0x00470AA0, 0x004725D2, "zsnd", "Bmx/Src/Pc/zge/sound/winStrm.c"),
         ZSNDSEQ: (28, 0x0047C4C0, 0x0047DE07, "zsndSeq", "Bmx/Src/com/zge/zsndseq.c"),
         ZSNDSRC: (26, 0x0047DE20, 0x0047EBC3, "zsnd", "Bmx/Src/com/zge/zsndsrc.c")}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+)(?: \*/\n|\n(?: \*[^\n]*\n)*?)((?:[^\n]+\n)+?)\{\n")
MUSIC_TRACKS = 10


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    out = []
    for m in VA_RE.finditer((SRC / f).read_text(encoding="utf-8")):
        name = re.search(r"([A-Za-z_]\w*)\s*\([^()]*\)\s*$", m.group(4).strip()).group(1)
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
@pytest.mark.parametrize("f", list(FILES))
def test_every_function_drafted_and_named_consistently(f):
    count, lo, hi, prefix, src = FILES[f]
    rows = [r for r in csv_rows() if r["file"] == f]
    assert len(rows) == count
    cf = c_functions(f)
    assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in rows)
    by_va = {int(r["va"], 16): r for r in rows}
    syms = last_symbols("func")
    for va, size, status, name in cf:
        r = by_va[va]
        assert lo <= va < hi, hex(va)
        assert status == "draft" and r["status"] == "draft", hex(va)
        assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
        assert syms[va]["name"] == name and syms[va]["card"] == "DM-055", hex(va)
        assert syms[va]["source_file"] == src
        assert name.startswith(prefix), name
        assert re.match(r"\w+ .*\b%s\(" % name, r["signature"]), r["signature"]


def test_moved_and_created_functions():
    rows = {int(r["va"], 16): r for r in csv_rows()}
    for va, name in ((0x0044DC30, "frontendShutdownApp"), (0x0044DC70, "frontendUpdateApp"),
                     (0x0044DD10, "frontendRequestExit")):
        assert rows[va]["name"] == name and rows[va]["file"] == "zaxis/Bmx/Src/com/menu/frontend.c"
    assert rows[0x0047C4C0]["file"] == ZSNDSEQ and rows[0x0047C4C0]["name"] == "zsndSeqInit"
    assert not (SRC / "_unassigned" / "gap_zprim__zsndseq.c").exists()
    # the event handlers and timer callback exist as functions now
    for va in (0x0047D7B0, 0x0047DA10, 0x0047DB90, 0x0047DC10, 0x00450020):
        assert rows[va]["status"] == "draft"


OLD_NAMES = re.compile(r"\b(bmxsound_[0-9a-f]{8}|winChan_[0-9a-f]{8}|winLoad_[0-9a-f]{8}|winStrm_[0-9a-f]{8}|"
                       r"zsndseq_[0-9a-f]{8}|zsndsrc_[0-9a-f]{8}|winLoad_RegisterAudioAsset|winLoad_GetStreamFile|"
                       r"winStrm_Play|winStrm_OpenStreamFile|winStrm_ReadStream|winStrm_ImaDecode|"
                       r"zsndseq_StartTrack|bmxsound_Open|FUN_0047c4c0)\b")


def test_no_ghidra_artefacts_and_old_names():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|"
                     r"[iu]Var\d+|pc?Var\d+|extraout_\w+|CONCAT\d+|thunk_\w+)\b")
    allowed = {"ngappWorldToCamera"}          # NgIF camera-space transform, not named yet
    for f in FILES:
        t = (SRC / f).read_text(encoding="utf-8")
        assert "TODO(decomp)" not in t
        for n, line in enumerate(t.splitlines(), 1):
            for m in bad.finditer(line):
                assert m.group(0) in allowed, f"{f}:{n}: {line.strip()}"
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
            fp = re.match(r"\s*\w+\s*\(__cdecl \*(\w+)\)\(.*\);\s*/\*\s*\+0x([0-9A-Fa-f]+)", line)
            if fp:
                name, comment, size, align, count = fp.group(1), fp.group(2), 4, 4, 1
            else:
                d = re.match(r"\s*(?:const\s+)?(?:struct\s+)?(\w+)\s*(\**)\s*(\w+)((?:\[\w+\])*);"
                             r"\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?", line)
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


SIZED = {"ZsndVoice": 0x14, "ZsndSample": 0x18, "ZsndSampleFile": 8, "ZsndTrackDef": 0x10, "ZsndKeymapKey": 2,
         "ZsndBankHeader": 0x48, "ZsndAsset": 0x48, "ZsndFileDesc": 0x0C, "ZsndcInitParams": 0x14,
         "ZsndcEffect": 0x14, "ZsndcChannel": 0x48, "ZsndcHandle": 2, "ZsndStreamFile": 0x24,
         "ZsndStreamQueue": 0x0C, "ZsndStream": 0x44, "ZsndSeqTrack": 0x48, "ZsndSeqNote": 0x14,
         "ZsndSeqEvent": 0x10, "ZsndSrc": 0x28}


def test_struct_layouts():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    assert "zsound.h" in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    lay = layouts("zsound.h")
    t = (INC / "zsound.h").read_text(encoding="utf-8")
    for n, size in SIZED.items():
        assert lay[n][0] == size == want[n], n
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (n, size), t), n
    # the bank header's table pointers sit where audio.TABLES says
    h = lay["ZsndBankHeader"][1]
    for tname, cnt, ptr in (("A", "numVoices", "voices"), ("B", "numSamples", "samples"), ("C", "numFiles", "files"),
                            ("D", "numPhrases", "phrases"), ("E", "numTrackDefs", "trackDefs"),
                            ("F", "numKeymaps", "keymaps")):
        assert (h[cnt], h[ptr]) == audio.TABLES[tname][:2], tname
    assert h["headerSize"] == 0x0C and h["fileSize"] == 8
    # the voice / track def fields audio.py decodes
    v = lay["ZsndVoice"][1]
    assert [v[k] for k in ("sample", "pitch", "volume", "pan", "attack", "decay", "sustainTime", "release",
                           "sustainLevel", "unused_0b", "psxFlag", "bank")] == \
        [0, 2, 4, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x11, 0x12]
    assert struct.calcsize(audio.Voice.FMT) == 0x14 and struct.calcsize(audio.TrackDef.FMT) == 0x10
    assert lay["ZsndTrackDef"][1]["ztrk"] == 8 and lay["ZsndSample"][1]["loopEnd"] == 0x10
    structs = {n for n, r in last_symbols("struct").items() if r["card"] == "DM-055"}
    assert structs == set(SIZED) | {"ZsndKeymap"}


# ------------------------------------------------------------------ exe
@pytest.fixture(scope="module")
def img():
    exe = GAME_DIR / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    return pefile.PE(str(exe), fast_load=True).get_memory_mapped_image()


def cstr(img, va):
    return img[va - B:img.index(b"\0", va - B)].decode("latin-1")


SITE_RE = re.compile(rb"(?=(?:\x6a(.)|\x68(....))\x68(....)\x68(....))", re.S)   # overlapping matches


def exe_sites(img, lo, hi, path):
    """{(message, line)} of every push line / push file / push message with this file's path."""
    out = set()
    for m in SITE_RE.finditer(img[lo - B:hi - B]):
        f = struct.unpack("<I", m.group(3))[0]
        if not 0x00550000 <= f < 0x00570000 or cstr(img, f) != path:
            continue
        line = m.group(1)[0] if m.group(1) is not None else struct.unpack("<I", m.group(2))[0]
        out.add((cstr(img, struct.unpack("<I", m.group(4))[0]), line))
    return out


def c_sites(f):
    """{(message, line)} the C asserts, with each file's macros expanded."""
    t = re.sub(r"/\*.*?\*/", "", (SRC / f).read_text(encoding="utf-8"), flags=re.S)
    t = re.sub(r"#define [^\n]*(?:\\\n[^\n]*)*", "", t)
    s = set()
    for m in re.finditer(r'zAssertFail\("((?:[^"\\]|\\.)*)", \w+_FILE, (0x[0-9A-Fa-f]+|\d+)', t):
        s.add((m.group(1), int(m.group(2), 0)))
    for m in re.finditer(r'\bFAIL\("((?:[^"\\]|\\.)*)", (0x[0-9A-Fa-f]+|\d+)\)', t):
        s.add((m.group(1), int(m.group(2), 0)))
    for m in re.finditer(r'\bASSERT_AT\(([^;]*?),\s+"((?:[^"\\]|\\.)*)", (0x[0-9A-Fa-f]+|\d+)\);', t):
        s.add((m.group(2), int(m.group(3), 0)))
    for m in re.finditer(r"\bDS_OK_AT\(.*?, (0x[0-9A-Fa-f]+)\);", t):
        s.add(("hResult == DS_OK", int(m.group(1), 16)))
    for m in re.finditer(r"\bTRACK_CHECK\(\w+, (0x[0-9A-Fa-f]+)\)", t):
        s.add(("hTrack < kzsndSeqMaxTracks", int(m.group(1), 16)))
    for m in re.finditer(r"\bCHECK_HANDLE\(\w+, (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)", t):
        s.add(("invalid file handle", int(m.group(1), 16)))
        s.add(("file not loaded", int(m.group(2), 16)))
    for m in re.finditer(r"\bSRC_CHECK\(\w+, (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)", t):
        s.add(("not initialized", int(m.group(1), 16)))
        s.add(("invalid handle", int(m.group(2), 16)))
    return {(msg.replace('\\"', '"'), line) for msg, line in s}


@pytest.mark.parametrize("f", list(FILES))
def test_assert_sites_match_exe(img, f):
    count, lo, hi, prefix, src = FILES[f]
    t = (SRC / f).read_text(encoding="utf-8")
    path = re.search(r'#define \w+_FILE "([^"]+)"', t).group(1).replace("\\\\", "\\")
    c = c_sites(f)
    exe = exe_sites(img, lo, hi, path)
    assert c and c == exe, (sorted(c - exe), sorted(exe - c))
    # the two winStrm checks compiled into a shared call (no push-line pattern) are still there
    if f == WINSTRM:
        assert re.findall(r"DS_OK_SHARED\(.*?, (0x[0-9A-F]+)\);", t) == ["0x296", "0x2E1"]
        for line in (0x296, 0x2E1):
            assert struct.pack("<I", line) in img[lo - B:hi - B]


def u16s(img, va, n):
    return struct.unpack_from(f"<{n}H", img, va - B)


def test_tables_match_exe(img):
    t = (SRC / WINCHAN).read_text(encoding="utf-8")
    semi = u16s(img, 0x0055E820, 73)
    assert semi[36] == 0x1000 and semi[0] == 0x200 and semi[72] == 0x8000
    assert all(abs(semi[i] - round(4096 * 2 ** ((i - 36) / 12))) <= 1 for i in range(73))
    cents = u16s(img, 0x0055E8B4, 100)
    assert all(abs(cents[c] - round(4096 * 2 ** (c / 1200))) <= 1 for c in range(100))
    gain = struct.unpack_from("<128f", img, 0x0055E97C - B)
    assert gain[0] == 0.0 and gain[127] == 1.0 and list(gain) == sorted(gain)
    assert u16s(img, 0x0055EB7C, 128) == audio.ENV_TIME_MS
    assert struct.unpack_from("<I", img, 0x0055ED7C - B)[0] == 1000 and "g_zsndcMsPerSecond = 1000" in t
    atten = struct.unpack_from("<128h", img, 0x0055EC7C - B)
    assert atten[0] == -10000 and atten[127] == 0 and list(atten) == sorted(atten)
    # IMA tables
    assert struct.unpack_from("<16i", img, 0x0055F1AC - B) == audio.IMA_INDEX
    assert struct.unpack_from("<89H", img, 0x0055F1EC - B) == audio.IMA_STEP
    # bmxsound data: initial values
    b = (SRC / BMXSOUND).read_text(encoding="utf-8")
    lt = re.search(r"g_bmxsoundLevelTrack\[12\] = \{([^}]*)\}", b).group(1)
    assert tuple(int(x) for x in lt.split(",")) == tuple(img[0x0055B550 - B:0x0055B55C - B])
    assert struct.unpack_from("<ii", img, 0x0055B530 - B) == (-1, -1)
    assert tuple(img[0x0055B538 - B:0x0055B53B - B]) == (0x7F, 0x7F, 0x7F)
    assert u16s(img, 0x0055B53C, 8) == (0xFFFF,) * 8
    assert struct.unpack_from("<bB", img, 0x0055B54C - B) == (-1, 1)
    assert struct.unpack_from("<I", img, 0x0055FFC0 - B)[0] == 30
    # float constants used by the C: release level x -127, 1/127, the tempo factors
    f32 = lambda va: struct.unpack_from("<f", img, va - B)[0]  # noqa: E731
    assert f32(0x0054DC94) == -127.0 and f32(0x0054DC98) == pytest.approx(1 / 127)
    assert f32(0x0054DCB4) == 4096000.0 and f32(0x0054DCB0) == pytest.approx(4.096)
    assert f32(0x0054DB98) == -2000.0 and f32(0x0054DB94) == pytest.approx(0.042333335)
    # the strings the loader compares, and the bank file names the game builds
    assert cstr(img, 0x0055F090) == "ZSND" and cstr(img, 0x0055F060) == "WIN "
    for va, s in ((0x0055B660, "SFXCOM.ZSD"), (0x0055B614, "MUSIC.ZSD"), (0x0055B5DC, "SPEECH.ZSD"),
                  (0x0055B800, "SFXMEN%02d.ZSD"), (0x0055B948, "SFXGAME.ZSD"), (0x0055B8F8, "SFXGAME2.ZSD"),
                  (0x0055B908, "SFXLEV%02d.ZSD"), (0x0055B88C, "SFXCHR%02d.ZSD")):
        assert cstr(img, va) == s and f'"{s}"' in b


def test_ztrk_decoder_tables_match_exe(img):
    """zsndSeqDecodeEvent's jump tables: form -> layout, opcode -> handler."""
    forms = struct.unpack_from("<8I", img, 0x0047D680 - B)
    # each form case reads delta / length as audio.FORMS says (checked by the case's first
    # instructions: where it reads the length from and how far it advances)
    expect = {0: b"\xc7\x47\x04\x00\x00\x00\x00", 3: b"\xc7\x47\x04\x00\x00\x00\x00\x46",
              6: b"\xc7\x47\x04\x00\x00\x00\x00\x83\xc6\x04"}
    for form, code in expect.items():
        assert img[forms[form] - B:forms[form] - B + len(code)] == code, form
    assert img[forms[1] - B:forms[1] - B + 4] == b"\x33\xc0\x8a\x06"            # length = byte [esi]
    assert img[forms[4] - B:forms[4] - B + 5] == b"\x33\xc9\x8a\x4e\x01"        # length = byte [esi+1]
    handlers = struct.unpack_from("<7I", img, 0x0047D6B0 - B)
    index = img[0x0047D6CC - B:0x0047D6CC - B + 0x41]
    by_op = {op: handlers[index[op]] for op in range(0, 0x41, 8)}
    # what each case stores as the event handler (mov dword ptr [edi + 0xc], imm32)
    store = {}
    for op, case in by_op.items():
        m = re.match(rb"(?:.{0,8}?)\xc7\x47\x0c(....)", img[case - B:case - B + 16], re.S)
        store[op] = struct.unpack("<I", m.group(1))[0] if m else None
    assert store == {0x00: 0x0047D7B0, 0x08: 0x0047D7B0, 0x10: 0x0047DA10, 0x18: 0x0047DB90,
                     0x20: None, 0x28: None, 0x30: 0x0047DC10, 0x38: None, 0x40: None}
    assert set(audio.OP_PARAMS) == set(by_op)
    rows = {int(r["va"], 16): r["name"] for r in csv_rows()}
    assert [rows[v] for v in (0x0047D7B0, 0x0047DA10, 0x0047DB90, 0x0047DC10)] == \
        ["zsndSeqEvNote", "zsndSeqEvControl", "zsndSeqEvMarker", "zsndSeqEvJump"]


def _thunks_to(img, target):
    out, a = [target], 0x00401005
    while img[a - B] == 0xE9:
        if a + 5 + struct.unpack_from("<i", img, a + 1 - B)[0] == target:
            out.append(a)
        a += 5
    return out


def test_event_codes_the_game_uses_are_handled(img):
    """Every event code pushed before a call to bmxsoundEvent is a case of the C dispatcher."""
    tg = set(_thunks_to(img, 0x0044E300))
    text = img[0x3000:0x004A4A80 - B]
    codes = set()
    for i in range(len(text) - 5):
        if text[i] == 0xE8 and 0x403000 + i + 5 + struct.unpack_from("<i", text, i + 1)[0] in tg:
            back = text[max(0, i - 12):i]
            m = [struct.unpack_from("<I", back, k + 1)[0] for k in range(len(back) - 4) if back[k] == 0x68]
            m = [c for c in m if 1000 <= c <= 6000]
            if m:
                codes.add(m[-1])
    assert len(codes) >= 30
    c = re.sub(r"/\*.*?\*/", "", (SRC / BMXSOUND).read_text(encoding="utf-8"), flags=re.S)
    cases = {int(x, 0) for x in re.findall(r"case (\d+|0x[0-9A-Fa-f]+):", c)}
    names = {"BMXSND_SET_MODE": 1000, "BMXSND_SET_MUSIC": 1001, "BMXSND_SET_VOLUMES": 1002,
             "BMXSND_SET_REVERB": 1003, "BMXSND_GET_TRACK": 1004, "BMXSND_SET_TRACK": 1005, "BMXSND_PAUSE": 6000}
    cases |= {v for k, v in names.items() if f"case {k}:" in c}
    assert codes <= cases, sorted(codes - cases)
    # the codes the event map in audio.py and docs/re/sound.md describe are the ones in use
    doc = (REPO / "docs" / "re" / "sound.md").read_text(encoding="utf-8")
    for code in codes:
        assert f"| {code} " in doc, code


# ------------------------------------------------------------------ ZTRK: port of the decoder
def port_peek_delta(code, pc):
    """zsndSeqPeekDelta 0x0047D450."""
    form = code[pc] & 7
    if form < 3:
        return 0
    if form < 6:
        return code[pc + 1]
    return struct.unpack_from("<I", code, pc + 1)[0]


def port_decode_event(code, pc, velocity):
    """zsndSeqDecodeEvent 0x0047D490 -> (new pc, length, op, p0, p1 / u32, ok)."""
    b = code[pc]
    p = pc + 1
    op, form = b & 0xF8, b & 7
    if form == 0:
        length = 0
    elif form == 1:
        length, p = code[p], p + 1
    elif form == 2:
        length, p = struct.unpack_from("<I", code, p)[0], p + 4
    elif form == 3:
        length, p = 0, p + 1
    elif form == 4:
        length, p = code[p + 1], p + 2
    elif form == 5:
        length, p = struct.unpack_from("<I", code, p + 1)[0], p + 5
    elif form == 6:
        length, p = 0, p + 4
    else:
        length, p = struct.unpack_from("<I", code, p + 4)[0], p + 8
    assert op != 0xF8                         # no bank uses the indirect form
    par = code[p:p + 2]
    size, ok, a, c = 0, True, None, None
    if op == 0x00:
        a, c, size = par[0], par[1], 2
    elif op == 0x08:
        a, c, size = par[0], velocity, 1
    elif op in (0x10, 0x18):
        a, c, size = par[0], par[1], 2
    elif op == 0x30:
        a, size = struct.unpack_from("<I", code, p)[0], 4
    elif op in (0x20, 0x28, 0x38, 0x40):
        pass
    else:
        ok = False
    return p + size, length, op, a, c, ok


@pytest.fixture(scope="module")
def banks():
    d = GAME_DIR / "bin" / "audio"
    if not d.is_dir():
        pytest.skip("no corpus")
    return {p.stem.lower(): audio.read(p.read_bytes()) for p in sorted(d.glob("*.zsd"))}


def test_ztrk_port_agrees_with_audio_py_on_every_track(banks):
    n = 0
    for name, b in banks.items():
        for (td, events), (s, e) in zip(b.ztrk_tracks(), b.ztrk_ranges()):
            n += 1
            record = bytes(b.header[s:e])
            assert audio.ztrk_encode(events) == record, name          # round trip
            code = record[4:]
            pc, velocity = 0, 0x7F
            for ev in events:
                assert pc == ev.offset
                assert port_peek_delta(code, pc) == ev.delta
                pc, length, op, a, c, ok = port_decode_event(code, pc, velocity)
                assert ok and op == ev.op and length == ev.length
                if op in (0x00, 0x10, 0x18):
                    assert (a, c) == (ev.params[0], ev.params[1])
                if op == 0x00:
                    velocity = c                                       # zsndSeqEvNote keeps it
                if op == 0x30:
                    assert a == struct.unpack("<I", ev.params)[0]
            assert pc == len(code)
            # every track is an endless loop: it ends with a jump to an earlier event
            assert events[-1].op == audio.OP_JUMP
            assert td.voice == 0xFF and td.keymap == 0xFF and td.volume == 0x7F
    assert n == 42


def test_bank_facts_behind_the_answers(banks):
    """Voice fields +0x0B..+0x11, sample flags, loop_end, ZTRK opcodes in use."""
    assert len(banks) == 22
    ops = set()
    for name, b in banks.items():
        for v in b.voices:
            assert v.unused_0b == bytes(6) and v.psx_flag in (0, 1)
            assert v.sustain_level == 127                 # the decay rate bug never matters
            assert v.pack() == audio.Voice.parse(v.pack()).pack()
        for v, raw in zip(b.voices, b.sound_defs()):
            assert v.pack() == raw
        for sd in b.sample_defs:
            assert sd.flags in (0, audio.SAMPLE_LOOP) and sd.x14 == 0
        for td, events in b.ztrk_tracks():
            ops |= {(e.op, e.params[0] if e.op == audio.OP_CONTROL else None) for e in events}
        if name.startswith("sfxlev") and name != "sfxlev12":
            assert 3 <= b.count("E") <= 4
    assert {o for o, _ in ops} == {audio.OP_NOTE, audio.OP_NOTE_RUN, audio.OP_CONTROL, audio.OP_JUMP}
    assert {c for o, c in ops if o == audio.OP_CONTROL} <= {0, 1, 4}


def test_voice_event_map(banks):
    for name, b in banks.items():
        ev = audio.voice_events(name, b.count("A"))
        assert all(0 <= v < b.count("A") for v in ev)
    g = audio.voice_events("sfxgame")
    assert any(s.startswith("6000") for s in g[0x6F]) and any(s.startswith("5000") for s in g[0x5D])
    assert audio.voice_events("sfxchr01")[3][0].startswith("3006")
    amb = audio.ztrk_voices(banks["sfxlev00"])
    assert amb[20] == [0] and set(amb) == set(range(20, 28))


# ------------------------------------------------------------------ IMA: port of the stream decoder
def port_ima_decode(src, n, state):
    """zsndImaDecode 0x004724C0 (dst = a list of int16)."""
    pred, step, idx = state
    out = [0] * n
    byte = 0
    for i in range(n):
        if i & 1 == 0:
            byte = src[i >> 1]
            nib = byte & 0xF
        else:
            nib = byte >> 4
        diff = step if nib & 4 else 0
        if nib & 2:
            diff += step >> 1
        if nib & 1:
            diff += step >> 2
        diff += step >> 3
        if nib & 8:
            diff = -diff
        pred += diff
        pred = 0x7FFF if pred > 0x7FFF else (-0x8000 if pred < -0x8000 else pred)
        out[i] = pred
        idx += audio.IMA_INDEX[nib]
        idx = 88 if idx > 88 else (0 if idx < 0 else idx)
        step = audio.IMA_STEP[idx]
    return out, (pred, step, idx)


def port_stream_read(data, want_bytes, pos, state):
    """zsndStreamRead 0x00472250, ADPCM branch: 0x4000-byte chunks, state carried."""
    want = want_bytes >> 2
    out = []
    while True:
        n = len(data) - pos
        if min(want, n) > 0x4000:
            n = 0x4000
        elif want < n:
            n = want
        if n == 0:
            return out, pos, state
        chunk = data[pos:pos + n]
        pos += n
        want -= n
        samples, state = port_ima_decode(chunk, len(chunk) * 2, state)
        out += samples


def _ima_check(data):
    # the stream opener's start state: predictor 0, step 7, index 0 (zsndStreamOpenFile / Rewind)
    got, pos, _ = port_stream_read(data, len(data) * 4, 0, (0, 7, 0))
    assert pos == len(data)
    ref = audio.ima_decode(data)
    assert struct.pack(f"<{len(got)}h", *got) == ref


@pytest.fixture(scope="module")
def music(banks):
    m = banks["music"]
    assert len(m.samples) == MUSIC_TRACKS
    return m


def test_ima_port_agrees_with_audio_py_track_starts(music):
    for i in range(MUSIC_TRACKS):
        _ima_check(music.samples[i][:0x20000])        # 8 chunks of 0x4000 per track


# whole tracks: ~100 s, so opt-in (set DMBMX_SLOW=1); conftest.py registers no 'slow' marker
@pytest.mark.skipif(not os.environ.get("DMBMX_SLOW"), reason="slow: set DMBMX_SLOW=1")
@pytest.mark.parametrize("track", range(MUSIC_TRACKS))
def test_ima_port_agrees_with_audio_py_whole_tracks(music, track):
    _ima_check(music.samples[track])


def test_ima_state_transition_is_identical():
    """Exhaustive: for every step index and nibble the port and audio.py move the state the same way."""
    for idx in range(89):
        for nib in range(16):
            for pred in (-0x8000, -12345, -1, 0, 1, 2000, 0x7FFF):
                step = audio.IMA_STEP[idx]
                got, (p2, s2, i2) = port_ima_decode(bytes([nib]), 1, (pred, step, idx))
                # audio.ima_decode starts at (0, step 7); emulate one step from (pred, idx)
                diff = step >> 3
                if nib & 4:
                    diff += step
                if nib & 2:
                    diff += step >> 1
                if nib & 1:
                    diff += step >> 2
                want = max(-0x8000, min(0x7FFF, pred - diff if nib & 8 else pred + diff))
                assert got == [want] and p2 == want
                assert i2 == max(0, min(88, idx + audio.IMA_INDEX[nib])) and s2 == audio.IMA_STEP[i2]


# ------------------------------------------------------------------ audio.py ZTRK codec (synthetic)
def test_ztrk_codec_every_form():
    evs = [audio.ZtrkEvent(audio.OP_CONTROL, 0, 0, 0, bytes([4, 20])),
           audio.ZtrkEvent(audio.OP_NOTE, 1, 0, 200, bytes([60, 100])),
           audio.ZtrkEvent(audio.OP_NOTE, 2, 0, 70000, bytes([61, 90])),
           audio.ZtrkEvent(audio.OP_NOTE_RUN, 3, 9, 0, bytes([62])),
           audio.ZtrkEvent(audio.OP_NOTE_RUN, 4, 10, 11, bytes([63])),
           audio.ZtrkEvent(audio.OP_NOTE_RUN, 5, 12, 100000, bytes([64])),
           audio.ZtrkEvent(audio.OP_MARKER, 6, 300000, 0, bytes([2, 5])),
           audio.ZtrkEvent(0x20, 3, 1, 0, b""),
           audio.ZtrkEvent(audio.OP_INDIRECT, 0, 0, 0, bytes([0x10, 0x02, 1, 2, 3])),
           audio.ZtrkEvent(audio.OP_JUMP, 7, 5, 6, struct.pack("<I", 0))]
    rec = audio.ztrk_encode(evs)
    dec = audio.ztrk_decode(rec)
    assert [(e.op, e.form, e.delta, e.length, e.params) for e in dec] == \
        [(e.op, e.form, e.delta, e.length, e.params) for e in evs]
    assert audio.ztrk_encode(dec) == rec
    assert "end of track" in audio.ZtrkEvent(audio.OP_MARKER, 0, 0, 0, bytes([1, 0])).describe()
    for e in dec:
        assert e.describe()


def test_ztrk_codec_errors():
    with pytest.raises(FormatError):
        audio.ztrk_decode(b"ZTRX")
    with pytest.raises(FormatError):
        audio.ztrk_decode(b"ZTRK\x02\x01\x02")                 # truncated length
    with pytest.raises(FormatError):
        audio.ztrk_decode(b"ZTRK\x48")                         # unknown opcode 0x48
    with pytest.raises(FormatError):
        audio.ztrk_decode(b"ZTRK\x10\x04\x14\x30\x05\x00\x00\x00")   # jump into the middle of an event
    with pytest.raises(ValueError):
        audio.ztrk_encode([audio.ZtrkEvent(audio.OP_NOTE, 0, 5, 0, b"\x3c\x7f")])
    with pytest.raises(ValueError):
        audio.ztrk_encode([audio.ZtrkEvent(audio.OP_NOTE, 4, 300, 0, b"\x3c\x7f")])


def test_ztrk_fuzz(banks):
    b = banks["sfxlev07"]
    s, e = max(b.ztrk_ranges(), key=lambda r: r[1] - r[0])
    fuzz_parser(audio.ztrk_decode, bytes(b.header[s:e]), n=300, seed=5)
    fuzz_parser(audio.ztrk_decode, bytes(banks["sfxlev00"].header[0x470:0x487]), n=200, seed=1)


# ------------------------------------------------------------------ CLI
def test_cli_ztrk_dump_and_names(capsys):
    lev = GAME_DIR / "bin" / "audio" / "sfxlev00.zsd"
    if not lev.is_file():
        pytest.skip("no corpus")
    assert main(["audio", "ztrk-dump", str(lev), "--track", "0"]) == 0
    out = capsys.readouterr().out
    assert "track 0: ZTRK at 0x470" in out and "control voice = 20" in out and "jump -> +0" in out
    assert "note note 60 vel 127  len 303000 ms" in out and "track 1" not in out
    assert main(["audio", "list", str(lev)]) == 0
    out = capsys.readouterr().out
    assert "v20 " in out and "ambience (ZTRK track 0)" in out and "5003 crowd" in out
    assert main(["audio", "ztrk-dump", str(GAME_DIR / "bin" / "audio" / "sfxcom.zsd")]) == 0
    assert "no ZTRK tracks" in capsys.readouterr().out

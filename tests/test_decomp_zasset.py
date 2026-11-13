"""Self-consistency checks for the decomp of zasset.c, zcompres.c, ziff.c.

No C compiler on the build PC, so the C is checked two ways:
1. structure: every function drafted; C names agree with decomp/functions.csv and decomp/symbols/symbols.csv; the
   struct layouts declared in the headers agree with their "+0x.." comments and the size checks;
2. behaviour: line-for-line Python ports of the C (below) are run against the reference
   implementations: the LZO1X decoder over every compressed corpus stream, the LZRW3-A encoder through
   the exe-transcribed decoder, zassetOpenLibrary's header decoding against dmbmx.formats.zal for every
   ZAL, and the ziff chunk walker against a plain chunk-tree parse of real ZMD files.
"""
import csv
import os
import re
import struct
from pathlib import Path

import pytest

from conftest import GAME_DIR, corpus_files
from dmbmx.core import lzrw3a
from dmbmx.formats import zal

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src" / "NgEngine" / "Zge"
INC = REPO / "decomp" / "include"
# The asset / compression / IFF functions live in these files. zanim.c and zrwModelCreate.c received the
# zanim node-table functions and zrwModelCreateRunLinks (they also hold todo stubs).
FILES = ("zasset.c", "zcompres.c", "ziff.c", "zanim.c", "Com/ZglRw/zrwModelCreate.c")
# the rest of zanim.c is checked by test_decomp_zmath.py (which renamed 0x0049A530/40/70); only these five
# zanim.c functions belong here.
ZANIM_NODE_TABLE = {0x0049A4E0, 0x0049A530, 0x0049A540, 0x0049A570, 0x0049A630}
# the ZglRw pass decompiled the rest of zrwModelCreate.c and added two wrappers to zcompres.c (test_decomp_zglrw.py).
ZGLRW_PASS = {"Com/ZglRw/zrwModelCreate.c": lambda va: va != 0x0049C570,
         "zcompres.c": lambda va: va in (0x0049B880, 0x0049B8B0)}


def c_functions():
    """{va: (file, status, name)} for the drafted (non-todo) functions of FILES."""
    out = {}
    for f in FILES:
        t = (SRC / f).read_text(encoding="utf-8")
        for m in re.finditer(r"/\* VA 0x([0-9a-f]{8}), \d+ bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{\n([^\n]*)", t):
            if m.group(2) == "todo" or (f == "zanim.c" and int(m.group(1), 16) not in ZANIM_NODE_TABLE):
                continue
            if f in ZGLRW_PASS and ZGLRW_PASS[f](int(m.group(1), 16)):
                continue
            assert "TODO(decomp)" not in m.group(4), (f, m.group(1))
            name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(3)).group(1)
            out[int(m.group(1), 16)] = (f, m.group(2), name)
    return out


# ------------------------------------------------------------------ structure
def test_all_functions_drafted_and_named_consistently():
    fns = c_functions()
    assert len(fns) == 43   # + zasset_DumpLibraryInfo, zassetSkipAsset / LoadAlloc; ziff +5 (clean-up)
    rows = {int(r["va"], 16): r for r in csv.DictReader(open(REPO / "decomp" / "functions.csv", encoding="utf-8"))}
    for va, (f, status, name) in fns.items():
        r = rows[va]
        assert r["file"] == f"NgEngine/Zge/{f}" and r["status"] == "draft" and status == "draft", hex(va)
        assert r["name"] == name, (hex(va), r["name"], name)
    sym = {}
    for r in csv.DictReader(open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8")):
        if r["kind"] == "func":
            sym[int(r["address"], 16)] = r["name"]          # later rows win, as in ApplySymbols
    for va, (_f, _s, name) in fns.items():
        assert sym.get(va) == name, (hex(va), sym.get(va), name)
    assert {n for _f, _s, n in fns.values() if n.startswith("ziff")} >= {"ziffChunkReadData", "ziffOpen", "ziffParse"}


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4, "ZiffHandlerFn": 4}


def layouts(header):
    """{struct: (size, [(name, offset, commented_offset)])} computed from the declarations (x86, natural alignment)."""
    t = (INC / header).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    out = {}
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields = 0, []
        for line in m.group(2).splitlines():
            d = re.match(r"\s*((?:struct\s+)?\w+)\s*(\*?)\s*(\w+)(?:\[(\w+)\])?;\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?", line)
            if not d:
                continue
            typ, ptr, name, count, comment = d.groups()
            typ = typ.replace("struct ", "")
            size = 4 if ptr else SIZES.get(typ) or out[typ][0]
            align = 4 if ptr else min(size, 4)
            off = (off + align - 1) // align * align
            fields.append((name, off, int(comment, 16) if comment else None))
            off += size * (int(count, 0) if count else 1)
        out[m.group(3)] = ((off + 3) // 4 * 4, fields)
    return out


@pytest.mark.parametrize("header", ["ziff.h", "zasset.h", "zanim.h"])
def test_struct_layouts(header):
    lay = layouts(header)
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    assert lay
    for name, (size, fields) in lay.items():
        assert size == want[name], (name, hex(size))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))


# ------------------------------------------------------------------ behaviour: zcompres.c ports
def c_lzo_decompress(src):
    """Port of zcompresDecompress (method 2)."""
    ip, op = 0, bytearray()
    LIT, FIRST, TRAIL = 0, 1, 2
    state, t = LIT, 0
    if src[ip] > 17:
        t = src[ip] - 17
        ip += 1
        if t < 4:
            state = TRAIL
        else:
            op += src[ip:ip + t]
            ip += t
            state = FIRST
    while True:
        if state == TRAIL:
            op += src[ip:ip + t]
            ip += t
            t = src[ip]
            ip += 1
        else:
            t = src[ip]
            ip += 1
            if t < 16:
                if state == LIT:
                    if t == 0:
                        t = 15
                        while src[ip] == 0:
                            t += 255
                            ip += 1
                        t += src[ip]
                        ip += 1
                    t += 3
                    op += src[ip:ip + t]
                    ip += t
                    state = FIRST
                    continue
                m = len(op) - (1 + 0x800) - (t >> 2) - (src[ip] << 2)
                ip += 1
                for _ in range(3):
                    op.append(op[m])
                    m += 1
                t = src[ip - 2] & 3
                state = TRAIL if t else LIT
                continue
        if t >= 64:
            m = len(op) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
            ip += 1
            n = (t >> 5) + 1
        elif t >= 32:
            n = t & 31
            if n == 0:
                n = 31
                while src[ip] == 0:
                    n += 255
                    ip += 1
                n += src[ip]
                ip += 1
            n += 2
            m = len(op) - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6))
            ip += 2
        elif t >= 16:
            dist = (t & 8) << 11
            n = t & 7
            if n == 0:
                n = 7
                while src[ip] == 0:
                    n += 255
                    ip += 1
                n += src[ip]
                ip += 1
            n += 2
            dist += (src[ip] >> 2) + (src[ip + 1] << 6)
            ip += 2
            if dist == 0:
                break
            m = len(op) - dist - 0x4000
        else:
            m = len(op) - 1 - (t >> 2) - (src[ip] << 2)
            ip += 1
            n = 2
        for _ in range(n):
            op.append(op[m])
            m += 1
        t = src[ip - 2] & 3
        state = TRAIL if t else LIT
    return bytes(op), ip


def c_lzrw3a_compress(src):
    """Port of zcompresCompressLZRW3A."""
    start = b"123456789012345678\0"
    table = [-1] * 4096
    n = len(src)
    out = bytearray(4)
    control = len(out)
    out += b"\0\0"
    ctrl = nbits = cycle = p = 0
    pend1 = pend2 = None

    def at(q, k):
        return start[k] if q < 0 else src[q + k]
    while p < n:
        if len(out) > n:
            return bytes([1, 0, 0, 0]) + src
        best, bslot, part = 0, 0, None
        if p <= n - 18:
            part = lzrw3a._hash(src, p)
            for s in range(8):
                if best == 18:
                    break
                q = table[part + s]
                if src[p + best] != at(q, best):
                    continue
                k = 0
                while k < 18 and src[p + k] == at(q, k):
                    k += 1
                if k > best:
                    best, bslot = k, s
        if best >= 3:
            index = part + bslot
            out += bytes([((index >> 4) & 0xF0) | (best - 3), index & 0xFF])
            ctrl |= 1 << nbits
            if pend1 is not None:
                if pend2 is not None:
                    table[pend2 + cycle] = p - 2
                    cycle = (cycle + 1) & 7
                    pend2 = None
                table[pend1 + cycle] = p - 1
                cycle = (cycle + 1) & 7
                pend1 = None
            table[part + cycle] = p
            cycle = (cycle + 1) & 7
            p += best
        else:
            out.append(src[p])
            if pend2 is not None:
                table[pend2 + cycle] = p - 2
                cycle = (cycle + 1) & 7
            pend2, pend1 = pend1, part
            p += 1
        nbits += 1
        if nbits == 16:
            out[control:control + 2] = ctrl.to_bytes(2, "little")
            control = len(out)
            out += b"\0\0"
            ctrl = nbits = 0
    out[control:control + 2] = ctrl.to_bytes(2, "little")
    if nbits == 0:
        del out[-2:]
    return bytes(out)


def corpus_compressed_assets():
    for name in ("material.zal", "motion.zal", "Intwrap.zal"):
        stack = [zal.read((GAME_DIR / "assets" / name).read_bytes())]
        while stack:
            a = stack.pop()
            for e in a.entries:
                for asset in e.assets:
                    data = a.asset_data(asset)
                    if a.compressed:
                        yield asset.stored, data
                    if data[:4] == zal.MAGIC_BYTES:
                        stack.append(zal.read(data))


def test_lzo_port_matches_corpus(game_dir):
    n = 0
    for stored, data in corpus_compressed_assets():
        got, used = c_lzo_decompress(stored)
        assert got == data and used == len(stored)
        n += 1
    assert n == 1298


@pytest.mark.parametrize("seed", range(4))
def test_lzrw3a_port_roundtrips(seed):
    rnd = __import__("random").Random(seed)
    words = [bytes(rnd.randrange(97, 123) for _ in range(rnd.randrange(1, 8))) for _ in range(40)]
    data = b" ".join(rnd.choice(words) for _ in range(rnd.randrange(0, 3000)))
    if seed == 3:
        data = os.urandom(800)
    z = c_lzrw3a_compress(data)
    assert lzrw3a.decompress(z, len(data)) == data


# ------------------------------------------------------------------ behaviour: zasset.c port
def c_zasset_header(raw):
    """Port of zassetOpenLibrary's header decoding."""
    shift = 0 if struct.unpack_from("<I", raw, 0)[0] == 0x2A81511C else -8
    flags, count = struct.unpack_from("<HH", raw, 0x10 + shift)
    compressed = ((flags >> 15) & 1) * 2 + ((count >> 15) & 1)
    entry = 12 if compressed else 8
    return dict(compressed=compressed, method=(count >> 14) & 1, entrySize=entry, numGroups=count & 0x3FFF,
                assetsPerGroup=flags & 0x1FFF, refMode=(flags >> 13) & 1, dirOffset=0x14 + shift,
                dirSize=((flags & 0x1FFF) * entry + 4) * (count & 0x3FFF))


@pytest.mark.parametrize("path", [pytest.param(p, id=p.name) for p in corpus_files(".zal")] or
                         [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))])
def test_zasset_port_agrees_with_zal_py(path):
    data = path.read_bytes()
    h = c_zasset_header(data[:0x1C])
    arc = zal.read(data)
    assert h["numGroups"] == len(arc.entries) and h["assetsPerGroup"] == arc.assets_per_group
    assert bool(h["compressed"]) == arc.compressed and h["refMode"] == bool(arc.ref_mode)
    assert h["entrySize"] * h["assetsPerGroup"] + 4 == arc.record_size
    if arc.compressed:
        assert (zal.METHOD_LZO1X if h["method"] else zal.METHOD_1) == arc.method
    directory = data[h["dirOffset"]:h["dirOffset"] + h["dirSize"]]
    group_size = h["entrySize"] * h["assetsPerGroup"] + 4
    for g, entry in enumerate(arc.entries):                     # zassetFindAsset + LoadAsset offsets
        base = g * group_size
        assert struct.unpack_from("<I", directory, base)[0] == entry.id
        off, size = struct.unpack_from("<II", directory, base + 4)
        assert size & 0xFFFFFF == entry.assets[0].size and data[off + 0x10:off + 0x10 + 4] == entry.assets[0].stored[:4]


# ------------------------------------------------------------------ behaviour: ziff.c port
class CZiff:
    """Port of ziffOpen / ziffChunkOpen / ziffChunkClose / ziffChunkFindFirst / FindNext over bytes."""

    def __init__(self, data, root_id):
        self.d, self.stack = data, []
        self.root = struct.unpack_from("<II", data, 0)
        assert self.root[0] == root_id
        self.pos = 8
        self.open(root=True)

    def header(self):
        c = self.stack[-1]
        c["childStart"] = self.pos
        c["childId"], c["childSize"] = struct.unpack_from("<II", self.d, self.pos)
        self.pos += 8

    def seek(self, mode, off):
        c = self.stack[-1]
        pos = c["start"] + 8 + off if mode == 0 else self.pos + off
        assert c["start"] <= pos <= c["end"]
        self.pos = pos
        return -1 if pos == c["end"] else pos

    def open(self, root=False):
        if root:
            c = dict(id=self.root[0], size=self.root[1], start=0)
        else:
            p = self.stack[-1]
            assert p["childStart"] + 8 == self.pos and len(self.stack) < 8
            c = dict(id=p["childId"], size=p["childSize"], start=self.pos - 8)
        c["end"] = c["start"] + 8 + c["size"]
        self.stack.append(c)
        self.pos = c["start"]
        self.header()

    def close(self):
        self.stack.pop()

    def children(self):
        """FindFirst(ANY) / FindNext(ANY) loop as game code uses it."""
        ids = []
        if self.seek(0, 0) == -1:                            # empty chunk
            return ids
        self.header()
        while True:
            c = self.stack[-1]
            ids.append((c["childId"], c["childStart"]))
            if self.seek(0, (c["childStart"] - c["start"]) + c["childSize"]) == -1:
                return ids                                    # ziffChunkFindNext returns 0
            self.header()


CONTAINERS = {b"MODL", b"MLOD", b"MATI"}


def plain_tree(d, start, end):
    out, o = [], start
    while o + 8 <= end:
        tag, size = d[o:o + 4], struct.unpack_from("<I", d, o + 4)[0]
        out.append((tag, o, plain_tree(d, o + 8, o + 8 + size) if tag in CONTAINERS else None))
        o += 8 + size
    return out


def ziff_tree(z):
    out = []
    for cid, cstart in z.children():
        tag = struct.pack("<I", cid)
        sub = None
        if tag in CONTAINERS:
            z.seek(0, cstart - z.stack[-1]["start"] - 8)      # back onto the child header (data-relative)
            z.header()
            z.open()
            sub = ziff_tree(z)
            z.close()
        out.append((tag, cstart, sub))
    return out


def test_ziff_port_walks_zmd_like_a_plain_parser(game_dir):
    files = [p for p in sorted(corpus_files(".zmd"), key=lambda p: p.stat().st_size)
             if p.read_bytes()[:4] == b"MODL"][:12]
    assert files
    for p in files:
        d = p.read_bytes()
        z = CZiff(d, struct.unpack("<I", b"MODL")[0])
        assert ziff_tree(z) == plain_tree(d, 8, 8 + struct.unpack_from("<I", d, 4)[0]), p.name
        assert len(z.stack) == 1                              # ziffClose's "Chunks still open" check

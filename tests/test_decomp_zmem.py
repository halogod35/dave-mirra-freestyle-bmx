"""Self-consistency checks for the decomp of zmem.c, zList.c, zstring.c, zutil.c, zdebug.c, NgIfzmemDMM.c
(+ the assert handler in zrwZgl.c).

No C compiler on the build PC, so the C is checked two ways:
1. structure: every function drafted, names agree with decomp/functions.csv and decomp/symbols/symbols.csv, no
   Ghidra artefacts, struct layouts agree with their "+0x.." comments and the size checks in
   ghidra_types.txt, every assert in the C (message, file, line) exists as a call site in NgBMX.exe,
   and the zutil plane table matches the exe;
2. behaviour: line-for-line Python ports of the allocators (memory pool, free list, DMM), the heap
   list and zHashName are run on random traces with invariant checks, and zHashName against
   dmbmx.core.hash.zhash (the corpus-verified hash).
"""
import csv
import random
import re
import struct
from pathlib import Path

import pytest

from dmbmx.core.hash import zhash

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "decomp" / "src"
INC = REPO / "decomp" / "include"
FILES = ["NgEngine/Zge/zmem.c", "NgEngine/Zge/zList.c", "NgEngine/Zge/zstring.c", "NgEngine/Zge/zutil.c",
         "NgEngine/Zge/zdebug.c", "zaxis/Bmx/Src/com/zge/NgIF/NgIfzmemDMM.c"]
ZGL = "NgEngine/Zge/Com/ZglRw/zrwZgl.c"
COUNTS = {"NgEngine/Zge/zmem.c": 41,  # + zmemDump, zmemSetHeader
           # 0x00486910/0x00486940 moved to zprop.c
          "NgEngine/Zge/zList.c": 20, "NgEngine/Zge/zstring.c": 39,
          "NgEngine/Zge/zutil.c": 10,  # + the five box helpers 0x004A2C70-0x004A2E10 "NgEngine/Zge/zdebug.c": 7,
          "zaxis/Bmx/Src/com/zge/NgIF/NgIfzmemDMM.c": 11}  # + zmemDMMStats 0x0046B730
ZGL_DRAFT = {0x00489CC0: "zrwZglErrorBox", 0x00489CE0: "zrwZglDebugString", 0x00489CF0: "zAssertMessageBox"}
VA_RE = re.compile(r"/\* VA 0x([0-9a-f]{8}), (\d+) bytes, [^\n]*status (\w+) \*/\n([^\n]+)\n\{")


def csv_rows():
    with open(REPO / "decomp" / "functions.csv", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def c_functions(f):
    """[(va, size, status, name)] in source order."""
    out = []
    for m in VA_RE.finditer((SRC / f).read_text(encoding="utf-8")):
        name = re.match(r"^.*?([A-Za-z_]\w*)\s*\(", m.group(4)).group(1)
        out.append((int(m.group(1), 16), int(m.group(2)), m.group(3), name))
    return out


def last_symbol_names():
    """{va: name} of func rows in decomp/symbols/symbols.csv, later rows win (as in ApplySymbols)."""
    out = {}
    with open(REPO / "decomp" / "symbols" / "symbols.csv", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == "func":
                out[int(r["address"], 16)] = r["name"]
    return out


# ------------------------------------------------------------------ structure
def test_every_function_drafted_and_named_consistently():
    rows = csv_rows()
    syms = last_symbol_names()
    for f, n in COUNTS.items():
        fr = [r for r in rows if r["file"] == f]
        assert len(fr) == n, (f, len(fr))
        cf = c_functions(f)
        assert [va for va, *_ in cf] == sorted(int(r["va"], 16) for r in fr), f
        by_va = {int(r["va"], 16): r for r in fr}
        for va, size, status, name in cf:
            r = by_va[va]
            assert status == "draft" and r["status"] == "draft", (f, hex(va))
            assert int(r["size"]) == size and r["name"] == name, (hex(va), r["name"], name)
            assert syms[va] == name, (hex(va), syms[va], name)
    zgl = {va: (status, name) for va, _s, status, name in c_functions(ZGL)}
    for va, name in ZGL_DRAFT.items():
        assert zgl[va] == ("draft", name)
        assert syms[va] == name


def test_renamed_helpers_used_consistently():
    """The earlier names zmemPool* (free lists) and zmemHeap* (memory pools) became zmemFreeList* and
    zmemPool*; every caller in decomp/src must use the new names with the new meaning."""
    for p in SRC.rglob("*.c"):
        t = p.read_text(encoding="utf-8")
        assert "zmemHeap" not in t and "zMemTemp" not in t, p
    for f in ["NgEngine/Zge/zfile.c", "NgEngine/Zge/zasset.c", "NgEngine/Zge/ziff.c", "NgEngine/Zge/ZgeMods/zramdisk.c"]:
        t = (SRC / f).read_text(encoding="utf-8")
        assert re.search(r"zmemFreeList(Create|Alloc|Free)\(", t), f
        # memory-pool calls only with the zfile heap global
        for m in re.finditer(r"zmemPool\w*\(([^,)]*)", t):
            assert m.group(1).strip() == "g_zfileHeap", (f, m.group(0))


def test_no_ghidra_artefacts():
    bad = re.compile(r"\b(undefined[1-8]?|FUN_[0-9a-f]{8}|param_\d+|DAT_[0-9a-f]{8}|LAB_[0-9a-f]{8}|in_stack_\w+|"
                     r"[iu]Var\d+|pc?Var\d+|(?:zmem|zstring|zList|zutil|zdebug|NgIfzmemDMM)_[0-9a-f]{8})\b")
    texts = [(f, (SRC / f).read_text(encoding="utf-8")) for f in FILES]
    zgl = (SRC / ZGL).read_text(encoding="utf-8")
    texts.append((ZGL, zgl[:zgl.index("/* VA 0x00489d70")]))
    for f, t in texts:
        assert "TODO(decomp)" not in t, f
        for n, line in enumerate(t.splitlines(), 1):
            assert not bad.search(line), f"{f}:{n}: {line.strip()}"


SIZES = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
         "int": 4, "float": 4}
FNPTR_TYPES = {"ZmemAllocFn", "ZmemFreeFn", "ZmemReallocFn", "ZmemFreeListCallback", "zHeapListFn"}


def layouts(header):
    """{struct: (size, [(field, offset, commented_offset)])} from the declarations (x86, natural alignment)."""
    t = (INC / header).read_text(encoding="utf-8")
    t = re.sub(r"/\* GHIDRA-SKIP-BEGIN \*/.*?/\* GHIDRA-SKIP-END \*/", "", t, flags=re.S)
    out = {}
    for m in re.finditer(r"typedef struct (\w+) \{(.*?)\} (\w+);", t, re.S):
        off, fields = 0, []
        for line in m.group(2).splitlines():
            d = re.match(r"\s*(?:const\s+)?((?:struct\s+)?\w+)\s*(\*{0,2})\s*(\w+)(?:\[(\w+)\])?;\s*(?:/\*\s*\+0x([0-9A-Fa-f]+))?", line)
            if not d:
                continue
            typ, ptr, name, count, comment = d.groups()
            typ = typ.replace("struct ", "")
            size = 4 if (ptr or typ in FNPTR_TYPES) else SIZES.get(typ) or out[typ][0]
            align = 4 if (ptr or typ in FNPTR_TYPES) else min(size, 4)
            off = (off + align - 1) // align * align
            fields.append((name, off, int(comment, 16) if comment else None))
            off += size * (int(count, 0) if count else 1)
        out[m.group(3)] = ((off + 3) // 4 * 4, fields)
    return out


def ghidra_sizes():
    want = {}
    for line in (INC / "ghidra_types.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("size "):
            _, n, v = line.split()
            want[n] = int(v, 16)
    return want


@pytest.mark.parametrize("header,structs", [
    ("zmem.h", {"ZmemBlock", "ZmemPoolFuncs", "ZmemPool", "ZmemFreeNode", "ZmemFreeList", "ZmemGlobals",
                "ZmemDMMHandle", "ZmemDMMFree", "ZmemDMMGlobals"}),
    ("zlist.h", {"zListNode", "zList", "zHeapList"}),
    ("zstring.h", {"ZstringSet", "ZfxRegEntry"}),
])
def test_struct_layouts(header, structs):
    lay = layouts(header)
    want = ghidra_sizes()
    assert set(lay) == structs
    assert header in (INC / "ghidra_types.txt").read_text(encoding="utf-8").split()
    for name, (size, fields) in lay.items():
        assert size == want[name], (name, hex(size))
        for fname, off, comment in fields:
            if comment is not None:
                assert off == comment, (name, fname, hex(off), hex(comment))
    # the C-side size checks list the same sizes
    t = (INC / header).read_text(encoding="utf-8")
    for name in structs:
        assert re.search(r"sizeof\(%s\) == 0x%02X\b" % (name, want[name]), t), name


def c_asserts():
    """{(path, msg, line)} for every literal assert in this C."""
    out = set()
    texts = [(SRC / f).read_text(encoding="utf-8") for f in FILES]
    for t in texts:
        defs = {m.group(1): m.group(2).replace("\\\\", "\\") for m in re.finditer(r'#define (\w+_SRC) "([^"]+)"', t)}
        for m in re.finditer(r'(?:zAssertFail|g_assertHandler)\(\s*"((?:[^"\\]|\\.)*)",\s*(\w+),\s*(0x[0-9a-f]+)', t):
            out.add((defs[m.group(2)], m.group(1).replace("\\n", "\n"), int(m.group(3), 16)))
        for m in re.finditer(r'ZASSERT\([^;]*?,\s*"((?:[^"\\]|\\.)*)",\s*(\w+),\s*(0x[0-9a-f]+)\)', t):
            out.add((defs[m.group(2)], m.group(1).replace("\\n", "\n"), int(m.group(3), 16)))
    return out


def load_image(game_dir):
    exe = game_dir / "NgBMX.exe"
    if not exe.is_file():
        pytest.skip("NgBMX.exe not found")
    pefile = pytest.importorskip("pefile")
    pe = pefile.PE(str(exe), fast_load=True)
    return pe.get_memory_mapped_image()


def push(v):
    return bytes([0x6A, v]) if v < 0x80 else b"\x68" + struct.pack("<I", v)


def test_assert_sites_exist_in_exe(game_dir):
    img = load_image(game_dir)
    sites = c_asserts()
    assert len(sites) == 41
    for path, msg, line in sorted(sites):
        p_addr = img.find(path.encode() + b"\0") + 0x400000
        assert p_addr > 0x400000, path
        addrs = [m.start() + 0x400000 for m in re.finditer(re.escape(msg.encode() + b"\0"), img)]
        assert addrs, msg
        pats = [push(line) + b"\x68" + struct.pack("<I", p_addr) + b"\x68" + struct.pack("<I", a) for a in addrs]
        assert any(p in img for p in pats), (path, msg, hex(line))


def test_bbox_plane_table_matches_exe(game_dir):
    img = load_image(game_dir)
    t = (SRC / "NgEngine/Zge/zutil.c").read_text(encoding="utf-8")
    body = re.search(r"g_zutilBBoxPlaneNormals\[6\]\[4\] = \{(.*?)\n\};", t, re.S).group(1)
    vals = [float(x.rstrip("f")) for x in re.findall(r"-?\d+\.\d+f", body)]
    assert vals == list(struct.unpack("<24f", img[0x163770:0x163770 + 96]))


def test_zassert_handler_pointer(game_dir):
    img = load_image(game_dir)
    assert struct.unpack_from("<I", img, 0x16147C)[0] == 0x00489CF0     # g_assertHandler
    assert struct.unpack_from("<I", img, 0x1611D8)[0] == 0x00561204     # title pointer
    assert img[0x161204:0x161225] == b"RenderWare(tm) Application Error\0"


# ------------------------------------------------------------------ behaviour: Python ports
class AssertFail(Exception):
    pass


class Mem:
    """Little-endian 32-bit memory at a fake base address."""
    def __init__(self, size, base=0x10000):
        self.b = bytearray(size)
        self.base = base

    def rd(self, a):
        return struct.unpack_from("<I", self.b, a - self.base)[0]

    def wr(self, a, v):
        struct.pack_into("<I", self.b, a - self.base, v & 0xFFFFFFFF)

    def read(self, a, n):
        return bytes(self.b[a - self.base:a - self.base + n])

    def write(self, a, data):
        self.b[a - self.base:a - self.base + len(data)] = data


class PoolPort:
    """Port of zmemPoolCreate/Alloc/Realloc/Free/Walk (0x00486410 ...). Block: next @0, prev|free @4."""
    MIN, SPLIT = 0x20, 0x28

    def __init__(self, size, garbage=0):
        self.m = Mem(size + 64)
        self.m.b[:] = bytes([garbage]) * len(self.m.b)   # backing memory is not cleared
        size = self.MIN if size < self.MIN else (size + 3) & ~3
        first = self.m.base
        sentinel = first + size - 8
        m = self.m
        m.wr(sentinel + 4, m.rd(sentinel + 4) & ~1)
        m.wr(first + 4, m.rd(first + 4) | 1)
        m.wr(sentinel, 0)
        m.wr(first, sentinel)
        m.wr(first + 4, m.rd(first + 4) & 1)
        self.set_prev(sentinel, first)
        self.first, self.sentinel = first, sentinel

    def set_prev(self, b, p):
        self.m.wr(b + 4, (self.m.rd(b + 4) & 1) | p)

    def prev(self, b):
        return self.m.rd(b + 4) & ~1

    def free_(self, b):
        return self.m.rd(b + 4) & 1

    def bsize(self, b):
        return self.m.rd(b) - b - 8

    def alloc(self, size):
        m = self.m
        payload = self.MIN if size < self.MIN else (size + 3) & ~3
        need = payload + 8
        best, best_size = 0, 0xFFFFFFFF
        b = self.first
        while b:
            if self.free_(b):
                avail = self.bsize(b)
                if avail < best_size and avail >= need:
                    best, best_size = b, avail
                    if avail == need:
                        break
            b = m.rd(b)
        if not best:
            return 0
        if best_size - need >= self.SPLIT:
            rest = best + 8 + payload
            m.wr(rest, m.rd(best))
            self.set_prev(m.rd(best), rest)
            m.wr(best, rest)
            self.set_prev(rest, best)
            m.wr(rest + 4, m.rd(rest + 4) | 1)
        m.wr(best + 4, m.rd(best + 4) & ~1)
        return best + 8

    def split_after(self, blk, payload, nxt):
        m = self.m
        rest = blk + 8 + payload
        self.set_prev(rest, blk)
        m.wr(blk, rest)
        self.set_prev(nxt, rest)
        m.wr(rest, nxt)
        m.wr(rest + 4, m.rd(rest + 4) | 1)

    def check_range(self, p):
        if p < self.first + 8 or p >= self.sentinel:
            raise AssertFail("Memory being released is not from this pool.")

    def realloc(self, p, size):
        m = self.m
        if p == 0:
            return 0
        self.check_range(p)
        payload = self.MIN if size < self.MIN else (size + 3) & ~3
        blk = p - 8
        nxt = m.rd(blk)
        avail = self.bsize(blk)
        if payload <= avail:
            if avail - payload >= self.SPLIT:
                self.split_after(blk, payload, m.rd(nxt) if self.free_(nxt) else nxt)
            return p
        if self.free_(nxt):
            total = (m.rd(nxt) - nxt) + avail
            if total >= payload:
                if total - payload >= self.SPLIT:
                    self.split_after(blk, payload, m.rd(nxt))
                else:
                    m.wr(blk, m.rd(nxt))
                    self.set_prev(m.rd(nxt), blk)
                return p
        prev = self.prev(blk)
        if prev and self.free_(prev):
            total = self.bsize(prev) + avail
            if total >= payload:
                if total - payload >= self.MIN:
                    moved = prev + 8 + (total - payload)
                    self.set_prev(moved, prev)
                    m.wr(prev, moved)
                    self.set_prev(nxt, moved)
                    m.wr(moved, nxt)
                    m.wr(moved + 4, m.rd(moved + 4) & ~1)
                else:
                    m.wr(prev, nxt)
                    self.set_prev(nxt, prev)
                    m.wr(prev + 4, m.rd(prev + 4) & ~1)
                    moved = prev
                m.write(moved + 8, m.read(p, size))            # memmove(moved + 8, p, size)
                return moved + 8
        q = self.alloc(size)
        if q == 0:
            return 0
        m.write(q, m.read(p, size))
        self.free(p)
        return q

    def free(self, p):
        m = self.m
        if p == 0:
            return
        self.check_range(p)
        blk = p - 8
        if self.free_(blk):
            raise AssertFail("zmemPoolFree(): Memory has been previously freed.")
        m.wr(blk + 4, m.rd(blk + 4) | 1)
        prev = self.prev(blk)
        nxt = m.rd(blk)
        if self.free_(nxt):
            nxt = m.rd(nxt)
            m.wr(blk, nxt)
            self.set_prev(nxt, blk)
        if prev and self.free_(prev):
            m.wr(prev, nxt)
            self.set_prev(nxt, prev)

    def walk(self):
        out, b = [], self.first
        while b != self.sentinel:
            out.append((b + 8, self.bsize(b), self.free_(b)))
            b = self.m.rd(b)
        return out


def check_pool(pool, live):
    """Blocks tile [first, sentinel) with consistent prev links, no two free neighbours, and every
    live allocation lies inside exactly one used block with its contents intact."""
    m = pool.m
    b, prev, last_free = pool.first, 0, False
    used = {}
    while b != pool.sentinel:
        nxt = m.rd(b)
        assert pool.first <= b < nxt <= pool.sentinel
        assert pool.prev(b) == prev
        f = pool.free_(b)
        assert not (f and last_free), "two adjacent free blocks"
        if not f:
            used[b + 8] = nxt - b - 8
        prev, last_free, b = b, f, nxt
    assert pool.prev(pool.sentinel) == prev and not pool.free_(pool.sentinel) and m.rd(pool.sentinel) == 0
    assert set(used) == set(live)
    for p, data in live.items():
        assert len(data) <= used[p]
        assert m.read(p, len(data)) == data


@pytest.mark.parametrize("seed", range(6))
def test_pool_port_random_trace(seed):
    rnd = random.Random(seed)
    pool = PoolPort(0x4000 + seed * 0x100, garbage=0xCD)
    live = {}
    for step in range(1500):
        op = rnd.random()
        if op < 0.45 or not live:
            n = rnd.choice([1, 7, 0x20, 0x21, 0x40, 0x7F, 0x100, 0x333])
            p = pool.alloc(n)
            if p:
                data = bytes(rnd.randrange(256) for _ in range(n))
                pool.m.write(p, data)
                live[p] = data
        elif op < 0.75:
            p = rnd.choice(sorted(live))
            pool.free(p)
            del live[p]
        else:
            p = rnd.choice(sorted(live))
            old = live.pop(p)
            n = rnd.choice([1, 0x10, 0x30, 0x80, 0x200, len(old) + 4, max(1, len(old) - 8)])
            q = pool.realloc(p, n)
            if q == 0:
                live[p] = old          # failed: the old block is untouched
            else:
                keep = old[:min(len(old), n)]
                assert pool.m.read(q, len(keep)) == keep, (step, hex(p), hex(q))
                data = keep + bytes(rnd.randrange(256) for _ in range(n - len(keep)))
                pool.m.write(q, data)
                live[q] = data
        check_pool(pool, live)
    for p in sorted(live):
        pool.free(p)
    assert pool.walk() == [(pool.first + 8, pool.sentinel - pool.first - 8, 1)]


def test_pool_port_documented_quirks():
    pool = PoolPort(0x200)
    # an exact fit of the payload alone is not taken: the whole pool minus one header cannot be allocated
    assert pool.alloc(0x200 - 16) == 0
    assert pool.alloc(0x200 - 24) != 0
    pool = PoolPort(0x200)
    p = pool.alloc(0x40)
    pool.free(p)
    with pytest.raises(AssertFail, match="previously freed"):
        pool.free(p)
    assert pool.realloc(0, 0x10) == 0


class FreeListPort:
    """Port of zmemFreeListCreate/Alloc/Free/ForEach (node: next @0, prev @4, data @8)."""
    def __init__(self, elem, count):
        self.elem = (elem + 3) & ~3
        self.grow = max(count, 0x20)
        self.nodes = {}          # node address -> [next, prev]
        self.chunks = []
        self.next_addr = 0x100000
        self.free_head = self.new_chunk(self.grow)
        self.used_head = 0
        self.used = 0
        self.free = self.grow

    def new_chunk(self, n):
        base = self.next_addr + 4
        stride = self.elem + 8
        self.next_addr += 4 + n * stride + 0x100
        self.chunks.append(base - 4)
        for i in range(n):
            self.nodes[base + i * stride] = [base + (i + 1) * stride if i + 1 < n else 0, 0]
        return base

    def alloc(self):
        if self.free_head == 0:
            if self.free != 0:
                raise AssertFail("Free list should be empty")
            self.grow *= 2
            self.free_head = self.new_chunk(self.grow)
            self.free = self.grow
        node = self.free_head
        self.free_head = self.nodes[node][0]
        self.nodes[node] = [self.used_head, 0]
        if self.used_head:
            self.nodes[self.used_head][1] = node
        self.used_head = node
        self.used += 1
        self.free -= 1
        return node + 8

    def release(self, elem):
        node = elem - 8
        it = self.used_head
        while it and it != node:
            it = self.nodes[it][0]
        if it == 0:
            raise AssertFail("Invalid block released from free list.")
        nxt, prev = self.nodes[node]
        if node == self.used_head:
            assert prev == 0
            self.used_head = nxt
        if prev:
            self.nodes[prev][0] = nxt
        if nxt:
            self.nodes[nxt][1] = prev
        self.nodes[node] = [self.free_head, prev]
        self.free_head = node
        self.used -= 1
        self.free += 1

    def used_list(self):
        out, it = [], self.used_head
        while it:
            out.append(it + 8)
            it = self.nodes[it][0]
        return out


@pytest.mark.parametrize("seed", range(4))
def test_free_list_port_random_trace(seed):
    rnd = random.Random(seed)
    fl = FreeListPort(rnd.choice([1, 8, 0x14, 0x108]), rnd.choice([1, 8, 0x40]))
    live = []
    for _ in range(2000):
        if rnd.random() < 0.55 or not live:
            e = fl.alloc()
            assert e not in live
            live.insert(0, e)       # newest first, like the used list
        else:
            e = rnd.choice(live)
            fl.release(e)
            live.remove(e)
        assert fl.used_list() == live
        assert fl.used == len(live)
        assert fl.used + fl.free == sum(fl.grow // 2 ** i for i in range(len(fl.chunks)))   # chunks double
        # every node is either used or on the free chain exactly once
        free_chain, it = [], fl.free_head
        while it:
            free_chain.append(it)
            it = fl.nodes[it][0]
        assert len(free_chain) == fl.free
        assert not set(free_chain) & {e - 8 for e in live}
    with pytest.raises(AssertFail):
        fl.release(0xDEAD08)


class HeapListPort:
    """Port of zHeapListAdd/First/Next/RemoveCurrent/RemoveAt (zList.c)."""
    def __init__(self, cap):
        self.items, self.count, self.cap, self.cursor = [0] * cap, 0, cap, -1

    def add(self, x):
        if self.count == self.cap:
            self.items = self.items[:self.cap] + [0] * self.cap
            self.cap *= 2
        if self.cursor == -1:
            self.items[self.count] = x
            self.count += 1
        else:
            self.items[self.count] = self.items[self.cursor]
            self.count += 1
            self.items[self.cursor] = x

    def first(self):
        self.cursor = -1
        if self.count <= 0:
            return 0
        self.cursor = 0
        return self.items[0]

    def next(self):
        if self.cursor >= 0:
            self.cursor += 1
            if self.cursor < self.count:
                return self.items[self.cursor]
        self.cursor = -1
        return 0

    def remove_current(self):
        if self.cursor == -1:
            self.count -= 1
            x, self.items[self.count] = self.items[self.count], 0
            return x
        x = self.items[self.cursor]
        self.count -= 1
        self.items[self.cursor] = self.items[self.count]
        self.items[self.count] = 0
        return x


def test_heap_list_port():
    hl = HeapListPort(2)
    for i in range(1, 6):
        hl.add(i)
    assert hl.cap == 8 and hl.items[:5] == [1, 2, 3, 4, 5]
    # adding during an iteration: the new item takes the current slot, the current one moves to the end
    assert hl.first() == 1 and hl.next() == 2
    hl.add(9)
    seen = [hl.next() for _ in range(5)]
    assert seen == [3, 4, 5, 2, 0]
    # the zfx registry shutdown idiom (First / RemoveCurrent / First) empties the list
    got = []
    x = hl.first()
    while x:
        got.append(hl.remove_current())
        x = hl.first()
    assert sorted(got) == [1, 2, 3, 4, 5, 9] and hl.count == 0


def zhash_port(name: bytes):
    """Port of zHashName 0x004828C0 (toupper on a signed char, as MSVC's CRT with the C locale)."""
    def up(c):
        c = c - 256 if c >= 128 else c
        return c - 32 if ord("a") <= c <= ord("z") else c
    h = 0
    for i, c in enumerate(name, 1):
        h += up(c) * i
    prev = 0
    for c in name:
        h += (up(c) + prev * 0xFFFF) << 16
        prev = up(c)
    return h & 0xFFFFFFFF


def test_zhash_port_matches_dmbmx():
    rnd = random.Random(7)
    names = [b"Hips", b"_LeftShoulder", b"wheel", b"", b"a", b"SFXCube"]
    names += [bytes(rnd.randrange(1, 256) for _ in range(rnd.randrange(1, 40))) for _ in range(500)]
    for n in names:
        assert zhash_port(n) == zhash(n), n


class DMMPort:
    """Port of NgIfzmemDMM.c. Free header: prev @0, next @4, size @8; handle: used @0, lock @1, ptr @4, size @8."""
    NONE = 0xFFFFFFFF

    def __init__(self, size):
        self.m = Mem(size + 0x20)
        m = self.m
        self.size = (size + 3) & ~3
        self.base = m.base
        self.top = self.limit = self.base + self.size
        self.high = self.base
        self.locks = 0
        self.grew = 0
        self.free_list = self.base
        m.wr(self.base, 0)
        m.wr(self.base + 4, 0)
        m.wr(self.base + 8, self.size - 0x10)

    def H(self, h):
        return self.top - h * 16

    def last_free(self, f):
        while self.m.rd(f + 4):
            f = self.m.rd(f + 4)
        return f

    def new_handle(self):
        m = self.m
        self.grew = 0
        h = 1
        while self.H(h) >= self.limit:
            if m.b[self.H(h) - m.base] != 1:
                return h
            h += 1
        if self.high >= self.limit - 0x10 or self.free_list == self.NONE:
            return 0
        last = self.last_free(self.free_list)
        if m.rd(last + 8) <= 0x10:
            return 0
        m.wr(last + 8, m.rd(last + 8) - 0x10)
        self.grew = 1
        self.limit -= 0x10
        return h

    def alloc_block(self, blk, size):
        m = self.m
        h = self.new_handle()
        if h == 0:
            raise AssertFail("ZMEM: Out of DMM handles")
        e = self.H(h)
        end = blk + size
        if end + 0x10 > self.limit:
            if self.grew == 1:
                if self.free_list == self.NONE:
                    return 0
                last = self.last_free(self.free_list)
                m.wr(last + 8, m.rd(last + 8) + 0x10)
                self.limit += 0x10
                self.grew = 0
            return 0
        self.grew = 0
        m.write(e, b"\x01\x00\x00")
        m.wr(e + 4, blk)
        m.wr(e + 8, size)
        if end > self.high:
            self.high = end
        prev, nxt, bsize = m.rd(blk), m.rd(blk + 4), m.rd(blk + 8)
        if self.high == self.limit:
            self.free_list = self.NONE
        elif blk + size + 12 > blk + bsize:
            m.wr(e + 8, bsize)
            if prev == 0:
                if nxt == 0:
                    self.free_list = self.NONE
                else:
                    self.free_list = nxt
                    m.wr(nxt, 0)
            else:
                m.wr(prev + 4, nxt)
                if nxt:
                    m.wr(nxt, prev)
        else:
            rest = blk + size
            m.wr(rest, prev)
            m.wr(rest + 4, nxt)
            m.wr(rest + 8, bsize - size)
            if prev:
                m.wr(prev + 4, rest)
            if m.rd(rest + 4):
                m.wr(m.rd(rest + 4), rest)
            if m.rd(rest) == 0:
                self.free_list = rest
        m.write(m.rd(e + 4), bytes(m.rd(e + 8)))
        return h

    def alloc(self, size):
        if size == 0:
            raise AssertFail("size = 0")
        if self.free_list == self.NONE:
            raise AssertFail("ZMEM: Out of RAM")
        size = max(size, 12)
        size = (size + 3) & ~3
        f = self.free_list
        while f:
            if size <= self.m.rd(f + 8):
                return self.alloc_block(f, size)
            f = self.m.rd(f + 4)
        raise AssertFail("ZMEMDMM: DMM failed to allocate")

    def dealloc(self, h):
        m = self.m
        e = self.H(h)
        if h == 0 or e < self.limit or m.b[e - m.base] == 0:
            return "invalid"
        m.b[e - m.base] = 0
        blk, size = m.rd(e + 4), m.rd(e + 8)
        m.write(blk, bytes(size))
        m.wr(blk + 8, size)
        head = self.free_list
        if blk < head:
            m.wr(blk, 0)
            m.wr(blk + 4, 0 if head == self.NONE else head)
            if head != self.NONE:
                m.wr(head, blk)
            self.free_list = blk
        else:
            it = head      # C: if (blk > it) while (it->next != NULL && blk > (it = it->next)) ;
            if blk > it:
                while m.rd(it + 4):
                    it = m.rd(it + 4)
                    if not blk > it:
                        break
            if m.rd(it + 4) == 0 and blk > it:
                m.wr(it + 4, blk)
                m.wr(blk, it)
                m.wr(blk + 4, 0)
            else:
                p = m.rd(it)
                m.wr(blk + 4, it)
                m.wr(blk, p)
                m.wr(p + 4, blk)
                m.wr(it, blk)
        nxt = m.rd(blk + 4)
        if nxt and blk + m.rd(blk + 8) == nxt:
            m.wr(blk + 8, m.rd(blk + 8) + m.rd(nxt + 8))
            nn = m.rd(nxt + 4)
            if nn:
                m.wr(nn, blk)
            m.wr(blk + 4, nn)
            m.write(nxt, bytes(12))
        prev = m.rd(blk)
        if prev and prev + m.rd(prev + 8) == blk:
            m.wr(prev + 8, m.rd(prev + 8) + m.rd(blk + 8))
            m.wr(prev + 4, m.rd(blk + 4))
            if m.rd(blk + 4):
                m.wr(m.rd(blk + 4), prev)
            m.write(blk, bytes(12))
        self.high = self.last_free(self.free_list)

    def find_handle(self, ptr):
        m = self.m
        h = 0
        while True:
            h += 1
            e = self.H(h)
            if m.b[e - m.base] and m.rd(e + 4) == ptr:
                return h
            if e <= self.limit:
                return 0

    def lock(self, h):
        e = self.H(h)
        if e >= self.limit and self.m.b[e - self.m.base]:
            self.locks += 1
            self.m.b[e + 1 - self.m.base] += 1
            return self.m.rd(e + 4)
        return 0

    def optimize(self):
        m = self.m
        if self.locks > 0:
            return 1
        f = self.free_list
        if f == self.NONE or (m.rd(f) == 0 and m.rd(f + 4) == 0):
            return 0
        while m.rd(f + 4):
            fsize, nxt = m.rd(f + 8), m.rd(f + 4)
            src = f + fsize
            h = self.find_handle(src)
            if h == 0:
                raise AssertFail("DMM optimize found unrecognized pointer")
            e = self.H(h)
            n = m.rd(e + 8)
            m.write(f, m.read(src, n))
            m.wr(e + 4, f)
            nf = f + n
            m.wr(nf, 0)
            m.wr(nf + 4, nxt)
            m.wr(nf + 8, fsize)
            if nxt:
                m.wr(nxt, nf)
                if nf + m.rd(nf + 8) == m.rd(nf + 4):
                    m.wr(nf + 8, m.rd(nf + 8) + m.rd(nxt + 8))
                    nn = m.rd(nxt + 4)
                    if nn:
                        m.wr(nn, nf)
                    m.wr(nf + 4, nn)
            self.free_list = nf
            f = nf
        self.high = self.last_free(f)
        return 0

    def blocks(self):
        """{handle: (ptr, size)} of the used handles."""
        out, h = {}, 1
        while self.H(h) >= self.limit:
            e = self.H(h)
            if self.m.b[e - self.m.base]:
                out[h] = (self.m.rd(e + 4), self.m.rd(e + 8))
            h += 1
        return out

    def free_blocks(self):
        out, f = [], self.free_list
        if f == self.NONE:
            return out
        while f:
            out.append((f, self.m.rd(f + 8)))
            f = self.m.rd(f + 4)
        return out


def check_dmm(d, contents):
    used = d.blocks()
    assert set(used) == set(contents)
    spans = sorted(list(used.values()) + d.free_blocks())
    for (a, n), (b, _) in zip(spans, spans[1:]):
        assert a + n <= b, "overlap"
    for a, n in spans:
        assert d.base <= a and a + n <= d.limit
    prev = 0
    for f, _ in d.free_blocks():
        assert d.m.rd(f) == prev and f > prev
        prev = f
    for h, data in contents.items():
        ptr, size = used[h]
        assert size >= len(data) and d.m.read(ptr, len(data)) == data


@pytest.mark.parametrize("seed", range(5))
def test_dmm_port_random_trace_with_compaction(seed):
    rnd = random.Random(seed)
    d = DMMPort(0x3000)
    contents = {}
    optimized = 0
    for step in range(600):
        r = rnd.random()
        if r < 0.5 or not contents:
            n = rnd.choice([4, 12, 13, 0x20, 0x55, 0x100])
            try:
                h = d.alloc(n)
            except AssertFail:
                h = 0
            if h:
                data = bytes(rnd.randrange(1, 256) for _ in range(n))
                d.m.write(d.blocks()[h][0], data)
                contents[h] = data
        elif r < 0.85:
            h = rnd.choice(sorted(contents))
            d.dealloc(h)
            del contents[h]
        else:
            try:
                assert d.optimize() == 0
            except AssertFail:
                # [V] reachable: AllocBlock drops the whole free list when the data reaches the
                # handle table, so an unlisted hole can follow the first free block
                break
            optimized += 1
            free = d.free_blocks()
            assert len(free) <= 1, free    # compaction leaves one free block at the top
        check_dmm(d, contents)
    assert optimized > 0


def test_dmm_port_documented_quirks():
    d = DMMPort(0x400)
    assert d.dealloc(5) == "invalid"
    with pytest.raises(AssertFail):
        d.alloc(0)
    h = d.alloc(0x20)
    assert h == 1 and d.limit == d.top - 0x10
    ptr = d.lock(h)
    assert ptr == d.base and d.optimize() == 1      # locked: nothing moves


def test_dmm_port_truncated_block_quirk():
    """[V via the port] zmemDMMNewHandle takes 16 bytes for a new handle slot from the *last* free
    block, wherever it is. If that is the block zmemDMMAlloc chose, zmemDMMAllocBlock re-reads the
    shrunken size and hands out a block smaller than requested (seen here: 16 asked, 4 recorded),
    so a later zmemDMMOptimize moves only part of the data."""
    rnd = random.Random(0)
    d = DMMPort(0x800)
    live = {}
    for _ in range(600):
        r = rnd.random()
        if r < 0.5 or not live:
            n = rnd.choice([4, 12, 13, 0x20, 0x55, 0x100])
            try:
                h = d.alloc(n)
            except AssertFail:
                h = 0
            if h:
                if d.blocks()[h][1] < max((n + 3) & ~3, 12):
                    return
                live[h] = bytes(rnd.randrange(1, 256) for _ in range(n))   # same draws as the trace test
        elif r < 0.85:
            h = rnd.choice(sorted(live))
            d.dealloc(h)
            del live[h]
        else:
            d.optimize()
    pytest.fail("no truncated DMM block in the trace")

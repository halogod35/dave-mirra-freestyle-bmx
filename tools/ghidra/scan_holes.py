"""Find executable bytes in the game/engine part of .text that no Ghidra function covers.

    tools/testing/py.sh tools/ghidra/scan_holes.py                 # report every hole

Range: CODE_START (0x00403C30, after the /INCREMENTAL thunk table) up to the first library byte
(build_symbols.LIB_START). Function bodies come from decomp/symbols/functions.csv (va..end, inclusive). What is
left between two functions is classified, in this order:

  padding      0xCC / 0x90 bytes and the MSVC multi-byte alignment no-ops (8B FF, 8D 49 00, ...)
  jump table   aligned dwords that all point into the game/engine code (switch tables)
  index table  the byte table MSVC emits after a jump table (every byte < the jump table's size)
  import stub  FF 25 jmp dword ptr [IAT] import-library stubs
  hole         anything else, as one run of instructions up to a ret / jmp followed by padding: code
               (or data) that is not part of any function

A hole is reported with its size, whether it is referenced (a call / jmp rel32, or an absolute
dword anywhere in the image equal to its start, e.g. a vtable or a thunk) and its first
instruction. Exit status 0; the test (tests/test_decomp_cleanup.py) decides what is acceptable.
"""
from __future__ import annotations

import argparse
import csv
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import build_symbols as bs  # noqa: E402

BASE = 0x400000
IMPORT_JMP = bytes([0xFF, 0x25])   # jmp dword ptr [abs32]
DEFAULT_EXE = os.path.join(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"), "NgBMX.exe")

# MSVC 6 alignment fillers (longest first)
PAD_SEQS = sorted([
    b"\x90", b"\xcc", b"\x8b\xff", b"\x8b\xc0", b"\x8b\xc9", b"\x8b\xd2", b"\x8b\xdb", b"\x8b\xe4",
    b"\x8b\xed", b"\x8b\xf6", b"\x8d\x40\x00", b"\x8d\x49\x00", b"\x8d\x52\x00", b"\x8d\x5b\x00",
    b"\x8d\x64\x24\x00", b"\x8d\x6d\x00", b"\x8d\x76\x00", b"\x8d\x7f\x00",
    b"\x8d\x74\x26\x00", b"\x8d\xa4\x24\x00\x00\x00\x00", b"\x8d\x9b\x00\x00\x00\x00",
    b"\x8d\x80\x00\x00\x00\x00", b"\x8d\x89\x00\x00\x00\x00", b"\x8d\x92\x00\x00\x00\x00",
    b"\x8d\xb4\x26\x00\x00\x00\x00", b"\x8d\xbf\x00\x00\x00\x00", b"\x8d\xb6\x00\x00\x00\x00",
    b"\x8d\x64\x24\x00",
], key=len, reverse=True)


class Image:
    """The mapped exe (stdlib only: PE section table -> flat image)."""

    def __init__(self, path):
        data = open(path, "rb").read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt = struct.unpack_from("<H", data, pe + 20)[0]
        size = struct.unpack_from("<I", data, pe + 24 + 56)[0]
        self.img = bytearray(size)
        self.sections = []
        off = pe + 24 + opt
        for i in range(nsec):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, off + 40 * i)
            n = min(rsize, vsize) if vsize else rsize
            self.img[va:va + n] = data[raw:raw + n]
            self.sections.append((name.rstrip(b"\0").decode("latin-1"), BASE + va, BASE + va + max(vsize, rsize)))

    def byte(self, va):
        return self.img[va - BASE]

    def bytes(self, va, n):
        return bytes(self.img[va - BASE:va - BASE + n])

    def dword(self, va):
        return struct.unpack_from("<I", self.img, va - BASE)[0]


def load_bodies(path):
    out = []
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            out.append((int(r["va"], 16), int(r["end"], 16), r["name"]))
    return sorted(out)


def references(im, lo, hi):
    """rel32 call/jmp targets inside [lo, hi) and every aligned absolute dword of the image."""
    rel = set()
    img = im.img
    for i in range(lo - BASE, hi - BASE - 4):
        if img[i] in (0xE8, 0xE9):
            t = (i + 5 + struct.unpack_from("<i", img, i + 1)[0]) & 0xFFFFFFFF
            rel.add(t + BASE)
    absd = set(struct.unpack_from("<%dI" % (len(img) // 4), img, 0))
    return rel, absd


def pad_len(im, va, end):
    for s in PAD_SEQS:
        if va + len(s) <= end and im.bytes(va, len(s)) == s:
            return len(s)
    return 0


def classify(im, lo, hi, code_lo, code_hi):
    """Split [lo, hi) into (kind, start, end) pieces."""
    pieces = []
    va = lo
    last_jt = 0
    while va < hi:
        n = pad_len(im, va, hi)
        if n:
            s = va
            while va < hi and (n := pad_len(im, va, hi)):
                va += n
            pieces.append(("padding", s, va))
            continue
        if va % 4 == 0 and va + 8 <= hi:
            s = va
            while va + 4 <= hi and code_lo <= im.dword(va) < code_hi:
                va += 4
            if va - s >= 8:
                pieces.append(("jump table", s, va))
                last_jt = (va - s) // 4
                continue
            va = s
        if last_jt and pieces and pieces[-1][0] == "jump table":
            s = va
            while va < hi and im.byte(va) < last_jt:
                va += 1
            if va > s:
                pieces.append(("index table", s, va))
                last_jt = 0
                continue
        # import-library jump stubs: jmp dword ptr [IAT]
        if im.bytes(va, 2) == IMPORT_JMP:
            s = va
            while va + 6 <= hi and im.bytes(va, 2) == IMPORT_JMP:
                va += 6
            pieces.append(("import stub", s, va))
            last_jt = 0
            continue
        # anything else is a hole: sweep instructions until a ret / jmp is followed by padding
        s = va
        va = sweep(im, va, hi)
        pieces.append(("hole", s, va))
        last_jt = 0
    return pieces


def sweep(im, va, hi):
    """End of the code run starting at va: linear disassembly until an unconditional ret / jmp is
    followed by padding (or hi); undecodable bytes end the run."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    while va < hi:
        insn = next(md.disasm(im.bytes(va, min(16, hi - va)), va), None)
        if insn is None:
            return va + 1
        va += insn.size
        if insn.mnemonic in ("ret", "jmp", "int3") and (va >= hi or pad_len(im, va, hi)):
            return va
    return hi


def first_insn(im, va):
    try:
        import capstone
    except ImportError:
        return im.bytes(va, 8).hex()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i in md.disasm(im.bytes(va, 16), va):
        return f"{i.mnemonic} {i.op_str}".strip()
    return "(not decodable)"


def scan(exe=DEFAULT_EXE, functions=os.path.join(ROOT, "decomp", "symbols", "functions.csv")):
    """Return a list of dicts {va, size, referenced, insn, prev, next} for every hole."""
    im = Image(exe)
    bodies = load_bodies(functions)
    lo, hi = bs.CODE_START, bs.LIB_START
    rel, absd = references(im, BASE + 0x1000, hi)
    holes = []
    covered_to = lo
    prev = None
    inside = [b for b in bodies if b[1] >= lo and b[0] < hi]
    for i, (va, end, name) in enumerate(inside + [(hi, hi, "(libraries)")]):
        if va > covered_to:
            for kind, s, e in classify(im, covered_to, va, lo, hi):
                if kind == "hole":
                    holes.append({"va": s, "size": e - s, "referenced": s in rel or s in absd,
                                  "insn": first_insn(im, s), "prev": prev, "next": name})
        covered_to = max(covered_to, end + 1)
        prev = name
    return holes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--functions", default=os.path.join(ROOT, "decomp", "symbols", "functions.csv"))
    a = ap.parse_args()
    holes = scan(a.exe, a.functions)
    for h in holes:
        print(f"0x{h['va']:08x} {h['size']:5d} {'ref' if h['referenced'] else '   '} "
              f"{h['prev']} .. {h['next']}: {h['insn']}")
    print(f"{len(holes)} holes, {sum(h['size'] for h in holes)} bytes")


if __name__ == "__main__":
    main()

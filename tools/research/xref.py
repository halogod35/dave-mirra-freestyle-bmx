"""Tiny capstone helper: find code xrefs to a VA (imm32 operands) and disassemble around them.

usage: xref.py refs <va>...        list instructions whose bytes contain the imm32 va
       xref.py dis <va> [count]    linear-disassemble count instructions from va
       xref.py func <va>           disassemble from the nearest preceding function start (CC/C3 padding heuristic) to ret
"""
import os, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.path.join(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"), "NgBMX.exe")
pe = pefile.PE(EXE, fast_load=True)
IMG = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
TVA = IMG + text.VirtualAddress
TDATA = text.get_data()
md = Cs(CS_ARCH_X86, CS_MODE_32)


def read(va, n):
    return pe.get_data(va - IMG, n)


def refs(target):
    pat = struct.pack("<I", target)
    i = TDATA.find(pat)
    out = []
    while i >= 0:
        out.append(TVA + i)
        i = TDATA.find(pat, i + 1)
    return out


def dis(va, count=60):
    off = va - TVA
    for ins in md.disasm(TDATA[off:off + count * 15], va, count):
        print(f"{ins.address:08x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")


def func_start(va):
    off = va - TVA
    while off > 0:
        if TDATA[off - 1] in (0xCC, 0x90, 0xC3) and TDATA[off] in (0x55, 0x83, 0x81, 0x53, 0x56, 0x57, 0x8B, 0x6A, 0xA1, 0x51):
            # aligned starts are 16-byte aligned in MSVC release builds
            if (TVA + off) % 16 == 0:
                return TVA + off
        off -= 1
    return TVA


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "refs":
        for a in sys.argv[2:]:
            t = int(a, 16)
            for r in refs(t):
                print(f"{t:08x} <- {r - 1:08x}?  func~{func_start(r):08x}")
    elif cmd == "dis":
        dis(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    elif cmd == "func":
        s = func_start(int(sys.argv[2], 16))
        print(f"; func start ~{s:08x}")
        dis(s, int(sys.argv[3]) if len(sys.argv) > 3 else 200)

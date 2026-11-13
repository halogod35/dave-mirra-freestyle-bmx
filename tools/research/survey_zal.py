"""Corpus survey of the .zal archives: header words, group table, payload types, padding, tails.

usage: tools/testing/py.sh tools/research/survey_zal.py
Recurses into nested archives (Intwrap.zal group 0). Reads the install read-only.
"""
import collections
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dmbmx.formats import zal  # noqa: E402

GAME = Path(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))


def kind(payload):
    h = payload[:4]
    if h == zal.MAGIC_BYTES:
        return "ZAL"
    if h[:2] == b"BM":
        return "BMP"
    if len(h) == 4 and all(0x20 <= b < 0x7F for b in h):
        return h.decode()
    return "bin"


def survey(arc, name, depth=0):
    ind = "  " * depth
    ids = [e.id for e in arc.entries]
    print(f"{ind}{name}: flags=0x{arc.flags:04x} count=0x{arc.count_word:04x} -> groups={len(ids)} "
          f"per_group={arc.assets_per_group} compressed={arc.compressed} method={arc.method} "
          f"ref={arc.ref_mode} reserved={arc.reserved.hex()}")
    seq = ids == list(range(ids[0], ids[0] + len(ids))) if ids else True
    print(f"{ind}  ids: first=0x{ids[0]:x} last=0x{ids[-1]:x} sequential={seq} "
          f"{'' if seq else [hex(i) for i in ids]}")
    kinds = collections.Counter()
    pads = collections.Counter()
    hi = collections.Counter()
    ratio = [0, 0]
    for e in arc.entries:
        a = e.assets[0]
        p = arc.asset_data(a)
        kinds[kind(p)] += 1
        pads[(len(a.pad), a.pad[:1].hex())] += 1
        hi[a.size_field >> 24] += 1
        ratio[0] += len(a.stored)
        ratio[1] += a.size
        if kind(p) == "ZAL":
            survey(zal.read(p), f"{name}#{e.id:x}", depth + 1)
    aligned = all((e.assets[0].offset + 0x10) % 4 == 0 for e in arc.entries)
    print(f"{ind}  payloads: {dict(kinds)}  stored/size={ratio[0]}/{ratio[1]}")
    print(f"{ind}  4-aligned data: {aligned}; pad (len, first byte): {dict(pads)}")
    print(f"{ind}  size-field high bytes: {dict(hi)}; tail: {arc.tail.hex() or '-'}")


def main():
    for p in sorted(GAME.joinpath("assets").glob("*.zal"), key=lambda p: p.name.lower()):
        d = p.read_bytes()
        print(f"== {p.name} ({len(d)} bytes)")
        survey(zal.read(d), p.name)


if __name__ == "__main__":
    main()

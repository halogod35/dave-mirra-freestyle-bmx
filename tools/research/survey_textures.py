"""Survey of embedded textures: MATI/MADF/TBMP/TPNG in every .zmd and level Mat._sd,
plus the BMPs in material.zal. Prints MADF field histograms and image format statistics.

usage: tools/testing/py.sh tools/research/survey_textures.py
"""
import collections
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dmbmx.core.chunks import parse_4cc_ex  # noqa: E402
from dmbmx.formats import zal  # noqa: E402

GAME = Path(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))


def bmp_info(b):
    hs = struct.unpack_from("<I", b, 14)[0]
    w, h, planes, bpp, comp = struct.unpack_from("<iiHHI", b, 18)
    clr = struct.unpack_from("<I", b, 46)[0] if hs >= 40 else 0
    return f"BMP{bpp} hdr{hs} c{comp} clr{clr}", (w, h)


def png_info(b):
    w, h, depth, ctype, comp, filt, il = struct.unpack_from(">IIBBBBB", b, 16)
    chunks = []
    p = 8
    while p + 8 <= len(b):
        n, t = struct.unpack_from(">I4s", b, p)
        crc_ok = zlib.crc32(b[p + 4:p + 8 + n]) == struct.unpack_from(">I", b, p + 8 + n)[0]
        chunks.append(t.decode("latin1") + ("" if crc_ok else "!crc"))
        p += 12 + n
        if t == b"IEND":
            break
    return f"PNG d{depth} ct{ctype} il{il} [{' '.join(c for c in chunks if c not in ('IDAT',))}]", (w, h)


def matis(path):
    data = path.read_bytes()
    if not data:
        return
    roots, _ = parse_4cc_ex(data, (b"MODL", b"MLOD", b"MATI"))
    stack = list(roots)
    while stack:
        c = stack.pop(0)
        if c.tag == b"MATI":
            yield c
        elif c.children:
            stack[:0] = c.children


def main():
    files = sorted(GAME.joinpath("assets").rglob("*"))
    files = [p for p in files if p.suffix.lower() == ".zmd" or p.name.lower() == "mat._sd"]
    fmt = collections.Counter()
    sizes = collections.Counter()
    madf_len = collections.Counter()
    words = collections.defaultdict(collections.Counter)
    pow2 = collections.Counter()
    per_src = collections.Counter()
    for p in files:
        src = "zmd" if p.suffix.lower() == ".zmd" else "mat_sd"
        for m in matis(p):
            madf, img = m.children[0], m.children[1]
            d = madf.data
            madf_len[(src, len(d))] += 1
            for o in range(4, 0x44, 4):
                f = struct.unpack_from("<f", d, o)[0]
                u = struct.unpack_from("<I", d, o)[0]
                words[o][round(f, 3) if 1e-6 < abs(f) < 1e6 else hex(u)] += 1
            body = img.data
            info, wh = bmp_info(body) if img.tag == b"TBMP" else png_info(body)
            fmt[(src, info)] += 1
            sizes[(src, wh)] += 1
            pow2[all(x and (x & (x - 1)) == 0 for x in map(abs, wh))] += 1
            per_src[src] += 1
    print("MATI per source:", dict(per_src))
    print("MADF lengths:", dict(madf_len))
    print("image formats:")
    for k, v in sorted(fmt.items(), key=lambda kv: -kv[1]):
        print(f"  {v:5d} {k}")
    print("sizes:", dict(sorted(sizes.items(), key=lambda kv: -kv[1])))
    print("power-of-two w/h:", dict(pow2))
    print("MADF words (offset: value histogram, floats rounded):")
    for o in sorted(words):
        print(f"  0x{o:02x}: {dict(words[o].most_common(8))}  ({len(words[o])} distinct)")
    mz = zal.read(GAME.joinpath("assets", "material.zal").read_bytes())
    mf = collections.Counter()
    ms = collections.Counter()
    for e in mz.entries:
        info, wh = bmp_info(e.data())
        mf[info] += 1
        ms[wh] += 1
    print("material.zal:", dict(mf), "sizes:", dict(ms.most_common(10)))


if __name__ == "__main__":
    main()

"""Write texture contact sheets to out/textures/: one PNG grid per source.

Usage: tools/testing/py.sh tools/research/contact_sheets.py [--game DIR] [--out DIR] [--tile 64] [--only NAME]
Sources: every .zmd (one sheet per model family: riders, bikes, other), each level Mat._sd, and
material.zal. Transparent pixels (PNG alpha, BMP colour key 255,0,255) are drawn on a checkerboard.
Reads the install read-only.
"""
import argparse
import os
from pathlib import Path

from dmbmx.formats import texture as T
from dmbmx.formats import zal, zmd
from dmbmx.formats.mati import MatSd

REPO = Path(__file__).resolve().parents[2]


def sheet(images, tile=64, cols=16):
    """images: list of Image -> one Image grid (alpha composited over a checkerboard)."""
    rows = (len(images) + cols - 1) // cols or 1
    W, H = cols * tile, rows * tile
    buf = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            g = 200 if ((x // 8) + (y // 8)) % 2 else 150
            o = (y * W + x) * 4
            buf[o:o + 4] = bytes((g, g, g, 255))
    for n, img in enumerate(images):
        t = T.resize_nearest(img, tile, tile)
        ox, oy = (n % cols) * tile, (n // cols) * tile
        for y in range(tile):
            for x in range(tile):
                s = (y * tile + x) * 4
                r, g, b, a = t.rgba[s:s + 4]
                o = ((oy + y) * W + ox + x) * 4
                if a == 255:
                    buf[o:o + 4] = bytes((r, g, b, 255))
                elif a:
                    br, bg, bb = buf[o:o + 3]
                    buf[o:o + 3] = bytes(((r * a + br * (255 - a)) // 255, (g * a + bg * (255 - a)) // 255,
                                          (b * a + bb * (255 - a)) // 255))
    return T.Image(W, H, buf)


def load(kind_img):
    kind, image = kind_img
    img = T.decode(image)
    return T.apply_color_key(img) if kind == "bmp" else img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "textures"))
    ap.add_argument("--tile", type=int, default=64)
    ap.add_argument("--only", default=None, help="only sheets whose name contains this")
    a = ap.parse_args()
    assets = Path(a.game) / "assets"
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    groups = {"zmd_riders": [], "zmd_bikes": [], "zmd_other": []}
    for p in sorted(assets.glob("*")):
        if p.suffix.lower() != ".zmd" or not p.stat().st_size:
            continue
        low = p.stem.lower()
        g = "zmd_bikes" if "bike" in low else "zmd_other" if ("shadow" in low or low == "sfx") else "zmd_riders"
        groups[g] += [(f"{p.stem}:{m.name}", m.kind, m.image) for m in zmd.read(p.read_bytes()).materials()]
    for p in sorted(assets.rglob("Mat._sd")):
        groups[f"level_{p.parent.name}"] = [(m.name, m.kind, m.image) for m in MatSd.read(p.read_bytes()).materials()]
    groups["material_zal"] = [(f"{e.id}", "bmp", e.data()) for e in zal.read((assets / "material.zal").read_bytes()).entries]
    for name, items in groups.items():
        if a.only and a.only.lower() not in name.lower():
            continue
        img = sheet([load((k, b)) for _, k, b in items], a.tile)
        (out / f"{name}.png").write_bytes(T.encode_png(img, alpha=False))
        (out / f"{name}.txt").write_text("".join(f"{i:4d} {n}\n" for i, (n, _, _) in enumerate(items)), newline="\n")
        print(f"{name}: {len(items)} textures -> {out / (name + '.png')}")


if __name__ == "__main__":
    main()

"""Tile PNG renders into one image with Blender headless (no PIL in the venv). Used for the arm-pose render strip.

    blender -b --factory-startup --python tools/blender/scripts/compose_strip.py -- out.png cols a.png b.png ...

Images are placed row-major, `cols` per row, in the given order; all must have the same size.
"""
import os
import sys

import bpy


def main(argv):
    out, cols, files = argv[0], int(argv[1]), argv[2:]
    imgs = [bpy.data.images.load(os.path.abspath(f)) for f in files]
    w, h = imgs[0].size
    rows = (len(imgs) + cols - 1) // cols
    W, H = w * cols, h * rows
    px = [0.0] * (W * H * 4)
    for i, im in enumerate(imgs):
        assert tuple(im.size) == (w, h), files[i]
        src = list(im.pixels)
        c, r = i % cols, rows - 1 - i // cols          # Blender pixel rows start at the bottom
        for y in range(h):
            o = ((r * h + y) * W + c * w) * 4
            px[o:o + w * 4] = src[y * w * 4:(y + 1) * w * 4]
    out_img = bpy.data.images.new("strip", W, H, alpha=True)
    out_img.pixels = px
    out_img.filepath_raw = os.path.abspath(out)
    out_img.file_format = "PNG"
    out_img.save()
    print("WROTE", out_img.filepath_raw)
    return 0


if __name__ == "__main__":
    rc = main(sys.argv[sys.argv.index("--") + 1:])
    sys.stdout.flush()
    os._exit(rc)

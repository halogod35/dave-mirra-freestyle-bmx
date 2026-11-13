"""Corpus survey for .zmd: 4CC tree shapes, RW chunk ids/versions, size histograms.

Usage: tools/testing/py.sh tools/research/survey_zmd.py [game_dir]
"""
import collections, os, sys
from pathlib import Path

from dmbmx.core.chunks import parse_4cc_ex, parse_rw_ex, walk

GAME = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
EXCL = {"_pristine", "_testgame", "wt", "dave-mirra-bmx", "dave-mirra-freestyle-bmx"}


def files():
    for root, dirs, fs in os.walk(GAME):
        if Path(root) == GAME:
            dirs[:] = [d for d in dirs if d not in EXCL]
        for f in fs:
            if f.lower().endswith(".zmd"):
                yield Path(root) / f


tag_count = collections.Counter()
paths = collections.Counter()
rw_ids = collections.Counter()
rw_vers = collections.Counter()
leaf_sizes = collections.defaultdict(list)
trailers = collections.Counter()
mldf = collections.Counter()
shapes = collections.Counter()
empty = []
n = 0
CONT = {b"MODL", b"MLOD", b"MATI"}
for p in sorted(files()):
    n += 1
    data = p.read_bytes()
    if not data:
        empty.append(p.name)
        continue
    roots, trail = parse_4cc_ex(data, CONT)
    if trail:
        trailers[("top", len(trail))] += 1
    shape = []
    for d, c in walk(roots):
        tp = c.tag.decode()
        paths["  " * d + tp] += 1
        tag_count[tp] += 1
        if c.children is None:
            leaf_sizes[tp].append(len(c.data))
        if c.trailing:
            trailers[(tp, len(c.trailing))] += 1
        if c.tag == b"MLDF":
            mldf[c.data.hex(" ")] += 1
        if d <= 1:
            shape.append(tp)
        if c.tag == b"MDFF":
            rws, rt = parse_rw_ex(c.data)
            if rt:
                trailers[("MDFF-rw", len(rt))] += 1
            for rd, rc in walk(rws):
                rw_ids[(rd, rc.name)] += 1
                rw_vers[hex(rc.version)] += 1
    shapes[" ".join(shape)] += 1

print(f"files: {n}, empty: {empty}")
print("\n4CC paths:")
for k, v in paths.items():
    print(f"  {v:5d}  {k}")
print("\ntop-level shapes:")
for k, v in shapes.most_common():
    print(f"  {v:4d}  {k}")
print("\nMLDF payloads:", dict(mldf))
print("\nleaf sizes (min/max/count):")
for k, v in leaf_sizes.items():
    print(f"  {k}: {min(v)}..{max(v)} n={len(v)}")
print("\ntrailing bytes:", dict(trailers))
print("\nRW versions:", dict(rw_vers))
print("\nRW ids by depth:")
for (d, nm), v in sorted(rw_ids.items()):
    print(f"  d{d} {nm:28s} {v}")

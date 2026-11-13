"""RenderWare 3.x per-sector collision tree (the 384-byte block after a BSP._sd world sector's
triangles).

Layout [V] (RW 3.1 in NgBMX.exe: the line test 0x004BE560 and the sphere test 0x004BED60 walk it
from ``sector + 0x54``; docs/re/physics.md 3, docs/formats/LEVEL.md 4.3):

* 128 nodes of 3 bytes, an implicit binary tree: node 1 is the root, the children of node i are
  2i (the low side) and 2i + 1 (the high side); node 0 is unused (all zero in the corpus).
* ``byte0``: bit 7 = split node; bits 5-6 = split axis (0 x, 1 y, 2 z: the code reads
  ``(byte0 >> 3) & 0xC`` as a byte offset into a vector); bits 0-4 = number of triangles stored at
  this node (straddling triangles at a split, all of them at a leaf).
* ``byte1``: split nodes: index of the sector vertex whose coordinate on the axis is the plane.
* ``byte2``: first triangle (index into the sector's triangle list); the node owns triangles
  ``[byte2, byte2 + count)``.

The tests only see the triangles the tree references, so the triangle list must be ordered so
that every node's triangles are contiguous, and every triangle must be referenced exactly once
[V: 784/784 sectors of the 12 levels]. A geometry edit therefore has to rebuild this block
(and re-order the triangles) for collision to stay correct.

The writer side:

* :func:`build` / :func:`tree`: a builder for this block. RenderWare's own builder is part of the
  offline world importer and is **not** in NgBMX.exe [V: the exe only reads, sizes and writes the
  block: 0x004C2F50 / 0x004BB760 / 0x004C2CD0]. The original trees cannot be reproduced byte for
  byte from the file (the build-time vertex numbering and coordinates are lost, docs/re/physics.md
  3.1), so :func:`build` produces its own valid tree (same node rules, same triangle classification
  as the corpus) and sorts the triangles to match.
* :func:`used_size` (= RW 0x004BB760) and :func:`col_sector_size`: the World header field
  ``colSectorSize`` is the sum over sectors of ``used_size`` rounded up to 4 [V 12/12 levels]; the
  world reader allocates exactly that much for the trees, so it must be recomputed after a rebuild.
* :func:`line_test` / :func:`sphere_test`: Python ports of the RW per-sector walks (0x004BE560,
  0x004BED60) for checking that a tree finds the triangles a query touches.
"""
from __future__ import annotations

import math
from bisect import bisect_left, bisect_right

from ..core.errors import FormatError

COLL_NODES = 128
COLL_SIZE = 3 * COLL_NODES       # 384
AXES = "xyz"
#: bits 0-4 of byte0: at most 31 triangles per node.
MAX_NODE_TRIS = 31
#: a node can split only if both children fit in the 128-node array (2i + 1 < 128).
LAST_SPLIT_NODE = COLL_NODES // 2 - 1
#: byte2 (first triangle) is a u8.
MAX_FIRST_TRI = 255


class CollError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="RWCOLL", offset=offset)


class CollCapacityError(CollError):
    """The sector's triangles do not fit in one 128-node tree (a node would need more than 31
    triangles, or a node would start beyond triangle 255). Split the sector
    (:meth:`dmbmx.formats.rwworld.World.rebuild_collision` does that with ``split=True``)."""


class CollNode:
    """One node of the tree: ``index``, ``split`` (bool), ``axis`` (0..2, splits only),
    ``vertex`` (plane vertex index, splits only), ``start``, ``count`` (triangles)."""
    __slots__ = ("index", "split", "axis", "vertex", "start", "count")

    def __init__(self, index, b0, b1, b2):
        self.index = index
        self.split = bool(b0 & 0x80)
        self.axis = ((b0 >> 3) & 0xC) // 4
        self.vertex = b1
        self.start = b2
        self.count = b0 & 0x1F

    def __repr__(self):
        kind = f"split {AXES[self.axis] if self.axis < 3 else '?'}@v{self.vertex}" if self.split else "leaf"
        return f"CollNode({self.index}, {kind}, tris {self.start}+{self.count})"


def decode(block: bytes) -> list:
    """The reachable nodes of a collision block, in pre-order (root first, low before high)."""
    if len(block) != COLL_SIZE:
        raise CollError(f"collision block is {len(block)} bytes, expected {COLL_SIZE}")
    out, stack = [], [1]
    while stack:
        i = stack.pop()
        if i >= COLL_NODES:
            raise CollError(f"node {i // 2} splits but its children are outside the {COLL_NODES}-node tree",
                            offset=3 * (i // 2))
        node = CollNode(i, block[3 * i], block[3 * i + 1], block[3 * i + 2])
        out.append(node)
        if node.split:
            stack += [2 * i + 1, 2 * i]
    return out


def check(sector) -> list:
    """Problems of an :class:`~dmbmx.formats.rwworld.AtomicSector`'s collision block (empty list =
    consistent): every triangle referenced exactly once, triangle and vertex indices in range, the
    axis valid, and every triangle below a split on its side of the plane (0.001 tolerance)."""
    if not sector.coll_present:
        return [] if not sector.coll else ["collision bytes without collSectorPresent"]
    try:
        nodes = decode(sector.coll)
    except CollError as e:
        return [str(e)]
    probs = []
    nt, nv = len(sector.triangles), len(sector.vertices)
    seen = [0] * nt
    byidx = {n.index: n for n in nodes}

    def tris(i):
        n = byidx.get(i)
        if n is None:
            return []
        own = list(range(n.start, n.start + n.count))
        return own + (tris(2 * i) + tris(2 * i + 1) if n.split else [])

    for n in nodes:
        if n.start + n.count > nt:
            probs.append(f"node {n.index}: triangles {n.start}..{n.start + n.count - 1} beyond {nt}")
            continue
        for t in range(n.start, n.start + n.count):
            seen[t] += 1
        if not n.split:
            continue
        if n.axis > 2:
            probs.append(f"node {n.index}: bad axis bits")
            continue
        if n.vertex >= nv:
            probs.append(f"node {n.index}: plane vertex {n.vertex} >= {nv}")
            continue
        plane = sector.vertices[n.vertex][n.axis]
        for t in tris(2 * n.index):
            if t < nt and max(sector.vertices[k][n.axis] for k in sector.triangles[t][1:]) > plane + 1e-3:
                probs.append(f"node {n.index}: triangle {t} not on the low side")
        for t in tris(2 * n.index + 1):
            if t < nt and min(sector.vertices[k][n.axis] for k in sector.triangles[t][1:]) < plane - 1e-3:
                probs.append(f"node {n.index}: triangle {t} not on the high side")
    bad = [t for t, c in enumerate(seen) if c != 1]
    if bad:
        probs.append(f"{len(bad)} triangles referenced {sorted(set(seen[t] for t in bad))} times (first {bad[0]})")
    return probs


# ------------------------------------------------------------------------------- sizes

def used_size(block) -> int:
    """Bytes of the block RW keeps in memory: (highest reachable node index + 1) x 3, 0 for no block
    (port of RW 0x004BB760; the stream reader 0x004C2F50 copies only this much, the tail of the
    384 bytes on disk is never loaded: in the corpus it is uninitialised junk [V 783/784])."""
    if not block:
        return 0
    return 3 * max(n.index for n in decode(bytes(block))) + 3


def col_sector_size(sectors) -> int:
    """``World.colSectorSize``: sum of :func:`used_size` rounded up to 4 over all sectors with a
    collision block [V: equals the stored value in all 12 levels; RW's world writer 0x004C3AE0
    computes it the same way, and the world reader 0x004C3E60 adds it to the size of the one
    block it allocates for the whole world]."""
    return sum((used_size(s.coll) + 3) & ~3 for s in sectors if s.coll_present and s.coll)


# ------------------------------------------------------------------------------- builder

def side(coords, plane) -> int:
    """Which part of a split a triangle goes to: -1 low child, +1 high child, 0 stays at the node.

    ``coords``: the triangle's three vertex coordinates on the split axis. [V: the rule the shipped
    trees follow for 97 % of the triangles that touch their plane, and for every triangle that does
    not]: a triangle goes to a child only if it has no vertex on the other side **and** at least two
    vertices strictly on its own side; a triangle that crosses the plane, or lies in it with an edge
    or its whole face, stays at the node. (The remaining 3 % disagree in both directions; the
    build-time coordinates were evidently not exactly the stored ones.)"""
    below = sum(c < plane for c in coords)
    above = sum(c > plane for c in coords)
    if below and above:
        return 0
    if below >= 2:
        return -1
    if above >= 2:
        return 1
    return 0


class _Axis:
    """Per-axis sweep data for one node: counts of low / high triangles for any plane in O(log n)."""
    __slots__ = ("lo_le", "lo_lt", "hi_ge", "hi_gt", "values")

    def __init__(self, cs):
        # low iff c2 <= p and c1 < p: c1 < c2 -> p >= c2 ; c1 == c2 -> p > c2
        self.lo_le = sorted(c[2] for c in cs if c[1] < c[2])
        self.lo_lt = sorted(c[2] for c in cs if c[1] == c[2])
        # high iff c0 >= p and c1 > p: c1 > c0 -> p <= c0 ; c1 == c0 -> p < c0
        self.hi_ge = sorted(c[0] for c in cs if c[1] > c[0])
        self.hi_gt = sorted(c[0] for c in cs if c[1] == c[0])
        self.values = sorted({x for c in cs for x in c})

    def counts(self, p):
        low = bisect_right(self.lo_le, p) + bisect_left(self.lo_lt, p)
        high = (len(self.hi_ge) - bisect_left(self.hi_ge, p)) + (len(self.hi_gt) - bisect_right(self.hi_gt, p))
        return low, high


def _best_split(V, TR, tris):
    """(axis, plane) maximising low x high (both non-empty, at most 31 triangles left at the
    node); ties: fewer at the node, then the axis along which the node's triangles extend
    furthest (x before y before z on equal extents), then the lower plane. None = leaf."""
    n = len(tris)
    best = None
    for a in range(3):
        ax = _Axis([sorted(V[v][a] for v in TR[t][1:]) for t in tris])
        ext = ax.values[-1] - ax.values[0]
        for p in ax.values:
            low, high = ax.counts(p)
            stay = n - low - high
            if not low or not high or stay > MAX_NODE_TRIS:
                continue
            key = (-low * high, stay, -ext, a, p)
            if best is None or key < best:
                best = key
    return None if best is None else (best[3], best[4])


def tree(vertices, triangles):
    """Build a collision block for a sector's geometry without touching it.

    ``vertices``: (x, y, z) per vertex; ``triangles``: (material, a, b, c) per triangle.
    Returns ``(block, order)``: the 384-byte block, and ``order`` with ``order[k]`` = the index in
    ``triangles`` of the triangle that must be stored at position ``k`` (the block refers to the
    triangles in that order). Raises :class:`CollCapacityError` when the sector needs more than one
    tree can hold.

    Rules (the node format, the 128-node limit and the classification are RW's [V]; the choice of
    the split is this builder's own, see docs/re/physics.md 3.1): every node with two or more
    triangles splits at the vertex coordinate that maximises (triangles below) x (triangles above)
    if one with both sides non-empty exists; the triangles that :func:`side` keeps at the node stay
    there (at most 31); nodes 64-127 are always leaves (at most 31 triangles); the triangles are
    stored in pre-order (the node's own, then the low subtree, then the high subtree), keeping
    their relative input order."""
    V, TR = list(vertices), list(triangles)
    nv = len(V)
    for t in TR:
        if len(t) != 4 or max(t[1:]) >= nv or min(t[1:]) < 0:
            raise CollError(f"triangle {t} does not index the {nv} vertices")
    if nv > 256:
        raise CollCapacityError(f"{nv} vertices: the plane vertex is a u8")
    block = bytearray(COLL_SIZE)
    order = []
    first_vertex = [{} for _ in range(3)]
    for i in range(nv - 1, -1, -1):
        for a in range(3):
            first_vertex[a][V[i][a]] = i          # lowest index with that coordinate

    def emit(idx, b0, b1, own):
        if len(own) > MAX_NODE_TRIS:
            raise CollCapacityError(f"node {idx} needs {len(own)} triangles (at most {MAX_NODE_TRIS})")
        if own and len(order) > MAX_FIRST_TRI:
            raise CollCapacityError(f"node {idx} would start at triangle {len(order)} (u8)")
        block[3 * idx:3 * idx + 3] = bytes((b0 | len(own), b1, len(order) if own else 0))
        order.extend(own)

    stack = [(1, list(range(len(TR))))]
    while stack:
        idx, tris = stack.pop()
        sp = _best_split(V, TR, tris) if idx <= LAST_SPLIT_NODE and len(tris) > 1 else None
        if sp is None:
            emit(idx, 0, 0, tris)
            continue
        a, p = sp
        own, low, high = [], [], []
        parts = {-1: low, 0: own, 1: high}
        for t in tris:
            parts[side([V[v][a] for v in TR[t][1:]], p)].append(t)
        emit(idx, 0x80 | (a << 5), first_vertex[a][p], own)
        stack += [(2 * idx + 1, high), (2 * idx, low)]
    return bytes(block), order


def build(sector) -> list:
    """Rebuild ``sector.coll`` for the sector's current triangles and **re-order**
    ``sector.triangles`` to match (in place). Returns the permutation (``new[k] = old[order[k]]``).
    A sector without triangles gets no tree (``coll_present`` 0, as the empty sectors in the
    corpus). The BinMesh is not affected (it does not depend on the triangle order); call
    :meth:`~dmbmx.formats.rwworld.AtomicSector.rebuild_bin_mesh` after a geometry edit as before."""
    if not sector.triangles:
        sector.coll, sector.coll_present = b"", 0
        return []
    block, order = tree(sector.vertices, sector.triangles)
    sector.triangles = [sector.triangles[i] for i in order]
    sector.coll, sector.coll_present = block, 1
    return order


# ------------------------------------------------------------------------------- queries (ports)

def _neg(x):
    """The RW walks test the float's sign bit (``(int)f < 0``): -0.0 counts as negative."""
    return math.copysign(1.0, x) < 0


def _block(sector, block):
    blk = sector.coll if block is None else block
    if len(blk) != COLL_SIZE:
        raise CollError(f"collision block is {len(blk)} bytes, expected {COLL_SIZE}")
    return blk


def _split_plane(blk, V, node, b0, b1):
    a = (b0 >> 5) & 3
    if a > 2 or b1 >= len(V) or 2 * node + 1 >= COLL_NODES:
        raise CollError(f"node {node}: bad split", offset=3 * node)
    return a, V[b1][a]


def line_test(sector, start, end, *, block=None):
    """Triangles RW's per-sector line test (0x004BE560) hands to its triangle test for the segment
    ``start``-``end``, in visiting order. Port of the walk: every visited node's own triangles are
    tested; at a split the walk goes on to the child on the start's side with the segment cut at
    the plane, and pushes the other child with the rest of the segment if the segment crosses.
    ``block`` defaults to ``sector.coll``. Intersect the result with :func:`segment_hits`."""
    blk = _block(sector, block)
    V = sector.vertices
    out, stack = [], []
    node, s0, s1 = 1, tuple(start), tuple(end)
    while True:
        b0, b1, b2 = blk[3 * node], blk[3 * node + 1], blk[3 * node + 2]
        out.extend(range(b2, b2 + (b0 & 0x1F)))
        if not b0 & 0x80:
            if not stack:
                return out
            node, s0, s1 = stack.pop()
            continue
        a, p = _split_plane(blk, V, node, b0, b1)
        d0, d1 = s0[a] - p, s1[a] - p
        if _neg(d0) != _neg(d1):
            f = d0 / (d1 - d0)
            ip = tuple(s0[i] - (s1[i] - s0[i]) * f for i in range(3))
            stack.append((2 * node + 1 if _neg(d0) else 2 * node, ip, s1))
            s1 = ip
        node = 2 * node if _neg(d0) else 2 * node + 1


def sphere_test(sector, center, radius, *, block=None):
    """Triangles RW's per-sector sphere test (0x004BED60) hands to its triangle test, in visiting
    order. Port of the walk: the sphere's box goes to the low child if its minimum is below the
    plane and to the high child if its maximum is above (both if it straddles). RW quirk [V]: a
    sphere of radius 0 exactly on a split plane revisits the same node forever; the port raises
    :class:`CollError` instead."""
    blk = _block(sector, block)
    V = sector.vertices
    lo = [c - radius for c in center]
    hi = [c + radius for c in center]
    out, stack, node = [], [], 1
    while True:
        b0, b1, b2 = blk[3 * node], blk[3 * node + 1], blk[3 * node + 2]
        out.extend(range(b2, b2 + (b0 & 0x1F)))
        if not b0 & 0x80:
            if not stack:
                return out
            node = stack.pop()
            continue
        a, p = _split_plane(blk, V, node, b0, b1)
        below, above = _neg(lo[a] - p), _neg(p - hi[a])
        if below:
            if above:
                stack.append(2 * node + 1)
            node = 2 * node
        elif above:
            node = 2 * node + 1
        else:
            raise CollError(f"node {node}: zero-radius sphere on the plane (RW 0x004BED60 loops forever)")


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def segment_hits(sector, start, end, tris=None, eps=1e-9):
    """Indices of the triangles (from ``tris``, default all) that the segment intersects
    (two-sided Moller-Trumbore, parameter in [0, 1]). [I] RW's own triangle test (0x004BEAB0) is
    the same algorithm with a 1e-6 determinant threshold; whether it culls back faces was not
    settled, so this check is two-sided."""
    V, TR = sector.vertices, sector.triangles
    d = _sub(end, start)
    out = []
    for t in (range(len(TR)) if tris is None else tris):
        _, a, b, c = TR[t]
        e1, e2 = _sub(V[b], V[a]), _sub(V[c], V[a])
        pv = _cross(d, e2)
        det = _dot(e1, pv)
        if abs(det) <= eps:
            continue
        inv = 1.0 / det
        tv = _sub(start, V[a])
        u = _dot(tv, pv) * inv
        if u < 0.0 or u > 1.0:
            continue
        qv = _cross(tv, e1)
        v = _dot(d, qv) * inv
        if v < 0.0 or u + v > 1.0:
            continue
        if 0.0 <= _dot(e2, qv) * inv <= 1.0:
            out.append(t)
    return out


def sphere_hits(sector, center, radius, tris=None):
    """Indices of the triangles (from ``tris``, default all) within ``radius`` of ``center``
    (closest-point test). [I] Stands in for RW's sphere-triangle test 0x004BF0B0 (not ported)."""
    V, TR = sector.vertices, sector.triangles
    out = []
    for t in (range(len(TR)) if tris is None else tris):
        _, a, b, c = TR[t]
        dd = _sub(center, _closest(center, V[a], V[b], V[c]))
        if _dot(dd, dd) <= radius * radius:
            out.append(t)
    return out


def _closest(p, a, b, c):
    """Closest point to p on triangle abc (Ericson, Real-Time Collision Detection 5.1.5)."""
    ab, ac, ap = _sub(b, a), _sub(c, a), _sub(p, a)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        return a
    bp = _sub(p, b)
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3)
        return tuple(a[i] + v * ab[i] for i in range(3))
    cp = _sub(p, c)
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6)
        return tuple(a[i] + w * ac[i] for i in range(3))
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return tuple(b[i] + w * (c[i] - b[i]) for i in range(3))
    denom = va + vb + vc
    if denom == 0:          # degenerate triangle: the nearest vertex
        return min((a, b, c), key=lambda q: _dot(_sub(p, q), _sub(p, q)))
    v, w = vb / denom, vc / denom
    return tuple(a[i] + ab[i] * v + ac[i] * w for i in range(3))

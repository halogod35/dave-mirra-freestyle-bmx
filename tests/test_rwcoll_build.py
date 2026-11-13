"""DM-049: the RW per-sector collision-tree builder (dmbmx.formats.rwcoll.build / tree), the ports
of RW's line and sphere walks, colSectorSize, and the world writer's rebuild / sector split."""
import copy
import random
from collections import Counter

import pytest

from conftest import corpus_files
from fuzz import fuzz_parser
from dmbmx.core.errors import FormatError
from dmbmx.formats import rwcoll, rwworld


# ------------------------------------------------------------------ helpers
def grid_sector(n=6, step=1.0, y=0.0, bumps=True):
    """A synthetic ground sector: an n x n grid of quads (2 triangles each), bumpy in y."""
    s = rwworld.AtomicSector()
    rnd = random.Random(n)
    for i in range(n):
        for j in range(n):
            base = len(s.vertices)
            h = [y + (rnd.uniform(0, 0.5) if bumps else 0.0) for _ in range(4)]
            s.vertices += [(i * step, h[0], j * step), ((i + 1) * step, h[1], j * step),
                           ((i + 1) * step, h[2], (j + 1) * step), (i * step, h[3], (j + 1) * step)]
            s.triangles += [(0, base, base + 1, base + 2), (0, base, base + 2, base + 3)]
    s.normals = [bytes((0, 127, 0, 0xCD))] * len(s.vertices)
    s.prelit = [(255, 255, 255, 255)] * len(s.vertices)
    s.uvs = [(0.0, 0.0)] * len(s.vertices)
    s.inf = (0.0, y, 0.0)
    s.sup = (n * step, y + 0.5, n * step)
    s.rebuild_bin_mesh()
    return s


def world_of(*sectors):
    w = rwworld.World()
    w.flags = 0x4000003C
    w.surface = (0.6, 0.0, 0.2)
    w.materials = [rwworld.Material(texture=rwworld.Texture("grass"))]
    w.material_indices = [-1]
    if len(sectors) == 1:
        w.root = sectors[0]
    else:
        a, b = sectors
        w.root = rwworld.PlaneSector(0, b.inf[0], a, b, a.sup[0], b.inf[0])
    return w


def unseparable_sector(count=40):
    """``count`` triangles that each span the whole box on every axis (no plane separates any two),
    with slightly different centroids: one collision tree cannot hold them (> 31 at one node)."""
    s = rwworld.AtomicSector()
    for k in range(count):
        d = 0.01 * k
        base = len(s.vertices)
        s.vertices += [(-10.0, -10.0, -10.0 + d), (10.0, 10.0, -10.0 + 2 * d), (0.0 + d, 0.0, 10.0)]
        s.triangles.append((0, base, base + 1, base + 2))
    s.normals = [bytes((0, 127, 0, 0xCD))] * len(s.vertices)
    s.prelit = [(255, 255, 255, 255)] * len(s.vertices)
    s.uvs = [(0.0, 0.0)] * len(s.vertices)
    s.inf, s.sup = (-10.0, -10.0, -10.0), (10.0, 10.0, 10.0)
    s.rebuild_bin_mesh()
    return s


def queries_agree(s_ref, s_new, rng, n=10):
    """Line and sphere queries find the same triangles (as vertex-position sets) through both trees,
    and exactly what brute force finds."""
    def key(s, t):
        return frozenset(s.vertices[v] for v in s.triangles[t][1:])

    lo, hi = s_ref.inf, s_ref.sup
    pad = [(hi[i] - lo[i]) * 0.1 + 0.5 for i in range(3)]

    def rp():
        return tuple(rng.uniform(lo[i] - pad[i], hi[i] + pad[i]) for i in range(3))

    for _ in range(n):
        a = rp()
        b = rp() if rng.random() < 0.5 else tuple(x + rng.uniform(-1, 1) for x in a)
        brute = {key(s_ref, t) for t in rwcoll.segment_hits(s_ref, a, b)}
        for s in (s_ref, s_new):
            got = {key(s, t) for t in rwcoll.segment_hits(s, a, b, rwcoll.line_test(s, a, b))}
            assert got == brute, ("line", a, b)
        c, r = rp(), rng.uniform(0.05, 3.0)
        brute = {key(s_ref, t) for t in rwcoll.sphere_hits(s_ref, c, r)}
        for s in (s_ref, s_new):
            got = {key(s, t) for t in rwcoll.sphere_hits(s, c, r, rwcoll.sphere_test(s, c, r))}
            assert got == brute, ("sphere", c, r)


# ------------------------------------------------------------------ unit
def test_side_rule():
    assert rwcoll.side((0, 1, 2), 3) == -1 and rwcoll.side((4, 5, 6), 3) == 1
    assert rwcoll.side((0, 5, 1), 3) == 0                 # crosses
    assert rwcoll.side((0, 1, 3), 3) == -1                # one vertex on the plane: goes to its side
    assert rwcoll.side((3, 4, 5), 3) == 1
    assert rwcoll.side((0, 3, 3), 3) == 0                 # an edge in the plane stays at the node
    assert rwcoll.side((3, 3, 5), 3) == 0
    assert rwcoll.side((3, 3, 3), 3) == 0                 # in the plane


def test_build_synthetic_grid():
    s = grid_sector(8)                                    # 128 triangles, 256 vertices
    old = list(s.triangles)
    order = rwcoll.build(s)
    assert sorted(order) == list(range(len(old))) and s.triangles == [old[i] for i in order]
    assert len(s.coll) == rwcoll.COLL_SIZE and s.coll_present == 1 and s.coll[:3] == b"\0\0\0"
    assert rwcoll.check(s) == []
    nodes = rwcoll.decode(s.coll)
    assert all(n.count <= rwcoll.MAX_NODE_TRIS for n in nodes)
    assert all(n.index < 64 for n in nodes if n.split)
    assert rwcoll.used_size(s.coll) == 3 * max(n.index for n in nodes) + 3
    # pre-order storage: node ranges are consecutive in walk order
    pos = 0
    for n in nodes:
        if n.count:
            assert n.start == pos
            pos += n.count
    assert pos == len(s.triangles)
    queries_agree(s, s, random.Random(1), n=30)


def test_build_is_deterministic_and_idempotent():
    a, b = grid_sector(5), grid_sector(5)
    rwcoll.build(a)
    rwcoll.build(b)
    assert a.coll == b.coll and a.triangles == b.triangles
    blk = a.coll
    assert rwcoll.build(a) == list(range(len(a.triangles)))   # already sorted: identity
    assert a.coll == blk


def test_build_empty_and_errors():
    s = rwworld.AtomicSector()
    assert rwcoll.build(s) == [] and s.coll == b"" and s.coll_present == 0
    with pytest.raises(rwcoll.CollError):
        rwcoll.tree([(0.0, 0.0, 0.0)], [(0, 0, 1, 2)])
    with pytest.raises(rwcoll.CollCapacityError):
        rwcoll.tree([(float(i), 0.0, 0.0) for i in range(257)], [(0, 0, 1, 2)])
    with pytest.raises(rwcoll.CollCapacityError, match="at most 31"):
        rwcoll.build(unseparable_sector(40))
    s = unseparable_sector(31)                            # exactly fits one leaf
    rwcoll.build(s)
    assert rwcoll.check(s) == [] and rwcoll.decode(s.coll)[0].count == 31


def test_sphere_walk_rw_quirk():
    s = grid_sector(3, bumps=False)
    rwcoll.build(s)
    root = rwcoll.decode(s.coll)[0]
    p = list(s.vertices[root.vertex])
    with pytest.raises(rwcoll.CollError, match="loops forever"):
        rwcoll.sphere_test(s, p, 0.0)
    assert rwcoll.sphere_test(s, p, 0.001)                # any radius > 0 is fine


def test_world_write_rebuilds_changed_sector_only():
    w = world_of(grid_sector(4))
    data = w.write()
    w2 = rwworld.read(data)
    s = w2.sectors()[0]
    assert rwcoll.check(s) == [] and w2.col_sector_size == rwcoll.col_sector_size(w2.sectors())
    assert not s.geometry_changed() and w2.write() == data
    # add a triangle: the tree is rebuilt, the triangle is found by the game's walk (Python port)
    base = len(s.vertices)
    s.vertices += [(1.0, 5.0, 1.0), (3.0, 5.0, 1.0), (1.0, 5.0, 3.0)]
    s.normals += [bytes((0, 127, 0, 0xCD))] * 3
    s.prelit += [(255, 255, 255, 255)] * 3
    s.uvs += [(0.0, 0.0)] * 3
    s.triangles.append((0, base, base + 1, base + 2))
    s.rebuild_bin_mesh()
    assert s.geometry_changed()
    out = w2.write()
    w3 = rwworld.read(out)
    s3 = w3.sectors()[0]
    assert rwcoll.check(s3) == [] and len(s3.triangles) == 33
    hits = w3.line_hits((1.5, 9.0, 1.5), (1.5, 4.0, 1.5))
    assert [s3.vertices[v][1] for v in s3.triangles[hits[0][1]][1:]] == [5.0, 5.0, 5.0]
    assert s3.sup[1] >= 5.0                               # sector box widened
    # without the rebuild (control) the old tree misses the new triangle
    w4 = rwworld.read(data)
    s4 = w4.sectors()[0]
    s4.vertices += [(1.0, 5.0, 1.0), (3.0, 5.0, 1.0), (1.0, 5.0, 3.0)]
    s4.normals += [bytes((0, 127, 0, 0xCD))] * 3
    s4.prelit += [(255, 255, 255, 255)] * 3
    s4.uvs += [(0.0, 0.0)] * 3
    s4.triangles.append((0, base, base + 1, base + 2))
    w5 = rwworld.read(w4.write(rebuild_collision=False))
    assert not w5.line_hits((1.5, 9.0, 1.5), (1.5, 4.0, 1.5))
    assert any("referenced" in p for p in rwcoll.check(w5.sectors()[0]))


def test_world_widens_ancestor_planes():
    a, b = grid_sector(3), grid_sector(3)
    b.vertices = [(x + 3.0, y, z) for x, y, z in b.vertices]
    b.inf, b.sup = (3.0, 0.0, 0.0), (6.0, 0.5, 3.0)
    w = rwworld.read(world_of(a, b).write())
    left = w.sectors()[0]
    # move a left-sector triangle 2 m into the right half: queries there must still reach it
    m, i, j, k = left.triangles[0]
    for v in (i, j, k):
        x, y, z = left.vertices[v]
        left.vertices[v] = (x + 4.0, y + 3.0, z)
    w2 = rwworld.read(w.write())
    assert w2.root.left_value >= 5.0
    s = w2.sectors()[0]
    hit = w2.line_hits((4.5, 9.0, 0.5), (4.5, 2.0, 0.5))
    assert any(sec is s for sec, _ in hit)


def test_world_splits_sector_that_does_not_fit():
    w = world_of(unseparable_sector(40))
    with pytest.raises(rwcoll.CollCapacityError):
        copy.deepcopy(w).rebuild_collision(split=False)
    data = w.write()                                       # split=True by default
    w2 = rwworld.read(data)
    secs = w2.sectors()
    assert len(secs) == 2 and w2.num_plane_sectors == 1 and w2.num_triangles == 40
    assert all(rwcoll.check(s) == [] for s in secs)
    assert w2.col_sector_size == rwcoll.col_sector_size(secs)
    assert sum(len(s.bin_mesh.meshes[0][1]) for s in secs) == 120
    p = w2.root
    assert p.left_value >= max(v[p.axis // 4] for v in secs[0].vertices)
    assert p.right_value <= min(v[p.axis // 4] for v in secs[1].vertices)
    # every triangle is still found by a segment through it (both halves)
    rng = random.Random(3)
    for _ in range(40):
        a = (rng.uniform(-9, 9), 12.0, rng.uniform(-9, 9))
        b = (a[0], -12.0, a[2])
        brute = sum(len(rwcoll.segment_hits(s, a, b)) for s in secs)
        assert len(w2.line_hits(a, b)) == brute


def test_walks_reject_bad_blocks():
    s = grid_sector(3)
    rwcoll.build(s)

    def run(blk):
        rwcoll.line_test(s, (0.5, 2, 0.5), (2.5, -1, 2.5), block=blk)
        rwcoll.sphere_test(s, (1.5, 0.2, 1.5), 0.7, block=blk)
        return rwcoll.used_size(blk)

    fuzz_parser(run, s.coll, n=300, seed=49)
    with pytest.raises(FormatError):
        run(b"\x00" * 10)


# ------------------------------------------------------------------ corpus
def _bsp_files():
    files = [f for f in corpus_files("._sd") if f.name.lower() == "bsp._sd"]
    if not files:
        pytest.skip("no corpus")
    assert len(files) == 12
    return files


def test_corpus_col_sector_size_and_layout_facts():
    """[V] colSectorSize = sum of the used tree bytes rounded to 4; the vertex arrays are grouped by
    material, in first-use order of the stored (tree-sorted) triangles; the bytes after the used
    part of the 384 are not zero (never loaded)."""
    junk = Counter()
    for f in _bsp_files():
        w = rwworld.read(f.read_bytes())
        assert w.col_sector_size == rwcoll.col_sector_size(w.sectors()), f.parent.name
        for s in w.sectors():
            if not s.coll_present:
                continue
            junk[any(s.coll[rwcoll.used_size(s.coll):])] += 1
            first = {}
            for m, *vv in s.triangles:
                for v in vv:
                    first.setdefault(v, (m, len(first)))
            mats = []
            for v in range(len(s.vertices)):
                if not mats or mats[-1] != first[v][0]:
                    mats.append(first[v][0])
            assert len(mats) == len(set(mats))
            assert sorted(range(len(s.vertices)), key=lambda v: (mats.index(first[v][0]), first[v][1])) \
                == list(range(len(s.vertices)))
    assert junk == Counter({True: 783, False: 1})


def test_corpus_rebuild_every_sector():
    """The builder handles all 784 sectors: valid trees, and line / sphere queries through the
    rebuilt tree find exactly what the original tree and brute force find."""
    rng = random.Random(49)
    n = 0
    for f in _bsp_files():
        w = rwworld.read(f.read_bytes())
        for s in w.sectors():
            if not s.coll_present:
                continue
            new = copy.copy(s)
            new.triangles = list(s.triangles)
            order = rwcoll.build(new)
            assert sorted(order) == list(range(len(s.triangles)))
            assert rwcoll.check(new) == [], (f.parent.name, rwcoll.check(new)[:2])
            queries_agree(s, new, rng, n=3)
            n += 1
    assert n == 784


def test_corpus_classification_rule():
    """[V] The shipped trees follow rwcoll.side() for at least 97 % of the triangle placements
    (the rest disagree in both directions: build-time coordinates differed)."""
    agree = total = 0
    for f in _bsp_files()[:4]:
        w = rwworld.read(f.read_bytes())
        for s in w.sectors():
            if not s.coll_present:
                continue
            nodes = {n.index: n for n in rwcoll.decode(s.coll)}

            def sub(i):
                n = nodes[i]
                own = list(range(n.start, n.start + n.count))
                return own + (sub(2 * i) + sub(2 * i + 1) if n.split else [])

            for n in nodes.values():
                if not n.split:
                    continue
                p = s.vertices[n.vertex][n.axis]
                for want, ts in ((0, range(n.start, n.start + n.count)), (-1, sub(2 * n.index)), (1, sub(2 * n.index + 1))):
                    for t in ts:
                        total += 1
                        agree += rwcoll.side([s.vertices[v][n.axis] for v in s.triangles[t][1:]], p) == want
    assert agree / total > 0.97


def test_corpus_unedited_levels_untouched_and_edit_rebuilds(game_dir):
    data = (game_dir / "assets" / "WoodwardDirt" / "BSP._sd").read_bytes()
    w = rwworld.read(data)
    assert w.write() == data                              # nothing changed: original trees kept
    s = max(w.sectors(), key=lambda s: len(s.triangles))
    old_blocks = [x.coll for x in w.sectors()]
    s.triangles = s.triangles[::-1]                       # re-order only
    out = w.write()
    assert len(out) == len(data)
    w2 = rwworld.read(out)
    assert all(rwcoll.check(x) == [] for x in w2.sectors() if x.coll_present)
    assert [x.coll for x in w2.sectors()].count(None) == 0
    changed = [a != b.coll for a, b in zip(old_blocks, w2.sectors())]
    assert changed.count(True) == 1
    assert w2.col_sector_size == rwcoll.col_sector_size(w2.sectors())

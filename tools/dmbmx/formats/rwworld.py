"""Level world geometry ``BSP._sd``: a RenderWare 3.0 binary **World** (RpWorld, RW id 0x0B).

Spec: ``docs/formats/LEVEL.md`` section 4. Pure stdlib (vendored by the Blender add-on).

Grammar (RW chunk = ``u32 type, u32 size, u32 version``; version 0x00000301 everywhere [V])::

    World(0x0B)
      Struct            52 bytes: WorldHeader                                   (WORLD_FMT)
      MaterialList(0x08)  Struct{u32 n; i32 index[n]} + Material(0x07) x count(index == -1)
      PlaneSect(0x0A) | AtomicSect(0x09)   root sector (rootIsWorldSector picks which)
      Extension         empty
    PlaneSect(0x0A)
      Struct            24 bytes: u32 type (0/4/8 = x/y/z), f32 value, u32 leftIsAtomic,
                        u32 rightIsAtomic, f32 leftValue, f32 rightValue
      left sector, right sector   (PlaneSect or AtomicSect, as the two flags say)
    AtomicSect(0x09)
      Struct            44-byte header + vertex arrays + u8 triangles + collision block
      Extension         BinMesh(0x50E) (mesh split by material, triangle lists)

The object model re-encodes every struct; unknown chunks stay raw :class:`Chunk` objects.
Writing rebuilds the collision tree of every sector whose geometry changed since it was read
(DM-049, :meth:`World.rebuild_collision`) and recomputes ``colSectorSize``; unedited sectors keep
their original bytes.
Materials and textures reuse the ZMD classes (:class:`dmbmx.formats.zmd.Material`).
"""
from __future__ import annotations

import struct

from dmbmx.core.chunks import RW, Chunk, ChunkError, Reader, Writer, parse_rw
from dmbmx.core.errors import BadMagicError, FormatError, LimitError
from dmbmx.formats import rwcoll
from dmbmx.formats import zmd as _zmd
from dmbmx.formats.zmd import BinMesh, Material, Texture  # noqa: F401  (re-export)

__all__ = ["BspError", "World", "PlaneSector", "AtomicSector", "read", "write",
           "WF_TRISTRIP", "WF_POSITIONS", "WF_TEXTURED", "WF_PRELIT", "WF_NORMALS", "WF_LIGHT",
           "WF_MODULATE", "WF_TEXTURED2"]

FMT = "BSP"

WF_TRISTRIP = 0x01
WF_POSITIONS = 0x02
WF_TEXTURED = 0x04
WF_PRELIT = 0x08
WF_NORMALS = 0x10
WF_LIGHT = 0x20
WF_MODULATE = 0x40
WF_TEXTURED2 = 0x80

#: World struct: rootIsWorldSector, invWorldOrigin[3], surface (ambient, specular, diffuse),
#: numTriangles, numVertices, numPlaneSectors, numWorldSectors, colSectorSize, format.
WORLD_FMT = "<I3f3f6I"
SECTOR_HDR = "<3I3f3f2I"         # matListWindowBase, numTriangles, numVertices, inf[3], sup[3], collPresent, unused
PLANE_FMT = "<IfIIff"
#: [V] every sector with ``coll_present == 1`` carries exactly this many collision bytes.
COLL_SIZE = 384
MAX_SECTOR_DEPTH = 64          # the corpus needs 13
MAX_SECTOR_VERTS = 256         # triangles index vertices with a u8


class BspError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


def _expect(ch, tag, where):
    if ch is None or ch.tag != tag:
        got = "nothing" if ch is None else ch.name
        raise BspError(f"{where}: expected RW 0x{tag:X}, got {got}", offset=getattr(ch, "offset", None))
    if tag != RW.STRUCT and ch.children is None:
        raise BspError(f"{where}: {ch.name} is not a valid container", offset=ch.offset)
    return ch


def _kids(ch, n, where):
    if ch.children is None or len(ch.children) != n:
        raise BspError(f"{where}: expected {n} child chunks", offset=ch.offset)
    return ch.children


def _struct(ch, where):
    _expect(ch, RW.STRUCT, where)
    if ch.data is None:
        raise BspError(f"{where}: Struct is a container", offset=ch.offset)
    return ch.data


# ------------------------------------------------------------------------------- sectors

class PlaneSector:
    """RpPlaneSector: split plane ``axis`` (0 = x, 4 = y, 8 = z: byte offset into a V3d),
    ``value``; ``left``/``right`` child sectors (below / above the plane);
    ``left_value``/``right_value`` (RW: the children's extents along the axis, kept verbatim)."""
    __slots__ = ("axis", "value", "left", "right", "left_value", "right_value", "version")

    def __init__(self, axis=0, value=0.0, left=None, right=None, left_value=0.0, right_value=0.0, version=0x301):
        self.axis, self.value, self.left, self.right = axis, value, left, right
        self.left_value, self.right_value, self.version = left_value, right_value, version

    @classmethod
    def from_chunk(cls, ch, flags, depth=0):
        if depth > MAX_SECTOR_DEPTH:
            raise LimitError(f"sector tree deeper than {MAX_SECTOR_DEPTH}", fmt=FMT, offset=ch.offset)
        st, a, b = _kids(ch, 3, "PlaneSect")
        r = Reader(_struct(st, "PlaneSect"), base=st.offset + 12, fmt=FMT, what="PlaneSect struct")
        axis, value, la, ra, lv, rv = r.unpack(PLANE_FMT[1:])
        if r.remaining():
            raise BspError(f"PlaneSect struct: {len(st.data)} bytes, expected 24", offset=st.offset)
        if axis not in (0, 4, 8) or la not in (0, 1) or ra not in (0, 1):
            raise BspError(f"PlaneSect: bad axis {axis} / flags {la},{ra}", offset=st.offset)
        p = cls(axis, value, None, None, lv, rv, ch.version)
        p.left = _sector_from_chunk(a, la, flags, depth + 1)
        p.right = _sector_from_chunk(b, ra, flags, depth + 1)
        return p

    def to_chunk(self):
        v = self.version
        la, ra = isinstance(self.left, AtomicSector), isinstance(self.right, AtomicSector)
        st = struct.pack(PLANE_FMT, self.axis, self.value, la, ra, self.left_value, self.right_value)
        return Chunk(RW.PLANESECT, version=v, children=[
            Chunk(RW.STRUCT, data=st, version=v), self.left.to_chunk(), self.right.to_chunk()])


class AtomicSector:
    """RpWorldSector (RW 3.0 layout).

    ``mat_base`` (matListWindowBase; 0, or 0xCDCD junk in empty sectors), ``inf``/``sup``
    (bounding box min/max; :meth:`World.rebuild_collision` widens it to cover the vertices),
    ``coll_present``, ``unused``; per vertex ``vertices`` (3 f32),
    ``normals`` (4 raw bytes: s8 x, y, z + one pad byte, 0xCD in the corpus), ``prelit``
    (RGBA bytes), ``uvs`` (2 f32), ``uvs2`` (only with flag 0x80); ``triangles`` as
    ``(material, a, b, c)`` u8 each (indices < 256); ``coll``: raw collision block
    (384 bytes when ``coll_present``, :mod:`dmbmx.formats.rwcoll`); ``ext``: extension chunks
    (``bin_mesh`` decoded).

    :meth:`geometry_changed` tells whether ``vertices``/``triangles`` differ from what was read
    (a sector built in code counts as changed); :meth:`rebuild_collision` rebuilds ``coll`` and
    re-orders ``triangles`` (DM-049).
    """
    __slots__ = ("mat_base", "inf", "sup", "coll_present", "unused", "vertices", "normals", "prelit",
                 "uvs", "uvs2", "triangles", "coll", "ext", "bin_mesh", "version", "_geom")

    def __init__(self):
        self.mat_base, self.inf, self.sup = 0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
        self.coll_present, self.unused = 0, 0
        self.vertices, self.normals, self.prelit, self.uvs, self.uvs2 = [], None, None, None, None
        self.triangles, self.coll, self.ext, self.bin_mesh, self.version = [], b"", [], None, 0x301
        self._geom = None

    @classmethod
    def from_chunk(cls, ch, flags, depth=0):
        st, ex = _kids(ch, 2, "AtomicSect")
        data = _struct(st, "AtomicSect")
        _expect(ex, RW.EXTENSION, "AtomicSect")
        s = cls()
        s.version = ch.version
        r = Reader(data, base=st.offset + 12, fmt=FMT, what="AtomicSect struct")
        s.mat_base, nt, nv, *rest = r.unpack(SECTOR_HDR[1:])
        s.inf, s.sup = tuple(rest[0:3]), tuple(rest[3:6])
        s.coll_present, s.unused = rest[6], rest[7]
        if nv > MAX_SECTOR_VERTS:
            raise BspError(f"AtomicSect: {nv} vertices (u8 indices allow {MAX_SECTOR_VERTS})", offset=st.offset)
        s.vertices = r.tuples("3f", nv)
        if flags & WF_NORMALS:
            s.normals = [bytes(x) for x in r.tuples("4B", nv)]
        if flags & WF_PRELIT:
            s.prelit = r.tuples("4B", nv)
        if flags & (WF_TEXTURED | WF_TEXTURED2):
            s.uvs = r.tuples("2f", nv)
        if flags & WF_TEXTURED2:
            s.uvs2 = r.tuples("2f", nv)
        s.triangles = r.tuples("4B", nt)
        for t in s.triangles:
            if max(t[1:]) >= nv:
                raise BspError(f"AtomicSect: triangle index {max(t[1:])} >= {nv} vertices", offset=st.offset)
        s.coll = r.rest()
        if bool(s.coll) != bool(s.coll_present):
            raise BspError(f"AtomicSect: collPresent={s.coll_present} but {len(s.coll)} collision bytes",
                           offset=st.offset)
        s.ext = list(ex.children)
        for c in s.ext:
            if c.tag == RW.BINMESH and c.data is not None:
                s.bin_mesh = BinMesh.decode(c.data, c.offset + 12)
        s._geom = s._fingerprint()
        return s

    def _fingerprint(self):
        return (tuple(map(tuple, self.vertices)), tuple(map(tuple, self.triangles)))

    def geometry_changed(self) -> bool:
        """True if ``vertices`` or ``triangles`` differ from the state the sector was read in (or
        the collision tree was last rebuilt for), or the sector was built in code."""
        return self._geom is None or self._geom != self._fingerprint()

    def rebuild_collision(self):
        """Rebuild the collision tree for the current triangles (:func:`dmbmx.formats.rwcoll.build`):
        **re-orders** ``triangles`` and returns the permutation. Raises
        :class:`~dmbmx.formats.rwcoll.CollCapacityError` if the sector does not fit one tree."""
        order = rwcoll.build(self)
        self._geom = self._fingerprint()
        return order

    def encode_struct(self, flags):
        w = Writer()
        nv = len(self.vertices)
        if nv > MAX_SECTOR_VERTS:
            raise ValueError(f"sector has {nv} vertices; triangles use u8 indices (max {MAX_SECTOR_VERTS})")
        w.pack(SECTOR_HDR[1:], self.mat_base, len(self.triangles), nv, *self.inf, *self.sup,
               self.coll_present, self.unused)
        w.tuples("3f", self.vertices)
        arrays = []
        if flags & WF_NORMALS:
            arrays.append(("normals", self.normals, None))
        if flags & WF_PRELIT:
            arrays.append(("prelit", self.prelit, "4B"))
        if flags & (WF_TEXTURED | WF_TEXTURED2):
            arrays.append(("uvs", self.uvs, "2f"))
        if flags & WF_TEXTURED2:
            arrays.append(("uvs2", self.uvs2, "2f"))
        for name, arr, fmt in arrays:
            if arr is None or len(arr) != nv:
                raise ValueError(f"sector {name}: {0 if arr is None else len(arr)} entries, need {nv}")
            if fmt is None:
                for n in arr:
                    if len(n) != 4:
                        raise ValueError("sector normals: 4 bytes each (x, y, z, pad)")
                    w.raw(bytes(n))
            else:
                w.tuples(fmt, arr)
        for t in self.triangles:
            if max(t[1:]) >= nv:
                raise ValueError(f"triangle index {max(t[1:])} >= {nv} vertices")
        w.tuples("4B", self.triangles)
        w.raw(self.coll)
        return w.getvalue()

    def to_chunk(self, flags):
        v = self.version
        ext = []
        for c in self.ext:
            if c.tag == RW.BINMESH and self.bin_mesh is not None:
                c = Chunk(RW.BINMESH, data=self.bin_mesh.encode(), version=c.version)
            ext.append(c)
        return Chunk(RW.ATOMICSECT, version=v, children=[
            Chunk(RW.STRUCT, data=self.encode_struct(flags), version=v),
            Chunk(RW.EXTENSION, children=ext, version=v)])

    def rebuild_bin_mesh(self):
        """Regenerate the BinMesh (triangle lists, flags 0) from ``triangles`` after a geometry
        edit. [V] The shipped BinMesh holds the same triangles per material (absolute material
        index = ``mat_base + m``) but in an optimiser's order, so an unedited sector should keep
        its original BinMesh; the mesh order of the old BinMesh is kept for existing materials."""
        groups = {}
        for m, a, b, c in self.triangles:
            groups.setdefault(self.mat_base + m, []).extend((a, b, c))
        old = [m for m, _ in self.bin_mesh.meshes] if self.bin_mesh is not None else []
        order = [m for m in old if m in groups] + [m for m in groups if m not in old]
        self.bin_mesh = BinMesh(0, [(m, groups[m]) for m in order])
        if not any(c.tag == RW.BINMESH for c in self.ext):
            self.ext.append(Chunk(RW.BINMESH, data=b"", version=self.version))
        return self.bin_mesh

    def normal_vectors(self):
        """Normals as unit-ish float 3-tuples (s8 / 127), or None."""
        if self.normals is None:
            return None
        out = []
        for n in self.normals:
            x, y, z = (b - 256 if b > 127 else b for b in n[:3])
            out.append((x / 127.0, y / 127.0, z / 127.0))
        return out


def _contains(node, sector):
    stack = [node]
    while stack:
        n = stack.pop()
        if n is sector:
            return True
        if isinstance(n, PlaneSector):
            stack += [n.left, n.right]
    return False


def _widen_box(s):
    if s.vertices:
        s.inf = tuple(min([s.inf[a]] + [v[a] for v in s.vertices]) for a in range(3))
        s.sup = tuple(max([s.sup[a]] + [v[a] for v in s.vertices]) for a in range(3))


def _sub_sector(src, tris):
    """A new AtomicSector with ``tris`` (vertex indices into ``src``) and only the vertices they
    use (in first-use order); the per-vertex arrays follow, other fields are copied."""
    s = AtomicSector()
    s.mat_base, s.unused, s.version = src.mat_base, src.unused, src.version
    remap = {}
    for t in tris:
        for v in t[1:]:
            remap.setdefault(v, len(remap))
    keep = sorted(remap, key=remap.get)
    s.vertices = [src.vertices[v] for v in keep]
    for name in ("normals", "prelit", "uvs", "uvs2"):
        arr = getattr(src, name)
        setattr(s, name, None if arr is None else [arr[v] for v in keep])
    s.triangles = [(t[0], remap[t[1]], remap[t[2]], remap[t[3]]) for t in tris]
    s.inf = tuple(min(v[a] for v in s.vertices) for a in range(3))
    s.sup = tuple(max(v[a] for v in s.vertices) for a in range(3))
    s.ext = [c for c in src.ext if c.tag != RW.BINMESH]
    s.rebuild_bin_mesh()
    return s


def _sector_from_chunk(ch, is_atomic, flags, depth):
    if is_atomic:
        _expect(ch, RW.ATOMICSECT, "sector")
        return AtomicSector.from_chunk(ch, flags, depth)
    _expect(ch, RW.PLANESECT, "sector")
    return PlaneSector.from_chunk(ch, flags, depth)


# ------------------------------------------------------------------------------- world

class World:
    """A decoded ``BSP._sd``.

    ``root_is_sector``, ``inv_origin`` (RW ``invWorldOrigin``), ``surface`` (ambient, specular,
    diffuse), ``num_triangles``/``num_vertices``/``num_plane_sectors``/``num_sectors`` (header
    counts; recomputed on write unless ``keep_counts``), ``col_sector_size`` (sum of the
    sectors' used collision-tree bytes rounded up to 4 [V]; recomputed with the counts: the
    game's world reader allocates exactly that much),
    ``flags`` (format word, 0x4000003C in all 12 levels), ``materials`` (list of
    :class:`~dmbmx.formats.zmd.Material`), ``material_indices`` (MaterialList indices,
    all -1 = one material per slot), ``root`` (sector tree), ``ext`` (World extension chunks).
    """

    def __init__(self):
        self.root_is_sector = 0
        self.inv_origin = (0.0, 0.0, 0.0)
        self.surface = (0.0, 0.0, 0.0)
        self.num_triangles = self.num_vertices = self.num_plane_sectors = self.num_sectors = 0
        self.col_sector_size = 0
        self.flags = 0
        self.materials = []
        self.material_indices = []
        self.root = None
        self.ext = []
        self.version = 0x301

    # -- io ---------------------------------------------------------------
    @classmethod
    def read(cls, data):
        data = bytes(data)
        if len(data) < 12 or struct.unpack_from("<I", data)[0] != RW.WORLD:
            raise BadMagicError("expected a RW World chunk (0x0B)", fmt=FMT, offset=0)
        try:
            roots = parse_rw(data)
        except ChunkError as e:
            raise type(e)(f"chunk tree: {e.msg}", fmt=FMT, offset=e.offset) from e
        if len(roots) != 1:
            raise BspError(f"expected one World chunk, found {len(roots)}", offset=0)
        wc = _expect(roots[0], RW.WORLD, "file")
        kids = _kids(wc, 4, "World")
        w = cls()
        w.version = wc.version
        r = Reader(_struct(kids[0], "World"), base=kids[0].offset + 12, fmt=FMT, what="World struct")
        v = r.unpack(WORLD_FMT[1:])
        if r.remaining():
            raise BspError(f"World struct: {len(kids[0].data)} bytes, expected 52", offset=kids[0].offset)
        w.root_is_sector = v[0]
        w.inv_origin, w.surface = tuple(v[1:4]), tuple(v[4:7])
        (w.num_triangles, w.num_vertices, w.num_plane_sectors, w.num_sectors,
         w.col_sector_size, w.flags) = v[7:13]
        try:
            w.materials, w.material_indices = _matlist_from_chunk(_expect(kids[1], RW.MATLIST, "World"))
        except _zmd.ZmdError as e:
            raise BspError(f"material list: {e.msg}", offset=e.offset) from e
        w.root = _sector_from_chunk(kids[2], w.root_is_sector, w.flags, 0)
        w.ext = list(_expect(kids[3], RW.EXTENSION, "World").children)
        return w

    def write(self, keep_counts=False, rebuild_collision=True) -> bytes:
        return self.to_chunk(keep_counts, rebuild_collision).to_bytes()

    def to_chunk(self, keep_counts=False, rebuild_collision=True):
        """``rebuild_collision``: first rebuild the collision trees of the sectors whose geometry
        changed (:meth:`rebuild_collision`, splitting sectors that do not fit one tree).
        ``keep_counts``: write the header counts and ``colSectorSize`` as they are."""
        v = self.version
        if rebuild_collision:
            self.rebuild_collision()
        if not keep_counts:
            self.update_counts()
        st = struct.pack(WORLD_FMT, isinstance(self.root, AtomicSector), *self.inv_origin, *self.surface,
                         self.num_triangles, self.num_vertices, self.num_plane_sectors, self.num_sectors,
                         self.col_sector_size, self.flags)
        root = self.root.to_chunk(self.flags) if isinstance(self.root, AtomicSector) else self._plane_chunk(self.root)
        return Chunk(RW.WORLD, version=v, children=[
            Chunk(RW.STRUCT, data=st, version=v), _matlist_to_chunk(self.materials, self.material_indices, v),
            root, Chunk(RW.EXTENSION, children=list(self.ext), version=v)])

    def _plane_chunk(self, p):
        kids = []
        for s in (p.left, p.right):
            kids.append(s.to_chunk(self.flags) if isinstance(s, AtomicSector) else self._plane_chunk(s))
        st = struct.pack(PLANE_FMT, p.axis, p.value, isinstance(p.left, AtomicSector),
                         isinstance(p.right, AtomicSector), p.left_value, p.right_value)
        return Chunk(RW.PLANESECT, version=p.version,
                     children=[Chunk(RW.STRUCT, data=st, version=p.version)] + kids)

    # -- views ------------------------------------------------------------
    @property
    def origin(self):
        """World origin = ``-invWorldOrigin`` (the engine's ZGE coordinates are relative to it)."""
        return tuple(-c for c in self.inv_origin)

    def sectors(self):
        """All :class:`AtomicSector` objects in file (depth-first, left before right) order."""
        out, stack = [], [self.root]
        while stack:
            s = stack.pop()
            if isinstance(s, AtomicSector):
                out.append(s)
            else:
                stack += [s.right, s.left]
        return out

    def plane_sectors(self):
        out, stack = [], [self.root]
        while stack:
            s = stack.pop()
            if isinstance(s, PlaneSector):
                out.append(s)
                stack += [s.right, s.left]
        return out

    def update_counts(self):
        secs = self.sectors()
        self.num_sectors = len(secs)
        self.num_plane_sectors = len(self.plane_sectors())
        self.num_triangles = sum(len(s.triangles) for s in secs)
        self.num_vertices = sum(len(s.vertices) for s in secs)
        self.col_sector_size = rwcoll.col_sector_size(secs)

    # -- collision (DM-049) ------------------------------------------------
    def rebuild_collision(self, force=False, split=True):
        """Rebuild the collision tree of every sector whose geometry changed (all sectors with
        ``force``). A sector that does not fit one tree (more than 31 triangles that no split
        separates, more than 256 vertices, ...) is **split in two** with ``split`` (see
        :meth:`split_sector`), recursively; without ``split`` the
        :class:`~dmbmx.formats.rwcoll.CollCapacityError` propagates. Returns the rebuilt
        :class:`AtomicSector` objects. The box (``inf``/``sup``) of a rebuilt sector is widened to
        cover its vertices."""
        done = []
        for s in self.sectors():
            if force or s.geometry_changed():
                done += self._rebuild_one(s, split, 0)
        return done

    def _rebuild_one(self, s, split, depth):
        _widen_box(s)
        try:
            if len(s.vertices) > MAX_SECTOR_VERTS:
                raise rwcoll.CollCapacityError(f"{len(s.vertices)} vertices (u8 indices)")
            s.rebuild_collision()
            self._widen_ancestors(s)
            return [s]
        except rwcoll.CollCapacityError:
            if not split or depth >= MAX_SECTOR_DEPTH or len(s.triangles) < 2:
                raise
        plane = self.split_sector(s)
        return self._rebuild_one(plane.left, split, depth + 1) + self._rebuild_one(plane.right, split, depth + 1)

    def split_sector(self, sector, axis=None, value=None):
        """Replace ``sector`` in the BSP by a :class:`PlaneSector` with two new sectors and return
        it. Whole triangles go to the side of their centroid (no clipping), along ``axis``
        (0/1/2, default: the longest extent of the centroids) at ``value`` (default: the median
        centroid); each side keeps only the vertices it uses. ``left_value``/``right_value`` are
        the children's extents along the axis (the maximum of the left one, the minimum of the
        right one), which is what RW 3.1's world walk tests [V: 0x004BD400 reads only these two,
        not ``value``; in the corpus leftValue >= every left vertex and rightValue <= every right
        vertex, 1099/1099 planes]. The BinMesh of both halves is rebuilt; their collision trees
        are not (call :meth:`rebuild_collision`, which treats them as changed)."""
        if len(sector.triangles) < 2:
            raise ValueError("a sector needs at least two triangles to split")
        V, TR = sector.vertices, sector.triangles
        cent = [[sum(V[v][a] for v in t[1:]) / 3 for a in range(3)] for t in TR]
        if axis is None:
            axis = max(range(3), key=lambda a: max(c[a] for c in cent) - min(c[a] for c in cent))
        if value is None:
            cs = sorted(c[axis] for c in cent)
            value = cs[len(cs) // 2]
            if value == cs[0]:
                value = next((x for x in cs if x > value), value)
        left = [k for k, c in enumerate(cent) if c[axis] < value]
        right = [k for k, c in enumerate(cent) if c[axis] >= value]
        if not left or not right:
            raise ValueError(f"split at {value} on axis {axis} leaves one side empty")
        halves = [_sub_sector(sector, [TR[k] for k in ks]) for ks in (left, right)]
        lv = max(v[axis] for v in halves[0].vertices)
        rv = min(v[axis] for v in halves[1].vertices)
        plane = PlaneSector(axis * 4, value, halves[0], halves[1], lv, rv, sector.version)
        self._replace(sector, plane)
        return plane

    def line_hits(self, start, end):
        """``[(sector, triangle index)]`` the segment intersects, found the way the game does:
        the world walk picks the sectors (routing of RW 0x004BD400: a side is visited when the
        segment reaches its ``left_value``/``right_value``; the segment clipping is left out,
        which can only add sectors), then each sector's collision tree
        (:func:`dmbmx.formats.rwcoll.line_test`) picks the triangles to intersect
        (:func:`dmbmx.formats.rwcoll.segment_hits`). Triangles the tree does not reference are
        not found, exactly as in the game."""
        out, stack = [], [self.root]
        while stack:
            n = stack.pop()
            if isinstance(n, AtomicSector):
                if n.coll_present and n.coll:
                    out += [(n, t) for t in rwcoll.segment_hits(n, start, end, rwcoll.line_test(n, start, end))]
                continue
            a = n.axis // 4
            lo, hi = min(start[a], end[a]), max(start[a], end[a])
            if hi >= n.right_value:
                stack.append(n.right)
            if lo <= n.left_value:
                stack.append(n.left)
        return out

    def _widen_ancestors(self, sector):
        """Make every PlaneSect above ``sector`` route queries to it wherever its vertices are:
        on the way down, ``left_value`` >= the sector's maximum (left side) or ``right_value`` <=
        its minimum (right side) along the plane's axis. RW 3.1's world walk 0x004BD400 visits a
        side only where the query reaches that side's value [V], so geometry moved or added past a
        plane would otherwise never be tested. A no-op for geometry inside its region."""
        if not sector.vertices:
            return
        path, node = [], self.root
        while node is not sector:
            if not isinstance(node, PlaneSector):
                raise ValueError("sector is not part of this world")
            left = _contains(node.left, sector)
            path.append((node, left))
            node = node.left if left else node.right
        for p, left in path:
            a = p.axis // 4
            if left:
                p.left_value = max(p.left_value, max(v[a] for v in sector.vertices))
            else:
                p.right_value = min(p.right_value, min(v[a] for v in sector.vertices))

    def _replace(self, old, new):
        if self.root is old:
            self.root = new
            return
        for p in self.plane_sectors():
            if p.left is old:
                p.left = new
                return
            if p.right is old:
                p.right = new
                return
        raise ValueError("sector is not part of this world")

    def texture_names(self):
        """Texture name per material slot (None when untextured)."""
        return [m.texture.name if m is not None and m.texture is not None else None for m in self.material_list()]

    def material_list(self):
        out, it = [], iter(self.materials)
        for i in self.material_indices:
            out.append(next(it, None) if i < 0 else out[i] if 0 <= i < len(out) else None)
        return out

    def meshes(self):
        """One dict per non-empty sector: ``positions``, ``normals`` (floats), ``uvs``, ``colors``,
        ``indices`` (flat, file order a, b, c), ``material_ids`` (absolute material slot)."""
        for k, s in enumerate(self.sectors()):
            if not s.triangles:
                continue
            idx, mids = [], []
            for m, a, b, c in s.triangles:
                idx += (a, b, c)
                mids.append(s.mat_base + m)
            yield {"name": f"sector{k}", "sector": k, "positions": list(s.vertices),
                   "normals": s.normal_vectors(), "uvs": list(s.uvs) if s.uvs else None,
                   "colors": list(s.prelit) if s.prelit else None, "indices": idx, "material_ids": mids}

    def bbox(self):
        pts = [v for s in self.sectors() for v in s.vertices]
        if not pts:
            return None
        return tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3))


def _matlist_from_chunk(ch):
    kids = ch.children
    if not kids:
        raise BspError("MaterialList: empty", offset=ch.offset)
    r = Reader(_struct(kids[0], "MaterialList"), base=kids[0].offset + 12, fmt=FMT, what="MaterialList struct")
    n = r.u32()
    if n > len(kids[0].data) // 4:
        raise BspError(f"MaterialList: count {n} too large", offset=kids[0].offset)
    idx = [x - (1 << 32) if x >= 1 << 31 else x for x in r.array("I", n)]
    if r.remaining():
        raise BspError("MaterialList struct: stray bytes", offset=kids[0].offset)
    new = sum(1 for i in idx if i < 0)
    if len(kids) - 1 != new:
        raise BspError(f"MaterialList: {len(kids) - 1} materials, index list needs {new}", offset=ch.offset)
    mats = []
    for c in kids[1:]:
        _expect(c, RW.MATERIAL, "MaterialList")
        mats.append(_zmd._material_from_chunk(c))
    return mats, idx


def _matlist_to_chunk(mats, idx, ver):
    if sum(1 for i in idx if i < 0) != len(mats):
        raise ValueError("material_indices: the number of -1 entries must equal len(materials)")
    st = struct.pack(f"<I{len(idx)}i", len(idx), *idx)
    return Chunk(RW.MATLIST, version=ver, children=[Chunk(RW.STRUCT, data=st, version=ver)]
                 + [_zmd._material_to_chunk(m, ver) for m in mats])


def read(data) -> World:
    """Parse ``BSP._sd`` bytes. Raises :class:`~dmbmx.core.errors.FormatError` on malformed input."""
    try:
        return World.read(data)
    except (struct.error, IndexError, ValueError) as e:
        if isinstance(e, FormatError):
            raise
        raise BspError(f"decode failed: {e!r}") from e


def write(world: World, keep_counts=False, rebuild_collision=True) -> bytes:
    return world.write(keep_counts, rebuild_collision)

"""A recording mock of 3ds Max's pymxs for the importer tests (3ds Max is not installed here).

It implements exactly the MAXScript functions the importer calls, with MAXScript's rules checked:
1-based indices, ranges (face/vertex/map ids), files that must exist for bitmaps, skin bone ids, and
keyframes recorded under ``pymxs.animate``/``pymxs.attime``. Any other attribute raises, so a call
the mock does not know (a typo, or an API we never verified) fails the test.
"""
import contextlib
import os


class P3(tuple):
    def __new__(cls, x, y, z):
        return super().__new__(cls, (float(x), float(y), float(z)))


class M3:
    def __init__(self, r1, r2, r3, r4):
        self.rows = (tuple(r1), tuple(r2), tuple(r3))
        self.pos = tuple(r4)

    def xf(self, p):
        return tuple(sum(p[k] * self.rows[k][j] for k in range(3)) + self.pos[j] for j in range(3))

    def inverse(self):
        """The inverse transform (test helper; general 3x3, not only rotations)."""
        (a, b, c), (d, e, f), (g, h, i) = self.rows
        det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
        assert abs(det) > 1e-12, "singular matrix3"
        inv = ((( e * i - f * h) / det, -(b * i - c * h) / det, (b * f - c * e) / det),
               (-(d * i - f * g) / det, (a * i - c * g) / det, -(a * f - c * d) / det),
               (( d * h - e * g) / det, -(a * h - b * g) / det, (a * e - b * d) / det))
        pos = tuple(-sum(self.pos[k] * inv[k][j] for k in range(3)) for j in range(3))
        return M3(*inv, pos)


class Obj:
    """Scene node / material / modifier: free attributes, plus a kind and a log."""
    _rt = None

    def __init__(self, kind, **kw):
        object.__setattr__(self, "kind", kind)
        object.__setattr__(self, "keys", [])
        self.name = kw.pop("name", kind)
        self.transform = None
        self.parent = None
        for k, v in kw.items():
            setattr(self, k, v)

    def __setattr__(self, k, v):
        if k == "transform" and v is not None and Obj._rt is not None and Obj._rt.animating():
            self.keys.append((Obj._rt.time, v))
        object.__setattr__(self, k, v)

    def __repr__(self):
        return f"<{self.kind} {self.name}>"


class _Meshop:
    def __init__(self, rt):
        self.rt = rt

    def setMapSupport(self, node, ch, on):
        assert node.kind == "mesh" and ch in (-2, -1, 0, 1, 2) and on is True
        node.maps[ch] = {"verts": {}, "faces": {}, "n": 0}

    def setNumMapVerts(self, node, ch, n):
        node.maps[ch]["n"] = n

    def setMapVert(self, node, ch, i, p):
        m = node.maps[ch]
        assert 1 <= i <= m["n"], (ch, i, m["n"])
        assert isinstance(p, P3)
        m["verts"][i] = p

    def setMapFace(self, node, ch, f, p):
        m = node.maps[ch]
        assert 1 <= f <= len(node.faces)
        assert all(1 <= int(c) <= m["n"] for c in p)
        m["faces"][f] = p


class _Skinops:
    def addBone(self, sk, bone, update):
        assert sk.kind == "Skin" and bone.kind in ("bone", "dummy") and update in (0, 1)
        # Skin stores the bone's transform when it is added as that bone's bind (initial) pose, and the
        # mesh's transform as the mesh bind pose; deformation is relative to them (skin_world)
        assert bone.transform is not None and sk.node is not None and sk.node.transform is not None
        sk.bones.append(bone)
        sk.bind.append(bone.transform)
        sk.mesh_tm = sk.node.transform

    def ReplaceVertexWeights(self, sk, v, b, w):
        assert sk.node is not None and sk.kind == "Skin"
        assert 1 <= v <= len(sk.node.verts) and 1 <= b <= len(sk.bones) and w == 1.0
        sk.weights[v] = b


class _ModPanel:
    def __init__(self, rt):
        self.rt = rt

    def setCurrentObject(self, mod):
        assert self.rt.mode == "modify" and self.rt.selection and mod in self.rt.selection[0].modifiers
        self.rt.current_mod = mod


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm(a):
    return sum(c * c for c in a) ** 0.5


class _Bone(Obj):
    """A Bone node (BoneGeometry). ``length``, ``width`` and ``height`` are the object's float parameters:
    positive numbers (this mock refuses 0 or negative, which Max would clamp or draw degenerate)."""

    def __setattr__(self, k, v):
        if k in ("length", "width", "height"):
            assert isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0, f"bone {k} = {v!r}"
        super().__setattr__(k, v)


class _BoneSys:
    def createBone(self, a, b, z):
        """BoneSys.createBone <startPos> <endPos> <zAxis>: a bone at startPos whose X axis points at endPos,
        its Z axis the given zAxis made perpendicular to X (Y = Z x X), and length = distance(start, end).
        A zero-length bone or a zAxis parallel to X has no defined orientation: refused here."""
        assert isinstance(a, P3) and isinstance(b, P3) and isinstance(z, P3)
        d = _sub(b, a)
        ln = _norm(d)
        assert ln > 1e-6, "createBone: start and end are the same point"
        x = tuple(c / ln for c in d)
        y = _cross(z, x)
        ny = _norm(y)
        assert ny > 1e-6 * max(_norm(z), 1e-30), "createBone: zAxis is parallel to the bone"
        y = tuple(c / ny for c in y)
        zz = _cross(x, y)
        n = _Bone("bone", start=a, end=b, width=4.0, height=4.0, length=ln)
        n.transform = M3(x, y, zz, a)
        return n


class MxsName:
    """A MAXScript name value (#centimeters) as pymxs returns it: not a Python str (no str methods), equal
    only to another name (case-insensitive, like MAXScript); str() gives the bare name, repr() '#name'."""

    def __init__(self, s):
        assert isinstance(s, str) and s and not s.startswith("#"), f"rt.Name({s!r})"
        self._s = s

    def __str__(self):
        return self._s

    def __repr__(self):
        return "#" + self._s

    def __eq__(self, o):
        return isinstance(o, MxsName) and self._s.lower() == o._s.lower()

    def __hash__(self):
        return hash(self._s.lower())


SYSTEM_TYPES = ("inches", "feet", "miles", "millimeters", "centimeters", "meters", "kilometers")


class _Units:
    """The ``units`` struct's System Unit Setup: 1 system unit = SystemScale SystemType. SystemType is a name
    value, one of SYSTEM_TYPES (3ds Max returns them lower case); SystemScale a positive float. 3ds Max's
    default (US install) is 1 inch. Any other property raises (unverified API)."""

    _FIELDS = {"systemtype": "SystemType", "systemscale": "SystemScale"}

    def __init__(self):
        object.__setattr__(self, "_type", "inches")
        object.__setattr__(self, "_scale", 1.0)

    def __getattr__(self, k):
        f = _Units._FIELDS.get(k.lower()) if not k.startswith("_") else None
        if f == "SystemType":
            return MxsName(self._type)
        if f == "SystemScale":
            return self._scale
        raise AttributeError(f"mock pymxs: units.{k} is not implemented (unverified API)")

    def __setattr__(self, k, v):
        f = _Units._FIELDS.get(k.lower())
        if f == "SystemType":
            assert isinstance(v, MxsName) and str(v).lower() in SYSTEM_TYPES, f"units.SystemType = {v!r}"
            object.__setattr__(self, "_type", str(v).lower())
        elif f == "SystemScale":
            assert isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0, f"units.SystemScale = {v!r}"
            object.__setattr__(self, "_scale", float(v))
        else:
            raise AttributeError(f"mock pymxs: units.{k} is not implemented (unverified API)")

    def set(self, system_type, system_scale):
        """Test helper (not MAXScript): the System Unit Setup dialog."""
        self.SystemType = MxsName(system_type)
        self.SystemScale = system_scale


class Runtime:
    def __init__(self):
        self.units = _Units()
        self.meshop = _Meshop(self)
        self.skinOps = _Skinops()
        self.modPanel = _ModPanel(self)
        self.BoneSys = _BoneSys()
        self.nodes, self.materials, self.calls = [], [], []
        self.mode, self.selection, self.current_mod = "create", [], None
        self.time, self._animate = None, False
        self.undo_on, self.redraw_on = True, True       # MAXScript 'with undo off' / 'with redraw off' (pymxs.undo/redraw)
        Obj._rt = self

    def animating(self):
        return self._animate and self.time is not None

    # -- values
    def Name(self, s):
        return MxsName(s)

    def Point3(self, x, y, z):
        return P3(x, y, z)

    def Color(self, r, g, b):
        assert all(0 <= c <= 255 for c in (r, g, b))
        return ("color", r, g, b)

    def Matrix3(self, a, b, c, d):
        return M3(a, b, c, d)

    # -- scene
    def mesh(self, vertices, faces, materialIDs):
        assert len(materialIDs) == len(faces)
        assert all(1 <= int(i) <= len(vertices) for f in faces for i in f), "face index out of range"
        n = Obj("mesh", verts=list(vertices), faces=list(faces), matids=list(materialIDs), maps={},
                smooth={}, modifiers=[], material=None)
        self.nodes.append(n)
        return n

    def instance(self, node):
        """MAXScript instance: a new node sharing the source's mesh (and material)."""
        assert node.kind == "mesh"
        # an instance shares the source's whole object reference, modifier stack included (an Edit_Normals
        # added to the master shows on every instance)
        n = Obj("mesh", verts=node.verts, faces=node.faces, matids=node.matids, maps=node.maps, smooth=node.smooth,
                modifiers=node.modifiers, material=node.material, instance_of=getattr(node, "instance_of", None) or node)
        for m in node.modifiers:
            if isinstance(m, _EditNormals):
                m.nodes.append(n)
        self.nodes.append(n)
        return n

    def Dummy(self, name, boxsize):
        n = Obj("dummy", name=name, boxsize=boxsize)
        self.nodes.append(n)
        return n

    def setFaceSmoothGroup(self, node, f, g):
        assert 1 <= f <= len(node.faces) and g == 1
        node.smooth[f] = g

    def update(self, node):
        self.calls.append(("update", node))

    def setUserProp(self, node, key, val):
        object.__setattr__(node, "userprops", {**getattr(node, "userprops", {}), key: val})

    # -- materials
    def MultiMaterial(self, numsubs):
        subs = [None] * numsubs
        m = Obj("multi", subs=subs, materialList=subs)      # MAXScript materialList (1-based) is the same list
        self.materials.append(m)
        return m

    def setSubMtl(self, mm, i, m):
        assert mm.kind == "multi" and 1 <= i <= len(mm.subs)
        mm.subs[i - 1] = m

    def StandardMaterial(self, name):
        return Obj("standard", name=name, diffuse=None, diffuseMap=None, opacityMap=None, shown=False)

    def Bitmaptexture(self, filename):
        assert os.path.isfile(filename), filename
        return Obj("bitmap", filename=filename, monoOutput=0)

    def showTextureMap(self, m, on):
        assert m.diffuseMap is not None
        m.shown = on

    # -- modifiers / UI state
    def Skin(self):
        return Obj("Skin", bones=[], weights={}, node=None, bind=[], mesh_tm=None)

    def Edit_Normals(self):
        return _EditNormals(self)

    def addModifier(self, node, mod):
        assert node.kind == "mesh"
        if isinstance(mod, _EditNormals):
            assert not mod.nodes, "this mock adds a modifier to one node (instances share it)"
            mod.nodes.append(node)
        node.modifiers.append(mod)
        mod.node = node

    def select(self, node):
        self.selection = [node]

    def execute(self, s):
        assert s == "max modify mode", s
        self.mode = "modify"

    def classOf(self, node):
        return node.kind

    def __getattr__(self, name):
        raise AttributeError(f"mock pymxs: runtime.{name} is not implemented (unverified API)")


class _EditNormals(Obj):
    """The Edit_Normals modifier's MAXScript interface (EditNormals methods), as 3ds Max documents it:

    * the methods work only while the modifier is the current object of the Modify panel (the node
      selected, ``max modify mode``, ``modPanel.setCurrentObject``); otherwise they fail. This is checked
      on every call, so selecting another node in between breaks the calls, as in Max;
    * when the modifier is shared by several nodes (instances), every call needs ``node:``;
    * face, corner and normal ids are 1-based integers and range-checked; faces are triangles (corners 1..3);
    * normals are not per face corner: the modifier builds one normal per vertex and smoothing group
      (corners of one vertex in one smoothing group share an id; smoothing group 0 gives each face its own
      normal). Max's id order is not documented, so this mock numbers them in an order unrelated to the
      vertex order: an importer must ask GetNormalID, never compute an id;
    * a normal that is not explicit is recomputed from the smoothing groups when the modifier evaluates,
      so only explicit normals keep a value set with SetNormal (``result()``).
    """

    def __init__(self, rt=None):
        super().__init__("Edit_Normals", node=None)
        object.__setattr__(self, "rt", rt)
        object.__setattr__(self, "nodes", [])
        object.__setattr__(self, "topo", None)          # (ids per [face][corner], {id: vertex}, count)
        object.__setattr__(self, "normals", {})         # id -> P3 set with SetNormal
        object.__setattr__(self, "explicit", set())     # ids made explicit
        object.__setattr__(self, "calls", 0)
        object.__setattr__(self, "calls_with_undo", 0)  # calls made while undo was on (slow in Max)
        object.__setattr__(self, "calls_with_redraw", 0)

    # -- state checks shared by every method
    def _ready(self, node):
        rt = self.rt
        assert rt is not None and self.nodes, "Edit_Normals: the modifier is not on a node"
        assert rt.mode == "modify" and rt.current_mod is self and rt.selection, \
            "Edit_Normals methods need the modifier to be the current object in the Modify panel"
        assert rt.selection[0] in self.nodes, "Edit_Normals: the selected node does not have this modifier"
        if node is None:
            assert len(self.nodes) == 1, "Edit_Normals is instanced on several nodes: pass node:"
        else:
            assert node in self.nodes and node is rt.selection[0]
        self.calls += 1
        self.calls_with_undo += rt.undo_on
        self.calls_with_redraw += rt.redraw_on
        if self.topo is None:
            self._build(self.nodes[0])
        return self.topo

    def _build(self, mesh):
        keys, rows = {}, []
        for f, face in enumerate(mesh.faces, 1):
            g = mesh.smooth.get(f, 0)
            row = []
            for v in face:
                v = int(v)
                key = ("f", f) if g == 0 else ("v", v, g)
                if key not in keys:
                    keys[key] = (len(keys), v)
                row.append(keys[key][0])
            rows.append(row)
        n = len(keys)
        perm = list(range(n))[::-1]                     # ids in an order unrelated to the vertex order
        vert = {perm[k] + 1: v for k, v in keys.values()}
        object.__setattr__(self, "topo", ([[perm[k] + 1 for k in row] for row in rows], vert, n))

    @staticmethod
    def _int(i):
        assert isinstance(i, int) and not isinstance(i, bool), f"Edit_Normals: integer id expected, got {i!r}"
        return i

    def _face_corner(self, topo, f, c):
        assert 1 <= self._int(f) <= len(topo[0]), f"face {f} out of range"
        assert 1 <= self._int(c) <= 3, f"corner {c} out of range (triangles)"

    def _nid(self, topo, nid):
        assert 1 <= self._int(nid) <= topo[2], f"normal id {nid} out of range 1..{topo[2]}"

    # -- the methods
    def GetNumFaces(self, node=None):
        return len(self._ready(node)[0])

    def GetNumNormals(self, node=None):
        return self._ready(node)[2]

    def GetNormalID(self, face, corner, node=None):
        topo = self._ready(node)
        self._face_corner(topo, face, corner)
        return topo[0][face - 1][corner - 1]

    def GetVertexID(self, face, corner, node=None):
        topo = self._ready(node)
        self._face_corner(topo, face, corner)
        return int(self.nodes[0].faces[face - 1][corner - 1])

    def SetNormal(self, nid, normal, node=None):
        topo = self._ready(node)
        self._nid(topo, nid)
        assert isinstance(normal, P3), "SetNormal needs a Point3"
        self.normals[nid] = normal

    def SetNormalExplicit(self, nid, explicit, node=None):
        topo = self._ready(node)
        self._nid(topo, nid)
        assert isinstance(explicit, bool)
        (self.explicit.add if explicit else self.explicit.discard)(nid)

    # -- test helpers (not MAXScript)
    def result(self):
        """{vertex (1-based): explicit normal} after the modifier evaluates. An explicit id that was never
        given a value keeps the computed one (None here)."""
        if self.topo is None:
            return {}
        return {self.topo[1][nid]: self.normals.get(nid) for nid in self.explicit}


class PymxsModule:
    """Stand-in for the ``pymxs`` module: ``animate`` / ``attime`` / ``undo`` / ``redraw`` context managers."""

    def __init__(self, rt):
        self.rt = rt

    @contextlib.contextmanager
    def animate(self, on):
        old, self.rt._animate = self.rt._animate, on
        try:
            yield
        finally:
            self.rt._animate = old

    @contextlib.contextmanager
    def undo(self, state, label=None):
        old, self.rt.undo_on = self.rt.undo_on, bool(state)
        try:
            yield
        finally:
            self.rt.undo_on = old

    @contextlib.contextmanager
    def redraw(self, state):
        old, self.rt.redraw_on = self.rt.redraw_on, bool(state)
        try:
            yield
        finally:
            self.rt.redraw_on = old

    @contextlib.contextmanager
    def attime(self, t):
        old, self.rt.time = self.rt.time, t
        try:
            yield
        finally:
            self.rt.time = old


# ----------------------------------------------------------------------------- test helpers (not MAXScript)

def transform_at(node, t=None):
    """A node's transform at Max frame ``t``: its key at that time (the last one set), or its transform
    when ``t`` is None or it has no key there."""
    if t is not None:
        for kt, m in reversed(node.keys):
            if kt == t:
                return m
    return node.transform


def skin_world(sk, t=None):
    """World position of every vertex of a Skin modifier's mesh (rigid weights) at frame ``t``, the way
    Skin deforms: v * meshBind * inverse(boneBind) * bone(t). {vertex (1-based): (x, y, z)}"""
    node = sk.node
    out = {}
    for v, b in sk.weights.items():
        w = sk.mesh_tm.xf(node.verts[v - 1])
        local = sk.bind[b - 1].inverse().xf(w)
        out[v] = transform_at(sk.bones[b - 1], t).xf(local)
    return out

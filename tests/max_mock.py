"""A recording mock of 3ds Max's pymxs for the importer tests (3ds Max is not installed here).

It implements exactly the MAXScript functions the importer calls, with MAXScript's rules checked:
1-based indices, ranges (face/vertex/map ids), files that must exist for bitmaps, skin bone ids, and
keyframes recorded under ``pymxs.animate``/``pymxs.attime``. Any other attribute raises, so a call
the mock does not know (a typo, or an API we never verified) fails the test.
"""
import contextlib
import os
import re
import struct


class P3(tuple):
    def __new__(cls, x, y, z):
        return super().__new__(cls, (float(x), float(y), float(z)))

    # pymxs Point3 components
    x = property(lambda self: self[0])
    y = property(lambda self: self[1])
    z = property(lambda self: self[2])


class M3:
    def __init__(self, r1, r2, r3, r4):
        self.rows = (tuple(r1), tuple(r2), tuple(r3))
        self.pos = tuple(r4)

    def xf(self, p):
        return tuple(sum(p[k] * self.rows[k][j] for k in range(3)) + self.pos[j] for j in range(3))

    # pymxs Matrix3 rows (Point3 values)
    row1 = property(lambda self: P3(*self.rows[0]))
    row2 = property(lambda self: P3(*self.rows[1]))
    row3 = property(lambda self: P3(*self.rows[2]))
    row4 = property(lambda self: P3(*self.pos))

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
    """Scene node / material / modifier: free attributes, plus a kind and a log.

    Keys: setting ``transform`` under ``animate on`` + ``at time t`` records the key (t, matrix). As in 3ds Max,
    the first key set on a node at a time other than 0 also creates a key at frame 0 holding the transform it
    had before (Auto Key keeps the original value at frame 0)."""
    _rt = None

    def __init__(self, kind, **kw):
        object.__setattr__(self, "kind", kind)
        object.__setattr__(self, "keys", [])
        object.__setattr__(self, "_scene", Obj._rt)     # the runtime (scene) the node was made in
        self.name = kw.pop("name", kind)
        self.transform = None
        self.parent = None
        for k, v in kw.items():
            setattr(self, k, v)

    def __setattr__(self, k, v):
        rt = self.__dict__.get("_scene")
        if k == "transform" and v is not None and rt is not None and rt.animating():
            t = rt.time
            if not self.keys and t != 0 and self.__dict__.get("transform") is not None:
                self.keys.append((0, self.__dict__["transform"]))
            self.keys.append((t, v))
        object.__setattr__(self, k, v)

    @property
    def controller(self):
        """The node's transform controller (PRS), see _Controller. Scene nodes only."""
        assert self.kind in ("mesh", "dummy", "bone"), f"{self.kind} has no transform controller"
        return _Controller.prs(self)

    def __repr__(self):
        return f"<{self.kind} {self.name}>"


class MxsTime:
    """A MAXScript Time value: ``.frame`` (float frames), ``.ticks``; ``as integer`` gives ticks (160 per frame at
    30 fps), so code must use ``.frame``. Compares with Time values only."""
    TICKS_PER_FRAME = 160

    def __init__(self, frame):
        assert isinstance(frame, (int, float)) and not isinstance(frame, bool), f"time {frame!r}"
        self._ticks = int(round(frame * self.TICKS_PER_FRAME))

    @property
    def frame(self):
        return self._ticks / self.TICKS_PER_FRAME

    @property
    def ticks(self):
        return self._ticks

    def __int__(self):
        return self._ticks

    def __eq__(self, o):
        return isinstance(o, MxsTime) and o._ticks == self._ticks

    def __hash__(self):
        return hash(self._ticks)

    def __repr__(self):
        return f"{self.frame:g}f"


class Interval:
    """MAXScript ``interval start end`` (frames). ``animationRange`` must have start < end (3ds Max refuses an
    empty or reversed range)."""

    def __init__(self, a, b):
        self.start, self.end = MxsTime(a), MxsTime(b)

    def __repr__(self):
        return f"(interval {self.start!r} {self.end!r})"


class _SubAnim:
    def __init__(self, name, controller):
        self.name, self.controller = name, controller


class _Controller:
    """A node's transform controllers as 3ds Max builds them by default: PRS > Position_XYZ (X/Y/Z
    Bezier_Float), Euler_XYZ (X/Y/Z Bezier_Float), Bezier_Scale. Setting ``node.transform`` under animate keys
    every leaf at that time (the mock's node.keys). Keys live on the leaves: ``numKeys`` of a compound
    controller (PRS, Position_XYZ, Euler_XYZ) is -1 here, so code must walk the sub-anims (``c[i].controller``,
    ``c.numSubs``) to find them. ``getKeyTime c i`` is 1-based, keys sorted by time."""

    def __init__(self, node, cls, subs):
        self.node, self.cls, self._subs = node, cls, subs

    @staticmethod
    def prs(node):
        def xyz(cls):
            return _Controller(node, cls, [(a, _Controller(node, "Bezier_Float", None)) for a in "XYZ"])
        return _Controller(node, "prs", [("Position", xyz("Position_XYZ")), ("Rotation", xyz("Euler_XYZ")),
                                         ("Scale", _Controller(node, "Bezier_Scale", None))])

    @property
    def numSubs(self):
        return len(self._subs or [])

    def subanim(self, i):
        assert isinstance(i, int) and not isinstance(i, bool) and 1 <= i <= self.numSubs, \
            f"{self.cls}[{i!r}]: sub-anim index out of range"
        name, c = self._subs[i - 1]
        return _SubAnim(name, c)

    def _times(self):
        return sorted({t for t, _ in self.node.keys})

    def numKeys(self):
        return -1 if self._subs is not None else len(self._times())

    def getKeyTime(self, i):
        assert self._subs is None, f"getKeyTime on {self.cls}: a compound controller has no keys of its own"
        times = self._times()
        assert isinstance(i, int) and 1 <= i <= len(times), f"getKeyTime: key {i!r} out of range 1..{len(times)}"
        return MxsTime(times[i - 1])

    def __repr__(self):
        return f"<{self.cls} of {self.node.name}>"


def _user_prop_value(s):
    """getUserProp's value: the stored text read as a MAXScript number when it is one (a Float is 32-bit, so
    "39.37007874015748" comes back rounded), else the string. (Booleans, point3s etc. are not modelled.)"""
    if re.fullmatch(r"-?[0-9]+", s):
        return int(s)
    if re.fullmatch(r"-?([0-9]+\.[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?|-?[0-9]+[eE][-+]?[0-9]+", s):
        return struct.unpack("<f", struct.pack("<f", float(s)))[0]
    return s


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
    def __init__(self, rt=None):
        self.rt = rt

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
        if self.rt is not None:
            self.rt._own(n)
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
        self.BoneSys = _BoneSys(self)
        self.animationRange = Interval(0, 100)          # 3ds Max's default
        self._mxs = None
        self.nodes, self.materials, self.calls = [], [], []
        self.mode, self.selection, self.current_mod = "create", [], None
        self.time, self._animate = None, False
        self.undo_on, self.redraw_on = True, True       # MAXScript 'with undo off' / 'with redraw off' (pymxs.undo/redraw)
        Obj._rt = self

    def _own(self, n):
        """A new scene node: in this scene's node list, keyed by this scene's animate / at time."""
        object.__setattr__(n, "_scene", self)
        self.nodes.append(n)
        return n

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
        self._own(n)
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
        self._own(n)
        return n

    def Dummy(self, name, boxsize):
        n = Obj("dummy", name=name, boxsize=boxsize)
        self._own(n)
        return n

    def setFaceSmoothGroup(self, node, f, g):
        assert 1 <= f <= len(node.faces) and g == 1
        node.smooth[f] = g

    def update(self, node):
        self.calls.append(("update", node))

    def setUserProp(self, node, key, val):
        """The value is stored as text (MAXScript writes ``key = value`` lines into the node's user property
        buffer). Strings and integers are modelled; a float's text is not (pass a formatted string)."""
        assert isinstance(key, str) and key and "=" not in key and "\n" not in key
        assert isinstance(val, (str, int)) and not isinstance(val, bool), f"setUserProp {key}: {val!r} not modelled"
        assert "\n" not in str(val) and "\r" not in str(val)
        object.__setattr__(node, "userprops", {**getattr(node, "userprops", {}), key: val})

    def getUserProp(self, node, key):
        """The value for key (case-insensitive), as _user_prop_value reads it, or None (undefined)."""
        for k, v in getattr(node, "userprops", {}).items():
            if k.lower() == key.lower():
                return _user_prop_value(str(v))
        return None

    def getUserPropBuffer(self, node):
        """The node's whole user property text: one ``key = value`` line per property, CR LF separated."""
        return "".join(f"{k} = {v}\r\n" for k, v in getattr(node, "userprops", {}).items())

    @property
    def objects(self):
        """Every node in the scene (the MAXScript ``objects`` set), in creation order."""
        return list(self.nodes)

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
        """``max modify mode``, or MAXScript source run by the MAXScript-subset interpreter on this scene (so its
        rules apply); returns the value of the last expression (e.g. a function the source defines)."""
        if s == "max modify mode":
            self.mode = "modify"
            return True
        assert not s.strip().lower().startswith("max "), f"{s!r} is not modelled"
        from maxscript_interp import Interp, Parser, tokenize
        if self._mxs is None:
            self._mxs = Interp(scene=self, stop_marker=None)
        v = None
        for e in Parser(tokenize(s)).program():
            v = self._mxs.eval(e)
        return v

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


# ----------------------------------------------------------------------------- Qt (PySide) for the Python dialog

class QtMock:
    """A strict stand-in for the parts of PySide6's QtCore / QtWidgets the Python importer's dialog uses.

    ``install(monkeypatch)`` puts it in ``sys.modules`` as PySide6 (and makes ``qtmax`` unavailable, as outside
    3ds Max). Only the methods listed exist (anything else raises AttributeError) and Qt's rules are kept:
    * signals call their slots with the signal's arguments, dropping the ones a Python slot does not take (as
      PySide does); ``blockSignals(True)`` silences a widget; a value set by code emits the change signal only
      when the value changes (``toggled``, ``valueChanged``, ``textChanged``, ``currentIndexChanged``,
      ``currentChanged``), user actions (``click``, ``type_text``, ``choose``, ``click_tab``) need an enabled
      widget;
    * a widget goes into one layout, a widget has one layout;
    * QSpinBox is 0..99 by default and takes integers only; QDoubleSpinBox is 0..99.99 with 2 decimals by default
      and rounds its value to its decimals; both clamp to their range;
    * QComboBox / QTabWidget: the first item / tab becomes current (and emits); ``setCurrentIndex`` with an
      index out of range is ignored (QTabWidget) or sets no item (-1, QComboBox);
    * QFileDialog's static functions return the test's next answer (``answers``; None = Cancel, which Qt
      returns as an empty path); QMessageBox's static functions record (kind, title, text) in ``messages``."""

    def __init__(self):
        import types
        mock = self
        self.answers, self.messages = [], []

        class Signal:
            def __init__(self, owner, n):
                self.owner, self.n, self.slots = owner, n, []

            def connect(self, slot):
                assert callable(slot)
                self.slots.append(slot)

            def emit(self, *args):
                assert len(args) == self.n
                if self.owner._blocked:
                    return
                import inspect
                for slot in self.slots:
                    try:
                        ps = [p for p in inspect.signature(slot).parameters.values()
                              if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD, p.VAR_POSITIONAL)]
                    except (TypeError, ValueError):
                        ps = []
                    k = len(args) if any(p.kind == p.VAR_POSITIONAL for p in ps) else min(len(args), len(ps))
                    slot(*args[:k])

        class QEvent:
            ActivationChange = 99

            def __init__(self, t):
                self._t = t

            def type(self):
                return self._t

        class QObject:
            def __init__(self):
                self._blocked = False

            def blockSignals(self, b):
                old, self._blocked = self._blocked, bool(b)
                return old

        class QWidget(QObject):
            def __init__(self, parent=None):
                super().__init__()
                assert parent is None or isinstance(parent, QWidget)
                self._parent, self._layout, self._in_layout, self._enabled, self._shown = parent, None, None, True, False

            def setEnabled(self, b):
                assert isinstance(b, bool)
                self._enabled = b

            def isEnabled(self):
                return self._enabled

            def show(self):
                self._shown = True

            def _user(self):
                assert self._enabled, f"{self!r} is disabled: the user cannot use it"

        class _Layout:
            def __init__(self, parent=None):
                self._items = []
                self._in_layout = None
                if parent is not None:
                    assert isinstance(parent, QWidget) and parent._layout is None, "the widget already has a layout"
                    parent._layout = self

            def _take(self, w):
                assert isinstance(w, (QWidget, _Layout))
                assert w._in_layout is None, f"{w!r} is already in a layout"
                w._in_layout = self
                self._items.append(w)

            def addWidget(self, w):
                assert isinstance(w, QWidget)
                self._take(w)

            def addLayout(self, lay):
                assert isinstance(lay, _Layout)
                self._take(lay)

            def addStretch(self, n=0):
                self._items.append(("stretch", n))

        class QVBoxLayout(_Layout):
            pass

        class QFormLayout(_Layout):
            def addRow(self, *a):
                if len(a) == 1:
                    self._take(a[0])
                else:
                    label, w = a
                    assert isinstance(label, str)
                    self._take(w)

        class QDialog(QWidget):
            def __init__(self, parent=None):
                super().__init__(parent)
                self._title, self._active = "", False

            def setWindowTitle(self, t):
                assert isinstance(t, str)
                self._title = t

            def windowTitle(self):
                return self._title

            def isActiveWindow(self):
                return self._active

            def changeEvent(self, event):
                assert isinstance(event, QEvent)

        class QLabel(QWidget):
            def __init__(self, text="", parent=None):
                super().__init__(parent)
                assert isinstance(text, str)
                self._text = text

            def setText(self, t):
                assert isinstance(t, str), f"QLabel.setText({t!r})"
                self._text = t

            def text(self):
                return self._text

            def setWordWrap(self, b):
                assert isinstance(b, bool)

        class QPushButton(QWidget):
            def __init__(self, text="", parent=None):
                super().__init__(parent)
                self._text = text
                self.clicked = Signal(self, 1)

            def text(self):
                return self._text

            def click(self):
                """The user clicks the button."""
                self._user()
                self.clicked.emit(False)

        class QCheckBox(QWidget):
            def __init__(self, text="", parent=None):
                super().__init__(parent)
                self._text, self._checked = text, False
                self.toggled = Signal(self, 1)
                self.clicked = Signal(self, 1)

            def text(self):
                return self._text

            def setChecked(self, b):
                assert isinstance(b, bool)
                if b != self._checked:
                    self._checked = b
                    self.toggled.emit(b)

            def isChecked(self):
                return self._checked

            def click(self):
                """The user clicks the checkbox."""
                self._user()
                self.setChecked(not self._checked)
                self.clicked.emit(self._checked)

        class QSpinBox(QWidget):
            def __init__(self, parent=None):
                super().__init__(parent)
                self._min, self._max, self._value = 0, 99, 0
                self.valueChanged = Signal(self, 1)

            def _check(self, v):
                assert isinstance(v, int) and not isinstance(v, bool), f"QSpinBox needs an int, got {v!r}"
                return v

            def setRange(self, a, b):
                self._min, self._max = self._check(a), self._check(b)
                self.setValue(self._value)

            def setValue(self, v):
                v = min(max(self._check(v), self._min), self._max)
                if v != self._value:
                    self._value = v
                    self.valueChanged.emit(v)

            def value(self):
                return self._value

            def enter(self, v):
                """The user types v."""
                self._user()
                self.setValue(v)

        class QDoubleSpinBox(QSpinBox):
            def __init__(self, parent=None):
                super().__init__(parent)
                self._min, self._max, self._value, self._decimals = 0.0, 99.99, 0.0, 2

            def _check(self, v):
                assert isinstance(v, (int, float)) and not isinstance(v, bool), f"QDoubleSpinBox: {v!r}"
                return round(float(v), self._decimals)

            def setDecimals(self, n):
                assert isinstance(n, int)
                self._decimals = n
                self.setValue(self._value)

        class QLineEdit(QWidget):
            def __init__(self, text="", parent=None):
                super().__init__(parent)
                self._text = text
                self.textChanged = Signal(self, 1)

            def setText(self, t):
                assert isinstance(t, str), f"QLineEdit.setText({t!r})"
                if t != self._text:
                    self._text = t
                    self.textChanged.emit(t)

            def text(self):
                return self._text

            def setPlaceholderText(self, t):
                assert isinstance(t, str)

            def type_text(self, t):
                """The user replaces the text with t."""
                self._user()
                self.setText(t)

        class QComboBox(QWidget):
            def __init__(self, parent=None):
                super().__init__(parent)
                self._items, self._index = [], -1
                self.currentIndexChanged = Signal(self, 1)

            def _set(self, i):
                if i != self._index:
                    self._index = i
                    self.currentIndexChanged.emit(i)

            def addItem(self, t):
                assert isinstance(t, str)
                self._items.append(t)
                if self._index < 0:
                    self._set(0)

            def addItems(self, ts):
                for t in ts:
                    self.addItem(t)

            def clear(self):
                self._items = []
                self._set(-1)

            def count(self):
                return len(self._items)

            def itemText(self, i):
                return self._items[i] if 0 <= i < len(self._items) else ""

            def currentIndex(self):
                return self._index

            def currentText(self):
                return self.itemText(self._index)

            def setCurrentIndex(self, i):
                assert isinstance(i, int)
                self._set(i if 0 <= i < len(self._items) else -1)

            def choose(self, i):
                """The user picks item i (0-based)."""
                self._user()
                assert 0 <= i < len(self._items)
                self._set(i)

        class QGroupBox(QWidget):
            def __init__(self, title="", parent=None):
                super().__init__(parent)
                self._title = title

            def title(self):
                return self._title

        class QTabWidget(QWidget):
            def __init__(self, parent=None):
                super().__init__(parent)
                self._tabs, self._index = [], -1
                self.currentChanged = Signal(self, 1)

            def addTab(self, w, label):
                assert isinstance(w, QWidget) and isinstance(label, str) and w._in_layout is None
                assert all(w is not t[0] for t in self._tabs), "the page is already a tab"
                self._tabs.append((w, label))
                if self._index < 0:
                    self._index = 0
                    self.currentChanged.emit(0)
                return len(self._tabs) - 1

            def count(self):
                return len(self._tabs)

            def tabText(self, i):
                return self._tabs[i][1] if 0 <= i < len(self._tabs) else ""

            def widget(self, i):
                return self._tabs[i][0] if 0 <= i < len(self._tabs) else None

            def currentIndex(self):
                return self._index

            def currentWidget(self):
                return self.widget(self._index)

            def setCurrentIndex(self, i):
                assert isinstance(i, int)
                if 0 <= i < len(self._tabs) and i != self._index:
                    self._index = i
                    self.currentChanged.emit(i)

            def click_tab(self, i):
                """The user clicks tab i."""
                self._user()
                self.setCurrentIndex(i)

        def _answer(kind, caption):
            assert mock.answers, f"{kind} {caption!r} opened, but the test gave no answer"
            a = mock.answers.pop(0)
            return "" if a is None else str(a)

        class QFileDialog:
            @staticmethod
            def getOpenFileName(parent=None, caption="", dir="", filter=""):
                return _answer("getOpenFileName", caption), ""

            @staticmethod
            def getExistingDirectory(parent=None, caption="", dir=""):
                return _answer("getExistingDirectory", caption)

        class QMessageBox:
            Ok = 0x400

            @staticmethod
            def _box(kind, parent, title, text):
                assert isinstance(title, str) and isinstance(text, str)
                mock.messages.append((kind, title, text))
                return QMessageBox.Ok

            @staticmethod
            def information(parent, title, text):
                return QMessageBox._box("information", parent, title, text)

            @staticmethod
            def warning(parent, title, text):
                return QMessageBox._box("warning", parent, title, text)

            @staticmethod
            def critical(parent, title, text):
                return QMessageBox._box("critical", parent, title, text)

        self.QtCore = types.ModuleType("PySide6.QtCore")
        self.QtCore.QEvent = QEvent
        self.QtWidgets = types.ModuleType("PySide6.QtWidgets")
        for c in (QWidget, QDialog, QVBoxLayout, QFormLayout, QLabel, QPushButton, QCheckBox, QSpinBox, QDoubleSpinBox,
                  QLineEdit, QComboBox, QGroupBox, QTabWidget, QFileDialog, QMessageBox):
            setattr(self.QtWidgets, c.__name__, c)
        self.package = types.ModuleType("PySide6")
        self.package.QtCore, self.package.QtWidgets = self.QtCore, self.QtWidgets

    def install(self, monkeypatch):
        monkeypatch.setitem(__import__("sys").modules, "PySide6", self.package)
        monkeypatch.setitem(__import__("sys").modules, "PySide6.QtCore", self.QtCore)
        monkeypatch.setitem(__import__("sys").modules, "PySide6.QtWidgets", self.QtWidgets)
        monkeypatch.setitem(__import__("sys").modules, "qtmax", None)      # outside 3ds Max: import fails
        return self

"""Dave Mirra Freestyle BMX importer for 3ds Max 2022+ (Python / pymxs). See ../README.md.

Run inside 3ds Max:   Scripting > Run Script... (pick this file)   or   python.ExecuteFile @"...\\dave_mirra_import.py"
It opens a small dialog (Qt). From the MAXScript listener you can also call:
    python.Execute "import dave_mirra_import as d; d.import_zmd(r'C:\\...\\DaveMirra1.ZMD')"

Parsing is done by dmparse.py (the reference parser, copied next to this file); this module only
builds the 3ds Max scene. Model data stays in game (RenderWare) space and one root Dummy carries the
axis change: game Y-up -> Max Z-up, plus the rider upright correction (../../../docs/formats/SPEC.md section 2.5).

Every call into 3ds Max goes through ``rt`` (pymxs.runtime). The test suite drives this module with a
recording mock of pymxs (tests/test_3dsmax_max_python.py); real-Max behaviour is checked by hand in 3ds Max.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
for _p in (os.path.join(HERE, "..", "parser"),):     # repo layout: tools/3dsmax/parser/dmparse.py
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)
import dmparse  # noqa: E402

try:
    import pymxs
    from pymxs import runtime as rt
except ImportError:            # outside 3ds Max (tests install a mock with set_runtime)
    pymxs = rt = None

VERSION = "1.3"

# Row-vector 3x3 matrices (3ds Max and RenderWare both use p' = p * M).
GAME_TO_MAX = ((1, 0, 0), (0, 0, 1), (0, -1, 0))     # (x, y, z) -> (x, -z, y): Y-up -> Z-up
UPRIGHT = {"rx-90": ((1, 0, 0), (0, 0, -1), (0, 1, 0)),  # (x, y, z) -> (x, z, -y): rider lies along +Z
           "rx+90": ((1, 0, 0), (0, 0, 1), (0, -1, 0))}


def set_runtime(runtime, module=None):
    """Use another pymxs runtime (tests)."""
    global rt, pymxs
    rt, pymxs = runtime, module


# ---------------------------------------------------------------------------- matrix helpers
# A transform is (rows, pos): rows = 3 row vectors (right, up, at), pos = translation; p' = p*rows + pos.

def _mul3(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def _xf(rows, p):
    return tuple(sum(p[k] * rows[k][j] for k in range(3)) for j in range(3))


def compose(local, parent):
    """world = local * parent (row-vector order)."""
    (lr, lp), (pr, pp) = local, parent
    return _mul3(lr, pr), tuple(a + b for a, b in zip(_xf(pr, lp), pp))


def frame_local(f, scale):
    return (tuple(f["right"]), tuple(f["up"]), tuple(f["at"])), tuple(c * scale for c in f["pos"])


def quat_rows(q):
    """Quaternion (x, y, z, w) -> row-vector rotation rows (transpose of the column matrix)."""
    x, y, z, w = q
    col = ((1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
           (2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
           (2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)))
    return tuple(tuple(col[j][i] for j in range(3)) for i in range(3))


def _vdot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _vcross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _vunit(a):
    n = _vdot(a, a) ** 0.5
    return tuple(c / n for c in a)


BONE_MODES = ("aim at children", "game frame orientation")   # dialog "Bones" choices (index 0 = default)
BONE_MIN = 0.02          # shortest bone, game units (metres): 2 cm
BONE_DEFAULT = 0.05      # a lone bone (no parent, no child) and the game-frame orientation mode
IDENTITY3 = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def bone_aim(frames, world):
    """Per frame: (O, length). O = constant rotation rows in the frame's own (game) space such that the
    node transform ``O * frameWorld`` has its local X pointing at the frame's main child; length in game
    units. ``frames``: dmparse LOD frames (parent before child); ``world``: their game world transforms
    (rows, pos) (any uniform scale). The joint positions are those of the frames (O has no translation).

    Main child: among the children not at the joint itself (> 1e-6 away), the one that best continues the
    bone's incoming direction (parent joint -> this joint; largest cosine). A bone whose parent is a root
    (or that has no incoming direction) takes the child with the most descendants instead (Hips -> the
    spine, not a leg). Ties: the first in file order. Length = the distance to that child.
    A leaf continues its parent's (bind-pose) direction, half the parent's length. Every length is at
    least BONE_MIN; a lone root keeps X and BONE_DEFAULT.
    O's Z axis is the frame's local Z made perpendicular to the aim (local Y when the aim is within ~25
    degrees of local Z); Y = Z x X."""
    n = len(frames)
    kids = [[] for _ in range(n)]
    for i, f in enumerate(frames):
        if 0 <= f["parent"] < i:
            kids[f["parent"]].append(i)
    desc = [0] * n
    for i in reversed(range(n)):
        for c in kids[i]:
            desc[i] += 1 + desc[c]
    out, aim_world = [], []
    for i, f in enumerate(frames):
        rows, pos = world[i]
        par = f["parent"] if 0 <= f["parent"] < i else -1
        valid = [c for c in kids[i] if _vdot(frames[c]["pos"], frames[c]["pos"]) ** 0.5 > 1e-6]
        if valid:
            inc = tuple(pos[k] - world[par][1][k] for k in range(3)) if par >= 0 else (0.0, 0.0, 0.0)
            best = valid[0]
            if par >= 0 and 0 <= frames[par]["parent"] < par and _vdot(inc, inc) > 1e-18:
                inc = _vunit(inc)
                score = None
                for c in valid:
                    d = _vunit(tuple(world[c][1][k] - pos[k] for k in range(3)))
                    s = _vdot(d, inc)
                    if score is None or s > score:
                        best, score = c, s
            else:
                for c in valid:
                    if desc[c] > desc[best]:
                        best = c
            p = frames[best]["pos"]
            length = _vdot(p, p) ** 0.5
            x = _vunit(p)
        elif par >= 0:
            dw = aim_world[par]                                    # parent's direction, world
            x = _vunit(tuple(_vdot(dw, rows[k]) for k in range(3)))   # into this frame (rows orthonormal)
            length = 0.5 * out[par][1]
        else:
            x, length = (1.0, 0.0, 0.0), BONE_DEFAULT
        length = max(length, BONE_MIN)
        hint = (0.0, 0.0, 1.0) if abs(x[2]) <= 0.9 else (0.0, 1.0, 0.0)
        z = _vunit(tuple(hint[k] - _vdot(hint, x) * x[k] for k in range(3)))
        y = _vcross(z, x)
        out.append(((x, y, z), length))
        aim_world.append(_vunit(tuple(sum(x[k] * rows[k][j] for k in range(3)) for j in range(3))))
    return out


def flip_normals_to_faces(positions, normals, triangles):
    """"Flip normals to faces": each vertex normal that points against its faces is negated. A vertex's
    faces vote with their area-weighted CCW normals ((b - a) x (c - a), summed over every triangle using
    the vertex): the normal flips when its dot product with that sum is negative. Vertices whose faces
    disagree follow the larger area; a zero sum (no face, or faces cancelling exactly) keeps the normal.
    Missing (None) normals stay None. Returns a new list."""
    acc = [[0.0, 0.0, 0.0] for _ in positions]
    for t in triangles:
        a, b, c = positions[t[0]], positions[t[1]], positions[t[2]]
        fn = _vcross(tuple(b[k] - a[k] for k in range(3)), tuple(c[k] - a[k] for k in range(3)))
        for v in t[:3]:
            s = acc[v]
            s[0] += fn[0]
            s[1] += fn[1]
            s[2] += fn[2]
    out = []
    for n, s in zip(normals, acc):
        if n is not None and _vdot(n, s) < 0.0:
            n = [-n[0], -n[1], -n[2]]
        out.append(n)
    return out


def to_max(xf):
    rows, pos = xf
    return rt.Matrix3(rt.Point3(*rows[0]), rt.Point3(*rows[1]), rt.Point3(*rows[2]), rt.Point3(*pos))


def root_transform(z, lod, upright, animated=False):
    """Root rows: [upright correction] * game->Max. Animated riders get no upright correction (the Hips
    channel stands the skeleton up by itself, ../../../docs/formats/SPEC.md section 8.4)."""
    corr = dmparse.upright_correction(z, lod) if (upright and not animated) else None
    rows = _mul3(UPRIGHT[corr], GAME_TO_MAX) if corr else GAME_TO_MAX
    return rows, (0.0, 0.0, 0.0)


# ---------------------------------------------------------------------------- safety

def inside_game_install(path):
    """True if ``path`` is inside a folder tree that contains NgBMX.exe (the game, _pristine, _testgame)."""
    p = os.path.abspath(path)
    while True:
        if os.path.isfile(os.path.join(p, "NgBMX.exe")):
            return True
        parent = os.path.dirname(p)
        if parent == p:
            return False
        p = parent


def texture_folder(src_path, texture_dir=None):
    """Where textures are written: the user's choice, else %TEMP%\\DaveMirraTextures\\<model>.
    Never inside the game install (it is read-only)."""
    d = texture_dir or os.path.join(tempfile.gettempdir(), "DaveMirraTextures",
                                    os.path.splitext(os.path.basename(src_path))[0])
    if inside_game_install(d):
        raise ValueError(f"texture folder {d} is inside the game install; choose another folder")
    os.makedirs(d, exist_ok=True)
    return d


def _safe(name):
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in name) or "tex"


def write_textures(textures, folder):
    """Write embedded TBMP/TPNG files; returns {TEXTURE NAME (upper): (path, has_alpha_png)}."""
    out = {}
    for t in textures:
        path = os.path.join(folder, f"{_safe(t['name'])}.{t['kind']}")
        with open(path, "wb") as f:
            f.write(t["data"])
        out[t["name"].upper()] = (path, t["kind"] == "png")
    return out


# ---------------------------------------------------------------------------- materials

def build_material(name, slots, texfiles, cache):
    """A Multimaterial with one Standard sub-material per RW material slot (face material ID = slot + 1)."""
    mm = rt.MultiMaterial(numsubs=len(slots))
    mm.name = name
    for i, s in enumerate(slots):
        key = (s["texture"] or "").upper(), tuple(s["color"])
        if key not in cache:
            r, g, b, a = s["color"]
            m = rt.StandardMaterial(name=s["texture"] or f"untextured_{i}")
            m.diffuse = rt.Color(r, g, b)
            if s["texture"] and s["texture"].upper() in texfiles:
                path, alpha = texfiles[s["texture"].upper()]
                m.diffuseMap = rt.Bitmaptexture(filename=path)
                if alpha:
                    op = rt.Bitmaptexture(filename=path)
                    op.monoOutput = 1                     # alpha channel as opacity
                    m.opacityMap = op
                rt.showTextureMap(m, True)
            cache[key] = m
        rt.setSubMtl(mm, i + 1, cache[key])
    return mm


# ---------------------------------------------------------------------------- meshes

def build_mesh(name, g, scale, explicit_normals=False, flip_normals=False):
    """Editable mesh from a dmparse geometry dict (vertices in the part's own space).
    ``flip_normals`` (with ``explicit_normals``): flip_normals_to_faces first."""
    verts = [rt.Point3(p[0] * scale, p[1] * scale, p[2] * scale) for p in g["positions"]]
    faces = [rt.Point3(t[0] + 1, t[1] + 1, t[2] + 1) for t in g["triangles"]]   # 1-based, CCW
    mat_ids = [t[3] + 1 for t in g["triangles"]]
    node = rt.mesh(vertices=verts, faces=faces, materialIDs=mat_ids)
    node.name = name
    nf = len(faces)
    for ch, data in ((1, g["uvs"][0] if g["uvs"] else None), (2, g["uvs"][1] if len(g["uvs"]) > 1 else None)):
        if data is None:
            continue
        rt.meshop.setMapSupport(node, ch, True)
        rt.meshop.setNumMapVerts(node, ch, len(data))
        for i, (u, v) in enumerate(data):
            rt.meshop.setMapVert(node, ch, i + 1, rt.Point3(u, 1.0 - v, 0.0))   # game UV origin is top-left
        for f in range(nf):
            rt.meshop.setMapFace(node, ch, f + 1, faces[f])
    if g["colors"]:
        for ch, pick in ((0, lambda c: rt.Point3(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0)),
                         (-2, lambda c: rt.Point3(c[3] / 255.0, c[3] / 255.0, c[3] / 255.0))):
            rt.meshop.setMapSupport(node, ch, True)
            rt.meshop.setNumMapVerts(node, ch, len(g["colors"]))
            for i, c in enumerate(g["colors"]):
                rt.meshop.setMapVert(node, ch, i + 1, pick(c))
            for f in range(nf):
                rt.meshop.setMapFace(node, ch, f + 1, faces[f])
    for f in range(nf):
        rt.setFaceSmoothGroup(node, f + 1, 1)                 # one smoothing group: smooth shading
    rt.update(node)
    if explicit_normals and g["normals"]:
        normals = g["normals"]
        if flip_normals:
            normals = flip_normals_to_faces(g["positions"], normals, g["triangles"])
        set_explicit_normals(node, normals, g["triangles"])
    return node


def unit_normal(n):
    """The game normal scaled to length 1, or None when it is missing or zero-length (level sectors store
    s8/127 normals, 0.996-1.019 long, and a few are (0, 0, 0))."""
    if n is None:
        return None
    x, y, z = n
    length = (x * x + y * y + z * z) ** 0.5
    return None if length < 1e-6 else (x / length, y / length, z / length)


def set_explicit_normals(node, normals, triangles):
    """Exact game normals through an Edit_Normals modifier (optional). ``normals``: one per vertex, None
    for none; ``triangles``: the mesh faces (0-based vertex ids) in face order.
    Edit_Normals keeps one normal per vertex and smoothing group, so each normal id is set once:
    GetNormalID per face corner, then SetNormal + SetNormalExplicit per new id. Vertices without a usable
    normal keep the smoothing-group normal. The methods need the modifier current in the Modify panel
    (one setCurrentObject per mesh); undo and redraw are off while the ids are set.
    Returns the modifier, or None when no vertex has a usable normal."""
    unit = [unit_normal(n) for n in normals]
    if not any(u is not None for u in unit):
        return None
    en = rt.Edit_Normals()
    rt.addModifier(node, en)
    rt.select(node)
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(en)
    done = set()
    with pymxs.undo(False), pymxs.redraw(False):
        for f, t in enumerate(triangles, 1):
            for c in range(3):
                u = unit[t[c]]
                if u is None:
                    continue
                nid = en.GetNormalID(f, c + 1)
                if nid in done:
                    continue
                done.add(nid)
                en.SetNormal(nid, rt.Point3(*u))
                en.SetNormalExplicit(nid, explicit=True)
    return en


# ---------------------------------------------------------------------------- import

def import_zmd(path, scale=100.0, lod=0, upright=True, texture_dir=None, explicit_normals=False,
               motion_path=None, motion_ids=(), bone_aim_children=True, flip_normals=False):
    """Import a .zmd. Returns a summary dict (also used by the tests).
    ``bone_aim_children``: rider bones point at their main child (bone_aim; default) instead of along
    their game frame's X axis. The joints, the skin and the motion keys are the same either way.
    ``flip_normals`` (with ``explicit_normals``): flip_normals_to_faces."""
    data = open(path, "rb").read()
    z = dmparse.read_zmd(data)
    if z["empty"]:
        raise ValueError(f"{os.path.basename(path)} is an empty (0-byte) model; nothing to import")
    if not 0 <= lod < len(z["lods"]):
        raise ValueError(f"LOD {lod} does not exist (the model has {len(z['lods'])})")
    folder = texture_folder(path, texture_dir)
    texfiles = write_textures(z["textures"], folder)
    clips = []
    if motion_ids:
        bank = dmparse.read_motion_bank(open(motion_path, "rb").read())
        clips = [(m, bank[m]) for m in motion_ids]
    model = (z["mdef"] or {}).get("name") or os.path.splitext(os.path.basename(path))[0]
    lo = z["lods"][lod]
    sk = dmparse.skeleton(z, lod)
    root_xf = root_transform(z, lod, upright, animated=bool(clips))
    root = rt.Dummy(name=model, boxsize=rt.Point3(*(0.1 * scale,) * 3))     # 10 cm helper icon
    root.transform = to_max(root_xf)

    world = []                                                # game-space world transform per frame
    for f in lo["frames"]:
        local = frame_local(f, scale)
        world.append(compose(local, world[f["parent"]]) if 0 <= f["parent"] < len(world) else local)
    skinned = any(a["geometry"]["bones"] for a in lo["atomics"])
    aim = bone_aim(lo["frames"], world) if (skinned and bone_aim_children) else None
    offsets = [a[0] for a in aim] if aim else [IDENTITY3] * len(sk)
    frame_nodes = []
    for i, s in enumerate(sk):
        wmax = compose(world[i], root_xf)
        if skinned and aim:
            o_rows, length = aim[i]
            wmax = compose((o_rows, (0.0, 0.0, 0.0)), wmax)            # O * frame world: X at the child
            p, xw, zw = wmax[1], wmax[0][0], wmax[0][2]
            node = rt.BoneSys.createBone(rt.Point3(*p), rt.Point3(*(p[k] + xw[k] * length * scale for k in range(3))),
                                         rt.Point3(*zw))
            node.width = node.height = 0.02 * scale
            node.length = length * scale
        elif skinned:
            p = wmax[1]
            node = rt.BoneSys.createBone(rt.Point3(*p), rt.Point3(p[0] + BONE_DEFAULT * scale, p[1], p[2]),
                                         rt.Point3(0, 0, 1))
            node.width = node.height = 0.02 * scale
            node.length = BONE_DEFAULT * scale
        else:
            node = rt.Dummy(name=s["name"], boxsize=rt.Point3(*(0.03 * scale,) * 3))
        node.name = s["name"]
        node.transform = to_max(wmax)
        node.parent = frame_nodes[s["parent"]] if 0 <= s["parent"] < i else root
        frame_nodes.append(node)

    summary = {"model": model, "root": root, "frames": frame_nodes, "meshes": [], "textures": texfiles,
               "texture_dir": folder, "skinned": skinned, "root_transform": root_xf, "world": world, "keys": 0,
               "bone_offsets": offsets, "bone_lengths": [a[1] for a in aim] if aim else None}
    cache = {}
    for ai, a in enumerate(lo["atomics"]):
        g = a["geometry"]
        node = build_mesh(f"{sk[a['frame']]['name']}_mesh{ai}", g, scale, explicit_normals, flip_normals)
        node.material = build_material(f"{model}_atomic{ai}", g["material_slots"], texfiles, cache)
        if g["bones"]:
            node.transform = to_max(root_xf)                  # model space under the root
            node.parent = root
            skin_mesh(node, frame_nodes, dmparse.vertex_frames(z, lod, ai))
        else:
            node.transform = to_max(compose(world[a["frame"]], root_xf))
            node.parent = frame_nodes[a["frame"]]
        summary["meshes"].append(node)
    for mid, clip in clips:
        summary["keys"] += key_motion(clip, lo["frames"], sk, frame_nodes, root_xf, scale, offsets)
    return summary


def skin_mesh(node, bones, vframes):
    """Skin modifier: every frame is a bone, each vertex 100% on its one bone (the game is rigid)."""
    sk = rt.Skin()
    rt.addModifier(node, sk)
    rt.select(node)
    rt.execute("max modify mode")
    rt.modPanel.setCurrentObject(sk)
    for i, b in enumerate(bones):
        rt.skinOps.addBone(sk, b, 1 if i == len(bones) - 1 else 0)
    rt.classOf(node)                                          # force the modifier to evaluate
    for v, f in enumerate(vframes):
        rt.skinOps.ReplaceVertexWeights(sk, v + 1, f + 1, 1.0)   # bone index = frame index + 1 (added in order)
    return sk


def key_motion(clip, frames, sk, frame_nodes, root_xf, scale, offsets=None):
    """Key one motion clip (ids from motion.zal) on the bones/dummies, one key per game frame.
    Channel frames get the game's quaternion with their bind translation; others keep their bind.
    ``offsets``: per node, the constant bone rotation O (bone_aim) put in front of every key."""
    names = {s["name"]: s["index"] for s in sk}
    chan = {}
    for si, (cnt, _f, kind) in enumerate(clip["sections"]):
        table = dmparse.RIDER_CHANNELS if kind == 0 else dmparse.BIKE_CHANNELS
        chan[kind == 0] = [names.get(table[c]) for c in range(min(cnt, len(table)))]
    n = 0
    for k, pose in enumerate(dmparse.sample_motion(clip)):
        local = [frame_local(f, scale) for f in frames]
        for is_rider, eulers in ((True, pose["rider"]), (False, pose["bike"])):
            if eulers is None:
                continue
            table = dmparse.RIDER_CHANNELS if is_rider else dmparse.BIKE_CHANNELS
            for c, fi in enumerate(chan.get(is_rider, [])):
                if fi is not None and c < len(eulers):
                    local[fi] = (quat_rows(dmparse.channel_rotation(table[c], eulers[c])), local[fi][1])
        world = []
        for i, f in enumerate(frames):
            world.append(compose(local[i], world[f["parent"]]) if 0 <= f["parent"] < i else local[i])
        with pymxs.animate(True):
            with pymxs.attime(k):
                for i, node in enumerate(frame_nodes):
                    xf = compose(world[i], root_xf)
                    if offsets is not None and offsets[i] != IDENTITY3:
                        xf = compose((offsets[i], (0.0, 0.0, 0.0)), xf)
                    node.transform = to_max(xf)
                    n += 1
    return n


def write_level_textures(textures, folder):
    """Level textures: BMPs that contain the game's colour key (RGB 255,0,255 = transparent) are
    written as PNG with that colour made transparent; everything else as-is.
    Returns {TEXTURE NAME (upper): (path, has_alpha)}."""
    out = {}
    for t in textures:
        keyed = dmparse.color_key_png(t["data"]) if t["kind"] == "bmp" else None
        ext = "png" if keyed is not None else t["kind"]
        path = os.path.join(folder, f"{_safe(t['name'])}.{ext}")
        with open(path, "wb") as f:
            f.write(keyed if keyed is not None else t["data"])
        out[t["name"].upper()] = (path, keyed is not None or t["kind"] == "png")
    return out


def merge_model(z):
    """All atomics of a model (frames baked, dmparse.model_meshes: normals rotated by the frame) as one
    geometry dict for build_mesh."""
    g = {"positions": [], "normals": [], "uvs": [[]], "colors": None, "triangles": [], "material_slots": []}
    has_uv = True
    for m in dmparse.model_meshes(z):
        base, sbase = len(g["positions"]), len(g["material_slots"])
        g["positions"] += m["positions"]
        g["normals"] += m["normals"] or [None] * len(m["positions"])        # None: no game normal
        if m["uvs"] is None:
            has_uv = False
        else:
            g["uvs"][0] += m["uvs"]
        g["triangles"] += [[a + base, b + base, c + base, sl + sbase] for a, b, c, sl in m["triangles"]]
        g["material_slots"] += m["slots"]
    if not has_uv:
        g["uvs"] = []
    return g


def _tag_color(code):
    """Display colour per tag code (same formula in the MAXScript importer: no 32-bit overflow)."""
    if code is None:
        return (150, 150, 150)
    return ((code * 67 + 40) & 255, (code * 151 + 90) & 255, (code * 211 + 150) & 255)


def import_level(folder, scale=100.0, texture_dir=None, centre=True, explicit_normals=False, flip_normals=False):
    """Import a level folder (the one holding the .ZSD), ../../../docs/formats/SPEC.md section 5:
    World (one mesh, Mat._sd textures with the colour key as opacity), Props (one mesh per PropDef,
    then instances placed and rotated), Tags (one hidden mesh per tag code), ScenePoints (dummies:
    Player, PlayerT, Globals, SpotLights) and the Panorama (sky model at the world origin).
    ``centre``: put the world origin (BSP invWorldOrigin) at the Max origin (presentation only).
    ``explicit_normals``: the game normals on World, the PropDef models and the Panorama (Edit_Normals;
    sectors without normals, and zero-length normals, keep the smoothing-group normals).
    ``flip_normals`` (with ``explicit_normals``): flip_normals_to_faces on each of those meshes.
    Props are rotated by their stored rows (dmparse.prop_matrix_to_rw, as the game renders them)."""
    lv = dmparse.read_level(folder)
    name = lv["name"]
    tfolder = texture_folder(os.path.join(folder, name), texture_dir)
    texfiles = write_level_textures(lv["textures"], tfolder)
    o_full = lv["origin"]
    o = o_full if centre else (0.0, 0.0, 0.0)
    root_xf = (GAME_TO_MAX, (0.0, 0.0, 0.0))

    def place(rows, rw_pos):
        return compose((tuple(tuple(r) for r in rows), tuple((rw_pos[i] - o[i]) * scale for i in range(3))), root_xf)

    def group(gname):
        d = rt.Dummy(name=gname, boxsize=rt.Point3(*(0.05 * scale,) * 3))
        d.transform = to_max(root_xf)
        d.parent = root
        return d

    root = rt.Dummy(name=name, boxsize=rt.Point3(*(0.1 * scale,) * 3))
    root.transform = to_max(root_xf)
    rt.setUserProp(root, "dmbmx_components", ";".join(f"{f['kind']}={f['name']}:{f['size']}" for f in lv["files"]))
    rt.setUserProp(root, "dmbmx_world_origin", " ".join(repr(c) for c in o_full))
    cache = {}
    identity = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))

    # World: all sectors in one mesh; triangle material = world material slot
    w = lv["world"]
    g = {"positions": [], "normals": [], "uvs": [[]], "colors": [], "triangles": [], "material_slots": w["slots"]}
    for sec in w["sectors"]:
        base = len(g["positions"])
        g["positions"] += [[v[i] - o[i] for i in range(3)] for v in sec["vertices"]]
        g["normals"] += sec["normals"] or [None] * len(sec["vertices"])        # RW world axes, as positions
        g["uvs"][0] += sec["uvs"] or [[0.0, 0.0]] * len(sec["vertices"])
        g["colors"] += sec["colors"] or [[255, 255, 255, 255]] * len(sec["vertices"])
        g["triangles"] += [[a + base, b + base, c + base, sl] for sl, a, b, c in sec["triangles"]]
    world = build_mesh("World", g, scale, explicit_normals, flip_normals)
    world.material = build_material(f"{name}_World", w["slots"], texfiles, cache)
    world.transform = to_max(root_xf)
    world.parent = root

    # Props: one mesh per definition (first instance), instances for the rest
    props_grp = group("Props")
    masters, prop_nodes = {}, []
    for k, (p, d, t, R) in enumerate(dmparse.prop_instances(lv)):
        if d is None:
            continue
        if d["id"] not in masters:
            mg = merge_model(d["model"])
            node = build_mesh(f"PropDef_{d['name']}", mg, scale, explicit_normals, flip_normals)   # instances share it
            node.material = build_material(f"{name}_{d['name']}", mg["material_slots"], texfiles, cache)
            masters[d["id"]] = node
        else:
            node = rt.instance(masters[d["id"]])
        node.name = f"{p['name']}_{k}"
        node.transform = to_max(place(R, t))
        node.parent = props_grp
        rt.setUserProp(node, "dmbmx_def_id", str(d["id"]))
        prop_nodes.append(node)

    # Tags: collision faces grouped by face-tag code, hidden
    tags_grp = group("Tags")
    by_code = {}
    for f in lv["tags"]["faces"]:
        by_code.setdefault(f["code"], []).append(f)
    tag_nodes = {}
    for code in sorted(by_code, key=lambda c: -1 if c is None else c):
        faces = by_code[code]
        tg = {"positions": [], "normals": None, "uvs": [], "colors": None, "triangles": [],
              "material_slots": [{"texture": None, "color": list(_tag_color(code)) + [255]}]}
        for f in faces:
            base = len(tg["positions"])
            for v in f["verts"]:
                rw = dmparse.zge_to_rw(v, o_full)
                tg["positions"].append([rw[i] - o[i] for i in range(3)])
            tg["triangles"].append([base, base + 1, base + 2, 0])
        cname = "none" if code is None else "%04x" % code
        node = build_mesh(f"tag_{cname}", tg, scale)
        node.material = build_material(f"{name}_tag_{cname}", tg["material_slots"], {}, cache)
        node.transform = to_max(root_xf)
        node.parent = tags_grp
        node.isHidden = True
        rt.setUserProp(node, "dmbmx_tag_code", "none" if code is None else str(code))
        tag_nodes[code] = node

    # Scene points
    pts_grp = group("ScenePoints")
    point_nodes = []
    for k, sp in enumerate(lv["scene"]["points"]):
        node = rt.Dummy(name=sp["name"], boxsize=rt.Point3(*(0.5 * scale,) * 3))
        node.transform = to_max(place(dmparse.matrix_to_rw(sp["matrix"]), dmparse.pos_to_rw(sp["pos"], o_full)))
        node.parent = pts_grp
        rt.setUserProp(node, "dmbmx_point_type", str(sp["type"]))
        point_nodes.append(node)

    # Panorama: the sky model, placed at the world origin
    pano = None
    if lv["scene"]["panorama"] is not None:
        pg = merge_model(lv["scene"]["panorama"])
        pano = build_mesh("Panorama", pg, scale, explicit_normals, flip_normals)
        pano.material = build_material(f"{name}_Panorama", pg["material_slots"], texfiles, cache)
        pano.transform = to_max(place(identity, o_full))
        pano.parent = root
    return {"root": root, "world": world, "props": prop_nodes, "masters": masters, "tags": tag_nodes,
            "points": point_nodes, "panorama": pano, "textures": texfiles, "texture_dir": tfolder,
            "files": lv["files"], "origin": o_full, "centre": o, "level": lv}


# ---------------------------------------------------------------------------- scene units (real-world size)
# Game units are metres. 3ds Max's System Unit Setup says 1 system unit = units.SystemScale units.SystemType
# (for example 5 #centimeters), so the import scale that keeps 1 game metre = 1 real metre is
# 1 / (metres per system unit). The MAXScript importer has the same functions (DM_unitInfo, DM_realWorldScale,
# DM_scaleLabel).

UNIT_METRES = {"inches": (0.0254, "in"), "feet": (0.3048, "ft"), "miles": (1609.344, "mi"),
               "millimeters": (0.001, "mm"), "centimeters": (0.01, "cm"), "meters": (1.0, "m"),
               "kilometers": (1000.0, "km")}     # units.SystemType -> (metres, abbreviation)


def scene_units():
    """The scene's System Unit Setup: (unit type name, e.g. "centimeters", units.SystemScale)."""
    t = str(rt.units.SystemType).lstrip("#").lower()    # a MAXScript name value (#centimeters)
    s = float(rt.units.SystemScale)
    if t not in UNIT_METRES:
        raise ValueError(f"unknown system unit type: {t}")
    if not s > 0.0:
        raise ValueError("the System Unit Setup scale must be positive")
    return t, s


def metres_per_system_unit():
    """Metres in one 3ds Max system unit (units.SystemScale x units.SystemType)."""
    t, s = scene_units()
    return s * UNIT_METRES[t][0]


def real_world_scale():
    """The import ``scale`` that gives real-world size in this scene (1 game metre = 1 m)."""
    return 1.0 / metres_per_system_unit()


def fmt_scale(v):
    """A scale for the label: 4 significant digits, whole numbers from 1000 up."""
    return "%.0f" % v if v >= 1000.0 else "%.4g" % v


def scale_label(manual_scale=None):
    """The dialog's units label. ``manual_scale``: None in real-world mode, else the manual multiplier."""
    t, s = scene_units()
    mpu = metres_per_system_unit()
    text = "Scene: 1 unit = %g %s" % (s, UNIT_METRES[t][1])
    if manual_scale is None:
        r = fmt_scale(1.0 / mpu)
        return f"{text} -> scale {r} (game 1 m = {r} units)"
    return f"{text} -> manual scale {fmt_scale(manual_scale)}: 1 game metre = {fmt_scale(manual_scale * mpu)} m in this scene"


# ---------------------------------------------------------------------------- UI (Qt)

def show_dialog():
    try:
        from PySide6 import QtCore, QtWidgets
    except ImportError:
        from PySide2 import QtCore, QtWidgets
    parent = None
    try:
        from qtmax import GetQMaxMainWindow
        parent = GetQMaxMainWindow()
    except ImportError:
        pass

    class Dialog(QtWidgets.QDialog):
        def __init__(self):
            super().__init__(parent)
            self.setWindowTitle(f"Dave Mirra Importer {VERSION}")
            lay = QtWidgets.QFormLayout(self)
            self.real = QtWidgets.QCheckBox("Real-world size (use scene units)")
            self.real.setChecked(True)
            self.scale = QtWidgets.QDoubleSpinBox()
            self.scale.setDecimals(6)
            self.scale.setRange(0.000001, 1e9)
            self.scale.setValue(100.0)
            self.units = QtWidgets.QLabel()
            self.units.setWordWrap(True)
            refresh = QtWidgets.QPushButton("Refresh scene units")
            refresh.clicked.connect(self.refresh_units)
            self.real.toggled.connect(self.refresh_units)
            self.scale.valueChanged.connect(self.refresh_units)
            self.lod = QtWidgets.QSpinBox()
            self.upright = QtWidgets.QCheckBox("Stand riders upright")
            self.upright.setChecked(True)
            self.normals = QtWidgets.QCheckBox("Exact normals (Edit_Normals, slower)")
            self.flip = QtWidgets.QCheckBox("Flip normals to faces (with Exact normals)")
            self.bones = QtWidgets.QComboBox()
            self.bones.addItems(BONE_MODES)
            self.texdir = QtWidgets.QLineEdit()
            self.texdir.setPlaceholderText("(temp folder)")
            self.motions = QtWidgets.QLineEdit()
            self.motions.setPlaceholderText("e.g. 1,8,15 (needs motion.zal)")
            for label, w in (("", self.real), ("Scale", self.scale), ("", self.units), ("", refresh),
                             ("LOD", self.lod), ("", self.upright), ("", self.normals), ("", self.flip), ("Bones", self.bones), ("Texture folder", self.texdir),
                             ("Motion ids", self.motions)):
                lay.addRow(label, w)
            b1 = QtWidgets.QPushButton("Import ZMD...")
            b2 = QtWidgets.QPushButton("Import Level (.ZSD)...")
            b1.clicked.connect(self.do_zmd)
            b2.clicked.connect(self.do_level)
            lay.addRow(b1)
            lay.addRow(b2)
            self.refresh_units()

        def refresh_units(self, *_):
            """Real-world mode: the spinner shows the scene's scale (read-only); manual: the raw multiplier."""
            real = self.real.isChecked()
            try:
                if real:
                    self.scale.blockSignals(True)
                    self.scale.setValue(real_world_scale())
                    self.scale.blockSignals(False)
                self.units.setText(scale_label(None if real else self.scale.value()))
            except Exception as e:  # an unexpected unit setup: show it, keep the dialog usable
                self.units.setText(str(e))
            self.scale.setEnabled(not real)

        def import_scale(self):
            return real_world_scale() if self.real.isChecked() else self.scale.value()

        def changeEvent(self, event):
            # the user may change the System Unit Setup while the dialog is open
            if event.type() == QtCore.QEvent.ActivationChange and self.isActiveWindow():
                self.refresh_units()
            super().changeEvent(event)

        def _run(self, fn, *a, **kw):
            try:
                s = fn(*a, **kw)
                QtWidgets.QMessageBox.information(self, "Dave Mirra", f"Imported. Textures: {s['texture_dir']}")
            except Exception as e:  # show any failure to the user
                QtWidgets.QMessageBox.critical(self, "Dave Mirra", str(e))

        def do_zmd(self):
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Dave Mirra model", "", "ZMD (*.zmd *.ZMD)")
            if not path:
                return
            ids = [int(x) for x in self.motions.text().replace(" ", "").split(",") if x]
            mpath = None
            if ids:
                mpath = os.path.join(os.path.dirname(path), "motion.zal")
                if not os.path.isfile(mpath):
                    mpath, _ = QtWidgets.QFileDialog.getOpenFileName(self, "motion.zal", "", "ZAL (*.zal *.ZAL)")
            self.refresh_units()
            self._run(import_zmd, path, scale=self.import_scale(), lod=self.lod.value(), upright=self.upright.isChecked(),
                      texture_dir=self.texdir.text() or None, explicit_normals=self.normals.isChecked(),
                      motion_path=mpath, motion_ids=ids, bone_aim_children=self.bones.currentIndex() == 0,
                      flip_normals=self.flip.isChecked())

        def do_level(self):
            d = QtWidgets.QFileDialog.getExistingDirectory(self, "Level folder (contains the .ZSD)")
            if d:
                self.refresh_units()
                self._run(import_level, d, scale=self.import_scale(), texture_dir=self.texdir.text() or None,
                          explicit_normals=self.normals.isChecked(), flip_normals=self.flip.isChecked())

    dlg = Dialog()
    dlg.show()
    return dlg


if __name__ == "__main__" and rt is not None:
    _DIALOG = show_dialog()

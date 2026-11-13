"""ZMD -> Blender scene (DM-022). Builds objects directly (no Blender glTF importer).

Pipeline: ``dmbmx.convert.gltf.export_glb`` (in memory) -> walk that glTF -> Blender objects.
The glTF JSON (with every ``extras.dmbmx`` rebuild block) is stored on the root object as the custom
property ``dmbmx_gltf``; export (``zmd_export``) rebuilds the glTF from Blender data plus that JSON.

Coordinates: mesh and frame data stay in **RW model space, unchanged** (exact floats). Only the root
object carries ``scale @ Rx(+90) @ upright`` (glTF/RW Y-up -> Blender Z-up, plus the rider correction of
docs/tools/export.md section 2), so riders and bikes stand on +Z and face -Y (Blender's Front view).
"""
import hashlib
import json
import math
import os
import tempfile
import uuid

import bpy
from mathutils import Matrix, Quaternion

from dmbmx.convert import gltf
from dmbmx.core.chunks import parse_rw
from dmbmx.formats import zmd

ADDON_FORMAT = 1
GLTF_TO_BLENDER = Matrix.Rotation(math.radians(90.0), 4, "X")   # (x, y, z) -> (x, -z, y)


def node_matrix(node):
    """glTF node -> mathutils Matrix (matrix or TRS)."""
    if "matrix" in node:
        m = node["matrix"]
        return Matrix([[m[c * 4 + r] for c in range(4)] for r in range(4)])
    t = node.get("translation", [0.0, 0.0, 0.0])
    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    return (Matrix.Translation(t) @ Quaternion((w, x, y, z)).to_matrix().to_4x4()
            @ Matrix.Diagonal((s[0], s[1], s[2], 1.0)))


def flat(m):
    return [float(m[r][c]) for r in range(4) for c in range(4)]


def _ex(node):
    return (node.get("extras") or {}).get(gltf.EXTRAS_KEY, {})


def _safe(name):
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name) or "tex"


def _subtree(js, i):
    out, stack = [], [i]
    while stack:
        k = stack.pop()
        out.append(k)
        stack.extend(reversed(js["nodes"][k].get("children", [])))
    return out


def load(context, filepath, *, lod=-1, upright=True, scale=1.0, texture_dir=""):
    """Import ``filepath``; returns the root object. ``lod`` -1 = all LODs (export can then rebuild
    the whole file without the original); ``texture_dir``: write the textures there as PNG files and
    link them (edit them with any paint program); empty = pack them into the .blend."""
    data = open(filepath, "rb").read()
    model = zmd.read(data)
    if model.empty:
        raise ValueError(f"{os.path.basename(filepath)} is an empty (0-byte) ZMD; nothing to import")
    lods = None if lod is None or lod < 0 else [lod]
    glb = gltf.export_glb(model, source=data, source_name=os.path.basename(filepath), lods=lods,
                          upright=upright)
    js, bin_ = gltf.read_glb(glb)
    root_i = js["scenes"][0]["nodes"][0]
    rnode = js["nodes"][root_i]
    name = rnode.get("name") or os.path.splitext(os.path.basename(filepath))[0]
    uid = uuid.uuid4().hex

    coll = bpy.data.collections.new(name)
    context.scene.collection.children.link(coll)

    def new_obj(oname, odata, parent=None):
        ob = bpy.data.objects.new(oname, odata)
        coll.objects.link(ob)
        if parent is not None:
            ob.parent = parent
            ob.matrix_parent_inverse = Matrix.Identity(4)
        return ob

    root = new_obj(name, None)
    root.empty_display_type = "ARROWS"
    root.empty_display_size = 0.3
    root.matrix_basis = Matrix.Scale(scale, 4) @ GLTF_TO_BLENDER @ node_matrix(rnode)
    root["dmbmx_format"] = ADDON_FORMAT
    root["dmbmx_uid"] = uid
    root["dmbmx_source"] = os.path.abspath(filepath)
    root["dmbmx_source_name"] = os.path.basename(filepath)
    root["dmbmx_sha256"] = hashlib.sha256(data).hexdigest()
    root["dmbmx_gltf"] = json.dumps(js, separators=(",", ":"))
    root["dmbmx_node"] = root_i

    images = _images(js, bin_, name, uid, texture_dir)
    materials = _materials(js, images, name)

    first_lod = True
    for ln in rnode.get("children", []):
        lx = _ex(js["nodes"][ln])
        if "mldf" not in lx:
            continue
        objs = _build_lod(context, js, bin_, ln, root, new_obj, materials, coll)
        if not first_lod:
            for ob in objs:
                ob.hide_set(True)
                ob.hide_render = True
        first_lod = False
    context.view_layer.update()
    _store_rest(root)
    return root


# -- textures / materials --------------------------------------------------------------------

def _images(js, bin_, model_name, uid, texture_dir):
    out = []
    for i, im in enumerate(js.get("images", [])):
        png = gltf.image_bytes(js, bin_, i)
        iname = im.get("name") or f"image{i}"
        if texture_dir:
            os.makedirs(bpy.path.abspath(texture_dir), exist_ok=True)
            path = os.path.join(bpy.path.abspath(texture_dir), f"{_safe(model_name)}_{_safe(iname)}.png")
            with open(path, "wb") as f:
                f.write(png)
            img = bpy.data.images.load(path, check_existing=False)
        else:
            fd, path = tempfile.mkstemp(suffix=".png")
            with os.fdopen(fd, "wb") as f:
                f.write(png)
            try:
                img = bpy.data.images.load(path, check_existing=False)
                img.pack()
                img.filepath = f"//{_safe(model_name)}_{_safe(iname)}.png"
            finally:
                os.unlink(path)
        img.name = f"{model_name}_{iname}"
        img["dmbmx_owner"] = uid
        img["dmbmx_image"] = i
        img["dmbmx_png_sha256"] = hashlib.sha256(png).hexdigest()
        out.append(img)
    return out


def _materials(js, images, model_name):
    out = []
    for k, gm in enumerate(js.get("materials", [])):
        mat = bpy.data.materials.new(f"{model_name}_{gm.get('name', k)}")
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = nt.nodes.get("Principled BSDF")
        pbr = gm.get("pbrMetallicRoughness", {})
        bsdf.inputs["Base Color"].default_value = pbr.get("baseColorFactor", [1, 1, 1, 1])
        bsdf.inputs["Metallic"].default_value = pbr.get("metallicFactor", 0.0)
        bsdf.inputs["Roughness"].default_value = pbr.get("roughnessFactor", 1.0)
        tex = pbr.get("baseColorTexture")
        if tex is not None:
            src = js["textures"][tex["index"]]["source"]
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = images[src]
            tn.location = (-400, 250)
            nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
            if gm.get("alphaMode") in ("BLEND", "MASK"):
                nt.links.new(tn.outputs["Alpha"], bsdf.inputs["Alpha"])
                mat.surface_render_method = "BLENDED" if gm.get("alphaMode") == "BLEND" else "DITHERED"
        mat["dmbmx_material"] = k
        out.append(mat)
    return out


# -- LOD / frames / meshes -------------------------------------------------------------------

def _build_lod(context, js, bin_, ln, root, new_obj, materials, coll):
    nodes = js["nodes"]
    lnode = nodes[ln]
    sub = _subtree(js, ln)
    frames = {}                                  # frame index -> node index
    for i in sub:
        e = _ex(nodes[i])
        if "record" in e and "frame" in e:
            frames[e["frame"]] = i
    mesh_nodes = [i for i in sub if "mesh" in nodes[i]]
    skinned = any("skin" in nodes[i] for i in mesh_nodes)
    parent_of = {c: p for p in sub for c in nodes[p].get("children", [])}
    world = {}
    for fi in sorted(frames):
        ni = frames[fi]
        p = parent_of.get(ni)
        pf = _ex(nodes[p]).get("frame") if p is not None and "record" in _ex(nodes[p]) else None
        world[fi] = (world[pf] if pf is not None else Matrix.Identity(4)) @ node_matrix(nodes[ni])
    created = []
    frame_obj = {}                               # frame index -> (object, bone name or None)

    if skinned:
        adata = bpy.data.armatures.new(lnode.get("name", "armature"))
        arm = new_obj(lnode.get("name", "armature"), adata, root)
        arm.matrix_basis = node_matrix(lnode)
        arm["dmbmx_node"] = ln
        arm.show_in_front = True
        created.append(arm)
        context.view_layer.objects.active = arm
        bpy.ops.object.mode_set(mode="EDIT")
        ebs = {}
        size = max((world[f].translation.length for f in world), default=1.0) or 1.0
        for fi in sorted(frames):
            ni = frames[fi]
            eb = adata.edit_bones.new(nodes[ni].get("name") or f"frame_{fi}")
            kids = [k for k, w in world.items() if _ex(nodes[frames[k]]).get("parent") == fi]
            dist = min(((world[k].translation - world[fi].translation).length for k in kids), default=0.0)
            length = dist if dist > 0.02 * size else 0.08 * size
            eb.head = (0.0, 0.0, 0.0)
            eb.tail = (0.0, length, 0.0)
            eb.matrix = world[fi]
            parent = _ex(nodes[ni]).get("parent", -1)
            if parent in ebs:
                eb.parent = ebs[parent]
            ebs[fi] = eb
        names = {fi: eb.name for fi, eb in ebs.items()}
        bpy.ops.object.mode_set(mode="OBJECT")
        for fi, bn in names.items():
            b = adata.bones[bn]
            b["dmbmx_node"] = frames[fi]
            b["dmbmx_frame"] = fi
            frame_obj[fi] = (arm, bn)
    else:
        lod_obj = new_obj(lnode.get("name", "LOD"), None, root)
        lod_obj.matrix_basis = node_matrix(lnode)
        lod_obj.empty_display_size = 0.05
        lod_obj["dmbmx_node"] = ln
        created.append(lod_obj)
        for fi in sorted(frames):
            ni = frames[fi]
            p = parent_of.get(ni)
            pf = _ex(nodes[p]).get("frame") if p is not None and "record" in _ex(nodes[p]) else None
            ob = new_obj(nodes[ni].get("name") or f"frame_{fi}", None, frame_obj[pf][0] if pf is not None else lod_obj)
            ob.empty_display_type = "SPHERE"
            ob.empty_display_size = 0.02
            ob.matrix_basis = node_matrix(nodes[ni])
            ob["dmbmx_node"] = ni
            ob["dmbmx_frame"] = fi
            frame_obj[fi] = (ob, None)
            created.append(ob)

    for ni in mesh_nodes:
        node = nodes[ni]
        ax = _ex(node)
        ob = new_obj(node.get("name") or "mesh", _build_mesh(js, bin_, node, materials), None)
        ob["dmbmx_node"] = ni
        created.append(ob)
        fobj, bone = frame_obj[ax["frame"]]
        if "skin" in node:
            ob.parent = fobj
            ob.matrix_parent_inverse = Matrix.Identity(4)
            skin = js["skins"][node["skin"]]
            jframe = [_ex(nodes[j])["frame"] for j in skin["joints"]]
            vgs = [ob.vertex_groups.new(name=frame_obj[f][1]) for f in jframe]
            prim = js["meshes"][node["mesh"]]["primitives"][0]
            groups = {}
            for v, jt in enumerate(gltf.accessor_values(js, bin_, prim["attributes"]["JOINTS_0"])):
                groups.setdefault(jt[0], []).append(v)
            for j, vs in groups.items():
                vgs[j].add(vs, 1.0, "REPLACE")
            mod = ob.modifiers.new("Armature", "ARMATURE")
            mod.object = fobj
        elif bone is not None:
            ob.parent = fobj
            ob.parent_type = "BONE"
            ob.parent_bone = bone
            context.view_layer.update()
            ob.matrix_world = fobj.matrix_world @ fobj.data.bones[bone].matrix_local
        else:
            ob.parent = fobj
            ob.matrix_parent_inverse = Matrix.Identity(4)
    return created


def _slot_count(ax):
    ml = parse_rw(gltf._unb64(ax["geometry"]["material_list"]))[0]
    return int.from_bytes(ml.children[0].data[:4], "little")


def _build_mesh(js, bin_, node, materials):
    prims = js["meshes"][node["mesh"]]["primitives"]
    attrs = prims[0]["attributes"]

    def acc(key):
        return gltf.accessor_values(js, bin_, attrs[key]) if key in attrs else None

    P = acc("POSITION")
    faces, mats = [], []
    slot_mat = {}
    for p in prims:
        slot = _ex(p).get("material_slot", 0)
        slot_mat[slot] = p.get("material")
        ix = gltf.accessor_values(js, bin_, p["indices"])
        for k in range(0, len(ix), 3):
            faces.append((ix[k], ix[k + 1], ix[k + 2]))
            mats.append(slot)
    me = bpy.data.meshes.new(node.get("name") or "mesh")
    me.from_pydata([tuple(p) for p in P], [], faces)
    nslots = max(_slot_count(_ex(node)), max(slot_mat) + 1 if slot_mat else 0)
    for s in range(nslots):
        mi = slot_mat.get(s)
        me.materials.append(materials[mi] if mi is not None else None)
    me.polygons.foreach_set("material_index", mats)
    corner_vert = [0] * len(me.loops)
    me.loops.foreach_get("vertex_index", corner_vert)
    for k, key in enumerate(("TEXCOORD_0", "TEXCOORD_1")):
        T = acc(key)
        if T is None:
            continue
        layer = me.uv_layers.new(name="UVMap" if k == 0 else f"UVMap.{k:03d}")
        layer.uv.foreach_set("vector", [c for v in corner_vert for c in (T[v][0], 1.0 - T[v][1])])
        raw = me.attributes.new(f"dmbmx_uv{k}_v", "FLOAT", "CORNER")
        raw.data.foreach_set("value", [T[v][1] for v in corner_vert])
    N = acc("NORMAL")
    if N is not None:
        a = me.attributes.new("dmbmx_normal", "FLOAT_VECTOR", "POINT")
        a.data.foreach_set("vector", [c for n in N for c in n])
        me.shade_smooth()                        # flat (sharp) faces would ignore custom normals
        if "sharp_edge" in me.attributes:
            me.attributes.remove(me.attributes["sharp_edge"])
        me.normals_split_custom_set_from_vertices([tuple(n) for n in N])
    C = acc("COLOR_0")
    if C is not None:
        ca = me.color_attributes.new("Col", "BYTE_COLOR", "POINT")
        ca.data.foreach_set("color_srgb", [c / 255.0 for col in C for c in col])
    me.update()
    return me


def _store_rest(root):
    """Remember each frame/mesh transform exactly as Blender holds it after import; export compares
    against these to tell 'unchanged' (keep the exact RW record) from 'edited'."""
    for ob in [root] + list(root.children_recursive):
        if "dmbmx_node" in ob and ob is not root:
            ob["dmbmx_rest"] = flat(local_matrix(ob))
        if ob.type == "ARMATURE":
            for b in ob.data.bones:
                if "dmbmx_node" in b:
                    b["dmbmx_rest"] = flat(b.matrix_local)


def local_matrix(ob):
    """Transform of ``ob`` relative to its parent object (or parent bone), parent-inverse included."""
    if ob.parent is None:
        return ob.matrix_basis.copy()
    if ob.parent_type == "OBJECT":
        return ob.matrix_parent_inverse @ ob.matrix_basis   # pure data: no depsgraph, no inversion
    if ob.parent_type == "BONE" and ob.parent_bone:
        pw = ob.parent.matrix_world @ ob.parent.data.bones[ob.parent_bone].matrix_local
    else:
        pw = ob.parent.matrix_world
    return pw.inverted_safe() @ ob.matrix_world

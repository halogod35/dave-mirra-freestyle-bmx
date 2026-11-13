"""Blender scene -> ZMD (DM-022).

Rebuilds the glTF document of the import (the ``dmbmx_gltf`` JSON on the root object) from the current
Blender data: meshes (positions, normals, UVs, colours, material per face, vertex groups), frame
transforms (empties / bones), images. Then it calls ``dmbmx.convert.gltf_import.import_glb``, so all
ZMD rules (BinMesh/Bone/sphere regeneration, texture re-encoding, refusals) are the importer's.

Exactness: Blender keeps positions as float32 (exact), but stores custom normals as 16-bit and UVs
as (u, 1 - v). Import therefore also stores the original normals (``dmbmx_normal``) and V coordinates
(``dmbmx_uvN_v``); export uses those wherever Blender's current value still equals what import made,
and Blender's value elsewhere (edited areas).
"""
import hashlib
import json
import math
import os
import shutil

import bpy
from mathutils import Matrix, Vector

from dmbmx.convert import gltf, imaging
from dmbmx.convert.glb_edit import GlbDoc
from dmbmx.convert.gltf_import import import_glb

from .zmd_import import flat, local_matrix

FLOAT, UBYTE, USHORT, UINT = 5126, 5121, 5123, 5125
TOL = 1e-6


class ExportError(Exception):
    pass


def find_root(obj):
    while obj is not None and "dmbmx_gltf" not in obj:
        obj = obj.parent
    return obj


def _f32(x):
    import struct
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _changed(cur, rest):
    return rest is None or any(abs(a - b) > TOL * max(1.0, abs(b)) for a, b in zip(flat(cur), rest))


def _colmajor(m):
    return [float(m[r][c]) for c in range(4) for r in range(4)]


def build_glb(root, normals="keep"):
    """Return ``(glb_bytes, notes)`` for the model under ``root``. ``normals``: 'keep' = the original
    normals stored at import (Blender carries them along when you edit; new vertices without one use
    Blender's), 'recalc' = Blender's current shading normals for every vertex."""
    js = json.loads(root["dmbmx_gltf"])
    nodes = js["nodes"]
    doc = GlbDoc(js=js)
    notes = []
    objs = list(root.children_recursive)

    # frames (empties) and bones
    for ob in objs:
        if "dmbmx_node" not in ob:
            continue
        node = nodes[ob["dmbmx_node"]]
        ex = gltf_ex(node)
        if ob.type == "EMPTY" and "record" in ex:
            cur = local_matrix(ob)
            if _changed(cur, ob.get("dmbmx_rest")):
                node["matrix"] = _colmajor(cur)
                notes.append(f"frame {ob.name}: transform edited")
        elif ob.type == "ARMATURE":
            if _changed(local_matrix(ob), ob.get("dmbmx_rest")):
                notes.append(f"{ob.name}: armature object transform is ignored (edit bones in Edit Mode)")
            for b in ob.data.bones:
                if "dmbmx_node" not in b or not _changed(b.matrix_local, b.get("dmbmx_rest")):
                    continue
                local = b.parent.matrix_local.inverted_safe() @ b.matrix_local if b.parent else b.matrix_local
                bnode = nodes[b["dmbmx_node"]]
                # keep the frame's scale (bones cannot hold scale)
                old = Matrix([[bnode.get("matrix", _colmajor(Matrix.Identity(4)))[c * 4 + r] for c in range(4)]
                              for r in range(4)])
                s = old.to_scale()
                bnode["matrix"] = _colmajor(local @ Matrix.Diagonal((s[0], s[1], s[2], 1.0)))
                notes.append(f"bone {b.name}: rest pose edited")

    # meshes
    for ob in objs:
        if ob.type != "MESH" or "dmbmx_node" not in ob:
            continue
        node = nodes[ob["dmbmx_node"]]
        if "mesh" not in node:
            continue
        others = [m.type for m in ob.modifiers if m.type != "ARMATURE"]
        if others:
            notes.append(f"{ob.name}: modifiers {others} are NOT applied (apply them first to export their result)")
        skinned = "skin" in node
        cur = local_matrix(ob)
        if skinned:
            if _changed(cur, ob.get("dmbmx_rest")):
                notes.append(f"{ob.name}: object transform of a skinned mesh is ignored (edit it in Edit Mode)")
        elif _changed(cur, ob.get("dmbmx_rest")):
            node["matrix"] = _colmajor(cur)
            notes.append(f"{ob.name}: object transform baked into the part")
        _write_mesh(doc, js, node, ob, skinned, normals)

    # images
    uid = root.get("dmbmx_uid")
    have = {}
    for img in bpy.data.images:
        if img.get("dmbmx_owner") == uid and "dmbmx_image" in img:
            have[img["dmbmx_image"]] = img
    for i in range(len(js.get("images", []))):
        if i not in have:
            raise ExportError(f"texture image #{i} ({js['images'][i].get('name')}) is missing from the .blend")
        doc.set_image(i, image_png(have[i]))
    return doc.to_bytes(), notes


def gltf_ex(node):
    return (node.get("extras") or {}).get(gltf.EXTRAS_KEY, {})


def _write_mesh(doc, js, node, ob, skinned, normals="keep"):
    me = ob.data
    nv0 = len(me.vertices)
    if nv0 == 0:
        raise ExportError(f"{ob.name}: mesh has no vertices")
    co = [0.0] * (3 * nv0)
    me.vertices.foreach_get("co", co)
    nl = len(me.loops)
    corner_vert = [0] * nl
    me.loops.foreach_get("vertex_index", corner_vert)

    # per-corner UVs, exact where unchanged
    uv_sets = []
    for k in range(2):
        lname = "UVMap" if k == 0 else f"UVMap.{k:03d}"
        layer = me.uv_layers.get(lname) or (me.uv_layers[k] if len(me.uv_layers) > k else None)
        if layer is None:
            break
        uv = [0.0] * (2 * nl)
        layer.uv.foreach_get("vector", uv)
        raw = me.attributes.get(f"dmbmx_uv{k}_v")
        rawv = None
        if raw is not None and raw.domain == "CORNER" and raw.data_type == "FLOAT":
            rawv = [0.0] * nl
            raw.data.foreach_get("value", rawv)
        vals = []
        for c in range(nl):
            u, bv = uv[2 * c], uv[2 * c + 1]
            v = 1.0 - bv
            if rawv is not None and _f32(1.0 - rawv[c]) == _f32(bv):
                v = rawv[c]
            vals.append((u, v))
        uv_sets.append(vals)

    # per-vertex normals: Blender corner normals, replaced by the stored exact ones where they agree
    cn = [0.0] * (3 * nl)
    me.corner_normals.foreach_get("vector", cn)
    stored = me.attributes.get("dmbmx_normal")
    sn = None
    if stored is not None and stored.domain == "POINT":
        sn = [0.0] * (3 * nv0)
        stored.data.foreach_get("vector", sn)

    colors = None
    ca = me.color_attributes.get("Col")
    if ca is not None:
        raw = [0.0] * (4 * len(ca.data))
        ca.data.foreach_get("color_srgb", raw)
        if ca.domain == "POINT":
            colors = [tuple(int(round(raw[4 * v + k] * 255)) for k in range(4)) for v in range(nv0)]
        else:  # corner colours: take the first corner of each vertex
            colors = [None] * nv0
            for c, v in enumerate(corner_vert):
                if colors[v] is None:
                    colors[v] = tuple(int(round(raw[4 * c + k] * 255)) for k in range(4))
            colors = [c or (255, 255, 255, 255) for c in colors]

    # split vertices whose corners disagree on UVs (seams added in Blender); vertex i keeps index i
    key_of_corner = [tuple(s[c] for s in uv_sets) for c in range(nl)]
    vkey = [None] * nv0
    extra = {}                                   # (vertex, key) -> new index
    src = list(range(nv0))                       # new vertex -> source Blender vertex
    corner_out = [0] * nl
    for c, v in enumerate(corner_vert):
        k = key_of_corner[c]
        if vkey[v] is None or vkey[v] == k:
            vkey[v] = k
            corner_out[c] = v
        else:
            if (v, k) not in extra:
                extra[(v, k)] = len(src)
                src.append(v)
            corner_out[c] = extra[(v, k)]
    first_corner = {}
    for c, o in enumerate(corner_out):
        first_corner.setdefault(o, c)
    corners_of = {}
    for c, o in enumerate(corner_out):
        corners_of.setdefault(o, []).append(c)

    P = [(co[3 * v], co[3 * v + 1], co[3 * v + 2]) for v in src]
    N = []
    for o, v in enumerate(src):
        s = sn[3 * v:3 * v + 3] if sn is not None and normals == "keep" else None
        if s is not None and abs(math.sqrt(s[0] ** 2 + s[1] ** 2 + s[2] ** 2) - 1.0) < 1e-3:
            N.append(tuple(s))                   # the original normal, exact (carried along by Blender edits)
            continue
        acc = Vector((0.0, 0.0, 0.0))
        for c in corners_of.get(o, []):
            acc += Vector(cn[3 * c:3 * c + 3])
        N.append(tuple(acc.normalized()) if acc.length > 1e-12 else tuple(me.vertices[v].normal))
    UV = []
    for s in range(len(uv_sets)):
        col = []
        for o, v in enumerate(src):
            c = first_corner.get(o)
            col.append(uv_sets[s][c] if c is not None else (0.0, 0.0))
        UV.append(col)

    # triangles per material slot, CCW, polygon order
    by_slot = {}
    me.calc_loop_triangles()
    tri_of_poly = {}
    for lt in me.loop_triangles:
        tri_of_poly.setdefault(lt.polygon_index, []).append(tuple(lt.loops))
    for p in me.polygons:
        loops = [tuple(p.loop_indices)] if p.loop_total == 3 else tri_of_poly.get(p.index, [])
        for ls in loops:
            by_slot.setdefault(p.material_index, []).append(tuple(corner_out[c] for c in ls))

    attrs = {"POSITION": doc.add(P, FLOAT, "VEC3", target=34962, minmax=True)}
    old_prims = js["meshes"][node["mesh"]]["primitives"]
    old_attrs = old_prims[0]["attributes"]
    if "NORMAL" in old_attrs:
        attrs["NORMAL"] = doc.add(N, FLOAT, "VEC3", target=34962)
    for s, key in enumerate(("TEXCOORD_0", "TEXCOORD_1")):
        if key in old_attrs:
            if s >= len(UV):
                raise ExportError(f"{ob.name}: the UV map {key} was deleted; the game needs it")
            attrs[key] = doc.add(UV[s], FLOAT, "VEC2", target=34962)
    if "COLOR_0" in old_attrs and colors is not None:
        attrs["COLOR_0"] = doc.add([colors[v] for v in src], UBYTE, "VEC4", target=34962, normalized=True)
    if skinned:
        j, w = _skin_weights(ob, js, node, nv0)
        attrs["JOINTS_0"] = doc.add([j[v] for v in src], USHORT, "VEC4", target=34962)
        attrs["WEIGHTS_0"] = doc.add([w[v] for v in src], FLOAT, "VEC4", target=34962)
    slot_mat = {gltf_ex(p).get("material_slot"): p.get("material") for p in old_prims}
    prims = []
    itype = USHORT if len(P) <= 65535 else UINT
    for s in sorted(by_slot):
        ix = [i for t in by_slot[s] for i in t]
        prim = {"attributes": attrs, "indices": doc.add(ix, itype, "SCALAR", target=34963, minmax=True), "mode": 4,
                "extras": {gltf.EXTRAS_KEY: {"material_slot": s}}}
        if slot_mat.get(s) is not None:
            prim["material"] = slot_mat[s]
        prims.append(prim)
    if not prims:
        raise ExportError(f"{ob.name}: mesh has no faces")
    js["meshes"][node["mesh"]] = {"name": js["meshes"][node["mesh"]].get("name", ob.name), "primitives": prims}


def _skin_weights(ob, js, node, nv):
    """Per vertex up to 4 (joint, weight) from the vertex groups; joint = index in the glTF skin."""
    skin = js["skins"][node["skin"]]
    arm = ob.parent if ob.parent is not None and ob.parent.type == "ARMATURE" else None
    if arm is None:
        for m in ob.modifiers:
            if m.type == "ARMATURE" and m.object is not None:
                arm = m.object
    if arm is None:
        raise ExportError(f"{ob.name}: skinned mesh is no longer attached to its armature")
    joint_of_node = {n: k for k, n in enumerate(skin["joints"])}
    joint_of_group = {}
    for vg in ob.vertex_groups:
        b = arm.data.bones.get(vg.name)
        if b is not None and b.get("dmbmx_node") in joint_of_node:
            joint_of_group[vg.index] = joint_of_node[b["dmbmx_node"]]
    J, W, missing = [], [], 0
    for v in ob.data.vertices:
        gw = sorted(((g.weight, joint_of_group[g.group]) for g in v.groups
                     if g.group in joint_of_group and g.weight > 0.0), reverse=True)[:4]
        if not gw:
            missing += 1
            gw = [(1.0, 0)]
        tot = sum(w for w, _ in gw)
        gw += [(0.0, 0)] * (4 - len(gw))
        J.append(tuple(j for _, j in gw))
        W.append(tuple(w / tot for w, _ in gw))
    if missing:
        raise ExportError(f"{ob.name}: {missing} vertices are not assigned to any bone (every vertex of a rider "
                          "needs one bone; use Weight Paint or Ctrl+G > Assign)")
    return J, W


def image_png(img):
    """PNG bytes for a Blender image: the original bytes when untouched, else re-encoded pixels."""
    data = None
    if not img.is_dirty:
        if img.packed_file is not None:
            data = bytes(img.packed_file.data)
        else:
            path = bpy.path.abspath(img.filepath)
            if os.path.isfile(path):
                data = open(path, "rb").read()
    if data is not None and data[:8] == imaging.PNG_SIG:
        return data
    if data is not None and data[:2] == b"BM":
        return imaging.bmp_to_png(data)
    w, h = img.size
    if w == 0 or h == 0:
        raise ExportError(f"image {img.name}: no pixel data (missing file?)")
    px = [0.0] * (w * h * 4)
    img.pixels.foreach_get(px)
    rows = []
    for y in range(h - 1, -1, -1):          # Blender pixels are bottom-up
        o = y * w * 4
        rows.append(bytes(max(0, min(255, int(round(c * 255.0)))) for c in px[o:o + w * 4]))
    return imaging.png_encode(w, h, rows, color_type=6)


def save(root, filepath, *, binmesh="auto", palettised="promote", strict=False, copy_to_testgame=False,
         game_dir=None, normals="keep"):
    """Export ``root``'s model to ``filepath``. Returns ``(report, notes, testgame_path or None)``."""
    from dmbmx.cli.import_ import protected
    game_dir = game_dir or os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX")
    if protected(filepath, game_dir):
        raise ExportError("refusing to write into the game install or _pristine (they are read-only). "
                          "Save somewhere else and use 'Copy into _testgame'.")
    glb, notes = build_glb(root, normals)
    js = json.loads(root["dmbmx_gltf"])
    rx = gltf_ex(js["nodes"][root["dmbmx_node"]])
    base = None
    if not rx.get("complete"):
        src = root.get("dmbmx_source", "")
        if not os.path.isfile(src):
            raise ExportError(f"this model was imported with a single LOD; the original file {src} is needed "
                              "for the other LODs but was not found")
        base = open(src, "rb").read()
    out, rep = import_glb(glb, base=base, binmesh=binmesh, strict=strict, palettised=palettised)
    with open(filepath, "wb") as f:
        f.write(out)
    dest = None
    if copy_to_testgame:
        dest = os.path.join(game_dir, "_testgame", "assets", root.get("dmbmx_source_name", os.path.basename(filepath)))
        if not os.path.isfile(dest):
            raise ExportError(f"{dest} does not exist; only stock model files can be replaced in _testgame")
        shutil.copyfile(filepath, dest)
    return rep, notes, dest

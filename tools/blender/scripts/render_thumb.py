"""Render a preview of an exported .glb with Blender headless (DM-020 review thumbnails).

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
        --python tools/blender/scripts/render_thumb.py -- <in.glb> <out.png> [--size 960x540]

The model is imported twice: on the left as imported (textured), on the right translucent with
the armature drawn as orange bone shapes (skinned models) or the frame empties drawn as small
markers (rigid models), so the skeleton/frame hierarchy is visible. Camera: front 3/4 view in
Blender's Z-up world, so an upright rider stands vertically in the image.
"""
import argparse
import math
import os
import sys

import bpy
from mathutils import Vector


def _import(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    new = [o for o in new if not any(c.name.startswith("glTF_not_exported") for c in o.users_collection)]
    roots = [o for o in new if o.parent is None]
    return new, roots


def _bbox(objs):
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
    for o in objs:
        if o.type != "MESH":
            continue
        ev = o.evaluated_get(deps)
        me = ev.to_mesh()
        for v in me.vertices:
            p = ev.matrix_world @ v.co
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
        ev.to_mesh_clear()
    return lo, hi


def _mat(name, rgba, emission=0.0, alpha=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = rgba
    if emission:
        bsdf.inputs["Emission Color"].default_value = rgba
        bsdf.inputs["Emission Strength"].default_value = emission
    bsdf.inputs["Alpha"].default_value = alpha
    if alpha < 1.0:
        m.surface_render_method = "BLENDED"
    return m


def _octa(head, tail, scale):
    axis = tail - head
    ln = axis.length
    if ln < 1e-6:
        return [], []
    d = axis.normalized()
    ref = Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((1, 0, 0))
    u = d.cross(ref).normalized()
    v = d.cross(u).normalized()
    w = max(ln * 0.12, scale)
    mid = head + axis * 0.15
    verts = [head, tail, mid + u * w, mid + v * w, mid - u * w, mid - v * w]
    faces = [(0, 2, 3), (0, 3, 4), (0, 4, 5), (0, 5, 2), (1, 3, 2), (1, 4, 3), (1, 5, 4), (1, 2, 5)]
    return verts, faces


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("out")
    ap.add_argument("--size", default="960x540")
    a = ap.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene

    objs_a, roots_a = _import(a.glb)
    lo, hi = _bbox(objs_a)
    size = hi - lo
    gap = max(size.x, size.y) * 1.3 + 0.2
    objs_b, roots_b = _import(a.glb)
    for r in roots_b:
        r.location.x += gap
    bpy.context.view_layer.update()

    # right copy: translucent model + skeleton markers
    ghost = _mat("ghost", (0.6, 0.7, 0.9, 1.0), alpha=0.25)
    bone_mat = _mat("bones", (1.0, 0.45, 0.05, 1.0), emission=2.0)
    for o in objs_b:
        if o.type == "MESH":
            o.data.materials.clear()
            o.data.materials.append(ghost)
    verts, faces = [], []
    marker = max(size.length * 0.012, 0.004)
    for o in objs_b:
        if o.type == "ARMATURE":
            # glTF joints have no length (the importer guesses tails), so draw parent -> child
            # links between joint positions, and the importer's tail only for leaf bones.
            for b in o.data.bones:
                if b.parent is not None:
                    v, f = _octa(o.matrix_world @ b.parent.head_local, o.matrix_world @ b.head_local, marker)
                    faces += [tuple(i + len(verts) for i in fc) for fc in f]
                    verts += v
                if b.children:
                    continue
                tail = b.head_local + (b.tail_local - b.head_local).normalized() * (size.length * 0.04)
                v, f = _octa(o.matrix_world @ b.head_local, o.matrix_world @ tail, marker)
                faces += [tuple(i + len(verts) for i in fc) for fc in f]
                verts += v
        elif o.type == "EMPTY" and o.parent is not None and o.children:
            # frame empties (rigid models): a small diamond at each frame + a line to the parent
            p = o.matrix_world.translation
            q = o.parent.matrix_world.translation
            v, f = _octa(q, p, marker) if (p - q).length > 1e-4 else ([], [])
            faces += [tuple(i + len(verts) for i in fc) for fc in f]
            verts += v
    if verts:
        me = bpy.data.meshes.new("skeleton")
        me.from_pydata([tuple(v) for v in verts], [], faces)
        me.materials.append(bone_mat)
        ob = bpy.data.objects.new("skeleton", me)
        sc.collection.objects.link(ob)

    # camera / lights / world
    center = (lo + hi) / 2 + Vector((gap / 2, 0, 0))
    span = max(size.z, size.x + gap, size.y) * 1.25
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = span
    direction = Vector((0.45, -1.0, 0.35)).normalized()  # front 3/4 view (front = -Y in Blender)
    cam.location = center + direction * (span * 3)
    cam.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_end = span * 10
    sc.camera = cam
    for rot, energy in (((math.radians(50), 0, math.radians(30)), 3.0), ((math.radians(60), 0, math.radians(200)), 1.0)):
        sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
        sun.data.energy = energy
        sun.rotation_euler = rot
        sc.collection.objects.link(sun)
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.18, 0.19, 0.22, 1.0)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.8
    sc.world = world

    w, h = (int(x) for x in a.size.split("x"))
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = "PNG"
    sc.render.filepath = os.path.abspath(a.out)
    sc.view_settings.view_transform = "Standard"
    bpy.ops.render.render(write_still=True)
    print("WROTE", sc.render.filepath, "bbox", tuple(round(x, 3) for x in lo), tuple(round(x, 3) for x in hi))


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.stdout.flush()
    os._exit(0)

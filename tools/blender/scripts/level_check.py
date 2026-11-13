"""Headless Blender check + thumbnail of `dmbmx level export` output (DM-013). Not part of pytest.

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
        --python tools/blender/scripts/level_check.py -- <level.glb> <level_dir> [--png out.png] [--size 1280x720]
        [--view persp|top] [--json out.json]

Imports the .glb and compares it with what dmbmx reads from the level folder: world triangle
count, number of prop nodes and their mesh triangle total, tag face count, scene-point empties.
Then (optionally) renders the world + props (tags hidden) from a 3/4 perspective or top view.
Exit code 0 = everything matches.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

import bpy
from mathutils import Vector

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
from dmbmx.formats import level as lvl  # noqa: E402


def expected(level_dir):
    lv = lvl.Level.read_dir(level_dir)
    # Blender keeps one face per vertex set: double-sided duplicates (same 3 vertices, any
    # winding) inside one sector/material collapse on import, so they are not counted.
    dups = 0
    for s in lv.world.sectors():
        seen = set()
        for m, a, b, c in s.triangles:
            k = (m, frozenset((a, b, c)))
            dups += k in seen
            seen.add(k)
    e = {"world_tris": lv.world.num_triangles - dups, "world_dup_tris": dups, "props": len(lv.props.props),
         "prop_tris": 0, "tag_faces": len(lv.tags.faces), "points": len(lv.scene.points)}
    # Blender also drops triangles that repeat a vertex index (4 such faces in ProTourDirt props).
    defs = lv.prop_defs.by_id()
    for p in lv.props.props:
        d = defs.get(p.def_id)
        if d is not None:
            for ms in d.model.meshes(0):
                ix = ms["indices"]
                e["prop_tris"] += sum(1 for t in range(len(ix) // 3) if len(set(ix[3 * t:3 * t + 3])) == 3)
    return e


def _tris(ob):
    return sum(1 for p in ob.data.polygons if p.loop_total == 3)


def measure():
    r = {"world_tris": 0, "props": 0, "prop_tris": 0, "tag_faces": 0, "points": 0, "non_tri": 0,
         "textured_mats": 0, "images": len(bpy.data.images)}
    by_name = {o.name: o for o in bpy.data.objects}
    for o in bpy.data.objects:
        top = o
        chain = []
        while top.parent is not None:
            chain.append(top.parent.name)
            top = top.parent
        group = chain[-2] if len(chain) >= 2 else None   # child of <Level> root
        if o.type == "MESH":
            r["non_tri"] += sum(1 for p in o.data.polygons if p.loop_total != 3)
        if o.name.startswith("World") and o.type == "MESH":
            r["world_tris"] += _tris(o)
        elif group and group.startswith("Props"):
            r["props"] += 1
            if o.type == "MESH":
                r["prop_tris"] += _tris(o)
        elif group and group.startswith("Tags") and o.type == "MESH":
            r["tag_faces"] += _tris(o)
        elif group and group.startswith("ScenePoints"):
            r["points"] += 1
    for m in bpy.data.materials:
        if m.use_nodes and any(n.type == "TEX_IMAGE" and n.image for n in m.node_tree.nodes):
            r["textured_mats"] += 1
    return r, by_name


def render(out, size, view):
    sc = bpy.context.scene
    for o in bpy.data.objects:   # hide tags + scene points in the picture
        p, hide = o, False
        while p is not None:
            if p.name.startswith("Tags") or p.name.startswith("ScenePoints"):
                hide = True
            p = p.parent
        o.hide_render = hide
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
    for o in bpy.data.objects:
        if o.type != "MESH" or o.hide_render:
            continue
        for c in o.bound_box:
            p = o.matrix_world @ Vector(c)
            lo, hi = Vector(map(min, lo, p)), Vector(map(max, hi, p))
    center, ext = (lo + hi) / 2, hi - lo
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    span = max(ext.x, ext.y)
    if view == "top":
        cam.data.type = "ORTHO"
        cam.data.ortho_scale = span * 1.05
        cam.location = center + Vector((0, 0, ext.z + span))
        cam.rotation_euler = (0, 0, 0)
    else:
        cam.data.lens = 28
        d = Vector((0.9, -1.1, 0.75)).normalized()
        cam.location = center + d * span * 0.95
        cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_end = span * 20
    sc.camera = cam
    for rot, energy in (((math.radians(45), 0, math.radians(35)), 3.0), ((math.radians(70), 0, math.radians(210)), 1.2)):
        sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
        sun.data.energy = energy
        sun.rotation_euler = rot
        sc.collection.objects.link(sun)
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.65, 0.8, 1.0)
    world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
    sc.world = world
    w, h = (int(x) for x in size.split("x"))
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = "PNG"
    sc.render.filepath = os.path.abspath(out)
    sc.view_settings.view_transform = "Standard"
    bpy.ops.render.render(write_still=True)
    print("WROTE", sc.render.filepath)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("level_dir")
    ap.add_argument("--png")
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--view", default="persp", choices=("persp", "top"))
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=a.glb)
    got, _ = measure()
    want = expected(a.level_dir)
    keys = ("world_tris", "props", "prop_tris", "tag_faces", "points")
    ok = all(got[k] == want[k] for k in keys) and got["non_tri"] == 0
    print("EXPECTED", json.dumps(want))
    print("BLENDER ", json.dumps(got))
    print("RESULT", "OK" if ok else "MISMATCH")
    if a.json:
        Path(a.json).write_text(json.dumps({"expected": want, "blender": got, "ok": ok}, indent=1))
    if a.png:
        render(a.png, a.size, a.view)
    return 0 if ok else 1


if __name__ == "__main__":
    rc = main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.stdout.flush()
    os._exit(rc)

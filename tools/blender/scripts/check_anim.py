"""Blender headless check for dmbmx .glb animations (DM-016), plus optional frame renders.

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
        --python tools/blender/scripts/check_anim.py -- <in.glb> [--render out/prefix] [--action motion_008] \
        [--frames 0,10,20] [--max-ratio 2.0] [--view front34|side|front]

For every imported action it steps through all frames, evaluates the skinned mesh and checks that:
  * every vertex is finite,
  * the mesh bounding-box diagonal stays within --max-ratio of the rest-pose diagonal (no explosion),
  * the mesh stays near the origin (center within one rest diagonal).
Prints one line per action and "ANIM CHECK OK" / "ANIM CHECK FAIL". With --render it renders the
chosen frames of --action (front 3/4 view) to <prefix>_f<frame>.png.
"""
import argparse
import math
import os
import sys

import bpy
from mathutils import Vector


def bbox(objs):
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
    finite = True
    for o in objs:
        if o.type != "MESH":
            continue
        ev = o.evaluated_get(deps)
        me = ev.to_mesh()
        for v in me.vertices:
            p = ev.matrix_world @ v.co
            if not all(math.isfinite(c) for c in p):
                finite = False
                continue
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
        ev.to_mesh_clear()
    return lo, hi, finite


def assign(arm, act):
    ad = arm.animation_data or arm.animation_data_create()
    ad.action = act
    if hasattr(ad, "action_slot") and getattr(act, "slots", None):
        try:
            ad.action_slot = act.slots[0]
        except (TypeError, AttributeError, RuntimeError):
            pass


def setup_render(sc, lo, hi, w=640, h=640, view="front34"):
    center = (lo + hi) / 2
    span = max((hi - lo).length, 1.0) * 1.1
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = span
    d = Vector({"front34": (0.6, -1.0, 0.3), "side": (1.0, 0.0, 0.05), "front": (0.0, -1.0, 0.05)}[view]).normalized()
    cam.location = center + d * span * 3
    cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_end = span * 10
    sc.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    sc.collection.objects.link(sun)
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.2, 0.21, 0.24, 1.0)
    sc.world = world
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.image_settings.file_format = "PNG"
    sc.view_settings.view_transform = "Standard"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("--render", default=None)
    ap.add_argument("--action", default=None)
    ap.add_argument("--frames", default="0")
    ap.add_argument("--max-ratio", type=float, default=2.0)
    ap.add_argument("--view", default="front34", choices=("front34", "side", "front"))
    a = ap.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    bpy.ops.import_scene.gltf(filepath=a.glb)
    objs = list(bpy.data.objects)
    arms = [o for o in objs if o.type == "ARMATURE"]
    acts = sorted(bpy.data.actions, key=lambda x: x.name)
    if not arms or not acts:
        print("ANIM CHECK FAIL: no armature or no actions", len(arms), len(acts))
        return 1
    arm = arms[0]
    # rest reference: pose of frame 0 of the first action
    ok = True
    ref = None
    results = []
    for act in acts:
        assign(arm, act)
        f0, f1 = (int(x) for x in act.frame_range)
        worst = 0.0
        for f in range(f0, f1 + 1):
            sc.frame_set(f)
            lo, hi, finite = bbox(objs)
            diag = (hi - lo).length
            if ref is None:
                ref = diag
            ratio = diag / ref
            off = ((lo + hi) / 2).length / ref
            worst = max(worst, ratio)
            if not finite or ratio > a.max_ratio or off > 1.0:
                ok = False
                print(f"  {act.name} frame {f}: finite={finite} ratio={ratio:.2f} center_off={off:.2f}")
        results.append(f"{act.name}: frames {f0}-{f1}, worst bbox ratio {worst:.2f}")
    for r in results:
        print(r)
    if a.render:
        act = next((x for x in acts if a.action and x.name.startswith(a.action)), acts[0])
        assign(arm, act)
        sc.frame_set(int(act.frame_range[0]))
        lo, hi, _ = bbox(objs)
        setup_render(sc, lo, hi, view=a.view)
        for f in (int(x) for x in a.frames.split(",")):
            sc.frame_set(f)
            sc.render.filepath = os.path.abspath(f"{a.render}_f{f:03d}.png")
            bpy.ops.render.render(write_still=True)
            print("WROTE", sc.render.filepath)
    print("ANIM CHECK OK" if ok else "ANIM CHECK FAIL", f"({len(acts)} actions, rest diag {ref:.3f})")
    return 0 if ok else 1


if __name__ == "__main__":
    rc = main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.stdout.flush()
    os._exit(rc)

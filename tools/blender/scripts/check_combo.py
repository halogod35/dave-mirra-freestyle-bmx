"""Blender headless check (and renders) for `dmbmx export-combo` files (DM-018).

    blender -b --factory-startup --python tools/blender/scripts/check_combo.py -- combo.glb [--render out/prefix]
        [--frames 0,8,16] [--view side|front34|front] [--riding]

Imports the .glb, plays the first clip (the importer assigns it to every animated object) and checks every
frame:
  * all mesh vertices finite, scene bounding box within 2x of frame 0 (no explosion)
  * feet: LeftFoot/RightFoot bone heads vs the LeftPedal/RightPedal empties      (median, max)
  * hands: LeftHand/RightHand bone heads vs the nearest Handlebars-mesh vertex   (median)
  * wheels: total rotation of the BackWheel empty over the clip                  (degrees)
Prints "COMBO CHECK OK" when nothing explodes (and, with --riding, feet < 0.12 and hands < 0.25 medians;
use it for riding clips such as 8 and 15 - in tricks the hands/feet leave the bike on purpose).
"""
import argparse
import math
import os
import statistics as st
import sys

import bpy
from mathutils import Vector


def find(name, typ=None):
    for o in bpy.data.objects:
        if o.name.split(".")[0] == name and (typ is None or o.type == typ):
            return o
    return None


def bone_head(arm, bone):
    pb = arm.pose.bones.get(bone)
    return arm.matrix_world @ pb.head if pb else None


def mesh_points(obj):
    deps = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(deps)
    me = ev.to_mesh()
    pts = [ev.matrix_world @ v.co for v in me.vertices]
    ev.to_mesh_clear()
    return pts


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("--render", default=None)
    ap.add_argument("--frames", default="0")
    ap.add_argument("--view", default="side")
    ap.add_argument("--riding", action="store_true",
                    help="also require feet on pedals (< 0.12) and hands near the bars (< 0.25), medians")
    a = ap.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    bpy.ops.import_scene.gltf(filepath=a.glb)
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    bars = next((o for o in meshes if o.parent is not None and o.parent.name.split(".")[0] == "Handlebars"), None)
    pedals = [find("LeftPedal"), find("RightPedal")]
    wheel = find("BackWheel")
    acts = [x for x in bpy.data.actions]
    f0, f1 = 0, 0
    for o in bpy.data.objects:
        ad = o.animation_data
        if ad and ad.action:
            f0 = min(f0, int(ad.action.frame_range[0]))
            f1 = max(f1, int(ad.action.frame_range[1]))
    feet, hands, worst, ok = [], [], 0.0, True
    ref = None
    wheel_rot = []
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        lo, hi, finite = Vector((1e9,) * 3), Vector((-1e9,) * 3), True
        for o in meshes:
            for p in mesh_points(o):
                if not all(math.isfinite(c) for c in p):
                    finite = False
                    continue
                lo = Vector(map(min, lo, p))
                hi = Vector(map(max, hi, p))
        diag = (hi - lo).length
        ref = ref or diag
        worst = max(worst, diag / ref)
        if not finite or diag / ref > 2.0:
            ok = False
            print(f"  frame {f}: finite={finite} ratio={diag / ref:.2f}")
        for bone, ped in zip(("LeftFoot", "RightFoot"), pedals):
            h = bone_head(arm, bone)
            if h is not None and ped is not None:
                feet.append((h - ped.matrix_world.translation).length)
        if bars is not None:
            bp = mesh_points(bars)
            for bone in ("LeftHand", "RightHand"):
                h = bone_head(arm, bone)
                if h is not None:
                    hands.append(min((h - q).length for q in bp))
        if wheel is not None:
            wheel_rot.append(wheel.matrix_local.to_quaternion())
    turned = 0.0
    for q0, q1 in zip(wheel_rot, wheel_rot[1:]):
        turned += math.degrees(q0.rotation_difference(q1).angle)
    fm = st.median(feet) if feet else float("nan")
    hm = st.median(hands) if hands else float("nan")
    print(f"frames {f0}-{f1} actions {len(acts)} worst bbox ratio {worst:.2f}")
    print(f"feet->pedals median {fm:.3f} max {max(feet) if feet else float('nan'):.3f}")
    print(f"hands->bars median {hm:.3f}")
    print(f"back wheel turned {turned:.0f} deg over the clip")
    if a.riding and not (fm < 0.12 and hm < 0.25):
        ok = False
    if a.render:
        d = Vector({"side": (1.0, 0.0, 0.05), "front34": (0.6, -1.0, 0.3), "front": (0.0, -1.0, 0.05)}[a.view]).normalized()
        cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        sc.collection.objects.link(cam)
        cam.data.type = "ORTHO"
        cam.data.ortho_scale = 2.6
        cam.location = Vector((0, 0, 0.45)) + d * 8
        cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
        sc.camera = cam
        sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
        sun.data.energy = 3.0
        sun.rotation_euler = (math.radians(50), 0, math.radians(30))
        sc.collection.objects.link(sun)
        sc.world = bpy.data.worlds.new("w")
        sc.world.color = (0.2, 0.21, 0.24)
        sc.render.engine = "BLENDER_EEVEE"
        sc.render.resolution_x = sc.render.resolution_y = 480
        sc.render.image_settings.file_format = "PNG"
        sc.view_settings.view_transform = "Standard"
        for f in (int(x) for x in a.frames.split(",")):
            sc.frame_set(f)
            sc.render.filepath = os.path.abspath(f"{a.render}_f{f:03d}.png")
            bpy.ops.render.render(write_still=True)
            print("WROTE", sc.render.filepath)
    print("COMBO CHECK OK" if ok else "COMBO CHECK FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    rc = main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.stdout.flush()
    os._exit(rc)

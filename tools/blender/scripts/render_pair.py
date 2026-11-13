"""Render an animated rider .glb on an animated bike .glb (arm-pose check).

    blender -b --factory-startup --python tools/blender/scripts/render_pair.py -- rider.glb bike.glb out_prefix \
        --offsets offsets.json [--frames 0,8,16] [--view side|front34|front]

offsets.json: {"<frame>": [x, y, z], ...} = the rider root offset in glTF/bike space for that frame
(e.g. the feet-on-pedals placement computed by dmbmx.formats.motion.rider_on_bike).
"""
import argparse
import json
import math
import os
import sys

import bpy
from mathutils import Vector


def imp(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    return new, [o for o in new if o.parent is None]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("rider")
    ap.add_argument("bike")
    ap.add_argument("out")
    ap.add_argument("--offsets", required=True)
    ap.add_argument("--frames", default="0")
    ap.add_argument("--view", default="side")
    a = ap.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    robjs, rroots = imp(a.rider)
    bobjs, broots = imp(a.bike)
    offs = json.load(open(a.offsets))
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 2.6
    d = Vector({"side": (1.0, 0.0, 0.05), "front34": (0.6, -1.0, 0.3), "front": (0.0, -1.0, 0.05)}[a.view]).normalized()
    center = Vector((0, 0, 0.45))
    cam.location = center + d * 8
    cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    sc.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    sc.collection.objects.link(sun)
    world = bpy.data.worlds.new("w")
    sc.world = world
    world.color = (0.2, 0.21, 0.24)
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x = sc.render.resolution_y = 640
    sc.render.image_settings.file_format = "PNG"
    sc.view_settings.view_transform = "Standard"
    for f in (int(x) for x in a.frames.split(",")):
        sc.frame_set(f)
        o = offs.get(str(f), [0, 0, 0])
        for r in rroots:
            r.location = Vector((o[0], -o[2], o[1]))     # glTF (x, y, z) -> Blender (x, -z, y)
        bpy.context.view_layer.update()
        sc.render.filepath = os.path.abspath(f"{a.out}_f{f:03d}.png")
        bpy.ops.render.render(write_still=True)
        print("WROTE", sc.render.filepath)
    return 0


if __name__ == "__main__":
    rc = main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.stdout.flush()
    os._exit(rc)

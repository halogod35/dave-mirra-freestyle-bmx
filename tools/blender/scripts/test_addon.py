"""Headless tests of the Blender add-on (DM-022). Not part of the default pytest run.

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
        --python tools/blender/scripts/test_addon.py -- [--zip out/io_dmbmx.zip] [--only DMbike01,DaveMirra1] [--no-corpus]

1. Round-trip: for every ZMD in the install, File > Import then File > Export (the real operators) must
   give a byte-identical file.
2. Edit scenarios on DMbike01 / DaveMirra1 / ABBike01, each checked by reading the result with dmbmx.
With --zip the add-on is loaded from the built zip (vendored dmbmx), otherwise from tools/blender.
Exit code 0 = all passed.
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
import traceback
import zipfile
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

REPO = Path(__file__).resolve().parents[3]
GAME = Path(os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
TMP = Path(tempfile.mkdtemp(prefix="dmbmx_addon_"))


def setup(zip_path):
    if zip_path:
        d = TMP / "addon" / "io_dmbmx"
        d.mkdir(parents=True)
        zipfile.ZipFile(zip_path).extractall(d)
        sys.path.insert(0, str(d.parent))
    else:
        sys.path.insert(0, str(REPO / "tools" / "blender"))
        sys.path.insert(0, str(REPO / "tools"))
    import io_dmbmx
    io_dmbmx.register()
    import dmbmx
    print("add-on from", Path(io_dmbmx.__file__).parent, "| dmbmx from", Path(dmbmx.__file__).parent)
    return io_dmbmx


def clear():
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.armatures, bpy.data.materials, bpy.data.images,
                 bpy.data.collections):
        bpy.data.batch_remove(list(coll))


def corpus():
    out = []
    for d, dirs, files in os.walk(GAME):
        if Path(d) == GAME:
            dirs[:] = [x for x in dirs if x not in {"_pristine", "_testgame", "wt", "dave-mirra-bmx", "dave-mirra-pro-bmx"}]
        out += [Path(d) / f for f in files if f.lower().endswith(".zmd")]
    return sorted(out)


def find(stem):
    return next(p for p in corpus() if p.stem.lower() == stem.lower())


def do_import(path, **kw):
    clear()
    r = bpy.ops.import_scene.dmbmx_zmd(filepath=str(path), **kw)
    assert r == {"FINISHED"}, r
    return bpy.context.view_layer.objects.active


def do_export(name, **kw):
    out = TMP / f"{name}.zmd"
    r = bpy.ops.export_scene.dmbmx_zmd(filepath=str(out), **kw)
    assert r == {"FINISHED"}, r
    return out.read_bytes()


def obj(root, name):
    return next(o for o in root.children_recursive if o.name == name or o.name.startswith(name + "_mesh"))


def mesh_of_frame(root, frame):
    return next(o for o in root.children_recursive if o.type == "MESH" and o.parent and o.parent.name == frame)


# ------------------------------------------------------------------------------------------ tests

def roundtrip_all(only):
    files = [p for p in corpus() if not only or p.stem.lower() in only]
    ok = skipped = 0
    fails = []
    for p in files:
        data = p.read_bytes()
        if not data:
            clear()
            try:
                bpy.ops.import_scene.dmbmx_zmd(filepath=str(p))
                fails.append(f"{p.name}: empty file was not refused")
            except RuntimeError as e:          # operator reported ERROR (shown to the user)
                assert "empty" in str(e), e
            skipped += 1
            continue
        try:
            do_import(p)
            out = do_export(p.stem)
            if out == data:
                ok += 1
            else:
                i = next((k for k in range(min(len(out), len(data))) if out[k] != data[k]), min(len(out), len(data)))
                fails.append(f"{p.name}: differs at 0x{i:x} ({len(out)} vs {len(data)} bytes)")
        except Exception as e:
            fails.append(f"{p.name}: {type(e).__name__}: {e}")
            traceback.print_exc()
    print(f"ROUNDTRIP {ok}/{len(files) - skipped} byte-identical, {skipped} empty refused, {len(fails)} failed")
    for f in fails:
        print("  FAIL", f)
    return not fails


def _read(b):
    from dmbmx.formats import zmd
    return zmd.read(b)


def _geo(m, ai):
    lo = m.lods[0]
    return lo.clump.atomics[ai].geometry.to_chunk(lo.rw_version).to_bytes()


def t_bike_wheel_scale():
    src = find("DMbike01")
    root = do_import(src)
    wheel = mesh_of_frame(root, "FrontWheel")
    for v in wheel.data.vertices:
        v.co = v.co * 1.5
    a, b = _read(src.read_bytes()), _read(do_export("wheel"))
    sk = a.skeleton()
    fw = next(i for i, at in enumerate(a.lods[0].clump.atomics) if sk[at.frame]["name"] == "FrontWheel")
    ga, gb = a.lods[0].clump.atomics[fw].geometry, b.lods[0].clump.atomics[fw].geometry
    for p, q in zip(ga.morph_targets[0].vertices, gb.morph_targets[0].vertices):
        assert all(abs(q[k] - p[k] * 1.5) < 1e-5 for k in range(3))
    assert ga.triangles == gb.triangles and [c.to_bytes() for c in ga.ext] == [c.to_bytes() for c in gb.ext]
    assert ga.uv_sets == gb.uv_sets and ga.morph_targets[0].normals == gb.morph_targets[0].normals
    assert all(_geo(a, i) == _geo(b, i) for i in range(7) if i != fw)
    assert [t.image for t in a.materials()] == [t.image for t in b.materials()]


def t_bike_frame_move():
    src = find("DMbike01")
    root = do_import(src)
    fw = next(o for o in root.children_recursive if o.name == "FrontWheel" and o.type == "EMPTY")
    fw.location.x += 0.1
    a, b = _read(src.read_bytes()), _read(do_export("frame"))
    k = fw["dmbmx_frame"]
    pa, pb = a.lods[0].clump.frames[k].pos, b.lods[0].clump.frames[k].pos
    assert abs(pb[0] - pa[0] - 0.1) < 1e-5 and abs(pb[1] - pa[1]) < 1e-5 and abs(pb[2] - pa[2]) < 1e-5
    others = [i for i in range(len(a.lods[0].clump.frames)) if i != k]
    assert all(a.lods[0].clump.frames[i].pos == b.lods[0].clump.frames[i].pos for i in others)
    assert all(_geo(a, i) == _geo(b, i) for i in range(7))


def t_bike_delete_faces():
    src = find("DMbike01")
    root = do_import(src)
    deck = mesh_of_frame(root, "Hipsdeck")
    bpy.context.view_layer.objects.active = deck
    bpy.ops.object.mode_set(mode="EDIT")
    bm = bmesh.from_edit_mesh(deck.data)
    bm.faces.ensure_lookup_table()
    n0 = len(bm.faces)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index % 3 == 0], context="FACES_ONLY")
    bmesh.update_edit_mesh(deck.data)
    bpy.ops.object.mode_set(mode="OBJECT")
    n1 = len(deck.data.polygons)
    b = _read(do_export("faces"))
    g = b.lods[0].clump.atomics[6].geometry
    assert len(g.triangles) == n1 < n0 and g.bin_mesh.flags == 1


def t_rider_edit_mode_noop():
    src = find("DaveMirra1")
    root = do_import(src)
    body = next(o for o in root.children_recursive if o.type == "MESH")
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.object.mode_set(mode="OBJECT")
    assert do_export("noop") == src.read_bytes(), "entering/leaving Edit Mode must not change the export"


def t_rider_move_head():
    src = find("DaveMirra1")
    root = do_import(src)
    body = next(o for o in root.children_recursive if o.type == "MESH")
    gi = body.vertex_groups["Head"].index
    moved = [v.index for v in body.data.vertices if any(g.group == gi for g in v.groups)]
    for i in moved:
        body.data.vertices[i].co.z += 0.05          # RW model space: rider up = +Z
    a, b = _read(src.read_bytes()), _read(do_export("head"))
    va, vb = a.lods[0].clump.atomics[0].geometry, b.lods[0].clump.atomics[0].geometry
    pa, pb = va.morph_targets[0].vertices, vb.morph_targets[0].vertices
    ms = set(moved)
    assert all(abs(pb[i][2] - pa[i][2] - 0.05) < 1e-5 for i in ms)
    assert all(pa[i] == pb[i] for i in range(len(pa)) if i not in ms)
    assert va.bones == vb.bones and va.triangles == vb.triangles
    fa = [(f.right, f.up, f.at, f.pos) for f in a.lods[0].clump.frames]
    fb = [(f.right, f.up, f.at, f.pos) for f in b.lods[0].clump.frames]
    assert fa == fb and a.materials()[0].image == b.materials()[0].image


def t_rider_reweight():
    src = find("DaveMirra1")
    root = do_import(src)
    body = next(o for o in root.children_recursive if o.type == "MESH")
    hand, head = body.vertex_groups["LeftHand"], body.vertex_groups["Head"]
    vs = [v.index for v in body.data.vertices if any(g.group == hand.index for g in v.groups)][:5]
    hand.remove(vs)
    head.add(vs, 1.0, "REPLACE")
    b = _read(do_export("reweight"))
    ms = next(b.meshes())
    sk = b.skeleton()
    head_f = next(x["index"] for x in sk if x["name"] == "Head")
    import collections
    arm = body.parent
    frame_of_group = {vg.index: arm.data.bones[vg.name]["dmbmx_frame"] for vg in body.vertex_groups}
    want = collections.Counter((tuple(v.co), frame_of_group[max(v.groups, key=lambda g: g.weight).group])
                               for v in body.data.vertices)
    got = collections.Counter((tuple(p), j[0]) for p, j in zip(ms["positions"], ms["joints"]))
    assert got == want
    assert sum(1 for v in vs if frame_of_group[max(body.data.vertices[v].groups, key=lambda g: g.weight).group] == head_f) == 5
    g = b.lods[0].clump.atomics[0].geometry
    assert sum(e[1] for e in g.bones) == g.num_vertices


def t_texture_replace(io_dmbmx):
    from dmbmx.convert import imaging
    from dmbmx.formats import texture
    src = find("DMbike01")
    root = do_import(src)
    png = TMP / "red.png"
    png.write_bytes(imaging.png_encode(32, 32, [bytes((200, 0, 0)) * 32] * 32, color_type=2))   # wrong size
    img = next(i for i in bpy.data.images if i.name.endswith("NAME_431_0"))
    r = bpy.ops.dmbmx.replace_texture(filepath=str(png), image=img.name, resize=True)
    assert r == {"FINISHED"}, r
    b = _read(do_export("tex"))
    t = b.texture("NAME_431_0")
    im = texture.decode(t.image)
    assert t.kind == "bmp" and (im.width, im.height) == (64, 64)
    assert im.rgba[:4] == bytes((200, 0, 0, 255))
    a = _read(src.read_bytes())
    assert all(_geo(a, i) == _geo(b, i) for i in range(7))


def t_texture_paint_pixels():
    """Painting in Blender (pixels changed, image dirty) is picked up; palettised -> 24-bit."""
    from dmbmx.convert import imaging
    src = find("ABBike01")
    root = do_import(src)
    a = _read(src.read_bytes())
    pal = next(t for t in a.materials() if t.kind == "bmp" and imaging.bmp_info(t.image).bpp <= 8)
    img = next(i for i in bpy.data.images if i.name.endswith(pal.name))
    px = list(img.pixels)
    px[0:4] = [0.0, 1.0, 0.0, 1.0]
    img.pixels = px
    b = _read(do_export("paint"))
    t = b.texture(pal.name)
    assert imaging.bmp_info(t.image).bpp == 24


def t_save_reopen():
    src = find("DaveMirra1")
    do_import(src)
    blend = TMP / "rider.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    bpy.ops.wm.open_mainfile(filepath=str(blend))
    root = next(o for o in bpy.context.scene.objects if "dmbmx_gltf" in o)
    bpy.context.view_layer.objects.active = root
    assert do_export("reopen") == src.read_bytes()


def t_refuse_install_path():
    src = find("DMbike01")
    do_import(src)
    try:
        bpy.ops.export_scene.dmbmx_zmd(filepath=str(GAME / "assets" / "dmbmx_should_not_exist.zmd"))
        raise AssertionError("export into the install was not refused")
    except RuntimeError as e:
        assert "refusing" in str(e), e
    assert not (GAME / "assets" / "dmbmx_should_not_exist.zmd").exists()


def t_copy_to_testgame():
    """Unedited export with 'Copy into _testgame': the test copy gets identical bytes (stays pristine)."""
    src = find("DMbike01")
    do_import(src)
    do_export("copy", copy_to_testgame=True)
    assert (GAME / "_testgame" / "assets" / src.name).read_bytes() == src.read_bytes()


def t_upright():
    root = do_import(find("DaveMirra1"))
    bpy.context.view_layer.update()
    arm = next(o for o in root.children_recursive if o.type == "ARMATURE")
    head = arm.matrix_world @ arm.data.bones["Head"].head_local
    foot = arm.matrix_world @ arm.data.bones["LeftFoot"].head_local
    assert head.z - foot.z > 1.0, (head, foot)
    root = do_import(find("DMbike01"))
    bpy.context.view_layer.update()
    seat = next(o for o in root.children_recursive if o.name == "SeatGrip").matrix_world.translation
    back = next(o for o in root.children_recursive if o.name == "BackWheel").matrix_world.translation
    assert seat.z > back.z + 0.3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip")
    ap.add_argument("--only", default="")
    ap.add_argument("--no-corpus", action="store_true")
    a = ap.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    io_dmbmx = setup(a.zip)
    ok = True
    if not a.no_corpus:
        ok &= roundtrip_all({s.strip().lower() for s in a.only.split(",") if s.strip()})
    tests = [t_upright, t_bike_wheel_scale, t_bike_frame_move, t_bike_delete_faces, t_rider_edit_mode_noop,
             t_rider_move_head, t_rider_reweight, lambda: t_texture_replace(io_dmbmx), t_texture_paint_pixels,
             t_refuse_install_path, t_copy_to_testgame, t_save_reopen]
    names = ["upright", "bike_wheel_scale", "bike_frame_move", "bike_delete_faces", "rider_edit_mode_noop",
             "rider_move_head", "rider_reweight", "texture_replace", "texture_paint_pixels", "refuse_install_path",
             "copy_to_testgame", "save_reopen"]
    for n, t in zip(names, tests):
        try:
            t()
            print(f"PASS {n}")
        except Exception as e:
            ok = False
            print(f"FAIL {n}: {type(e).__name__}: {e}")
            traceback.print_exc()
    print("ALL PASSED" if ok else "SOME FAILED")
    shutil.rmtree(TMP, ignore_errors=True)
    sys.stdout.flush()
    os._exit(0 if ok else 1)


if __name__ == "__main__":
    main()

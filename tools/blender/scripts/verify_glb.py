"""Headless Blender check of `dmbmx export` output. Not part of the pytest run.

Imports every .glb in a folder into Blender and compares it with what dmbmx reads from the
source ZMD (vertex/triangle counts, bone counts, frame empties, upright orientation, winding).

    "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
        --python tools/blender/scripts/verify_glb.py -- <glb_dir> [--game-dir DIR] [--json out.json]

dmbmx is imported from this checkout (it is pure stdlib, so Blender's Python can run it).
Exit code 0 = every file matches.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import bpy

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
from dmbmx.formats import zmd  # noqa: E402


def expected(path):
    data = Path(path).read_bytes()
    m = zmd.read(data)
    e = {"vertices": 0, "vertices_split": 0, "triangles": 0, "bones": 0, "frames_unskinned": 0, "lods": len(m.lods),
         "skinned": False, "duplicate_tris": 0}
    for li in range(len(m.lods)):
        meshes = list(m.meshes(li))
        skinned = any(ms["joints"] for ms in meshes)
        e["skinned"] |= skinned
        nfr = len(m.lods[li].clump.frames)
        if skinned:
            e["bones"] += nfr
        else:
            e["frames_unskinned"] += nfr
        for ms in meshes:
            e["vertices"] += len(ms["positions"])
            e["triangles"] += len(ms["material_ids"])
            # glTF primitives are per material; Blender's importer duplicates a vertex for each
            # material it is used by and drops vertices no triangle references.
            # Blender also cannot store two faces on the same vertex set, so duplicate triangles
            # (bike_hires.zmd has 96, double-sided faces) collapse into one.
            pairs, faces = set(), set()
            ix = ms["indices"]
            for t, mid in enumerate(ms["material_ids"]):
                tri = [(mid, ix[3 * t + k]) for k in range(3)]
                pairs.update(tri)
                key = frozenset(tri)
                e["duplicate_tris"] += key in faces
                faces.add(key)
            e["vertices_split"] += len(pairs)
    return e


def measure(glb):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(glb))
    deps = bpy.context.evaluated_depsgraph_get()
    r = {"vertices": 0, "triangles": 0, "polys_non_tri": 0, "bones": 0, "armatures": 0, "empties": 0,
         "meshes": 0, "winding_ok": 0, "winding_total": 0, "zmin": None, "zmax": None, "textured_mats": 0}
    for ob in bpy.context.scene.objects:
        if any(c.name.startswith("glTF_not_exported") for c in ob.users_collection):
            continue  # the importer's bone-shape helper (Icosphere), not model data
        if ob.type == "MESH":
            me = ob.data
            r["meshes"] += 1
            r["vertices"] += len(me.vertices)
            r["triangles"] += sum(1 for p in me.polygons if p.loop_total == 3)
            r["polys_non_tri"] += sum(1 for p in me.polygons if p.loop_total != 3)
            cn = me.corner_normals
            for p in me.polygons:
                avg = [0.0, 0.0, 0.0]
                for li in p.loop_indices:
                    v = cn[li].vector
                    avg = [avg[0] + v[0], avg[1] + v[1], avg[2] + v[2]]
                d = sum(p.normal[k] * avg[k] for k in range(3))
                r["winding_total"] += 1
                r["winding_ok"] += d > 0
            ev = ob.evaluated_get(deps)
            em = ev.to_mesh()
            for v in em.vertices:
                z = (ev.matrix_world @ v.co).z
                r["zmin"] = z if r["zmin"] is None else min(r["zmin"], z)
                r["zmax"] = z if r["zmax"] is None else max(r["zmax"], z)
            ev.to_mesh_clear()
            for slot in ob.material_slots:
                mat = slot.material
                if mat and mat.use_nodes and any(n.type == "TEX_IMAGE" and n.image for n in mat.node_tree.nodes):
                    r["textured_mats"] += 1
        elif ob.type == "ARMATURE":
            r["armatures"] += 1
            r["bones"] += len(ob.data.bones)
            pb = {b.name: (ob.matrix_world @ b.head_local) for b in ob.data.bones}
            if "Head" in pb and "LeftFoot" in pb:
                r["head_z"], r["foot_z"] = pb["Head"].z, pb["LeftFoot"].z
        elif ob.type == "EMPTY":
            r["empties"] += 1
    return r


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("glb_dir")
    ap.add_argument("--game-dir", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--json", default=None)
    a = ap.parse_args(argv)
    src = {}
    for d, dirs, files in os.walk(a.game_dir):
        if Path(d) == Path(a.game_dir):
            dirs[:] = [x for x in dirs if x not in {"_pristine", "_testgame", "wt", "dave-mirra-bmx", "dave-mirra-freestyle-bmx"}]
        for f in files:
            if f.lower().endswith(".zmd"):
                src[Path(f).stem.lower()] = Path(d) / f
    results, fails = {}, 0
    glbs = sorted(Path(a.glb_dir).glob("*.glb"))
    for g in glbs:
        e = expected(src[g.stem.lower()])
        r = measure(g)
        problems = []
        if r["triangles"] not in (e["triangles"], e["triangles"] - e["duplicate_tris"]) or r["polys_non_tri"]:
            problems.append(f"triangles {r['triangles']} (+{r['polys_non_tri']} non-tri) != {e['triangles']}")
        if r["vertices"] not in (e["vertices"], e["vertices_split"]):
            problems.append(f"vertices {r['vertices']} != {e['vertices']} / split {e['vertices_split']}")
        if r["bones"] != e["bones"]:
            problems.append(f"bones {r['bones']} != {e['bones']}")
        # model root -> empty; LOD nodes -> empties, except that the importer turns the LOD node
        # of a skinned LOD into the armature object; unskinned frames -> empties
        if e["skinned"]:
            want_empties = 1
        else:
            want_empties = 1 + e["lods"] + e["frames_unskinned"]
        if r["empties"] != want_empties:
            problems.append(f"empties {r['empties']} != {want_empties}")
        if "head_z" in r and not r["head_z"] > r["foot_z"] + 1.0:
            problems.append(f"not upright: head z {r['head_z']:.3f}, foot z {r['foot_z']:.3f}")
        if r["winding_total"] and r["winding_ok"] / r["winding_total"] < 0.7:
            problems.append(f"winding: only {r['winding_ok']}/{r['winding_total']} faces agree with normals")
        status = "OK" if not problems else "FAIL"
        fails += bool(problems)
        results[g.name] = {"expected": e, "blender": r, "problems": problems}
        vmode = "exact" if r["vertices"] == e["vertices"] else "split"
        if r["triangles"] != e["triangles"]:
            problems_note = f" [{e['duplicate_tris']} duplicate source triangles merged by Blender]"
        else:
            problems_note = ""
        print(f"{status:4s} {g.name:<26s} v={r['vertices']:5d}({vmode}) t={r['triangles']:5d} "
              f"bones={r['bones']:2d} empties={r['empties']:2d} z=[{r['zmin']:.2f},{r['zmax']:.2f}] "
              f"wind={r['winding_ok']}/{r['winding_total']} tex={r['textured_mats']} {'; '.join(problems)}{problems_note}")
    print(f"SUMMARY {len(glbs) - fails}/{len(glbs)} OK, {fails} FAIL")
    if a.json:
        Path(a.json).write_text(json.dumps(results, indent=1))
    return 1 if fails else 0


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    rc = main(argv)
    sys.stdout.flush()
    os._exit(rc)

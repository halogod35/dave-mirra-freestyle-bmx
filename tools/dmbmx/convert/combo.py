"""Rider + bike in one animated glTF (DM-018). CLI: ``dmbmx export-combo``.

Both models are exported with DM-020's exporter, given their motion tracks by DM-016/017's
:mod:`dmbmx.convert.anim_gltf`, then merged into one .glb:

    scene
    └─ <rider>+<bike>            combo root (extras.dmbmx.combo)
       ├─ <BIKE root>            bike frames + meshes (bike channels of each clip)
       └─ <RIDER root>           rider armature + skinned mesh (rider channels of each clip)
                                 + a translation track: the rider is placed on the bike every frame

Placement: per frame, the rider root is translated so that the mid-point of its feet frames sits on
the mid-point of the bike's pedal frames (DM-017, ``tools/research/arm_pose_check.py``). [V] median foot-to-pedal
error 0.068. Frames where the feet leave the pedals (tricks) follow the game's own placement (DM-047,
:func:`engine_offsets`): the clip's bike-section translation b is the bike's offset from the rider in
engine (ZGE) units, 100 x model units with Y and Z negated, so the rider root sits at F(-b) / 100 from
the bike root plus a constant, calibrated per clip on its on-pedal frames ([V] median 0.023 over all
clips; ENGINE_OFFSET when a clip has no such frame). ``fill="interpolate"`` keeps DM-018's linear
interpolation between on-pedal frames.

Wheels: the clips have no wheel channels. With ``spin_wheels`` the BackWheel/FrontWheel frames get a
rotation track derived from the Sprocket channel (crank angle x gear ratio, default 44/16 = 2.75 [I]),
about the bike's lateral axis, so the wheels turn when the rider pedals.
"""
from __future__ import annotations

import copy
import json
import math
import struct

from dmbmx.convert import anim_gltf, gltf
from dmbmx.convert.gltf import EXTRAS_KEY, read_glb
from dmbmx.formats import motion as M

__all__ = ["export_combo", "rider_offsets", "merge_glbs", "GEAR_RATIO"]

GEAR_RATIO = 44 / 16          # [I] typical BMX gearing; the game's value is unknown
_INDEXED = {  # top-level array -> fields that hold indices into other arrays
    "accessors": [("bufferView", "bufferViews")],
    "images": [("bufferView", "bufferViews")],
    "textures": [("sampler", "samplers"), ("source", "images")],
    "skins": [("inverseBindMatrices", "accessors"), ("skeleton", "nodes")],
    "nodes": [("mesh", "meshes"), ("skin", "skins")],
}


ENGINE_SCALE = 0.01                # [V] ZGE (engine) units = 100 x RW/model units (LEVEL.md, zrwZgl(1, 100.0))
ENGINE_OFFSET = (0.007, -0.251, 0.044)  # [V] fit: DaveMirra1 on DMbike01, median over 1,246 on-pedal frames
DEFAULT_OFFSET = (0.0, 0.6, 0.0)   # [V] typical feet-on-pedals placement (rider root ~0.6 above Hipsdeck)
FOOT_TOLERANCE = 0.15              # a frame whose feet are farther than this from the pedals is "off the bike"
                                   # (DM-019: with the engine F q F the feet sit 0.09-0.12 from the pedal frames
                                   # while pedalling, the feet ~0.37 apart vs pedals ~0.2; was 0.12 under DM-017)


def engine_offsets(motion, offset=ENGINE_OFFSET):
    """Per frame: the rider root relative to the bike root as the game places them (DM-047): the bike
    section's translation b (ZGE units; physics places the rider actor at bike - b, zactorGetBikeOffset 0x00474DE0) in
    model space, F(-b) * ENGINE_SCALE with F = diag(1, -1, -1), plus a constant ``offset`` (the rider
    model's root vs the actor origin)."""
    out = []
    for p in motion.sample():
        b = p.bike_trans or (0.0, 0.0, 0.0)
        e = (-b[0] * ENGINE_SCALE, b[1] * ENGINE_SCALE, b[2] * ENGINE_SCALE)
        out.append(tuple(a + c for a, c in zip(e, offset)))
    return out


def rider_offsets(rider_skel, bike_skel, motion, info=None, fill="engine"):
    """Per frame: translation (glTF/model space) of the rider root relative to the bike root.

    Feet-on-pedals placement: the mid-point of the feet goes onto the mid-point of the pedals. In frames
    where a foot is still more than FOOT_TOLERANCE from its pedal after that (tricks: no-footers,
    tailwhips), ``fill="engine"`` uses :func:`engine_offsets` with the constant calibrated on the clip's
    good frames (ENGINE_OFFSET if there are none); ``fill="interpolate"`` interpolates between the
    nearest good frames (held at the ends; the default offset if no frame is good). ``info`` (a dict)
    receives ``good`` (list of bools), ``residual`` and, for "engine", ``engine_offset``."""
    raw, good, resid = [], [], []
    for p in motion.sample():
        R = M.pose_positions(rider_skel, M.rider_rotations(p))
        B = M.pose_positions(bike_skel, M.bike_rotations(p))
        fm = [(a + b) / 2 for a, b in zip(R["LeftFoot"], R["RightFoot"])]
        pm = [(a + b) / 2 for a, b in zip(B["LeftPedal"], B["RightPedal"])]
        t = tuple(b - a for a, b in zip(fm, pm))
        e = max(math.dist([a + c for a, c in zip(R[f], t)], B[pd])
                for f, pd in (("LeftFoot", "LeftPedal"), ("RightFoot", "RightPedal")))
        raw.append(t)
        resid.append(e)
        good.append(e <= FOOT_TOLERANCE)
    idx = [i for i, g in enumerate(good) if g]
    if fill == "engine":
        eng = engine_offsets(motion, (0.0, 0.0, 0.0))
        if idx:
            c = tuple(sorted(raw[i][a] - eng[i][a] for i in idx)[len(idx) // 2] for a in range(3))
        else:
            c = ENGINE_OFFSET
        if info is not None:
            info["good"], info["residual"], info["engine_offset"] = good, resid, c
        return [raw[i] if good[i] else tuple(e + o for e, o in zip(eng[i], c)) for i in range(len(raw))]
    if fill != "interpolate":
        raise ValueError(f"fill must be 'engine' or 'interpolate', not {fill!r}")
    out = []
    for i, t in enumerate(raw):
        if good[i]:
            out.append(t)
        elif not idx:
            out.append(DEFAULT_OFFSET)
        else:
            lo = max((j for j in idx if j < i), default=None)
            hi = min((j for j in idx if j > i), default=None)
            if lo is None or hi is None:
                out.append(raw[lo if lo is not None else hi])
            else:
                f = (i - lo) / (hi - lo)
                out.append(tuple(a + (b - a) * f for a, b in zip(raw[lo], raw[hi])))
    if info is not None:
        info["good"], info["residual"] = good, resid
    return out


def _wheel_tracks(bike_skel, motion, ratio):
    """{wheel frame name: [local quaternion per frame]}: bind rotation followed by a spin about the bike's
    lateral (+X) axis, expressed in the wheel's parent frame."""
    poses = motion.sample()
    if not poses or not poses[0].bike or len(poses[0].bike) < 3:
        return {}
    crank, last, total = [], poses[0].bike[2][0], 0
    for p in poses:
        a = p.bike[2][0]
        total += M._s16(a - last)
        last = a
        crank.append(total)
    tracks = {}
    for b in bike_skel:
        if b["name"] not in ("BackWheel", "FrontWheel"):
            continue
        col = [b["matrix"][r][c] for c in range(4) for r in range(4)]
        _, qb, _ = anim_gltf.matrix_to_trs(col)
        qs = []
        for c in crank:
            ang = c * ratio * math.pi / 32768.0          # s16 binary angle -> radians
            spin = (math.sin(ang / 2), 0.0, 0.0, math.cos(ang / 2))
            qs.append(M.quat_mul(spin, qb))             # spin about the parent's X (bike lateral)
        tracks[b["name"]] = qs
    return tracks


def _offset_indices(js, base):
    """Shift every cross-reference in glTF JSON ``js`` by ``base`` (dict array -> offset)."""
    for arr, fields in _INDEXED.items():
        for item in js.get(arr, []):
            for f, target in fields:
                if f in item:
                    item[f] += base[target]
    for n in js.get("nodes", []):
        if "children" in n:
            n["children"] = [c + base["nodes"] for c in n["children"]]
    for s in js.get("skins", []):
        s["joints"] = [j + base["nodes"] for j in s["joints"]]
    for m in js.get("meshes", []):
        for pr in m["primitives"]:
            pr["attributes"] = {k: v + base["accessors"] for k, v in pr["attributes"].items()}
            if "indices" in pr:
                pr["indices"] += base["accessors"]
            if "material" in pr:
                pr["material"] += base["materials"]
    for mat in js.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})
        if "baseColorTexture" in pbr:
            pbr["baseColorTexture"]["index"] += base["textures"]
    for a in js.get("animations", []):
        for s in a["samplers"]:
            s["input"] += base["accessors"]
            s["output"] += base["accessors"]
        for c in a["channels"]:
            c["target"]["node"] += base["nodes"]


def merge_glbs(parts, root_name="combo", root_extras=None):
    """Merge .glb byte strings into one scene under a new root node. Animations with the same name are
    merged into one animation (their channels play together)."""
    arrays = ("buffers", "bufferViews", "accessors", "images", "samplers", "textures", "materials",
              "meshes", "skins", "nodes", "animations")
    out = {k: [] for k in arrays}
    out["asset"] = {"version": "2.0", "generator": "dmbmx combo (DM-018)"}
    out_bin = bytearray()
    roots = []
    used_ext = set()
    for data in parts:
        js, b = read_glb(data)
        js = copy.deepcopy(js)
        while len(out_bin) % 4:
            out_bin.append(0)
        for bv in js.get("bufferViews", []):
            bv["byteOffset"] = bv.get("byteOffset", 0) + len(out_bin)
            bv["buffer"] = 0
        base = {k: len(out[k]) for k in arrays}
        _offset_indices(js, base)
        out_bin += b
        roots += [n + base["nodes"] for n in js["scenes"][js.get("scene", 0)]["nodes"]]
        used_ext |= set(js.get("extensionsUsed", []))
        for k in arrays:
            if k in ("buffers",):
                continue
            if k == "animations":
                for a in js.get(k, []):
                    same = next((x for x in out["animations"] if x["name"] == a["name"]), None)
                    if same is None:
                        out["animations"].append(a)
                    else:
                        so = len(same["samplers"])
                        same["samplers"] += a["samplers"]
                        for c in a["channels"]:
                            c["sampler"] += so
                            same["channels"].append(c)
                continue
            out[k] += js.get(k, [])
    root = {"name": root_name, "children": roots}
    if root_extras:
        root["extras"] = {EXTRAS_KEY: root_extras}
    out["nodes"].append(root)
    out["scenes"] = [{"nodes": [len(out["nodes"]) - 1]}]
    out["scene"] = 0
    while len(out_bin) % 4:
        out_bin.append(0)
    out["buffers"] = [{"byteLength": len(out_bin)}]
    if used_ext:
        out["extensionsUsed"] = sorted(used_ext)
    out = {k: v for k, v in out.items() if v != []}
    j = json.dumps(out, separators=(",", ":")).encode()
    j += b" " * (-len(j) % 4)
    head = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(out_bin))
    return head + struct.pack("<II", len(j), 0x4E4F534A) + j + struct.pack("<II", len(out_bin), 0x004E4942) + bytes(out_bin)


def _add_tracks(glb, motions, node_tracks):
    """Add per-clip tracks: node_tracks(motion) -> list of (node name, path, values)."""
    from dmbmx.convert.glb_edit import GlbDoc
    doc = GlbDoc(glb)
    names = {}
    for i, n in enumerate(doc.js["nodes"]):
        names.setdefault(n.get("name"), []).append(i)
    anims = doc.js.setdefault("animations", [])
    for name, mo in motions:
        times = [k * mo.frame_ms / 1000.0 for k in range(mo.nframes)]
        tin = anim_gltf._accessor(doc, times, "SCALAR")
        a = next((x for x in anims if x["name"] == name), None)
        if a is None:
            a = {"name": name, "samplers": [], "channels": []}
            anims.append(a)
        for node_name, path, values in node_tracks(mo):
            if len(values) != len(times):
                continue
            acc = _vec_accessor(doc, values)
            for ni in names.get(node_name, []):
                n = doc.js["nodes"][ni]
                anim_gltf._to_trs(n)
                a["samplers"].append({"input": tin, "output": acc, "interpolation": "LINEAR"})
                a["channels"].append({"sampler": len(a["samplers"]) - 1, "target": {"node": ni, "path": path}})
    return doc.to_bytes()


def _vec_accessor(doc, values):
    n = len(values[0])
    flat = [c for v in values for c in v]
    bv = doc._view(struct.pack(f"<{len(flat)}f", *flat))
    doc.js["accessors"].append({"bufferView": bv, "componentType": 5126, "count": len(values),
                                "type": {3: "VEC3", 4: "VEC4"}[n]})
    return len(doc.js["accessors"]) - 1


def export_combo(rider_model, bike_model, motions, *, rider_source=None, bike_source=None,
                 rider_name="rider", bike_name="bike", spin_wheels=True, ratio=GEAR_RATIO):
    """``motions``: list of (name, Motion). Returns .glb bytes with one scene and one animation per clip."""
    rsk = rider_model.skeleton()
    bsk = bike_model.skeleton()
    rglb = gltf.export_glb(rider_model, source=rider_source, source_name=rider_name)
    bglb = gltf.export_glb(bike_model, source=bike_source, source_name=bike_name)
    if motions:
        rglb = anim_gltf.add_animations(rglb, motions, which="rider")
        bglb = anim_gltf.add_animations(bglb, motions, which="bike")
    # rider placement on the rider's model root node
    rjs, _ = read_glb(rglb)
    rroot = rjs["nodes"][rjs["scenes"][0]["nodes"][0]]["name"]
    rglb = _add_tracks(rglb, motions, lambda mo: [(rroot, "translation", rider_offsets(rsk, bsk, mo))])
    if spin_wheels:
        bglb = _add_tracks(bglb, motions,
                           lambda mo: [(n, "rotation", q) for n, q in _wheel_tracks(bsk, mo, ratio).items()])
    extras = {"format": "dmbmx-combo", "version": 1, "rider": rider_name, "bike": bike_name,
              "placement": "feet-on-pedals+engine", "wheels": {"spin": spin_wheels, "gear_ratio": ratio},
              "clips": [n for n, _ in motions]}
    return merge_glbs([bglb, rglb], root_name=f"{rider_name}+{bike_name}", root_extras=extras)

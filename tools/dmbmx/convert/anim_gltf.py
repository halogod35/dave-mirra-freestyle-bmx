"""Add motion.zal animations to a dmbmx .glb (DM-016). Used by ``dmbmx export --anims``.

For each motion, every frame node whose name is a motion channel (rider: Hips..RightFoot; bike:
Hipsdeck..SeatGrip) gets a LINEAR rotation track: the game's per-frame local rotation
(:func:`dmbmx.formats.motion.channel_rotation`: Euler -> quaternion, the arm table, and the engine ->
model conversion (x, -y, -z, w) of zrwModelQuatToRwMatrix 0x00487E70, DM-019),
which **replaces** the frame's bind rotation (NgIFzactor_004654b0 sets the frame quaternion; the
translation stays the bind translation). Animated nodes are converted from ``matrix`` to TRS.

The rider's Hips channel already stands the skeleton up in a Y-up world, so the DM-020 upright
root rotation (Rx(-90)) is removed when rider tracks are added (docs/formats/ANIM.md section 5).
Root motion (actor translation) is not exported: its units are world units, not model units.
"""
from __future__ import annotations

import math
import struct

from dmbmx.convert.gltf import EXTRAS_KEY
from dmbmx.convert.glb_edit import GlbDoc
from dmbmx.formats.motion import BIKE_CHANNELS, RIDER_CHANNELS, channel_rotation

__all__ = ["add_animations", "matrix_to_trs"]


def matrix_to_trs(m):
    """Column-major 4x4 (glTF ``matrix``) -> (translation, rotation xyzw, scale)."""
    c = [m[0:3], m[4:7], m[8:11]]
    t = tuple(m[12:15])
    s = [math.sqrt(sum(v * v for v in col)) or 1.0 for col in c]
    r = [[c[j][i] / s[j] for j in range(3)] for i in range(3)]      # r[row][col]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0:
        k = math.sqrt(tr + 1.0) * 2
        q = ((r[2][1] - r[1][2]) / k, (r[0][2] - r[2][0]) / k, (r[1][0] - r[0][1]) / k, 0.25 * k)
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        k = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        q = (0.25 * k, (r[0][1] + r[1][0]) / k, (r[0][2] + r[2][0]) / k, (r[2][1] - r[1][2]) / k)
    elif r[1][1] > r[2][2]:
        k = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        q = ((r[0][1] + r[1][0]) / k, 0.25 * k, (r[1][2] + r[2][1]) / k, (r[0][2] - r[2][0]) / k)
    else:
        k = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
        q = ((r[0][2] + r[2][0]) / k, (r[1][2] + r[2][1]) / k, 0.25 * k, (r[1][0] - r[0][1]) / k)
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return t, tuple(x / n for x in q), tuple(s)


def _channel_nodes(doc, names):
    """{channel index: [node indices]} for frame nodes named like a channel (all LODs)."""
    out = {}
    for i, n in enumerate(doc.js["nodes"]):
        ex = (n.get("extras") or {}).get(EXTRAS_KEY, {})
        if "record" in ex and n.get("name") in names:
            out.setdefault(names.index(n["name"]), []).append(i)
    return out


def _to_trs(node):
    if "matrix" in node:
        t, r, s = matrix_to_trs(node.pop("matrix"))
        node["translation"], node["rotation"] = list(t), list(r)
        if any(abs(x - 1.0) > 1e-6 for x in s):
            node["scale"] = list(s)


def add_animations(glb: bytes, motions, *, which="auto") -> bytes:
    """``motions``: list of (name, Motion). ``which``: 'rider' | 'bike' | 'auto' (by frame names).
    Returns the new .glb bytes (unchanged input if no channel node is found)."""
    doc = GlbDoc(glb)
    rider = _channel_nodes(doc, RIDER_CHANNELS)
    bike = _channel_nodes(doc, BIKE_CHANNELS)
    if which == "auto":
        which = "rider" if len(rider) >= len(bike) else "bike"
    names, nodes = (RIDER_CHANNELS, rider) if which == "rider" else (BIKE_CHANNELS, bike)
    if not nodes:
        return glb
    for idxs in nodes.values():
        for ni in idxs:
            _to_trs(doc.js["nodes"][ni])
    if which == "rider":
        root = doc.js["scenes"][doc.js.get("scene", 0)]["nodes"][0]
        rn = doc.js["nodes"][root]
        rn.pop("rotation", None)
        ex = (rn.get("extras") or {}).get(EXTRAS_KEY)
        if isinstance(ex, dict) and isinstance(ex.get("axis"), dict):
            ex["axis"]["root_correction_removed_for_animation"] = True
    anims = doc.js.setdefault("animations", [])
    for name, mo in motions:
        poses = mo.sample()
        times = [p.time_ms / 1000.0 for p in poses]
        tin = _accessor(doc, times, "SCALAR")
        samplers, channels = [], []
        for ci, idxs in sorted(nodes.items()):
            rots = []
            for p in poses:
                src = p.rider if which == "rider" else p.bike
                if src is None or ci >= len(src):
                    break
                rots.append(channel_rotation(names[ci], src[ci]))
            if len(rots) != len(poses):
                continue
            for k in range(1, len(rots)):                      # keep the shortest path for LINEAR
                if sum(a * b for a, b in zip(rots[k - 1], rots[k])) < 0:
                    rots[k] = tuple(-x for x in rots[k])
            out = _accessor(doc, rots, "VEC4")
            for ni in idxs:
                samplers.append({"input": tin, "output": out, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": ni, "path": "rotation"}})
        if channels:
            anims.append({"name": name, "samplers": samplers, "channels": channels,
                          "extras": {EXTRAS_KEY: {"motion_frames": mo.nframes, "frame_ms": mo.frame_ms,
                                                  "channels": which}}})
    return doc.to_bytes()


def _accessor(doc, values, typ):
    n = {"SCALAR": 1, "VEC4": 4}[typ]
    flat = [c for v in values for c in (v if n > 1 else (v,))]
    bv = doc._view(struct.pack(f"<{len(flat)}f", *flat))
    acc = {"bufferView": bv, "componentType": 5126, "count": len(values), "type": typ}
    if typ == "SCALAR":
        acc["min"], acc["max"] = [min(values)], [max(values)]
    doc.js["accessors"].append(acc)
    return len(doc.js["accessors"]) - 1

"""Hands-on-handlebars check: put the posed rider on the posed bike and measure the hands.

Usage:  tools/testing/py.sh tools/research/arm_pose_check.py [--motions 8,5,36] [--rider DaveMirra1.ZMD] [--bike DMbike01.zmd]
                                            [--offsets out/pose/offs8.json --offsets-motion 8]

For each frame: the bike is posed with the clip's bike section, the rider with its rider section.
The rider is placed so that the mid-point of its feet sits on the mid-point of the pedals (translation
only; both models share model units and axes). Reported for the current rotations (the engine's
F q F conjugation + NgIFzactor's literal arm table) and the legacy ones (Rx(180), no F):
  feet->pedals   distance of each foot frame to its pedal frame (checks the placement)
  hand->bar      distance of each hand frame to the nearest vertex of the Handlebars mesh
  hand->bar-end  distance of each hand to the centroid of that side's bar-end vertices (|x| > 0.22, top 0.2)
  hand->grip     distance to the LeftGrip/RightGrip frames (these are NOT on the bar ends in the motion
                 pose; listed for reference only)
--offsets writes {blender_frame: rider root offset} for tools/blender/scripts/render_pair.py.
"""
import argparse
import json
import math
import os
import statistics as st
from pathlib import Path

from dmbmx.formats import motion as M
from dmbmx.formats import zmd


def setup(game, rider_name, bike_name):
    a = Path(game) / "assets"
    rider = zmd.read((a / rider_name).read_bytes()).skeleton()
    bz = zmd.read((a / bike_name).read_bytes())
    bsk = bz.skeleton()
    bars = next(ms for ms in bz.meshes(0) if bsk[ms["frame"]]["name"] == "Handlebars")
    bank = M.read_bank((a / "motion.zal").read_bytes())
    return rider, bsk, bars, bank


def frame_metrics(rider, bsk, bars, pose, arm_fix=None, legacy=False, frf=None):
    W = M.pose_matrices(bsk, M.bike_rotations(pose, legacy=legacy, frf=frf))
    B = {b["name"]: [W[i][r][3] for r in range(3)] for i, b in enumerate(bsk)}
    Wm = W[bars["frame"]]
    pts = [[sum(Wm[r][c] * v[c] for c in range(3)) + Wm[r][3] for r in range(3)] for v in bars["positions"]]
    top = max(q[1] for q in pts)
    ends = []
    for sgn in (1, -1):
        e = [q for q in pts if sgn * q[0] > 0.22 and q[1] > top - 0.2]
        ends.append([sum(q[i] for q in e) / len(e) for i in range(3)] if e else None)
    R = M.pose_positions(rider, M.rider_rotations(pose, arm_fix, legacy=legacy, frf=frf))
    fm = [(a + b) / 2 for a, b in zip(R["LeftFoot"], R["RightFoot"])]
    pm = [(a + b) / 2 for a, b in zip(B["LeftPedal"], B["RightPedal"])]
    t = [b - a for a, b in zip(fm, pm)]
    at = lambda n: [a + b for a, b in zip(R[n], t)]
    return {
        "offset": t,
        "feet": [math.dist(at("LeftFoot"), B["LeftPedal"]), math.dist(at("RightFoot"), B["RightPedal"])],
        "bar": [min(math.dist(at(h), q) for q in pts) for h in ("LeftHand", "RightHand")],
        "barend": [math.dist(at(h), e) for h, e in zip(("LeftHand", "RightHand"), ends) if e is not None],
        "grip": [math.dist(at("LeftHand"), B["LeftGrip"]), math.dist(at("RightHand"), B["RightGrip"])],
        "gap": math.dist(R["LeftHand"], R["RightHand"]),
    }


def summarize(rider, bsk, bars, bank, motions, arm_fix=None, step=2, legacy=False, frf=None):
    acc = {"feet": [], "bar": [], "barend": [], "grip": [], "gap": []}
    for mid in motions:
        for p in bank.motion(mid).sample()[::step]:
            m = frame_metrics(rider, bsk, bars, p, arm_fix, legacy, frf)
            for k in ("feet", "bar", "barend", "grip"):
                acc[k] += m[k]
            acc["gap"].append(m["gap"])
    return {k: (st.median(v) if v else float("nan")) for k, v in acc.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--rider", default="DaveMirra1.ZMD")
    ap.add_argument("--bike", default="DMbike01.zmd")
    ap.add_argument("--motions", default="8,5,36,22,57,106")
    ap.add_argument("--offsets", default=None)
    ap.add_argument("--offsets-motion", type=int, default=8)
    a = ap.parse_args()
    rider, bsk, bars, bank = setup(a.game, a.rider, a.bike)
    mids = [int(x) for x in a.motions.split(",")]
    for label, kw in (("F q F, literal", {}), ("legacy (Rx180)", {"legacy": True}),
                      ("literal, no F", {"frf": False})):
        s = summarize(rider, bsk, bars, bank, mids, **kw)
        print(f"{label:24s} median feet->pedals {s['feet']:.3f}  hand->bar {s['bar']:.3f}  hand->bar-end {s['barend']:.3f}  "
              f"hand->grip-frame {s['grip']:.3f}  hand gap {s['gap']:.3f}")
    if a.offsets:
        mo = bank.motion(a.offsets_motion)
        out = {}
        for k, p in enumerate(mo.sample()):
            out[str(round(k * mo.frame_ms / 1000 * 24))] = frame_metrics(rider, bsk, bars, p)["offset"]
        Path(a.offsets).parent.mkdir(parents=True, exist_ok=True)
        Path(a.offsets).write_text(json.dumps(out))
        print("wrote", a.offsets)


if __name__ == "__main__":
    main()

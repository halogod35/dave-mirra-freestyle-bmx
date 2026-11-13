"""What the rider physics (Bmx/Src/com/action/physics.c, DM-048) does with level data, plus a
Python port of its simple integration rules. Pure stdlib.

Units: ZGE world units (the level is in centimetres), y points DOWN, velocities per frame,
angles as 16-bit binary angles (0x10000 = 360 degrees). Reference: docs/re/physics.md; the C is
decomp/src/zaxis/Bmx/Src/com/action/physics.c.
"""
from __future__ import annotations

import math
import struct

IMAGE_BASE = 0x400000

# ---------------------------------------------------------------- constants [V] (.data, VA)
CONSTANTS = {                       # name: (VA, struct format, value)
    "roll_drag":        (0x005577EC, "<5f", (0.0015, 0.0085, 0.0015, 0.019, 0.0018)),
    "air_drag":         (0x00557800, "<f", 0.001),
    "spin_damp":        (0x00557804, "<f", 0.35),
    "gravity":          (0x00557810, "<f", 1.58),
    "max_roll_speed":   (0x00557818, "<f", 60.0),
    "crash_impact":     (0x0055781C, "<f", 60.0),
    "land_impact":      (0x00557820, "<f", 85.0),
    "land_sideways":    (0x00557824, "<f", 35.0),
    "steer_speed":      (0x00557828, "<f", 12.0),
    "air_speed_cap":    (0x0055782C, "<f", 16.0),
    "spine_hop":        (0x00557834, "<f", 0.85),
    "grind_min_speed":  (0x00557838, "<f", 6.0),
    "body_box":         (0x0055783C, "<3f", (50.0, 40.0, 40.0)),
    "angles":           (0x00557848, "<9h", (0x3777, 0x238E, 0x3777, 0x2000, 0x4000, 0x2EEE, 0x2AAA, 0x238E, 0x271C)),
    "lock_frames_grind": (0x0055785C, "<i", 12),
    "lock_frames_stall": (0x00557860, "<i", 24),
    "auto_rot_frames":  (0x00557864, "<i", 15),
    "lock_radius":      (0x00557868, "<f", 150.0),
    "edge_radius":      (0x00557870, "<f", 30.0),
    "susp_gravity":     (0x00557878, "<f", 30.0),
    "yaw_damp":         (0x0055787C, "<f", 0.3),
    "orient_blend":     (0x00557880, "<f", 0.15),
    "max_lean_deg":     (0x00557884, "<f", 45.0),
    "up":               (0x00557888, "<3f", (0.0, -1.0, 0.0)),
}
GRAVITY = CONSTANTS["gravity"][2]
AIR_DRAG = CONSTANTS["air_drag"][2]

# bike defaults written by rider_Create (rider +0xC68.., DM-046/DM-048)
BIKE_TUNING = {"com_height": 50.0, "transfer": 5.0, "spring_mass": 0.2,
               "suspension": (-15.0, 25.0, 0.3, 0.1, 0.1),   # min, max, stiffness, damp extend, damp compress
               "wheel_front": (55.25, -5.6, 0.0), "wheel_rear": (-36.52, -5.6, 0.0), "wheel_radius": 24.65}

STATES = ("roll", "air", "crash", "grind_lock", "grind", "stall_lock", "stall")

# ---------------------------------------------------------------- Tag._sd codes as physics reads them
#: face-tag class (TAGI face record code >> 12) -> meaning. [V] code paths; names [I].
FACE_CLASSES = {
    0: "ground: plain surface (ids 0xFA1-0xFA4 = respawn faces)",
    1: "zone id: stripped to class 0 by the collision layer when extra == 0 (always); game.c reads the id",
    2: "spine: id = coping edge code; air from it hops over the coping",
    3: "vert wall: id = coping edge code; air goes straight up and back down (fakie)",
    4: "launch ramp, take-off speed cap 11",
    5: "launch ramp, cap 15",
    6: "launch ramp, cap 21",
    7: "launch ramp, cap 23",
    8: "launch ramp, cap 35",
    9: "trigger face: not solid, reported to prop.c (prop_0042EF60) when touched",
    10: "launch pad: id = launch speed along the face normal",
}
#: take-off speed cap (horizontal, per frame) by surface class; anything else uses air_speed_cap
TAKEOFF_CAP = {4: 11.0, 5: 15.0, 6: 21.0, 7: 23.0, 8: 35.0}
RESPAWN_IDS = range(0xFA1, 0xFA5)
#: edge-tag kind (TAGI edge record byte 2) -> grind sound / sparks
EDGE_KINDS = {0: "default (sparks, sound 0)", 1: "metal (sparks, sound 7)", 2: "wood (no sparks, sound 2)",
              3: "default (sparks, sound 0)"}


def face_flags(tag_flags: int) -> int:
    """The face-record flag byte physics sees (+0x15) for a TAGI face record ``flags`` byte:
    bit0 -> 0x10 (no respawn / not a wall target), bit1 -> 0x08, bit2 -> 0x20 (not a wall target),
    bit3 -> 0x40 ([V] NgIFzcollide_00469B00)."""
    b = tag_flags
    return ((((b & 1) << 1 | (b & 0xC)) << 1 | (b & 2)) << 2) & 0xFF


def tag_class(code: int) -> int:
    return (code >> 12) & 0xF


def tag_id(code: int) -> int:
    return code & 0xFFF


# ---------------------------------------------------------------- integration rules
def air_step(vel):
    """bmxphysUpdateForcesAir 0x00425760: gravity, then 0.1 % drag; returns (vel, frameDist)
    where frameDist is measured before the drag (as the exe does)."""
    vx, vy, vz = vel
    vy += GRAVITY
    dist = math.sqrt(vx * vx + vy * vy + vz * vz)
    k = 1.0 - AIR_DRAG
    return (vx * k, vy * k, vz * k), dist


def grind_step(vel, edge_dir, forward, stat5):
    """bmxphysUpdateForcesGrind 0x004257E0: gravity along the edge (oriented like the rider's
    forward) then friction 0.85 % x (1 + 10 * (1 - stat5)). Returns (vel, accel, frameDist)."""
    d = list(edge_dir)
    if sum(a * b for a, b in zip(forward, d)) < 0.0:
        d = [-x for x in d]
    g = GRAVITY * d[1]
    acc = [x * g for x in d]
    v = [a + b for a, b in zip(vel, acc)]
    k = 1.0 - ((1.0 - stat5) * 10.0 + 1.0) * CONSTANTS["roll_drag"][2][1]
    v = [x * k for x in v]
    return tuple(v), tuple(acc), math.sqrt(sum(x * x for x in v))


def takeoff_cap(surface: int, slope_from_45: int) -> float:
    """bmxphysStateInitAir 0x00422E50: horizontal take-off speed cap. ``slope_from_45`` is
    |acos(-up.y) - 45 deg| as a binary angle; within 30 deg the cap grows by (1 + sin)."""
    cap = TAKEOFF_CAP.get(surface, CONSTANTS["air_speed_cap"][2])
    a = abs(slope_from_45)
    if (a & 0xFFFF) < 0x1555:
        cap *= math.sin(a * 2 * math.pi / 65536.0) + 1.0
    return cap


def lockon_frames(vel, delta, max_frames):
    """bmxphysStateInitLockOn 0x00421190: frames to reach a target ``delta`` = target - bike
    position (integer-truncated per axis in the exe) with velocity ``vel``; clamped to
    [6, max_frames] (12 grind, 24 stall)."""
    dx, dy, dz = (int(x) for x in delta)
    horiz = math.hypot(vel[0], vel[2])
    if abs(vel[1]) <= horiz:
        frames = int(int(math.sqrt(dz * dz + dx * dx)) / horiz) if horiz else max_frames
    else:
        disc = vel[1] * vel[1] + 2.0 * delta[1] * GRAVITY
        if disc < 0.0:
            frames = max_frames
        else:
            s = math.sqrt(disc)
            frames = int((s - vel[1]) / GRAVITY) if -vel[1] < s else int((-vel[1] - s) / GRAVITY)
    if frames < 6:
        return 6
    return max_frames if frames >= max_frames else frames


def lockon_step(delta, frames):
    """The per-frame move of the lock-on flight (rider +0x468): straight line in x/z, a parabola
    in y (gravity is added to step.y every frame by bmxphysUpdateForcesLockOn)."""
    f = float(frames)
    return (delta[0] / f, delta[1] / f - GRAVITY * 0.5 * f, delta[2] / f)


def simulate_lockon(delta, frames):
    """Run the lock-on flight as the exe does (forces, then position; the last frame switches to
    GRIND/STALL instead of moving, which snaps to the target). Returns the offset reached
    before the snap."""
    step = list(lockon_step(delta, frames))
    pos = [0.0, 0.0, 0.0]
    timer = frames
    while True:
        step[1] += GRAVITY
        timer -= 1
        if timer == 0:
            return tuple(pos)
        pos = [p + s for p, s in zip(pos, step)]


def wheel_spin(speed: float, backwards: bool = False) -> int:
    """Rolling wheel rotation per frame (binary angle): speed / (2 pi 25) * 65536, as int16."""
    v = int(speed * 0.006366203 * 65536.0)
    v = ((v + 0x8000) & 0xFFFF) - 0x8000
    return -v if backwards else v


def edge_plane_distance(p, plane):
    """bmxphysEdgePlaneDistance 0x0042D670: the point is truncated to integers first."""
    return int(p[2]) * plane[2] + int(p[1]) * plane[1] + int(p[0]) * plane[0] + plane[3]


def closest_point_on_segment(a, b, p):
    """bmxphysClosestPointOnSegment 0x0042D6E0 (int16 end points / point)."""
    d = [b[i] - a[i] for i in range(3)]
    n = math.sqrt(sum(x * x for x in d))
    d = [x / n * 0.9999 for x in d] if n else d          # zmathVecNormalize scales by 0.9999
    if sum((a[i] - p[i]) * d[i] for i in range(3)) >= 0.0:
        return tuple(float(x) for x in a)
    t = sum((b[i] - p[i]) * d[i] for i in range(3))
    if t > 0.0:
        return tuple(b[i] - t * d[i] for i in range(3))
    return tuple(float(x) for x in b)


def read_exe_constants(img) -> dict:
    """The CONSTANTS table read from a memory-mapped NgBMX.exe image."""
    out = {}
    for name, (va, fmt, _) in CONSTANTS.items():
        v = struct.unpack_from(fmt, img, va - IMAGE_BASE)
        out[name] = v if len(v) > 1 else v[0]
    return out

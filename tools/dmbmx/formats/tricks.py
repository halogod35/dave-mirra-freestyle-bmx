"""Trick ids -> motion ids (DM-046) and the trick selection from the stick (DM-047).
Spec: docs/formats/ANIM.md sections 7 and 8.

Trick ids index g_trickNames 0x00559390 (84 names: 46 tricks, then 38 modifiers). The rider code
(rider.c, decompiled by DM-047) requests motions by number into the motion bank (motion.zal, ids
1..238, see ANIM.md):

- air trick t (0..26): g_trickMotion[t] (u16 table 0x00558FF8) = start, +1 hold, +2 release;
  several tricks share an animation (the flips reuse the base trick's clip, the engine adds the
  rotation);
- modifier m (0..37): g_modMotion[m] (0x00559030) = start, +1 hold, +2 release (73..186);
- grind / stall / plant t (0x1B..0x2B): 3t + 0x6A enter, 3t + 0x6B hold, 3t + 0x6C exit (187..237;
  fast plant 232-234, wall tap 235-237: the plants run through the stall state, riderStall);
- manual / nose wheelie (0x2C/0x2D): 19 enter (20 when the take-off clip 15 was playing), 20 hold,
  21 exit (riderManual).
[V] rider.c (riderAirTrick, riderAirModifier, riderGrind, riderStall, riderManual); tables [V] (exe).

Selection (riderAirTrick / riderAirModifier / riderGrindTrick ..., [V] rider.c): while airborne the
stick directions go into a 4-entry buffer (DIR_*). The trick button takes
1. the rider's signature combos (SIGNATURE_TRICKS: the last 3 directions -> trick + modifier),
2. else the last two directions (AIR_TRICK_DIR2), 3. else the last one (AIR_TRICK_DIR1).
The modifier button maps the last two / last direction to a modifier (MOD_DIR2 / MOD_DIR1) and
TRICK_MOD_TABLE[trick][modifier] filters it (None = not allowed, or a substitute modifier).
Grind / stall / plant tricks come from the last direction (or the stick when the buffer is empty).
"""

TRICK_MOTION_VA, MOD_MOTION_VA = 0x00558FF8, 0x00559030
AIR_TRICKS, MODIFIERS = 27, 38
FAST_PLANT, WALL_TAP, MANUAL, NOSE_WHEELIE = 0x2A, 0x2B, 0x2C, 0x2D

# g_trickMotion[0..26] (entry 27 is 0)
TRICK_MOTION = (25, 28, 31, 34, 37, 40, 43, 67, 34, 34, 46, 46, 25, 28, 31, 67,
                37, 40, 43, 49, 49, 52, 55, 58, 61, 64, 70)
# g_modMotion[0..37] = 73 + 3m
MOD_MOTION = tuple(73 + 3 * m for m in range(MODIFIERS))

# Motions the riding states request directly (rider.c constants, [V]); every other id in 1..238 except
# the trick clips is never requested (UNUSED_MOTIONS, [V]: rider.c and physics.c are the only users of
# g_motionTable and only rider.c writes the motion slots). Motion 4 is the default of the second slot
# (riderSetState), which only physics samples (0x0042B5E0, zactorPlaySecondMotion 0x00477210).
STATE_MOTIONS = {
    2: "idle (standing still)",
    5: "rolling / landing",
    8: "pedalling (rate = max(1, speed / 20))",
    10: "skid start", 11: "skid hold",
    13: "crouch (jump button held)", 14: "crouch hold",
    15: "take-off (hop / ollie)",
    17: "turnaround (180)", 18: "turnaround, roll out backwards",
    19: "manual enter", 20: "manual hold (also the enter after clip 15)", 21: "manual exit",
    22: "landing (variant)",
    23: "airborne, no trick",
    24: "air idle (trick button without a direction)",
}
SECOND_SLOT_MOTION = 4
UNUSED_MOTIONS = (1, 3, 6, 7, 9, 12, 16, 238)


def trick_motions(trick):
    """(start, hold, end) motion ids of trick id 0..45 (None for ids outside 0..45)."""
    if 0 <= trick < AIR_TRICKS:
        m = TRICK_MOTION[trick]
        return m, m + 1, m + 2
    if 0x1B <= trick <= WALL_TAP:
        return 3 * trick + 0x6A, 3 * trick + 0x6B, 3 * trick + 0x6C
    if trick in (MANUAL, NOSE_WHEELIE):
        return 19, 20, 21
    return None


def modifier_motions(mod):
    """(start, hold, end) motion ids of modifier 0..37 (trick name index 46 + mod)."""
    m = MOD_MOTION[mod]
    return m, m + 1, m + 2


# ----------------------------------------------------------------------------- directions
# RIDER_DIR_*: 1 left, 2 right, 3 up, 4 down, 5 up-left, 6 up-right, 7 down-left, 8 down-right
# ("up" = stick y < 0 / d-pad bit 1, as contrler_00408610 folds the d-pad in).
DIR_NAMES = (None, "left", "right", "up", "down", "up-left", "up-right", "down-left", "down-right")
DIR_FROM_MASK_VA = 0x00558904
DIR_FROM_MASK = (0, 3, 4, 0, 1, 5, 7, 0, 2, 6, 8, 0, 0, 0, 0, 0)   # mask bits 1 up, 2 down, 4 left, 8 right


def direction(buttons=0, x=0.0, y=0.0):
    """riderCurrentDirection 0x00435F90: d-pad bits + the stick beyond half way -> DIR_* (0 none)."""
    m = buttons
    if abs(x) > 0.5:
        m |= 4 if x < 0 else 8
    if abs(y) > 0.5:
        m |= 1 if y < 0 else 2
    return DIR_FROM_MASK[m & 0xF]


# ----------------------------------------------------------------------------- tables [V] exe
AIR_TRICK_DIR1_VA, AIR_TRICK_DIR2_VA = 0x00558AF7, 0x00558B00
MOD_DIR1_VA, MOD_DIR2_VA = 0x00558B40, 0x00558B4C
TRICK_MOD_TABLE_VA = 0x00558B8C
GRIND_TRICK_DIR_VA, STALL_TRICK_DIR_VA, PLANT_TRICK_DIR_VA = 0x00558F90, 0x00558F9C, 0x00558FA8
SIGNATURE_TRICKS_VA = 0x00553840
NONE = 0xFF

AIR_TRICK_DIR1 = (0, 0, 1, 2, 3, 7, 4, 5, 6)            # [last direction]
AIR_TRICK_DIR2 = (                                       # [previous - 1][last - 1]
    (22, 24, NONE, 12, NONE, NONE, 25, NONE),
    (20, 21, NONE, 13, NONE, NONE, NONE, NONE),
    (NONE, NONE, 10, 14, NONE, NONE, NONE, NONE),
    (12, 13, 14, 8, 15, 16, 17, 18),
    (NONE, NONE, NONE, 15, NONE, NONE, NONE, NONE),
    (NONE, NONE, NONE, 16, NONE, 26, NONE, NONE),
    (25, NONE, NONE, 17, NONE, NONE, NONE, NONE),
    (NONE, NONE, NONE, 18, NONE, NONE, NONE, 23),
)
MOD_DIR1 = (NONE, 1, 2, 3, 0, 12, 6, 7, 8)
MOD_DIR2 = (
    (34, 10, 20, 26, 32, 16, 33, 22),
    (10, 18, 11, 37, 29, 14, 21, 5),
    (20, 11, 4, NONE, 27, 35, 19, NONE),
    (26, 37, NONE, 24, 31, 36, 25, 23),
    (32, 29, 27, 31, NONE, 28, NONE, 30),
    (16, 14, 35, 36, 28, 17, 15, NONE),
    (33, 21, 19, 25, NONE, 15, 33, 9),
    (22, 5, NONE, 23, 30, NONE, 9, 5),
)
GRIND_TRICK_DIR = (27, 28, 29, 30, 31, 32, 33, 34, 35)   # [direction] (none: double peg)
STALL_TRICK_DIR = (36, 41, 41, 37, 38, 39, 39, 40, 40)
PLANT_TRICK_DIR = (NONE, FAST_PLANT, WALL_TAP, NONE, NONE, NONE, NONE, NONE, NONE)

# g_trickModTable[27][38]: identity except these entries (NONE = the modifier is refused)
_N = NONE
_NO_ROCKETS = {27: _N, 28: _N, 29: _N, 30: _N, 31: _N, 32: _N}
TRICK_MOD_EXCEPTIONS = {
    1: {9: _N, 12: 32, **_NO_ROCKETS, 34: _N},
    4: {1: _N, 7: _N, 9: _N, 10: 2, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: 2, 22: 8, 25: _N, 26: _N,
        **_NO_ROCKETS, 34: _N},
    5: {7: 1, 9: _N, 12: 32, **_NO_ROCKETS, 33: _N},
    6: {1: _N, 6: _N, 7: _N, 9: _N, 10: 2, 12: _N, 17: _N, **_NO_ROCKETS, 34: _N},
    7: {6: 17},
    13: {9: _N, 12: 32, **_NO_ROCKETS, 34: _N},
    15: {6: 17},
    16: {1: _N, 7: _N, 9: _N, 10: 2, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: 2, 22: 8, 25: _N, 26: _N,
         **_NO_ROCKETS, 34: _N},
    17: {7: 1, 9: _N, 12: 32, **_NO_ROCKETS, 33: _N},
    18: {1: _N, 7: _N, 9: _N, 10: 2, 12: _N, 17: _N, **_NO_ROCKETS, 34: _N},
    19: {0: _N, 1: _N, 6: _N, 7: _N, 9: _N, 10: 2, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: _N, 22: _N,
         25: _N, 26: _N, **_NO_ROCKETS, 34: _N},
    20: {0: _N, 1: _N, 6: _N, 7: _N, 9: _N, 10: 2, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: _N, 22: _N,
         25: _N, 26: _N, **_NO_ROCKETS, 34: _N},
    21: {1: _N, 7: _N, 9: _N, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: _N, 22: _N, 25: _N, 26: _N,
         **_NO_ROCKETS, 34: _N},
    23: {1: _N, 6: _N, 7: _N, 9: _N, 10: 2, 12: _N, 17: _N, **_NO_ROCKETS, 34: _N},
    24: {1: 34, 7: 34, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: _N, 22: _N, 25: _N, 26: _N, **_NO_ROCKETS,
         33: _N, 34: _N},
    25: {7: 1, 9: _N, 12: 32, **_NO_ROCKETS, 33: _N},
    26: {1: _N, 7: _N, 9: _N, 10: 2, 12: _N, 15: _N, 16: _N, 19: _N, 20: _N, 21: 2, 22: 8, 25: _N, 26: _N,
         **_NO_ROCKETS, 33: _N, 34: _N},
}


def trick_mod_table():
    """g_trickModTable as 27 rows of 38 bytes."""
    return tuple(tuple(TRICK_MOD_EXCEPTIONS.get(t, {}).get(m, m) for m in range(MODIFIERS))
                 for t in range(AIR_TRICKS))


# g_signatureTricks[rider]: ((directions, trick, modifier or NONE), ...); two per rider, slots 3-4 empty
SIGNATURE_TRICKS = (
    (((4, 4, 4), 9, NONE), ((2, 6, 2), 26, 17)),       # 0 Dave Mirra
    (((2, 2, 6), 21, 6), ((4, 4, 8), 3, 23)),
    (((2, 2, 4), 1, 36), ((4, 7, 1), 17, 22)),
    (((8, 8, 8), 23, 7), ((4, 8, 8), 18, 8)),
    (((6, 1, 1), 4, 17), ((6, 4, 4), 4, 37)),
    (((3, 4, 3), 13, NONE), ((4, 3, 4), 3, 13)),
    (((2, 1, 4), 19, NONE), ((2, 1, 6), 20, 17)),
    (((3, 1, 1), 2, 1), ((4, 6, 6), 16, 6)),
    (((3, 3, 3), 11, NONE), ((2, 1, 2), 20, 2)),
    (((8, 8, 2), 23, 37), ((1, 4, 2), 1, 21)),
    (((1, 4, 2), 13, 10), ((2, 3, 1), 7, 30)),
    (((2, 3, 1), 4, 21), ((1, 4, 2), 3, 28)),          # 11 Amish Boy (save_fields.RIDER_NAMES order)
)


def _none(v):
    return None if v == NONE else v


def select_air_trick(dirs, rider=0):
    """riderAirTrick, phase 0 (0x00434760): the direction buffer (oldest first, max 4) -> (trick, mod)
    with None for "no trick" / "no modifier"."""
    dirs = tuple(dirs)[-4:]
    n = len(dirs)
    if n == 0:
        return None, None
    for combo, trick, mod in SIGNATURE_TRICKS[rider]:
        if 0 < len(combo) <= n and dirs[n - len(combo):] == combo:
            return trick, _none(mod)
    t = NONE
    if n > 1:
        t = AIR_TRICK_DIR2[dirs[-2] - 1][dirs[-1] - 1]
    if t == NONE:
        t = AIR_TRICK_DIR1[dirs[-1]]
    return _none(t), None


def modifier_slot(dirs):
    """The modifier the buffered directions ask for (riderAirTrick / riderAirModifier), None if none."""
    dirs = tuple(dirs)[-4:]
    m = NONE
    if len(dirs) >= 2:
        m = MOD_DIR2[dirs[-2] - 1][dirs[-1] - 1]
    if m == NONE:
        m = MOD_DIR1[dirs[-1] if dirs else 0]
    return _none(m)


def select_modifier(trick, dirs):
    """Modifier added to air trick ``trick`` (g_trickModTable filter); None when refused / no direction."""
    m = modifier_slot(dirs)
    if m is None:
        return None
    return _none(trick_mod_table()[trick][m])


def select_ground_trick(kind, dirs=(), current=0):
    """Grind / stall / plant trick for the last buffered direction (``current`` stick direction when the
    buffer is empty): riderGrindTrick / riderStallTrick / riderPlantTrick."""
    table = {"grind": GRIND_TRICK_DIR, "stall": STALL_TRICK_DIR, "plant": PLANT_TRICK_DIR}[kind]
    d = tuple(dirs)[-1] if dirs else current
    return _none(table[d])

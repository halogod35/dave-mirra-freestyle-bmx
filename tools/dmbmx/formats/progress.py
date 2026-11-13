"""Career progress of SAVE.DAT: challenge goals, rewards and records.

A Python port of the game-flow code in NgBMX.exe (decomp/src/zaxis/Bmx/Src/com/action/game.c and
comdata.c). It works on a ``dmbmx.formats.save.Save`` and agrees with ``save_fields`` (tests:
tests/test_decomp_game.py). Spec: docs/formats/SAVE.md sections 2.2, 2.4 and 7.

    progress.objective_state(save, rider, level, tier, goal, obj)      # gameGetObjectiveState
    progress.apply_reward(save, rider, (type, param))                  # gameApplyReward
    progress.award_level_progress(save, rider, level, ...)             # gameAwardLevelProgress
    progress.complete_all(save)                                        # every goal/comp, rewards included

The tables below are the exe's ([V], VAs given; tests compare them with NgBMX.exe when present).
"""
from dmbmx.formats import save as S

LEVELS = 12
GOAL_LEVELS = 6                      # challenge levels 0..5; 6..11 are competitions
TIERS = 3
GOALS = 4
GOAL_INFO_OFF, GOAL_INFO_LEN = 7, 0x27
COMP_OFF = 0xEB                      # + level (6..11)

TIER_NAMES = ("amateur", "pro", "hardcore")

# g_goalDefs 0x00555118: numObjectives of [level * 3 + tier][goal]
OBJECTIVE_COUNTS = (
    (1, 1, 4, 1), (1, 1, 1, 1), (1, 1, 1, 1),
    (1, 1, 4, 1), (1, 1, 4, 1), (1, 1, 4, 4),
    (4, 1, 1, 1), (1, 1, 1, 1), (1, 1, 1, 1),
    (3, 1, 4, 1), (3, 1, 4, 1), (3, 1, 4, 2),
    (1, 1, 4, 1), (1, 1, 4, 1), (1, 1, 1, 1),
    (1, 1, 1, 1), (1, 2, 1, 1), (1, 1, 1, 1),
)
GOAL_DEFS_VA = 0x00555118
GOAL_DEF_SIZE, OBJECTIVE_SIZE = 0x54, 0x14

# g_rewards 0x005568B8 (GameRewardTables): (type, param) pairs
REWARDS_VA, REWARDS_SIZE = 0x005568B8, 0xE0
LEVEL_TIER_REWARDS = (       # [level][tier] -> 2 rewards (competitions: tier = placing 3rd/2nd/1st)
    (((4, 1), (0, 0)), ((5, 0), (0, 0)), ((6, 0), (0, 0))),
    (((4, 2), (0, 0)), ((5, 1), (0, 0)), ((6, 1), (0, 0))),
    (((2, 1), (0, 0)), ((1, 1), (0, 0)), ((6, 2), (0, 0))),
    (((5, 2), (0, 0)), ((4, 4), (0, 0)), ((6, 3), (0, 0))),
    (((5, 3), (0, 0)), ((4, 5), (0, 0)), ((6, 4), (0, 0))),
    (((2, 2), (0, 0)), ((1, 2), (4, 6)), ((6, 5), (0, 0))),
    (((5, 4), (0, 0)), ((6, 6), (0, 0)), ((4, 7), (0, 0))),
    (((5, 5), (0, 0)), ((6, 7), (0, 0)), ((4, 8), (0, 0))),
    (((2, 3), (0, 0)), ((6, 8), (0, 0)), ((4, 9), (0, 0))),
    (((2, 4), (0, 0)), ((6, 9), (0, 0)), ((4, 10), (0, 0))),
    (((5, 6), (0, 0)), ((6, 10), (0, 0)), ((4, 11), (0, 0))),
    (((5, 7), (0, 0)), ((6, 11), (0, 0)), ((3, 10), (0, 0))),
)
PARK_TIER_REWARDS = (        # [park][tier]: all 3 levels of the park done at that tier
    ((0, 0), (4, 3), (7, 0)),
    ((0, 0), (0, 0), (7, 1)),
    ((0, 0), (0, 0), (1, 3)),
    ((0, 0), (0, 0), (1, 4)),
)
FINAL_REWARDS = ((9, 0), (3, 11), (9, 1), (0, 0))   # all hardcore; riders 0-9 all done; rider 11 done
RIDER_FINAL_REWARDS = (      # [rider] -> 2, reached through reward type 9
    ((8, 0), (0, 0)), ((8, 0), (0, 0)), ((10, 3), (0, 0)), ((10, 0), (0, 0)),
    ((10, 4), (0, 0)), ((10, 5), (0, 0)), ((10, 7), (0, 0)), ((10, 1), (0, 0)),
    ((10, 2), (0, 0)), ((10, 6), (0, 0)), ((6, 12), (0, 0)), ((8, 0), (0, 0)),
)
REWARD_TYPES = {0: "none", 1: "bike", 2: "style", 3: "rider", 4: "level", 5: "sponsor [I]",
                6: "secret [I]", 7: "park secret [I]", 8: "movie", 9: "per rider", 10: "cheat"}

# objective types (GoalObjective.type), see game.h / SAVE.md 7
OBJECTIVE_TYPES = (
    "trick", "trick off edge", "trick over prop group", "trick off object", "trick count",
    "rider state", "state on edge (grind)", "state on prop group", "grind object", "grind prop group",
    "jump over prop group", "jump over prop group with trick", "touch prop group", "transfer",
    "transfer with trick", "transfer with spin", "transfer objects", "transfer prop groups",
    "transfer edge to object", "transfer edge to prop group", "record", "hit object",
)
STATE_IDLE, STATE_IN_AREA, STATE_ACTIVE, STATE_PENDING, STATE_DONE, STATE_REWARDED = range(6)

# ---------------------------------------------------------------------------- records
GAME_MODES = ("single player", "best run", "high five", "B-M-X", "random spot", "wipeout",
              "longest grind", "highest walltap", "big air contest", "furthest jump", "longest manual",
              "death match")                                      # text 0x3001 + mode
STATS = ("total score", "best run", "trick set", "single trick", "big air", "highest mark",
         "longest grind", "longest jump", "longest manual", "longest skid")   # text 0x1094 + stat
MODE_STAT = (1, 1, 1, 1, 0, 1, 6, 5, 4, 7, 8, 1)                 # g_gameModeStat 0x00553658
MODE_ROUNDS = (1, 1, 5, 15, 5, 3, 3, 3, 3, 3, 3, 1)              # g_gameModeRounds 0x00554C8C
TIME_LIMITS = ("30 sec", "60 sec", "2 min", "5 min")             # best run / death match sub-mode
RECORD_ENTRIES = (                                               # the 24 entries per level (SAVE.md 2.4)
    "single player #1", "single player #2", "single player #3",
    "best run 30 sec", "best run 60 sec", "best run 2 min", "best run 5 min",
    "high five", "B-M-X", "random spot (run)", "random spot (total)", "wipeout",
    "death match 30 sec", "death match 60 sec", "death match 2 min", "death match 5 min",
    "trick set", "single trick", "big air", "highest mark", "longest grind", "longest jump",
    "longest manual", "longest skid",
)


def record_id(mode, sub=0, stat=0, slot=0, index=0):
    return (mode << 24) | (sub << 22) | (stat << 16) | (slot << 12) | index


def record_entry(rid):
    """Entry 0..23 of a level's records for a record id, or None (comdataGetRecordPtr 0x00407D20)."""
    stat, sub, mode = (rid >> 16) & 0x3F, (rid >> 22) & 3, rid >> 24
    if stat > 1:
        return stat + 14
    if mode == 0:
        return rid & 0xFFF
    if mode == 1:
        return 3 + sub
    if mode in (2, 3, 5):
        return {2: 7, 3: 8, 5: 11}[mode] if stat == 1 else None
    if mode == 4:
        return {0: 10, 1: 9}.get(stat)
    if 6 <= mode <= 10:
        return {6: 20, 7: 19, 8: 18, 9: 21, 10: 22}[mode]
    if mode == 11:
        return 12 + sub
    return None


# ---------------------------------------------------------------------------- goal bytes
def _goal_info_off(level):
    if not 0 <= level < GOAL_LEVELS:
        raise ValueError(f"invalid level for goal info: {level}")   # comdataGetGoalInfo asserts level < 6
    return GOAL_INFO_OFF + level * GOAL_INFO_LEN


def goal_entry_off(level, tier, goal):
    return _goal_info_off(level) + 3 + tier * 12 + goal * 3


def objective_state(save, rider, level, tier, goal, obj):
    """gameGetObjectiveState 0x004102C0 (for an explicit tier; the game uses the current one)."""
    b = save.riders[rider][goal_entry_off(level, tier, goal) + 1 + (obj >> 1)]
    return (b >> ((obj & 1) * 4)) & 7


def set_objective_state(save, rider, level, tier, goal, obj, state):
    """gameSetObjectiveState 0x00410350: replaces the whole nibble (the flag bit 3 is cleared)."""
    rec = save.riders[rider]
    off = goal_entry_off(level, tier, goal) + 1 + (obj >> 1)
    sh = (obj & 1) * 4
    rec[off] = (rec[off] & ~(0xF << sh) & 0xFF) | ((state & 0xF) << sh)


def objective_flag(save, rider, level, tier, goal, obj):
    """gameGetObjectiveFlag 0x00410480 (mask 1)."""
    b = save.riders[rider][goal_entry_off(level, tier, goal) + 1 + (obj >> 1)]
    return (b >> ((obj & 1) * 4 + 3)) & 1


def goal_info(save, rider, level):
    """(byte0, tier, tier_goals_done) of a challenge level."""
    off = _goal_info_off(level)
    rec = save.riders[rider]
    return rec[off], rec[off + 1], rec[off + 2]


def tier_done(save, rider, level, tier):
    """Is `tier` of `level` complete (the test in gameAwardLevelProgress 0x00410E70)?"""
    rec = save.riders[rider]
    if level < GOAL_LEVELS:
        _, t, done = goal_info(save, rider, level)
        return not (t <= tier and (t != tier or done != 4))
    return rec[COMP_OFF + level] > tier


# ---------------------------------------------------------------------------- rewards
def _u32(rec, off=0):
    return int.from_bytes(rec[off:off + 4], "little")


def _set_u32(rec, v, off=0):
    rec[off:off + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")


def apply_reward(save, rider, reward, profile=None):
    """gameApplyReward 0x00411090: unlocks only raise. `profile` (0/1 or None) is the player whose
    tutorial-hints flag the first level unlock clears. Returns the reward actually applied."""
    rec = save.riders[rider]
    typ, param = reward
    while typ == 9:                              # per-rider indirection
        typ, param = RIDER_FINAL_REWARDS[rider][param]
    if typ == 1:
        rec[6] = max(rec[6], param + 1)
        _set_u32(rec, _u32(rec) | 0x40)
    elif typ == 2:
        rec[5] = max(rec[5], param + 1)
        _set_u32(rec, _u32(rec) | 0x80)
    elif typ == 3:
        if param > 11:
            raise ValueError("goal completion error - char")
        if param + 1 >= save.options[7]:
            save.options[7] = param + 1
    elif typ == 4:
        if param > 11:
            raise ValueError("goal completion error - level")
        rec[4] = max(rec[4], param + 1)
        _set_u32(rec, _u32(rec) | 0x100)
        if param == 1 and profile is not None:
            save.profiles[profile].raw[0] &= ~2 & 0xFF
    elif typ == 8:
        _set_u32(rec, _u32(rec) | 1)
    elif typ == 10:
        w = int.from_bytes(save.options[0:2], "little") | (1 << (param & 0x1F))
        save.options[0:2] = (w & 0xFFFF).to_bytes(2, "little")
    return typ, param


def award_level_progress(save, rider, level, *, from_tier, to_tier, profile=None):
    """The reward part of gameAwardLevelProgress 0x00410C60 for tiers from_tier..to_tier-1 of `level`
    (the caller has already stored the progress: goal bytes / comp result). Returns the run-reward
    bits (BmxRider.runRewards) and the list of applied (type, param)."""
    rec = save.riders[rider]
    bits, applied = 0, []
    park = level // 3
    for t in range(from_tier, to_tier):
        for k in range(2):
            bits |= 1 << (t * 2 + k)
            applied.append(apply_reward(save, rider, LEVEL_TIER_REWARDS[level][t][k], profile))
        for scope in range(2):
            lo, hi = (park * 3, park * 3 + 2) if scope == 0 else (0, 11)
            if not all(tier_done(save, rider, L, t) for L in range(lo, hi + 1)):
                break                            # (the binary skips the all-levels check too)
            if scope == 0:
                rw = PARK_TIER_REWARDS[park][t]
                if rw[0] != 0:
                    bits |= 1 << (t + 6)
                    applied.append(apply_reward(save, rider, rw, profile))
                if t == 2:
                    _set_u32(rec, _u32(rec) | (4 << park))
            elif t == 2:
                bits |= 0x200
                _set_u32(rec, _u32(rec) | 2)
                applied.append(apply_reward(save, rider, FINAL_REWARDS[0], profile))
                if rider < 10:
                    if not all(_u32(save.riders[i]) & 2 for i in range(10)):
                        continue
                    rw, bit = FINAL_REWARDS[1], 0x400
                elif rider == 11:
                    rw, bit = FINAL_REWARDS[2], 0x800
                else:
                    continue
                bits |= bit
                applied.append(apply_reward(save, rider, rw, profile))
    return bits, applied


def complete_level_tier(save, rider, level, tier):
    """Mark every goal of a challenge level tier done, as gameUpdateObjectives + gameAdvanceGoalTier
    leave it: objectives REWARDED (5), goal byte 0x80 | objectives, tier_goals_done 4. Raise-only:
    a tier below the stored one is left alone. Returns True if anything was missing."""
    rec = save.riders[rider]
    off = _goal_info_off(level)
    if rec[off + 1] > tier:
        return False
    changed = False
    for g in range(GOALS):
        n = OBJECTIVE_COUNTS[level * 3 + tier][g]
        e = goal_entry_off(level, tier, g)
        want = bytearray(3)
        want[0] = 0x80 | n
        for j in range(n):
            want[1 + (j >> 1)] |= STATE_REWARDED << ((j & 1) * 4)
        if rec[e:e + 3] != want:
            rec[e:e + 3] = want
            changed = True
    if rec[off + 1] != tier or rec[off + 2] != 4:
        changed = True
    rec[off + 1] = tier
    rec[off + 2] = 4
    return changed


def complete_all(save):
    """Every challenge goal and 1st place in every competition, for every rider, with the rewards the
    game gives for them (a port of the level-end logic, run tier by tier). Raise-only: nothing is
    lowered and nothing but progress, unlocks, the record flags and the cheat bits is touched.
    Returns {rider: [(type, param), ...]} of the rewards applied."""
    out = {}
    for r in range(S.RIDER_COUNT):
        applied = []
        for t in range(TIERS):
            for level in range(LEVELS):
                if level < GOAL_LEVELS:
                    if not complete_level_tier(save, r, level, t):
                        continue
                else:
                    rec = save.riders[r]
                    if rec[COMP_OFF + level] > t:
                        continue
                    rec[COMP_OFF + level] = t + 1
                applied += award_level_progress(save, r, level, from_tier=t, to_tier=t + 1)[1]
        out[r] = applied
    return out


# ---------------------------------------------------------------------------- the exe's tables
def read_exe_tables(image, base=0x400000):
    """Goal definitions and reward tables from a memory-mapped NgBMX.exe image (pefile
    get_memory_mapped_image()). Returns (goals, rewards_raw): goals[level*3+tier][goal] =
    {"objectives": [dict(type, param, area, props)], "props": [...]}."""
    import struct
    goals = []
    for lt in range(GOAL_LEVELS * TIERS):
        tier = []
        for g in range(GOALS):
            a = GOAL_DEFS_VA - base + lt * 0x150 + g * GOAL_DEF_SIZE
            n = image[a]
            objs = []
            for j in range(n):
                o = a + 4 + j * OBJECTIVE_SIZE
                typ, p0, p1, p2, x0, z0, x1, z1 = struct.unpack_from("<4H4h", image, o)
                objs.append({"type": typ, "param": (p0, p1, p2), "area": (x0, z0, x1, z1),
                             "props": tuple(image[o + 16:o + 20])})
            tier.append({"objectives": objs, "props": tuple(image[a + 1:a + 4])})
        goals.append(tier)
    rewards = bytes(image[REWARDS_VA - base:REWARDS_VA - base + REWARDS_SIZE])
    return goals, rewards


def rewards_from_raw(raw):
    """(level_tier, park_tier, final, rider_final) from the 0xE0 raw bytes of g_rewards."""
    p = [(raw[i], raw[i + 1]) for i in range(0, REWARDS_SIZE, 2)]
    lt = tuple(tuple(tuple(p[(L * 3 + t) * 2:(L * 3 + t) * 2 + 2]) for t in range(TIERS)) for L in range(LEVELS))
    pk = tuple(tuple(p[72 + k * 3:72 + k * 3 + 3]) for k in range(4))
    return lt, pk, tuple(p[84:88]), tuple(tuple(p[88 + r * 2:90 + r * 2]) for r in range(12))

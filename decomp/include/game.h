/*
 * game.h - game flow, challenge goals, rewards and records (Bmx/Src/com/action/game.c, comdata.c).
 * DM-046. Offsets/sizes [V] (exe tables and the code that reads them); meanings [I] unless noted.
 * Save-side layout: save.h / docs/formats/SAVE.md. Python port: tools/dmbmx/formats/progress.py.
 */
#ifndef GAME_H
#define GAME_H
#include "dmtypes.h"

/* ---- challenge goals: g_goalDefs 0x00555118, [6 levels x 3 tiers] x 4 goals ---- */

/* One objective of a goal (20 bytes). Meaning of param[] depends on type, see GOAL_OBJ_* below. */
typedef struct GoalObjective {
    uint16_t type;             /* +0x00 GOAL_OBJ_* */
    uint16_t param[3];         /* +0x02 */
    int16_t  area[4];          /* +0x08 x0, z0, x1, z1: the objective is live only inside this box */
    uint8_t  props[4];         /* +0x10 bmxprop ids switched by the objective (0xFF none) */
} GoalObjective;               /* 0x14 */

/* One goal ("challenge"): up to 4 objectives that all have to be done (84 bytes). */
typedef struct GoalDef {
    uint8_t  numObjectives;    /* +0x00 1..4 */
    uint8_t  props[3];         /* +0x01 bmxprop ids switched when the goal is done (0xFF none) */
    GoalObjective obj[4];      /* +0x04 */
} GoalDef;                     /* 0x54 */

/* The 4 goals of one level tier (amateur / pro / hardcore). */
typedef struct GoalTierDef {
    GoalDef  goal[4];
} GoalTierDef;                 /* 0x150 */

/* ---- rewards: g_rewards 0x005568B8 (directly after the goal table) ---- */
typedef struct GameReward {
    uint8_t  type;             /* GAME_REWARD_* (0 = none) */
    uint8_t  param;
} GameReward;                  /* 2 */

typedef struct GameRewardTables {
    GameReward levelTier[12][3][2]; /* +0x00 level L, tier t (competitions: t = placing 3rd/2nd/1st) */
    GameReward parkTier[4][3];      /* +0x90 all 3 levels of park p done at tier t */
    GameReward final[4];            /* +0xA8 [0] all 12 levels hardcore, [1] riders 0-9 all done, [2] rider 11 done */
    GameReward riderFinal[12][2];   /* +0xB0 GAME_REWARD_PER_RIDER p -> riderFinal[rider][p] */
} GameRewardTables;                 /* 0xE0 */

/* ---- live records (this game): g_gameLive 0x0056B70C [4 slots][24 entries] ---- */
typedef struct GameLiveRecord {
    uint32_t value;            /* +0x00 score, or distance/height in cm */
    uint8_t  player;           /* +0x04 who set it */
    uint8_t  flags;            /* +0x05 bit0 beaten (needs a name), bit1 "new record" message shown */
    uint8_t  pad_06[2];
} GameLiveRecord;              /* 8 */

/* Per trick / modifier: base points and points per frame held (g_trickInfo 0x00559118:
 * [trick 0..45], [46] no modifier, [47 + modifier 0..37]). */
typedef struct GameTrickInfo {
    uint16_t points;           /* +0x00 */
    uint16_t rate;             /* +0x02 */
} GameTrickInfo;               /* 4 */

/* GHIDRA-SKIP-BEGIN */
typedef char game_h_size_check[(sizeof(GoalObjective) == 0x14 && sizeof(GoalDef) == 0x54 &&
                                sizeof(GoalTierDef) == 0x150 && sizeof(GameReward) == 0x02 &&
                                sizeof(GameRewardTables) == 0xE0 && sizeof(GameLiveRecord) == 0x08 &&
                                sizeof(GameTrickInfo) == 0x04) ? 1 : -1];

/* Objective types (GoalObjective.type), from gameUpdateObjectives 0x0040EC40 and the callbacks.
 * "trick" params: trick id (0xFF any) and minimum half-spins. States: GOAL_STATE_*. */
enum {
    GOAL_OBJ_TRICK            = 0,  /* trick p0 with >= p1 half spins (state 3, done when landed) */
    GOAL_OBJ_TRICK_OFF_EDGE   = 1,  /* trick p0/p1 while on edge tag ~p2 (+-5) */
    GOAL_OBJ_TRICK_OVER_GROUP = 2,  /* trick p0/p1 over prop group p2 */
    GOAL_OBJ_TRICK_OFF_OBJECT = 3,  /* trick p0/p1 off an object with tag >= p2 */
    GOAL_OBJ_TRICK_COUNT      = 4,  /* >= p0 air tricks in one set */
    GOAL_OBJ_RIDER_STATE      = 5,  /* rider state == p0 (inside the area) */
    GOAL_OBJ_STATE_ON_EDGE    = 6,  /* rider state p0 on edge tag p1..p2 (grind a rail) */
    GOAL_OBJ_STATE_ON_GROUP   = 7,  /* rider state p0 on prop group p1..p2 */
    GOAL_OBJ_GRIND_OBJECT     = 8,  /* grind/stall object tag p0..p1 (p2: also in a trick) */
    GOAL_OBJ_GRIND_GROUP      = 9,  /* grind/stall prop group p0..p1 (p2: also in a trick) */
    GOAL_OBJ_JUMP_GROUP       = 10, /* in the air over prop group p0..p1, not taken off from it, not landed on it */
    GOAL_OBJ_JUMP_GROUP_TRICK = 11, /* as 10, with trick p2 in the set */
    GOAL_OBJ_COLLECT          = 12, /* touch prop group p0..p1 (gameOnPropTouched) */
    GOAL_OBJ_TRANSFER         = 13, /* jump between edge tags ~p0 and ~p1 (either way) */
    GOAL_OBJ_TRANSFER_TRICK   = 14, /* as 13 with trick p2 */
    GOAL_OBJ_TRANSFER_SPIN    = 15, /* as 13 with >= p2 half spins */
    GOAL_OBJ_TRANSFER_OBJECTS = 16, /* jump between objects p0 and p1 */
    GOAL_OBJ_TRANSFER_GROUPS  = 17, /* jump between prop groups p0 and p1 */
    GOAL_OBJ_TRANSFER_TO_OBJ  = 18, /* edge ~p0 to an object p1..p2 */
    GOAL_OBJ_TRANSFER_TO_GROUP= 19, /* edge ~p0 to prop group p1..p2 */
    GOAL_OBJ_RECORD           = 20, /* record stat p0 > p1 (score, or cm for stats >= 4) */
    GOAL_OBJ_HIT_OBJECT       = 21  /* hit an object with tag p0..p1 (gameOnObjectHit) */
};
enum { GOAL_STATE_IDLE = 0, GOAL_STATE_IN_AREA = 1, GOAL_STATE_ACTIVE = 2, GOAL_STATE_PENDING = 3,
       GOAL_STATE_DONE = 4, GOAL_STATE_REWARDED = 5 };

/* Reward types (GameReward.type), gameApplyReward 0x00411090 (jump table 0x004111E4). */
enum {
    GAME_REWARD_NONE = 0, GAME_REWARD_BIKE = 1, GAME_REWARD_STYLE = 2, GAME_REWARD_RIDER = 3,
    GAME_REWARD_LEVEL = 4, GAME_REWARD_SPONSOR = 5, GAME_REWARD_TIP = 6, GAME_REWARD_TRICK_TIP = 7,
    GAME_REWARD_MOVIE = 8, GAME_REWARD_PER_RIDER = 9, GAME_REWARD_CHEAT = 10
    /* 5, 6, 7: no effect in gameApplyReward, only a text on the RESULTS screen (DM-054,
     * overlay.c overlayAddReward): 5 sponsor / magazine cover (per rider, table 0x00556998),
     * 6 a gameplay TIP!, 7 the rider's signature-trick input ("TRICKTIP!") */
};

/* Game modes (text 0x3001 + mode) and the record statistics (text 0x1094 + stat). */
enum {
    GAME_MODE_SINGLE = 0, GAME_MODE_BEST_RUN = 1, GAME_MODE_HIGH_FIVE = 2, GAME_MODE_BMX = 3,
    GAME_MODE_RANDOM_SPOT = 4, GAME_MODE_WIPEOUT = 5, GAME_MODE_LONGEST_GRIND = 6,
    GAME_MODE_HIGHEST_WALLTAP = 7, GAME_MODE_BIG_AIR = 8, GAME_MODE_FURTHEST_JUMP = 9,
    GAME_MODE_LONGEST_MANUAL = 10, GAME_MODE_DEATH_MATCH = 11
};
enum { GAME_SUB_PROQUEST = 0, GAME_SUB_SESSION = 1, GAME_SUB_FREE_RIDE = 2 };   /* single player [I] */
enum {
    STAT_TOTAL = 0, STAT_RUN = 1, STAT_TRICK_SET = 2, STAT_SINGLE_TRICK = 3, STAT_BIG_AIR = 4,
    STAT_HIGHEST_MARK = 5, STAT_LONGEST_GRIND = 6, STAT_LONGEST_JUMP = 7, STAT_LONGEST_MANUAL = 8,
    STAT_LONGEST_SKID = 9
};
/* Game states (gameSetState 0x0040AA50) */
enum {
    GAME_STATE_NONE = 0, GAME_STATE_START = 1, GAME_STATE_TURN = 3, GAME_STATE_RUN = 4,
    GAME_STATE_RUN_OVER = 5, GAME_STATE_LEVEL_END = 6
};
/* g_gameFlags bits */
enum {
    GAME_FLAG_OVER = 0x01, GAME_FLAG_ROUND_DONE = 0x02, GAME_FLAG_SETTER = 0x04, GAME_FLAG_TIE = 0x08,
    GAME_FLAG_MISSED = 0x10
};

/* Record ids (gameGetLiveRecord / comdataGetRecordPtr):
 * mode << 24 | sub << 22 | stat << 16 | slot << 12 | index. Slots of the live table:
 * 0 all-time (loaded from the save), 1 this game, 2 this player turn, 3 this run [I]. */
#define GAME_RECORD_ID(mode, sub, stat, slot, idx) \
    (((uint32_t)(mode) << 24) | ((uint32_t)(sub) << 22) | ((uint32_t)(stat) << 16) | \
     ((uint32_t)(slot) << 12) | (uint32_t)(idx))
#define GAME_RECORD_MODE(id)   ((id) >> 24)
#define GAME_RECORD_SUB(id)    (((id) >> 22) & 3)
#define GAME_RECORD_STAT(id)   (((id) >> 16) & 0x3F)
#define GAME_RECORD_SLOT(id)   (((id) >> 12) & 0xF)
#define GAME_RECORD_INDEX(id)  ((id) & 0xFFF)

/* comdata.c */
void      comdataInitDefaults(void);
SaveRecordEntry *comdataGetRecordPtr(uint32_t id);
SaveGoalInfo *comdataGetGoalInfo(int player, uint32_t level);

/* game.c (exported) */
uint8_t   gameGetMode(void);
uint8_t   gameGetSubMode(void);
uint8_t   gameGetState(void);
uint8_t   gameGetRound(void);
uint8_t   gameGetFirstPlayer(void);
uint8_t   gameGetNumRounds(void);
int       gameTestFlag(uint8_t mask);
void      gameSetFlag(uint8_t mask, int on);
uint8_t   gameSetState(int state);
GameLiveRecord *gameGetLiveRecord(uint32_t id);
uint8_t   gameGetObjectiveState(int player, int goal, uint32_t obj);
void      gameSetObjectiveState(int player, int goal, uint32_t obj, int state);
uint8_t   gameGetObjectiveFlag(int player, int goal, uint32_t obj, uint8_t mask);
void      gameSetObjectiveFlag(int player, int goal, uint32_t obj, uint8_t bit, int set);
void      gameApplyReward(int player, const GameReward *reward);

extern const GoalTierDef      g_goalDefs[18];      /* 0x00555118 [level * 3 + tier] */
extern const GameRewardTables g_rewards;           /* 0x005568B8 */
extern uint8_t  g_currentLevel;                    /* 0x0056B614 */
extern uint8_t  g_currentPlayer;                   /* 0x0056B615 */
extern uint8_t  g_numPlayers;                      /* 0x0069621C (right after g_saveBlockA) */
/* GHIDRA-SKIP-END */

#endif /* GAME_H */

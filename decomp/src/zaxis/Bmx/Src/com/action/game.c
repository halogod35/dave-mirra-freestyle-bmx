/*
 * zaxis/Bmx/Src/com/action/game.c
 * Original: C:\zaxis\Bmx\Src\com\action\game.c
 * Evidence: [V] path string 0x00554D04 referenced by 11 functions; the others [I] (link order between
 *           fx.c and memcard.c, the game globals 0x0056B6E8-0x0056BC33, the goal/reward tables).
 * 47 functions, VA 0x004099c0-0x00412011 (gameSetWord 0x0040BFD0 added by DM-058).
 * DM-046 (draft). Non-matching C (AD-8). Types: game.h, rider.h, save.h.
 * Behavioural reference: docs/formats/SAVE.md, tools/dmbmx/formats/save_fields.py; Python port of the
 * goal/reward/score logic: tools/dmbmx/formats/progress.py (tests/test_decomp_dm046.py).
 *
 * The game-flow layer between the menus and the rider simulation:
 *  - the game state machine (gameSetState / gameUpdate): game modes (single player = proquest /
 *    session / free ride, and the multiplayer games: best run, high five, B-M-X, random spot,
 *    wipeout, longest grind, highest walltap, big air, furthest jump, longest manual, death match),
 *    rounds and turns, the run timer, level end and saving;
 *  - tricks: building the trick set while the rider is in the air (gameUpdateTricks), landing it
 *    (gameRecordLandedTricks), scoring it (gameScoreTrickSet), the per-area statistics and the
 *    competition judges (gameJudgeRun);
 *  - records: the live record table (4 slots x 24 entries) and its twin in SAVE.DAT
 *    (comdataGetRecordPtr), gameSubmitRecord;
 *  - challenges: the goal definitions g_goalDefs (6 levels x 3 tiers x 4 goals x <= 4 objectives),
 *    objective tracking (gameUpdateObjectives + the prop/transfer callbacks), the objective nibbles in
 *    the rider's career record (gameGet/SetObjectiveState/Flag), tier advance, rewards
 *    (gameAwardLevelProgress, gameApplyReward), tutorial hints, objective highlights.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "decomp.h"
#include "zmath.h"     /* DM-045 names; float arrays are passed where it declares RwV3d / RwMatrix */
#include "physics.h"   /* DM-048: bmxphysGetState (BMXPHYS_*), bmxphysFindRespawnFace, bmxphysUpdate */
#include "camera.h"    /* DM-051: cameraSetPlayer, cameraSetMode (CAM_MODE_*) */

#define GAME_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\game.c"
#define FRAMES_PER_SECOND 30

/* ------------------------------------------------------------------ other modules (current names) */
uint32_t riderGetState(const BmxRider *r, uint32_t level);        /* 0x00431DD0 state machine state, level 0..2 */
void     riderReset(int slot, int active);                        /* 0x00431AA0 back to the start, shown or hidden */
void     riderUpdateAll(void);                                    /* 0x00431000 */
uint32_t actionGetMode(void);                                    /* 0x004056A0 non-zero: pause menu / replay / demo */
void     overlaySetItem(int item, const void *data);            /* set a HUD item */
void     overlayGetItem(int item, void *data);                  /* read a HUD item back */
void     overlaySetScreen(uint32_t screen);                       /* open an overlay screen */
void     bmxsoundEvent(int cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
float   *zrollerGetWorldBounds(void);                              /* level bounding box min[4], max[4] */
void     zrollerAddDrawCallback(int (*draw)(void));                 /* per-frame draw callback */
uint8_t *zrollerGetPlayer(uint32_t player);                   /* player options; +7 difficulty [I] */
void     zrollerGetPlayerStartPos(int which, float *pos, int16_t *angles);
void     zbspFaceCentroid(void *ground, float *pos);             /* snap pos onto ground */
/* object lists for the highlights: NULL-terminated arrays of pointers (copied, 0x100 bytes) */
void    *zbspFindTaggedFaces(uint32_t lo, uint32_t hi);             /* tagged edges, tag lo..hi */
void    *zbspFindPropsByGroup(uint32_t lo, uint32_t hi);             /* props of group lo..hi */
void    *zbspFindTaggedEdges(uint32_t lo, uint32_t hi);             /* BSP objects, tag lo..hi */
void     zresGetMap(uint8_t group);                        /* level resource group */
void    *zcolGetPropShape(int16_t shape);                    /* collision shape {type, size[3], yoff} */
void     psxDrawPoly3D(int prim, int n, void *verts, int flags);  /* draw polygon */
void     psxDrawLine3D(int prim, void *a, void *b);            /* draw line */
int      memcard_WriteSave(void);

/* ------------------------------------------------------------------ globals */
float    g_gameSpotPos[3];          /* 0x0056B6E8 random-spot start */
uint8_t  g_gameReplayShared;        /* 0x0056B6F8 */
uint8_t  g_gameReplayView;          /* 0x0056B6F9 replay shown by "view replay" */
uint8_t  g_gameReplay[4];           /* 0x0056B6FC one per player */
uint8_t  g_gameFlags;               /* 0x0056B700 GAME_FLAG_* */
uint8_t  g_gameMode;                /* 0x0056B701 GAME_MODE_*, 0xFF none */
uint8_t  g_gameSubMode;             /* 0x0056B702 */
uint8_t  g_gameRound;               /* 0x0056B703 */
uint8_t  g_gameState;               /* 0x0056B704 GAME_STATE_* */
uint8_t  g_gamePhase;               /* 0x0056B705 phase of the current turn (gameUpdate) */
uint8_t  g_gamePhaseTimer;          /* 0x0056B706 */
uint8_t  g_gameFirstPlayer;         /* 0x0056B707 */
uint32_t g_gameTimer;               /* 0x0056B708 frames left in the run */
GameLiveRecord g_gameLive[4][24];   /* 0x0056B70C */
uint8_t  g_gameRoundWinner[15];     /* 0x0056BA0C */
uint8_t  g_gameLeader;              /* 0x0056BA1B */
char     g_gameWord[12];            /* 0x0056BA1C B-M-X word ("BMX" by default) */
void    *g_gameHighlightA[64];      /* 0x0056BA28 objects highlighted for the active objective */
void    *g_gameHighlightB[64];      /* 0x0056BB28 second group (transfer targets) */
uint16_t g_gameAreaSize[2];         /* 0x0056BC28 width/depth of one of the 4 x 4 areas */
uint32_t g_gameFrame;               /* 0x0056BC2C highlight blink counter */
uint32_t g_gamePaused;              /* 0x0056BC30 timer frozen while non-zero [I] */

uint8_t  g_judgeScores[10][3];      /* 0x00695200 per rider: competition run 1, run 2, average */
uint8_t  g_compRanking[10];         /* 0x0069521E riders sorted by average */
BmxTrickRec g_bmxTarget[3];         /* 0x00695260 B-M-X: the set to copy */
uint8_t  g_bmxTargetCount;          /* 0x00695290 */
uint32_t g_gameSaveDone;            /* 0x006966DC set after the level-end save */

/* constant tables */
extern const uint8_t  g_gameModeStat[12];      /* 0x00553658 main record stat per mode: 1,1,1,1,0,1,6,5,4,7,8,1 */
extern const uint8_t  g_gameModeRounds[12];    /* 0x00554C8C 1,1,5,15,5,3,3,3,3,3,3,1 */
extern const uint32_t g_gameDefaultTime;       /* 0x00554CB0 120 (seconds) */
extern const uint32_t g_gameTierColour[3];     /* 0x00554CB4 highlight colour per tier */
extern const int8_t   g_judgeBias[10];         /* 0x00554CC0 per rider: 10,8,2,4,6,-10,-2,-4,0,2 */
extern const uint32_t g_compParScore[6];       /* 0x00554C98 levels 6..11: 40000, 50000, 60000, 45000, 55000, 70000 */
extern const char     g_gameDefaultWord[4];    /* 0x00554D00 "BMX" */
extern const uint8_t  g_levelResGroup[12];     /* 0x00551FB8 */
extern const uint8_t  g_trickFlags[28];        /* 0x00558FB4 air tricks: bits 0-2 flip kind, bit 3 */
extern const GameTrickInfo g_trickInfo[85];    /* 0x00559118 [trick 0..45], [46] no modifier, [47 + mod] */
extern const uint16_t g_spinPoints[9];         /* 0x0055926C 0,100,200,500,1000,2000,3500,5000,7500 */
extern const float    g_modMult[39];           /* 0x00559280 [mod + 1] */
extern const float    g_spinMult[9];           /* 0x0055931C 1,1.1,1.25,1.5,2,3,5,7,10 */
extern const float    g_repeatMult[4];         /* 0x00559340 1,0.75,0.5,0.25 */
extern const float    g_varietyMult[9];        /* 0x0055934C 0.25,1,1.5,1.75,2,2.25,2.5,2.75,3 */
extern const float    g_modCountMult[8];       /* 0x00559370 1,1,2,3,4,5,5,5 */

extern RwV3d g_zmathOrigin;                    /* 0x006702D8 (0,0,0) [I]: the middle point of zmathTriangleNormal */

/* this file */
int      gameDrawObjectiveHighlights(void);
void     gameUpdateTricks(int player);
void     gameUpdateHints(int player);
void     gameUpdateObjectives(int player);
void     gameRecordLandedTricks(int player, int landed);
void     gameSubmitRecord(int player, uint32_t stat, uint32_t value, uint8_t msgId, uint32_t flags);
int      gameRiderInTrick(const BmxRider *r);
void     gameResetRider(int player, int newRun);
void     gameNullPlayerHook(int player);
int      gameIsTierComplete(int player);
void     gameAdvanceGoalTier(int player);
void     gameResolvePendingObjectives(int player, int landed);
void     gameScoreTrickSet(BmxScoreBlock *set);
void     gameJudgeRun(int player);
void     gameAwardLevelProgress(int player);
uint8_t  gameGetRoundWinner(uint32_t round, uint32_t which);
void     gameDrawEdgeHighlight(const float *edge, uint32_t colour);
void     gameDrawObjectHighlight(const uint8_t *object, uint32_t colour);
void     gameDrawPropHighlight(const int16_t *prop, uint32_t colour);
void     gameDrawPropHighlightSolid(const int16_t *prop, uint32_t colour);
static int iabs(int v);

/* trick id ranges (g_trickNames 0x00559390) */
#define TRICK_NONE        0xFF
#define TRICK_IS_AIR(t)   ((t) < 0x1B)                    /* turndown .. superman indian */
#define TRICK_IS_GRIND(t) ((t) >= 0x1B && (t) < 0x24)     /* double peg grind .. sprocket grind */
#define TRICK_IS_STALL(t) ((t) >= 0x24 && (t) < 0x2A)     /* double peg stall .. disaster */
#define TRICK_IS_PLANT(t) ((t) == 0x2A || (t) == 0x2B)    /* fast plant, wall tap */
#define TRICK_IS_MANUAL(t) ((t) == 0x2C || (t) == 0x2D)   /* manual, nose wheelie */

#define RIDER_OF(player) (&g_riders[(player)])
#define CAREER(player)   (&g_saveBlockA.riders[g_saveProfiles[(player)].rider])

static int gameIsActivePlayer(uint32_t p)
{
    /* in death match every player rides at once; otherwise only the one whose turn it is */
    return p == g_currentPlayer || gameGetMode() == GAME_MODE_DEATH_MATCH;
}

static int gameIsProquest(void)
{
    return gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_PROQUEST;
}

/* The goal-entry byte of goal g in the current tier (SAVE.md 2.2): 0x80 done, 0x40 this session,
 * low 6 bits objectives done. */
#define GOAL_BYTE(gi, g) ((gi)->goal[(gi)->tier][(g)][0])

/* ------------------------------------------------------------------ setup */

/* VA 0x004099c0, 615 bytes, [V] direct, status draft */
void gameInit(void)
{
    /* Level load: replays, the all-time records into slot 0 of the live table, the area grid. */
    BmxReplayDesc desc;
    uint32_t p, mode, sub, stat, idx;
    float *box;
    int w, h;

    if (g_numPlayers == 0) {                 /* attract mode */
        g_gameMode = 0;
        g_gameState = 4;
    } else {
        for (p = 0; p < g_numPlayers; p++) {
            desc.flags = (g_numPlayers > 1) ? 7 : 1;
            desc.player = (uint8_t)p;
            desc.size = 0x1000 / g_numPlayers;
            g_gameReplay[p] = bmxreplayCreate(&desc);
            if (g_gameReplay[p] == (uint8_t)-1)
                zAssertFail("Failed to create replay (probably out of RAM).", GAME_SRC, 0xde, 0);
            /* clear the "new bike / style / level" notices of the rider */
            g_saveBlockA.riders[g_saveProfiles[p].rider].flags &= ~0x1C0u;
        }
        if (g_numPlayers == 1) {
            g_gameReplayShared = g_gameReplay[0];
        } else {
            desc.flags = 7;
            desc.size = 0x1000 / g_numPlayers;
            desc.player = 0;
            g_gameReplayShared = bmxreplayCreate(&desc);
            if (g_gameReplayShared == (uint8_t)-1)
                zAssertFail("Failed to create replay (probably out of RAM).", GAME_SRC, 0xea, 0);
        }
        g_gameReplayView = g_gameReplay[0];
        *(uint32_t *)g_gameWord = *(const uint32_t *)g_gameDefaultWord;
        g_gameMode = 0xFF;
    }
    g_gameSubMode = 0;

    /* every (mode, sub, stat, index) that has a live record gets the saved value of this level */
    for (mode = 0; mode < 12; mode++)
        for (sub = 0; sub < 4; sub++)
            for (stat = 0; stat < 10; stat++)
                for (idx = 0; idx < 3; idx++) {
                    uint32_t id = GAME_RECORD_ID(mode, sub, stat, 0, idx);
                    GameLiveRecord *live = gameGetLiveRecord(id);
                    if (live != NULL) {
                        SaveRecordEntry *saved = comdataGetRecordPtr(id);
                        if (saved == NULL)
                            zAssertFail("no stored high score entry found", GAME_SRC, 0xfe, 0);
                        live->flags = 0;
                        live->value = saved->value;
                    }
                }

    /* the level is split into 4 x 4 areas for the judges' "areas used" (gameRecordLandedTricks) */
    box = zrollerGetWorldBounds();
    w = (int)(box[4] - box[0]);
    h = (int)(box[6] - box[2]);
    g_gameAreaSize[0] = (uint16_t)((w + 3) / 4);
    g_gameAreaSize[1] = (uint16_t)((h + 3) / 4);
    zrollerAddDrawCallback(gameDrawObjectiveHighlights);
}

/* VA 0x00409cd0, 62 bytes, [I] between, status draft */
void gameShutdown(void)
{
    int p;

    if (g_numPlayers == 0)
        return;
    if (g_numPlayers != 1)
        bmxreplayDestroy(g_gameReplayShared);
    for (p = g_numPlayers - 1; p >= 0; p--)
        bmxreplayDestroy(g_gameReplay[p]);
}

/* ------------------------------------------------------------------ per frame */

static void gameTickTimer(void)
{
    /* one frame off the run timer, with a beep at 5, 4, 3, 2, 1 and 0 seconds left */
    uint32_t old = g_gameTimer;
    g_gameTimer = old - 1;
    switch (old) {
    case 1: case 31: case 61: case 91: case 121: case 151:
        bmxsoundEvent(5000, g_gameTimer / FRAMES_PER_SECOND, 0, 0, 0);
        break;
    }
}

/* VA 0x00409d20, 1066 bytes, [I] between, status draft */
void gameUpdate(void)
{
    /* Per frame while a level runs: tricks, hints, objectives, then the turn phases:
     * 0 waiting for the first move, 1 riding, 2 time up (waiting for the last trick to land),
     * 3 landed / finished, 4 bailed (a 2 s pause), 5 over -> gameSetState(GAME_STATE_RUN_OVER). */
    uint32_t p = g_currentPlayer;
    BmxRider *r = RIDER_OF(p);
    uint8_t hud8, hud7;
    uint32_t timer;
    int mode;

    zresGetMap(g_levelResGroup[g_currentLevel]);
    gameUpdateTricks(r->slot);
    if (gameGetMode() == GAME_MODE_SINGLE)
        gameUpdateHints(r->slot);
    gameUpdateObjectives(r->slot);
    hud8 = r->hud4e;
    overlaySetItem(8, &hud8);
    hud7 = (riderGetState(r, 0) == 6) ? r->crashKind : 0xFF;
    overlaySetItem(7, &hud7);

    mode = gameGetMode();
    if (mode != GAME_MODE_SINGLE) {
        /* multiplayer: one attempt per turn */
        if (mode == GAME_MODE_DEATH_MATCH || gameGetState() != GAME_STATE_RUN)
            return;
        if (g_gamePhase < 2) {
            if (actionGetMode() == 0 && g_gameTimer != 0)
                gameTickTimer();
            timer = g_gameTimer;
            overlaySetItem(2, &timer);
        }
        switch (g_gamePhase) {
        case 0:
            if (g_gameTimer == 0 || r->trickState == 1 || r->trickState == 2) {
                g_gamePhase = 1;
                return;
            }
            break;
        case 1:
            if (g_gameTimer == 0) {
                if (r->trickState != 1 && r->trickState != 2) {
                    if (r->trickState != 0)
                        gameRecordLandedTricks(g_currentPlayer, 1);
                    g_gamePhase = 5;
                    gameSetState(GAME_STATE_RUN_OVER);
                    return;
                }
                g_gamePhase = 2;
                return;
            }
            /* high five, B-M-X, random spot, wipeout: the attempt ends with the first landing */
            if (gameGetMode() > 1 && gameGetMode() < 6 && r->trickState != 1 && r->trickState != 2) {
                g_gamePhase = 2;
                return;
            }
            break;
        case 2:
            if (r->trickState == 0) {
                g_gamePhase = 4;
                g_gamePhaseTimer = 0;
                return;
            }
            if (r->trickState > 2) {
                if (r->trickState == 3) {
                    g_gamePhase = 3;
                    return;
                }
                g_gamePhase = 4;
                g_gamePhaseTimer = 0;
                return;
            }
            break;
        case 3:
            if (r->trickState == 0) {
                g_gamePhase = 5;
                gameSetState(GAME_STATE_RUN_OVER);
                return;
            }
            break;
        case 4:
            if (++g_gamePhaseTimer == 60) {
                g_gamePhase = 5;
                gameSetState(GAME_STATE_RUN_OVER);
                return;
            }
            break;
        }
        if (g_gamePhase == 5)
            gameSetState(GAME_STATE_RUN_OVER);
        return;
    }

    /* single player: a timed run (no timer in free ride) */
    if (gameGetState() != GAME_STATE_RUN)
        return;
    if (gameGetSubMode() != GAME_SUB_FREE_RIDE) {
        if (g_gamePhase < 2 && actionGetMode() == 0 && g_gameTimer != 0 && g_gamePaused == 0)
            gameTickTimer();
        timer = g_gameTimer;
        overlaySetItem(2, &timer);
    }
    switch (g_gamePhase) {
    case 0:
        if (g_gameTimer == 0 || r->trickState == 1 || r->trickState == 2) {
            g_gamePhase = 1;
            return;
        }
        break;
    case 1:
        if (g_gameTimer != 0)
            break;
        if (r->trickState == 1 || r->trickState == 2 || bmxphysGetState(r) == 1) {
            g_gamePhase = 2;                 /* time is up, but the rider is still in the air */
            return;
        }
        if (r->trickState != 0)
            gameRecordLandedTricks(g_currentPlayer, 1);
        g_gamePhase = 3;
        goto run_over;
    case 2:
        if (r->trickState == 0) {
            g_gamePhase = 3;
            goto run_over;
        }
        break;
    }
    if (g_gamePhase != 3)
        return;
run_over:
    gameSetState(GAME_STATE_RUN_OVER);
    bmxsoundEvent(0x3E9, 4, 0, 0, 0);    /* "time up" */
}

/* VA 0x0040a400, 795 bytes, [I] between, status draft */
int gameDrawObjectiveHighlights(void)
{
    /* Draw callback (registered by gameInit): blink a box around the objects of every active
     * objective (state GOAL_STATE_ACTIVE) in the tier colour. g_gameHighlightA/B were filled by
     * gameUpdateObjectives; the object kind decides the drawing routine. */
    SaveGoalInfo *gi;
    const GoalDef *def;
    uint32_t goal, obj, colour;
    void **it;

    if (actionGetMode() == 0 && gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == 0 &&
        g_currentLevel < 6) {
        gi = comdataGetGoalInfo(g_currentPlayer, g_currentLevel);
        def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
        colour = g_gameTierColour[gi->tier];
        for (goal = 0; goal < 4; goal++, def++) {
            if (GOAL_BYTE(gi, goal) & 0x80)
                continue;
            for (obj = 0; obj < def->numObjectives; obj++) {
                if (gameGetObjectiveState(g_currentPlayer, goal, obj) != GOAL_STATE_ACTIVE)
                    continue;
                switch (def->obj[obj].type) {
                case GOAL_OBJ_TRANSFER: case GOAL_OBJ_TRANSFER_TRICK: case GOAL_OBJ_TRANSFER_SPIN:
                    for (it = g_gameHighlightB; *it != NULL; it++)
                        gameDrawEdgeHighlight((float *)*it, colour);
                    /* fall through: and the take-off edges */
                case GOAL_OBJ_TRICK_OFF_EDGE: case GOAL_OBJ_STATE_ON_EDGE: case GOAL_OBJ_HIT_OBJECT:
                    for (it = g_gameHighlightA; *it != NULL; it++) {
                        void *shape = ((void **)*it)[0x2C / 4];
                        if (shape == NULL)
                            gameDrawEdgeHighlight((float *)*it, colour);
                        else
                            gameDrawPropHighlightSolid((int16_t *)shape, colour);
                    }
                    break;
                case GOAL_OBJ_TRANSFER_OBJECTS:
                    for (it = g_gameHighlightB; *it != NULL; it++)
                        gameDrawObjectHighlight(*it, colour);
                    /* fall through */
                case GOAL_OBJ_TRICK_OFF_OBJECT: case GOAL_OBJ_GRIND_OBJECT:
                    for (it = g_gameHighlightA; *it != NULL; it++)
                        gameDrawObjectHighlight(*it, colour);
                    break;
                case GOAL_OBJ_TRANSFER_GROUPS:
                    for (it = g_gameHighlightB; *it != NULL; it++)
                        gameDrawPropHighlight((int16_t *)*it, colour);
                    /* fall through */
                case GOAL_OBJ_TRICK_OVER_GROUP: case GOAL_OBJ_STATE_ON_GROUP: case GOAL_OBJ_GRIND_GROUP:
                case GOAL_OBJ_JUMP_GROUP: case GOAL_OBJ_JUMP_GROUP_TRICK: case GOAL_OBJ_COLLECT:
                    for (it = g_gameHighlightA; *it != NULL; it++)
                        gameDrawPropHighlight((int16_t *)*it, colour);
                    break;
                case GOAL_OBJ_TRANSFER_TO_OBJ:
                    for (it = g_gameHighlightA; *it != NULL; it++)
                        gameDrawEdgeHighlight((float *)*it, colour);
                    for (it = g_gameHighlightB; *it != NULL; it++)
                        gameDrawObjectHighlight(*it, colour);
                    break;
                case GOAL_OBJ_TRANSFER_TO_GROUP:
                    for (it = g_gameHighlightA; *it != NULL; it++)
                        gameDrawEdgeHighlight((float *)*it, colour);
                    for (it = g_gameHighlightB; *it != NULL; it++)
                        gameDrawPropHighlight((int16_t *)*it, colour);
                    break;
                }
            }
        }
    }
    g_gameFrame++;
    return 0;
}

/* VA 0x0040a850, 48 bytes, [I] between, status draft */
void gameSetFlag(uint8_t mask, int on)
{
    if (on == 0)
        g_gameFlags &= ~mask;
    else
        g_gameFlags |= mask;
}

/* VA 0x0040a890, 18 bytes, [I] between, status draft */
int gameTestFlag(uint8_t mask)
{
    return (g_gameFlags & mask) != 0;
}

/* VA 0x0040a8c0, 227 bytes, [I] between, status draft */
uint8_t gameStart(uint8_t mode)
{
    /* A new game of `mode` on the loaded level (action.c, after the game-select menu). */
    uint32_t p, stat, slot, idx;
    GameLiveRecord *live;

    g_gameFlags = 0;
    g_gameMode = mode;
    g_gameRound = 0;
    g_gameState = 0xFF;
    g_gameFirstPlayer = 0;
    g_currentPlayer = gameGetFirstPlayer();
    for (p = 0; p < 2; p++) {
        g_riders[p].score.totalScore = 0;
        g_riders[p].score.wins = 0;
    }
    /* reset the "this game" records (slot 1) */
    for (stat = 0; stat < 10; stat++)
        for (idx = 0; idx < 3; idx++) {
            live = gameGetLiveRecord(GAME_RECORD_ID(mode, gameGetSubMode(), stat, 1, idx));
            if (live != NULL) {
                live->value = 0;
                live->flags = 0;
            }
        }
    /* and the "beaten" flags of the total-score entries of every slot */
    for (slot = 0; slot < 4; slot++)
        for (idx = 0; idx < 3; idx++) {
            live = gameGetLiveRecord(GAME_RECORD_ID(mode, gameGetSubMode(), STAT_TOTAL, slot, idx));
            if (live != NULL)
                live->flags = 0;
        }
    gameSetFlag(GAME_FLAG_TIE, mode != 0);
    gameSetState(GAME_STATE_START);
    return g_gameMode;
}

/* VA 0x0040a9f0, 8 bytes, [I] between, status draft */
uint8_t gameGetMode(void)
{
    return g_gameMode;
}

/* VA 0x0040aa10, 15 bytes, [I] between, status draft */
uint8_t gameSetSubMode(uint8_t sub)
{
    g_gameSubMode = sub;
    return sub;
}

/* VA 0x0040aa30, 8 bytes, [I] between, status draft */
uint8_t gameGetSubMode(void)
{
    return g_gameSubMode;
}

/* ------------------------------------------------------------------ the state machine */

static void gameLiveBeat(uint32_t id, uint32_t value, uint32_t player)
{
    GameLiveRecord *live = gameGetLiveRecord(id);
    if (live != NULL && live->value < value) {
        live->value = value;
        live->player = (uint8_t)player;
        live->flags |= 1;
    }
}

static int gameAnyNewRecord(void)
{
    /* an all-time record (slot 0) of this mode was beaten: the name-entry screen comes next */
    uint32_t stat;
    for (stat = 0; stat < 10; stat++) {
        GameLiveRecord *live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
        if (live != NULL && (live->flags & 1))
            return 1;
    }
    return 0;
}

static uint8_t gameShowScreen(int screen)
{
    overlaySetScreen(screen);
    cameraSetPlayer(0);
    cameraSetMode(0, 4);
    return g_gameState;
}

/* VA 0x0040aa50, 3797 bytes, [V] direct, status draft */
uint8_t gameSetState(int state)
{
    uint32_t p, stat, slot;
    BmxRider *r;
    GameLiveRecord *live;
    uint8_t hud[8];
    uint32_t hud1[2];

    /* GAME_STATE_RUN_OVER -> GAME_STATE_TURN: the next player's turn (or the next round) */
    if (g_gameState == GAME_STATE_RUN_OVER && gameGetMode() != GAME_MODE_DEATH_MATCH &&
        state == GAME_STATE_TURN) {
        g_currentPlayer = (uint8_t)((g_currentPlayer + 1) % g_numPlayers);
        if (gameTestFlag(GAME_FLAG_ROUND_DONE)) {
            /* B-M-X: the winner of a round sets the next trick, the player after him starts */
            if (gameGetMode() == GAME_MODE_BMX && !gameTestFlag(GAME_FLAG_MISSED))
                g_gameFirstPlayer = (uint8_t)((gameGetRoundWinner(gameGetRound(), 2) + 1) % g_numPlayers);
            g_currentPlayer = gameGetFirstPlayer();
            if ((gameGetMode() >= GAME_MODE_WIPEOUT && gameGetMode() <= GAME_MODE_LONGEST_MANUAL) ||
                !gameTestFlag(GAME_FLAG_MISSED))
                g_gameRound++;
        }
    }
    g_gameState = (uint8_t)state;
    g_gamePhase = 0;
    g_gameTimer = 0;

    switch (state) {
    case GAME_STATE_START:
        if (gameGetMode() == GAME_MODE_SINGLE) {
            cameraSetMode(g_currentPlayer, 4);
            overlaySetScreen(12);
            bmxsoundEvent(0x3E9, 4, 0, 0, 0);
            return g_gameState;
        }
        overlaySetScreen(12);
        return g_gameState;

    case GAME_STATE_TURN:
        overlaySetScreen(0);
        for (stat = 0; stat < 10; stat++) {
            live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 3, 0));
            if (live != NULL) {
                live->value = 0;
                live->flags = 0;
            }
            live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
            if (live != NULL)
                live->flags = 0;
        }
        if (g_currentPlayer == gameGetFirstPlayer()) {
            /* a new round */
            gameSetFlag(GAME_FLAG_SETTER, 1);
            for (stat = 0; stat < 10; stat++) {
                live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 2, 0));
                if (live != NULL) {
                    live->value = 0;
                    live->flags = 0;
                }
            }
            for (slot = 0; slot < 4; slot++)
                for (stat = 0; stat < 10; stat++) {
                    live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, slot, 0));
                    if (live != NULL)
                        live->flags = 0;
                }
            if (gameGetMode() == GAME_MODE_RANDOM_SPOT) {
                /* pick a random spot on the ground inside the level box */
                float *box = zrollerGetWorldBounds();
                void *ground;
                g_gameSpotPos[1] = 0.0f;
                do {
                    int w = (int)(box[4] - box[0]);
                    int d = (int)(box[6] - box[2]);
                    g_gameSpotPos[0] = (float)(int16_t)(((zmathRandom() & 0xFF) * w) >> 8) + box[0];
                    g_gameSpotPos[2] = (float)(int16_t)(((zmathRandom() & 0xFF) * d) >> 8) + box[2];
                    ground = bmxphysFindRespawnFace((RwV3d *)g_gameSpotPos, 3);
                } while (ground == NULL);
                zbspFaceCentroid(ground, g_gameSpotPos);
            }
            gameSetFlag(GAME_FLAG_ROUND_DONE, 0);
            gameSetFlag(GAME_FLAG_MISSED, 0);
            g_bmxTargetCount = 0;
        } else {
            gameSetFlag(GAME_FLAG_SETTER, 0);
        }
        for (p = 0; p < g_numPlayers; p++) {
            r = RIDER_OF(p);
            if (!gameIsActivePlayer(p)) {
                riderReset(p, 0);
                continue;
            }
            gameResetRider(p, 1);
            gameNullPlayerHook(p);
            r->profileByte2 = g_saveProfiles[p].unk_02;
            r->score.runScore = 0;
            r->trickState = 0;
            r->trickCount = 0;
            zstringMemSet(r->trickLog, 0, sizeof(r->trickLog));
            hud1[0] = 0;
            hud1[1] = 1;
            overlaySetItem(1, hud1);
            zstringMemSet(r->area, 0, sizeof(r->area) + sizeof(r->total));
            r->hud4e = 0xFF;
            hud[0] = 0xFF;
            overlaySetItem(8, hud);
            if (gameGetMode() == GAME_MODE_SINGLE && (g_saveProfiles[p].flags & 2))
                r->hintMask = 0x003FFFFF;   /* all 22 tutorial hints */
        }
        cameraSetPlayer(g_currentPlayer);
        cameraSetMode(g_currentPlayer, 3);
        hud[0] = 0xFF; overlaySetItem(10, hud);
        hud[0] = 0xFF; overlaySetItem(11, hud);
        hud[0] = 0xFF; overlaySetItem(7, hud);
        hud[0] = 0xFF; overlaySetItem(12, hud);
        g_gameTimer = 300;
        if (gameGetMode() == GAME_MODE_SINGLE) {
            if (gameGetSubMode() == GAME_SUB_PROQUEST && g_currentLevel < 6) {
                overlaySetScreen(13);           /* the challenge list */
                return g_gameState;
            }
        } else if (gameGetMode() != GAME_MODE_DEATH_MATCH) {
            overlaySetScreen(24);               /* "player n" */
            bmxsoundEvent(0x3E9, 0, 0, 0, 0);
            return g_gameState;
        }
        return gameSetState(GAME_STATE_RUN);

    case GAME_STATE_RUN:
        for (p = 0; p < g_numPlayers; p++) {
            if (!gameIsActivePlayer(p))
                continue;
            r = RIDER_OF(p);
            r->trickState = 0;
            r->trickCount = 0;
            bmxreplaySetPlayer(g_gameReplay[p], p);
            bmxreplaySetMode(g_gameReplay[p], 1);
            if (gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(),
                                                 g_gameModeStat[gameGetMode()], 0, 0)) == NULL)
                zAssertFail("", GAME_SRC, 0x415, 0);
            cameraSetPlayer(p);
            cameraSetMode(p, 0);
        }
        switch (gameGetMode()) {
        case GAME_MODE_SINGLE:
            g_gameTimer = g_gameDefaultTime * FRAMES_PER_SECOND;   /* 2 minutes */
            bmxsoundEvent(0x3E9, 1, 0, 0, 0);
            return g_gameState;
        case GAME_MODE_BEST_RUN:
        case GAME_MODE_DEATH_MATCH:
            switch (gameGetSubMode()) {
            case 0:  break;                                         /* 30 s */
            case 1:  g_gameTimer = 60 * FRAMES_PER_SECOND;  return g_gameState;
            case 3:  g_gameTimer = 300 * FRAMES_PER_SECOND; return g_gameState;
            default: g_gameTimer = g_gameDefaultTime * FRAMES_PER_SECOND; return g_gameState;
            }
            break;
        case GAME_MODE_RANDOM_SPOT:
            g_gameTimer = 15 * FRAMES_PER_SECOND;
            return g_gameState;
        }
        g_gameTimer = 30 * FRAMES_PER_SECOND;
        return g_gameState;

    case GAME_STATE_RUN_OVER:
        for (p = 0; p < g_numPlayers; p++)
            if (gameIsActivePlayer(p))
                bmxreplaySetMode(g_gameReplay[p], 0);
        g_gameReplayView = g_gameReplay[g_currentPlayer];
        if (gameGetMode() == GAME_MODE_BMX && gameTestFlag(GAME_FLAG_SETTER)) {
            if (RIDER_OF(g_currentPlayer)->score.runScore == 0) {
                /* the setter missed: the next player sets */
                g_gameFirstPlayer = (uint8_t)((g_gameFirstPlayer + 1) % g_numPlayers);
                gameSetFlag(GAME_FLAG_MISSED, 1);
                gameSetFlag(GAME_FLAG_ROUND_DONE, 1);
                return gameSetState(GAME_STATE_TURN);
            }
            g_gameRoundWinner[gameGetRound()] = g_currentPlayer;   /* who set this round's trick */
        }
        if (gameGetMode() != GAME_MODE_SINGLE || gameGetSubMode() != GAME_SUB_FREE_RIDE) {
            for (p = 0; p < g_numPlayers; p++) {
                if (!gameIsActivePlayer(p))
                    continue;
                for (slot = 0; slot < 3; slot++)
                    gameLiveBeat(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_RUN, slot, 0),
                                 RIDER_OF(p)->score.runScore, p);
                gameJudgeRun(p);
            }
        }
        if (gameGetMode() < GAME_MODE_HIGH_FIVE || gameGetMode() > GAME_MODE_BMX) {
            r = RIDER_OF(g_currentPlayer);
            if (gameGetMode() == GAME_MODE_SINGLE) {
                r->score.totalScore = r->score.runScore;
            } else if (gameGetMode() < GAME_MODE_WIPEOUT || gameGetMode() > GAME_MODE_LONGEST_MANUAL) {
                for (p = 0; p < g_numPlayers; p++)
                    if (gameIsActivePlayer(p))
                        RIDER_OF(p)->score.totalScore += RIDER_OF(p)->score.runScore;
            } else if (r->score.totalScore < r->score.runScore) {
                r->score.totalScore = r->score.runScore;  /* best attempt */
            }
            if (gameGetMode() != GAME_MODE_SINGLE || gameGetSubMode() != GAME_SUB_FREE_RIDE) {
                for (p = 0; p < g_numPlayers; p++) {
                    if (!gameIsActivePlayer(p))
                        continue;
                    for (slot = 0; slot < 3; slot++)
                        gameLiveBeat(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_TOTAL, slot, 0),
                                     RIDER_OF(p)->score.totalScore, p);
                }
            }
        }
        if ((g_currentPlayer + 1) % g_numPlayers == gameGetFirstPlayer()) {
            /* the last player of the round */
            gameSetFlag(GAME_FLAG_ROUND_DONE, 1);
            if (gameGetMode() == GAME_MODE_BMX) {
                uint32_t len = (uint32_t)strlen(g_gameWord), best = len, out = 0;
                gameSetFlag(GAME_FLAG_MISSED, 1);
                for (p = 0; p < g_numPlayers; p++) {
                    r = RIDER_OF(p);
                    if (r->score.runScore == 0) {        /* missed the trick: a letter */
                        uint32_t letters = r->score.wins++;
                        if (strlen(g_gameWord) <= letters + 1)
                            out++;
                        gameSetFlag(GAME_FLAG_MISSED, 0);
                    } else if (r->score.wins < best) {
                        g_gameLeader = (uint8_t)p;
                        best = r->score.wins & 0xFF;
                    }
                }
                gameSetFlag(GAME_FLAG_OVER, out == (uint32_t)g_numPlayers - 1);
            } else {
                live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_RUN, 2, 0));
                if (live != NULL) {
                    gameSetFlag(GAME_FLAG_MISSED, 0);
                    for (p = 0; p < g_numPlayers; p++)
                        if (p != live->player && RIDER_OF(p)->score.runScore == live->value)
                            gameSetFlag(GAME_FLAG_MISSED, 1);   /* a tie: nobody wins the round */
                    if (!gameTestFlag(GAME_FLAG_MISSED)) {
                        RIDER_OF(live->player)->score.wins++;
                        g_gameRoundWinner[gameGetRound()] = live->player;
                    }
                }
                if (gameGetMode() == GAME_MODE_HIGH_FIVE) {
                    if (!gameTestFlag(GAME_FLAG_MISSED)) {
                        uint32_t best = 0;
                        g_gameLeader = 0;
                        gameSetFlag(GAME_FLAG_TIE, 0);
                        for (p = 0; p < g_numPlayers; p++) {
                            uint32_t w = RIDER_OF(p)->score.wins;
                            if (best < w) {
                                g_gameLeader = (uint8_t)p;
                                best = w & 0xFF;
                                gameSetFlag(GAME_FLAG_TIE, 0);
                            } else if (w == best) {
                                gameSetFlag(GAME_FLAG_TIE, 1);
                            }
                        }
                        if (gameGetNumRounds() != 0 && gameGetRound() == gameGetNumRounds() - 1)
                            gameSetFlag(GAME_FLAG_OVER, 1);
                    }
                } else if (gameGetNumRounds() != 0 && gameGetRound() == gameGetNumRounds() - 1) {
                    gameSetFlag(GAME_FLAG_OVER, 1);
                }
            }
            if (gameGetMode() != GAME_MODE_SINGLE) {
                live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(),
                                                        g_gameModeStat[gameGetMode()], 1, 0));
                if (live == NULL)
                    zAssertFail("", GAME_SRC, 0x4ee, 0);
                if (live->flags & 1) {
                    /* keep the replay of this game's best attempt as the shared one */
                    uint8_t tmp = g_gameReplay[live->player];
                    g_gameReplay[live->player] = g_gameReplayShared;
                    g_gameReplayShared = tmp;
                }
            }
        }
        for (p = 0; p < g_numPlayers; p++)
            riderReset(p, 0);
        if (!gameTestFlag(GAME_FLAG_OVER) && gameAnyNewRecord())
            return gameShowScreen(17);          /* NEW RECORD! enter your name */
        switch (gameGetMode()) {
        case GAME_MODE_SINGLE:
            if (gameTestFlag(GAME_FLAG_OVER))
                return gameSetState(GAME_STATE_LEVEL_END);
            return gameShowScreen(8);           /* STANDINGS (only competitions get here: 2 runs) */
        case GAME_MODE_BMX:
            if (gameTestFlag(GAME_FLAG_MISSED) || !gameTestFlag(GAME_FLAG_ROUND_DONE))
                return gameSetState(GAME_STATE_TURN);
            /* fall through */
        default:
            cameraSetPlayer(g_currentPlayer);
            cameraSetMode(g_currentPlayer, 4);
            overlaySetScreen(25);               /* "TIME UP" / winner (overlymp.c) */
            return g_gameState;
        case GAME_MODE_WIPEOUT: case GAME_MODE_LONGEST_GRIND: case GAME_MODE_HIGHEST_WALLTAP:
        case GAME_MODE_BIG_AIR: case GAME_MODE_FURTHEST_JUMP: case GAME_MODE_LONGEST_MANUAL:
            if (!gameTestFlag(GAME_FLAG_OVER))
                return gameSetState(GAME_STATE_TURN);
            return gameSetState(GAME_STATE_LEVEL_END);
        }

    case GAME_STATE_LEVEL_END:
        if (gameGetMode() != GAME_MODE_HIGH_FIVE) {
            live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(),
                                                    g_gameModeStat[gameGetMode()], 1, 0));
            gameSetFlag(GAME_FLAG_TIE, 0);
            for (p = 0; p < g_numPlayers; p++)
                if (p != live->player && RIDER_OF(p)->score.totalScore == live->value)
                    gameSetFlag(GAME_FLAG_TIE, 1);
        }
        gameAwardLevelProgress(g_currentPlayer);
        if (gameAnyNewRecord())
            return gameShowScreen(17);
        if (gameIsProquest()) {
            if (g_currentLevel > 5)
                return gameShowScreen(8);       /* competition STANDINGS */
            if (gameIsTierComplete(g_currentPlayer))
                return gameShowScreen(16);      /* RESULTS (rewards) */
        }
        memcard_WriteSave();
        overlaySetScreen(10);
        g_gameSaveDone = 1;
        return g_gameState;
    }
    return g_gameState;
}

/* VA 0x0040bd50, 11 bytes, [I] between, status draft */
uint8_t gameGetState(void)
{
    return g_gameState;
}

/* VA 0x0040bd70, 49 bytes, [I] between, status draft */
uint8_t gameGetNumRounds(void)
{
    /* proquest competitions (levels 6..11) have 2 runs; otherwise per mode */
    if (gameIsProquest() && g_currentLevel > 5)
        return 2;
    return g_gameModeRounds[gameGetMode()];
}

/* VA 0x0040bdc0, 8 bytes, [I] between, status draft */
uint8_t gameGetRound(void)
{
    return g_gameRound;
}

/* VA 0x0040bde0, 8 bytes, [I] between, status draft */
uint8_t gameGetFirstPlayer(void)
{
    return g_gameFirstPlayer;
}

/* VA 0x0040be00, 64 bytes, [V] direct, status draft */
uint8_t gameGetRoundWinner(uint32_t round, uint32_t which)
{
    /* which < 2: the series leader; otherwise the winner (B-M-X: the setter) of `round` */
    if (which < 2)
        return g_gameLeader;
    if (round >= gameGetNumRounds())
        zAssertFail("round number out of range", GAME_SRC, 0x59a, 0);
    return g_gameRoundWinner[round];
}

/* VA 0x0040be50, 256 bytes, [I] between, status draft */
GameLiveRecord *gameGetLiveRecord(uint32_t id)
{
    /* The live twin of comdataGetRecordPtr: the same 24 entries, one set per slot. */
    GameLiveRecord *s = g_gameLive[GAME_RECORD_SLOT(id)];
    uint32_t stat = GAME_RECORD_STAT(id);
    uint32_t sub = GAME_RECORD_SUB(id);

    if (stat > 1)
        return &s[stat + 14];
    switch (GAME_RECORD_MODE(id)) {
    case GAME_MODE_SINGLE:      return &s[GAME_RECORD_INDEX(id)];
    case GAME_MODE_BEST_RUN:    return &s[3 + sub];
    case GAME_MODE_HIGH_FIVE:   if (stat == STAT_RUN) return &s[7]; break;
    case GAME_MODE_BMX:         if (stat == STAT_RUN) return &s[8]; break;
    case GAME_MODE_RANDOM_SPOT:
        if (stat == STAT_TOTAL) return &s[10];
        if (stat == STAT_RUN) return &s[9];
        break;
    case GAME_MODE_WIPEOUT:     if (stat == STAT_RUN) return &s[11]; break;
    case GAME_MODE_LONGEST_GRIND: case GAME_MODE_HIGHEST_WALLTAP: case GAME_MODE_BIG_AIR:
    case GAME_MODE_FURTHEST_JUMP: case GAME_MODE_LONGEST_MANUAL:
        return &s[g_gameModeStat[GAME_RECORD_MODE(id)] + 14];
    case GAME_MODE_DEATH_MATCH: return &s[12 + sub];
    }
    return NULL;
}

/* The B-M-X game's word (overlymp.c's word entry): at most 8 letters, copied to g_gameWord.
 * Ghidra had missed the function (DM-054's discovered work; created by a DM-058 row). */
/* VA 0x0040bfd0, 89 bytes, [V] direct, status draft */
char *gameSetWord(const char *word)
{
    if (strlen(word) > 8)
        zAssertFail("horse word too long", GAME_SRC, 0x5e4, 0);
    return strcpy(g_gameWord, word);
}

/* VA 0x0040c050, 6 bytes, [I] between, status draft */
char *gameGetWord(void)
{
    return g_gameWord;
}

/* VA 0x0040c070, 36 bytes, [I] between, status draft */
uint8_t gameGetViewReplay(void)
{
    /* the replay "view replay" shows */
    if (gameGetMode() == GAME_MODE_SINGLE)
        return g_gameReplay[0];
    if (gameGetState() == GAME_STATE_LEVEL_END)
        return g_gameReplayShared;
    return g_gameReplayView;
}

/* VA 0x0040c0b0, 164 bytes, [I] between, status draft */
void gameEndRun(void)
{
    /* "end run" from the pause menu (and the time-out path): land or bail what is in the air */
    uint32_t st = gameGetState();
    BmxRider *r;

    if (st != GAME_STATE_RUN) {
        if (st < GAME_STATE_RUN_OVER || st > GAME_STATE_LEVEL_END) {
            overlaySetScreen(0);
            return;
        }
        cameraSetPlayer(g_currentPlayer);
        cameraSetMode(g_currentPlayer, 0);
    }
    r = RIDER_OF(g_currentPlayer);
    if (r->trickState != 0) {
        if (r->trickState < 3)
            gameRecordLandedTricks(g_currentPlayer, 0);
        else if (r->trickState == 3)
            gameRecordLandedTricks(g_currentPlayer, 1);
    }
    gameSetState(GAME_STATE_RUN_OVER);
    bmxsoundEvent(0x3E9, 4, 0, 0, 0);
    bmxsoundEvent(1000, 2, 0, 0, 0);
}

/* VA 0x0040c190, 282 bytes, [I] between, status draft */
void gameResetRider(int player, int newRun)
{
    /* Put the rider at the level start (random spot: at g_gameSpotPos, 50 cm up). */
    BmxRider *r = RIDER_OF(player);
    float pos[3];
    int16_t angles[3];

    riderReset(player, newRun);
    bmxpropReset();
    if (gameGetMode() == GAME_MODE_SINGLE) {
        if (newRun) {
            if (g_currentLevel < 6)
                gameAdvanceGoalTier(player);
            r->runRewards = 0;
        }
    } else if (gameGetMode() == GAME_MODE_RANDOM_SPOT) {
        pos[0] = g_gameSpotPos[0];
        pos[1] = g_gameSpotPos[1] - 50.0f;
        pos[2] = g_gameSpotPos[2];
        angles[0] = angles[1] = angles[2] = 0;
        r->spawnPos[0] = pos[0];
        r->spawnPos[1] = pos[1];
        r->spawnPos[2] = pos[2];
        goto place;
    }
    zrollerGetPlayerStartPos(0, pos, angles);
place:
    r->pos[0] = pos[0];
    r->pos[1] = pos[1];
    r->pos[2] = pos[2];
    zmathMatrixIdentity((RwMatrix *)r->matrix);
    zmathVecNegate((RwV3d *)&r->matrix[4], (RwV3d *)&r->matrix[4]);   /* flip the up row (Y-down world) */
    zmathMatrixRotateXYZ((RwMatrix *)r->matrix, (const RwMatrix *)r->matrix, angles);
    if (newRun)
        riderUpdateAll();
    bmxphysUpdate();
}

/* HUD message (overlay item 12): text 0x1063 + id, e.g. 0x0B "HUGE TRANSFER!", 0x0D "NEW HIGH SCORE!" */
typedef struct HudMessage {
    uint8_t  id;
    uint8_t  pad;
    int16_t  busy;          /* read back: non-zero while a message is shown */
    uint32_t value;
} HudMessage;

static int iabs(int v) { return v < 0 ? -v : v; }

/* VA 0x0040c300, 1438 bytes, [I] between, status draft */
void gameCheckTransfer(int player, int landed)
{
    /* At a landing (rider.c, grind/stall exits): compare what the bike left from with what it
     * landed on. Gives the "longest jump" record, the transfer objectives (13-19) and the
     * HUGE TRANSFER message. */
    BmxRider *r = RIDER_OF(player);
    uint16_t kind[2];
    uint16_t tag[3];            /* tag[2] is never written but read by objective 18 [V] */
    uint32_t i, goal, obj, k;
    int dx, dy, dz;
    uint32_t dist;
    SaveGoalInfo *gi;
    const GoalDef *def;
    HudMessage msg;

    if (landed) {
        for (i = 0; i < 2; i++) {
            BmxContact *c = &r->contact[i];
            uint8_t *o = (uint8_t *)c->obj;
            if (o == NULL)
                continue;
            switch (c->type) {
            case 0: case 4: case 5: case 6: case 7: case 8:
                kind[i] = 10;
                tag[i] = *(uint16_t *)(o + 0x16) & 0xFFF;
                break;
            case 1: case 2: case 3:
                kind[i] = 10;
                tag[i] = *(uint16_t *)(o + 0x16);
                break;
            case 9:
                if (*(uint8_t **)(o + 0x34) == NULL) {
                    kind[i] = 9;
                    tag[i] = *(uint16_t *)(o + 0x16);
                } else {
                    kind[i] = 12;
                    tag[i] = (*(uint8_t **)(o + 0x34))[2] & 0x1F;
                }
                break;
            case 10: case 11: case 13: case 14:
                return;
            case 12:
                kind[i] = 12;
                tag[i] = o[2] & 0x1F;
                break;
            }
        }
        dx = (int)(r->contact[1].pos[0] - r->contact[0].pos[0]);
        dy = (int)(r->contact[1].pos[1] - r->contact[0].pos[1]);
        dz = (int)(r->contact[1].pos[2] - r->contact[0].pos[2]);
        dist = (uint32_t)(int)sqrt((double)(dz * dz + dy * dy + dx * dx));
        gameSubmitRecord(player, STAT_LONGEST_JUMP, (uint32_t)(int)(int16_t)dist, 0x15, 0);

        if (tag[0] != 0 && tag[1] != 0 && tag[0] != tag[1]) {
            if (gameIsProquest() && g_currentLevel < 6) {
                gi = comdataGetGoalInfo(player, g_currentLevel);
                def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
                for (goal = 0; goal < 4; goal++, def++) {
                    if (GOAL_BYTE(gi, goal) & 0x80)
                        continue;
                    for (obj = 0; obj < def->numObjectives; obj++) {
                        const GoalObjective *o = &def->obj[obj];
                        if (gameGetObjectiveState(player, goal, obj) >= GOAL_STATE_PENDING)
                            continue;
                        switch (o->type) {
                        case GOAL_OBJ_TRANSFER: case GOAL_OBJ_TRANSFER_TRICK: case GOAL_OBJ_TRANSFER_SPIN:
                            if (kind[0] != 10 || kind[1] != 10)
                                break;
                            if (!((iabs(tag[0] - o->param[0]) < 6 && iabs(tag[1] - o->param[1]) < 6) ||
                                  (iabs(tag[0] - o->param[1]) < 6 && iabs(tag[1] - o->param[0]) < 6)))
                                break;
                            if (o->type == GOAL_OBJ_TRANSFER) {
                                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                                break;
                            }
                            for (k = 0; k < r->trickCount; k++) {
                                const BmxTrickRec *t = &r->score.trick[k];
                                if ((o->type == GOAL_OBJ_TRANSFER_TRICK && t->trick == o->param[2]) ||
                                    (o->type == GOAL_OBJ_TRANSFER_SPIN && o->param[2] <= t->spins)) {
                                    gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                                    break;
                                }
                            }
                            break;
                        case GOAL_OBJ_TRANSFER_OBJECTS:
                        case GOAL_OBJ_TRANSFER_GROUPS: {
                            uint16_t want = (o->type == GOAL_OBJ_TRANSFER_OBJECTS) ? 9 : 12;
                            if (kind[0] == want && kind[1] == want &&
                                ((tag[0] == o->param[0] && tag[1] == o->param[1]) ||
                                 (tag[1] == o->param[0] && tag[0] == o->param[1])))
                                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                            break;
                        }
                        case GOAL_OBJ_TRANSFER_TO_OBJ:
                            if (kind[0] == 10 && kind[1] == 9 && iabs(tag[0] - o->param[0]) < 6 &&
                                o->param[1] <= tag[1] && tag[2] <= o->param[2])
                                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                            break;
                        case GOAL_OBJ_TRANSFER_TO_GROUP:
                            if (kind[0] == 10 && kind[1] == 12 && iabs(tag[0] - o->param[0]) < 6 &&
                                o->param[1] <= tag[1] && tag[1] <= o->param[2])
                                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                            break;
                        }
                    }
                }
            }
            if (actionGetMode() == 0 && r->contact[0].type == 1 && r->contact[1].type == 1 &&
                iabs(tag[0] - tag[1]) > 5 && (float)dist >= 400.0f) {
                overlayGetItem(12, &msg);
                if (msg.busy == 0) {
                    msg.id = 0x0B;               /* HUGE TRANSFER! */
                    msg.value = tag[1];
                    overlaySetItem(12, &msg);
                    bmxsoundEvent(0x1392, 0, 0, 0, 0);
                }
            }
        }
    }
    if (r->trickCount == 0)
        gameResolvePendingObjectives(player, landed);
}

/* VA 0x0040ca60, 296 bytes, [I] between, status draft */
int gameOnPropTouched(int player, void *prop)
{
    /* prop.c: the rider touched a prop. GOAL_OBJ_COLLECT objectives on its group are done. */
    SaveGoalInfo *gi;
    const GoalDef *def;
    uint32_t goal, obj, group;
    int found = 0;

    if (!gameIsProquest() || g_currentLevel > 5)
        return 0;
    gi = comdataGetGoalInfo(player, g_currentLevel);
    def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
    for (goal = 0; goal < 4; goal++, def++) {
        if (GOAL_BYTE(gi, goal) & 0x80)
            continue;
        for (obj = 0; obj < def->numObjectives; obj++) {
            const GoalObjective *o = &def->obj[obj];
            group = *(uint16_t *)(*(uint8_t **)((uint8_t *)prop + 8) + 2) & 0x1F;
            if (gameGetObjectiveState(player, goal, obj) < GOAL_STATE_DONE && o->type == GOAL_OBJ_COLLECT &&
                o->param[0] <= group && group <= o->param[1]) {
                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                bmxpropActivate(player, prop);
                fxCollectEffect();
                found = 1;
            }
        }
    }
    return found;
}

/* VA 0x0040cbe0, 272 bytes, [I] between, status draft */
int gameOnObjectHit(int player, void *object)
{
    /* prop.c: the rider hit a tagged object. GOAL_OBJ_HIT_OBJECT objectives on its tag are done. */
    SaveGoalInfo *gi;
    const GoalDef *def;
    uint32_t goal, obj, tag;
    int found = 0;

    if (!gameIsProquest() || g_currentLevel > 5)
        return 0;
    gi = comdataGetGoalInfo(player, g_currentLevel);
    def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
    for (goal = 0; goal < 4; goal++, def++) {
        if (GOAL_BYTE(gi, goal) & 0x80)
            continue;
        for (obj = 0; obj < def->numObjectives; obj++) {
            const GoalObjective *o = &def->obj[obj];
            tag = *(uint16_t *)((uint8_t *)object + 0x16) & 0xFFF;
            if (gameGetObjectiveState(player, goal, obj) < GOAL_STATE_DONE && o->type == GOAL_OBJ_HIT_OBJECT &&
                o->param[0] <= tag && tag <= o->param[1]) {
                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                found = 1;
            }
        }
    }
    return found;
}

/* VA 0x0040cd40, 408 bytes, [I] row, status draft */
void gameAdvanceGoalTier(int player)
{
    /* At the start of a proquest run: count the done goals of the tier; a full tier (4) moves on to
     * the next (amateur -> pro -> hardcore, capped at hardcore). Open goals get their objectives
     * reset; done goals re-apply their prop changes (e.g. opened doors stay open). The "done this
     * session" bit 0x40 is cleared. */
    SaveGoalInfo *gi = comdataGetGoalInfo(player, g_currentLevel);
    const GoalDef *def;
    uint32_t goal, obj, j;
    void *prop;

    gi->tier = 0;
    do {
        gi->tier_goals_done = 0;
        def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
        for (goal = 0; goal < 4; goal++, def++) {
            if (!(GOAL_BYTE(gi, goal) & 0x80)) {
                for (obj = 0; obj < def->numObjectives; obj++) {
                    gameSetObjectiveState(player, goal, obj, GOAL_STATE_IDLE);
                    gameSetObjectiveFlag(player, goal, obj, 1, 1);
                }
                GOAL_BYTE(gi, goal) = 0;
            } else {
                gi->tier_goals_done++;
                for (obj = 0; obj < def->numObjectives; obj++)
                    for (j = 0; j < 4; j++)
                        if (def->obj[obj].props[j] != 0xFF &&
                            (prop = bmxpropGetPropByID((int8_t)def->obj[obj].props[j])) != NULL)
                            bmxpropActivateNow(player, prop);
                for (j = 0; j < 3; j++)
                    if (def->props[j] != 0xFF && (prop = bmxpropGetPropByID((int8_t)def->props[j])) != NULL)
                        bmxpropActivateNow(player, prop);
            }
            GOAL_BYTE(gi, goal) &= ~0x40;
        }
    } while (gi->tier_goals_done > 3 && ++gi->tier < 3);
    if (gi->tier >= 2)
        gi->tier = 2;
}

/* VA 0x0040cf50, 1 bytes, [I] between, status draft */
void gameNullPlayerHook(int player)
{
    /* empty (ret); called for every active player at the start of a turn */
    (void)player;
}

/* VA 0x0040cf70, 127 bytes, [I] row, status draft */
int gameIsTierComplete(int player)
{
    /* Level end in proquest: did this run finish the tier (challenge levels: all 4 goals done
     * and at least one of them this session) / earn a reward (competitions)? */
    SaveGoalInfo *gi;
    uint32_t goal;

    if (!gameIsProquest())
        return 0;
    if (g_currentLevel < 6) {
        gi = comdataGetGoalInfo(player, g_currentLevel);
        if (gi->tier_goals_done == 4)
            for (goal = 0; goal < 4; goal++)
                if (gi->goal[gi->tier][goal][0] & 0x40)
                    return 1;
    } else if (RIDER_OF(player)->runRewards != 0) {
        return 1;
    }
    return 0;
}

/* HUD meter (overlay items 5 and 6): a float with a kind (4 height, 6 grind, 8 manual, 9 skid) */
typedef struct HudMeter {
    float    value;
    uint8_t  pad[6];
    uint16_t kind;
} HudMeter;

static void gameLandSound(const BmxRider *r)
{
    /* the bigger the set, the louder the crowd */
    int level;
    if (r->trickCount > 2 || r->score.setScore > 4000) {
        level = (int)((float)(int16_t)(uint16_t)(r->score.setScore - 4000) * 0.0005f);
        if (level > 5)
            level = 5;
        bmxsoundEvent(0x138B, level, 1, 0, 0);
    }
}

/* VA 0x0040d010, 2185 bytes, [V] direct, status draft */
void gameUpdateTricks(int player)
{
    /* Per frame: the trick-set state machine (rider->trickState)
     *   0 riding -> 1 airborne (gameRiderInTrick) -> 2 a trick record is open -> 3 landed, 30 frames
     *   of grace (a new trick in that time continues the set) -> gameRecordLandedTricks -> 0.
     * While a record is open it copies the rider's trick/modifier/spin/flip into it every frame. */
    BmxRider *r = RIDER_OF(player);
    BmxTrickRec *t;
    uint32_t st, s1, k, n, v;
    uint8_t tr, fk;
    float tmp[4], up[3];
    HudMeter meter;
    uint32_t hudScore[2];
    float balance;
    GameLiveRecord *live;

    if (gameGetMode() == GAME_MODE_WIPEOUT) {
        /* wipeout: the score is the crash damage */
        if (r->trickState == 0) {
            if (bmxphysGetState(r) == 2) {
                r->trickState = 1;
                r->landTimer = 0;
            }
        } else if (r->trickState == 1) {
            if (bmxphysGetState(r) == 2)
                r->score.runScore = r->crash * 100;
            else
                r->trickState = 0;
        }
        goto update;
    }
    if (r->trickState == 2 && (r->stateFlags & 0x80))
        r->trickState = 1;                   /* bailing: the open record is dropped */

    switch (r->trickState) {
    case 0:
        if (!gameRiderInTrick(r))
            goto update;
        r->trickState = 1;
        r->landTimer = 0;
        r->bestWallHeight = 0.0f;
        /* fall through */
    case 1:
        if (!gameRiderInTrick(r) || r->trickCount > 6) {
            st = riderGetState(r, 0);
            if (st == 0) {                   /* landed */
                r->trickState = 3;
                r->landTimer = 30;
                gameResolvePendingObjectives(player, 1);
                gameLandSound(r);
            } else if (st == 6) {            /* crashed */
                gameRecordLandedTricks(g_currentPlayer, 0);
                gameResolvePendingObjectives(player, 0);
                bmxsoundEvent(0x138B, 2, 0, 0, 0);
            }
            goto update;
        }
        /* open the next record */
        t = &r->score.trick[r->trickCount++];
        t->ticks = 0;
        t->age = 0;
        t->flags = 0;
        t->length = 0.0f;
        r->trickState = 2;
        bmxsoundEvent(0x1389, 2, r->trickCount - 1, 0, 0);
        /* fall through */
    case 2:
        st = riderGetState(r, 0);
        if (st == 0) {
            /* landed: a flip that did not come round is no trick; a partial multi-flip is
             * scored as the single/double one */
            t = &r->score.trick[r->trickCount - 1];
            tr = t->trick;
            if (tr != TRICK_NONE && tr < 0x1B && (fk = g_trickFlags[tr] & 7) != 0) {
                if (t->flips == 0) {
                    t->trick = TRICK_NONE;
                } else if (t->flips == 1) {
                    if (fk > 1) {
                        if (fk < 4)
                            t->trick = 3;            /* backflip */
                        else if (fk == 5)
                            t->trick = 10;           /* front flip */
                    }
                } else if (t->flips == 2) {
                    t->trick = 8;                    /* double backflip */
                }
            }
            r->trickState = 3;
            r->landTimer = 30;
            gameResolvePendingObjectives(player, 1);
            gameLandSound(r);
        } else if (st == 1) {
            if (r->trickId == TRICK_NONE && r->modId == TRICK_NONE && r->spin == 0)
                r->trickState = 1;
        } else if (st == 6) {
            gameRecordLandedTricks(g_currentPlayer, 0);
            gameResolvePendingObjectives(player, 0);
            bmxsoundEvent(0x138B, 2, 0, 0, 0);
        }
        if (r->trickState != 2 || riderGetState(r, 0) == 5)
            goto update;

        t = &r->score.trick[r->trickCount - 1];
        t->trick = r->trickId;
        t->mod = r->modId;
        t->modCount = r->modCount;
        if (!(r->stateFlags & 0x1000)) {
            v = (uint32_t)iabs(r->spin) + 0x4000;          /* rounded to half turns */
            t->spins = ((v & 0xFFFF8000u) < 0x40000) ? (uint8_t)(v >> 15) : 8;
        } else {
            t->spins = 0;
        }
        v = (uint32_t)iabs(r->flip) + 0x8000;              /* rounded to whole flips */
        t->flips = ((v & 0xFFFF0000u) < 0x30000) ? (uint8_t)(v >> 16) : 3;

        st = riderGetState(r, 0);
        if (st == 1) {
            s1 = riderGetState(r, 1);
            if (s1 == 1) {
                if ((g_trickFlags[r->trickId] & 7) == 0 && riderGetState(r, 2) == 2 &&
                    (g_trickFlags[r->trickId] & 8) == 0)
                    t->ticks++;                  /* holding a (non-flip) trick */
            } else if (s1 == 2 && riderGetState(r, 2) == 2) {
                t->ticks++;
            }
        } else if (st < 2 || st > 3 || riderGetState(r, 2) == 3 || riderGetState(r, 2) == 4) {
            t->ticks++;
        }
        if (bmxphysGetState(r) == 4 || riderGetState(r, 0) == 4)
            t->length += r->frameDist;       /* grind / manual length */

        tr = t->trick;
        if (tr == TRICK_NONE || tr < 0x1B) {
            if (t->spins == 0) {
                if (tr == TRICK_NONE && t->mod == TRICK_NONE) {
                    /* nothing happened: drop the record again */
                    r->trickCount--;
                    r->trickState = (r->trickCount != 0);
                }
            } else if (r->surface == 1 && (r->stateFlags & 0x2000)) {
                /* a spin off an edge: x1.2 when it goes against the riding direction [I] */
                up[0] = 0.0f; up[1] = -1.0f; up[2] = 0.0f;
                zmathTriangleNormal((RwV3d *)tmp, (const RwV3d *)up, &g_zmathOrigin, (const RwV3d *)&r->matrix[4]);
                if ((0 < r->spin) != (zmathVecDot((const RwV3d *)r->vel, (const RwV3d *)tmp) < 0.0f))
                    t->flags |= 2;
            }
        } else if (r->trickId < 0x24 && zmathVecDot((const RwV3d *)r->vel, (const RwV3d *)r->matrix) < 0.0f) {
            t->flags |= 2;                   /* grinding backwards */
        }
        if ((r->stateFlags & 0x10000000) && t->spins != 0) {
            t->flags |= 4;
            r->stateFlags &= ~0x10000000u;
        }
        if (t->trick > 0x29 && t->trick < 0x2C && r->bestWallHeight < r->wallHeight)
            r->bestWallHeight = r->wallHeight;   /* fast plant / wall tap height */
        break;

    case 3:
        if (!gameRiderInTrick(r)) {
            if (--r->landTimer == 0)
                gameRecordLandedTricks(g_currentPlayer, 1);
        } else {
            gameRecordLandedTricks(g_currentPlayer, 1);
        }
        break;
    }

update:
    gameScoreTrickSet(&r->score);
    /* earlier tricks of the set keep ageing while later ones are done (16 frames per later trick) */
    n = r->trickCount;
    for (k = 0; k < n; k++) {
        if (r->trickState != 2 ||
            (n > 1 && k < n - 1 && r->score.trick[k].age < 16 * (n - 1 - k)) ||
            (n > 2 && k < n - 2))
            r->score.trick[k].age++;
        n = r->trickCount;
    }
    hudScore[0] = r->score.runScore;
    hudScore[1] = 0;
    overlaySetItem(1, hudScore);

    if (riderGetState(r, 0) == 1 && !(r->stateFlags & 0x8000000) && r->pos[1] < r->contact[0].pos[1]) {
        /* in the air above the take-off point: the height meter, the big-air record at the top */
        if (r->vel[1] > 0.0f) {
            gameSubmitRecord(player, STAT_BIG_AIR, (uint32_t)(int)(r->contact[0].pos[1] - r->pos[1]), 0x14, 0);
        } else {
            meter.value = r->contact[0].pos[1] - r->pos[1];
            meter.kind = 4;
            overlaySetItem(5, &meter);
        }
    }
    if (riderGetState(r, 0) == 0) {
        if (riderGetState(r, 1) == 2) {
            meter.value = r->skidLength;
            meter.kind = 9;
        } else {
            if (r->skidLength != 0.0f) {
                gameSubmitRecord(player, STAT_LONGEST_SKID, (uint32_t)(int)r->skidLength, 0x12, 0);
                r->skidLength = 0.0f;
            }
            meter.value = 0.0f;
        }
        overlaySetItem(5, &meter);
    }
    if (gameGetMode() > GAME_MODE_WIPEOUT && gameGetMode() < GAME_MODE_DEATH_MATCH) {
        /* the stat games: the run score is this run's value of the mode's stat */
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(),
                                                g_gameModeStat[gameGetMode()], 3, 0));
        if (live == NULL)
            zAssertFail("", GAME_SRC, 0x9a6, 0);
        r->score.runScore = live->value;
    }
    balance = (riderGetState(r, 0) == 4) ? r->grindBalance : 0.0f;
    overlaySetItem(6, &balance);
}

/* VA 0x0040dae0, 1075 bytes, [I] row, status draft */
void gameRecordLandedTricks(int player, int landed)
{
    /* The set is over: landed -> per-trick records, statistics, the career "trick landed" bits
     * (SAVE.md rider +0xF7), the set score into the run; bailed -> a fall. */
    BmxRider *r = RIDER_OF(player);
    SaveRiderRecord *career = CAREER(player);
    BmxAreaStats *a;
    const BmxTrickRec *t;
    uint32_t k, n, area, sh;
    uint8_t *cell;
    float *box, grind, manual;
    HudMessage msg;
    uint32_t zero[3];

    if (actionGetMode() != 0)
        goto done;
    box = zrollerGetWorldBounds();
    area = (uint32_t)((int)(r->pos[2] - box[2]) / (int)g_gameAreaSize[1] +
                      ((int)(r->pos[0] - box[0]) / (int)g_gameAreaSize[0]) * 4);
    if (area > 14)
        area = 15;
    a = &r->area[area];
    if (!landed) {
        bmxsoundEvent(0x138C, 0, 0, 0, 0);
        a->falls++;
        r->total.falls++;
        goto done;
    }
    grind = 0.0f;
    manual = 0.0f;
    for (k = 0; k < r->trickCount; k++) {
        t = &r->score.trick[k];
        gameSubmitRecord(player, STAT_SINGLE_TRICK, t->points, 0x0E, 0);
        /* 2-bit "times landed" counter per trick x modifier (gameScoreTrickSet's repeat penalty) */
        cell = &r->trickLog[(int8_t)t->trick + 1][((int8_t)t->mod + 1) >> 2];
        sh = ((t->mod + 1) & 3) << 1;
        if (((*cell >> sh) & 3) < 3)
            *cell = (uint8_t)(*cell + (1 << sh));
        if (TRICK_IS_AIR(t->trick)) {
            a->tricks++;
            r->total.tricks++;
        } else if (TRICK_IS_GRIND(t->trick)) {
            grind += t->length;
            a->grinds++;
            r->total.grinds++;
        } else if (TRICK_IS_STALL(t->trick)) {
            a->stalls++;
            r->total.stalls++;
        } else if (TRICK_IS_PLANT(t->trick)) {
            gameSubmitRecord(player, STAT_HIGHEST_MARK, (uint32_t)(int)r->bestWallHeight, 0x13, 0);
            a->stalls++;
            r->total.stalls++;
        } else if (TRICK_IS_MANUAL(t->trick)) {
            manual += t->length;
            a->manuals++;
            r->total.manuals++;
        }
        if (t->trick != TRICK_NONE)
            career->tricks_landed[t->trick >> 3] |= (uint8_t)(1 << (t->trick & 7));
        if (t->mod != TRICK_NONE) {
            /* modifiers are bits 46.. of the same set */
            career->tricks_landed[(t->mod + 0x2E) >> 3] |= (uint8_t)(1 << ((t->mod - 2) & 7));
            a->modifiers++;
            r->total.modifiers++;
            if (t->trick != TRICK_NONE) {
                a->modified++;
                r->total.modified++;
            }
        }
    }
    gameSubmitRecord(player, STAT_LONGEST_GRIND, (uint32_t)(int)grind, 0x10, 0);
    gameSubmitRecord(player, STAT_LONGEST_MANUAL, (uint32_t)(int)manual, 0x11, 0);

    if (gameGetMode() == GAME_MODE_BMX) {
        if (!gameTestFlag(GAME_FLAG_SETTER)) {
            /* copying the setter: every trick/modifier must match, with at least as much spin */
            n = g_bmxTargetCount;
            for (k = 0; k < n; k++) {
                const BmxTrickRec *want = &g_bmxTarget[k];
                t = &r->score.trick[k];
                if ((want->trick != TRICK_NONE && t->trick != want->trick) ||
                    (want->mod != TRICK_NONE && t->mod != want->mod) || t->spins < want->spins)
                    break;
            }
            if (k != n)
                goto message;                /* missed: the run score stays 0 */
        } else {
            n = (r->trickCount < 4) ? r->trickCount : 3;
            zstringMemCopy(g_bmxTarget, r->score.trick, n * sizeof(BmxTrickRec));
            g_bmxTargetCount = (uint8_t)n;
        }
        r->score.runScore = 1;               /* "made it" */
    } else if (gameGetMode() < GAME_MODE_WIPEOUT || gameGetMode() > GAME_MODE_LONGEST_MANUAL) {
        gameSubmitRecord(player, STAT_TRICK_SET, r->score.setScore, 0x0F, 0);
        r->score.runScore += r->score.setScore;
        gameSubmitRecord(player, STAT_RUN, r->score.runScore, 0x0D, 1);
    }
message:
    overlayGetItem(12, &msg);
    if (msg.busy == 0 && r->trickCount > 3) {
        msg.value = r->trickCount;
        msg.id = 0x0C;                       /* TRICK COMBO! */
        overlaySetItem(12, &msg);
        bmxsoundEvent(0x1392, 0, 0, 0, 0);
    }
    bmxsoundEvent(0x138C, 1, r->score.setScore, 0, 0);
done:
    zero[0] = 0;
    overlaySetItem(5, zero);
    r->trickState = 0;
    r->trickCount = 0;
}

/* VA 0x0040e030, 554 bytes, [I] between, status draft */
void gameSubmitRecord(int player, uint32_t stat, uint32_t value, uint8_t msgId, uint32_t flags)
{
    /* A new value of a record stat: update the 4 live slots (all-time slot 0 only outside free ride
     * and below difficulty 4 [I]), show "NEW ..." once per record (flags bit0: only once per run),
     * and complete GOAL_OBJ_RECORD objectives. */
    uint32_t slot, goal, obj;
    GameLiveRecord *live;
    HudMessage msg;
    SaveGoalInfo *gi;
    const GoalDef *def;

    if (actionGetMode() == 0) {
        for (slot = 0; slot < 4; slot++) {
            if (slot == 0 && !((gameGetMode() != GAME_MODE_SINGLE || gameGetSubMode() != GAME_SUB_FREE_RIDE) &&
                               zrollerGetPlayer(player)[7] < 4))
                continue;
            live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, slot, 0));
            if (live == NULL || live->value >= value)
                continue;
            if (slot == 0 && (!(flags & 1) || !(live->flags & 2))) {
                overlayGetItem(12, &msg);
                if (msg.busy == 0) {
                    msg.id = msgId;
                    msg.value = value;
                    overlaySetItem(12, &msg);
                    live->flags |= 2;
                    bmxsoundEvent(0x1391, msgId, value, 0, 0);
                }
            }
            live->value = value;
            live->player = (uint8_t)player;
            live->flags |= 1;
        }
    }
    if (!gameIsProquest() || g_currentLevel >= 6)
        return;
    gi = comdataGetGoalInfo(player, g_currentLevel);
    def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
    for (goal = 0; goal < 4; goal++, def++) {
        if (GOAL_BYTE(gi, goal) & 0x80)
            continue;
        for (obj = 0; obj < def->numObjectives; obj++) {
            const GoalObjective *o = &def->obj[obj];
            if (o->type != GOAL_OBJ_RECORD || stat != o->param[0] ||
                gameGetObjectiveState(player, goal, obj) >= GOAL_STATE_DONE)
                continue;
            if (stat < STAT_BIG_AIR) {
                if (o->param[1] < value)
                    gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
            } else if ((float)(int16_t)o->param[1] < (float)value) {    /* distances in cm */
                gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
            }
        }
    }
}

/* VA 0x0040e2f0, 103 bytes, [I] between, status draft */
int gameRiderInTrick(const BmxRider *r)
{
    /* a trick is under way: spun >= 90 degrees, bailing, or a trick/modifier input */
    uint32_t v;

    (void)gameGetMode();
    if (!(r->stateFlags & 0x1000)) {
        v = (uint32_t)iabs(r->spin) + 0x4000;
        if ((v & 0xFFFF8000u) > 0x37FFF || (v >> 15) != 0)
            return 1;
    }
    if (r->stateFlags & 0x80)
        return 1;
    if (r->trickId == TRICK_NONE)
        return r->modId != TRICK_NONE;
    return 1;
}

/* VA 0x0040e380, 1691 bytes, [I] between, status draft */
void gameUpdateHints(int player)
{
    /* Tutorial hints (profile option "tutorial hints", SAVE.md profile flags bit 1): hint i
     * (texts table 1 0x14A..) is shown once when its situation occurs; rider->hintMask holds the
     * ones not shown yet. When the mask is empty the option switches itself off. */
    SaveProfile *pf = &g_saveProfiles[player];
    BmxRider *r = RIDER_OF(player);
    struct { uint8_t id; uint8_t pad; int16_t busy; } h;
    uint32_t i, bit;
    int show, keep;

    if (!(pf->flags & 2))
        return;
    overlayGetItem(10, &h);
    if (h.busy == 0) {
        for (i = 0; i < 12; i++) {
            bit = 1u << i;
            if (!(r->hintMask & bit))
                continue;
            h.id = (uint8_t)i;
            show = 0;
            switch (i) {
            case 0: case 2: case 6:          /* first jump: tricks, modifiers, spins */
                show = riderGetState(r, 0) == 1 && riderGetState(r, 1) == 0 && r->trickCount == 0 &&
                       r->vel[1] < 0.0f;
                break;
            case 1:
                show = riderGetState(r, 0) == 1 && riderGetState(r, 1) == 2;
                break;
            case 3: case 4:
                show = riderGetState(r, 0) == 1 && riderGetState(r, 1) == 1;
                break;
            case 5:
                show = riderGetState(r, 0) == 1 && riderGetState(r, 1) == 0 && r->trickCount != 0 &&
                       r->score.trick[r->trickCount - 1].trick < 0x1B;
                break;
            case 7: case 8: case 9:
                show = riderGetState(r, 0) == 1 && riderGetState(r, 1) == 0 && r->vel[1] < 0.0f;
                break;
            case 10:
                show = riderGetState(r, 0) == 1 && r->vel[1] < 0.0f;
                break;
            case 11:
                show = riderGetState(r, 0) == 0 && (r->stateFlags & 0x400000);
                break;
            }
            if (show) {
                overlaySetItem(10, &h);
                r->hintMask &= ~bit;
            }
            if (r->hintMask == 0)
                pf->flags &= ~2;
            break;                           /* only the first pending hint is looked at */
        }
    }
    overlayGetItem(10, &h);
    if (h.busy != 0)
        return;
    for (i = 12; i <= 21; i++) {
        bit = 1u << i;
        if (!(r->hintMask & bit))
            continue;
        h.id = (uint8_t)i;
        show = 0;
        keep = 0;                            /* hints 13 and 17 stay pending after being shown */
        switch (i) {
        case 12: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 3; break;
        case 13: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 0 && r->frameDist == 0.0f;
                 keep = 1; break;
        case 14: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 0 && r->frameDist != 0.0f; break;
        case 15: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 1; break;
        case 16: show = riderGetState(r, 0) == 0 && r->surface == 1 && r->vel[1] < 0.0f;
                 if (show) r->hintMask &= ~0x1000u;   /* also retires hint 12 */
                 break;
        case 17: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 4; keep = 1; break;
        case 18: show = riderGetState(r, 0) == 6 && (r->crashKind == 7 || r->crashKind == 8); break;
        case 19: show = riderGetState(r, 0) == 0 && r->surface == 1 && r->vel[1] < 0.0f; break;
        case 20: show = riderGetState(r, 0) == 2; break;
        case 21: show = riderGetState(r, 0) == 0 && riderGetState(r, 1) == 3 && r->surface == 1 &&
                        r->vel[1] < 0.0f; break;
        }
        if (show) {
            overlaySetItem(10, &h);
            if (!keep)
                r->hintMask &= ~bit;
        }
        overlayGetItem(10, &h);
        if (h.busy != 0) {
            if (r->hintMask == 0)
                pf->flags &= ~2;
            return;
        }
    }
}

/* ------------------------------------------------------------------ challenge objectives */

static int gameTrickMatches(const BmxTrickRec *t, const GoalObjective *o)
{
    return (o->param[0] == 0xFF || t->trick == o->param[0]) && o->param[1] <= t->spins;
}

static int gameGroupOf(const uint8_t *obj)
{
    return obj[2] & 0x1F;
}

/* VA 0x0040ec40, 3093 bytes, [I] between, status draft */
void gameUpdateObjectives(int player)
{
    /* Per frame in proquest: for every objective of the open goals of the current tier
     *   - track the area box (IDLE <-> IN_AREA),
     *   - test the objective's condition (-> PENDING, needs a clean landing, or -> DONE),
     *   - DONE -> REWARDED: count it in the goal byte; a complete goal counts in tier_goals_done,
     *     plays "n CHALLENGES COMPLETE", switches the goal's props and gets 0x80 | 0x40,
     *   - an objective in its area becomes ACTIVE (one at a time): its text is shown and its
     *     objects are collected into g_gameHighlightA/B for gameDrawObjectiveHighlights. */
    BmxRider *r = RIDER_OF(player);
    SaveGoalInfo *gi;
    const GoalDef *def;
    const GoalObjective *o;
    const BmxTrickRec *last;
    uint32_t goal, obj, k, n, x, z, state, tag;
    struct { uint8_t id; uint8_t pad; int16_t busy; } shown;
    HudMessage msg;
    const uint8_t *g;
    void *list;
    void **dst;
    void *prop;

    if (!gameIsProquest() || g_currentLevel >= 6)
        return;
    overlayGetItem(11, &shown);
    gi = comdataGetGoalInfo(player, g_currentLevel);
    def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
    for (goal = 0; goal < 4; goal++, def++) {
        if (GOAL_BYTE(gi, goal) & 0x80)
            continue;
        for (obj = 0; obj < def->numObjectives; obj++) {
            o = &def->obj[obj];
            x = (uint32_t)(int)r->pos[0];
            z = (uint32_t)(int)r->pos[2];
            if ((int)x < o->area[0] || (int)x >= o->area[2] || (int)z < o->area[1] || (int)z >= o->area[3]) {
                state = gameGetObjectiveState(player, goal, obj);
                if (state == GOAL_STATE_ACTIVE) {
                    shown.id = 0xFF;
                    shown.busy = 0;
                    overlaySetItem(11, &shown);
                }
                if (state == GOAL_STATE_IN_AREA || state == GOAL_STATE_ACTIVE)
                    gameSetObjectiveState(player, goal, obj, GOAL_STATE_IDLE);
            } else if (gameGetObjectiveState(player, goal, obj) == GOAL_STATE_IDLE) {
                gameSetObjectiveState(player, goal, obj, GOAL_STATE_IN_AREA);
            }

            state = gameGetObjectiveState(player, goal, obj);
            last = r->trickCount ? &r->score.trick[r->trickCount - 1] : NULL;
            if (state != GOAL_STATE_IDLE && state < GOAL_STATE_PENDING) {
                switch (o->type) {
                case GOAL_OBJ_TRICK:
                    if (last && gameTrickMatches(last, o))
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                    break;
                case GOAL_OBJ_TRICK_OFF_EDGE:
                    if (!last || !gameTrickMatches(last, o))
                        break;
                    switch (r->surface) {
                    case 0: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 13: case 14:
                        tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 0x16) & 0xFFF;
                        if (iabs((int)tag - o->param[2]) < 6)
                            gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                        break;
                    case 1: case 2: case 3:
                        tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 0x16);
                        if (iabs((int)tag - o->param[2]) < 6)
                            gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                        break;
                    }
                    break;
                case GOAL_OBJ_TRICK_OVER_GROUP:
                    if (!last || !gameTrickMatches(last, o))
                        break;
                    if (r->contact[0].type == 12)
                        g = (const uint8_t *)r->contact[0].obj;
                    else if (r->contact[0].type == 9)
                        g = *(const uint8_t **)((uint8_t *)r->contact[0].obj + 0x34);
                    else
                        break;
                    if (g != NULL && gameGroupOf(g) == o->param[2])
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                    break;
                case GOAL_OBJ_TRICK_OFF_OBJECT:
                    if (last && gameTrickMatches(last, o) && r->surface == 9 &&
                        o->param[2] <= *(uint16_t *)((uint8_t *)r->surfaceObj + 0x16))
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                    break;
                case GOAL_OBJ_TRICK_COUNT:
                    n = 0;
                    for (k = 0; k < r->trickCount; k++)
                        if (TRICK_IS_AIR(r->score.trick[k].trick))
                            n++;
                    if (o->param[0] <= n)
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                    break;
                case GOAL_OBJ_RIDER_STATE:
                    if (riderGetState(r, 0) == o->param[0])
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                    break;
                case GOAL_OBJ_STATE_ON_EDGE:
                    if (riderGetState(r, 0) != o->param[0])
                        break;
                    tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 0x16);
                    if ((r->surface == 0 && o->param[1] <= (tag & 0xFFF) && (tag & 0xFFF) <= o->param[2]) ||
                        ((r->surface == 1 || r->surface == 2 || r->surface == 3) &&
                         o->param[1] <= tag && tag <= o->param[2]))
                        gameSetObjectiveState(player, goal, obj,
                                              (o->param[0] == 0 || o->param[0] == 4) ? GOAL_STATE_DONE
                                                                                     : GOAL_STATE_PENDING);
                    break;
                case GOAL_OBJ_STATE_ON_GROUP:
                    if (riderGetState(r, 0) == o->param[0] && r->surface == 12) {
                        tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 2) & 0x1F;
                        if (o->param[1] <= tag && tag <= o->param[2])
                            gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                    }
                    break;
                case GOAL_OBJ_GRIND_OBJECT:
                    if ((riderGetState(r, 0) == 2 || riderGetState(r, 0) == 3) && r->surface == 9) {
                        tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 0x16);
                        if (o->param[0] <= tag && tag <= o->param[1]) {
                            /* param[2]: the current trick must be done backwards */
                            if (o->param[2] != 0 && (!last || !(last->flags & 2)))
                                break;
                            gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                        }
                    }
                    break;
                case GOAL_OBJ_GRIND_GROUP:
                    if (((riderGetState(r, 0) == 2 && bmxphysGetState(r) == 4) ||
                         (riderGetState(r, 0) == 3 && bmxphysGetState(r) == 6)) && r->surface == 9 &&
                        (g = *(const uint8_t **)((uint8_t *)r->surfaceObj + 0x34)) != NULL &&
                        o->param[0] <= (uint32_t)gameGroupOf(g) && (uint32_t)gameGroupOf(g) <= o->param[1]) {
                        if (o->param[2] != 0 && (!last || !(last->flags & 2)))
                            break;
                        gameSetObjectiveState(player, goal, obj, GOAL_STATE_DONE);
                    }
                    break;
                case GOAL_OBJ_JUMP_GROUP:
                case GOAL_OBJ_JUMP_GROUP_TRICK:
                    /* in the air over prop group p0..p1, not having taken off from it */
                    if (riderGetState(r, 0) == 1 && r->surface == 12) {
                        uint32_t lo = o->param[0], hi = o->param[1], gr;
                        tag = *(uint16_t *)((uint8_t *)r->surfaceObj + 2) & 0x1F;
                        if (!(lo <= tag && tag <= hi))
                            break;
                        if (r->contact[0].type == 12) {
                            gr = gameGroupOf((const uint8_t *)r->contact[0].obj);
                            if (lo <= gr && gr <= hi)
                                break;
                        }
                        if (r->contact[0].type == 9 &&
                            (g = *(const uint8_t **)((uint8_t *)r->contact[0].obj + 0x34)) != NULL) {
                            gr = (uint32_t)gameGroupOf(g);
                            if (lo <= gr && gr <= hi)
                                break;
                        }
                        if (o->type != GOAL_OBJ_JUMP_GROUP_TRICK) {
                            gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                            break;
                        }
                        for (k = 0; k < r->trickCount; k++)
                            if (r->score.trick[k].trick == o->param[2]) {
                                gameSetObjectiveState(player, goal, obj, GOAL_STATE_PENDING);
                                break;
                            }
                    }
                    break;
                }
            }

            state = gameGetObjectiveState(player, goal, obj);
            if (state == GOAL_STATE_IDLE)
                continue;
            if (state > GOAL_STATE_ACTIVE) {
                if (state != GOAL_STATE_DONE)
                    continue;
                gameSetObjectiveState(player, goal, obj, GOAL_STATE_REWARDED);
                GOAL_BYTE(gi, goal)++;
                if ((GOAL_BYTE(gi, goal) & 0x3F) == def->numObjectives) {
                    /* the goal is complete */
                    gi->tier_goals_done++;
                    if (actionGetMode() == 0) {
                        msg.id = (uint8_t)(gi->tier_goals_done + 2);     /* ONE/TWO/THREE CHALLENGE(S) COMPLETE! */
                        if (gi->tier_goals_done == 4) {
                            msg.id = (uint8_t)(msg.id + gi->tier);       /* AMATEUR/PRO/HARDCORE CHALLENGES COMPLETE! */
                            bmxsoundEvent(0x138F, gi->tier, 0, 0, 0);
                        } else {
                            bmxsoundEvent(0x138E, gi->tier_goals_done, 0, 0, 0);
                        }
                        msg.value = 0;
                        overlaySetItem(12, &msg);
                    }
                    for (k = 0; k < 3; k++)
                        if (def->props[k] != 0xFF && (prop = bmxpropGetPropByID((int8_t)def->props[k])) != NULL)
                            bmxpropActivate(player, prop);
                    GOAL_BYTE(gi, goal) |= 0xC0;
                } else if (actionGetMode() == 0) {
                    msg.id = (uint8_t)(def->numObjectives - (GOAL_BYTE(gi, goal) & 0x3F) - 1);   /* n MORE TO GO... */
                    overlaySetItem(12, &msg);
                    bmxsoundEvent(0x138D, GOAL_BYTE(gi, goal) & 0x3F, 0, 0, 0);
                }
                continue;
            }
            /* IN_AREA / ACTIVE */
            if (o->type == GOAL_OBJ_RECORD && o->param[0] == STAT_RUN)
                continue;                    /* score goals have no highlight */
            if (shown.busy != 0) {
                if (shown.id != goal + gi->tier * 4)
                    gameSetObjectiveState(player, goal, obj, GOAL_STATE_IN_AREA);
                continue;
            }
            shown.busy = 0xFF;
            shown.id = (uint8_t)(gi->tier * 4 + goal);
            overlaySetItem(11, &shown);
            gameSetObjectiveState(player, goal, obj, GOAL_STATE_ACTIVE);
            g_gameHighlightA[0] = NULL;
            g_gameHighlightB[0] = NULL;
            list = NULL;
            dst = g_gameHighlightA;
            switch (o->type) {
            case GOAL_OBJ_TRICK_OFF_EDGE:   list = zbspFindTaggedFaces(o->param[2], o->param[2]); break;
            case GOAL_OBJ_TRICK_OVER_GROUP: list = zbspFindPropsByGroup(o->param[2], o->param[2]); break;
            case GOAL_OBJ_TRICK_OFF_OBJECT: list = zbspFindTaggedEdges(o->param[1], o->param[2]); break;
            case GOAL_OBJ_STATE_ON_EDGE:    list = zbspFindTaggedFaces(o->param[1], o->param[2]); break;
            case GOAL_OBJ_STATE_ON_GROUP:   list = zbspFindPropsByGroup(o->param[1], o->param[2]); break;
            case GOAL_OBJ_GRIND_OBJECT:     list = zbspFindTaggedEdges(o->param[0], o->param[1]); break;
            case GOAL_OBJ_GRIND_GROUP: case GOAL_OBJ_JUMP_GROUP: case GOAL_OBJ_JUMP_GROUP_TRICK:
            case GOAL_OBJ_COLLECT:
                list = zbspFindPropsByGroup(o->param[0], o->param[1]);
                break;
            case GOAL_OBJ_TRANSFER: case GOAL_OBJ_TRANSFER_TRICK: case GOAL_OBJ_TRANSFER_SPIN:
                if ((list = zbspFindTaggedFaces(o->param[0], o->param[0])) != NULL)
                    zstringMemCopy(g_gameHighlightA, list, sizeof(g_gameHighlightA));
                list = zbspFindTaggedFaces(o->param[1], o->param[1]);
                dst = g_gameHighlightB;
                break;
            case GOAL_OBJ_TRANSFER_OBJECTS:
                if ((list = zbspFindTaggedEdges(o->param[0], o->param[0])) != NULL)
                    zstringMemCopy(g_gameHighlightA, list, sizeof(g_gameHighlightA));
                list = zbspFindTaggedEdges(o->param[1], o->param[1]);
                dst = g_gameHighlightB;
                break;
            case GOAL_OBJ_TRANSFER_GROUPS:
                if ((list = zbspFindPropsByGroup(o->param[0], o->param[0])) != NULL)
                    zstringMemCopy(g_gameHighlightA, list, sizeof(g_gameHighlightA));
                list = zbspFindPropsByGroup(o->param[1], o->param[1]);
                dst = g_gameHighlightB;
                break;
            case GOAL_OBJ_TRANSFER_TO_OBJ:
                if ((list = zbspFindTaggedFaces(o->param[0], o->param[0])) != NULL)
                    zstringMemCopy(g_gameHighlightA, list, sizeof(g_gameHighlightA));
                list = zbspFindTaggedEdges(o->param[1], o->param[2]);
                dst = g_gameHighlightB;
                break;
            case GOAL_OBJ_TRANSFER_TO_GROUP:
                if ((list = zbspFindTaggedFaces(o->param[0], o->param[0])) != NULL)
                    zstringMemCopy(g_gameHighlightA, list, sizeof(g_gameHighlightA));
                list = zbspFindPropsByGroup(o->param[1], o->param[2]);
                dst = g_gameHighlightB;
                break;
            case GOAL_OBJ_HIT_OBJECT:
                list = zbspFindTaggedFaces(o->param[0], o->param[1]);
                break;
            }
            if (list != NULL)
                zstringMemCopy(dst, list, sizeof(g_gameHighlightA));
            g_gameHighlightA[63] = NULL;
            g_gameHighlightB[63] = NULL;
            if (gameGetObjectiveFlag(player, goal, obj, 1))
                gameSetObjectiveFlag(player, goal, obj, 1, 0);
            g_gameFrame = 0;
        }
    }
}

/* VA 0x0040fc30, 409 bytes, [I] between, status draft */
void gameResolvePendingObjectives(int player, int landed)
{
    /* The set is over: PENDING objectives become DONE if it was landed (GOAL_OBJ_TRICK: if a
     * matching trick is still in the set; GOAL_OBJ_JUMP_GROUP*: if the bike did not land on the
     * group), otherwise IDLE again. All goals are walked, done ones included [V]. */
    BmxRider *r = RIDER_OF(player);
    SaveGoalInfo *gi;
    const GoalDef *def;
    const GoalObjective *o;
    uint32_t goal, obj, k, grp;
    int state;

    if (!gameIsProquest() || g_currentLevel >= 6)
        return;
    gi = comdataGetGoalInfo(player, g_currentLevel);
    def = g_goalDefs[g_currentLevel * 3 + gi->tier].goal;
    for (goal = 0; goal < 4; goal++, def++) {
        for (obj = 0; obj < def->numObjectives; obj++) {
            o = &def->obj[obj];
            if (gameGetObjectiveState(player, goal, obj) != GOAL_STATE_PENDING)
                continue;
            if (!landed) {
                state = GOAL_STATE_IDLE;
            } else if (o->type == GOAL_OBJ_TRICK) {
                for (k = 0; k < r->trickCount; k++)
                    if (gameTrickMatches(&r->score.trick[k], o))
                        break;
                state = (k < r->trickCount) ? GOAL_STATE_DONE : GOAL_STATE_IDLE;
            } else if (o->type < GOAL_OBJ_JUMP_GROUP || o->type > GOAL_OBJ_JUMP_GROUP_TRICK) {
                state = GOAL_STATE_DONE;
            } else {
                state = GOAL_STATE_DONE;
                if (r->contact[1].type == 12) {
                    grp = (uint32_t)gameGroupOf((const uint8_t *)r->contact[1].obj);
                    if (o->param[0] <= grp && grp <= o->param[1])
                        state = GOAL_STATE_IDLE;
                }
            }
            gameSetObjectiveState(player, goal, obj, state);
        }
    }
}

/* VA 0x0040fe40, 904 bytes, [I] between, status draft */
void gameScoreTrickSet(BmxScoreBlock *set)
{
    /* Points of every trick in the set and the set score (Python port: progress.score_trick_set):
     *   points = (trick base + modifier base)
     *            x modifier factor (only with a trick) x modifier-count factor x repeat factor
     *            x spin factor + spin bonus, x1.2 / x1.2 / x1.3 for flag bits 0 / 1 / 2,
     *            + ticks held x max(trick rate, modifier rate);
     *   set = sum x variety factor (variety: +1 per distinct air trick / wall tap / manual /
     *         lone modifier, +0.25 per grind / stall / fast plant; factor interpolated, max 8).
     * The binary gets a pointer to rider->score; the rider is recovered from it. */
    BmxRider *r = (BmxRider *)((uint8_t *)set - offsetof(BmxRider, score));
    BmxTrickRec *t, *prev;
    float variety = 0.0f, grind = 0.0f, manual = 0.0f, mult;
    uint32_t total = 0, k, rate, repeat, mc;
    int pts, n, n2;
    int8_t tr, md;
    HudMeter meter;

    for (k = 0; k < r->trickCount; k++) {
        t = &set->trick[k];
        prev = k ? &set->trick[k - 1] : NULL;
        if (gameGetMode() != GAME_MODE_BMX &&
            (gameGetMode() < GAME_MODE_WIPEOUT || gameGetMode() > GAME_MODE_LONGEST_MANUAL)) {
            tr = (int8_t)t->trick;
            md = (int8_t)t->mod;
            pts = g_trickInfo[tr].points + g_trickInfo[47 + md].points;   /* tr = -1 reads the word before the table [V] */
            if (t->trick != TRICK_NONE)
                pts = (int)((float)pts * g_modMult[md + 1]);
            mc = (t->modCount > 7) ? 7 : t->modCount;
            pts = (int)((float)pts * g_modCountMult[mc]);
            repeat = (r->trickLog[tr + 1][(md + 1) >> 2] >> (((t->mod + 1) & 3) << 1)) & 3;
            pts = (int)((float)pts * g_repeatMult[repeat]);
            pts = (int)((float)pts * g_spinMult[t->spins]);
            pts += g_spinPoints[t->spins];
            if (t->flags & 1)
                pts = (int)((float)pts * 1.2f);
            if (t->flags & 2)
                pts = (int)((float)pts * 1.2f);
            if (t->flags & 4)
                pts = (int)((float)pts * 1.3f);
            rate = g_trickInfo[tr].rate;
            if (rate <= g_trickInfo[47 + md].rate)
                rate = g_trickInfo[47 + md].rate;
            pts += t->ticks * rate;
            t->points = (uint16_t)pts;
            total += pts;
            if (t->trick != TRICK_NONE) {
                if (!(prev && prev->trick == t->trick && prev->mod == t->mod)) {
                    if (t->trick < 0x1B)
                        variety += 1.0f;
                    else if (t->trick < 0x24 || t->trick < 0x2B)
                        variety += 0.25f;            /* grinds, stalls, fast plant */
                    else if (t->trick < 0x2E)
                        variety += 1.0f;             /* wall tap, manuals */
                }
            } else if (t->mod != TRICK_NONE && !(prev && prev->mod == t->mod)) {
                variety += 1.0f;
            }
        }
        if (TRICK_IS_GRIND(t->trick))
            grind += t->length;
        else if (TRICK_IS_MANUAL(t->trick))
            manual += t->length;
    }
    if (r->trickCount != 0) {
        uint8_t lastTrick = set->trick[r->trickCount - 1].trick;
        if (TRICK_IS_GRIND(lastTrick)) {
            meter.value = grind;
            meter.kind = 6;
            overlaySetItem(5, &meter);
        } else if (TRICK_IS_MANUAL(lastTrick)) {
            meter.value = manual;
            meter.kind = 8;
            overlaySetItem(5, &meter);
        }
    }
    if (!(variety > 1.0f))
        variety = 1.0f;
    n = (int)variety;
    mult = g_varietyMult[n > 8 ? 8 : n];
    if (variety - (float)n != 0.0f) {
        n2 = (int)(variety + 1.0f);
        if (n2 > 8)
            n2 = 8;
        mult += (g_varietyMult[n2] - mult) * (variety - (float)n);
    }
    set->setScore = (uint32_t)(int)((float)total * mult);
}

/* VA 0x004102c0, 100 bytes, [V] direct, status draft */
uint8_t gameGetObjectiveState(int player, int goal, uint32_t obj)
{
    /* SAVE.md 2.2: nibble obj of the goal entry, low 3 bits */
    SaveGoalInfo *gi;

    if (g_currentLevel > 5)
        zAssertFail("improper level for objective logic", GAME_SRC, 0xe9e, 0);
    gi = comdataGetGoalInfo(player, g_currentLevel);
    return (gi->goal[gi->tier][goal][1 + (obj >> 1)] >> ((obj & 1) << 2)) & 7;
}

/* VA 0x00410350, 233 bytes, [V] direct, status draft */
void gameSetObjectiveState(int player, int goal, uint32_t obj, int state)
{
    /* writes the whole nibble (so bit 3, the flag, is cleared); REWARDED switches the
     * objective's props */
    SaveGoalInfo *gi;
    uint8_t *b;
    uint32_t sh, j;
    const GoalObjective *o;
    void *prop;

    if (g_currentLevel > 5)
        zAssertFail("improper level for objective logic", GAME_SRC, 0xeb6, 0);
    gi = comdataGetGoalInfo(player, g_currentLevel);
    sh = (obj & 1) << 2;
    b = &gi->goal[gi->tier][goal][1 + (obj >> 1)];
    *b = (uint8_t)((*b & ~(0xF << sh)) | (state << sh));
    if (state == GOAL_STATE_REWARDED) {
        o = &g_goalDefs[g_currentLevel * 3 + gi->tier].goal[goal].obj[obj];
        for (j = 0; j < 4; j++)
            if ((int8_t)o->props[j] != -1 && (prop = bmxpropGetPropByID((int8_t)o->props[j])) != NULL)
                bmxpropActivate(player, prop);
    }
}

/* VA 0x00410480, 109 bytes, [V] direct, status draft */
uint8_t gameGetObjectiveFlag(int player, int goal, uint32_t obj, uint8_t mask)
{
    /* bit 3 of the objective's nibble (set by gameAdvanceGoalTier, cleared when the objective
     * first becomes active: "not seen yet" [I]) */
    SaveGoalInfo *gi;

    if (g_currentLevel > 5)
        zAssertFail("improper level for objective logic", GAME_SRC, 0xeda, 0);
    gi = comdataGetGoalInfo(player, g_currentLevel);
    return (uint8_t)((gi->goal[gi->tier][goal][1 + (obj >> 1)] >> (((obj & 1) << 2) + 3)) & 1 & mask);
}

/* VA 0x00410510, 143 bytes, [V] direct, status draft */
void gameSetObjectiveFlag(int player, int goal, uint32_t obj, uint8_t bit, int set)
{
    SaveGoalInfo *gi;
    uint8_t *b;
    uint32_t sh;

    if (g_currentLevel > 5)
        zAssertFail("improper level for objective logic", GAME_SRC, 0xef0, 0);
    gi = comdataGetGoalInfo(player, g_currentLevel);
    sh = ((obj & 1) << 2) + 3;
    b = &gi->goal[gi->tier][goal][1 + (obj >> 1)];
    if (set == 0)
        *b &= (uint8_t)~(bit << sh);
    else
        *b |= (uint8_t)(bit << sh);
}

/* ------------------------------------------------------------------ competitions and rewards */

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* VA 0x004105d0, 1333 bytes, [V] direct, status draft */
void gameJudgeRun(int player)
{
    /* Proquest competitions (levels 6..11): five judges score the run from the run score against
     * the level's par score, trick variety, areas used and the counts; the other nine riders get
     * random scores around 165..250 with a per-rider bias; g_compRanking sorts the averages
     * (gameAwardLevelProgress reads the top 3). Python port: progress.judge_base. */
    BmxRider *r = RIDER_OF(player);
    BmxJudgeRun *jr;
    uint32_t sScore, sTricks, sMods, sAreas, sNum, sModsN, sModified, sGrinds, sStalls, sManuals, sFalls;
    uint32_t uniq, mods, areas, t, m, j, i, sum, v, k, best;
    int base, bv;
    uint32_t avg[10];
    uint8_t val;
    SaveProfile *pf = &g_saveProfiles[player];

    if (!gameIsProquest() || g_currentLevel <= 5)
        return;
    if (gameGetRound() > 1)
        zAssertFail("", GAME_SRC, 0xf23, 0);
    jr = &r->judge[gameGetRound()];
    jr->score = (int32_t)r->score.runScore;
    g_pfnDebugPrintf("judge score calculation:\n");
    sScore = umin((r->score.runScore << 7) / g_compParScore[g_currentLevel - 6], 0x80);

    uniq = 0;                                /* air tricks landed with any modifier slot */
    for (t = 0; t < 27; t++)
        for (j = 0; j < 39; j++)
            if ((r->trickLog[t + 1][j >> 2] >> ((j & 3) << 1)) & 3) {
                uniq++;
                break;
            }
    sTricks = (uniq * 32 <= 256) ? uniq * 4 : 32;
    g_pfnDebugPrintf("total unique tricks performed: %d\n", uniq);
    mods = 0;                                /* modifiers 0..37 used with any air trick or alone */
    for (m = 1; m <= 38; m++)
        for (t = 0; t < 28; t++)
            if ((r->trickLog[t][m >> 2] >> ((m & 3) << 1)) & 3) {
                mods++;
                break;
            }
    sMods = umin((mods << 5) / 10, 32);
    g_pfnDebugPrintf("total unique modifiers performed: %d\n", mods);
    areas = 0;
    for (k = 0; k < 16; k++)
        if (r->area[k].tricks != 0 || r->area[k].modifiers != 0)
            areas++;
    sAreas = (areas * 16 <= 128) ? areas * 2 : 16;
    g_pfnDebugPrintf("total areas used: %d\n", areas);
    sNum = umin(r->total.tricks * 8u / 20, 8);
    g_pfnDebugPrintf("number of tricks: %d\n", r->total.tricks);
    sModsN = umin(r->total.modifiers * 8u / 24, 8);
    g_pfnDebugPrintf("number of modifiers: %d\n", r->total.modifiers);
    sModified = umin(r->total.modified, 8);
    g_pfnDebugPrintf("number of modified tricks: %d\n", r->total.modified);
    sGrinds = umin(r->total.grinds * 8u / 5, 8);
    g_pfnDebugPrintf("number of grinds: %d\n", r->total.grinds);
    sStalls = umin(r->total.stalls * 4u / 2, 4);
    g_pfnDebugPrintf("number of stalls: %d\n", r->total.stalls);
    sManuals = umin(r->total.manuals * 4u / 2, 4);
    g_pfnDebugPrintf("number of manuals: %d\n", r->total.manuals);
    sFalls = umin(r->total.falls * 4u / 3, 4);
    base = (int)(sScore + sTricks + sMods + sAreas + sNum + sModsN + sModified + sGrinds + sStalls +
                 sManuals) - (int)sFalls;
    g_pfnDebugPrintf("number of falls: %d\n", r->total.falls);
    g_pfnDebugPrintf("non random judgeScore is %d\n", base);

    sum = 0;
    for (j = 0; j < 5; j++) {
        if (r->score.runScore == 0) {
            jr->judge[j] = 0;
        } else {
            if (((int)(zmathRandom() & 7) + base >> 1) + 0x80 < 0x100)
                v = (uint32_t)(((int)(zmathRandom() & 7) + base >> 1) + 0x80);
            else
                v = 0xFF;
            jr->judge[j] = (uint8_t)v;
            g_pfnDebugPrintf("judge %d awards %d\n", j, v & 0xFF);
        }
        sum += jr->judge[j];
    }
    jr->average = (uint8_t)(sum / 5);

    /* the field: the player takes the place of his rider (Slim Jim Guy / Amish Boy: of rider 5) */
    for (i = 0; i < 10; i++) {
        if (i == pf->rider || (pf->rider > 9 && i == 5)) {
            val = jr->average;
        } else {
            int s = (0xA5 - (int)((float)(zmathRandom() & 0xFF) * -0.33333334f)) + g_judgeBias[i];
            val = (s < 0x100) ? (uint8_t)s : 0xFF;
        }
        g_judgeScores[i][gameGetRound()] = val;
        sum = 0;
        for (j = 0; j < (uint32_t)gameGetRound() + 1; j++)
            sum += g_judgeScores[i][j];
        avg[i] = sum / ((uint32_t)gameGetRound() + 1);
        g_judgeScores[i][2] = (uint8_t)avg[i];
        avg[i] &= 0xFF;
    }
    for (k = 0; k < 10; k++) {
        best = 0;
        bv = -1;
        for (i = 0; i < 10; i++)
            if (bv < (int)avg[i]) {
                best = i;
                bv = (int)avg[i];
            }
        g_compRanking[k] = (uint8_t)best;
        avg[best] = 0xFFFFFFFFu;
    }
}

/* VA 0x00410c60, 843 bytes, [I] row, status draft */
void gameAwardLevelProgress(int player)
{
    /* Level end in proquest: the tiers newly completed in this run (challenge levels: the tier
     * whose 4th goal was done this session; competitions: placing 3rd/2nd/1st = tiers 0..2 above
     * the stored comp result) give their rewards (Python port: progress.award_level_progress):
     *   - g_rewards.levelTier[level][tier][0..1];
     *   - when every level of the park is done at that tier: parkTier[park][tier], and for
     *     hardcore the record flag 4 << park;
     *   - when all 12 levels are done at hardcore: record flag 2, final[0] (per-rider reward),
     *     then final[1] (Amish Boy) if riders 0-9 all have flag 2, or final[2] for rider 11.
     * rider->runRewards collects what was given (results screen). */
    BmxRider *r = RIDER_OF(player);
    uint8_t rider;
    SaveRiderRecord *rec;
    SaveGoalInfo *gi;
    uint32_t from, to, t, k, scope, lo, hi, L, i, park;
    const GameReward *rw;

    if (!gameIsProquest())
        return;
    rider = g_saveProfiles[player].rider;
    rec = &g_saveBlockA.riders[rider];
    if (g_currentLevel < 6) {
        gi = comdataGetGoalInfo(player, g_currentLevel);
        from = to = gi->tier;
        if (gi->tier_goals_done == 4) {
            for (k = 0; k < 4; k++)
                if (gi->goal[gi->tier][k][0] & 0x40)
                    break;
            to = gi->tier + 1;
            if (k == 4)
                from = to;                   /* completed in an earlier session: nothing new */
        }
    } else {
        to = 0;
        for (i = 0; i < 3; i++)
            if (g_compRanking[i] == rider || (rider > 9 && g_compRanking[i] == 5))
                to = 3 - i;
        from = rec->comp_result[g_currentLevel - 6];
        if (to <= from)
            return;
        rec->comp_result[g_currentLevel - 6] = (uint8_t)to;
    }

    park = g_currentLevel / 3;
    for (t = from; t < to; t++) {
        for (k = 0; k < 2; k++) {
            r->runRewards |= 1u << (t * 2 + k);
            gameApplyReward(player, &g_rewards.levelTier[g_currentLevel][t][k]);
        }
        for (scope = 0; scope < 2; scope++) {
            if (scope == 0) {
                lo = park * 3;
                hi = lo + 2;
            } else {
                lo = 0;
                hi = 11;
            }
            for (L = lo; L <= hi; L++) {
                if (L < 6) {
                    SaveGoalInfo *g = comdataGetGoalInfo(player, L);
                    if (g->tier <= t && (g->tier != t || g->tier_goals_done != 4))
                        goto next_tier;
                } else if (rec->comp_result[L - 6] <= t) {
                    goto next_tier;
                }
            }
            if (scope == 0) {
                rw = &g_rewards.parkTier[park][t];
                if (rw->type != GAME_REWARD_NONE) {
                    r->runRewards |= 1u << (t + 6);
                    gameApplyReward(player, rw);
                }
                if (t == 2)
                    rec->flags |= 4u << park;
            } else if (t == 2) {
                r->runRewards |= 0x200;
                rec->flags |= 2;
                gameApplyReward(player, &g_rewards.final[0]);
                if (g_saveProfiles[player].rider < 10) {
                    for (i = 0; i < 10; i++)
                        if (!(g_saveBlockA.riders[i].flags & 2))
                            break;
                    if (i < 10)
                        continue;
                    rw = &g_rewards.final[1];
                    r->runRewards |= 0x400;
                } else {
                    if (g_saveProfiles[player].rider != 11)
                        continue;
                    rw = &g_rewards.final[2];
                    r->runRewards |= 0x800;
                }
                gameApplyReward(player, rw);
            }
        }
next_tier:
        ;
    }
}

/* VA 0x00411090, 340 bytes, [V] direct, status draft */
void gameApplyReward(int player, const GameReward *reward)
{
    /* Unlocks only ever raise (Python port: progress.apply_reward; SAVE.md 2.2). */
    uint8_t rider = g_saveProfiles[player].rider;
    SaveRiderRecord *rec = &g_saveBlockA.riders[rider];

    for (;;) {
        switch (reward->type) {
        case GAME_REWARD_PER_RIDER:
            reward = &g_rewards.riderFinal[rider][reward->param];
            continue;
        case GAME_REWARD_BIKE:
            if (rec->bikes_unlocked < reward->param + 1)
                rec->bikes_unlocked = (uint8_t)(reward->param + 1);
            rec->flags |= 0x40;              /* "new bike" notice */
            return;
        case GAME_REWARD_STYLE:
            if (rec->styles_unlocked < reward->param + 1)
                rec->styles_unlocked = (uint8_t)(reward->param + 1);
            rec->flags |= 0x80;
            return;
        case GAME_REWARD_RIDER:
            if (reward->param > 11)
                zAssertFail("goal completion error - char", GAME_SRC, 0x1040, 0);
            if (reward->param + 1 < g_saveBlockA.riders_unlocked)
                return;
            g_saveBlockA.riders_unlocked = (uint8_t)(reward->param + 1);
            return;
        case GAME_REWARD_LEVEL:
            if (reward->param > 11)
                zAssertFail("goal completion error - level", GAME_SRC, 0x1044, 0);
            if (rec->levels_unlocked < reward->param + 1)
                rec->levels_unlocked = (uint8_t)(reward->param + 1);
            rec->flags |= 0x100;
            if (reward->param == 1)
                g_saveProfiles[player].flags &= ~2;  /* first level unlock: tutorial hints off */
            return;
        case GAME_REWARD_MOVIE:
            rec->flags |= 1;
            return;
        case GAME_REWARD_CHEAT:
            g_saveBlockA.cheats_unlocked |= (uint16_t)(1 << (reward->param & 0x1F));
            return;
        default:                             /* none, 5, 6, 7 */
            return;
        }
    }
}

/* ------------------------------------------------------------------ objective highlight drawing */

/* vertex of psxDrawPoly3D / psxDrawLine3D (20 bytes) */
typedef struct HiliteVert {
    float   pos[3];
    float   unk;
    uint8_t rgb[3];         /* red, green, blue */
    uint8_t a;
} HiliteVert;

/* the blink: brightness 16..248 over 32 frames, used to scale the tier colour */
static uint32_t gameBlinkScale(void)
{
    uint32_t b = 0x1F - (g_gameFrame & 0x1F);
    return ((b & 0x1FFFFFFF) < 2) ? 0x10 : (uint8_t)(b << 3);
}

static void gameSetVertColour(HiliteVert *v, uint32_t colour, uint32_t scale)
{
    v->rgb[0] = (uint8_t)(((colour >> 16) & 0xFF) * scale >> 8);
    v->rgb[1] = (uint8_t)(((colour >> 8) & 0xFF) * scale >> 8);
    v->rgb[2] = (uint8_t)((colour & 0xFF) * scale >> 8);
}

/* VA 0x00411270, 366 bytes, [I] callers, status draft */
void gameDrawEdgeHighlight(const float *edge, uint32_t colour)
{
    /* A tagged edge is a triangle (normal at +0, corners at +0x30, stride 16 bytes): draw it pushed
     * out along the normal on both sides, the offset shrinking as the blink brightens. */
    uint32_t scale = gameBlinkScale();
    float off = (float)((int)(400 - scale) / 30);
    HiliteVert v[3];
    int i, k;

    for (i = 0; i < 3; i++) {
        for (k = 0; k < 3; k++)
            v[i].pos[k] = off * edge[k] + edge[12 + i * 4 + k];
        gameSetVertColour(&v[i], colour, scale);
    }
    psxDrawPoly3D(10, 3, v, 0);
    for (i = 0; i < 3; i++) {                /* back side, reversed winding */
        for (k = 0; k < 3; k++)
            v[2 - i].pos[k] = edge[12 + i * 4 + k] - off * edge[k];
        gameSetVertColour(&v[2 - i], colour, scale);
    }
    psxDrawPoly3D(10, 3, v, 0);
}

/* VA 0x00411440, 704 bytes, [I] callers, status draft */
void gameDrawObjectHighlight(const uint8_t *object, uint32_t colour)
{
    /* A BSP object: a wire box around its bounding box (s16 min at +8, max at +0xE), in the frame
     * of its two axis vectors (+0x18, +0x24), growing with the blink. 4 x 4 lines. */
    float mat[16], d[3], p[3];      /* rows: axis A, axis B, A x B (zmathTriangleNormal), 0 */
    uint32_t scale = gameBlinkScale(), face, c, k;
    float half = (float)(g_gameFrame & 0x1F) * 1.5f;
    HiliteVert v[4];
    const int16_t *lo = (const int16_t *)(object + 8);
    const int16_t *hi = (const int16_t *)(object + 0xE);

    for (k = 0; k < 3; k++) {
        mat[k] = ((const float *)(object + 0x18))[k];
        mat[4 + k] = ((const float *)(object + 0x24))[k];
    }
    zmathTriangleNormal((RwV3d *)&mat[8], (const RwV3d *)&mat[0], &g_zmathOrigin, (const RwV3d *)&mat[4]);
    zmathVecZero((RwV3d *)&mat[12]);
    for (face = 0; face < 4; face++) {
        for (c = 0; c < 4; c++) {
            d[0] = (c & 2) ? half : -half;
            if ((face & 1) == 0) {
                d[1] = ((c + 1) & 2) ? -half : half;
                d[2] = (face & 2) ? half : -half;
            } else {
                d[1] = (face & 2) ? half : -half;
                d[2] = ((c + 1) & 2) ? -half : half;
            }
            zmathTransformPoint((RwV3d *)p, (const RwMatrix *)mat, (const RwV3d *)d);
            for (k = 0; k < 3; k++)
                v[c].pos[k] = (float)((c & 2) ? hi[k] : lo[k]) + p[k];
            gameSetVertColour(&v[c], colour, scale);
        }
        psxDrawLine3D(10, &v[0], &v[1]);
        psxDrawLine3D(10, &v[1], &v[2]);
        psxDrawLine3D(10, &v[2], &v[3]);
        psxDrawLine3D(10, &v[3], &v[0]);
    }
}

/* collision shape of a prop (zcolGetPropShape): 1 cylinder (r, h), 2-4 boxes (x, y, z) */
typedef struct PropShape {
    uint8_t type;
    uint8_t pad[3];
    float   size[3];
    float   yoff;
} PropShape;

static void gamePropMatrix(float *m, const int16_t *prop)
{
    /* the prop's orientation: its model quaternion (y negated) then the prop's angles */
    const float *q = (const float *)(*(uint8_t **)(*(uint8_t **)((const uint8_t *)prop + 0x1F0) + 0x1C) + 0x14);
    float qq[4];
    int16_t angles[4];

    zmathMatrixIdentity((RwMatrix *)m);
    angles[0] = prop[2];
    angles[1] = (int16_t)-prop[3];
    angles[2] = prop[4];
    angles[3] = 0;
    qq[0] = q[0];
    qq[1] = -q[1];
    qq[2] = q[2];
    qq[3] = q[3];
    zquatToMatrix((RwMatrix *)m, (Zquat *)qq);
    zmathMatrixRotateXYZ((RwMatrix *)m, (const RwMatrix *)m, angles);
    zmathMatrixRotateXYZ((RwMatrix *)m, (const RwMatrix *)m, prop + 2);
}

static void gamePropCorner(float *out, const PropShape *s, uint32_t face, uint32_t c, float grow, int yoffFirst)
{
    float e[3];
    if (s->type == 1) {
        e[0] = s->size[0]; e[1] = s->size[1]; e[2] = s->size[0];
    } else if (s->type >= 2 && s->type <= 4) {
        e[0] = s->size[0]; e[1] = s->size[1]; e[2] = s->size[2];
    } else {
        zmathVecZero((RwV3d *)e);
    }
    if ((face & 4) == 0) {
        out[0] = ((c & 2) ? 1.0f : -1.0f) * (e[0] + grow);
        if ((face & 1) == 0) {
            out[1] = ((c + 1) & 2) ? -(e[1] + grow) : grow;
            out[2] = (face & 2) ? (e[2] + grow) : -(e[2] + grow);
        } else {
            out[1] = (face & 2) ? grow : -(e[1] + grow);
            out[2] = ((c + 1) & 2) ? -(e[2] + grow) : (e[2] + grow);
        }
    } else {
        out[0] = (face & 1) ? (e[0] + grow) : -(e[0] + grow);
        out[1] = ((c + 1) & 2) ? -(e[1] + grow) : grow;
        out[2] = (c & 2) ? (e[2] + grow) : -(e[2] + grow);
    }
    if (yoffFirst)
        out[1] += s->yoff;
}

/* VA 0x004117b0, 926 bytes, [I] callers, status draft */
void gameDrawPropHighlight(const int16_t *prop, uint32_t colour)
{
    /* A prop: a wire box (6 faces x 4 lines) around its collision shape, grown by 20 cm plus the
     * blink. The y offset of the shape is added after the rotation. Cylinders (type 3) skip
     * the corner computation of the top and bottom faces [V]. [I] exact sign pattern. */
    const PropShape *s = (const PropShape *)zcolGetPropShape(prop[0]);
    uint32_t scale = gameBlinkScale(), face, c, k;
    float grow = (float)(g_gameFrame & 0x1F) * 1.5f + 20.0f;
    float m[16], d[3], p[3];
    HiliteVert v[4];

    gamePropMatrix(m, prop);
    for (face = 0; face < 6; face++) {
        for (c = 0; c < 4 && !((face & 4) && s->type == 3); c++) {
            gamePropCorner(d, s, face, c, grow, 0);
            zmathTransformPoint((RwV3d *)p, (const RwMatrix *)m, (const RwV3d *)d);
            v[c].pos[0] = (float)prop[5] + p[0];
            v[c].pos[1] = (float)prop[6] + s->yoff + p[1];
            v[c].pos[2] = (float)prop[7] + p[2];
            gameSetVertColour(&v[c], colour, scale);
        }
        psxDrawLine3D(10, &v[0], &v[1]);
        psxDrawLine3D(10, &v[1], &v[2]);
        psxDrawLine3D(10, &v[2], &v[3]);
        psxDrawLine3D(10, &v[3], &v[0]);
    }
}

/* VA 0x00411c50, 962 bytes, [I] callers, status draft */
void gameDrawPropHighlightSolid(const int16_t *prop, uint32_t colour)
{
    /* As gameDrawPropHighlight, but each face is drawn as a filled quad, front and back
     * (two psxDrawPoly3D polygons with the corners reversed); the y offset is added before
     * the rotation. */
    const PropShape *s = (const PropShape *)zcolGetPropShape(prop[0]);
    uint32_t scale = gameBlinkScale(), face, c;
    float grow = (float)(g_gameFrame & 0x1F) * 1.5f + 20.0f;
    float m[16], d[3], p[3];
    HiliteVert v[4], back[4];

    gamePropMatrix(m, prop);
    for (face = 0; face < 6; face++) {
        for (c = 0; c < 4 && !((face & 4) && s->type == 3); c++) {
            gamePropCorner(d, s, face, c, grow, 1);
            zmathTransformPoint((RwV3d *)p, (const RwMatrix *)m, (const RwV3d *)d);
            v[c].pos[0] = (float)prop[5] + p[0];
            v[c].pos[1] = (float)prop[6] + p[1];
            v[c].pos[2] = (float)prop[7] + p[2];
            gameSetVertColour(&v[c], colour, scale);
        }
        psxDrawPoly3D(10, 4, v, 0);
        back[0] = v[3];
        back[1] = v[2];
        back[2] = v[1];
        back[3] = v[0];
        psxDrawPoly3D(10, 4, back, 0);
    }
}

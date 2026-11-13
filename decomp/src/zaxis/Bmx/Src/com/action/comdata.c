/*
 * zaxis/Bmx/Src/com/action/comdata.c
 * Original: C:\zaxis\Bmx\Src\com\action\comdata.c
 * Evidence: [V] path string 0x00553EB0 referenced by comdataGetGoalInfo; the other two [I]: they sit
 *           directly before it in link order (after the camera gap), touch only the save globals
 *           and are the accessors DM-012/DM-024 called save_InitDefaults / save_GetRecordPtr.
 * 3 functions, VA 0x00407ba0-0x0040800d.
 * DM-046 (draft). Non-matching C (AD-8). Types: save.h, game.h. Format: docs/formats/SAVE.md.
 * Python port (tests/test_decomp_dm046.py): tools/dmbmx/formats/progress.py.
 *
 * "Common data": the parts of the game state that the memory card stores (g_saveBlockA,
 * g_saveRecords, g_saveProfiles; memcard.c copies them to and from SAVE.DAT verbatim).
 */
#include "decomp.h"

#define COMDATA_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\comdata.c"

/* Default control bindings of a profile (button masks, SAVE.md 2.5). */
static const uint32_t comdataDefaultControls[7] = { 0x80, 0x20, 0x40, 0x10, 0x100, 0x200, 0x4000 };

/* VA 0x00407ba0, 295 bytes, [I] row, status draft */
void comdataInitDefaults(void)
{
    /* The defaults the game runs with when there is no SAVE.DAT (SAVE.md section 2). */
    int r, level, tier, goal, i;
    uint8_t p;

    for (r = 0; r < 12; r++) {
        SaveRiderRecord *rec = &g_saveBlockA.riders[r];
        rec->flags = 0;
        rec->levels_unlocked = 1;
        rec->styles_unlocked = 1;
        rec->bikes_unlocked = 1;
        zstringMemSet(rec->tricks_landed, 0, sizeof(rec->tricks_landed));
        for (level = 0; level < 6; level++) {
            /* [V] 0x00407BDD: the inner loops never advance their pointer, so the same three
             * bytes (byte0, tier, tier_goals_done) are cleared 12 times and the 12 goal entries
             * keep their (zero-initialised .bss) value. Probably a bug in the original. */
            uint8_t *gi = (uint8_t *)&rec->goals[level];
            for (tier = 0; tier < 3; tier++)
                for (goal = 0; goal < 4; goal++) {
                    gi[0] = 0;
                    gi[1] = 0;
                    gi[2] = 0;
                }
        }
        for (i = 0; i < 6; i++)
            rec->comp_result[i] = 0;
    }
    g_saveBlockA.riders_unlocked = 10;     /* Slim Jim Guy and Amish Boy are rewards */
    g_saveBlockA.cheats_unlocked = 0;
    g_saveBlockA.cheats_active = 0;
    g_saveBlockA.volume_0c = 18;
    g_saveBlockA.volume_0d = 18;
    g_saveBlockA.music_track = 1;
    g_saveBlockA.language = 0;

    for (p = 0; p < 2; p++) {
        SaveProfile *pr = &g_saveProfiles[p];
        pr->rider = 0;
        pr->style = 0;
        pr->index = p;
        pr->unk_02 = 0;
        pr->camera = 0;
        pr->flags = 3;                       /* bit 1: tutorial hints on */
        zstringPrintf(pr->name, "player");
        for (i = 0; i < 7; i++)
            pr->control[i] = comdataDefaultControls[i];
        pr->unk_40[0xB] = 0;                 /* +0x4B */
        /* pr->bike (+5) is not set here [V] */
    }
    g_currentPlayer = 0;
    g_numPlayers = 1;
}

/* VA 0x00407d20, 474 bytes, [I] row, status draft */
SaveRecordEntry *comdataGetRecordPtr(uint32_t id)
{
    /* The saved (all-time) record for a record id of the current level; the live-table twin is
     * gameGetLiveRecord 0x0040BE50 (same switch, slot instead of level). The 24 entries per
     * level (SAVE.md 2.4):
     *    0..2  single player, top 3 by run score (index = rank)
     *    3..6  best run, sub-mode 0..3 = 30 s / 60 s / 2 min / 5 min
     *    7     high five           8  B-M-X          9  random spot (run)   10  random spot (total)
     *   11     wipeout         12..15 death match, sub-mode 0..3
     *   16..23 stat 2..9: trick set, single trick, big air, highest mark, longest grind,
     *          longest jump, longest manual, longest skid (modes 6-10 share these) */
    SaveRecordEntry *lvl = g_saveRecords[g_currentLevel];
    uint32_t stat = GAME_RECORD_STAT(id);
    uint32_t sub = GAME_RECORD_SUB(id);

    if (stat > 1)
        return &lvl[stat + 14];
    switch (GAME_RECORD_MODE(id)) {
    case GAME_MODE_SINGLE:          return &lvl[GAME_RECORD_INDEX(id)];   /* not range-checked [V] */
    case GAME_MODE_BEST_RUN:        return &lvl[3 + sub];
    case GAME_MODE_HIGH_FIVE:       if (stat == STAT_RUN) return &lvl[7]; break;
    case GAME_MODE_BMX:             if (stat == STAT_RUN) return &lvl[8]; break;
    case GAME_MODE_RANDOM_SPOT:
        if (stat == STAT_TOTAL) return &lvl[10];
        if (stat == STAT_RUN) return &lvl[9];
        break;
    case GAME_MODE_WIPEOUT:         if (stat == STAT_RUN) return &lvl[11]; break;
    case GAME_MODE_LONGEST_GRIND:   return &lvl[20];
    case GAME_MODE_HIGHEST_WALLTAP: return &lvl[19];
    case GAME_MODE_BIG_AIR:         return &lvl[18];
    case GAME_MODE_FURTHEST_JUMP:   return &lvl[21];
    case GAME_MODE_LONGEST_MANUAL:  return &lvl[22];
    case GAME_MODE_DEATH_MATCH:     return &lvl[12 + sub];
    }
    return NULL;
}

/* VA 0x00407fc0, 78 bytes, [V] direct, status draft */
SaveGoalInfo *comdataGetGoalInfo(int player, uint32_t level)
{
    /* Goal progress of challenge level 0..5 in the career record of the player's rider. */
    if (level > 5)
        zAssertFail("invalid level for goal info", COMDATA_SRC, 0x34a, 0);
    return &g_saveBlockA.riders[g_saveProfiles[player].rider].goals[level];
}

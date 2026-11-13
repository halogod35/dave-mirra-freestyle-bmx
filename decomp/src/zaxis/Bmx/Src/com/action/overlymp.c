/*
 * zaxis/Bmx/Src/com/action/overlymp.c
 * Original: C:\zaxis\Bmx\Src\com\action\overlymp.c
 * Evidence: [V] path string 0x00557434, referenced by the asserts of overlympTurnDraw (lines 0x195,
 *           0x1B8) and overlympStandingsDraw (0x270, 0x29E, 0x33C, 0x367); [I] the others: link order
 *           (0x004124C0-0x004147AD, between memcard.c and overlay.c) and the four multiplayer entries
 *           of the overlay screen table (0x00557178-0x005571A8) that point at them.
 * 11 functions, 6914 bytes, VA 0x004124c0-0x004147ad.
 * Draft. Non-matching C (AD-8). Types: overlay.h, zoverii.h. Notes: docs/re/hud.md.
 *
 * The overlay pass created 5 functions Ghidra had missed here (reached only through the screen table) and
 * took in the overlymp/overlay gap function overlympDrawLetters 0x00414580 (called from here only).
 *
 * The multiplayer overlay screens: game select (22), the B-M-X word entry (23), "READY!" before
 * each turn (24) and "TIME UP" / winner after it (25). Texts 0x3xxx are the multiplayer string set
 * (overlayLoadStrings loads it only with two players).
 */

#include "decomp.h"

#include "zsound.h"
#include "zoverii.h"
#include "overlay.h"

#define OVERLAYMP_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\overlymp.c"

/* ------------------------------------------------------------------ other modules (current names) */
uint32_t  actionSetMode(uint32_t mode);                       /* action.c */
uint8_t   gameStart(uint8_t mode);                            /* game.c */
uint8_t   gameSetSubMode(uint8_t sub);
uint8_t   gameGetRoundWinner(uint32_t round, uint32_t which);
char     *gameGetWord(void);
char     *gameSetWord(const char *word);  /* 0x0040BFD0 game.c, "horse word too long" */
extern const uint8_t g_gameModeStat[12];  /* 0x00553658 */
extern BmxTrickRec   g_bmxTarget[3];      /* 0x00695260 the B-M-X set to copy */
extern uint8_t       g_bmxTargetCount;    /* 0x00695290 */
extern uint8_t       g_actionPausePlayer; /* 0x006962F5 */
extern uint8_t       g_overlympSubMode;   /* 0x00556EF8 overlay.c: best run time limit 0..3 */
extern const char    g_overlayEmpty[];    /* 0x0056BD44 "" */

enum { SND_CHANGE = 0x7D1, SND_SELECT = 0x7D2, SND_BACK = 0x7D3, SND_ERROR = 0x7D4, SND_DELETE = 0x7D5 };

/* ------------------------------------------------------------------ globals (this file) */
char    g_overlympWord[9] = "BMX";     /* 0x00557254 the B-M-X word being entered */
uint8_t g_overlympWordCursor = 2;      /* 0x0055725D */

/* ------------------------------------------------------------------ helpers */
static void drawCentredSmall(const char *s, int y, uint32_t colour)
{
    int16_t w = zoveriiTextWidth(s, &g_overlaySmallFont);

    zoveriiDrawText(&g_overlaySmallFont, s, (int16_t)(0x13F - ((uint16_t)w >> 1)), (int16_t)y, colour, 0);
}

/* "label:" left, a value right-aligned (the score lines of these screens) */
static void drawRow(const char *label, const char *value, int y, uint32_t colour)
{
    char buf[64];

    zstringPrintf(buf, "%s:", label);
    zoveriiDrawText(&g_overlaySmallFont, buf, 0x35, (int16_t)y, OVL_COL_ITEM, 0);
    zoveriiDrawText(&g_overlaySmallFont, value, (int16_t)(0x24A - zoveriiTextWidth(value, &g_overlaySmallFont)),
                    (int16_t)y, colour, 0);
}

/* the value colour: blinking red while it is a record the current player has just beaten */
static uint32_t recordColour(const GameLiveRecord *live)
{
    if ((live->flags & 1) && live->player == g_currentPlayer && (g_overlayFrame & 1))
        return OVL_COL_NEW;
    return OVL_COL_VALUE;
}

static void formatValue(char *buf, uint32_t v)
{
    if (gameGetMode() >= GAME_MODE_LONGEST_GRIND && gameGetMode() <= GAME_MODE_LONGEST_MANUAL)
        overlayFormatDistance(buf, (float)v);
    else
        zstringPrintf(buf, "%ld", v);
}

/* ------------------------------------------------------------------ functions */

/* VA 0x004124c0, 268 bytes, [I] split, status draft */
void overlympSelectDraw(void)
{
    /* SELECT GAME: best run (- 30 sec / 60 sec / 2 min / 5 min), high five, B-M-X, random spot,
     * wipeout, longest grind, highest walltap, big air contest, furthest jump, longest manual
     * (game mode = line + 1; death match is not offered), then "exit" */
    char buf[32];
    const char *s;
    uint32_t i;
    int y;

    overlympDrawTitle(zTextGet(0x3000));
    y = 0xF0 - ((g_overlayScreen->count * 0x14 - 0x14) >> 1);
    for (i = 0; i < (uint32_t)g_overlayScreen->count - 1; i++, y += 0x14) {
        if (i == 0) {
            zstringPrintf(buf, "%s - %s", zTextGet(0x3002), zTextGet(0x300Du + g_overlympSubMode));
            s = buf;
        } else {
            s = zTextGet(0x3002 + i);
        }
        zoveriiDrawText(&g_overlayTinyFont, s, (int16_t)(0x13F - ((uint16_t)zoveriiTextWidth(s, &g_overlayTinyFont) >> 1)),
                        (int16_t)y, i == g_overlayScreen->sel ? overlayPulseColour() : OVL_COL_LABEL, 0);
    }
    overlayDrawMenu(2, g_overlayScreen->count - 1);
}

/* VA 0x00412620, 55 bytes, [I] split, status draft */
void overlympDrawTitle(const char *s)
{
    /* the big white title at y 0x3C (also used by overlay.c's competition screens) */
    zoveriiDrawText(&g_overlaySmallFont, s, (int16_t)(0x13F - ((uint16_t)zoveriiTextWidth(s, &g_overlaySmallFont) >> 1)),
                    0x3C, OVL_COL_TITLE, 0);
}

/* VA 0x00412670, 372 bytes, [I] row, status draft */
int overlympSelectUpdate(void)
{
    /* any player: ok / start starts the game (gameStart(line + 1)); left / right on "best run"
     * cycles its time limit; "exit" asks EXIT? */
    OverlayScreen *s = g_overlayScreen;
    uint32_t p;

    if (overlayConfirmed((uint32_t)s->count - 1) && (int)s->sel - (int)s->count == -1) {
        g_overlayConfirmReturn = OVL_SCREEN_MP_SELECT;
        overlaySetScreen(OVL_SCREEN_CONFIRM);
        g_overlayCurrentOnly = 0;
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    for (p = 0; p < g_numPlayers; p++) {
        if (contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0) || contrlerTestButtons(p, PAD_START, 0)) {
            gameSetSubMode(g_overlympSubMode);
            gameStart((uint8_t)(g_overlayScreen->sel + 1));
            bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        if (contrlerTestButtons(p, PAD_LEFT, 0)) {
            if (g_overlayScreen->sel == 0) {
                if ((int8_t)--g_overlympSubMode < 0)
                    g_overlympSubMode = 3;
                bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
                return 1;
            }
        } else if (contrlerTestButtons(p, PAD_RIGHT, 0) && g_overlayScreen->sel == 0) {
            if (++g_overlympSubMode == 4)
                g_overlympSubMode = 0;
            bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
            return 1;
        }
    }
    return 0;
}

/* VA 0x00412850, 224 bytes, [I] between, status draft */
void overlympWordDraw(void)
{
    /* "select word": always 8 letter places 0x35 apart (centred for the current length) */
    char buf[32];
    uint32_t i;
    int x;

    drawCentredSmall(zTextGet(0x304D), 0x6C, OVL_COL_ITEM);
    x = 0x13F - ((zstringLength(g_overlympWord) * 0x35 - 0x35) >> 1);
    for (i = 0; i < 8; i++, x += 0x35) {
        zstringPrintf(buf, "%c", g_overlympWord[i]);
        zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(x - ((uint16_t)zoveriiTextWidth(buf, &g_overlaySmallFont) >> 1)),
                        0xE2, i == g_overlympWordCursor ? overlayPulseColour() : OVL_COL_ITEM, 0);
    }
}

/* VA 0x00412970, 509 bytes, [I] row, status draft */
int overlympWordUpdate(void)
{
    /* The B-M-X word (default "BMX", at most 8 letters, A-Z / a-z): up / down change the letter,
     * right copies it one place on, left deletes; ok sets it (gameSetWord) and starts the turn;
     * back returns to the game intro. */
    uint32_t p, snd;
    char *c;

    for (p = 0; p < g_numPlayers; p++) {
        if (contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0)) {
            if (zstringLength(g_overlympWord) == 0) {
                bmxsoundEvent(SND_ERROR, 0, 0, 0, 0);
                return 1;
            }
            overlaySetScreen(OVL_SCREEN_NONE);
            gameSetWord(g_overlympWord);
            gameSetState(GAME_STATE_TURN);
            bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        if (contrlerTestButtons(p, PAD_FIRE1 | PAD_FIRE5, 0)) {
            overlaySetScreen(OVL_SCREEN_INTRO);
            bmxsoundEvent(SND_BACK, 0, 0, 0, 0);
            return 1;
        }
        c = &g_overlympWord[g_overlympWordCursor];
        if (contrlerTestButtons(p, PAD_LEFT, 0)) {
            if (g_overlympWordCursor == 0) {
                snd = SND_ERROR;
            } else {
                *c = '\0';
                g_overlympWordCursor--;
                snd = SND_DELETE;
            }
        } else if (contrlerTestButtons(p, PAD_RIGHT, 0)) {
            if (g_overlympWordCursor >= 7) {
                snd = SND_ERROR;
            } else {
                c[1] = c[0];
                g_overlympWordCursor++;
                snd = SND_SELECT;
            }
        } else if (contrlerTestButtons(p, PAD_UP, 0)) {
            *c = *c == 'Z' ? 'a' : *c == 'z' ? 'A' : (char)(*c + 1);
            snd = SND_CHANGE;
        } else if (contrlerTestButtons(p, PAD_DOWN, 0)) {
            *c = *c == 'A' ? 'z' : *c == 'a' ? 'Z' : (char)(*c - 1);
            snd = SND_CHANGE;
        } else {
            continue;
        }
        bmxsoundEvent(snd, 0, 0, 0, 0);
        g_overlayBlink = 0;
        return 1;
    }
    return 0;
}

/* VA 0x00412bf0, 1603 bytes, [I] row, status draft */
void overlympTurnDraw(void)
{
    /* Before a turn: "round n" (high five, random spot), "first / second ... / last attempt" (the
     * stat games) or "READY!"; "player one" / "player two"; B-M-X copier: the set to copy
     * (overlayTrickName(.., full = 0) and the input of each trick in the button font, "to" between);
     * otherwise the score to beat (this game) and the all-time best. */
    const BmxScoreBlock *sc = &g_riders[g_currentPlayer].score;
    const GameLiveRecord *live;
    char buf[256];
    const char *s;
    uint32_t i, id, rounds;
    int y;

    switch (gameGetMode()) {
    case GAME_MODE_HIGH_FIVE:
    case GAME_MODE_RANDOM_SPOT:
        zstringPrintf(buf, "%s %ld", zTextGet(0x1004), gameGetRound() + 1);
        s = buf;
        break;
    default:
        s = zTextGet(0x304E);
        break;
    case GAME_MODE_WIPEOUT: case GAME_MODE_LONGEST_GRIND: case GAME_MODE_HIGHEST_WALLTAP:
    case GAME_MODE_BIG_AIR: case GAME_MODE_FURTHEST_JUMP: case GAME_MODE_LONGEST_MANUAL:
        rounds = gameGetNumRounds();
        id = gameGetRound() < ((rounds - 1) & 0xFFFF) ? (gameGetRound() + 0x1010u) & 0xFFFF : 0x1014;
        zstringPrintf(buf, "%s %s", zTextGet(id), zTextGet(0x1005));
        s = buf;
        break;
    }
    overlympDrawTitle(s);
    zstringPrintf(buf, "%s %s", zTextGet(0x1000), zTextGet(0x100Cu + g_currentPlayer));
    drawCentredSmall(buf, 0x6C, OVL_COL_ITEM);
    y = 0x8F;
    if (gameGetMode() == GAME_MODE_BMX) {
        if (!gameTestFlag(GAME_FLAG_SETTER)) {
            zstringPrintf(buf, "%s:", zTextGet(0x304F));
            drawCentredSmall(buf, 0x8F, OVL_COL_ITEM);
            y = 0xAC;
            for (i = 0; i < g_bmxTargetCount; i++, y += g_overlayButtonFont.height) {
                overlayTrickName(buf, &g_bmxTarget[i], 0);
                zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(0xBA - ((uint16_t)zoveriiTextWidth(buf, &g_overlayTinyFont) >> 1)),
                                (int16_t)(y + (g_overlayButtonFont.height >> 1) - 8), 0x02808080, 0);
                buf[0] = '\0';
                if (g_bmxTarget[i].trick != 0xFF)
                    zstringPrintf(buf + zstringLength(buf), "%s", zTextGet(0x10BBu + g_bmxTarget[i].trick));
                if (g_bmxTarget[i].mod != 0xFF) {
                    if (g_bmxTarget[i].trick != 0xFF)
                        zstringPrintf(buf + zstringLength(buf), " ");
                    zstringPrintf(buf + zstringLength(buf), "%s", zTextGet(0x10E9u + g_bmxTarget[i].mod));
                }
                zoveriiDrawText(&g_overlayButtonFont, buf,
                                (int16_t)(0x1C4 - ((uint16_t)zoveriiTextWidth(buf, &g_overlayButtonFont) >> 1)), (int16_t)y,
                                0x00808080, 0);
                if (i < (uint32_t)g_bmxTargetCount - 1) {
                    zstringPrintf(buf, "to");
                    zoveriiDrawText(&g_overlayTinyFont, buf,
                                    (int16_t)(0xBA - ((uint16_t)zoveriiTextWidth(buf, &g_overlayTinyFont) >> 1)),
                                    (int16_t)(y + g_overlayButtonFont.height - 8), 0x02A07010, 0);
                }
            }
        }
    } else {
        if (gameGetMode() == GAME_MODE_RANDOM_SPOT) {
            zstringPrintf(buf, "%ld", sc->totalScore);
            drawRow(zTextGet(0x1001), buf, 0x8F, OVL_COL_VALUE);
            y = 0xAC;
        }
        if (!gameTestFlag(GAME_FLAG_SETTER) || (gameGetMode() != GAME_MODE_HIGH_FIVE && gameGetRound() != 0)) {
            if (gameGetMode() == GAME_MODE_HIGH_FIVE)
                id = GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_RUN, 2, 0);
            else
                id = GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), g_gameModeStat[gameGetMode()], 1, 0);
            live = gameGetLiveRecord(id);
            if (live == NULL)
                zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x195, NULL);
            formatValue(buf, live->value);
            drawRow(zTextGet(0x100A), buf, y, OVL_COL_VALUE);
            y += 0x1D;
        }
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), g_gameModeStat[gameGetMode()], 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x1B8, NULL);
        formatValue(buf, live->value);
        drawRow(zTextGet(0x100B), buf, y, OVL_COL_VALUE);
        if (gameGetMode() == GAME_MODE_HIGH_FIVE)
            overlympDrawRoundLeader(y + 0x23);
    }
    if (gameGetMode() == GAME_MODE_BMX && !gameTestFlag(GAME_FLAG_SETTER))
        overlayDrawMenu(1, 0);                       /* continue / view replay (the set) / exit */
    else
        overlayDrawMenu(0, 0);
}

/* VA 0x004134a0, 463 bytes, [I] row, status draft */
void overlympDrawRoundLeader(int y)
{
    /* "player one leads 2-1" / "player two wins 3-0" / "... sweeps ..." / "series tied 1-1" */
    char buf[32];
    uint32_t w, id, p;

    if (!gameTestFlag(GAME_FLAG_TIE)) {
        w = gameGetRoundWinner(gameGetRound(), 1);
        if (!gameTestFlag(GAME_FLAG_OVER))
            id = 0x1017;                                         /* leads */
        else
            id = 0x1015u + (g_riders[w].score.wins != gameGetNumRounds()); /* sweeps / wins */
        zstringPrintf(buf, "%s %s %s ", zTextGet(0x1000), zTextGet(0x100C + w), zTextGet(id));
    } else {
        zstringPrintf(buf, "%s ", zTextGet(0x1018));
    }
    if (g_numPlayers == 2 && !gameTestFlag(GAME_FLAG_TIE)) {
        w = gameGetRoundWinner(gameGetRound(), 1);
        zstringPrintf(buf + zstringLength(buf), "%ld-%ld", g_riders[w].score.wins, g_riders[w == 0].score.wins);
    } else {
        for (p = 0; p < g_numPlayers; p++)
            zstringPrintf(buf + zstringLength(buf), "%s%ld", p ? "-" : g_overlayEmpty, g_riders[p].score.wins);
    }
    drawCentredSmall(buf, y, OVL_COL_ITEM);
}

/* VA 0x004136f0, 174 bytes, [I] row, status draft */
int overlympTurnUpdate(void)
{
    /* continue: start the run; B-M-X copier: view replay shows the setter's run; exit: EXIT? */
    int copier = gameGetMode() == GAME_MODE_BMX && !gameTestFlag(GAME_FLAG_SETTER);

    if (!overlayConfirmed(0))
        return 0;
    switch (g_overlayScreen->sel) {
    case 0:
        overlaySetScreen(OVL_SCREEN_NONE);
        gameSetState(GAME_STATE_RUN);
        break;
    case 1:
        if (copier) {
            overlaySetScreen(OVL_SCREEN_NONE);
            actionSetMode(2);
            g_actionPausePlayer = g_currentPlayer;
            break;
        }
        /* fall through: "exit" */
    case 2:
        g_overlayConfirmReturn = OVL_SCREEN_MP_TURN;
        overlaySetScreen(OVL_SCREEN_CONFIRM);
        break;
    }
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* VA 0x004137d0, 2781 bytes, [V] direct, status draft */
void overlympStandingsDraw(void)
{
    /* After a turn ("TIME UP"): the player's score and the all-time best (blinking red when just
     * beaten); high five: every player's run score, the round winner in green, or "tie" and
     * "replay the round", and the round leader; B-M-X: each player's letters; random spot: run and
     * total score with their bests. Then continue / view replay / exit. */
    const BmxScoreBlock *sc = &g_riders[g_currentPlayer].score;
    const GameLiveRecord *live;
    char buf[32], value[32];
    uint32_t p, w;
    int y = 0x6C;

    switch (gameGetMode()) {
    case GAME_MODE_BEST_RUN:
        overlympDrawTitle(zTextGet(0x3050));
        zstringPrintf(buf, "%s %s", zTextGet(0x1000), zTextGet(0x100Cu + g_currentPlayer));
        drawCentredSmall(buf, y, OVL_COL_ITEM);
        y += 0x23;
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_TOTAL, 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x270, NULL);
        zstringPrintf(value, "%ld", sc->runScore);
        drawRow(zTextGet(0x1001), value, y, recordColour(live));
        y += 0x1D;
        zstringPrintf(value, "%ld", live->value);
        drawRow(zTextGet(0x100B), value, y, recordColour(live));
        y += 0x1D;
        break;
    case GAME_MODE_HIGH_FIVE:
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_RUN, 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x29E, NULL);
        if (!gameTestFlag(GAME_FLAG_ROUND_DONE)) {
            overlympDrawTitle(zTextGet(0x3050));
            zstringPrintf(buf, "%s %s", zTextGet(0x1000), zTextGet(0x100Cu + g_currentPlayer));
            drawCentredSmall(buf, y, OVL_COL_ITEM);
            y += 0x23;
            zstringPrintf(value, "%ld", sc->runScore);
            drawRow(zTextGet(0x1001), value, y, recordColour(live));
            y += 0x1D;
            zstringPrintf(value, "%ld", live->value);
            drawRow(zTextGet(0x100B), value, y, recordColour(live));
        } else if (!gameTestFlag(GAME_FLAG_MISSED)) {
            overlympDrawTitle(zTextGet(0x3052));
            for (p = 0; p < g_numPlayers; p++, y += 0x1D) {
                zstringPrintf(buf, "%s %s", zTextGet(0x1000), zTextGet(0x100C + p));
                zstringPrintf(value, "%ld", g_riders[p].score.runScore);
                w = gameGetRoundWinner(gameGetRound(), 2);
                drawRow(buf, value, y,
                        ((live->flags & 1) && live->player == p && (g_overlayFrame & 1)) ? OVL_COL_NEW :
                        p == w ? 0x00E0A040u : OVL_COL_VALUE);
            }
            zstringPrintf(value, "%ld", live->value);
            drawRow(zTextGet(0x100B), value, y, recordColour(live));
        } else {
            overlympDrawTitle(zTextGet(0x1019));
            drawCentredSmall(zTextGet(0x3051), y, OVL_COL_ITEM);
        }
        y += 0x23;
        overlympDrawRoundLeader(y);
        y += 0x1D;
        break;
    case GAME_MODE_BMX:
        overlympDrawTitle(zTextGet(0x3050));
        if (gameTestFlag(GAME_FLAG_ROUND_DONE)) {
            overlympDrawLetters(&y);
            overlayDrawMenu(1, 0);
            return;
        }
        break;
    case GAME_MODE_RANDOM_SPOT:
        overlympDrawTitle(zTextGet(0x3050));
        zstringPrintf(buf, "%s %s", zTextGet(0x1000), zTextGet(0x100Cu + g_currentPlayer));
        drawCentredSmall(buf, y, OVL_COL_ITEM);
        y += 0x23;
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_RUN, 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x33C, NULL);
        zstringPrintf(value, "%ld", sc->runScore);
        drawRow(zTextGet(0x1001), value, y, recordColour(live));
        y += 0x1D;
        zstringPrintf(value, "%ld", live->value);
        drawRow(zTextGet(0x100B), value, y, recordColour(live));
        y += 0x1D;
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), STAT_TOTAL, 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAYMP_SRC, 0x367, NULL);
        zstringPrintf(value, "%ld", sc->totalScore);
        drawRow(zTextGet(0x1003), value, y, recordColour(live));
        y += 0x1D;
        zstringPrintf(value, "%ld", live->value);
        drawRow(zTextGet(0x100B), value, y, recordColour(live));
        y += 0x23;
        break;
    }
    overlayDrawMenu(1, 0);
}

/* VA 0x00414580, 276 bytes, [I] row (was gap_overlymp__overlay.c), status draft */
void overlympDrawLetters(int *y)
{
    /* B-M-X: per player "player n:" and the letters of the word collected so far (score.wins),
     * spread over x 0x1B4..0x23A; the newest letter blinks red if it came with this run */
    char buf[32];
    uint32_t p, k, n;
    const BmxScoreBlock *sc;

    for (p = 0; p < g_numPlayers; p++, *y += 0x1D) {
        sc = &g_riders[p].score;
        zstringPrintf(buf, "%s %s:", zTextGet(0x1000), zTextGet(0x100C + p));
        zoveriiDrawText(&g_overlaySmallFont, buf, 0x35, (int16_t)*y, OVL_COL_ITEM, 0);
        n = sc->wins;
        if (n == 0)
            continue;
        buf[1] = '\0';
        for (k = 0; k < n; k++) {
            buf[0] = gameGetWord()[k];
            zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(0x86 * (k + 1) / (n + 1) + 0x1B4), (int16_t)*y,
                            (sc->runScore == 0 && k == n - 1 && (g_overlayFrame & 1)) ? OVL_COL_NEW : OVL_COL_VALUE, 0);
        }
    }
}

/* VA 0x004146f0, 189 bytes, [I] row, status draft */
int overlympStandingsUpdate(void)
{
    /* continue: next turn, or the level end when the game is over; view replay; exit: EXIT?
     * (view replay returns 0, as in the exe) */
    if (!overlayConfirmed(0))
        return 0;
    switch (g_overlayScreen->sel) {
    case 0:
        overlaySetScreen(OVL_SCREEN_NONE);
        gameSetState(gameTestFlag(GAME_FLAG_OVER) ? GAME_STATE_LEVEL_END : GAME_STATE_TURN);
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    case 1:
        overlaySetScreen(OVL_SCREEN_NONE);
        actionSetMode(2);
        g_actionPausePlayer = g_currentPlayer;
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        break;
    case 2:
        g_overlayConfirmReturn = OVL_SCREEN_MP_STANDINGS;
        overlaySetScreen(OVL_SCREEN_CONFIRM);
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    return 0;
}

/*
 * zaxis/Bmx/Src/com/menu/frontend.c
 * Original: C:\zaxis\Bmx\src\com\menu\frontend.c
 * Evidence: [V] path string 0x0055B4C0 referenced by frontendLoadMacros (asserts 0xDD, 0xE4, 0xED);
 *           "frontendInitApp:" / "FRONTEND:" messages 0x0055B2E4-0x0055B518 (frontendInitApp,
 *           frontendAllocMacros); [I] the rest by link order (0x0043BE90-0x0044DD23, after guitoggl.c,
 *           before bmxsound.c) and because it is all screen code over guimain.c.
 * 136 functions, VA 0x0043be90-0x0044dd23.
 * Draft. Non-matching C (AD-8). Types: gui.h, save.h, game.h. Notes: docs/re/frontend.md.
 *
 * The front end: one handler per screen (guiLoadScreen(screen, handler)); a screen's sub-pages
 * swap the handler with guiSetScreenHandler. Animation and deferred work run through one
 * per-frame task pointer, g_feTask, which the screen handler calls on GUI_MSG_FRAME and which
 * clears itself when done (so "g_feTask != NULL" doubles as "busy, ignore input").
 *
 * Screens: 0xA005 title / main menu (PROQUEST, SESSION, FREERIDE, COOL STUFF, OPTIONS, QUIT),
 * 0xA009 the select screen (rider, bike, style, level pages), 0xA00C options (controller, cheats,
 * audio; color adjust is unreachable), 0xA00D cool stuff (challenges, high scores, movies,
 * credits), and three PSX leftovers no PC path reaches: 0xA012 load / save (memory card),
 * 0xA014 language, and the memory-card autoload on 0xA005.
 * 60 of the 136 functions were not in the Ghidra project (they are reached only through the
 * /INCREMENTAL thunks, as screen handlers and tasks): decomp/symbols/symbols.csv creates them.
 */
#include <string.h>
#include "decomp.h"
#include "zmem.h"
#include "zstring.h"
#include "zsound.h"
#include "gui.h"

#define FRONTEND_SRC "C:\\zaxis\\Bmx\\src\\com\\menu\\frontend.c"

/* ------------------------------------------------------------------ other modules (current names) */
int   memcard_CheckSaveExists(void);              /* memcard.c: 6 = SAVE.DAT exists, 5 = not */
int   memcard_Stub0(void);
int   memcard_LoadSave(void);
int   memcard_WriteSave(void);
void  zcardSelectPort(uint8_t slot);                 /* PC stand-ins for the PSX memory card */
int   zcardFormat(void);                         /* "format": returns 0 */
void  zcardStart(void);                         /* card access begins */
void  zcardStop(void);                         /* ... ends */
int   ngappPlayMovie(const void *const *playlist);  /* gap_MissingZGE__NgApp.c: play a movie (first entry) */
void  zconsoleLog(const char *msg);       /* log */
void  zsystemSetActionMode(void);
void  zrollerDisplayLoadingScreen(void);
void  zrollerLoadCodeModule(int a);
void  zrollerNop1(int a);
void  zrollerNop2(int a);
void  zrollerInit(void);
void  zrollerShutdown(void);                    /* frontendShutdownApp */
uint32_t *psx2DGetDepth(void);                 /* the current PSX ordering-table entry */
void  psx2DSetDepth(uint32_t *ot);             /* ... set it */
/* zinputGetDevice 0x00480400: contrler.h */
int   zrwZglConfirmQuit(void);                      /* zrwZgl.c: "really quit?" -> 1 */

extern SaveProfile  g_saveProfiles[2];
extern ZassetLib   *g_miscDataZal;                /* 0x00678490 */
extern uint8_t      g_numPlayers;                 /* 0x0069621C */
extern uint8_t      g_currentLevel;               /* 0x0056B614 */
extern uint8_t      g_selectedSubMode;            /* 0x0056B616 0 proquest, 1 session, 2 free ride (main menu) */
extern uint8_t      g_gameNextMode;               /* 0x0056B617 select page to reopen after a game */
extern uint32_t     g_appQuit;                    /* 0x00551FD4 */
extern uint32_t    *g_psxPrimNext;                /* 0x005D2510 PSX primitive buffer (NgIFpsx.c) */
extern const char  *g_padButtonNames[18];         /* 0x0055A3F4 "Up" .. "Fire 6" */
extern const char  *g_keyLayoutNames[5];          /* 0x0055A43C "< KEYBOARD LAYOUT n >" */
extern const char  *g_keyLayoutLabels[5][18];     /* 0x0055A450 key names per layout */

/* bmxsoundEvent ids */
enum {
    SND_MUSIC     = 1000,   /* (1000, 0) stop, (1000, 1) start the menu music */
    SND_VOLUME    = 0x3EA,  /* (ch, level 0..126): ch 0 = sfx (save +0x0C), ch 1 = music (+0x0D) */
    SND_MOVE      = 2000,
    SND_CHANGE    = 0x7D1,
    SND_SELECT    = 0x7D2,
    SND_BACK      = 0x7D3,
    SND_ERROR     = 0x7D4,
    SND_CARD_BEGIN= 0x7D8,
    SND_CARD_END  = 0x7D9
};
#define SOUND(ev)   bmxsoundEvent((ev), 0, 0, 0, 0)

/* screens (SCRN 0xA000 | n) and their elements (screen << 16 | n) */
#define SCR_MAIN     0xA005
#define SCR_SELECT   0xA009
#define SCR_OPTIONS  0xA00C
#define SCR_COOL     0xA00D
#define SCR_CARD     0xA012
#define SCR_LANGUAGE 0xA014
#define EID(scr, n)  ((uint32_t)(scr) << 16 | (n))
#define MAIN(n)      EID(SCR_MAIN, n)
#define SEL(n)       EID(SCR_SELECT, n)
#define OPT(n)       EID(SCR_OPTIONS, n)
#define COOL(n)      EID(SCR_COOL, n)
#define CARD(n)      EID(SCR_CARD, n)
#define LANG(n)      EID(SCR_LANGUAGE, n)

/* msg + 13 = the same message from the second pad (never sent on PC) */
#define P2(m)        ((m) + GUI_MSG_PAD2)

typedef void (*FeStep)(int delta);

/* ------------------------------------------------------------------ globals */
GuiTask    g_feTask;                 /* 0x0057458C per-frame task of the current screen */
FeStep    g_feStep;                 /* 0x00574588 select screen: left / right on the current page */
FeStep    g_feSlideDone;            /* 0x00574590 select screen: after the carousel stopped */
ZprimAsset *g_feCursorImage;        /* 0x00574594 the 64x32 cursor frame texture (sprite 0x40C4) */
int32_t   g_feCursorPhase;          /* 0x00574584 0..64 */
int32_t   g_feCursorStep = 2;       /* 0x0055A354 */
GuiTask    g_feCardDone;             /* 0x0057459C */
uint16_t  g_feMainSel;              /* 0x005745A0 main menu item 0..5 */
GuiTask    g_feMainNext;             /* 0x005745A4 the action of the chosen item, after 3 frames */
uint32_t  g_feBootLoaded;           /* 0x005745A8 */
uint16_t  g_feBootStep;             /* 0x005745AC autoload step */
uint16_t  g_feTitleShown;           /* 0x005745AE */
uint32_t  g_feExit;                 /* 0x00574654 frontendRequestExit: leave the menus */
uint32_t  g_feExitQuit;             /* 0x00574658 ... and quit the program */
extern uint8_t g_feAutoload;      /* 0x0056B618 memory-card autoload on start (never set on PC) */
void     *g_feMacroBuf;             /* 0x0057465C 16 KB for the frontend text set */
uint32_t  g_feMacroSize;            /* 0x00574660 */

/* the four main-menu tabs (in the options / cool stuff screens) slide 40 px */
int32_t   g_feTab0, g_feTab1, g_feTab2, g_feTab3; /* 0x00691F28, 0x00691F24, 0x00691F20, 0x00691F10 */

const uint8_t  g_feLanguageCodes[4] = { 1, 2, 3, 0 };          /* 0x0055A358 */
int16_t   g_feLangSel;              /* 0x00691F04 */

/* main menu: the six items (texts) and the carousel geometry (7 rows) */
const uint16_t g_feMainItems[6] = { 0x24, 0x25, 0x26, 0x29, 0x2A, 0x2C };   /* 0x0055A35C PROQUEST ... QUIT */
const int16_t  g_feMainRowPos[7][2] = {                                     /* 0x0055A368 */
    { 290, 219 }, { 305, 256 }, { 317, 300 }, { 328, 345 }, { 325, 390 }, { 321, 433 }, { 317, 471 }
};
int16_t   g_feMainRowAlpha[8] = { 4, 32, 64, 255, 64, 32, 4, 0 };       /* 0x0055A384 */
const int16_t g_feMainRowGrey[8] = { 32, 128, 128, 128, 128, 128, 32, 0 }; /* 0x0055A394 */
uint32_t  g_feTitleTimer;           /* 0x00691EB4 idle frames (1800 = 30 s: demo) */
uint32_t  g_feMainDelay;            /* 0x00691EB8 */
uint16_t  g_feTitleFrames;          /* 0x00691EBE */
uint8_t   g_feBootSlot;             /* 0x00691EBC */
uint16_t  g_feScroll;               /* 0x00691EA8 main menu scroll 0..60 */

/* memory card dialogue (PSX; reachable only from the unused load / save screen) */
int16_t   g_feCardYes;              /* 0x00691EC0 yes / no cursor */
uint16_t  g_feCardLoaded;           /* 0x00691ED4 */
int16_t   g_feCardDelay;            /* 0x00691ED6 */
uint16_t  g_feCardUnk;              /* 0x00691ED8 */
uint16_t  g_feCardSaved;            /* 0x00691EDA */
uint32_t  g_feCardIds[8];           /* 0x00691EE0 the dialogue's text elements */
uint16_t  g_feCardState;            /* 0x00691F00 */
uint16_t  g_feCardSlot;             /* 0x00691F02 */

/* empty string (overlay.c's) */
#define EMPTY g_overlayEmpty

/* ------------------------------------------------------------------ this file */
int frontendWrap(int v, int n);
void frontendCursorCreate(void);
void frontendCursorFree(void);
void frontendCursorDraw(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t shade, int full);
int frontendTabsOut(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void frontendTabsReset(void);
int frontendTabsIn(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
int frontendLanguageEnter(void);
void frontendLanguageInit(void);
void frontendLanguageDone(void);
void frontendCardOpen(int16_t isSave, uint16_t slot, const uint32_t ids[8], uint16_t unk, GuiTask done);
void frontendCardClose(void);
void frontendCardCheckLoad(void);
void frontendCardLoad(void);
void frontendCardAskFormat(void);
void frontendCardFormat(void);
void frontendCardShowContinue(void);
void frontendCardAskSave(void);
void frontendCardAskOverwrite(void);
void frontendCardCheckSave(void);
void frontendCardSave(void);
void frontendCardShowLoadResult(int r);
void frontendCardShowSaveResult(int r);
void frontendAutoloadEnter(void);
void frontendTitleEnter(void);
int frontendMainMenuEnter(int again);
void frontendMainMenuSetHandler(void);
void frontendMainMenuReturn(void);
void frontendMainMenuNext(void);
void frontendMainMenuPrev(void);
void frontendMainMenuRefresh(void);
int frontendStartDemo(void);
void frontendCheatsEnter(void);
void frontendCheatsLeave(void);
void frontendCheatsSlideIn(void);
void frontendCheatsSlideOut(void);
void frontendColorAdjustDraw(void);
void frontendColorAdjustEnter(void);
void frontendColorAdjustLeave(void);
void frontendControllerEnter(void);
void frontendControllerLeave(void);
void frontendAudioEnter(void);
void frontendAudioLeave(void);
void frontendAudioSlideIn(void);
void frontendAudioSlideOut(void);
int frontendOptionsEnter(void);
void frontendOptionsRefresh(void);
void frontendOptionsExit(void);
void frontendOptionsTabsOut(void);
void frontendOptionsTabsIn(void);
void frontendOptionsInit(void);
int frontendLoadSaveEnter(void);
void frontendLoadSaveInit(void);
void frontendLoadSaveDone(void);
void frontendLoadSaveReturn(void);
void frontendRiderStep(int delta);
void frontendRiderShow(int delta);
void frontendRiderPageEnter(void);
void frontendRiderPageLeave(void);
void frontendLevelStep(int delta);
void frontendLevelShowNow(int delta);
void frontendLevelShow(int delta);
void frontendLevelPageEnter(void);
void frontendLevelPageLeave(void);
void frontendStyleStep(int delta);
void frontendStyleShow(int delta);
void frontendStylePageEnter(void);
void frontendStylePageLeave(void);
void frontendBikeSpin(void);
void frontendBikeStep(int delta);
void frontendBikeShow(int delta);
void frontendBikePageEnter(void);
void frontendBikePageLeave(void);
void frontendSelectCheats(int16_t key);
int frontendSelectEnter(void);
void frontendSelectExit(void);
void frontendSelectStart(void);
void frontendSelectMakeImage(int slot, uint16_t w, uint16_t h, uint16_t bpp, uint16_t colours);
void frontendCarouselReset(void);
void frontendSelectChangePage(int dir);
void frontendTickerSet(const uint16_t *lines);
void frontendTickerScroll(int n, int at);
void frontendSelectTabsShow(void);
void frontendSelectTabsHide(void);
void frontendCarouselAnimate(void);
void frontendSelectSlideIn(void);
void frontendSelectSlideOut(void);
void frontendSelectInit(void);
void frontendMoviePlay(void);
void frontendMoviesEnter(void);
void frontendMoviesLeave(void);
void frontendMoviesSlideIn(void);
void frontendMoviesSlideOut(void);
void frontendRecordsEnter(void);
void frontendRecordsLeave(void);
void frontendRecordsBuild(void);
void frontendRecordsSlideIn(void);
void frontendRecordsSlideOut(void);
void frontendChallengesEnter(void);
void frontendChallengesLeave(void);
void frontendChallengesBuild(void);
void frontendChallengesSlideIn(void);
void frontendChallengesSlideOut(void);
int frontendCoolStuffEnter(void);
void frontendCoolStuffExit(void);
void frontendCoolStuffRefresh(void);
void frontendCoolStuffTabsIn(void);
void frontendCreditsPlay(void);
void frontendCoolStuffTabsOut(void);
void frontendCoolStuffInit(void);
void frontendCoolStuffReset(void);
int frontendAllocMacros(void);
int frontendInitApp(void);
int frontendLoadMacros(uint32_t language);
void frontendShutdownApp(void);
int frontendUpdateApp(void);
void frontendRequestExit(int quit);

/* ------------------------------------------------------------------ helpers */
static void setText(uint32_t id, const char *s)   { guiSetText(guiFindElement(id), s); }
static void showId(uint32_t id)                   { guiShow(guiFindElement(id)); }
static void hideId(uint32_t id)                   { guiHide(guiFindElement(id)); }
static void textPal(uint32_t id, uint32_t pal)    { guiSetTextPalette(guiFindElement(id), pal); }
static void textPos(uint32_t id, int x, int y)    { guiSetTextPos(guiFindElement(id), (int16_t)x, (int16_t)y); }
static void sprPos(uint32_t id, int x, int y)     { guiSetSpriteOffset(guiFindElement(id), (int16_t)x, (int16_t)y); }
static void tint(uint32_t id, int r, int g, int b, int a)
{
    guiSetHighlight(guiFindElement(id), (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint32_t)a);
}
static void setVolumes(void)
{
    bmxsoundEvent(SND_VOLUME, 1, g_saveBlockA.volume_0d * 7, 0, 0);
    bmxsoundEvent(SND_VOLUME, 0, g_saveBlockA.volume_0c * 7, 0, 0);
}

/* ================================================================== shared */

/* VA 0x0043be90, 27 bytes, [I] row, status draft */
int frontendWrap(int v, int n)
{
    while (v < 0)
        v += n;
    while (v >= n)
        v -= n;
    return v;
}

/* VA 0x0043bec0, 47 bytes, [I] row, status draft */
void frontendCursorCreate(void)
{
    g_feCursorImage = zprimCreateImage(0x40, 0x20, 4, 0x10);
    zprimCopyMaterial(zprimLoadAsset(0x40C4), g_feCursorImage);
}

/* VA 0x0043bf00, 29 bytes, [I] row, status draft */
void frontendCursorFree(void)
{
    if (g_feCursorImage != NULL)
        zprimFree(g_feCursorImage);
    g_feCursorImage = NULL;
}

/* One gouraud-shaded textured quad (PC-side PSX primitive, 0xA4 bytes; code 0x3E) added to the
 * ordering table: 4 vertices (x, y, u, v) with a grey level each. */
typedef struct FeQuadVertex { int16_t x, y, u, v; } FeQuadVertex;

static uint32_t *cursorQuad(uint32_t *p, uint32_t *ot, uint32_t material, const uint8_t shade[4],
                            const FeQuadVertex v[4])
{
    uint8_t *b = (uint8_t *)p;
    int k;

    p[0] = (p[0] & 0x0FFFFFFF) | 0xC0000000;            /* 12 words to the GPU */
    b[7] = 0x3C;
    for (k = 0; k < 4; k++) {
        /* vertex k: grey at +4 / +0x14 / +0x20 / +0x2C, x y at +0x0C + 12k, u v at +0x10 + 12k */
        uint8_t *rgb = b + (k == 0 ? 4 : 0x14 + 12 * (k - 1));
        rgb[0] = rgb[1] = rgb[2] = shade[k];
        *(int16_t *)(b + 0x0C + 12 * k) = v[k].x;
        *(int16_t *)(b + 0x0E + 12 * k) = v[k].y;
        *(int16_t *)(b + 0x10 + 12 * k) = v[k].u;
        *(int16_t *)(b + 0x12 + 12 * k) = v[k].v;
    }
    p[0xF] = (p[0xF] & 0xFFFFFF82) | 0x7F82;
    p[0xE] = material;
    b[7] = 0x3E;                                       /* semi-transparent */
    *(int16_t *)(b + 0x0A) = 0x20;
    p[0] = ((*ot ^ p[0]) & 0x0FFFFFFF) ^ p[0];          /* addPrim(ot, p) */
    *ot = ((*ot ^ (uint32_t)p) & 0x0FFFFFFF) ^ *ot;
    return p + 0x29;
}

/* VA 0x0043bf30, 1953 bytes, [I] row, status draft */
void frontendCursorDraw(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t shade, int full)
{
    /* The selection frame around (x0, y0)-(x1, y1): 8 quads of the 64x32 frame texture (three
     * along the top, three along the bottom, one on each side), drawn as PSX primitives straight
     * into the ordering table, grey level 'shade' (the callers pulse it). full == 0 draws only
     * the left side (top-left, top, left, bottom-left, bottom) and fades the top and bottom edges
     * to black towards the right. The (u, v) values are as the exe has them. */
    const int16_t XA = (int16_t)(x0 + 0x20), XB = (int16_t)(x1 - 0x20);
    const int16_t YT = (int16_t)(y0 - 0x10), YM = (int16_t)(y0 + 0x10);
    const int16_t YB = (int16_t)(y1 - 0x10), YC = (int16_t)(y1 - 0x30);
    const FeQuadVertex q0[4] = { { x0, YT, 0, 0 },     { XA, YT, 0x20, 0 },    { x0, YM, 0, 0x20 },    { XA, YM, 0x20, 0x20 } };
    const FeQuadVertex q1[4] = { { XA, YT, 0x10, 0 },  { XB, YT, 0x20, 0 },    { XA, YM, 0x20, 0x20 }, { XB, YM, 0x20, 0x20 } };
    const FeQuadVertex q2[4] = { { XB, YM, 0x20, 0x20 }, { XB, YT, 0, 0x20 },  { x1, YM, 0x20, 0 },    { x1, YT, 0, 0 } };
    const FeQuadVertex q3[4] = { { x1, YM, 0x10, 0 },  { x1, YC, 0x20, 0 },    { XB, YM, 0x1F, 0x20 }, { XB, YC, 0x1F, 0x20 } };
    const FeQuadVertex q4[4] = { { x1, YB, 0, 0 },     { XB, YB, 0x20, 0 },    { x1, YC, 0, 0x1F },    { XB, YC, 0x20, 0x1F } };
    const FeQuadVertex q5[4] = { { x0, YC, 0x10, 0 },  { x0, YM, 0x20, 0 },    { XA, YC, 0x20, 0x20 }, { XA, YM, 0x20, 0x20 } };
    const FeQuadVertex q6[4] = { { x0, YB, 0, 0 },     { x0, YC, 0x20, 0 },    { XA, YB, 0, 0x20 },    { XA, YC, 0x20, 0x20 } };
    const FeQuadVertex q7[4] = { { XA, YC, 0x20, 0x20 }, { XB, YC, 0x10, 0x20 }, { XA, YB, 0x10, 0 },  { XB, YB, 0x20, 0 } };
    uint8_t lit[4], fade[4];
    ZprimImage *img = (ZprimImage *)zprimLockAsset(g_feCursorImage);
    uint32_t mat = (uint32_t)img->frame[0].material;
    uint32_t *p = g_psxPrimNext;
    uint32_t *ot = psx2DGetDepth();

    lit[0] = lit[1] = lit[2] = lit[3] = shade;
    fade[0] = fade[2] = shade;
    fade[1] = fade[3] = full ? shade : 1;              /* vertices 1 and 3 almost black */
    p = cursorQuad(p, ot, mat, lit, q0);
    p = cursorQuad(p, ot, mat, fade, q1);
    if (full) {
        p = cursorQuad(p, ot, mat, lit, q2);
        p = cursorQuad(p, ot, mat, lit, q3);
        p = cursorQuad(p, ot, mat, lit, q4);
    }
    p = cursorQuad(p, ot, mat, lit, q5);
    p = cursorQuad(p, ot, mat, lit, q6);
    p = cursorQuad(p, ot, mat, fade, q7);
    psx2DSetDepth(ot - 1);
    g_psxPrimNext = p;
    zprimUnlockAsset(g_feCursorImage);
}

/* VA 0x0043c8d0, 375 bytes, [I] row, status draft */
int frontendTabsOut(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    /* the four main-menu tabs slide back to x = -72 one after the other (step = distance to 47 / 3);
     * returns 1 when the last one is out */
    g_feTab0 += (47 - g_feTab0) / -3;
    if (g_feTab0 < 0)
        g_feTab1 += (47 - g_feTab1) / -3;
    if (g_feTab1 < 0)
        g_feTab2 += (47 - g_feTab2) / -3;
    if (g_feTab2 < 0)
        g_feTab3 += (47 - g_feTab3) / -3;
    if (g_feTab0 < -72) g_feTab0 = -72;
    if (g_feTab1 < -72) g_feTab1 = -72;
    if (g_feTab2 < -72) g_feTab2 = -72;
    if (g_feTab3 < -72) g_feTab3 = -72;
    sprPos(a, g_feTab0, 0);
    sprPos(b, g_feTab1, 0);
    sprPos(c, g_feTab2, 0);
    sprPos(d, g_feTab3, 0);
    return g_feTab3 < -71;
}

/* VA 0x0043cab0, 26 bytes, [I] row, status draft */
void frontendTabsReset(void)
{
    g_feTab0 = g_feTab1 = g_feTab2 = g_feTab3 = -72;
}

/* VA 0x0043cae0, 519 bytes, [I] row, status draft */
int frontendTabsIn(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    /* ... and in to x = 40 (step = distance / 7), dimmed (white, alpha 0x50) */
    g_feTab0 += (47 - g_feTab0) / 7;
    if (g_feTab0 > 0)
        g_feTab1 += (47 - g_feTab1) / 7;
    if (g_feTab1 > 0)
        g_feTab2 += (47 - g_feTab2) / 7;
    if (g_feTab2 > 0)
        g_feTab3 += (47 - g_feTab3) / 7;
    if (g_feTab0 > 40) g_feTab0 = 40;
    if (g_feTab1 > 40) g_feTab1 = 40;
    if (g_feTab2 > 40) g_feTab2 = 40;
    if (g_feTab3 > 40) g_feTab3 = 40;
    sprPos(a, g_feTab0, 0);
    sprPos(b, g_feTab1, 0);
    sprPos(c, g_feTab2, 0);
    sprPos(d, g_feTab3, 0);
    tint(a, 0xFF, 0xFF, 0xFF, 0x50);
    tint(b, 0xFF, 0xFF, 0xFF, 0x50);
    tint(c, 0xFF, 0xFF, 0xFF, 0x50);
    tint(d, 0xFF, 0xFF, 0xFF, 0x50);
    return g_feTab3 > 39;
}

/* ================================================================== language (0xA014, PSX only) */

static int frontendLanguageHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* VA 0x0043cd80, 78 bytes, [I] row, status draft */
int frontendLanguageEnter(void)
{
    /* [V] not referenced: the PC build never offers the language screen */
    int i;

    guiLoadScreen(SCR_LANGUAGE, frontendLanguageHandler);
    g_feLangSel = 0;
    for (i = 0; i < 3; i++) {
        if (g_feLanguageCodes[i] == g_saveBlockA.language) {
            g_feLangSel = (int16_t)i;
            break;
        }
    }
    g_feTask = frontendLanguageInit;
    return 0;
}

/* VA 0x0043cdf0, 215 bytes, [I] row, status draft */
void frontendLanguageInit(void)
{
    textPal(LANG(4), 2);
    textPal(LANG(5), 2);
    textPal(LANG(6), 2);
    setText(LANG(3), zTextGet(0x249));      /* LANGUAGE */
    setText(LANG(4), zTextGet(0x24B));      /* ENGLISH  -> language 1 */
    setText(LANG(5), zTextGet(0x24D));      /* DEUTSCH  -> language 2, which is the French text set */
    setText(LANG(6), zTextGet(0x24C));      /* FRANCAIS -> language 3, the German one (labels swapped) */
    g_feTask = NULL;
}

/* VA 0x0043cf10, 19 bytes, [I] row, status draft */
void frontendLanguageDone(void)
{
    g_feTask = NULL;
    frontendMainMenuReturn();
}

/* VA 0x0043cf40, 58 bytes, [I] row, status draft */
static int frontendLanguageHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    uint32_t sel;

    switch (msg) {
    case GUI_MSG_FRAME:
        zprimSetAttr(ZPRIM_ATTR_COLOUR, 0x10101);
        zprimDrawRect(0x28, 0, 0x1DB, 200);
        if (g_feTask != NULL)
            g_feTask();
        tint(LANG(4), 0xC0, 0xC0, 0xC0, 0x80);
        tint(LANG(5), 0xC0, 0xC0, 0xC0, 0x80);
        tint(LANG(6), 0xC0, 0xC0, 0xC0, 0x80);
        textPal(LANG(3), 2);
        textPal(LANG(4), g_feLangSel == 0);
        textPal(LANG(5), g_feLangSel == 1);
        textPal(LANG(6), g_feLangSel == 2);
        if (g_feLangSel >= 0 && g_feLangSel <= 2) {
            sel = LANG(4 + g_feLangSel);
            tint(sel, 0xC0, 0xC0, 0x80, (int)g_guiPulse + 0x20);
        }
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        SOUND(SND_MOVE);
        g_feLangSel = (int16_t)frontendWrap(g_feLangSel - 1, 3);
        return 1;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        SOUND(SND_MOVE);
        g_feLangSel = (int16_t)frontendWrap(g_feLangSel + 1, 3);
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        g_saveBlockA.language = g_feLanguageCodes[g_feLangSel];
        frontendLoadMacros(g_feLanguageCodes[g_feLangSel]);
        g_feTask = frontendLanguageDone;
        SOUND(SND_SELECT);
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        g_feTask = frontendLanguageDone;
        if (!g_guiAutoBack)
            SOUND(SND_BACK);
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
}

/* ================================================================== memory card dialogue (PSX) */
/*
 * States (g_feCardState): save path 0 check -> 3 "SAVE GAME?" / 4 "OVERWRITE?" / 1 "FORMAT IT?"
 * -> 5 formatting / 6 saving -> 7 done / 8 failed / 10 format failed; load path 0x100 check ->
 * 0x101 loading -> 0x102 done / 0x103 failed; 0xB close (calls g_feCardDone), 0xC closed.
 * On PC memcard_CheckSaveExists only returns 6 or 5, and zcardFormat / memcard_Stub0 return 0.
 */

static void cardClear(int first)
{
    int i;

    for (i = first; i < 8; i++)
        setText(g_feCardIds[i], EMPTY);
}

static void cardSlotLine(uint32_t id)
{
    char buf[200];

    zstringPrintf(buf, zTextGet(0x269), g_feCardSlot + 1);   /* MEMORY CARD IN MEMORY CARD SLOT %d */
    setText(id, buf);
}

static void cardYesNo(void)
{
    uint32_t yes = g_feCardIds[7], no = g_feCardIds[6];

    textPal(no, 0);
    textPal(yes, 0);
    tint(no, 0xC0, 0xC0, 0xC0, 0xC0);
    tint(yes, 0xC0, 0xC0, 0xC0, 0xC0);
    if (g_feCardYes == 0) {
        textPal(no, 1);
        tint(no, 0xC0, 0xC0, 0x80, (int)g_guiPulse + 0x20);
    } else {
        textPal(yes, 1);
        tint(yes, 0xC0, 0xC0, 0x80, (int)g_guiPulse + 0x20);
    }
}

static int frontendCardHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* VA 0x0043d320, 303 bytes, [I] row, status draft */
void frontendCardOpen(int16_t isSave, uint16_t slot, const uint32_t ids[8], uint16_t unk, GuiTask done)
{
    char buf[128];
    int i;

    g_feCardSlot = slot;
    g_feCardLoaded = 0;
    g_feCardSaved = 0;
    g_feCardDone = done;
    g_feCardUnk = unk;
    for (i = 0; i < 8; i++)
        g_feCardIds[i] = ids[i];
    for (i = 0; i < 8; i++) {
        setText(g_feCardIds[i], EMPTY);
        showId(g_feCardIds[i]);
    }
    setText(g_feCardIds[0], zTextGet(0x26E));                /* CHECKING... */
    zstringPrintf(buf, zTextGet(0x269), g_feCardSlot + 1);
    setText(g_feCardIds[1], buf);
    guiSetScreenHandler(frontendCardHandler);
    g_feTask = NULL;
    g_feCardDelay = 4;
    g_feCardState = isSave ? 0 : 0x100;
}

/* VA 0x0043d4a0, 1169 bytes, [I] row, status draft */
static int frontendCardHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_FRAME:
        zprimSetAttr(ZPRIM_ATTR_COLOUR, 0x10101);
        zprimDrawRect(0x28, 0, 0x1DB, 0xE8);
        if (g_feCardDelay >= 0) {
            g_feCardDelay--;
            break;
        }
        switch (g_feCardState) {
        case 0:     g_feTask = frontendCardCheckSave; break;
        case 1:     g_feTask = frontendCardAskFormat; break;
        case 3:     g_feTask = frontendCardAskSave; break;
        case 4:     g_feTask = frontendCardAskOverwrite; break;
        case 5:     g_feTask = frontendCardFormat; break;
        case 6:     g_feTask = frontendCardSave; break;
        case 7:
        case 8:
        case 10:
        case 0x102:
        case 0x103: g_feTask = frontendCardShowContinue; break;
        case 0xB:   g_feTask = frontendCardClose; break;
        case 0x100: g_feTask = frontendCardCheckLoad; break;
        case 0x101: g_feTask = frontendCardLoad; break;
        }
        if (g_feTask != NULL)
            g_feTask();
        break;
    case GUI_MSG_UP:
    case GUI_MSG_DOWN:
    case GUI_MSG_L1:
    case GUI_MSG_L2:
    case P2(GUI_MSG_UP):
    case P2(GUI_MSG_DOWN):
    case P2(GUI_MSG_L1):
    case P2(GUI_MSG_L2):
        return 1;
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_LEFT):
    case P2(GUI_MSG_RIGHT):
        if (g_feCardState == 1 || g_feCardState == 3 || g_feCardState == 4) {
            g_feCardYes ^= 1;
            SOUND(SND_MOVE);
        }
        return 0;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feCardState >= 0x106)
            return 0;
        if (g_feCardState < 0x102) {
            switch (g_feCardState) {
            case 1:                                      /* format it? yes */
                if (g_feCardYes) {
                    cardClear(0);
                    setText(g_feCardIds[0], zTextGet(0x26F));   /* FORMATTING... */
                    setText(g_feCardIds[1], zTextGet(0x25C));   /* DO NOT REMOVE */
                    cardSlotLine(g_feCardIds[2]);
                    g_feCardState = 5;
                    g_feCardDelay = 3;
                    return 0;
                }
                break;
            case 3:                                      /* save game? / overwrite? yes */
            case 4:
                if (g_feCardYes) {
                    cardClear(0);
                    setText(g_feCardIds[0], zTextGet(0x265));   /* SAVING... */
                    setText(g_feCardIds[1], zTextGet(0x25C));
                    cardSlotLine(g_feCardIds[2]);
                    g_feCardState = 6;
                    g_feCardDelay = 3;
                    return 0;
                }
                break;
            case 7:
            case 8:
            case 10:
                break;
            default:
                return 0;
            }
        }
        g_feCardState = 0xB;
        g_feCardDelay = 3;
        return 0;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (!g_guiAutoBack) {
            if (g_feCardState < 7) {
                if (g_feCardState > 4 || g_feCardState == 0)
                    return 0;                            /* busy */
            } else if (g_feCardState > 0xFF && g_feCardState < 0x102) {
                return 0;
            }
        }
        g_feCardDelay = 3;
        g_feCardState = 0xB;
        return 1;
    }
    return guiDefaultScreenHandler(msg, e, a, b);
}

/* VA 0x0043db20, 72 bytes, [I] row, status draft */
void frontendCardClose(void)
{
    int i;

    SOUND(SND_SELECT);
    for (i = 0; i < 8; i++)
        hideId(g_feCardIds[i]);
    g_feCardDone();
    g_feCardState = 0xC;
}

/* VA 0x0043db90, 280 bytes, [I] row, status draft */
void frontendCardCheckLoad(void)
{
    int r;

    zcardStart();
    zcardSelectPort((uint8_t)g_feCardSlot);
    r = memcard_CheckSaveExists();
    frontendCardShowLoadResult(r);
    g_feCardDelay = 3;
    if (r != 6) {
        g_feCardState = 0x103;
        zcardStop();
        return;
    }
    cardClear(0);
    setText(g_feCardIds[0], zTextGet(0x25D));                /* LOADING... */
    setText(g_feCardIds[1], zTextGet(0x25C));
    cardSlotLine(g_feCardIds[2]);
    g_feCardState = 0x101;
    zcardStop();
}

/* VA 0x0043dd00, 199 bytes, [I] row, status draft */
void frontendCardLoad(void)
{
    int r;

    SOUND(SND_CARD_BEGIN);
    zcardStart();
    zcardSelectPort((uint8_t)g_feCardSlot);
    r = memcard_LoadSave();
    g_feCardState = (uint16_t)((r != 0) + 0x102);
    zcardStop();
    frontendLoadMacros(g_saveBlockA.language);
    setVolumes();
    SOUND(SND_CARD_END);
    frontendCardShowLoadResult(r);
    g_feTask = NULL;
}

/* VA 0x0043de10, 389 bytes, [I] row, status draft */
void frontendCardAskFormat(void)
{
    setText(g_feCardIds[5], zTextGet(0x26D));                /* DO YOU WANT TO FORMAT IT? */
    cardYesNo();
    setText(g_feCardIds[6], zTextGet(0x25B));                /* NO */
    setText(g_feCardIds[7], zTextGet(0x25A));                /* YES */
}

/* VA 0x0043e010, 695 bytes, [I] row, status draft */
void frontendCardFormat(void)
{
    int r;

    cardClear(0);
    setText(g_feCardIds[0], zTextGet(0x26F));
    setText(g_feCardIds[1], zTextGet(0x25C));
    cardSlotLine(g_feCardIds[2]);
    zcardStart();
    SOUND(SND_CARD_BEGIN);
    zcardSelectPort((uint8_t)g_feCardSlot);
    r = memcard_CheckSaveExists();
    switch (r) {
    case 0:
    case 6:
        g_feCardState = 4;
        g_feCardDelay = 3;
        g_feCardYes = 0;
        setText(g_feCardIds[0], zTextGet(0x253));            /* SAVE GAME FOUND. */
        goto done;
    case 4:
        zcardSelectPort((uint8_t)g_feCardSlot);
        r = zcardFormat();
        if (r == 0) {
            g_feCardDelay = 3;
            g_feCardState = 6;
            setText(g_feCardIds[0], zTextGet(0x265));
            setText(g_feCardIds[1], zTextGet(0x25C));
            cardSlotLine(g_feCardIds[2]);
            goto done;
        }
        frontendCardShowSaveResult(r);
        setText(g_feCardIds[0], zTextGet(0x271));            /* FORMAT FAILED. */
        break;
    case 5:
        zcardSelectPort((uint8_t)g_feCardSlot);
        r = memcard_Stub0();
        if (r == 0) {
            cardClear(0);
            g_feCardYes = 0;
            g_feCardState = 3;
            g_feCardDelay = 3;
            goto done;
        }
        /* fall through */
    default:
        frontendCardShowSaveResult(r);
        break;
    }
    g_feCardState = 10;
    g_feCardDelay = 3;
done:
    zcardStop();
    SOUND(SND_CARD_END);
    g_feTask = NULL;
}

/* VA 0x0043e3b0, 112 bytes, [I] row, status draft */
void frontendCardShowContinue(void)
{
    setText(g_feCardIds[6], zTextGet(0x258));                /* CONTINUE */
    textPal(g_feCardIds[6], 1);
    tint(g_feCardIds[6], 0xC0, 0xC0, 0x80, (int)g_guiPulse + 0x20);
}

/* VA 0x0043e440, 447 bytes, [I] row, status draft */
void frontendCardAskSave(void)
{
    cardClear(1);
    setText(g_feCardIds[0], zTextGet(0x254));                /* SAVE GAME? */
    setText(g_feCardIds[6], zTextGet(0x25B));
    setText(g_feCardIds[7], zTextGet(0x25A));
    cardYesNo();
}

/* VA 0x0043e670, 447 bytes, [I] row, status draft */
void frontendCardAskOverwrite(void)
{
    cardClear(1);
    setText(g_feCardIds[5], zTextGet(0x268));                /* OVERWRITE THE SAVED GAME? */
    setText(g_feCardIds[6], zTextGet(0x25B));
    setText(g_feCardIds[7], zTextGet(0x25A));
    cardYesNo();
}

/* VA 0x0043e8a0, 259 bytes, [I] row, status draft */
void frontendCardCheckSave(void)
{
    int r;

    zcardStart();
    zcardSelectPort((uint8_t)g_feCardSlot);
    r = memcard_CheckSaveExists();
    frontendCardShowSaveResult(r);
    switch (r) {
    case 0:
    case 5:
        zcardSelectPort((uint8_t)g_feCardSlot);
        r = memcard_Stub0();
        if (r == 0) {
            cardClear(0);
            g_feCardState = 3;
            g_feCardYes = 0;
            break;
        }
        frontendCardShowSaveResult(r);
        /* fall through */
    default:
        g_feCardState = 8;
        break;
    case 4:
        g_feCardState = 1;
        g_feCardYes = 0;
        break;
    case 6:
        setText(g_feCardIds[0], zTextGet(0x253));
        g_feCardState = 4;
        g_feCardYes = 0;
        break;
    }
    zcardStop();
    g_feTask = NULL;
    g_feCardDelay = 3;
}

/* VA 0x0043ea10, 234 bytes, [I] row, status draft */
void frontendCardSave(void)
{
    int r;

    SOUND(SND_CARD_BEGIN);
    zcardStart();
    zcardStart();
    zcardSelectPort((uint8_t)g_feCardSlot);
    r = memcard_CheckSaveExists();
    switch (r) {
    case 4:
        frontendCardShowSaveResult(4);
        g_feCardState = 1;
        g_feCardYes = 0;
        break;
    case 5:
        zcardSelectPort((uint8_t)g_feCardSlot);
        r = memcard_Stub0();
        if (r != 0) {
            frontendCardShowSaveResult(r);
            g_feCardState = 8;
            break;
        }
        /* fall through */
    case 0:
    case 6:
        zcardSelectPort((uint8_t)g_feCardSlot);
        r = memcard_WriteSave();
        frontendCardShowSaveResult(r);
        g_feCardState = (uint16_t)((r != 0) + 7);
        break;
    default:
        frontendCardShowSaveResult(r);
        g_feCardState = 8;
        break;
    }
    g_feCardDelay = 3;
    zcardStop();
    SOUND(SND_CARD_END);
    g_feTask = NULL;
}

/* VA 0x0043eb70, 470 bytes, [I] row, status draft */
void frontendCardShowLoadResult(int r)
{
    cardClear(0);
    setText(g_feCardIds[0], zTextGet(r == 0 ? 0x25E : 0x261));   /* LOAD COMPLETE. / LOAD FAILED. */
    switch (r) {
    case 0:
        g_feCardLoaded = 1;
        return;
    case 1:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x263));            /* NOT PRESENT. */
        return;
    case 2:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x264));            /* IS INVALID. */
        return;
    case 4:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x26C));            /* IS NOT FORMATTED. */
        return;
    case 5:
        setText(g_feCardIds[1], zTextGet(0x262));            /* NO SAVED GAME FOUND. */
        return;
    case -1:
        setText(g_feCardIds[1], zTextGet(0x272));            /* FILE MAY BE CORRUPTED. */
        return;
    }
}

/* VA 0x0043edf0, 544 bytes, [I] row, status draft */
void frontendCardShowSaveResult(int r)
{
    cardClear(0);
    setText(g_feCardIds[0], zTextGet(r == 0 ? 0x266 : 0x267));   /* SAVE COMPLETE. / SAVE FAILED. */
    switch (r) {
    case 0:
        g_feCardSaved = 1;
        return;
    case 1:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x263));
        return;
    case 2:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x264));
        return;
    case 4:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x26C));
        return;
    case 7:
        cardSlotLine(g_feCardIds[1]);
        setText(g_feCardIds[2], zTextGet(0x26A));            /* DOES NOT HAVE ENOUGH FREE BLOCKS. */
        setText(g_feCardIds[3], zTextGet(0x26B));            /* 2 FREE MEMORY BLOCKS NEEDED TO SAVE. */
        return;
    }
}

/* ================================================================== autoload (0xA005, PSX only) */

/* VA 0x0043f0c0, 1154 bytes, [I] row, status draft */
static int frontendAutoloadHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* "CHECKING FOR SAVED GAME..." over the title: slot 0, then slot 1; load at step 8, then
     * the main menu at step 120. [V] unreachable on PC (g_feAutoload 0x0056B618 is never set). */
    char buf[100];
    const char *t;
    uint32_t id;
    int r;

    if (msg != GUI_MSG_FRAME)
        return guiDefaultScreenHandler(msg, e, a, b);
    zprimSetAttr(ZPRIM_ATTR_COLOUR, 0x10101);
    zprimDrawRect(0x3C, 0x104, 0x1C7, 0x168);
    switch (g_feBootStep) {
    case 0:
        showId(MAIN(2));
        showId(MAIN(3));
        setText(MAIN(2), zTextGet(0x259));                   /* CHECKING FOR SAVED GAME... */
        setText(MAIN(3), EMPTY);
        setText(MAIN(4), EMPTY);
        setText(MAIN(5), EMPTY);
        break;
    case 4:
        zcardStart();
        zcardSelectPort(0);
        r = memcard_CheckSaveExists();
        if (r == 6) {                                        /* found in slot 1 */
            g_feBootSlot = 0;
            zstringPrintf(buf, "%s %s", zTextGet(0x25D), zTextGet(0x25C));   /* LOADING... DO NOT REMOVE */
            setText(MAIN(2), buf);
            zstringPrintf(buf, zTextGet(0x269), g_feBootSlot + 1);
            setText(MAIN(3), buf);
            setText(MAIN(4), EMPTY);
            setText(MAIN(5), EMPTY);
            zcardStop();
            break;
        }
        if (r == 5)
            memcard_Stub0();
        zcardSelectPort(1);                                     /* try slot 2 */
        r = memcard_CheckSaveExists();
        if (r == 5) {
            memcard_Stub0();
            setText(MAIN(2), zTextGet(0x262));               /* NO SAVED GAME FOUND. */
            t = EMPTY;
        } else if (r == 6) {
            g_feBootSlot = 1;
            zstringPrintf(buf, "%s %s", zTextGet(0x25D), zTextGet(0x25C));
            setText(MAIN(2), buf);
            zstringPrintf(buf, zTextGet(0x269), g_feBootSlot + 1);
            setText(MAIN(3), buf);
            zcardStop();
            break;
        } else {
            setText(MAIN(2), zTextGet(0x262));
            t = zTextGet(0x260);                             /* NO MEMORY CARD FOUND. */
        }
        setText(MAIN(3), t);
        g_feBootStep = 8;                                    /* skip the load */
        zcardStop();
        break;
    case 8:
        zcardStart();
        zcardSelectPort(g_feBootSlot);
        r = memcard_LoadSave();
        if (r == 0) {
            frontendLoadMacros(g_saveBlockA.language);
            g_feBootLoaded = 1;
            setText(MAIN(2), zTextGet(0x25E));               /* LOAD COMPLETE. */
            t = EMPTY;
            id = MAIN(3);
        } else {
            if (r == -1)
                setText(MAIN(3), zTextGet(0x272));
            t = zTextGet(0x261);
            id = MAIN(2);
        }
        setText(id, t);
        setText(MAIN(4), EMPTY);
        setText(MAIN(5), EMPTY);
        zcardStop();
        break;
    case 120:
        frontendMainMenuSetHandler();
        setVolumes();
        SOUND(SND_CARD_END);
        break;
    }
    g_feBootStep++;
    return guiDefaultScreenHandler(msg, e, a, b);
}

/* ================================================================== title / main menu (0xA005) */

static int frontendTitleHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendMainMenuHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendDemoHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* VA 0x0043f730, 71 bytes, [I] row, status draft */
void frontendAutoloadEnter(void)
{
    guiDestroyScreen();
    guiLoadBackdropImage(0xA007, 1);
    guiLoadScreen(SCR_MAIN, frontendAutoloadHandler);
    g_feMainNext = NULL;
    g_feTitleFrames = 0;
    g_feMainDelay = 0;
    g_feTitleTimer = 0;
    g_feMainSel = 0;
    g_feAutoload = 0;
}

/* VA 0x0043f7a0, 405 bytes, [I] row, status draft */
static int frontendTitleHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* "PRESS START BUTTON" over the title; 30 s idle starts the demo. [V] unreachable on PC
     * (frontendTitleEnter is not referenced: the PC build opens the main menu directly). */
    int8_t c;

    if (msg == GUI_MSG_FRAME) {
        if (++g_feTitleTimer > 1800) {
            g_feMainDelay = 0;
            g_feTitleTimer = 0;
            g_feMainNext = (GuiTask)frontendStartDemo;
        }
        if (g_feTitleShown == 0) {
            showId(MAIN(1));
            setText(MAIN(1), zTextGet(0x23));                    /* PRESS START BUTTON */
            textPal(MAIN(1), 1);
            guiSetTextFlags(guiFindElement(MAIN(1)), GUIT_CENTRE_X | GUIT_HIGHLIGHT | GUIT_MODE3);
            g_feTitleShown = 1;
        }
        if (g_feMainNext == NULL) {
            c = (int8_t)(g_guiPulse / 2 + 0x7F);                 /* signed char, as the exe */
            tint(MAIN(1), (uint8_t)c, (uint8_t)c, (uint8_t)c, c);
        } else if (g_feMainDelay++ > 2) {
            hideId(MAIN(1));
            if (g_feMainNext == (GuiTask)frontendMainMenuEnter) {
                frontendMainMenuEnter(1);
                g_feMainNext = NULL;
            } else {
                g_feMainNext();
                g_feMainNext = NULL;
            }
        }
        g_feTitleFrames++;
    } else if (msg == GUI_MSG_START || msg == P2(GUI_MSG_START)) {
        g_feMainNext = (GuiTask)frontendMainMenuEnter;
        g_feTitleTimer = 0;
        g_feMainDelay = 0;
    }
    return guiDefaultScreenHandler(msg, e, a, b);
}

/* VA 0x0043f9b0, 77 bytes, [I] row, status draft */
void frontendTitleEnter(void)
{
    guiDestroyScreen();
    guiLoadBackdropImage(0xA007, 1);
    guiLoadScreen(SCR_MAIN, frontendTitleHandler);
    g_feMainNext = NULL;
    g_feTitleFrames = 0;
    g_feMainDelay = 0;
    g_feTitleTimer = 0;
    g_feMainSel = 0;
    g_feTitleShown = 0;
    g_feAutoload = 0;
}

/* VA 0x0043fa20, 188 bytes, [I] row, status draft */
int frontendMainMenuEnter(int again)
{
    /* again == 0: the first entry (frontendInitApp): SAVE.DAT is loaded here */
    guiDestroyScreen();
    guiLoadBackdropImage(0xA007, 1);
    guiLoadScreen(SCR_MAIN, frontendMainMenuHandler);
    g_feMainNext = NULL;
    g_appQuit = 0;
    g_feTitleFrames = 0;
    g_feMainDelay = 0;
    g_feTitleTimer = 0;
    frontendMainMenuSetHandler();
    if (!again) {
        g_feMainSel = 0;
        memcard_LoadSave();
        setVolumes();
        SOUND(SND_CARD_END);
        return 0;
    }
    g_guiGroupLoaded = 0;
    return 0;
}

/* VA 0x0043fb10, 24 bytes, [I] row, status draft */
void frontendMainMenuSetHandler(void)
{
    guiSetScreenHandler(frontendMainMenuHandler);
    g_feTask = frontendMainMenuRefresh;
}

/* VA 0x0043fb40, 47 bytes, [I] row, status draft */
void frontendMainMenuReturn(void)
{
    guiLoadScreen(SCR_MAIN, frontendMainMenuHandler);
    g_feMainNext = NULL;
    g_feTitleFrames = 0;
    g_feMainDelay = 0;
    g_feTask = frontendMainMenuRefresh;
}

/* VA 0x0043fb80, 942 bytes, [I] row, status draft */
static int frontendMainMenuHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* Seven rows MAIN(10..16) show items sel-3 .. sel+3 of the six (wrapping); up / down scroll
     * them (frontendMainMenuPrev / Next), accept runs the item 3 frames later. 30 s idle: demo. */
    int i;

    switch (msg) {
    case GUI_MSG_FRAME:
        if (++g_feTitleTimer > 1800) {
            g_feMainDelay = 0;
            g_feTitleTimer = 0;
            g_feMainNext = (GuiTask)frontendStartDemo;
        }
        g_feMainRowAlpha[3] = (int16_t)g_guiPulse;            /* the middle row glows */
        if (g_feTask == NULL) {
            if (g_feMainNext == NULL) {
                tint(MAIN(13), 0xFF, 0xFF, 0x96, (int)g_guiPulse);
                g_feTitleFrames++;
                break;
            }
        } else {
            g_feTask();
            if (g_feMainNext == NULL) {
                g_feTitleFrames++;
                break;
            }
        }
        if (g_feMainDelay++ > 2) {
            g_feMainNext();
            g_feMainNext = NULL;
        }
        g_feTitleFrames++;
        break;
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        g_feTitleTimer = 0;
        if (g_feTask == NULL) {
            g_feScroll = 0;
            g_feTask = frontendMainMenuPrev;
        }
        return 1;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        g_feTitleTimer = 0;
        if (g_feTask == NULL) {
            g_feScroll = 0;
            g_feTask = frontendMainMenuNext;
        }
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        g_feTitleTimer = 0;
        if (g_feTask != NULL)
            return 1;
        switch (g_feMainSel) {
        case 0:                                     /* PROQUEST */
        case 1:                                     /* SESSION */
        case 2:                                     /* FREERIDE */
            if (msg == P2(GUI_MSG_ACCEPT) || msg == P2(GUI_MSG_START)) {
                SOUND(SND_ERROR);
                break;
            }
            g_numPlayers = 1;
            SOUND(SND_SELECT);
            g_feMainDelay = 0;
            g_feMainNext = (GuiTask)frontendSelectEnter;
            g_selectedSubMode = (uint8_t)g_feMainSel;
            break;
        case 3:                                     /* COOL STUFF */
            SOUND(SND_SELECT);
            g_feMainDelay = 0;
            g_feMainNext = (GuiTask)frontendCoolStuffEnter;
            break;
        case 4:                                     /* OPTIONS */
            SOUND(SND_SELECT);
            g_feMainDelay = 0;
            g_feMainNext = (GuiTask)frontendOptionsEnter;
            break;
        case 5:                                     /* QUIT */
            if (zrwZglConfirmQuit()) {
                frontendRequestExit(1);
                SOUND(SND_SELECT);
            }
            break;
        default:
            SOUND(SND_ERROR);
            break;
        }
        if (g_feMainNext != NULL) {
            hideId(MAIN(10));
            hideId(MAIN(11));
            hideId(MAIN(12));
            tint(MAIN(13), 0xFF, 0xFF, 0x96, 0x80);
            hideId(MAIN(14));
            hideId(MAIN(15));
            hideId(MAIN(16));
            for (i = 6; i <= 9; i++)
                hideId(MAIN(i));
        }
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        g_feTitleTimer = 0;
        return 0;
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case GUI_MSG_CIRCLE:
    case GUI_MSG_L2:
    case P2(GUI_MSG_LEFT):
    case P2(GUI_MSG_RIGHT):
    case P2(GUI_MSG_CIRCLE):
    case P2(GUI_MSG_L2):
        g_feTitleTimer = 0;
        return 1;
    }
    return guiDefaultScreenHandler(msg, e, a, b);
}

/* VA 0x004400a0, 431 bytes, [I] row, status draft */
void frontendMainMenuNext(void)
{
    /* rows 1..6 move up to rows 0..5 over 10 frames (6 units of 60 a frame) */
    int k, x, y, grey, alpha;

    g_feScroll += 6;
    for (k = 0; k < 6; k++) {
        x = g_feMainRowPos[k + 1][0] - (g_feMainRowPos[k + 1][0] - g_feMainRowPos[k][0]) * (int)g_feScroll / 60;
        y = g_feMainRowPos[k + 1][1] - (g_feMainRowPos[k + 1][1] - g_feMainRowPos[k][1]) * (int)g_feScroll / 60;
        grey = g_feMainRowGrey[k + 1] - (g_feMainRowGrey[k + 1] - g_feMainRowGrey[k]) * (int)g_feScroll / 60;
        alpha = g_feMainRowAlpha[k + 1] - (g_feMainRowAlpha[k + 1] - g_feMainRowAlpha[k]) * (int)g_feScroll / 60;
        textPos(MAIN(11 + k), x, y - 0x8C);
        tint(MAIN(11 + k), (uint8_t)grey, (uint8_t)grey, (uint8_t)grey, (int16_t)alpha);
    }
    if (g_feScroll > 59) {
        g_feMainSel = (uint16_t)frontendWrap(g_feMainSel + 1, 6);
        frontendMainMenuRefresh();
        g_feTask = NULL;
        SOUND(SND_MOVE);
    }
}

/* VA 0x004402c0, 428 bytes, [I] row, status draft */
void frontendMainMenuPrev(void)
{
    /* rows 0..5 move down to rows 1..6 */
    int k, x, y, grey, alpha;

    g_feScroll += 6;
    for (k = 0; k < 6; k++) {
        x = g_feMainRowPos[k][0] + (g_feMainRowPos[k + 1][0] - g_feMainRowPos[k][0]) * (int)g_feScroll / 60;
        y = g_feMainRowPos[k][1] + (g_feMainRowPos[k + 1][1] - g_feMainRowPos[k][1]) * (int)g_feScroll / 60;
        grey = g_feMainRowGrey[k] + (g_feMainRowGrey[k + 1] - g_feMainRowGrey[k]) * (int)g_feScroll / 60;
        alpha = g_feMainRowAlpha[k] + (g_feMainRowAlpha[k + 1] - g_feMainRowAlpha[k]) * (int)g_feScroll / 60;
        textPos(MAIN(10 + k), x, y - 0x8C);
        tint(MAIN(10 + k), (uint8_t)grey, (uint8_t)grey, (uint8_t)grey, (int16_t)alpha);
    }
    if (g_feScroll > 59) {
        g_feMainSel = (uint16_t)frontendWrap(g_feMainSel - 1, 6);
        frontendMainMenuRefresh();
        SOUND(SND_MOVE);
    }
}

/* VA 0x004404e0, 1681 bytes, [I] row, status draft */
void frontendMainMenuRefresh(void)
{
    int k;

    hideId(MAIN(2));
    hideId(MAIN(3));
    for (k = 0; k < 7; k++)
        showId(MAIN(10 + k));
    for (k = 0; k < 7; k++)
        setText(MAIN(10 + k), zTextGet(g_feMainItems[frontendWrap(g_feMainSel + k - 3, 6)]));
    for (k = 0; k < 7; k++)
        textPos(MAIN(10 + k), g_feMainRowPos[k][0], g_feMainRowPos[k][1] - 0x8C);
    for (k = 0; k < 7; k++)
        textPal(MAIN(10 + k), 1);
    for (k = 0; k < 7; k++)
        guiSetFlags(guiFindElement(MAIN(10 + k)), GUIF_HIGHLIGHT);
    for (k = 0; k < 7; k++)
        guiSetTextFlags(guiFindElement(MAIN(10 + k)), GUIT_MODE3 | GUIT_HIGHLIGHT);
    for (k = 0; k < 7; k++)
        tint(MAIN(10 + k), (uint8_t)g_feMainRowGrey[k], (uint8_t)g_feMainRowGrey[k], (uint8_t)g_feMainRowGrey[k],
             g_feMainRowAlpha[k]);
    g_feTask = NULL;
}

/* VA 0x00440d30, 53 bytes, [I] row, status draft */
int frontendStartDemo(void)
{
    g_numPlayers = 0;                   /* actionLoad: 0 players = the demo replays */
    guiDestroyScreen();
    guiLoadScreen(0, frontendDemoHandler);
    g_feTask = NULL;
    g_feMainDelay = 0;
    return 1;
}

/* VA 0x00440d80, 41 bytes, [I] row, status draft */
static int frontendDemoHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    if (msg == GUI_MSG_FRAME && g_feMainDelay++ > 4)
        frontendRequestExit(0);
    return 1;
}

/* ================================================================== options: cheats */

typedef struct FeCheat { uint16_t text; uint8_t bit; uint8_t pad; } FeCheat;
extern const FeCheat g_cheatTable[8];          /* 0x0055A3BC FIRST PERSON CAM (bit 0) .. STICKY CRASH MODE (7) */
const uint32_t g_feOnOff[2] = { 0x13B, 0x13A };  /* 0x0055A3B0 OFF, ON */
int32_t        g_feCheatX[8];                  /* 0x00691E40 slide-in of each line */
uint16_t       g_feCheatCount;                 /* 0x00691E10 */
uint32_t       g_feCheatFrames;                /* 0x00691E1C */
const FeCheat *g_feCheatList[8];               /* 0x00691E80 the unlocked cheats */
int32_t        g_feCheatSel;                   /* 0x00691EA0 */

static int frontendCheatsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendOptionsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* VA 0x00440dc0, 192 bytes, [I] row, status draft */
void frontendCheatsEnter(void)
{
    /* CHEATS lists only the cheats whose bit is set in cheats_unlocked (save +0x08); left / right
     * toggles the bit in cheats_active (save +0x0A) */
    uint16_t unlocked = g_saveBlockA.cheats_unlocked;
    int i, n = 0;

    memset(g_feCheatX, 0, sizeof(g_feCheatX));
    for (i = 0; i < 8; i++)
        if (unlocked & (1u << g_cheatTable[i].bit))
            g_feCheatList[n++] = &g_cheatTable[i];
    g_feCheatCount = (uint16_t)n;
    g_feCheatSel = 0;
    showId(OPT(0x15));
    guiSetScreenHandler(frontendCheatsHandler);
    guiFindElement(OPT(0x15))->handler = frontendCheatsHandler;   /* draws the list */
    g_feTask = frontendCheatsSlideIn;
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
}

/* VA 0x00440eb0, 221 bytes, [I] row, status draft */
void frontendCheatsLeave(void)
{
    hideId(OPT(0x0E));
    hideId(OPT(0x0D));
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    hideId(OPT(0x12));
    hideId(OPT(0x11));
    hideId(OPT(0x14));
    hideId(OPT(0x13));
    frontendTabsReset();
    frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB));
    hideId(OPT(0x15));
    guiSetScreenHandler(frontendOptionsHandler);
    guiFindElement(OPT(0x15))->handler = guiElementHandler;
    g_feTask = frontendOptionsTabsIn;
}

/* VA 0x00440fd0, 168 bytes, [I] row, status draft */
void frontendCheatsSlideIn(void)
{
    int i;

    g_feCheatX[0] += (500 - g_feCheatX[0]) / 5;
    if (g_feCheatX[0] > 0x1CC)
        g_feCheatX[0] = 0x1CC;
    for (i = 1; i < g_feCheatCount; i++) {
        if (g_feCheatX[i - 1] > 0x104)
            g_feCheatX[i] += (500 - g_feCheatX[i]) / 5;
        if (g_feCheatX[i] > 0x1CC)
            g_feCheatX[i] = 0x1CC;
    }
    if (g_feCheatX[g_feCheatCount - 1] > 0x1CB)
        g_feTask = NULL;
}

/* VA 0x004410b0, 154 bytes, [I] row, status draft */
void frontendCheatsSlideOut(void)
{
    int i;

    g_feCheatX[0] += (500 - g_feCheatX[0]) / -5;
    if (g_feCheatX[0] < 0)
        g_feCheatX[0] = 0;
    for (i = 1; i < g_feCheatCount; i++) {
        if (g_feCheatX[i - 1] < 400)
            g_feCheatX[i] -= (500 - g_feCheatX[i]) / 5;
        if (g_feCheatX[i] < 0)
            g_feCheatX[i] = 0;
    }
    if (g_feCheatX[g_feCheatCount - 1] < 1)
        frontendCheatsLeave();              /* inlined in the exe */
}

/* VA 0x00441180, 586 bytes, [I] row, status draft */
static int frontendCheatsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int i;

    switch (msg) {
    case GUI_MSG_DRAW_ELEM:                     /* OPT(0x15): name and ON / OFF per line */
        for (i = 0; i < g_feCheatCount; i++) {
            guiSetText(e, zTextGet(g_feCheatList[i]->text));
            guiSetTextPos(e, (int16_t)(0x200 - g_feCheatX[i]), (int16_t)(i * 0x23));
            guiSetTextPalette(e, g_feCheatSel == i);
            guiElementHandler(msg, e, a, b);
            guiSetText(e, zTextGet(g_feOnOff[(g_saveBlockA.cheats_active >> g_feCheatList[i]->bit) & 1]));
            guiSetTextPos(e, (int16_t)(0x336 - g_feCheatX[i]), (int16_t)(i * 0x23));
            guiSetTextPalette(e, (g_feCheatSel != i) + 1);
            guiElementHandler(msg, e, a, b);
        }
        return 0;
    case GUI_MSG_FRAME:
        g_feCheatFrames++;
        if (g_feTask != NULL)
            g_feTask();
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (g_feTask != NULL)
            return 1;
        if (--g_feCheatSel < 0)
            g_feCheatSel = g_feCheatCount - 1;
        break;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if (g_feTask != NULL)
            return 1;
        if (++g_feCheatSel >= (int)g_feCheatCount)
            g_feCheatSel = 0;
        break;
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_LEFT):
    case P2(GUI_MSG_RIGHT):
        if (g_feTask != NULL)
            return 1;
        g_saveBlockA.cheats_active ^= (uint16_t)(1u << g_feCheatList[g_feCheatSel]->bit);
        break;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        if (!g_guiAutoBack) {
            SOUND(SND_SELECT);
            g_feTask = frontendCheatsSlideOut;
            return 1;
        }
        g_feTask = frontendCheatsLeave;
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    SOUND(SND_MOVE);
    return 1;
}

/* ================================================================== options: color adjust (unused) */

const uint8_t g_feColorBars[8][3] = {       /* 0x0055A3DC SMPTE bars, 0..25 */
    { 25, 25, 25 }, { 25, 25, 0 }, { 0, 25, 25 }, { 0, 25, 0 }, { 25, 0, 25 }, { 25, 0, 0 }, { 0, 0, 25 }, { 0, 0, 0 }
};

static int frontendColorAdjustHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* one PSX POLY_G4 (0x90 bytes here): top two vertices 'top', bottom two 'bottom' */
static uint32_t *barQuad(uint32_t *p, int x0, int x1, int y0, int y1, const uint8_t top[3], const uint8_t bottom[3])
{
    uint8_t *q = (uint8_t *)p;
    uint32_t *ot;
    int k;

    p[0] = (p[0] & 0x0FFFFFFF) | 0x80000000;            /* 8 words */
    q[7] = 0x38;
    p[10] = (p[10] & 0xFFFFFF80) | 0x7F80;
    for (k = 0; k < 3; k++) {
        q[4 + k] = top[k];
        q[0xC + k] = top[k];
        q[0x14 + k] = bottom[k];
        q[0x1C + k] = bottom[k];
    }
    *(int16_t *)(q + 0x08) = (int16_t)x0; *(int16_t *)(q + 0x0A) = (int16_t)y0;
    *(int16_t *)(q + 0x10) = (int16_t)x1; *(int16_t *)(q + 0x12) = (int16_t)y0;
    *(int16_t *)(q + 0x18) = (int16_t)x0; *(int16_t *)(q + 0x1A) = (int16_t)y1;
    *(int16_t *)(q + 0x20) = (int16_t)x1; *(int16_t *)(q + 0x22) = (int16_t)y1;
    ot = psx2DGetDepth();
    p[0] = ((p[0] ^ *ot) & 0x0FFFFFFF) ^ p[0];
    ot = psx2DGetDepth();
    *ot = ((*ot ^ (uint32_t)p) & 0x0FFFFFFF) ^ *ot;
    return p + 0x24;
}

/* VA 0x004414d0, 715 bytes, [I] row, status draft */
void frontendColorAdjustDraw(void)
{
    /* 8 colour bars (x = 640 * 0.125 * i), bright (6x) at the top 300 lines, dark below */
    static const uint8_t black[3] = { 0, 0, 0 };
    uint32_t *p = g_psxPrimNext;
    uint8_t bright[3];
    int i, k, x0, x1;

    for (i = 0; i < 8; i++) {
        for (k = 0; k < 3; k++)
            bright[k] = (uint8_t)(g_feColorBars[i][k] * 6);
        x0 = (int)(640.0f * 0.125f * (float)i);
        x1 = (int)(640.0f * 0.125f * (float)(i + 1));
        p = barQuad(p, x0, x1, 0, 300, bright, g_feColorBars[i]);
        p = barQuad(p, x0, x1, 300, 0x1FF, g_feColorBars[i], black);
    }
    g_psxPrimNext = p;
    psx2DSetDepth(psx2DGetDepth() - 1);
    zprimSetAttr(ZPRIM_ATTR_COLOUR, 0xEEEEEE);
    zprimDrawLine(0, 0xF0, (int16_t)(640.0f * 0.125f * 8.0f), 0xF0);     /* "the white line" */
    zprimDrawLine(0, 0xF1, (int16_t)(640.0f * 0.125f * 8.0f), 0xF1);
}

/* VA 0x00441860, 316 bytes, [I] row, status draft */
void frontendColorAdjustEnter(void)
{
    /* [V] not referenced: COLOR ADJUST (text 0x134) is not one of the options */
    int i;

    for (i = 1; i <= 4; i++)
        showId(OPT(i));
    setText(OPT(1), zTextGet(0x135));           /* PLEASE ADJUST THE BRIGHTNESS */
    setText(OPT(2), zTextGet(0x136));           /* ON YOUR TV SET SO THE COLORS */
    setText(OPT(3), zTextGet(0x137));           /* BELOW THE WHITE LINE */
    setText(OPT(4), zTextGet(0x138));           /* ARE NOT VISIBLE. */
    hideId(OPT(0x0E));
    hideId(OPT(0x0D));
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    hideId(OPT(0x14));
    hideId(OPT(0x13));
    guiSetScreenHandler(frontendColorAdjustHandler);
    g_feTask = NULL;
}

/* VA 0x004419f0, 380 bytes, [I] row, status draft */
void frontendColorAdjustLeave(void)
{
    int i;

    for (i = 1; i <= 4; i++)
        hideId(OPT(i));
    for (i = 5; i <= 0xC; i++)
        showId(OPT(i));
    hideId(OPT(0x0E));
    hideId(OPT(0x0D));
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    hideId(OPT(0x12));
    hideId(OPT(0x11));
    hideId(OPT(0x14));
    hideId(OPT(0x13));
    frontendTabsReset();
    frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB));
    guiSetScreenHandler(frontendOptionsHandler);
    g_feTask = frontendOptionsTabsIn;
}

/* VA 0x00441bd0, 54 bytes, [I] row, status draft */
static int frontendColorAdjustHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        frontendColorAdjustDraw();
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case GUI_MSG_DOWN:
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_UP):
    case P2(GUI_MSG_DOWN):
    case P2(GUI_MSG_LEFT):
    case P2(GUI_MSG_RIGHT):
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        if (!g_guiAutoBack)
            SOUND(SND_SELECT);
        frontendColorAdjustLeave();
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
}

/* ================================================================== options: controller */
/*
 * 14 rows: 0-10 = Up, Down, Left, Right and the seven remappable buttons (label OPT(0x21 + row),
 * the layout's key OPT(0x2C + row), the bound pad button OPT(0x37 + row)), 11 = keyboard layout
 * (OPT(0x42)), 12 = accept, 13 = cancel. Accept copies the 11 bindings to the profile's +0x14 and
 * the layout to +0x4B (docs/re/input-camera.md 2.1).
 */
uint32_t g_feCtlBind[11];        /* 0x005745B4 */
uint8_t  g_feCtlRow;             /* 0x005745E0 */
uint8_t  g_feCtlMode;            /* 0x005745E1 0 browse, 1 wait for release, 2 wait for a button */
uint8_t  g_feCtlLayout;          /* 0x005745E2 */
int32_t  g_feOptPlayer;          /* 0x00691E08 profile being edited (pad-2 messages: 1) */

#define CTL_LABEL(r)  OPT(0x21 + (r))
#define CTL_KEY(r)    OPT(0x2C + (r))
#define CTL_BIND(r)   OPT(0x37 + (r))

static int frontendControllerHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

static uint32_t *profileBindings(int p)
{
    return (uint32_t *)((uint8_t *)&g_saveProfiles[p] + 0x14);
}

/* VA 0x00441cb0, 571 bytes, [I] row, status draft */
void frontendControllerEnter(void)
{
    int p = g_feOptPlayer;
    uint32_t r, bit;

    for (r = 0; r < 3; r++)
        showId(OPT(0x1E + r));
    g_feCtlMode = 0;
    g_feCtlRow = 13;
    for (r = 0; r < 14; r++) {
        if (r < 11) {
            showId(CTL_LABEL(r));
            showId(CTL_KEY(r));
            showId(CTL_BIND(r));
            textPal(CTL_LABEL(r), r == g_feCtlRow);
            textPal(CTL_KEY(r), r == g_feCtlRow);
            textPal(CTL_BIND(r), r == g_feCtlRow ? 2 : 0);
            g_feCtlBind[r] = profileBindings(p)[r];
            for (bit = 0; bit < 18; bit++) {
                if (g_feCtlBind[r] & (1u << bit)) {
                    setText(CTL_BIND(r), g_padButtonNames[bit]);
                    break;
                }
            }
            /* sic: the layout of the previous visit (set below) */
            setText(CTL_KEY(r), g_keyLayoutLabels[g_feCtlLayout][r]);
        } else {
            showId(CTL_BIND(r));
            textPal(CTL_BIND(r), r == g_feCtlRow ? 2 : 0);
        }
    }
    showId(OPT(0x45));
    showId(OPT(0x46));
    g_feCtlLayout = g_saveProfiles[p].unk_40[0x0B];         /* keyboard_layout (+0x4B) */
    setText(OPT(0x42), g_keyLayoutNames[g_feCtlLayout]);
    guiSetScreenHandler(frontendControllerHandler);
    g_feTask = NULL;
}

/* VA 0x00441f80, 220 bytes, [I] row, status draft */
void frontendControllerLeave(void)
{
    uint32_t r;

    for (r = 0; r < 3; r++)
        hideId(OPT(0x1E + r));
    for (r = 0; r < 14; r++) {
        if (r < 11) {
            hideId(CTL_LABEL(r));
            hideId(CTL_KEY(r));
        }
        hideId(CTL_BIND(r));
    }
    hideId(OPT(0x45));
    hideId(OPT(0x46));
    frontendTabsReset();
    frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB));
    guiSetScreenHandler(frontendOptionsHandler);
    g_feTask = frontendOptionsTabsIn;
}

/* VA 0x004420a0, 1296 bytes, [I] row, status draft */
static int frontendControllerHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int p = g_feOptPlayer;
    ZInputDevice *dev = (ZInputDevice *)zinputGetDevice(p);
    uint32_t r, bit, ev;
    int16_t y0, y1, x0, x1;

    if (g_feCtlMode != 0) {
        if (g_feCtlMode == 1) {
            if (msg == GUI_MSG_FRAME && dev->buttons == 0)
                g_feCtlMode = 2;                                 /* released: now wait */
        } else if (g_feCtlMode == 2 &&
                   (msg == GUI_MSG_FRAME || msg == GUI_MSG_BACK || msg == P2(GUI_MSG_BACK))) {
            for (r = 0; r < 4; r++) {                            /* a box around the row */
                y0 = (int16_t)(g_feCtlRow * 25 + 0x26);
                y1 = (int16_t)(r == 0 ? y0 : g_feCtlRow * 25 + 0x3E);
                if (r == 2)
                    y0 = y1;
                x0 = (int16_t)(r != 3 ? 0x152 : 0x19C);
                x1 = (int16_t)(r != 1 ? 0x19C : 0x152);
                zprimDrawLine(x0, y0, x1, y1);
            }
            if (dev->buttons != 0) {
                g_feCtlBind[g_feCtlRow] = 0;
                for (bit = 0; bit < 18; bit++) {
                    if (dev->buttons & (1u << bit)) {
                        g_feCtlBind[g_feCtlRow] = 1u << bit;     /* the lowest button pressed */
                        setText(CTL_BIND(g_feCtlRow), g_padButtonNames[bit]);
                        showId(CTL_BIND(g_feCtlRow));
                        break;
                    }
                }
                g_feCtlMode = 0;
                SOUND(SND_SELECT);
                return 1;
            }
        }
        return guiDefaultScreenHandler(msg, e, a, b);
    }

    switch (msg) {
    case GUI_MSG_UP:
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_UP):
    case P2(GUI_MSG_DOWN):
        if (g_feCtlRow < 11) {
            textPal(CTL_LABEL(g_feCtlRow), 0);
            textPal(CTL_KEY(g_feCtlRow), 0);
        }
        textPal(CTL_BIND(g_feCtlRow), 0);
        if (msg == GUI_MSG_UP || msg == P2(GUI_MSG_UP)) {
            if (g_feCtlRow == 0) {
                g_feCtlRow = 13;
                goto mark;
            }
            g_feCtlRow--;
        } else if (g_feCtlRow < 13) {
            g_feCtlRow++;
        } else {
            g_feCtlRow = 0;
        }
        if (g_feCtlRow <= 10) {
            textPal(CTL_LABEL(g_feCtlRow), 1);
            textPal(CTL_KEY(g_feCtlRow), 1);
        }
mark:
        textPal(CTL_BIND(g_feCtlRow), 2);
        SOUND(SND_MOVE);
        return 1;
    case GUI_MSG_LEFT:
    case P2(GUI_MSG_LEFT):
        if (g_feCtlRow == 11) {
            g_feCtlLayout = (uint8_t)(g_feCtlLayout == 0 ? 4 : (g_feCtlLayout - 1) % 5);
            setText(OPT(0x42), g_keyLayoutNames[g_feCtlLayout]);
            for (r = 0; r < 11; r++)
                setText(CTL_KEY(r), g_keyLayoutLabels[g_feCtlLayout][r]);
            return 1;
        }
        SOUND(SND_CHANGE);
        return 1;
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_RIGHT):
        if (g_feCtlRow == 11) {
            g_feCtlLayout = (uint8_t)((g_feCtlLayout + 1) % 5);
            setText(OPT(0x42), g_keyLayoutNames[g_feCtlLayout]);
            for (r = 0; r < 11; r++)
                setText(CTL_KEY(r), g_keyLayoutLabels[g_feCtlLayout][r]);
            return 1;
        }
        SOUND(SND_CHANGE);
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feCtlRow < 11) {
            g_feCtlMode = 1;                                     /* rebind this row */
            hideId(CTL_BIND(g_feCtlRow));
            SOUND(SND_CHANGE);
            return 1;
        }
        SOUND(SND_SELECT);
        if (g_feCtlRow == 11)
            return 1;
        if (g_feCtlRow == 12) {                                  /* accept */
            memcpy(profileBindings(p), g_feCtlBind, sizeof(g_feCtlBind));
            g_saveProfiles[p].unk_40[0x0B] = g_feCtlLayout;
            ev = SND_SELECT;
        } else if (g_feCtlRow == 13) {                           /* cancel */
            ev = SND_BACK;
        } else {
            frontendControllerLeave();
            return 1;
        }
        SOUND(ev);
        frontendControllerLeave();
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        SOUND(SND_BACK);
        frontendControllerLeave();
        return 1;
    }
    return guiDefaultScreenHandler(msg, e, a, b);
}

/* ================================================================== options: audio */

int32_t  g_feMusicVol;           /* 0x00691E64 0..18 (save +0x0D, channel 1) */
int32_t  g_feSfxVol;             /* 0x00691DD4 0..18 (save +0x0C, channel 0) */
int32_t  g_feAudioX0;            /* 0x00691E00 */
int32_t  g_feAudioX1;            /* 0x00691E04 */
int32_t  g_feAudioSel;           /* 0x00691E20 */

static int frontendAudioHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

/* VA 0x00442730, 561 bytes, [I] row, status draft */
void frontendAudioEnter(void)
{
    g_feMusicVol = g_saveBlockA.volume_0d * 18 / 18;
    g_feSfxVol = g_saveBlockA.volume_0c * 18 / 18;
    g_feAudioX0 = 0;
    g_feAudioX1 = 0;
    g_feAudioSel = 0;
    setText(OPT(0x49), zTextGet(0x147));                         /* MUSIC */
    setText(OPT(0x4C), zTextGet(0x148));                         /* SOUND FX */
    showId(OPT(0x47));
    showId(OPT(0x48));
    showId(OPT(0x4A));
    showId(OPT(0x4B));
    showId(OPT(0x49));
    showId(OPT(0x4C));
    textPal(OPT(0x49), 1);
    textPal(OPT(0x4C), 1);
    sprPos(OPT(0x47), 0x200, 0);
    sprPos(OPT(0x48), 0x200, 0);
    sprPos(OPT(0x4A), 0x200, 0);
    sprPos(OPT(0x4B), 0x200, 0);
    textPos(OPT(0x49), 0x200, 0);
    textPos(OPT(0x4C), 0x200, 0);
    guiFindElement(OPT(0x48))->handler = frontendAudioHandler;   /* the two level bars */
    guiFindElement(OPT(0x4B))->handler = frontendAudioHandler;
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    guiSetScreenHandler(frontendAudioHandler);
    g_feTask = frontendAudioSlideIn;
}

/* VA 0x00442a00, 284 bytes, [I] row, status draft */
void frontendAudioLeave(void)
{
    hideId(OPT(0x0E));
    hideId(OPT(0x0D));
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    hideId(OPT(0x12));
    hideId(OPT(0x11));
    hideId(OPT(0x14));
    hideId(OPT(0x13));
    frontendTabsReset();
    frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB));
    hideId(OPT(0x47));
    hideId(OPT(0x48));
    hideId(OPT(0x4A));
    hideId(OPT(0x4B));
    hideId(OPT(0x49));
    hideId(OPT(0x4C));
    guiSetScreenHandler(frontendOptionsHandler);
    g_feTask = frontendOptionsTabsIn;
}

/* VA 0x00442b70, 148 bytes, [I] row, status draft */
void frontendAudioSlideIn(void)
{
    g_feAudioX0 += (500 - g_feAudioX0) / 5;
    if (g_feAudioX0 > 0x1CC)
        g_feAudioX0 = 0x1CC;
    if (g_feAudioX0 > 0x104)
        g_feAudioX1 += (500 - g_feAudioX1) / 5;
    if (g_feAudioX1 > 0x1CC)
        g_feAudioX1 = 0x1CC;
    if (g_feAudioX1 > 0x1CB)
        g_feTask = NULL;
}

/* VA 0x00442c40, 133 bytes, [I] row, status draft */
void frontendAudioSlideOut(void)
{
    g_feAudioX0 += (500 - g_feAudioX0) / -5;
    if (g_feAudioX0 < 0)
        g_feAudioX0 = 0;
    if (g_feAudioX0 < 400)
        g_feAudioX1 -= (500 - g_feAudioX1) / 5;
    if (g_feAudioX1 < 0)
        g_feAudioX1 = 0;
    if (g_feAudioX1 < 1)
        frontendAudioLeave();               /* inlined in the exe */
}

/* VA 0x00442ce0, 1340 bytes, [I] row, status draft */
static int frontendAudioHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int16_t w0, w1;

    switch (msg) {
    case GUI_MSG_DRAW_ELEM:                 /* a level bar: clipped to 6 px per step */
        if (e->id == OPT(0x48))
            zprimSetClip(0x2BF - g_feAudioX0, 0, 0x2BF - g_feAudioX0 + g_feMusicVol * 6, 0x1DF);
        else if (e->id == OPT(0x4B))
            zprimSetClip(0x2BF - g_feAudioX1, 0, 0x2BF - g_feAudioX1 + g_feSfxVol * 6, 0x1DF);
        else
            return 1;
        guiElementHandler(msg, e, a, b);
        zprimSetClip(0, 0, 0x27F, 0x1DF);
        return 1;
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        guiSetFrame(guiFindElement(OPT(0x48)), (uint16_t)((g_feAudioSel != 0) + 1));
        guiSetFrame(guiFindElement(OPT(0x4B)), (uint16_t)((g_feAudioSel != 1) + 1));
        textPal(OPT(0x49), g_feAudioSel == 0);
        textPal(OPT(0x4C), g_feAudioSel == 1);
        w0 = guiGetTextWidth(guiFindElement(OPT(0x49)));
        w1 = guiGetTextWidth(guiFindElement(OPT(0x4C)));
        sprPos(OPT(0x47), 0x200 - g_feAudioX0, 0);
        sprPos(OPT(0x48), 0x200 - g_feAudioX0, 0);
        textPos(OPT(0x49), 0x2A0 - g_feAudioX0 - (uint16_t)w0, 0);
        sprPos(OPT(0x4A), 0x200 - g_feAudioX1, 0);
        sprPos(OPT(0x4B), 0x200 - g_feAudioX1, 0);
        textPos(OPT(0x4C), 0x2A0 - g_feAudioX1 - (uint16_t)w1, 0);
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (--g_feAudioSel < 0)
            g_feAudioSel = 1;
        SOUND(SND_MOVE);
        return 1;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if (++g_feAudioSel > 1)
            g_feAudioSel = 0;
        SOUND(SND_MOVE);
        return 1;
    case GUI_MSG_LEFT:
    case P2(GUI_MSG_LEFT):
        if (g_feAudioSel != 0) {
            if (g_feAudioSel == 1 && g_feSfxVol > 0) {
                g_feSfxVol--;
                bmxsoundEvent(SND_VOLUME, 0, g_feSfxVol * 7, 0, 0);
                SOUND(SND_MOVE);
            }
            goto store;
        }
        if (g_feMusicVol < 1)
            goto store;
        g_feMusicVol--;
        bmxsoundEvent(SND_VOLUME, 1, g_feMusicVol * 7, 0, 0);
        bmxsoundEvent(SND_VOLUME, 0, g_feMusicVol * 7, 0, 0);   /* sic: both, then sfx restored */
        SOUND(SND_MOVE);
        break;
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_RIGHT):
        if (g_feAudioSel != 0) {
            if (g_feAudioSel == 1 && g_feSfxVol < 18) {
                g_feSfxVol++;
                bmxsoundEvent(SND_VOLUME, 0, g_feSfxVol * 7, 0, 0);
                SOUND(SND_MOVE);
            }
            goto store;
        }
        if (g_feMusicVol > 17)
            goto store;
        g_feMusicVol++;
        bmxsoundEvent(SND_VOLUME, 1, g_feMusicVol * 7, 0, 0);
        bmxsoundEvent(SND_VOLUME, 0, g_feMusicVol * 7, 0, 0);
        SOUND(SND_MOVE);
        break;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask == NULL) {
            if (g_guiAutoBack) {
                g_feTask = frontendAudioLeave;
                return 1;
            }
            SOUND(SND_SELECT);
            g_feTask = frontendAudioSlideOut;
        }
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    bmxsoundEvent(SND_VOLUME, 0, g_feSfxVol * 7, 0, 0);
store:
    g_saveBlockA.volume_0d = (uint8_t)(g_feMusicVol * 18 / 18);
    g_saveBlockA.volume_0c = (uint8_t)(g_feSfxVol * 18 / 18);
    return 1;
}

/* ================================================================== options (0xA00C) */

int32_t  g_feOptSel;             /* 0x00691E60 0 controller, 1 cheats, 2 audio, -1 back */
uint32_t g_feOptReady;           /* 0x00691DF8 */
uint32_t g_feOptDelay;           /* 0x00691E14 */

/* VA 0x004433e0, 48 bytes, [I] row, status draft */
int frontendOptionsEnter(void)
{
    guiDestroyScreen();
    guiLoadBackdropImage(0xA015, 7);
    guiLoadScreen(SCR_OPTIONS, frontendOptionsHandler);
    g_feTask = frontendOptionsInit;
    return 0;
}

/* VA 0x00443420, 465 bytes, [I] row, status draft */
static int frontendOptionsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        if (g_feOptReady) {
            if (g_feCursorPhase < 0 || g_feCursorPhase > 0x40)
                g_feCursorStep = -g_feCursorStep;
            g_feCursorPhase += g_feCursorStep;
            frontendCursorDraw(0x1F, (int16_t)(g_feOptSel * 0x54 + 0x46), 0x71, (int16_t)(g_feOptSel * 0x54 + 0xA3),
                               (uint8_t)(0x80 - g_feCursorPhase), 1);
        }
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (g_feTask != NULL)
            return 0;
        if (--g_feOptSel < 0)
            g_feOptSel = 2;
        break;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if (g_feTask != NULL)
            return 0;
        if (++g_feOptSel > 2)
            g_feOptSel = 0;
        break;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feTask != NULL)
            return 1;
        if (g_feOptSel == 1 && g_saveBlockA.cheats_unlocked == 0) {
            SOUND(SND_ERROR);                                    /* CHEATS: nothing unlocked */
            return 1;
        }
        g_feOptPlayer = (msg == GUI_MSG_ACCEPT || msg == GUI_MSG_START) ? 0 : 1;
        goto go;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        g_feOptSel = -1;
        g_feTask = frontendOptionsTabsOut;
        if (g_guiAutoBack) {
            g_feOptReady = 0;
            g_feOptSel = -1;
            return 1;
        }
go:
        g_feOptReady = 0;
        g_feTask = frontendOptionsTabsOut;
        SOUND(SND_SELECT);
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    frontendOptionsRefresh();
    SOUND(SND_MOVE);
    return 0;
}

/* VA 0x004436c0, 282 bytes, [I] row, status draft */
void frontendOptionsRefresh(void)
{
    tint(OPT(5), 0xFF, 0xFF, 0xFF, g_feOptSel == 0 ? 0xFF : 0x50);
    tint(OPT(7), 0xFF, 0xFF, 0xFF, g_feOptSel == 1 ? 0xFF : 0x50);
    tint(OPT(9), 0xFF, 0xFF, 0xFF, g_feOptSel == 2 ? 0xFF : 0x50);
    textPal(OPT(6), g_feOptSel == 0);
    textPal(OPT(8), g_feOptSel == 1);
    textPal(OPT(0xA), g_feOptSel == 2);
}

/* VA 0x00443830, 35 bytes, [I] row, status draft */
void frontendOptionsExit(void)
{
    /* leaving OPTIONS writes SAVE.DAT */
    if (g_feOptDelay == 0) {
        frontendCursorFree();
        memcard_WriteSave();
        frontendMainMenuEnter(1);
        return;
    }
    g_feOptDelay--;
}

/* VA 0x00443870, 499 bytes, [I] row, status draft */
void frontendOptionsTabsOut(void)
{
    hideId(OPT(0x0E));
    hideId(OPT(0x0D));
    hideId(OPT(0x10));
    hideId(OPT(0x0F));
    hideId(OPT(0x12));
    hideId(OPT(0x11));
    hideId(OPT(0x14));
    hideId(OPT(0x13));
    textPos(OPT(6), (40 - g_feTab0) * 4, 0);
    textPos(OPT(8), (40 - g_feTab1) * 4, 0);
    textPos(OPT(0xA), (40 - g_feTab2) * 4, 0);
    if (g_guiAutoBack || frontendTabsOut(OPT(5), OPT(7), OPT(9), OPT(0xB))) {
        hideId(OPT(6));
        hideId(OPT(8));
        hideId(OPT(0xA));
        hideId(OPT(0xC));
        hideId(OPT(5));
        hideId(OPT(7));
        hideId(OPT(9));
        hideId(OPT(0xB));
        switch (g_feOptSel) {
        case 0:
            g_feTask = frontendControllerEnter;
            return;
        case 1:
            frontendCheatsEnter();           /* inlined in the exe */
            return;
        case 2:
            frontendAudioEnter();            /* inlined in the exe */
            return;
        case -1:
            g_feOptDelay = 2;
            g_feTask = frontendOptionsExit;
            return;
        }
    }
}

/* VA 0x00443af0, 340 bytes, [I] row, status draft */
void frontendOptionsTabsIn(void)
{
    showId(OPT(6));
    showId(OPT(8));
    showId(OPT(0xA));
    showId(OPT(5));
    showId(OPT(7));
    showId(OPT(9));
    hideId(OPT(0xB));
    if (g_guiAutoBack || frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB))) {
        hideId(OPT(0x14));
        hideId(OPT(0x13));
        frontendOptionsRefresh();
        g_feOptReady = 1;
        g_feTask = NULL;
    }
    textPos(OPT(6), (40 - g_feTab0) * 4, 0);
    textPos(OPT(8), (40 - g_feTab1) * 4, 0);
    textPos(OPT(0xA), (40 - g_feTab2) * 4, 0);
}

/* VA 0x00443cb0, 290 bytes, [I] row, status draft */
void frontendOptionsInit(void)
{
    g_feOptSel = 0;
    g_feOptReady = 0;
    frontendCursorCreate();
    frontendTabsReset();
    frontendTabsIn(OPT(5), OPT(7), OPT(9), OPT(0xB));
    textPos(OPT(6), (40 - g_feTab0) * 4, 0);
    textPos(OPT(8), (40 - g_feTab1) * 4, 0);
    textPos(OPT(0xA), (40 - g_feTab2) * 4, 0);
    setText(OPT(6), zTextGet(0x132));            /* CONTROLLER */
    setText(OPT(8), zTextGet(0x133));            /* CHEATS */
    setText(OPT(0xA), zTextGet(0x139));          /* AUDIO */
    g_feTask = frontendOptionsTabsIn;
}

/* ================================================================== load / save (0xA012, PSX only) */

int16_t   g_feLsSel;                 /* 0x00691DCE */
void    (*g_feLsDone)(uint32_t);     /* 0x00691DC0 never set: the screen is unreachable */
uint32_t  g_feLsDoneArg;             /* 0x00691DD0 */

static int frontendLoadSaveHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

static void loadSaveTexts(void)
{
    setText(CARD(7), zTextGet(0x28));            /* LOAD/SAVE */
    setText(CARD(8), zTextGet(0x24E));           /* SAVE - MEMORY CARD SLOT 1 */
    setText(CARD(9), zTextGet(0x24F));
    setText(CARD(0xA), zTextGet(0x250));         /* LOAD - MEMORY CARD SLOT 1 */
    setText(CARD(0xB), zTextGet(0x251));
    setText(CARD(0xC), zTextGet(0));             /* BACK TO MAIN MENU */
}

/* VA 0x00443e30, 34 bytes, [I] row, status draft */
int frontendLoadSaveEnter(void)
{
    /* [V] not referenced */
    guiLoadScreen(SCR_CARD, frontendLoadSaveHandler);
    g_feTask = frontendLoadSaveInit;
    return 1;
}

/* VA 0x00443e70, 524 bytes, [I] row, status draft */
void frontendLoadSaveInit(void)
{
    int i;

    g_feLsSel = 0;
    for (i = 8; i <= 0xC; i++)
        textPal(CARD(i), 2);
    loadSaveTexts();
    for (i = 0xD; i <= 0x14; i++)
        textPal(CARD(i), 3);
    g_feTask = NULL;
}

/* VA 0x00444110, 24 bytes, [I] row, status draft */
void frontendLoadSaveDone(void)
{
    g_feTask = NULL;
    g_feLsDone(g_feLsDoneArg);
}

/* VA 0x00444140, 360 bytes, [I] row, status draft */
void frontendLoadSaveReturn(void)
{
    /* the memory card dialogue closed */
    int i;

    if (g_feCardState == 7 || g_feCardState == 0x102) {
        g_feTask = frontendLoadSaveDone;
        guiSetScreenHandler(frontendLoadSaveHandler);
        return;
    }
    for (i = 7; i <= 0xC; i++)
        showId(CARD(i));
    loadSaveTexts();
    g_feTask = NULL;
    guiSetScreenHandler(frontendLoadSaveHandler);
}

/* VA 0x00444310, 1504 bytes, [I] row, status draft */
static int frontendLoadSaveHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    static const uint32_t ids[8] = { CARD(0xD), CARD(0xE), CARD(0xF), CARD(0x10),
                                     CARD(0x11), CARD(0x12), CARD(0x13), CARD(0x14) };
    int i;

    switch (msg) {
    case GUI_MSG_FRAME:
        zprimSetAttr(ZPRIM_ATTR_COLOUR, 0x10101);
        zprimDrawRect(0x28, 0, 0x1DB, 0xE8);
        if (g_feTask != NULL)
            g_feTask();
        for (i = 8; i <= 0xC; i++)
            tint(CARD(i), 0xC0, 0xC0, 0xC0, 0x80);
        textPal(CARD(7), 2);
        for (i = 0; i < 5; i++)
            textPal(CARD(8 + i), g_feLsSel == i);
        if (g_feLsSel >= 0 && g_feLsSel <= 4)
            tint(CARD(8 + g_feLsSel), 0xC0, 0xC0, 0x80, (int)g_guiPulse + 0x20);
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        SOUND(SND_MOVE);
        g_feLsSel = (int16_t)frontendWrap(g_feLsSel - 1, 5);
        return 1;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        SOUND(SND_MOVE);
        g_feLsSel = (int16_t)frontendWrap(g_feLsSel + 1, 5);
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feLsSel >= 0 && g_feLsSel <= 3) {
            hideId(CARD(2));
            hideId(CARD(1));
            hideId(CARD(4));
            hideId(CARD(3));
            hideId(CARD(6));
            hideId(CARD(5));
            for (i = 7; i <= 0xC; i++)
                hideId(CARD(i));
            SOUND(SND_SELECT);
            if (g_feLsSel <= 1)                                  /* SAVE - SLOT 1 / 2 */
                frontendCardOpen(1, (uint16_t)g_feLsSel, ids, 1, frontendLoadSaveReturn);
            else                                                 /* LOAD - SLOT 1 / 2 */
                frontendCardOpen(0, (uint16_t)(g_feLsSel - 2), ids, 1, frontendLoadSaveReturn);
            return 1;
        }
        if (g_feLsSel != 4)
            return 1;
        break;                                                   /* BACK TO MAIN MENU */
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        g_feTask = frontendLoadSaveDone;
        if (g_guiAutoBack)
            return 1;
        break;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    g_feTask = frontendLoadSaveDone;
    SOUND(SND_SELECT);
    return 1;
}

/* ================================================================== select screen (0xA009) */
/*
 * Pages (g_feSelPage): -1 back to the main menu, 0 rider, 1 bike, 2 style, 3 level, 4 start. With
 * two players the rider / bike / style pages run once per player (g_feSelPlayer) before the level.
 * Each page shows a 2- or 3-slot carousel of sprite + text elements (g_feCarouselSprite/Text);
 * left / right call g_feStep(-1 / +1), the carousel task slides it and g_feSlideDone commits.
 * Locked entries are drawn grey (zprimReplaceVRAMGrey) with "* LOCKED *" and cannot be accepted.
 * Unlock gating (career record of the selected rider, save.h):
 *   riders 0 .. riders_unlocked - 1 (save +0x07), bikes < record.bikes_unlocked (4 shown, the
 *   5th "secret bike" only when unlocked), styles < record.styles_unlocked, levels <
 *   record.levels_unlocked (two players: the highest of all twelve riders).
 */
typedef struct FeTickerHalf {
    int32_t  unk_00;             /* +0x00 */
    int32_t  unk_04;             /* +0x04 */
    uint16_t font;               /* +0x08 0x8001 */
    char     text[100];          /* +0x0A */
    uint16_t pad_6e;             /* +0x6E */
} FeTickerHalf;                  /* 0x70 */
typedef struct FeTickerLine { FeTickerHalf left, right; } FeTickerLine;   /* 0xE0 */

/* tables (0x0054D224 ...) */
const uint32_t g_feCarouselSprite[3] = { SEL(0xD), SEL(0xE), SEL(0xF) };     /* 0x0054D224 */
const uint32_t g_feCarouselText[3]   = { SEL(0x12), SEL(0x13), SEL(0x14) };  /* 0x0054D230 */
extern const uint16_t g_feRiderPortraits[12];   /* 0x0054D23C sprites 0x40B6.. */
extern const uint16_t g_feRiderNames[12];       /* 0x0054D254 texts 0xDE.. DAVE MIRRA .. AMISH BOY */
extern const int32_t  g_feRiderStats[12][5];    /* 0x0054D26C air, speed, spins, balance, (overall) 3..7 */
extern const uint32_t g_feLevelImages[12];      /* 0x0054D35C */
extern const uint32_t g_feLevelNames[12];       /* 0x0054D38C "GREENVILLE - VAIL TRAILS" ... */
extern const uint16_t g_feStyleImages[12][5];   /* 0x0054D3BC */
extern const uint16_t g_feRiderGroups[12];      /* 0x0054D434 zprim groups 0xA017.. (style art) */
extern const int16_t  g_feStyleIcons[12 * 5][8];/* 0x0054D44C icon index per slot, -1 none */
extern const uint16_t g_feStyleIconPos[8][2];   /* 0x0054D80C */
extern const int16_t  g_feBikePalette[12 * 5];  /* 0x0054D82C (palette + 1) */
extern const uint8_t  g_feBikeStats[12 * 5][5]; /* 0x0054D8A4 stat bonus of the bike */
extern const uint32_t g_feLevelThumbs[12];      /* 0x0054DB58 */
extern const uint16_t *const g_feRiderBios[12]; /* 0x0055AAA0 ticker lines ("#YNickname: #BMiracle Boy" ...) */
extern const uint16_t g_feTickerTier[4][4];     /* 0x0055AAD0 amateur / pro / hardcore / all done */
extern const uint16_t g_feTickerComp[4][4];     /* 0x0055AAF0 by competition result 0..3 */
extern const uint16_t g_feTickerLocked[4];      /* 0x0055AB10 */
extern const uint16_t g_feBikeNames[12];        /* 0x0055AB18 first bike text per rider (+ bike) */
extern const uint16_t g_feBikeImages[12];       /* 0x0055AB30 */
extern const uint16_t g_feTickerBikeLocked[4];  /* 0x0055AB48 */
extern const uint16_t *const g_feBikeBios[12 * 5]; /* 0x0055AD30 */

ZprimAsset *g_feStyleIconImg[16]; /* 0x005745EC */
int16_t   g_feBikeSpin;           /* 0x0057462C rotation frame 0..31 (sprites 0x4096..) */
uint32_t  g_feSelFrame;           /* 0x00691704 */
uint8_t   g_feBikeCount;          /* 0x0069181F */
int32_t   g_feCarouselX[3];       /* 0x00691820 */
uint32_t  g_feSelReady;           /* 0x0069182C input / page change waits until this frame */
int32_t   g_feCarouselCur;        /* 0x00691830 */
int16_t   g_feCarouselSpeed;      /* 0x00691834 */
int16_t   g_feStyleExtra;         /* 0x00691836 */
int32_t   g_feCarouselDir;        /* 0x00691838 the slide in progress (-1 / 1), 0 idle */
uint8_t   g_feStyleCount;         /* 0x0069183C */
FeTickerLine g_feTicker[5];       /* 0x00691840 */
int32_t   g_feSelRider[2];        /* 0x00691CA0 */
int32_t   g_feSelPlayer;          /* 0x00691CA8 */
const uint16_t *g_feTickerText;   /* 0x00691CAC NULL-terminated text-id list */
int32_t   g_feSelPage;            /* 0x00691CB0 */
int16_t   g_feCheatKeys[16];      /* 0x00691CC0 the last 16 key messages */
uint32_t  g_feTickerRun;          /* 0x00691CE0 */
int32_t   g_feCarouselN;          /* 0x00691CE4 */
uint32_t  g_feCursorFull;         /* 0x00691CE8 */
uint16_t  g_feCarouselDist;       /* 0x00691CEC */
uint16_t  g_feClipW;              /* 0x00691CEE */
uint16_t  g_feCheatPos;           /* 0x00691CF0 */
uint32_t  g_feLastChange;         /* 0x00691CF4 */
ZprimAsset *g_feImg1;             /* 0x00691CF8 */
ZprimAsset *g_feImg2;             /* 0x00691CFC */
ZprimAsset *g_feImg0;             /* 0x00691D00 */
uint16_t  g_feClipH;              /* 0x00691D04 */
uint16_t  g_feTickerScrollY;      /* 0x00691D06 */
uint8_t   g_feLevelCount;         /* 0x00691D08 */
int32_t   g_feSelStyle[2];        /* 0x00691D58 */
int32_t   g_feSelUnk60;           /* 0x00691D60 (= 4) */
uint16_t  g_feTickerPal[5][2];    /* 0x00691D80 */
int32_t   g_feSelLevel;           /* 0x00691D94 */
int32_t   g_feSelBike[2];         /* 0x00691D98 */
int32_t   g_feSelUnkA0;           /* 0x00691DA0 */
uint16_t  g_feTickerTop;          /* 0x00691DA4 */
uint16_t  g_feTickerWait;         /* 0x00691DA6 */
uint16_t  g_feTickerWidth[5];     /* 0x00691DB0 */
uint16_t  g_feTickerFill;         /* 0x00691DBA */
int32_t   g_feSelPending;         /* 0x00691DBC next / previous page asked for */

#define SELP (g_feSelPlayer)
#define RIDER (g_feSelRider[SELP])
#define RECORD(r) (g_saveBlockA.riders[(r)])

static int frontendSelectHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendSelectDrawHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

static uint32_t carouselSprite(int k) { return g_feCarouselSprite[frontendWrap(g_feCarouselCur + k, g_feCarouselN)]; }
static uint32_t carouselText(int k)   { return g_feCarouselText[frontendWrap(g_feCarouselCur + k, g_feCarouselN)]; }
static ZprimAsset *spriteOf(uint32_t id) { return guiFindElement(id)->sprite; }

/* VA 0x00444ae0, 175 bytes, [I] row, status draft */
void frontendRiderStep(int delta)
{
    int count = g_saveBlockA.riders_unlocked;

    g_feSelRider[SELP] += delta;
    if (g_feSelRider[SELP] >= count)
        g_feSelRider[SELP] = 0;
    if (g_feSelRider[SELP] < 0)
        g_feSelRider[SELP] = count - 1;
    hideId(carouselSprite(1));
    hideId(carouselText(1));
    frontendTickerSet(g_feRiderBios[g_feSelRider[SELP]]);
}

/* VA 0x00444bc0, 266 bytes, [I] row, status draft */
void frontendRiderShow(int delta)
{
    int r = g_feSelRider[SELP] + delta;

    if (r >= (int)g_saveBlockA.riders_unlocked)
        r = 0;
    else if (r < 0)
        r = g_saveBlockA.riders_unlocked - 1;
    g_feSelFrame++;
    setText(carouselText(1), zTextGet(g_feRiderNames[r]));
    g_feCarouselX[frontendWrap(g_feCarouselCur + 1, g_feCarouselN)] = delta << 7;
    zprimCopyMaterial(zprimLoadAsset(g_feRiderPortraits[r]), spriteOf(carouselSprite(1)));
    g_feLastChange = g_feSelFrame;
    g_feCarouselDir = -delta;
    g_feTask = frontendCarouselAnimate;
    if (delta != 0)
        SOUND(SND_MOVE);
}

/* VA 0x00444d20, 517 bytes, [I] row, status draft */
void frontendRiderPageEnter(void)
{
    int i;

    if (g_feSelFrame < g_feSelReady)
        return;
    if (!zprimGroupLoaded(3)) {
        zprimLoadGroup(3, 0xE004);                               /* rider portraits */
        frontendSelectMakeImage(0, 0, 0, 4, 0x10);
        frontendSelectMakeImage(1, 0, 0, 4, 0x10);
        frontendSelectMakeImage(2, 0, 0, 4, 0x10);
        frontendSelectMakeImage(1, 0x80, 0xC0, 8, 0x100);
        frontendSelectMakeImage(0, 0x80, 0xC0, 8, 0x100);
    }
    g_feCursorFull = 1;
    showId(SEL(2));
    showId(SEL(0x10));
    setText(SEL(0x10), zTextGet(0xD6));                          /* SELECT */
    showId(SEL(0x11));
    setText(SEL(0x11), zTextGet(0xD7));                          /* RIDER */
    g_feClipW = 0x80;
    g_feClipH = 0xC0;
    g_feSelRider[SELP]++;
    frontendRiderShow(-1);
    g_feCarouselDir = 1;
    g_feTask = frontendCarouselAnimate;
    g_feSlideDone = frontendRiderStep;
    g_feStep = frontendRiderShow;
    hideId(SEL(0x10));
    hideId(SEL(0x11));
    frontendSelectTabsShow();
    for (i = 0x18; i <= 0x1D; i++)                               /* stat bars, medals */
        showId(SEL(i));
    showId(SEL(1));
    showId(SEL(0x1E));
}

/* VA 0x00444fc0, 151 bytes, [I] row, status draft */
void frontendRiderPageLeave(void)
{
    zprimReplaceVRAMGrey(zprimLoadAsset(g_feRiderPortraits[RIDER]), spriteOf(SEL(0xD)), 8);
    frontendCarouselReset();
    g_feLastChange = g_feSelFrame;
    g_feStep = NULL;
    g_feSlideDone = NULL;
    frontendSelectTabsHide();
    zprimUnloadGroup(3);
    hideId(SEL(0x1C));
    hideId(SEL(0x1D));
    hideId(SEL(1));
    zmemDMMOptimize();
}

/* VA 0x00445090, 400 bytes, [I] row, status draft */
void frontendLevelStep(int delta)
{
    /* the ticker of a level: locked, the challenge tier reached, or the competition result */
    const uint16_t *list;
    const SaveGoalInfo *gi;
    int lvl;

    g_feSelLevel += delta;
    lvl = g_feSelLevel;
    showId(carouselSprite(0));
    showId(carouselSprite(1));
    hideId(carouselText(1));
    if (g_numPlayers == 2) {
        list = lvl < g_feLevelCount ? NULL : g_feTickerLocked;
    } else if (lvl > 5) {                                        /* competitions 6..11 */
        list = lvl < g_feLevelCount ? g_feTickerComp[RECORD(RIDER).comp_result[lvl - 6]] : g_feTickerLocked;
    } else if (lvl >= g_feLevelCount) {
        list = g_feTickerLocked;
    } else {                                                     /* challenge levels 0..5 */
        gi = &RECORD(RIDER).goals[lvl];
        switch (gi->tier) {
        case 0:  list = gi->tier_goals_done != 4 ? g_feTickerTier[0] : g_feTickerTier[1]; break;
        case 1:  list = gi->tier_goals_done != 4 ? g_feTickerTier[1] : g_feTickerTier[2]; break;
        case 2:  list = gi->tier_goals_done == 4 ? g_feTickerTier[3] : g_feTickerTier[2]; break;
        default:
            g_feTickerRun = 0;
            g_feTickerScrollY = 3;
            return;
        }
    }
    frontendTickerSet(list);
    g_feTickerRun = 0;
    g_feTickerScrollY = 3;
}

/* VA 0x00445290, 396 bytes, [I] row, status draft */
void frontendLevelShowNow(int delta)
{
    /* not referenced: a level change without the slide */
    int lvl = g_feSelLevel + delta;
    ZprimAsset *src = zprimLoadAsset(g_feLevelImages[lvl]);

    if (lvl < g_feLevelCount)
        zprimCopyMaterial(src, spriteOf(carouselSprite(1)));
    else
        zprimReplaceVRAMGrey(src, spriteOf(carouselSprite(1)), 8);
    g_feLastChange = g_feSelFrame;
    showId(carouselSprite(0));
    showId(carouselSprite(1));
    setText(g_feCarouselText[frontendWrap(g_feCarouselCur + delta * 2, g_feCarouselN)], EMPTY);
    setText(g_feCarouselText[frontendWrap(g_feCarouselCur + delta, g_feCarouselN)],
            zTextGet(lvl < g_feLevelCount ? g_feLevelNames[lvl] : 0x3A));
    g_feCarouselDir = 0;
    g_feTask = NULL;
    g_feSlideDone = frontendLevelStep;
}

/* VA 0x00445490, 565 bytes, [I] row, status draft */
void frontendLevelShow(int delta)
{
    int lvl = g_feSelLevel + delta;
    ZprimAsset *dst, *src;

    if (lvl >= 12 || lvl < 0) {
        SOUND(SND_ERROR);                                        /* no wrap on the level page */
        return;
    }
    g_feSelFrame++;
    showId(carouselSprite(0));
    showId(carouselSprite(1));
    g_feCarouselX[frontendWrap(g_feCarouselCur, g_feCarouselN)] = 0;
    g_feCarouselX[frontendWrap(g_feCarouselCur + 1, g_feCarouselN)] = delta < 1 ? -240 : 240;
    dst = spriteOf(carouselSprite(1));
    setText(g_feCarouselText[frontendWrap(g_feCarouselCur + delta, g_feCarouselN)],
            zTextGet(lvl < g_feLevelCount ? g_feLevelNames[lvl] : 0x3A));    /* "* LOCKED *" */
    src = zprimLoadAsset(g_feLevelImages[lvl]);
    if (lvl < g_feLevelCount)
        zprimCopyMaterial(src, dst);
    else
        zprimReplaceVRAMGrey(src, dst, 8);
    g_feCarouselDir = -delta;
    g_feLastChange = g_feSelFrame;
    g_feSlideDone = frontendLevelStep;
    g_feTask = frontendCarouselAnimate;
    if (delta != 0)
        SOUND(SND_MOVE);
}

/* VA 0x00445760, 535 bytes, [I] row, status draft */
void frontendLevelPageEnter(void)
{
    int r;

    if (g_feSelFrame < g_feSelReady)
        return;
    zprimLoadGroup(3, 0xE003);                                   /* level pictures */
    g_feCursorFull = 1;
    g_feCarouselN = 2;
    g_feCarouselDist = 0xF0;
    g_feCarouselSpeed = 0x14;
    if (g_numPlayers == 1) {
        g_feLevelCount = RECORD(RIDER).levels_unlocked;
    } else {
        g_feLevelCount = 1;                                      /* two players: the best rider's */
        for (r = 0; r < 12; r++)
            if (g_feLevelCount < RECORD(r).levels_unlocked)
                g_feLevelCount = RECORD(r).levels_unlocked;
    }
    showId(SEL(0x10));
    setText(SEL(0x10), zTextGet(0xDC));                          /* SELECT */
    showId(SEL(0x11));
    setText(SEL(0x11), zTextGet(0xDD));                          /* LEVEL */
    frontendSelectMakeImage(0, 0, 0, 8, 0x100);
    frontendSelectMakeImage(1, 0, 0, 8, 0x100);
    frontendSelectMakeImage(0, 0xF0, 0xB8, 8, 0x100);
    frontendSelectMakeImage(1, 0xF0, 0xB8, 8, 0x100);
    g_feClipW = 0xF0;
    g_feClipH = 0xB4;
    frontendSelectTabsShow();
    sprPos(SEL(0xD), 0, 0);
    sprPos(SEL(0xE), 0xF0, 0);
    showId(SEL(0xD));
    showId(SEL(0xE));
    hideId(SEL(0x10));
    hideId(SEL(0x11));
    g_feSelLevel++;
    g_feStep = frontendLevelShow;
    g_feTask = NULL;
    frontendLevelShow(-1);
    g_feCarouselDir = 1;
}

/* VA 0x00445a10, 177 bytes, [I] row, status draft */
void frontendLevelPageLeave(void)
{
    zprimReplaceVRAMGrey(zprimLoadAsset(g_feLevelImages[g_feSelLevel]), spriteOf(SEL(0xD)), 8);
    g_feLastChange = g_feSelFrame;
    frontendCarouselReset();
    showId(SEL(0xD));
    hideId(SEL(0xE));
    sprPos(SEL(0xE), 0x78, 0x78);
    g_feCarouselN = 2;
    g_feCarouselDist = 0x80;
    g_feCarouselSpeed = 0x10;
    g_feStep = NULL;
    frontendSelectTabsHide();
    zprimUnloadGroup(3);
}

/* VA 0x00445b00, 112 bytes, [I] row, status draft */
void frontendStyleStep(int delta)
{
    g_feSelStyle[SELP] += delta;
    showId(SEL(5));
    hideId(carouselSprite(1));
    hideId(carouselText(1));
}

/* VA 0x00445b90, 391 bytes, [I] row, status draft */
void frontendStyleShow(int delta)
{
    char buf[100];
    int st = g_feSelStyle[SELP] + delta, slot;
    ZprimAsset *src, *dst;

    if (st >= 5 || st < 0) {
        SOUND(SND_ERROR);
        return;
    }
    zstringPrintf(buf, "%s %d", zTextGet(0xDB), st + 1);         /* STYLE n */
    g_feSelFrame++;
    setText(carouselText(1), buf);
    slot = g_feCarouselCur ^ 1;                                  /* two slots */
    g_feCarouselX[slot] = delta << 7;
    dst = spriteOf(g_feCarouselSprite[slot]);
    src = zprimLoadAsset(g_feStyleImages[RIDER][st]);
    if (st < g_feStyleCount) {
        zprimCopyMaterial(src, dst);
    } else {
        zprimReplaceVRAMGrey(src, dst, 8);
        setText(carouselText(1), zTextGet(0x3A));
    }
    g_feCarouselDir = -delta;
    g_feLastChange = g_feSelFrame;
    g_feTask = frontendCarouselAnimate;
    if (delta != 0)
        SOUND(SND_MOVE);
}

/* VA 0x00445d90, 771 bytes, [I] row, status draft */
void frontendStylePageEnter(void)
{
    int i, n;
    ZprimAsset *icon;

    if (g_feSelFrame < g_feSelReady)
        return;
    if (!zprimGroupLoaded(3) || SELP == 0 || (SELP == 1 && g_feSelRider[0] != g_feSelRider[1])) {
        frontendSelectMakeImage(0, 0, 0, 4, 0x10);
        frontendSelectMakeImage(1, 0, 0, 4, 0x10);
        frontendSelectMakeImage(2, 0, 0, 4, 0x10);
        frontendSelectMakeImage(1, 0x80, 0xC0, 8, 0x100);
        frontendSelectMakeImage(0, 0x80, 0xC0, 8, 0x100);
        zprimUnloadGroup(3);
        zprimLoadGroup(3, (uint16_t)(g_feRiderGroups[RIDER] | 0xE000));   /* the rider's style art */
    }
    g_feCursorFull = 0;
    g_feStyleCount = RECORD(RIDER).styles_unlocked;
    g_feSelStyle[SELP] = g_feStyleCount - 1;                     /* starts on the newest style */
    showId(SEL(0x10));
    showId(SEL(0x11));
    showId(SEL(2));
    setText(SEL(0x12), EMPTY);
    setText(SEL(0x13), EMPTY);
    setText(SEL(0x14), EMPTY);
    for (i = 0; i < 16; i++) {
        if (g_feStyleIconImg[i] != NULL) {
            zprimFree(g_feStyleIconImg[i]);
            g_feStyleIconImg[i] = NULL;
        }
    }
    n = zprimGetGroupCount(3);
    g_feStyleExtra = (int16_t)(n - 6);
    for (i = 6; i < n; i++) {                                    /* entries 6.. are the icons */
        icon = zprimCreateImage(0x54, 0x30, 4, 0x10);
        g_feStyleIconImg[i - 6] = icon;
        zprimCopyMaterial(zprimSelectGroupEntry(3, (uint32_t)i), icon);
        g_feLastChange = g_feSelFrame;
    }
    g_feSelStyle[SELP]++;
    frontendStyleShow(-1);
    g_feCarouselDir = 1;
    frontendSelectTabsShow();
    for (i = 0x18; i <= 0x1D; i++)
        hideId(SEL(i));
    hideId(SEL(1));
    hideId(SEL(0x1E));
    showId(SEL(4));
    g_feClipW = 0x80;
    g_feClipH = 0xC0;
    g_feTask = frontendCarouselAnimate;
    g_feStep = frontendStyleShow;
    g_feSlideDone = frontendStyleStep;
}

/* VA 0x00446170, 198 bytes, [I] row, status draft */
void frontendStylePageLeave(void)
{
    int i;

    zprimReplaceVRAMGrey(zprimLoadAsset(g_feStyleImages[RIDER][g_feSelStyle[SELP]]), spriteOf(SEL(0xD)), 8);
    g_feLastChange = g_feSelFrame;
    frontendCarouselReset();
    hideId(SEL(5));
    for (i = 0; i < 16; i++) {
        if (g_feStyleIconImg[i] != NULL) {
            zprimFree(g_feStyleIconImg[i]);
            g_feStyleIconImg[i] = NULL;
        }
    }
    hideId(SEL(4));
    g_feStep = NULL;
    g_feSlideDone = NULL;
    frontendSelectTabsHide();
    zprimUnloadGroup(3);
    zmemDMMOptimize();
}

/* VA 0x00446280, 204 bytes, [I] row, status draft */
void frontendBikeSpin(void)
{
    /* the bike turns: every 5th frame the next of 32 views (sprites 0x4096..0x40B5) */
    ZprimAsset *src, *dst;

    if (g_feSelFrame % 5 != 2)
        return;
    g_feBikeSpin = (int16_t)frontendWrap(g_feBikeSpin + 1, 0x20);
    dst = spriteOf(carouselSprite(0));
    src = zprimLoadAsset((uint32_t)(g_feBikeSpin + 0x4096));
    if (g_feSelBike[SELP] >= g_feBikeCount) {
        zprimReplaceVRAMGrey(src, dst, 4);
        g_feLastChange = g_feSelFrame;
        return;
    }
    zprimSelectPaletteBank(src, dst, 4);
    g_feLastChange = g_feSelFrame;
}

/* VA 0x00446390, 240 bytes, [I] row, status draft */
void frontendBikeStep(int delta)
{
    g_feSelBike[SELP] += delta;
    hideId(carouselSprite(1));
    hideId(carouselText(1));
    frontendTickerSet(g_feSelBike[SELP] < g_feBikeCount ? g_feBikeBios[RIDER * 5 + g_feSelBike[SELP]]
                                                       : g_feTickerBikeLocked);
    g_feTickerRun = 0;
    g_feTickerScrollY = 3;
    sprPos(SEL(0xF), 0, 0xA0);
    showId(SEL(0xF));
    g_feTask = frontendBikeSpin;
}

/* VA 0x004464c0, 493 bytes, [I] row, status draft */
void frontendBikeShow(int delta)
{
    int max = g_feBikeCount > 4 ? g_feBikeCount : 4;             /* 4 shown + the secret bike if open */
    int bk = g_feSelBike[SELP] + delta;
    ZprimAsset *src, *dst;

    if (bk >= max || bk < 0) {
        SOUND(SND_ERROR);
        return;
    }
    setText(carouselText(1), zTextGet((uint32_t)(g_feBikeNames[RIDER] + bk)));
    g_feCarouselX[frontendWrap(g_feCarouselCur + 1, g_feCarouselN)] = delta << 7;
    guiSetPalette(guiFindElement(carouselSprite(1)), (uint32_t)(g_feBikePalette[RIDER * 5 + bk] - 1));
    dst = spriteOf(carouselSprite(1));
    src = zprimLoadAsset((uint32_t)(g_feBikeSpin + 0x4096));
    if (bk < g_feBikeCount) {
        zprimSelectPaletteBank(src, dst, 4);
    } else {
        zprimReplaceVRAMGrey(src, dst, 4);
        setText(carouselText(1), zTextGet(0x3A));
    }
    g_feCarouselDir = -delta;
    g_feLastChange = g_feSelFrame;
    g_feTask = frontendCarouselAnimate;
    if (delta != 0)
        SOUND(SND_MOVE);
}

/* VA 0x00446730, 589 bytes, [I] row, status draft */
void frontendBikePageEnter(void)
{
    if (g_feSelFrame < g_feSelReady)
        return;
    if (!zprimGroupLoaded(3)) {
        zprimLoadGroup(3, 0xE002);                               /* bikes */
        frontendSelectMakeImage(0, 0, 0, 4, 0x10);
        frontendSelectMakeImage(1, 0, 0, 4, 0x10);
        frontendSelectMakeImage(2, 0, 0, 4, 0x10);
        frontendSelectMakeImage(2, 0x80, 0x46, 4, 0x10);
        frontendSelectMakeImage(1, 0x80, 0x80, 4, 0x100);
        frontendSelectMakeImage(0, 0x80, 0x80, 4, 0x100);
    }
    g_feBikeCount = RECORD(RIDER).bikes_unlocked;
    if (g_feSelBike[SELP] > g_feBikeCount - 1)
        g_feSelBike[SELP] = g_feBikeCount - 1;
    g_feCursorFull = 1;
    setText(SEL(0x12), EMPTY);
    setText(SEL(0x13), EMPTY);
    g_feClipW = 0x80;
    g_feClipH = 0x80;
    showId(SEL(2));
    hideId(SEL(0x10));
    hideId(SEL(0x11));
    frontendCarouselReset();
    guiFindElement(SEL(3))->sprite = g_feImg2;                   /* the rider in the corner */
    showId(SEL(3));
    zprimCopyMaterial(zprimLoadAsset(g_feBikeImages[RIDER]), spriteOf(SEL(3)));
    g_feLastChange = g_feSelFrame;
    g_feSelBike[SELP]++;
    frontendBikeShow(-1);
    g_feCarouselDir = 1;
    g_feTask = frontendCarouselAnimate;
    g_feStep = frontendBikeShow;
    g_feSlideDone = frontendBikeStep;
    frontendSelectTabsShow();
    showId(SEL(0x18));
    showId(SEL(0x19));
    showId(SEL(0x1A));
    showId(SEL(0x1B));
    showId(SEL(0x1E));
}

/* VA 0x00446a20, 120 bytes, [I] row, status draft */
void frontendBikePageLeave(void)
{
    zprimReplaceVRAMGrey(zprimLoadAsset((uint32_t)(g_feBikeSpin + 0x4096)), spriteOf(SEL(0xD)), 4);
    g_feLastChange = g_feSelFrame;
    frontendCarouselReset();
    g_feStep = NULL;
    frontendSelectTabsHide();
    guiFindElement(SEL(3))->sprite = NULL;
    hideId(SEL(3));
    zprimUnloadGroup(3);
    zmemDMMOptimize();
}

/* ---- the cheat codes ---- */

/* six codes x (pad 1, pad 2) x 16 key messages, 0 = any key (the exe builds this table on the stack) */
static const int16_t s_cheatCodes[6][2][16] = {
    /* 0 Slim Jim Guy (rider page): down down left right up up circle */
    { { 0x1F, 0x1F, 0x20, 0x21, 0x1E, 0x1E, 0x23 }, { 0x2C, 0x2C, 0x2D, 0x2E, 0x2B, 0x2B, 0x30 } },
    /* 1 Amish Boy (rider page, after Slim Jim Guy): up up up down up up up right circle */
    { { 0x1E, 0x1E, 0x1E, 0x1F, 0x1E, 0x1E, 0x1E, 0x21, 0x23 }, { 0x2B, 0x2B, 0x2B, 0x2C, 0x2B, 0x2B, 0x2B, 0x2E, 0x30 } },
    /* 2 all bikes (bike page): up left up down up right left right circle */
    { { 0x1E, 0x20, 0x1E, 0x1F, 0x1E, 0x21, 0x20, 0x21, 0x23 }, { 0x2B, 0x2D, 0x2B, 0x2C, 0x2B, 0x2E, 0x2D, 0x2E, 0x30 } },
    /* 3 all styles (style page): left up right down left down right up left circle */
    { { 0x20, 0x1E, 0x21, 0x1F, 0x20, 0x1F, 0x21, 0x1E, 0x20, 0x23 },
      { 0x2D, 0x2B, 0x2E, 0x2C, 0x2D, 0x2C, 0x2E, 0x2B, 0x2D, 0x30 } },
    /* 4 all levels (level page): up up down down left right left circle */
    { { 0x1E, 0x1E, 0x1F, 0x1F, 0x20, 0x21, 0x20, 0x23 }, { 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2E, 0x2D, 0x30 } },
    /* 5 no effect: down left down left down left down circle */
    { { 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x20, 0x1F, 0x23 }, { 0x2C, 0x2D, 0x2C, 0x2D, 0x2C, 0x2D, 0x2C, 0x30 } },
};

static void cheatAccepted(void)
{
    SOUND(SND_SELECT);
    memset(g_feCheatKeys, 0, sizeof(g_feCheatKeys));
}

/* VA 0x00446ac0, 2113 bytes, [I] row, status draft */
void frontendSelectCheats(int16_t key)
{
    /* Every key message of the select screen goes into a 16-entry ring; a code matches when it
     * appears as a run in the ring (zeros in the code match anything). The effects write the
     * career record / riders_unlocked directly (they are saved with the next SAVE.DAT write). */
    const int16_t *code;
    int c, v, start, k, same;

    g_feCheatKeys[g_feCheatPos] = key;
    g_feCheatPos = (uint16_t)((g_feCheatPos + 1) & 0xF);
    for (c = 0; c < 6; c++) {
        for (v = 0; v < 2; v++) {
            code = s_cheatCodes[c][v];
            for (start = 0; start < 16; start++) {
                if (g_feCheatKeys[start] != code[0])
                    continue;
                same = 0;
                for (k = 0; k < 16; k++)
                    if (code[k] == g_feCheatKeys[(start + k) & 0xF] || code[k] == 0)
                        same++;
                if (same != 16)
                    continue;
                switch (c) {
                case 0:
                    if (g_saveBlockA.riders_unlocked < 11 && g_feSelPage == 0) {
                        g_saveBlockA.riders_unlocked = 11;             /* + Slim Jim Guy */
                        cheatAccepted();
                    }
                    break;
                case 1:
                    if (g_saveBlockA.riders_unlocked > 10 && g_feSelPage == 0) {
                        g_saveBlockA.riders_unlocked = 12;             /* + Amish Boy (no sound) */
                        break;
                    }
                    /* fall through (the compiled switch shares the tails) */
                case 2:
                    if (RECORD(RIDER).bikes_unlocked < 5 && g_feSelPage == 1) {
                        RECORD(RIDER).bikes_unlocked = 5;
                        g_feBikeCount = 5;
                        frontendBikeStep(0);
                        cheatAccepted();
                    }
                    /* fall through */
                case 3:
                    if (RECORD(RIDER).styles_unlocked < 5 && g_feSelPage == 2) {
                        RECORD(RIDER).styles_unlocked = 5;
                        g_feStyleCount = 5;
                        cheatAccepted();
                    }
                    /* fall through */
                case 4:
                    if (RECORD(RIDER).levels_unlocked < 12 && g_feSelPage == 3) {
                        RECORD(RIDER).levels_unlocked = 12;
                        g_feLevelCount = 12;
                        cheatAccepted();
                    }
                    break;
                default:
                    break;                                       /* code 5 does nothing */
                }
                break;                                           /* next code variant */
            }
        }
    }
}

/* VA 0x00447540, 124 bytes, [I] row, status draft */
int frontendSelectEnter(void)
{
    guiDestroyScreen();
    g_feImg0 = NULL;
    g_feImg1 = NULL;
    g_feImg2 = NULL;
    switch (g_selectedSubMode) {                                 /* the backdrop of the game mode */
    case 0: guiLoadBackdropImage(0xA00E, 3); break;              /* proquest */
    case 1: guiLoadBackdropImage(0xA010, 5); break;              /* session */
    case 2: guiLoadBackdropImage(0xA00F, 4); break;              /* free ride */
    case 3: guiLoadBackdropImage(0xA006, 9); break;              /* (never set) */
    }
    zprimLoadGroup(2, 0xE001);
    guiLoadScreen(SCR_SELECT, frontendSelectHandler);
    g_feTask = frontendSelectInit;
    return 0;
}

/* VA 0x00447600, 141 bytes, [I] row, status draft */
void frontendSelectExit(void)
{
    if (g_feSelFrame < g_feSelReady)
        return;
    if (g_feImg2 != NULL)
        zprimFree(g_feImg2);
    if (g_feImg1 != NULL)
        zprimFree(g_feImg1);
    if (g_feImg0 != NULL)
        zprimFree(g_feImg0);
    frontendCursorFree();
    zprimUnloadGroup(2);
    zprimUnloadGroup(3);
    g_feTask = NULL;
    guiDestroyScreen();
    if (g_feSelPage == -1)
        frontendMainMenuEnter(1);
    else if (g_feSelPage == 4)
        frontendRequestExit(0);                                  /* start the game */
}

/* VA 0x004476c0, 223 bytes, [I] row, status draft */
void frontendSelectStart(void)
{
    sprPos(SEL(0xD), 0, 0);
    sprPos(SEL(0xE), 0x78, 0);
    showId(SEL(0xD));
    if (g_feSelFrame < g_feSelReady)
        return;
    sprPos(SEL(0xF), 0xF0, 0);
    showId(SEL(6));                                              /* LOADING */
    g_saveProfiles[0].rider = (uint8_t)g_feSelRider[0];
    g_saveProfiles[1].rider = (uint8_t)g_feSelRider[1];
    g_saveProfiles[0].style = (uint8_t)g_feSelStyle[0];
    g_saveProfiles[1].style = (uint8_t)g_feSelStyle[1];
    g_saveProfiles[0].bike = (uint8_t)g_feSelBike[0];
    g_saveProfiles[1].bike = (uint8_t)g_feSelBike[1];
    g_currentLevel = (uint8_t)g_feSelLevel;
    g_feTask = frontendSelectSlideOut;
}

/* VA 0x004477e0, 156 bytes, [I] row, status draft */
void frontendSelectMakeImage(int slot, uint16_t w, uint16_t h, uint16_t bpp, uint16_t colours)
{
    if (slot == 0) {
        zprimFreeAsset(g_feImg0);
        g_feImg0 = zprimCreateImage(w, h, bpp, colours);
    } else if (slot == 1) {
        zprimFreeAsset(g_feImg1);
        g_feImg1 = zprimCreateImage(w, h, bpp, colours);
    } else if (slot == 2) {
        zprimFreeAsset(g_feImg2);
        g_feImg2 = zprimCreateImage(w, h, bpp, colours);
    }
}

/* VA 0x004478b0, 221 bytes, [I] row, status draft */
void frontendCarouselReset(void)
{
    g_feCarouselCur = 0;
    g_feCarouselX[2] = 0;
    g_feCarouselX[1] = 0;
    g_feCarouselX[0] = 0;
    showId(SEL(0xD));
    hideId(SEL(0xE));
    hideId(SEL(0xF));
    guiSetPalette(guiFindElement(SEL(0xD)), 0);
    guiSetPalette(guiFindElement(SEL(0xE)), 0);
    guiSetPalette(guiFindElement(SEL(0xF)), 0);
    sprPos(SEL(0xD), 0, 0);
    sprPos(SEL(0xE), 0, 0);
    sprPos(SEL(0xF), 0, 0);
}

/* VA 0x004479d0, 389 bytes, [I] row, status draft */
void frontendSelectChangePage(int dir)
{
    /* dir > 0 forward, < 0 back; dir 2..4 jump (after a game: g_gameNextMode) */
    char buf[32];

    frontendTickerSet(NULL);
    hideId(SEL(2));
    g_feSelReady = g_feSelFrame + 2;
    if (g_feSelPage == 3 || (SELP == 0 && dir < 0) || (SELP == g_numPlayers - 1 && dir > 0)) {
        switch (g_feSelPage) {
        case 0: frontendRiderPageLeave(); break;
        case 1: frontendBikePageLeave(); break;
        case 2: frontendStylePageLeave(); break;
        case 3: frontendLevelPageLeave(); break;
        }
    }
    g_feSelPlayer = frontendWrap(g_feSelPlayer + dir, g_numPlayers);
    if (g_feSelPage == 3 || (SELP == 0 && dir > 0) || (SELP == g_numPlayers - 1 && dir < 0))
        g_feSelPage += dir;
    if (g_numPlayers > 1 && g_feSelPage < 3) {
        zstringPrintf(buf, zTextGet(0x3B), SELP + 1);            /* PLAYER %d */
        setText(SEL(2), buf);
    }
    g_feSelReady = g_feSelFrame + 2;
    switch (g_feSelPage) {
    case 0:  g_feTask = frontendRiderPageEnter; return;
    case 1:  g_feTask = frontendBikePageEnter; return;
    case 2:  g_feTask = frontendStylePageEnter; return;
    case 3:  g_feTask = frontendLevelPageEnter; return;
    case 4:  g_feTask = frontendSelectStart; return;
    case -1: g_feTask = frontendSelectExit; return;
    }
}

/* VA 0x00447bf0, 723 bytes, [I] row, status draft */
static int frontendSelectHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    if ((msg == GUI_MSG_BACK || msg == P2(GUI_MSG_BACK)) && g_guiAutoBack) {
        g_feSelPage = -1;
        g_feTask = frontendSelectExit;
        return 1;
    }
    /* who may press what: one player ignores pad-2 messages (and pad-2 accept is an error);
     * with two players each one's page only takes its own pad's messages */
    if (g_numPlayers == 2) {
        if (SELP == 0 && msg >= P2(GUI_MSG_UP) && msg <= P2(GUI_MSG_R2) && msg != P2(GUI_MSG_SQUARE))
            return 1;
        if (SELP == 1 && msg >= GUI_MSG_UP && msg <= GUI_MSG_R2 && msg != GUI_MSG_SQUARE)
            return 1;
    } else {
        switch (msg) {
        case P2(GUI_MSG_UP): case P2(GUI_MSG_DOWN): case P2(GUI_MSG_LEFT): case P2(GUI_MSG_RIGHT):
        case P2(GUI_MSG_CIRCLE): case P2(GUI_MSG_BACK): case P2(GUI_MSG_L1): case P2(GUI_MSG_R1):
        case P2(GUI_MSG_L2): case P2(GUI_MSG_R2):
            return 1;
        case P2(GUI_MSG_ACCEPT):
        case P2(GUI_MSG_START):
            SOUND(SND_ERROR);
            return 1;
        }
    }

    switch (msg) {
    case GUI_MSG_FRAME:
        g_feSelFrame++;
        if (g_feSelFrame % 216000 == 0) {                        /* an hour: restart the counters */
            g_feSelFrame = 0;
            g_feSelReady = 0;
            g_feLastChange = 0;
            frontendTickerSet(g_feTickerText);
        }
        if (g_feTask != NULL)
            g_feTask();
        if (g_feSelPending != 0 && g_feSelFrame - g_feLastChange > 2) {
            frontendSelectChangePage(g_feSelPending);
            g_feSelPending = 0;
        }
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        frontendSelectCheats((int16_t)msg);
        return 1;
    case GUI_MSG_DOWN:
    case GUI_MSG_CIRCLE:
    case GUI_MSG_L1:
    case GUI_MSG_R1:
    case GUI_MSG_L2:
    case GUI_MSG_R2:
    case P2(GUI_MSG_DOWN):
    case P2(GUI_MSG_CIRCLE):
    case P2(GUI_MSG_L1):
    case P2(GUI_MSG_R1):
    case P2(GUI_MSG_L2):
    case P2(GUI_MSG_R2):
        frontendSelectCheats((int16_t)msg);
        return 1;
    case GUI_MSG_LEFT:
    case P2(GUI_MSG_LEFT):
        if (g_feCarouselDir == 0 && g_feStep != NULL) {
            g_feStep(-1);
            frontendSelectCheats((int16_t)msg);
        }
        return 1;
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_RIGHT):
        if (g_feCarouselDir == 0 && g_feStep != NULL) {
            g_feStep(1);
            frontendSelectCheats((int16_t)msg);
        }
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feTask != NULL && g_feTask != frontendBikeSpin)
            return 1;
        if (g_feSelFrame <= g_feSelReady)
            return 1;
        if ((g_feSelPage == 1 && g_feSelBike[SELP] >= g_feBikeCount) ||
            (g_feSelPage == 2 && g_feSelStyle[SELP] >= g_feStyleCount) ||
            (g_feSelPage == 3 && g_feSelLevel >= g_feLevelCount)) {
            SOUND(SND_ERROR);                                    /* locked */
            return 1;
        }
        g_feSelPending = 1;
        SOUND(SND_SELECT);
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL && g_feTask != frontendBikeSpin)
            return 1;
        g_feSelPending = -1;
        SOUND(SND_SELECT);
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
}

/* VA 0x00448040, 51 bytes, [I] row, status draft */
void frontendTickerSet(const uint16_t *lines)
{
    g_feTickerRun = 1;
    g_feTickerText = lines;
    g_feTickerTop = 0;
    g_feTickerScrollY = 0;
    g_feTickerFill = 4;
    g_feTickerWait = 180;
}

static void tickerHalf(FeTickerHalf *h, const char *text)
{
    memset(h->text, 0, sizeof(h->text));
    if (text != NULL)
        strcpy(h->text, text);
    h->font = 0x8001;
    h->unk_00 = 0;
    h->unk_04 = 0;
}

/* VA 0x00448090, 1449 bytes, [I] row, status draft */
void frontendTickerScroll(int n, int at)
{
    /* Bring n more lines into the 5-line ticker ring (slot = top + k + at). A line is "#Y..."
     * (yellow) / "#b..." (blue) on the left, "#B..." (blue) / "#y..." (yellow) on the right, e.g.
     * "#YNickname: #BMiracle Boy"; a line without a left code stays empty. */
    char buf[100], *p, *left, *right;
    uint16_t lpal, rpal;
    int count = 0, k, idx, slot;

    while (g_feTickerText[count] != 0)
        count++;
    if (n < 1) {
        g_feTickerTop = (uint16_t)(g_feTickerTop + n);
        return;
    }
    for (k = 0; k < n; k++) {
        idx = frontendWrap(g_feTickerTop + k, count + 1);
        if (g_feTickerText[idx] == 0)
            buf[0] = EMPTY[0];
        else
            strcpy(buf, zTextGet(g_feTickerText[idx]));
        left = right = NULL;
        lpal = 1;
        rpal = 0;
        if (buf[0] != '\0') {
            for (p = buf; p[1] != '\0'; p++) {
                if (*p != '#')
                    continue;
                *p = '\0';
                switch (p[1]) {
                case 'Y': lpal = 1; left = p + 2; break;
                case 'b': lpal = 0; left = p + 2; break;
                case 'B': rpal = 0; right = p + 2; break;
                case 'y': rpal = 1; right = p + 2; break;
                }
                p++;
            }
        }
        slot = frontendWrap(g_feTickerTop + k + at, 5);
        if (left == NULL) {
            tickerHalf(&g_feTicker[slot].left, NULL);
            g_feTickerWidth[slot] = 0;
        } else {
            tickerHalf(&g_feTicker[slot].left, left);
            setText(SEL(0x17), left);                            /* measure it */
            g_feTickerWidth[slot] = (uint16_t)guiGetTextWidth(guiFindElement(SEL(0x17)));
        }
        g_feTickerPal[slot][0] = lpal;
        tickerHalf(&g_feTicker[slot].right, right);
        g_feTickerPal[slot][1] = rpal;
    }
    g_feTickerTop = (uint16_t)(g_feTickerTop + n);
}

/* VA 0x004487b0, 265 bytes, [I] row, status draft */
void frontendSelectTabsShow(void)
{
    int i;

    showId(SEL(0x16));
    hideId(SEL(6));
    for (i = 0; i < 4; i++)                     /* RIDER BIKE STYLE LEVEL, the current one bright */
        tint(SEL(0x1F + i), 0xFF, 0xFF, 0xFF, g_feSelPage == i ? 0xFF : 0x50);
}

/* VA 0x00448910, 288 bytes, [I] row, status draft */
void frontendSelectTabsHide(void)
{
    int i;

    hideId(SEL(0x16));
    showId(SEL(6));
    for (i = 7; i <= 0xC; i++)
        hideId(SEL(i));
    for (i = 0; i < 4; i++)
        tint(SEL(0x1F + i), 0xFF, 0xFF, 0xFF, 0x50);
}

/* VA 0x00448a80, 2398 bytes, [I] row, status draft */
static int frontendSelectDrawHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int offR, offB, i, r, r2, idx, x1;

    if (msg != GUI_MSG_DRAW_ELEM)
        return 0;
    switch (e->id) {
    case SEL(1): {                              /* medals: challenge tier, competition result */
        const SaveRiderRecord *rec;

        offR = (g_feSelPage == 0 && g_feCarouselDir != 0) ? -g_feCarouselDir : 0;
        rec = &RECORD(frontendWrap(RIDER + offR, g_saveBlockA.riders_unlocked));
        for (i = 0; i < 6; i++) {
            guiSetSpriteOffset(e, (int16_t)(i * 0x11), 0);
            switch (rec->goals[i].tier) {
            case 0: guiSetFrame(e, 0); break;
            case 1: guiSetFrame(e, 1); break;
            case 2: guiSetFrame(e, rec->goals[i].tier_goals_done == 4 ? 3 : 2); break;
            }
            guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
            guiSetSpriteOffset(e, (int16_t)(i * 0x11), 0x32);
            guiSetFrame(e, rec->comp_result[i]);
            guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
        }
        return 1;
    }
    case SEL(4):                                /* lines from the style picture to its icons */
        if (g_feTask == NULL && g_feSelStyle[SELP] < g_feStyleCount) {
            idx = RIDER * 5 + g_feSelStyle[SELP];
            zprimSetAttr(ZPRIM_ATTR_COLOUR, 0xDDDD);
            if (g_feStyleIcons[idx][0] != -1) {
                zprimDrawLine(0xF0, 0, 0x14A, 0);
                zprimDrawLine(0xF0, 1, 0x14A, 1);
                zprimDrawLine(0xDC, 0xF, 0xF0, 0);
                zprimDrawLine(0xDC, 0x10, 0xF0, 1);
            }
            if (g_feStyleIcons[idx][3] != -1) {
                zprimDrawLine(0x104, 0x32, 0x14A, 0x32);
                zprimDrawLine(0x104, 0x33, 0x14A, 0x33);
                zprimDrawLine(0xF0, 0x41, 0x104, 0x32);
                zprimDrawLine(0xF0, 0x42, 0x104, 0x33);
            }
            if (g_feStyleIcons[idx][1] != -1) {
                zprimDrawLine(0xE6, 100, 0x14A, 100);
                zprimDrawLine(0xE6, 0x65, 0x14A, 0x65);
            }
            if (g_feStyleIcons[idx][2] != -1) {
                zprimDrawLine(0x14A, 0x96, 0xF0, 0x96);
                zprimDrawLine(0x14A, 0x97, 0xF0, 0x97);
                return 1;
            }
        }
        return 1;
    case SEL(5):                                /* the style's icons */
        if (g_feTask == NULL && g_feSelStyle[SELP] < g_feStyleCount) {
            for (i = 0; i < 8; i++) {
                idx = RIDER * 5 + g_feSelStyle[SELP];
                if (g_feStyleIcons[idx][i] != -1) {
                    guiSetSprite(e, g_feStyleIconImg[g_feStyleIcons[idx][i]]);
                    guiSetSpriteOffset(e, (int16_t)g_feStyleIconPos[i][0], (int16_t)g_feStyleIconPos[i][1]);
                    guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
                }
            }
            return 1;
        }
        return 1;
    case SEL(0xD):
    case SEL(0xE):
    case SEL(0xF):                              /* the carousel pictures, clipped to the window */
        zprimSetClip(0x8C, 100, g_feClipW + 0x8C, g_feClipH + 100);
        guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
        zprimSetClip(0, 0, 0x27F, 0x1DF);
        return 1;
    case SEL(0x15):                             /* the frame around the window */
        frontendCursorDraw(0x80, 0x58, (int16_t)(g_feClipW + 0x98), (int16_t)(g_feClipH + 0x70), 0x80,
                           (int)g_feCursorFull);
        return 1;
    case SEL(0x16):                             /* the pulsing frame on the page tab */
        if (g_feCursorPhase < 0 || g_feCursorPhase > 0x40)
            g_feCursorStep = -g_feCursorStep;
        g_feCursorPhase += g_feCursorStep;
        frontendCursorDraw(0x1F, (int16_t)(g_feSelPage * 0x54 + 0x5A), 0x71, (int16_t)(g_feSelPage * 0x54 + 0xB7),
                           (uint8_t)(0x80 - g_feCursorPhase), 1);
        return 1;
    case SEL(0x17):                             /* the ticker */
        if (g_feTickerText == NULL)
            return 1;
        if (g_feTask != NULL && g_feTask != frontendBikeSpin)
            return 1;
        if (g_feTickerFill != 0) {
            if (g_feSelFrame % 5 == 0)
                g_feTickerFill--;
            if (g_feTickerFill == 0)
                frontendTickerScroll(5, 0);
        }
        if (g_feTickerWait == 0) {
            if (g_feSelFrame % 5 == 0) {
                g_feTickerScrollY = (uint16_t)(g_feTickerScrollY + g_feTickerRun);
                if (g_feTickerScrollY > 0x13) {
                    g_feTickerScrollY = 0;
                    frontendTickerScroll(1, 5);
                }
            }
        } else {
            g_feTickerWait--;
        }
        if (g_feTickerFill != 0)
            return 1;
        zprimSetClip(0x82, 0x146, 500, 0x18A);
        for (i = 0; i < 5; i++) {
            int s = frontendWrap(g_feTickerTop + i, 5);
            int16_t y = (int16_t)(i * 0x14 - g_feTickerScrollY + 0xD0);

            zprimSetFont(zprimLoadAsset(g_feTicker[s].left.font));
            zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, g_feTickerPal[s][0]);
            zprimDrawText((const uint8_t *)g_feTicker[s].left.text, 0x82, y);
            zprimSetFont(zprimLoadAsset(g_feTicker[s].right.font));
            zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, g_feTickerPal[s][1]);
            zprimDrawText((const uint8_t *)g_feTicker[s].right.text, g_feTickerWidth[s] + 0x82, y);
        }
        zprimSetClip(0, 0, 0x27F, 0x1DF);
        return 1;
    case SEL(0x1E):                             /* the four stat bars (+ the bike's bonus) */
        offR = (g_feSelPage == 0 && g_feCarouselDir != 0) ? -g_feCarouselDir : 0;
        offB = (g_feSelPage == 1 && g_feCarouselDir != 0) ? -g_feCarouselDir : 0;
        for (i = 0; i < 4; i++) {
            r = frontendWrap(RIDER + offR, g_saveBlockA.riders_unlocked);
            zprimSetClip(0x17C, 0x40, (g_feRiderStats[r][i] << 3) + 0x17C, 0x1DF);
            guiSetSpriteOffset(e, 0, (int16_t)(i * 0x19));
            guiSetPalette(e, 0);
            guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
            if (g_feSelPage == 1 && g_feStep == frontendBikeShow && g_feSelBike[SELP] + offB < g_feBikeCount) {
                r2 = RIDER;
                /* sic: the bonus is added to the unwrapped rider's stat */
                x1 = (g_feBikeStats[r * 5 + g_feSelBike[SELP] + offB][i] + g_feRiderStats[r2][i]) * 8 + 0x17C;
                zprimSetClip((g_feRiderStats[r][i] << 3) + 0x17C, 0x40, x1, 0x1DF);
                guiSetPalette(e, 1);
                guiSetSpriteOffset(e, (int16_t)(g_feRiderStats[r2 + offR][i] << 3), (int16_t)(i * 0x19));
                guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
            }
        }
        zprimSetClip(0, 0, 0x27F, 0x1DF);
        return 1;
    default:
        guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
        return 1;
    }
}

/* VA 0x004496a0, 444 bytes, [I] row, status draft */
void frontendCarouselAnimate(void)
{
    int k, s, step, d;

    for (k = 0; k < g_feCarouselN; k++) {
        step = g_feCarouselDir < 1 ? -g_feCarouselSpeed : g_feCarouselSpeed;
        s = frontendWrap(g_feCarouselCur + k, g_feCarouselN);
        g_feCarouselX[s] += step;
        showId(g_feCarouselSprite[s]);
        sprPos(g_feCarouselSprite[s], g_feCarouselX[s], 0);
        showId(g_feCarouselText[s]);
        textPos(g_feCarouselText[s], g_feCarouselX[s] * 3, 0);   /* the text moves 3x faster */
    }
    s = frontendWrap(g_feCarouselCur, g_feCarouselN);
    if ((g_feCarouselX[s] < 0 ? -g_feCarouselX[s] : g_feCarouselX[s]) >= (int)g_feCarouselDist) {
        g_feTask = NULL;
        d = -g_feCarouselDir;
        g_feCarouselCur = frontendWrap(g_feCarouselCur + d, g_feCarouselN);
        g_feCarouselDir = 0;
        if (g_feSlideDone != NULL)
            g_feSlideDone(d);
    }
}

/* VA 0x004498d0, 271 bytes, [I] row, status draft */
void frontendSelectSlideIn(void)
{
    textPos(SEL(0x10), g_feTab0 * 6 - 0xF8, 0);
    textPos(SEL(0x11), g_feTab0 * -6 + 0xF8, 0);
    if (g_gameNextMode == 1) {
        if (g_feClipH < 0x80)
            g_feClipH += 8;
    } else if (g_gameNextMode == 3) {
        if (g_feClipH > 0xB3)
            g_feClipH = 0xB4;
        else
            g_feClipH += 8;
    } else if (g_feClipH < 0xC0) {
        g_feClipH += 8;
    }
    if (frontendTabsIn(SEL(0x1F), SEL(0x20), SEL(0x21), SEL(0x22))) {
        /* back from a game: reopen the page it came from */
        switch (g_gameNextMode) {
        case 1:  frontendSelectChangePage(2); break;              /* bike */
        case 2:  frontendSelectChangePage(3); break;              /* style */
        case 3:  frontendSelectChangePage(4); break;              /* level */
        default: frontendSelectChangePage(1); break;              /* rider */
        }
        g_gameNextMode = 0;
    }
}

/* VA 0x00449a30, 56 bytes, [I] row, status draft */
void frontendSelectSlideOut(void)
{
    if (frontendTabsOut(SEL(0x1F), SEL(0x20), SEL(0x21), SEL(0x22))) {
        g_feTask = frontendSelectExit;
        g_feSelReady = g_feSelFrame + 2;
    }
}

/* VA 0x00449a80, 1690 bytes, [I] row, status draft */
void frontendSelectInit(void)
{
    static const uint32_t custom[] = { SEL(0xD), SEL(0xE), SEL(0xF), SEL(0x15), SEL(0x16), SEL(0x17),
                                       SEL(0x1E), SEL(5), SEL(4), SEL(1) };
    char buf[32];
    uint32_t id;
    int i;

    g_feTickerScrollY = 0;
    g_feTickerRun = 1;
    if (g_gameNextMode == 0) {
        g_feSelRider[0] = g_feSelRider[1] = 0;
        g_feSelBike[0] = g_feSelBike[1] = 0;
        g_feSelStyle[0] = g_feSelStyle[1] = 0;
        g_feSelLevel = 0;
    } else {                                                     /* back from a game */
        g_feSelRider[0] = g_saveProfiles[0].rider;
        g_feSelRider[1] = g_saveProfiles[1].rider;
        g_feSelBike[0] = g_saveProfiles[0].bike;
        g_feSelStyle[0] = g_saveProfiles[0].style;
        g_feSelBike[1] = g_saveProfiles[1].bike;
        g_feSelStyle[1] = g_saveProfiles[1].style;
        g_feSelLevel = g_currentLevel;
    }
    memset(g_feCheatKeys, 0, sizeof(g_feCheatKeys));
    g_feLastChange = 0;
    g_feCheatPos = 0;
    g_feImg0 = NULL;
    g_feImg1 = NULL;
    g_feCarouselDir = 0;
    g_feSelPage = -1;
    g_feSelPending = 0;
    g_feSelUnk60 = 4;
    g_feSelFrame = 0;
    g_feSelReady = 0;
    g_feSelUnkA0 = 0;
    g_feCarouselSpeed = 0x10;
    g_feCarouselN = 2;
    g_feCursorFull = 1;
    g_feSelPlayer = -1;
    setText(SEL(6), zTextGet(0x3C));                             /* LOADING */
    setText(SEL(8), zTextGet(0x3E));                             /* CHANGE */
    setText(SEL(0x18), zTextGet(0xD1));                          /* air */
    setText(SEL(0x19), zTextGet(0xD2));                          /* speed */
    setText(SEL(0x1A), zTextGet(0xD3));                          /* spins */
    setText(SEL(0x1B), zTextGet(0xD4));                          /* balance */
    setText(SEL(0x1C), zTextGet(0x2E));                          /* challenges */
    setText(SEL(0x1D), zTextGet(0x2D));                          /* medals */
    switch (g_gameNextMode) {
    case 1:
        g_feCarouselDist = 0x80; g_feClipW = 0x80; g_feClipH = 0x80;
        setText(SEL(0x10), zTextGet(0xD8));
        id = 0xD9;                                               /* BIKE */
        break;
    case 2:
        g_feCarouselDist = 0x80; g_feClipW = 0x80; g_feClipH = 0xC0;
        setText(SEL(0x10), zTextGet(0xDA));
        id = 0xDB;                                               /* STYLE */
        break;
    case 3:
        g_feCarouselDist = 0x78; g_feClipW = 0xF0; g_feClipH = 0xB4;
        setText(SEL(0x10), zTextGet(0xDC));
        id = 0xDD;                                               /* LEVEL */
        break;
    default:
        g_feCarouselDist = 0x80; g_feClipW = 0x80; g_feClipH = 0x20;
        setText(SEL(0x10), zTextGet(0xD6));
        id = 0xD7;                                               /* RIDER */
        break;
    }
    setText(SEL(0x11), zTextGet(id));
    strcpy(buf, zTextGet(0xD6));                                 /* "SELECT RIDER" */
    strcat(buf, " ");
    strcat(buf, zTextGet(0xD7));
    setText(SEL(0x12), buf);
    setText(SEL(0x14), EMPTY);
    for (i = 0x10; i <= 0x14; i++)
        textPal(SEL(i), 1);
    for (i = 0; i < (int)(sizeof(custom) / sizeof(custom[0])); i++)
        guiFindElement(custom[i])->handler = frontendSelectDrawHandler;
    g_feImg0 = zprimCreateImage(0, 0, 8, 0x100);
    g_feImg1 = zprimCreateImage(0, 0, 8, 0x100);
    g_feImg2 = zprimCreateImage(0, 0, 8, 0x100);
    guiSetSprite(guiFindElement(SEL(0xD)), g_feImg0);
    guiSetSprite(guiFindElement(SEL(0xE)), g_feImg1);
    guiSetSprite(guiFindElement(SEL(0xF)), g_feImg2);
    frontendCarouselReset();
    hideId(SEL(0xF));
    hideId(SEL(0xE));
    hideId(SEL(0xD));
    frontendCursorCreate();
    frontendTabsReset();
    frontendTabsIn(SEL(0x1F), SEL(0x20), SEL(0x21), SEL(0x22));
    textPos(SEL(0x10), g_feTab0 * 6 - 0xF8, 0);
    textPos(SEL(0x11), g_feTab0 * -6 + 0xF8, 0);
    frontendTickerSet(NULL);
    g_feTask = frontendSelectSlideIn;
}

/* ================================================================== cool stuff (0xA00D) */

/* a movie: a title and the player's description (ngappPlayMovie plays desc[0]: its path with the
 * extension replaced by "AVI"; bin\movie is empty in this install) */
typedef struct FeMovieDesc {
    const char *path;            /* "bin\\movie\\Intro.str" */
    int32_t width;               /* 320 */
    int32_t height;              /* 240 */
    int32_t frames;
    int32_t fps;
    int32_t flags;
} FeMovieDesc;                   /* 0x18 */
typedef struct FeMovie {
    char        title[100];      /* +0x00 */
    FeMovieDesc desc;            /* +0x64 */
} FeMovie;                       /* 0x7C */
extern FeMovie g_feMovies[4];    /* 0x0055AF40 INTRO MOVIE, DAVE MIRRA, RYAN NYQUIST, AMISH BOY */

typedef struct FeTextLine {
    int32_t  unk_00;             /* +0x00 */
    int32_t  y;                  /* +0x04 */
    uint16_t font;               /* +0x08 0x8001 */
    uint16_t pad_0a;
    uint32_t palBlink;           /* +0x0C */
    uint32_t pal;                /* +0x10 10 = the best of all levels */
    char     text[100];          /* +0x14 */
} FeTextLine;                    /* 0x78 */

extern const uint16_t g_feGoalTexts[6][3][4];   /* 0x0055B138 first of the 3 lines of each goal (0x2089 ..) */
const uint16_t g_feTierNames[3] = { 0x30, 0x31, 0x32 };   /* 0x0055B130 AMATEUR / PRO / HARDCORE CHALLENGES */

int32_t   g_feCoolSel;            /* 0x00691214 0 challenges, 1 high scores, 2 movies, 3 credits, -1 back */
uint32_t  g_feCoolReady;          /* 0x00691228 */
uint32_t  g_feCoolDelay;          /* 0x00691700 */
FeMovie  *g_feMovieList[4];       /* 0x0068FFF0 */
uint16_t  g_feMovieCount;         /* 0x006916F0 */
uint16_t  g_feMovieSel;           /* 0x00574644 */
uint32_t  g_feMoviePlayed;        /* 0x00574648 */
int16_t   g_feMovieDelay;         /* 0x00690896 */
int16_t   g_feMovieX[4];          /* 0x00691230 */
ZstringSet *g_feLevelSet;         /* 0x0057464C the level text set (0x7D01 / 0x7D05 + language) */
uint32_t  g_feLevelSetSize;       /* 0x00690890 */
int       g_feLevelSetHandle;     /* 0x00691238 */
ZprimAsset *g_feRecThumb;         /* 0x0069123C */
FeTextLine g_feRecLabel[8];       /* 0x00690990 */
FeTextLine g_feRecScore[8];       /* 0x006904D0 */
FeTextLine g_feRecRider[8];       /* 0x00690E50 */
FeTextLine g_feRecName[8];        /* 0x00691330 */
int16_t   g_feRecX[4];            /* 0x006916F8 */
uint16_t  g_feRecBlink;           /* 0x006916F2 (never changed) */
uint32_t  g_feRecFlag;            /* 0x00690894 */
ZprimAsset *g_feChalThumb;        /* 0x00691210 */
uint8_t   g_feChalTier;           /* 0x00691218 */
int16_t   g_feChalX[4];           /* 0x00691220 */
uint32_t  g_feChalFrames;         /* 0x0069122C */
uint32_t  g_feChalFlag;           /* 0x00690898 */

static int frontendCoolStuffHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendMoviesHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendMoviesDraw(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendRecordsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
static int frontendChallengesHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);

static void coolTabsHide(void)
{
    hideId(COOL(9));
    hideId(COOL(0xB));
    hideId(COOL(0xD));
    hideId(COOL(0xF));
    hideId(COOL(8));
    hideId(COOL(0xA));
    hideId(COOL(0xC));
    hideId(COOL(0xE));
}

static void coolTabText(void)
{
    textPos(COOL(9), (40 - g_feTab0) * 4, 0);
    textPos(COOL(0xB), (40 - g_feTab1) * 4, 0);
    textPos(COOL(0xD), (40 - g_feTab2) * 4, 0);
    textPos(COOL(0xF), (40 - g_feTab3) * 4, 0);
}

/* load the language's STRS set 'base' + language into a DMM block and register it */
static void loadLevelSet(uint32_t base)
{
    g_feLevelSet = NULL;
    zassetFindAsset(g_miscDataZal, base + g_saveBlockA.language, 0);
    g_feLevelSetSize = zassetGetAssetSize(g_miscDataZal);
    if (g_feLevelSetSize != 0) {
        g_feLevelSetHandle = zmemDMMAlloc(g_feLevelSetSize);
        g_feLevelSet = (ZstringSet *)zmemDMMLock(g_feLevelSetHandle);
        zassetLoadAsset(g_miscDataZal, g_feLevelSet);
        zstringAddSet(g_feLevelSet);
    }
}

/* "GREENVILLE - VAIL TRAILS" -> COOL(5) "GREENVILLE ", COOL(6) "VAIL TRAILS" */
static void showLevelName(ZprimAsset *thumb)
{
    char buf[100], *second = buf;
    int i;

    strcpy(buf, zTextGet(g_feLevelNames[g_currentLevel]));
    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '-') {
            buf[i] = '\0';
            second = &buf[i + 2];
            break;
        }
    }
    setText(COOL(5), buf);
    setText(COOL(6), second);
    zprimCopyMaterial(zprimLoadAsset(g_feLevelThumbs[g_currentLevel]), thumb);
}

/* is level 'level''s record 'id' the highest of all 12 levels? */
static int bestOfAllLevels(uint32_t id)
{
    SaveRecordEntry *rec = comdataGetRecordPtr(id), *other;
    uint8_t saved = g_currentLevel;
    int lvl, best = 1;

    for (lvl = 0; lvl < 12; lvl++) {
        g_currentLevel = (uint8_t)lvl;
        other = comdataGetRecordPtr(id);
        if (rec->value <= other->value && lvl != saved) {
            best = 0;
            break;
        }
    }
    g_currentLevel = saved;
    return best;
}

/* ---- movies ---- */

/* VA 0x0044a2d0, 374 bytes, [I] row, status draft */
void frontendMoviePlay(void)
{
    const void *playlist[2];

    playlist[0] = &g_feMovieList[g_feMovieSel]->desc;
    playlist[1] = NULL;
    if (g_feMovieDelay != 0) {
        g_feMovieDelay--;
        return;
    }
    guiShutdownMenuMode();
    bmxsoundEvent(SND_MUSIC, 0, 0, 0, 0);
    ngappPlayMovie(playlist);
    bmxsoundEvent(SND_MUSIC, 1, 0, 0, 0);
    setVolumes();
    guiInitializeMenuMode();
    guiLoadBackdropImage(0xA011, 8);
    guiLoadScreen(SCR_COOL, frontendMoviesHandler);
    frontendCoolStuffReset();
    coolTabsHide();
    g_feCoolSel = 2;
    g_feTask = frontendMoviesEnter;
}

/* VA 0x0044a4b0, 380 bytes, [I] row, status draft */
void frontendMoviesEnter(void)
{
    /* INTRO MOVIE always; a rider's movie when his career flag bit 0 ("all levels complete",
     * reward type 8) is set: Dave Mirra (rider 0), Ryan Nyquist (1), Amish Boy (11) */
    int n = 1;

    g_feMovieX[0] = g_feMovieX[1] = g_feMovieX[2] = g_feMovieX[3] = 0;
    if (!g_feMoviePlayed)
        g_feMovieSel = 0;
    g_feMovieDelay = 0;
    g_feMoviePlayed = 0;
    g_feMovieList[0] = &g_feMovies[0];
    g_feMovieCount = 1;
    if (g_saveBlockA.riders[0].flags & 1) {
        g_feMovieList[n++] = &g_feMovies[1];
        g_feMovieCount = (uint16_t)n;
    }
    if (g_saveBlockA.riders[1].flags & 1) {
        g_feMovieList[n++] = &g_feMovies[2];
        g_feMovieCount = (uint16_t)n;
    }
    if (g_saveBlockA.riders[11].flags & 1) {
        g_feMovieList[n++] = &g_feMovies[3];
        g_feMovieCount = (uint16_t)n;
    }
    showId(COOL(7));
    guiSetScreenHandler(frontendMoviesHandler);
    guiFindElement(COOL(7))->handler = frontendMoviesDraw;
    g_feTask = frontendMoviesSlideIn;
    hideId(COOL(0x15));
    hideId(COOL(0x14));
    coolTabsHide();
}

/* VA 0x0044a690, 221 bytes, [I] row, status draft */
void frontendMoviesLeave(void)
{
    frontendTabsReset();
    frontendTabsIn(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE));
    hideId(COOL(7));
    guiSetScreenHandler(frontendCoolStuffHandler);
    guiFindElement(COOL(7))->handler = guiElementHandler;
    hideId(COOL(0x11));
    hideId(COOL(0x10));
    hideId(COOL(0x13));
    hideId(COOL(0x12));
    hideId(COOL(0x15));
    hideId(COOL(0x14));
    hideId(COOL(0x17));
    hideId(COOL(0x16));
    g_feTask = frontendCoolStuffTabsIn;
}

/* VA 0x0044a7b0, 139 bytes, [I] row, status draft */
void frontendMoviesSlideIn(void)
{
    int i;

    g_feMovieX[0] = (int16_t)(g_feMovieX[0] + (500 - g_feMovieX[0]) / 4);
    if (g_feMovieX[0] > 0x1CC)
        g_feMovieX[0] = 0x1CC;
    for (i = 1; i < 4; i++) {
        if (g_feMovieX[i - 1] > 0x104)
            g_feMovieX[i] = (int16_t)(g_feMovieX[i] + (500 - g_feMovieX[i]) / 4);
        if (g_feMovieX[i] > 0x1CC)
            g_feMovieX[i] = 0x1CC;
    }
    if (g_feMovieX[3] > 0x1CB)
        g_feTask = NULL;
}

/* VA 0x0044a870, 162 bytes, [I] row, status draft */
void frontendMoviesSlideOut(void)
{
    int i;

    g_feMovieX[0] = (int16_t)(g_feMovieX[0] - (500 - g_feMovieX[0]) / 4);
    if (g_feMovieX[0] < 0)
        g_feMovieX[0] = 0;
    for (i = 1; i < 4; i++) {
        if (g_feMovieX[i - 1] < 0x1A4)
            g_feMovieX[i] = (int16_t)(g_feMovieX[i] - (500 - g_feMovieX[i]) / 4);
        if (g_feMovieX[i] < 0)
            g_feMovieX[i] = 0;
    }
    if (g_feMovieX[3] < 1) {
        frontendMoviesLeave();
        if (g_feMoviePlayed) {
            g_feMovieDelay = 2;
            g_feTask = frontendMoviePlay;
        }
    }
}

/* VA 0x0044a950, 169 bytes, [I] row, status draft */
static int frontendMoviesDraw(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    uint32_t i;

    if (msg != GUI_MSG_DRAW_ELEM || e->id != COOL(7) || g_feMovieCount == 0)
        return 0;
    for (i = 0; i < g_feMovieCount; i++) {
        guiSetText(e, g_feMovieList[i]->title);
        guiSetTextPos(e, (int16_t)(0x200 - g_feMovieX[i]), (int16_t)(i * 0x23));
        guiSetTextPalette(e, g_feMovieSel == i);
        guiElementHandler(GUI_MSG_DRAW_ELEM, e, a, b);
    }
    return 0;
}

/* VA 0x0044aa30, 300 bytes, [I] row, status draft */
static int frontendMoviesHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (g_feMovieSel == 0)
            return 1;
        g_feMovieSel--;
        SOUND(SND_MOVE);
        return 1;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if ((int)g_feMovieSel >= g_feMovieCount - 1)
            return 1;
        g_feMovieSel++;
        SOUND(SND_MOVE);
        return 1;
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_LEFT):
    case P2(GUI_MSG_RIGHT):
        return 1;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feTask != NULL)
            return 1;
        g_feMoviePlayed = 1;
        break;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        if (g_guiAutoBack) {
            frontendMoviesLeave();
            if (g_feMoviePlayed) {
                g_feMovieDelay = 2;
                g_feTask = frontendMoviePlay;
            }
            return 1;
        }
        break;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    SOUND(SND_SELECT);
    g_feTask = frontendMoviesSlideOut;
    return 1;
}

/* ---- high scores ---- */

/* VA 0x0044ac00, 438 bytes, [I] row, status draft */
void frontendRecordsEnter(void)
{
    int i;

    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    zprimLoadGroup(3, 0xE00A);                                   /* level thumbnails */
    loadLevelSet(0x7D01);
    g_feRecThumb = zprimCreateImage(0xA6, 0x80, 8, 0x100);
    g_currentLevel = 0;
    for (i = 0; i < 8; i++) {                                    /* stat labels: stats 2..9 */
        g_feRecLabel[i].font = 0x8001;
        g_feRecLabel[i].unk_00 = 0;
        g_feRecLabel[i].y = i * 0x16;
        strcpy(g_feRecLabel[i].text, zTextGet(0x1094 + 2 + i));
    }
    frontendRecordsBuild();
    g_feRecX[0] = g_feRecX[1] = g_feRecX[2] = g_feRecX[3] = 0;
    g_feRecFlag = 2;
    textPal(COOL(5), 1);
    textPal(COOL(6), 1);
    guiFindElement(COOL(3))->handler = frontendRecordsHandler;
    guiFindElement(COOL(4))->handler = frontendRecordsHandler;
    g_feRecBlink = 0;
    guiSetScreenHandler(frontendRecordsHandler);
    g_feTask = frontendRecordsSlideIn;
}

/* VA 0x0044ae30, 207 bytes, [I] row, status draft */
void frontendRecordsLeave(void)
{
    zprimFree(g_feRecThumb);
    if (g_feLevelSet != NULL) {
        zstringRemoveSet(g_feLevelSet);
        zmemDMMUnlock(g_feLevelSetHandle);
        zmemDMMDealloc(g_feLevelSetHandle);
        g_feLevelSetHandle = 0;
        g_feLevelSet = NULL;
    }
    frontendTabsReset();
    frontendTabsIn(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE));
    showId(COOL(5));
    showId(COOL(6));
    showId(COOL(3));
    showId(COOL(4));
    guiSetScreenHandler(frontendCoolStuffHandler);
    zprimUnloadGroup(3);
    g_feTask = frontendCoolStuffTabsIn;
}

static void recordLine(FeTextLine *t, int y, uint32_t palBlink, uint32_t pal, const char *text)
{
    t->font = 0x8001;
    t->unk_00 = 0;
    t->y = y;
    t->palBlink = palBlink;
    t->pal = pal;
    strcpy(t->text, text);
}

/* VA 0x0044af40, 801 bytes, [I] row, status draft */
void frontendRecordsBuild(void)
{
    /* the level's records of stats 2..9 (block B of SAVE.DAT, entries 16..23); the ones that are
     * the best of all twelve levels get palette 10 */
    SaveRecordEntry *rec;
    char buf[32];
    int row, y, pal, inches;

    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    showLevelName(g_feRecThumb);
    for (row = 0; row < 8; row++) {
        y = row * 0x16;
        rec = comdataGetRecordPtr((uint32_t)(row + 2) << 16);
        if (y < 0x2C || y > 0x9A) {
            zstringPrintf(buf, "  %6ld", (long)rec->value);         /* points (trick set, single trick) */
        } else {
            inches = (int)(rec->value / 2.54f);                      /* [I] cm -> feet ` inches ~ */
            zstringPrintf(buf, " %3ld` %2ld~", (long)(inches / 12), (long)(inches % 12));
        }
        pal = bestOfAllLevels((uint32_t)(row + 2) << 16) ? 10 : 2;
        recordLine(&g_feRecScore[row], y, 2, (uint32_t)pal, buf);
        recordLine(&g_feRecName[row], y, 2, (uint32_t)pal, rec->name);
        recordLine(&g_feRecRider[row], y, 0, (uint32_t)pal, zTextGet(g_feRiderNames[rec->rider]));
        g_feRecRider[row].unk_00 = 2;
    }
    g_feTask = NULL;
}

/* VA 0x0044b340, 218 bytes, [I] row, status draft */
void frontendRecordsSlideIn(void)
{
    int i;

    showId(COOL(5));
    showId(COOL(6));
    showId(COOL(3));
    showId(COOL(4));
    g_feRecX[0] = (int16_t)(g_feRecX[0] + (500 - g_feRecX[0]) / 6);
    if (g_feRecX[0] > 0x1D6)
        g_feRecX[0] = 0x1D6;
    for (i = 1; i < 4; i++) {
        if (g_feRecX[i - 1] > 0x122)
            g_feRecX[i] = (int16_t)(g_feRecX[i] + (500 - g_feRecX[i]) / 6);
        if (g_feRecX[i] > 0x1D6)
            g_feRecX[i] = 0x1D6;
    }
    if (g_feRecX[3] > 0x1D5)
        g_feTask = NULL;
}

/* VA 0x0044b460, 209 bytes, [I] row, status draft */
void frontendRecordsSlideOut(void)
{
    int i;

    g_feRecX[0] = (int16_t)(g_feRecX[0] + (500 - g_feRecX[0]) / -6);
    if (g_feRecX[0] < 0)
        g_feRecX[0] = 0;
    for (i = 1; i < 4; i++) {
        if (g_feRecX[i - 1] < 0x1A4)
            g_feRecX[i] = (int16_t)(g_feRecX[i] - (500 - g_feRecX[i]) / 6);
        if (g_feRecX[i] < 0)
            g_feRecX[i] = 0;
    }
    if (g_feRecX[3] < 1)
        g_feTask = frontendRecordsLeave;
    hideId(COOL(0x17));
    hideId(COOL(0x16));
    hideId(COOL(0x15));
    hideId(COOL(0x14));
}

/* VA 0x0044b580, 1408 bytes, [I] row, status draft */
static int frontendRecordsHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    SaveRecordEntry *rec;
    char buf[32];
    uint32_t pal;
    int best, row;

    switch (msg) {
    case GUI_MSG_DRAW_ELEM:
        if (e->id == COOL(3)) {                 /* thumbnail, "total score" and the stat labels */
            guiSetText(e, zTextGet(0x1094));
            guiSetTextPos(e, (int16_t)(0x2C8 - g_feRecX[1]), 0x19);
            guiSetTextPalette(e, 0);
            guiElementHandler(msg, e, a, b);
            zprimSetSprite(g_feRecThumb);
            zprimSetAttr(ZPRIM_ATTR_SPR_ARG30, 0);
            zprimDrawSprite(0, (int16_t)(g_feRecX[0] - 0x1A2), -0x30);
            for (row = 0; row < 8; row++) {
                guiSetText(e, g_feRecLabel[row].text);
                guiSetTextPos(e, (int16_t)(0x200 - g_feRecX[2]), (int16_t)(g_feRecLabel[row].y + 0x6E));
                guiElementHandler(msg, e, a, b);
            }
            return 0;
        }
        if (e->id == COOL(4)) {                 /* the total score record, then the 8 rows */
            rec = comdataGetRecordPtr(0);
            best = bestOfAllLevels(0);
            pal = best ? (uint32_t)((~g_feRecBlink & 4 | 8) >> 2) : 2;
            guiSetText(e, rec->name);
            guiSetTextPos(e, (int16_t)(0x2C8 - g_feRecX[1]), 0x2D);
            guiSetTextPalette(e, pal);
            guiElementHandler(msg, e, a, b);
            guiSetText(e, zTextGet(g_feRiderNames[rec->rider]));
            guiSetTextPos(e, (int16_t)(0x2C8 - g_feRecX[1]), 0x41);
            guiSetTextPalette(e, pal);
            guiElementHandler(msg, e, a, b);
            zstringPrintf(buf, "%ld", (long)rec->value);
            guiSetTextFlags(e, GUIT_RIGHT | GUIT_MODE3);
            guiSetText(e, buf);
            guiSetTextPos(e, (int16_t)(0x3B3 - g_feRecX[1]), 0x19);
            guiSetTextPalette(e, pal);
            guiElementHandler(msg, e, a, b);
            guiSetTextFlags(e, GUIT_MODE3);
            for (row = 0; row < 8; row++) {
                zprimSetAttr(ZPRIM_ATTR_SPR_ARG30, ((g_feRecBlink & 4) >> 2) * 8 + 2);
                guiSetText(e, g_feRecName[row].text);
                guiSetTextPos(e, (int16_t)(0x27C - g_feRecX[1]), (int16_t)(g_feRecName[row].y + 0x6E));
                guiSetTextPalette(e, (g_feRecBlink & 4) ? g_feRecName[row].palBlink : g_feRecName[row].pal);
                guiElementHandler(msg, e, a, b);
                guiSetText(e, g_feRecRider[row].text);
                guiSetTextPos(e, (int16_t)(0x2EC - g_feRecX[1]), (int16_t)(g_feRecRider[row].y + 0x6E));
                guiSetTextPalette(e, (g_feRecBlink & 4) ? g_feRecRider[row].palBlink : g_feRecRider[row].pal);
                guiElementHandler(msg, e, a, b);
                guiSetText(e, g_feRecScore[row].text);
                guiSetTextPos(e, (int16_t)(0x37C - g_feRecX[1]), (int16_t)(g_feRecScore[row].y + 0x6E));
                guiSetTextPalette(e, (g_feRecBlink & 4) ? g_feRecScore[row].palBlink : g_feRecScore[row].pal);
                guiElementHandler(msg, e, a, b);
            }
        }
        return 0;
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        textPos(COOL(5), 0x2C8 - g_feRecX[0], -0x32);
        textPos(COOL(6), 0x2C8 - g_feRecX[0], -0x32);
        frontendCursorDraw((int16_t)(g_feRecX[0] - 0x1AE), 0x3C, (int16_t)(g_feRecX[0] - 0xF0), 0xD4, 0x80, 1);
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case GUI_MSG_DOWN:
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_UP):
    case P2(GUI_MSG_DOWN):
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        return 1;
    case GUI_MSG_LEFT:
    case P2(GUI_MSG_LEFT):
        if (g_feTask == NULL) {
            g_currentLevel = (uint8_t)frontendWrap(g_currentLevel - 1, 12);
            g_feCoolDelay = 2;
            SOUND(SND_MOVE);
            g_feTask = frontendRecordsBuild;
            return 1;
        }
        return 0;
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_RIGHT):
        if (g_feTask == NULL) {
            g_currentLevel = (uint8_t)frontendWrap(g_currentLevel + 1, 12);
            g_feCoolDelay = 2;
            g_feTask = frontendRecordsBuild;
            SOUND(SND_MOVE);
            return 1;
        }
        return 0;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        g_feRecFlag = 2;
        if (!g_guiAutoBack)
            SOUND(SND_SELECT);
        g_feTask = frontendRecordsSlideOut;
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
}

/* ---- challenges ---- */

/* VA 0x0044bcd0, 366 bytes, [I] row, status draft */
void frontendChallengesEnter(void)
{
    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    zprimLoadGroup(3, 0xE00A);
    loadLevelSet(0x7D05);                                        /* the single-player texts (goals) */
    g_feChalThumb = zprimCreateImage(0xA6, 0x80, 8, 0x100);
    g_currentLevel = 0;
    g_feChalTier = 0;
    frontendChallengesBuild();
    g_feChalX[0] = g_feChalX[1] = g_feChalX[2] = g_feChalX[3] = 0;
    g_feChalFlag = 2;
    textPal(COOL(5), 1);
    textPal(COOL(6), 1);
    guiFindElement(COOL(1))->handler = frontendChallengesHandler;
    guiFindElement(COOL(2))->handler = frontendChallengesHandler;
    hideId(COOL(0x13));
    hideId(COOL(0x12));
    g_feChalFrames = 0;
    guiSetScreenHandler(frontendChallengesHandler);
    g_feTask = frontendChallengesSlideIn;
}

/* VA 0x0044bea0, 151 bytes, [I] row, status draft */
void frontendChallengesLeave(void)
{
    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    zprimFree(g_feChalThumb);
    if (g_feLevelSet != NULL) {
        zstringRemoveSet(g_feLevelSet);
        zmemDMMUnlock(g_feLevelSetHandle);
        zmemDMMDealloc(g_feLevelSetHandle);
        g_feLevelSetHandle = 0;
    }
    frontendTabsReset();
    frontendTabsIn(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE));
    guiSetScreenHandler(frontendCoolStuffHandler);
    zprimUnloadGroup(3);
    g_feTask = frontendCoolStuffTabsIn;
}

/* VA 0x0044bf70, 233 bytes, [I] row, status draft */
void frontendChallengesBuild(void)
{
    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    showLevelName(g_feChalThumb);
    g_feTask = NULL;
}

/* VA 0x0044c0a0, 218 bytes, [I] row, status draft */
void frontendChallengesSlideIn(void)
{
    int i;

    showId(COOL(5));
    showId(COOL(6));
    showId(COOL(2));
    showId(COOL(1));
    g_feChalX[0] = (int16_t)(g_feChalX[0] + (500 - g_feChalX[0]) / 6);
    if (g_feChalX[0] > 0x1D6)
        g_feChalX[0] = 0x1D6;
    for (i = 1; i < 4; i++) {
        if (g_feChalX[i - 1] > 0x122)
            g_feChalX[i] = (int16_t)(g_feChalX[i] + (500 - g_feChalX[i]) / 6);
        if (g_feChalX[i] > 0x1D6)
            g_feChalX[i] = 0x1D6;
    }
    if (g_feChalX[3] > 0x1D5)
        g_feTask = NULL;
}

/* VA 0x0044c1c0, 358 bytes, [I] row, status draft */
void frontendChallengesSlideOut(void)
{
    int i;

    g_feChalX[0] = (int16_t)(g_feChalX[0] + (500 - g_feChalX[0]) / -6);
    if (g_feChalX[0] < 0)
        g_feChalX[0] = 0;
    for (i = 1; i < 4; i++) {
        if (g_feChalX[i - 1] < 0x1A4)
            g_feChalX[i] = (int16_t)(g_feChalX[i] - (500 - g_feChalX[i]) / 6);
        if (g_feChalX[i] < 0)
            g_feChalX[i] = 0;
    }
    if (g_guiAutoBack || g_feChalX[3] < 1) {
        hideId(COOL(5));
        hideId(COOL(6));
        hideId(COOL(2));
        hideId(COOL(1));
        g_feCoolDelay = 2;
        g_feTask = frontendChallengesLeave;
    }
    hideId(COOL(0x11));
    hideId(COOL(0x10));
    hideId(COOL(0x13));
    hideId(COOL(0x12));
    hideId(COOL(0x15));
    hideId(COOL(0x14));
    hideId(COOL(0x17));
    hideId(COOL(0x16));
}

/* VA 0x0044c390, 1227 bytes, [I] row, status draft */
static int frontendChallengesHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    char buf[200];
    uint16_t t;
    int g;

    switch (msg) {
    case GUI_MSG_DRAW_ELEM:
        if (e->id == COOL(1)) {                 /* thumbnail and the tier's four goals */
            zprimSetSprite(g_feChalThumb);
            zprimSetAttr(ZPRIM_ATTR_SPR_ARG30, 0);
            zprimDrawSprite(0, (int16_t)(g_feChalX[0] - 0x1A2), -0x30);
            for (g = 0; g < 4; g++) {
                t = g_feGoalTexts[g_currentLevel][g_feChalTier][g];   /* 3 lines: t, t + 1, t + 2 */
                strcpy(buf, zTextGet(t));
                strcat(buf, " ");
                strcat(buf, zTextGet(t + 1));
                strcat(buf, " ");
                strcat(buf, zTextGet(t + 2));
                strcat(buf, " ");
                guiSetText(e, buf);
                guiSetTextPos(e, (int16_t)(0x200 - g_feChalX[1]), (int16_t)((g * 5 + 0x23) * 4));
                guiSetTextPalette(e, 2);
                guiElementHandler(msg, e, a, b);
            }
            return 0;
        }
        if (e->id == COOL(2)) {                 /* AMATEUR / PRO / HARDCORE CHALLENGES */
            guiSetText(e, zTextGet(g_feTierNames[g_feChalTier]));
            guiSetTextPos(e, (int16_t)(0x200 - g_feChalX[1]), 100);
            guiSetTextPalette(e, 0);
            guiElementHandler(msg, e, a, b);
        }
        return 0;
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        g_feChalFrames++;
        textPos(COOL(5), 0x2C8 - g_feChalX[0], -0x32);
        textPos(COOL(6), 0x2C8 - g_feChalX[0], -0x32);
        frontendCursorDraw((int16_t)(g_feChalX[0] - 0x1AE), 0x3C, (int16_t)(g_feChalX[0] - 0xF0), 0xD4, 0x80, 1);
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (g_feTask != NULL)
            return 0;
        g_feCoolDelay = 2;
        g_feTask = frontendChallengesBuild;
        g_feChalTier = (uint8_t)frontendWrap(g_feChalTier - 1, 3);
        break;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if (g_feTask != NULL)
            return 0;
        g_feCoolDelay = 2;
        g_feTask = frontendChallengesBuild;
        g_feChalTier = (uint8_t)frontendWrap(g_feChalTier + 1, 3);
        break;
    case GUI_MSG_LEFT:
    case P2(GUI_MSG_LEFT):
        if (g_feTask != NULL)
            return 0;
        g_currentLevel = (uint8_t)frontendWrap(g_currentLevel - 1, 6);   /* the 6 challenge levels */
        g_feTask = frontendChallengesBuild;
        g_feCoolDelay = 2;
        break;
    case GUI_MSG_RIGHT:
    case P2(GUI_MSG_RIGHT):
        if (g_feTask != NULL)
            return 0;
        g_currentLevel = (uint8_t)frontendWrap(g_currentLevel + 1, 6);
        g_feCoolDelay = 2;
        g_feTask = frontendChallengesBuild;
        break;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 1;
        if (!g_guiAutoBack)
            SOUND(SND_SELECT);
        g_feTask = frontendChallengesSlideOut;
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
    SOUND(SND_MOVE);
    return 1;
}

/* ---- the cool stuff menu ---- */

/* VA 0x0044ca00, 48 bytes, [I] row, status draft */
int frontendCoolStuffEnter(void)
{
    guiDestroyScreen();
    guiLoadBackdropImage(0xA011, 8);
    guiLoadScreen(SCR_COOL, frontendCoolStuffHandler);
    g_feTask = frontendCoolStuffInit;
    return 0;
}

/* VA 0x0044ca40, 380 bytes, [I] row, status draft */
static int frontendCoolStuffHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_FRAME:
        if (g_feTask != NULL)
            g_feTask();
        if (g_feCoolReady) {
            if (g_feCursorPhase < 0 || g_feCursorPhase > 0x40)
                g_feCursorStep = -g_feCursorStep;
            g_feCursorPhase += g_feCursorStep;
            frontendCursorDraw(0x1F, (int16_t)(g_feCoolSel * 0x54 + 0x46), 0x71,
                               (int16_t)(g_feCoolSel * 0x54 + 0xA3), (uint8_t)(0x80 - g_feCursorPhase), 1);
        }
        return guiDefaultScreenHandler(msg, e, a, b);
    case GUI_MSG_UP:
    case P2(GUI_MSG_UP):
        if (g_feTask != NULL)
            return 0;
        if (--g_feCoolSel < 0)
            g_feCoolSel = 3;
        frontendCoolStuffRefresh();
        SOUND(SND_MOVE);
        return 0;
    case GUI_MSG_DOWN:
    case P2(GUI_MSG_DOWN):
        if (g_feTask != NULL)
            return 0;
        if (++g_feCoolSel > 3)
            g_feCoolSel = 0;
        frontendCoolStuffRefresh();
        SOUND(SND_MOVE);
        return 0;
    case GUI_MSG_ACCEPT:
    case GUI_MSG_START:
    case P2(GUI_MSG_ACCEPT):
    case P2(GUI_MSG_START):
        if (g_feTask == NULL) {
            g_feCoolReady = 0;
            g_feTask = frontendCoolStuffTabsOut;
            SOUND(SND_SELECT);
        }
        return 1;
    case GUI_MSG_BACK:
    case P2(GUI_MSG_BACK):
        if (g_feTask != NULL)
            return 0;
        g_feCoolReady = 0;
        g_feCoolSel = -1;
        g_feTask = frontendCoolStuffTabsOut;
        if (!g_guiAutoBack) {
            g_feCoolReady = 0;
            g_feTask = frontendCoolStuffTabsOut;
            SOUND(SND_SELECT);
        }
        return 1;
    default:
        return guiDefaultScreenHandler(msg, e, a, b);
    }
}

/* VA 0x0044cc70, 32 bytes, [I] row, status draft */
void frontendCoolStuffExit(void)
{
    if (g_feCoolDelay == 0) {
        frontendCursorFree();
        frontendMainMenuEnter(1);
        return;
    }
    g_feCoolDelay--;
}

/* VA 0x0044cca0, 375 bytes, [I] row, status draft */
void frontendCoolStuffRefresh(void)
{
    tint(COOL(8), 0xFF, 0xFF, 0xFF, g_feCoolSel == 0 ? 0xFF : 0x50);
    tint(COOL(0xA), 0xFF, 0xFF, 0xFF, g_feCoolSel == 1 ? 0xFF : 0x50);
    tint(COOL(0xC), 0xFF, 0xFF, 0xFF, g_feCoolSel == 2 ? 0xFF : 0x50);
    tint(COOL(0xE), 0xFF, 0xFF, 0xFF, g_feCoolSel == 3 ? 0xFF : 0x50);
    textPal(COOL(9), g_feCoolSel == 0);
    textPal(COOL(0xB), g_feCoolSel == 1);
    textPal(COOL(0xD), g_feCoolSel == 2);
    textPal(COOL(0xF), g_feCoolSel == 3);
}

/* VA 0x0044ce80, 397 bytes, [I] row, status draft */
void frontendCoolStuffTabsIn(void)
{
    showId(COOL(9));
    showId(COOL(0xB));
    showId(COOL(0xD));
    showId(COOL(0xF));
    showId(COOL(8));
    showId(COOL(0xA));
    showId(COOL(0xC));
    showId(COOL(0xE));
    if (g_guiAutoBack || frontendTabsIn(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE))) {
        hideId(COOL(0x17));
        hideId(COOL(0x16));
        frontendCoolStuffRefresh();
        g_feCoolReady = 1;
        g_feTask = NULL;
    }
    coolTabText();
}

/* VA 0x0044d080, 421 bytes, [I] row, status draft */
void frontendCreditsPlay(void)
{
    FeMovieDesc credits;
    const void *playlist[2];

    credits.path = "bin\\movie\\credits.str";
    credits.width = 0x140;
    credits.height = 0xF0;
    credits.frames = 0xD15;
    credits.fps = 0x18;
    credits.flags = 0;
    playlist[0] = &credits;
    playlist[1] = NULL;
    if (g_feCoolDelay != 0) {
        g_feCoolDelay--;
        return;
    }
    guiDestroyScreen();
    guiShutdownMenuMode();
    bmxsoundEvent(SND_MUSIC, 0, 0, 0, 0);
    ngappPlayMovie(playlist);
    bmxsoundEvent(SND_MUSIC, 1, 0, 0, 0);
    setVolumes();
    guiInitializeMenuMode();
    guiLoadBackdropImage(0xA011, 8);
    guiLoadScreen(SCR_COOL, frontendCoolStuffHandler);
    g_feTask = frontendCoolStuffInit;
    frontendCoolStuffReset();
    guiSetScreenHandler(frontendCoolStuffHandler);
    coolTabsHide();
    g_feCoolSel = 3;
}

/* VA 0x0044d2a0, 573 bytes, [I] row, status draft */
void frontendCoolStuffTabsOut(void)
{
    hideId(COOL(0x11));
    hideId(COOL(0x10));
    hideId(COOL(0x13));
    hideId(COOL(0x12));
    hideId(COOL(0x15));
    hideId(COOL(0x14));
    hideId(COOL(0x17));
    hideId(COOL(0x16));
    coolTabText();
    if (g_guiAutoBack || frontendTabsOut(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE))) {
        coolTabsHide();
        g_feCoolDelay = 2;
        switch (g_feCoolSel) {
        case 0:  g_feTask = frontendChallengesEnter; return;
        case 1:  g_feTask = frontendRecordsEnter; return;
        case 2:  g_feTask = frontendMoviesEnter; return;
        case 3:  g_feTask = frontendCreditsPlay; return;
        case -1: g_feTask = frontendCoolStuffExit; return;
        }
    }
}

static void coolStuffSetup(void)
{
    g_feCoolSel = 0;
    g_feCoolReady = 0;
    frontendCursorCreate();
    frontendTabsReset();
    frontendTabsIn(COOL(8), COOL(0xA), COOL(0xC), COOL(0xE));
    coolTabText();
    setText(COOL(9), zTextGet(0x2F));            /* CHALLENGES */
    setText(COOL(0xB), zTextGet(0x33));          /* HIGH SCORES */
    setText(COOL(0xD), zTextGet(0x34));          /* MOVIES */
    setText(COOL(0xF), zTextGet(0x35));          /* CREDITS */
    g_feTask = frontendCoolStuffTabsIn;
}

/* VA 0x0044d5a0, 353 bytes, [I] row, status draft */
void frontendCoolStuffInit(void)
{
    coolStuffSetup();
}

/* VA 0x0044d770, 353 bytes, [I] row, status draft */
void frontendCoolStuffReset(void)
{
    coolStuffSetup();                           /* the same code as frontendCoolStuffInit */
}

/* ================================================================== the app (NgApp calls these) */

/* VA 0x0044d940, 54 bytes, [I] row, status draft */
int frontendAllocMacros(void)
{
    if (g_feMacroBuf == NULL) {
        g_feMacroBuf = zmemTempAlloc(0x4000);
        if (g_feMacroBuf == NULL) {
            g_pfnDebugPrintf("FRONTEND: Failed to alloc memory\n");
            return 1;
        }
    }
    return 0;
}

/* VA 0x0044d990, 247 bytes, [I] row, status draft */
int frontendInitApp(void)
{
    zconsoleLog("APP: Init MENU\n");
    zrollerDisplayLoadingScreen();
    zrollerLoadCodeModule(0);
    frontendAllocMacros();
    if (guiInitializeApp() != 0) {
        g_pfnDebugPrintf("frontendInitApp: Failed to guiInitializeApp\n");
        return 1;
    }
    guiInputInit();
    g_feExit = 0;
    g_feExitQuit = 0;
    zrollerInit();
    zrollerNop1(0);
    zrollerNop2(0);
    if (frontendLoadMacros(g_saveBlockA.language) != 0) {
        g_pfnDebugPrintf("FRONTEND: Failed to load frontend macros\n");
        return 1;
    }
    bmxsoundEvent(SND_MUSIC, 1, 0, 0, 0);
    if (guiInitializeMenuMode() != 0) {
        g_pfnDebugPrintf("FRONTEND: Failed to guiInitializeMenuMode\n");
        return 1;
    }
    if (g_gameNextMode != 0 && g_gameNextMode < 4) {   /* back from a game: the select screen */
        frontendSelectEnter();
        return 0;
    }
    if (g_feAutoload == 0) {                            /* always on PC */
        frontendMainMenuEnter(0);
        return 0;
    }
    frontendAutoloadEnter();
    return 0;
}

/* VA 0x0044dad0, 269 bytes, [V] direct, status draft */
int frontendLoadMacros(uint32_t language)
{
    /* the frontend text set: miscdata.zal STRS 0x7919 + language (ids 0x000..0x272) */
    if (g_feMacroSize != 0) {
        zstringRemoveSet((ZstringSet *)g_feMacroBuf);
        g_feMacroSize = 0;
    }
    if (!zassetFindAsset(g_miscDataZal, language + 0x7919, 0)) {
        g_pfnDebugPrintf("FRONTEND: Could not find macro data\n");
        zAssertFail("FRONTEND: Could not find macro data\n\n", FRONTEND_SRC, 0xDD, NULL);
        return 1;
    }
    g_feMacroSize = zassetGetAssetSize(g_miscDataZal);
    if (g_feMacroSize == 0 || g_feMacroSize > 0x3FFF) {
        g_pfnDebugPrintf("FRONTEND: Macro data too big or too small\n");
        zAssertFail("FRONTEND: Macro data too big or too small\n\n", FRONTEND_SRC, 0xE4, NULL);
        return 1;
    }
    zassetLoadAsset(g_miscDataZal, g_feMacroBuf);
    if (!zstringAddSet((ZstringSet *)g_feMacroBuf)) {
        g_pfnDebugPrintf("FRONTEND: Failed to load macro string set\n");
        zAssertFail("FRONTEND: Failed to load macro string set\n\n", FRONTEND_SRC, 0xED, NULL);
        return 1;
    }
    return 0;
}

/* VA 0x0044dc30, 46 bytes, [I] row, status draft */
void frontendShutdownApp(void)
{
    zconsoleLog("APP: Shutdown MENU\n");
    bmxsoundEvent(SND_MUSIC, 0, 0, 0, 0);
    zrollerShutdown();
    guiShutdownMenuMode();
    guiInputShutdown();
}

/* VA 0x0044dc70, 121 bytes, [I] row, status draft */
int frontendUpdateApp(void)
{
    /* the menu loop: a second d-pad auto-repeat (every 3 frames after a 20-frame wait, every
     * frame while L1 / R1 is held), then guiUpdate. Returns 1 to quit the program. */
    GuiPad *pad;

    guiInputUpdate();
    pad = guiInputGetPad(0);
    if ((pad->pressed & ~(uint32_t)(PAD_LTRIG1 | PAD_RTRIG1)) == 0) {
        if (pad->repeat == 0) {
            pad->pressed = pad->held & PAD_DPAD;
            pad->repeat = (pad->held & (PAD_LTRIG1 | PAD_RTRIG1)) ? 0 : 3;
        } else {
            pad->repeat--;
        }
    } else {
        pad->repeat = 20;
    }
    if (!g_feExit)
        return guiUpdate() != 0;
    if (!g_feExitQuit) {
        zsystemSetActionMode();                          /* leave the menus for the game */
        return 0;
    }
    return 1;
}

/* VA 0x0044dd10, 20 bytes, [I] row, status draft */
void frontendRequestExit(int quit)
{
    /* 0: start the game (select screen done, demo); 1: QUIT */
    g_feExit = 1;
    g_feExitQuit = (uint32_t)quit;
}

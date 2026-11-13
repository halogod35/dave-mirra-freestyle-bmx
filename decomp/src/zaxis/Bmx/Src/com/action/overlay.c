/*
 * zaxis/Bmx/Src/com/action/overlay.c
 * Original: C:\zaxis\Bmx\Src\com\action\overlay.c
 * Evidence: [V] path string 0x005575C8, referenced by the asserts of overlayLoadFonts (lines
 *           0x491-0x4DB), overlayLoadStrings (0x50F-0x522), overlayRunOverDraw (0x125D, 0x126A),
 *           overlayMeterDraw (0x1897) and overlayHintSet (0x1B06); [I] the rest by link order
 *           (0x004147E0-0x0041FA0C, between overlymp.c and physics.c) and by the item / screen tables
 *           0x00556F00 / 0x00557098 that point at them.
 * 122 functions, 33470 bytes, VA 0x004147e0-0x0041fa0c.
 * Draft. Non-matching C (AD-8). Types: overlay.h, zoverii.h. Notes: docs/re/hud.md.
 *
 * The overlay pass created 85 functions Ghidra had missed in this range (decomp/symbols/symbols.csv): they are reached only
 * through the two function tables (HUD items, overlay screens) or through their ILT thunks.
 *
 * Coordinates are in the 640 x 480 PSX-style overlay space (x centre 0x13F/0x140).
 */

#include "decomp.h"

#include "zmath.h"
#include "zsound.h"
#include "zoverii.h"
#include "overlay.h"

#define OVERLAY_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\overlay.c"

/* ------------------------------------------------------------------ other modules (current names) */
uint32_t  actionSetMode(uint32_t mode);                                   /* action.c */
void      cameraSetPlayer(uint32_t player);                               /* camera.c */
void      cameraSetView(uint32_t player, uint32_t view);
void      cameraSetMode(uint32_t player, int mode);
void      riderReset(int slot, int active);                               /* rider.c */
uint8_t   gameStart(uint8_t mode);                                        /* game.c */
uint8_t   gameSetSubMode(uint8_t sub);
void      gameEndRun(void);
void      gameAdvanceGoalTier(int player);
int       gameIsTierComplete(int player);
uint8_t   gameGetRoundWinner(uint32_t round, uint32_t which);
uint8_t   gameGetViewReplay(void);
void      bmxreplayGetSaveArea(uint8_t handle, void **start, uint32_t *size); /* replay.c */
int       memcard_CheckSaveExists(void);                                  /* memcard.c */
int       memcard_Stub0(void);
int       memcard_WriteSave(void);
void      zsystemSetMenuMode(void);                       /* back to the menus (action.c's note) */
int       zrollerAllocImageSlots(int count);                 /* reserve image slots, returns the first */
void      zresAddMaterialToLoadList(int res, int a, int slot, int b, int c); /* load image resource 'res' */
const uint8_t *zresGetPlayer(uint16_t index);          /* PLYR entry (rider * 5 + style); name at +8 */
uint8_t  *zrollerGetPlayer(uint32_t player);           /* player options: +4 rider, +6, +7 */
void      zrollerAddDrawCallback(int (*draw)(void));         /* per-frame draw callback */
void      psxDrawRect(uint32_t bgr, int x, int y, int mode, const void *size);
/* PC stand-ins for the PSX memory card (gap_NgIFzroller__winChan.c, 1-3 byte stubs) */
void      zcardSelectPort(uint8_t slot);
int       zcardCheck(void (*done)(int, int));
int       zcardFormat(void);
void      zcardStart(void);
void      zcardStop(void);

extern char           *g_trickNames[84];             /* 0x00559390 tricks 0..45, then modifiers */
extern const char     *g_trickSpinNames[9];          /* 0x005594E0 "", "180", "360" ... "extreme" */
extern const uint8_t   g_trickFlags[28];             /* 0x00558FB4 bit 4: signature trick */
extern const uint8_t   g_modFlags[38];               /* 0x00558FD0 */
extern const uint8_t   g_gameModeStat[12];           /* 0x00553658 */
extern uint8_t         g_judgeScores[10][3];         /* 0x00695200 */
extern uint8_t         g_compRanking[10];            /* 0x0069521E */
extern uint8_t         g_actionPausePlayer;          /* 0x006962F5 */
extern uint32_t        g_gameSaveDone;               /* 0x006966DC "SAVING GAME DATA..." while set */
extern uint8_t         g_gameNextMode;               /* 0x0056B617 [I] what "level select / gear up" opens */
extern uint16_t        g_debugColour;                /* 0x006951E8 image slot of resource 0x3A */
extern ZassetLib      *g_miscDataZal;                /* 0x00678490 */

/* ------------------------------------------------------------------ sounds (bmxsoundEvent ids) */
enum {
    SND_MUSIC     = 1000,   /* (1000, 2) resume, (1000, 3) pause the music */
    SND_VOLUME    = 0x3EA,  /* (ch, level 0..127) */
    SND_GET_TRACK = 0x3EC,
    SND_SET_TRACK = 0x3ED,
    SND_MOVE      = 2000,   /* cursor */
    SND_CHANGE    = 0x7D1,  /* a value changed */
    SND_SELECT    = 0x7D2,
    SND_BACK      = 0x7D3,
    SND_ERROR     = 0x7D4,
    SND_DELETE    = 0x7D5,
    SND_TICK      = 0x1390, /* a line of the results appears */
    SND_REWARDS   = 0x1393  /* (page) */
};

/* ------------------------------------------------------------------ globals (this file) */
uint32_t       g_overlayFrame;            /* 0x0056BE14 frames since the screen / page opened */
OverlayTimedText g_hudTrack;              /* 0x0056BE18 */
uint16_t       g_hudTrackText[4];         /* 0x0056BE20 */
uint16_t       g_overlayTinySlot;         /* 0x0056BE28 */
uint8_t        g_overlayBackdropMode;     /* 0x0056BE2A */
uint16_t       g_hudScoreText;            /* 0x0056BE2C */
uint16_t       g_overlayLogoSlot;         /* 0x0056BE30 */
uint8_t        g_overlayConfirmReturn;    /* 0x0056BE32 screen under the yes/no question */
ZstringSet    *g_overlayStrings[2];       /* 0x0056BE34 text sets 0x1000 and 0x2000 / 0x3000 */
uint32_t       g_overlayCurrentOnly;      /* 0x0056BE3C only g_currentPlayer may use the screen */
uint16_t       g_overlayRecordText[10][4];/* 0x0056BE40 records screen: label, value, name, rider (row 0 unused) */
uint16_t       g_overlayLogo;             /* 0x0056BE90 */
uint16_t       g_overlayIconSlot;         /* 0x0056BE92 */
uint16_t       g_overlayDemoText;         /* 0x0056BE94 */
ZoverFont      g_overlayTinyFont;         /* 0x0056BE98 miscdata 0x7532 */
uint16_t       g_overlayAudioText[7];     /* 0x0056BEA4 title, 3 labels, cursor bar, artist, song */
uint16_t       g_hudGoalText[3];          /* 0x0056BEB4 */
uint8_t        g_overlayScreenId;         /* 0x0056BEBA */
uint16_t       g_overlayText[24];         /* 0x0056BEBC text of the open screen */
uint8_t        g_hudCrashShown;           /* 0x0056BEEC */
uint8_t        g_hudCrashId;              /* 0x0056BEED */
uint16_t       g_overlaySmallSlot;        /* 0x0056BEF4 */
uint16_t       g_overlayBackdrop;         /* 0x0056BEF8 dark full-screen quad behind the screens */
OverlayMenuEntry g_overlayPauseMenu[6];   /* 0x0056BEFC */
float          g_hudBalance;              /* 0x0056BF18 */
OverlayScreen *g_overlayScreen;           /* 0x0056BF20 open screen, NULL = HUD */
uint16_t       g_hudMeterText[6];         /* 0x0056BF24 background, 4 frame lines, marker */
uint8_t        g_overlayPage;             /* 0x0056BF30 page of the open screen */
ZoverFont      g_overlaySmallFont;        /* 0x0056BF38 miscdata 0x7531 */
OverlayTimedText g_hudHint;               /* 0x0056BF44 */
OverlayTimedText g_hudGoal;               /* 0x0056BF4C */
uint32_t       g_hudTimerShown;           /* 0x0056BF54 */
uint32_t       g_hudTimerFrames;          /* 0x0056BF58 */
uint16_t       g_hudReplayIcon[2];        /* 0x0056BF5C icon, frame counter (bit 4: blink) */
uint8_t        g_overlayNameCursor;       /* 0x0056BF64 */
uint32_t       g_overlayBlink;            /* 0x0056BF68 frames since the last cursor move */
uint16_t       g_overlayTrickText[21];    /* 0x0056BF6C 10 x (name, landed mark), description */
ZoverFont      g_overlayIconFont;         /* 0x0056BF98 miscdata 0x7535 */
uint32_t       g_overlayInputDelay;       /* 0x0056BFA4 frames before the screen takes input */
uint16_t       g_hudMessageText;          /* 0x0056BFA8 */
char           g_hudTimerStr[8];          /* 0x0056BFAC */
char           g_overlayName[12];         /* 0x0056BFB4 name being entered */
OverlayMeterReq g_hudMeter;               /* 0x0056BFC0 */
uint8_t        g_hudUnused8;              /* 0x0056BFD9 */
uint16_t       g_hudHintText[7];          /* 0x0056BFDC 3 lines, 4 button glyphs */
uint16_t       g_hudBalanceText[7];       /* 0x0056BFEC 2 gradients, 4 frame lines, marker */
uint16_t       g_overlayButtonSlot;       /* 0x0056BFFA */
uint16_t       g_hudCrashText;            /* 0x0056BFFC */
uint16_t       g_hudTimerText;            /* 0x0056C000 */
ZoverFont      g_overlayButtonFont;       /* 0x0056C008 miscdata 0x7534 */
OverlayMessage g_hudMessage;              /* 0x0056C018 */
OverlayMenuEntry g_overlayRunOverMenu[9]; /* 0x0056C028 */
OverlayFade    g_hudFade;                 /* 0x0056C050 */
uint16_t       g_overlayTrickTop;         /* 0x0056C070 first line shown */
uint16_t       g_overlayTrickSel;         /* 0x0056C072 */
uint8_t        g_overlaySaveSlot;         /* 0x0056C074 memory card slot 0/1 */
uint32_t       g_overlayDemoFrames;       /* 0x0056C07C */
float          g_hudMeterBase;            /* 0x0056BD48 lowest foot on the meter scale */
uint32_t       g_hudScoreShown;           /* 0x0056BD50 */
uint32_t       g_hudScoreTarget;          /* 0x0056BD58 */
uint8_t        g_hudScoreFlags;           /* 0x0056BD5C */
uint8_t        g_overlayLastScreen;       /* 0x0056BD60 for OVL_SCREEN_BACK */
uint32_t       g_overlayTrickCount;       /* 0x0056BD68 */
OverlayTrickEntry g_overlayTrickList[84]; /* 0x0056BD6C */
extern const char g_overlayEmpty[];       /* 0x0056BD44 "" (also the asserts' empty message) */

uint8_t        g_overlympSubMode = 2;       /* 0x00556EF8 multiplayer time limit chosen on the select screen */
int32_t        g_overlaySaveResult = -1;     /* 0x00556EFC memory card callback result, -1 pending */

/* HUD item table: { flags, init, draw, show, set, get } */
static void overlayLetterboxDraw(void);
static void overlayScoreInit(int on);   static void overlayScoreDraw(void);
static void overlayScoreShow(int on);   static void overlayScoreSet(const void *d);
static void overlayTimerInit(int on);   static void overlayTimerDraw(void);
static void overlayTimerShow(int on);   static void overlayTimerSet(const void *d);
static void overlayInputDraw(void);
static void overlayTricksDraw(void);
static void overlayMeterInit(int on);   static void overlayMeterDraw(void);
static void overlayMeterShow(int on);   static void overlayMeterSet(const void *d);
static void overlayBalanceInit(int on); static void overlayBalanceDraw(void);
static void overlayBalanceShow(int on); static void overlayBalanceSet(const void *d);
static void overlayCrashInit(int on);   static void overlayCrashDraw(void);
static void overlayCrashShow(int on);   static void overlayCrashSet(const void *d);
static void overlayUnused8Init(int on); static void overlayUnused8Draw(void);
static void overlayUnused8Show(int on); static void overlayUnused8Set(const void *d);
static void overlayReplayInit(int on);  static void overlayReplayDraw(void);
static void overlayReplayShow(int on);  static void overlayReplaySet(const void *d);
static void overlayHintInit(int on);    static void overlayHintDraw(void);
static void overlayHintShow(int on);    static void overlayHintSet(const void *d);
static void overlayHintGet(void *d);
static void overlayGoalInit(int on);    static void overlayGoalDraw(void);
static void overlayGoalShow(int on);    static void overlayGoalSet(const void *d);
static void overlayGoalGet(void *d);
static void overlayMessageInit(int on); static void overlayMessageDraw(void);
static void overlayMessageShow(int on); static void overlayMessageSet(const void *d);
static void overlayMessageGet(void *d);
static void overlayTrackInit(int on);   static void overlayTrackDraw(void);
static void overlayTrackShow(int on);   static void overlayTrackSet(const void *d);
static void overlayLogoInit(int on);    static void overlayLogoShow(int on);
static void overlayDemoInit(int on);    static void overlayDemoDraw(void);
static void overlayDemoShow(int on);
static void overlayFadeDraw(void);      static void overlayFadeSet(const void *d);

OverlayItem g_overlayItems[OVL_NUM_ITEMS] = {   /* 0x00556F00 */
    { 0, NULL,               overlayLetterboxDraw, NULL,               NULL,               NULL },
    { 0, overlayScoreInit,   overlayScoreDraw,     overlayScoreShow,   overlayScoreSet,    NULL },
    { 0, overlayTimerInit,   overlayTimerDraw,     overlayTimerShow,   overlayTimerSet,    NULL },
    { 0, NULL,               overlayInputDraw,     NULL,               NULL,               NULL },
    { 0, NULL,               overlayTricksDraw,    NULL,               NULL,               NULL },
    { 0, overlayMeterInit,   overlayMeterDraw,     overlayMeterShow,   overlayMeterSet,    NULL },
    { 0, overlayBalanceInit, overlayBalanceDraw,   overlayBalanceShow, overlayBalanceSet,  NULL },
    { 0, overlayCrashInit,   overlayCrashDraw,     overlayCrashShow,   overlayCrashSet,    NULL },
    { 0, overlayUnused8Init, overlayUnused8Draw,   overlayUnused8Show, overlayUnused8Set,  NULL },
    { 0, overlayReplayInit,  overlayReplayDraw,    overlayReplayShow,  overlayReplaySet,   NULL },
    { 0, overlayHintInit,    overlayHintDraw,      overlayHintShow,    overlayHintSet,     overlayHintGet },
    { 0, overlayGoalInit,    overlayGoalDraw,      overlayGoalShow,    overlayGoalSet,     overlayGoalGet },
    { 0, overlayMessageInit, overlayMessageDraw,   overlayMessageShow, overlayMessageSet,  overlayMessageGet },
    { 0, overlayTrackInit,   overlayTrackDraw,     overlayTrackShow,   overlayTrackSet,    NULL },
    { 0, overlayLogoInit,    NULL,                 overlayLogoShow,    NULL,               NULL },
    { 0, overlayDemoInit,    overlayDemoDraw,      overlayDemoShow,    NULL,               NULL },
    { 0, NULL,               overlayFadeDraw,      NULL,               overlayFadeSet,     NULL },
};

/* overlay screens: { enter, update, draw, count } (0x00557098 ...) */
static void overlayPauseEnter(int on);        static int overlayPauseUpdate(void);
static void overlayOptionsEnter(int on);      static int overlayOptionsUpdate(void);
static void overlayAudioEnter(int on);        static int overlayAudioUpdate(void);   static void overlayAudioDraw(void);
static void overlayTrickListEnter(int on);    static int overlayTrickListUpdate(void); static void overlayTrickListDraw(void);
static void overlayConfirmEnter(int on);      static int overlayConfirmUpdate(void);
static int  overlayStandingsUpdate(void);     static void overlayStandingsDraw(void);
static int  overlayJudgesUpdate(void);        static void overlayJudgesDraw(void);
static void overlayRunOverEnter(int on);      static int overlayRunOverUpdate(void); static void overlayRunOverDraw(void);
static void overlaySaveEnter(int on);         static int overlaySaveUpdate(void);    static void overlaySaveDraw(void);
static void overlayIntroEnter(int on);        static int overlayIntroUpdate(void);   static void overlayIntroDraw(void);
static void overlayGoalsEnter(int on);        static int overlayGoalsUpdate(void);   static void overlayGoalsDraw(void);
static void overlayResultsEnter(int on);      static int overlayResultsUpdate(void); static void overlayResultsDraw(void);
static void overlayNameEnter(int on);         static int overlayNameUpdate(void);    static void overlayNameDraw(void);
static void overlayRecordsEnter(int on);      static int overlayRecordsUpdate(void); static void overlayRecordsDraw(void);
int  overlympSelectUpdate(void);   void overlympSelectDraw(void);    /* overlymp.c */
int  overlympWordUpdate(void);     void overlympWordDraw(void);
int  overlympTurnUpdate(void);     void overlympTurnDraw(void);
int  overlympStandingsUpdate(void); void overlympStandingsDraw(void);

static void overlayOptionsRefresh(void);
static void overlayAudioRefresh(void);
static void overlayTrickListRefresh(void);
static uint32_t overlayTrickIsListed(uint32_t t);
static void overlayResultsPage(uint32_t page);
static void overlayAddReward(int player, const GameReward *rw, int *idx, int *y, int single);
static void overlayDrawJudgeScores(int *y);
static void overlaySaveRefresh(void);
static void overlaySaveCallback(int unused, int result);

static OverlayScreen s_screenPause     = { overlayPauseEnter,     overlayPauseUpdate,     NULL,                 5 }; /* 0x00557098 */
static OverlayScreen s_screenOptions   = { overlayOptionsEnter,   overlayOptionsUpdate,   NULL,                 3 }; /* 0x005570A8 */
static OverlayScreen s_screenAudio     = { overlayAudioEnter,     overlayAudioUpdate,     overlayAudioDraw,     3 }; /* 0x005570B8 */
static OverlayScreen s_screenTricks    = { overlayTrickListEnter, overlayTrickListUpdate, overlayTrickListDraw, 1 }; /* 0x005570C8 */
static OverlayScreen s_screenConfirm   = { overlayConfirmEnter,   overlayConfirmUpdate,   NULL,                 2 }; /* 0x005570D8 */
static OverlayScreen s_screenStandings = { NULL,                  overlayStandingsUpdate, overlayStandingsDraw, 1 }; /* 0x005570E8 */
static OverlayScreen s_screenJudges    = { NULL,                  overlayJudgesUpdate,    overlayJudgesDraw,    5 }; /* 0x005570F8 */
static OverlayScreen s_screenRunOver   = { overlayRunOverEnter,   overlayRunOverUpdate,   overlayRunOverDraw,   6 }; /* 0x00557108 */
static OverlayScreen s_screenSave      = { overlaySaveEnter,      overlaySaveUpdate,      overlaySaveDraw,      0 }; /* 0x00557118 */
static OverlayScreen s_screenIntro     = { overlayIntroEnter,     overlayIntroUpdate,     overlayIntroDraw,     2 }; /* 0x00557128 */
static OverlayScreen s_screenGoals     = { overlayGoalsEnter,     overlayGoalsUpdate,     overlayGoalsDraw,     1 }; /* 0x00557138 */
static OverlayScreen s_screenResults   = { overlayResultsEnter,   overlayResultsUpdate,   overlayResultsDraw,   1 }; /* 0x00557148 */
static OverlayScreen s_screenName      = { overlayNameEnter,      overlayNameUpdate,      overlayNameDraw,      2 }; /* 0x00557158 */
static OverlayScreen s_screenRecords   = { overlayRecordsEnter,   overlayRecordsUpdate,   overlayRecordsDraw,   1 }; /* 0x00557168 */
static OverlayScreen s_screenMpSelect  = { NULL, overlympSelectUpdate,    overlympSelectDraw,    11 }; /* 0x00557178 */
static OverlayScreen s_screenMpWord    = { NULL, overlympWordUpdate,      overlympWordDraw,       2 }; /* 0x00557188 */
static OverlayScreen s_screenMpTurn    = { NULL, overlympTurnUpdate,      overlympTurnDraw,       2 }; /* 0x00557198 */
static OverlayScreen s_screenMpStand   = { NULL, overlympStandingsUpdate, overlympStandingsDraw,  3 }; /* 0x005571A8 */

OverlayScreen *g_overlayScreens[OVL_NUM_SCREENS] = {     /* 0x005571B8 */
    NULL, &s_screenPause, &s_screenOptions, &s_screenAudio,
    &s_screenTricks, &s_screenTricks, &s_screenTricks,               /* 4..6: back to pause / judges / run over */
    &s_screenConfirm, &s_screenStandings, &s_screenJudges, &s_screenRunOver, &s_screenSave, &s_screenIntro,
    &s_screenGoals, &s_screenGoals, &s_screenGoals,                  /* 13..15: start / pause / level end */
    &s_screenResults, &s_screenName,
    &s_screenRecords, &s_screenRecords, &s_screenRecords, &s_screenRecords,  /* 18..21 */
    &s_screenMpSelect, &s_screenMpWord, &s_screenMpTurn, &s_screenMpStand,
};

/* memory card pages of the save screen: { items, first item text } (0x00557224) */
OverlaySavePage g_overlaySavePages[12] = {
    { 2, 0, 0x1142 }, { 1, 0, 0x1142 }, { 2, 0, 0x1144 }, { 1, 0, 0x1142 }, { 1, 0, 0x1142 },
    { 2, 0, 0x1146 }, { 2, 0, 0x1148 }, { 1, 0, 0x1142 }, { 1, 0, 0x1142 }, { 0, 0, 0 }, { 0, 0, 0 },
    { 0, 0, 0 },
};

/* the icon-font letter in front of a reward line, by reward type (overlayAddReward's local table) */
static const char s_rewardIcon[12] = { ' ', 'B', 'O', 'C', 'L', 'O', 'T', 'T', 'M', 'T', 'T', 'T' };

/* fixed rectangles for psxDrawRect: letterbox bar 640 x 60, full screen 640 x 480 */
static const uint32_t s_letterboxSize[2] = { 0x280, 0x3C };   /* 0x00557260 */
static const uint32_t s_screenSize[2]    = { 0x280, 0x1E0 };  /* 0x00557298 */

/* hint strings: an upper-case letter is a button glyph of the button font; the exe indexes the
 * table as 0x005572BB + c, i.e. g_hudButtonGlyph[c - 'A'] for 'A'..'L' -> "adwxXSTC2143" */
extern const char     g_hudButtonGlyph[12];     /* 0x005572FC */
/* the stick directions in the button font: RIDER_DIR_* 1..8 -> "adwxqezc" (index 0 = "") */
extern const char     g_hudDirGlyph[9];         /* 0x005572CB */
/* replay icons: "", "R" record, "P" play, "p" pause, "" and their colours */
extern const char    *g_hudReplayIconText[5];   /* 0x005572D4 */
extern const uint32_t g_hudReplayIconColour[5]; /* 0x005572E8 */
/* "double" "triple" "quad" "sick": stacked modifiers */
extern const char    *g_trickMultiNames[4];     /* 0x00557308 */
/* per-rider sponsor text of reward type 5, g_rewardSponsor[rider][param] */
extern const uint8_t  g_rewardSponsor[12][8];   /* 0x00556998 */

/* ------------------------------------------------------------------ small helpers */
#define CENTRE_X(w)   (0x13F - ((w) >> 1))
#define RIGHT_X(w)    (0x24A - (w))

static int16_t smallWidth(const char *s)  { return zoveriiTextWidth(s, &g_overlaySmallFont); }
static int16_t tinyWidth(const char *s)   { return zoveriiTextWidth(s, &g_overlayTinyFont); }

/* centred menu text in the small font (persistent, menu layer) */
static uint16_t addCentred(const char *s, int y, uint32_t colour)
{
    return zoveriiAddText(ZOVER_LAYER_MENU, &g_overlaySmallFont, s, (int16_t)CENTRE_X((uint16_t)smallWidth(s)),
                          (int16_t)y, colour, 0);
}

static void drawCentred(const ZoverFont *font, const char *s, int y, uint32_t colour)
{
    int16_t w = zoveriiTextWidth(s, font);

    zoveriiDrawText(font, s, (int16_t)(0x13F - ((uint16_t)w >> 1)), (int16_t)y, colour, 0);
}

static void freeTexts(uint16_t *h, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (h[i] != ZOVER_NO_HANDLE) {
            zoveriiFree(h[i]);
            h[i] = ZOVER_NO_HANDLE;
        }
}

/* the "selected" colour of item i, pulsing */
static uint32_t itemColour(uint32_t i, uint32_t sel, uint32_t normal)
{
    return i == sel ? overlayPulseColour() : normal;
}

/* a menu input from any player allowed to use the screen */
static int anyPlayerPressed(uint32_t mask)
{
    uint32_t p;

    for (p = 0; p < g_numPlayers; p++)
        if ((!g_overlayCurrentOnly || p == g_currentPlayer) && contrlerTestButtons(p, mask, 0))
            return 1;
    return 0;
}

static void menuAdd(OverlayMenuEntry *menu, uint16_t action, uint16_t text)
{
    menu[g_overlayScreen->count].action = action;
    menu[g_overlayScreen->count].text = text;
    g_overlayScreen->count++;
}

/* append "word" with a separating space (the trick-name builder's idiom) */
static void cat(char *s, const char *word)
{
    zstringPrintf(s + zstringLength(s), "%s%s", *s ? " " : g_overlayEmpty, word);
}

/* ================================================================== core */

/* VA 0x004147e0, 147 bytes, [I] split, status draft */
void overlayInit(void)
{
    /* actionLoad: the backdrop quad, every HUD item hidden, overlayDraw as a frame callback */
    uint32_t i;

    g_overlayScreen = NULL;
    g_overlayBackdrop = zoveriiAddRect(ZOVER_LAYER_MENU, 0, 0, 0x280, 0x1E0, 0x11202020);
    zoveriiShow(g_overlayBackdrop, 0);
    g_overlayBackdropMode = 0xFF;
    overlaySetBackdrop(0);
    zstringMemSet(g_overlayText, 0xFF, sizeof(g_overlayText));
    for (i = 0; i < OVL_NUM_ITEMS; i++) {
        if (g_overlayItems[i].init != NULL)
            g_overlayItems[i].init(1);
        g_overlayItems[i].flags = 0;
        overlayShowItem((int)i, 0);
    }
    g_overlayScreenId = OVL_SCREEN_NONE;
    zrollerAddDrawCallback(overlayDraw);
}

/* VA 0x004148b0, 134 bytes, [I] split, status draft */
void overlayShutdown(void)
{
    uint32_t i;

    overlaySetScreen(OVL_SCREEN_NONE);
    freeTexts(g_overlayText, 24);
    for (i = 0; i < OVL_NUM_ITEMS; i++)
        if (g_overlayItems[i].init != NULL)
            g_overlayItems[i].init(0);
    zoveriiFree(g_overlayBackdrop);
    g_overlayBackdrop = ZOVER_NO_HANDLE;
    for (i = 0; i < 2; i++)
        zstringRemoveSet(g_overlayStrings[i]);
}

/* load one font: image slot(s) + resource, then the glyph table from miscdata.zal */
static void loadFont(ZoverFont *font, uint16_t *slot, uint32_t asset, int line,
                     const char *missing, const char *bad)
{
    void *buf;

    if (!zassetFindAsset(g_miscDataZal, asset, 0))
        zAssertFail(missing, OVERLAY_SRC, line, NULL);
    buf = zmemTempAlloc(zassetGetAssetSize(g_miscDataZal));
    if (buf == NULL)
        zAssertFail("no memory for font", OVERLAY_SRC, line + 3, NULL);
    zassetLoadAsset(g_miscDataZal, buf);
    if (zoveriiLoadFont((const uint8_t *)buf, font) != 0)
        zAssertFail(bad, OVERLAY_SRC, line + 6, NULL);
    font->slot = slot;
}

/* VA 0x00414970, 882 bytes, [V] direct, status draft */
void overlayLoadFonts(void)
{
    /* The four overlay fonts (image resources 0x30/0x32/0x33/0x34 + glyph tables 0x7531/0x7532/
     * 0x7534/0x7535 in miscdata.zal), the logo image (0x39) and one more image slot (0x3A). */
    g_overlaySmallSlot = (uint16_t)zrollerAllocImageSlots(2);
    zresAddMaterialToLoadList(0x30, 0, g_overlaySmallSlot + 1, 0, 0);
    loadFont(&g_overlaySmallFont, &g_overlaySmallSlot, 0x7531, 0x491,
             "missing font data (small)", "error parsing font (small)");

    g_overlayTinySlot = (uint16_t)zrollerAllocImageSlots(1);
    zresAddMaterialToLoadList(0x32, 0, g_overlayTinySlot, 0, 0);
    loadFont(&g_overlayTinyFont, &g_overlayTinySlot, 0x7532, 0x4A5,
             "missing font data (tiny)", "error parsing font (tiny)");

    g_overlayButtonSlot = (uint16_t)zrollerAllocImageSlots(1);
    zresAddMaterialToLoadList(0x33, 0, g_overlayButtonSlot, 0, 0);
    loadFont(&g_overlayButtonFont, &g_overlayButtonSlot, 0x7534, 0x4B9,
             "missing font data (button)", "error parsing font (button)");

    g_overlayIconSlot = (uint16_t)zrollerAllocImageSlots(2);
    zresAddMaterialToLoadList(0x34, 0, g_overlayIconSlot + 1, 0, 0);
    loadFont(&g_overlayIconFont, &g_overlayIconSlot, 0x7535, 0x4D5,
             "missing font data (icons)", "error parsing font (icons)");

    g_overlayLogoSlot = (uint16_t)zrollerAllocImageSlots(1);
    zresAddMaterialToLoadList(0x39, 0, g_overlayLogoSlot, 0, 0);
    g_debugColour = (uint16_t)zrollerAllocImageSlots(1);
    zresAddMaterialToLoadList(0x3A, 0, g_debugColour, 0, 0);
}

/* VA 0x00414dd0, 305 bytes, [V] direct, status draft */
void overlayLoadStrings(uint8_t language)
{
    /* The HUD texts: STRS 0x7D01 + language (ids 0x1000..), and the single-player texts 0x7D05
     * (ids 0x2000..) or, with two players, the multiplayer texts 0x7D09 (ids 0x3000..). So 0x2xxx
     * ids only resolve in single player and 0x3xxx ids only in multiplayer. */
    uint32_t id;
    int line;

    if (!zassetFindAsset(g_miscDataZal, 0x7D01u + language, 0)) {
        line = 0x522;
    } else {
        g_overlayStrings[0] = (ZstringSet *)zmemTempAlloc(zassetGetAssetSize(g_miscDataZal));
        if (g_overlayStrings[0] == NULL)
            zAssertFail("no memory for string table", OVERLAY_SRC, 0x50F, NULL);
        zassetLoadAsset(g_miscDataZal, g_overlayStrings[0]);
        zstringAddSet(g_overlayStrings[0]);
        id = (g_numPlayers > 1 ? 0x7D09u : 0x7D05u) + language;
        if (zassetFindAsset(g_miscDataZal, id, 0)) {
            g_overlayStrings[1] = (ZstringSet *)zmemTempAlloc(zassetGetAssetSize(g_miscDataZal));
            if (g_overlayStrings[1] == NULL)
                zAssertFail("no memory for string table", OVERLAY_SRC, 0x51B, NULL);
            zassetLoadAsset(g_miscDataZal, g_overlayStrings[1]);
            zstringAddSet(g_overlayStrings[1]);
            return;
        }
        line = 0x51F;
    }
    zAssertFail("string table not found - invalid language?", OVERLAY_SRC, line, NULL);
}

/* VA 0x00414f60, 383 bytes, [I] row, status draft */
int overlayDraw(void)
{
    /* Frame callback: the HUD items when no screen is open, else the screen; the "SAVING GAME
     * DATA..." box fades in over 7 frames, stays to frame 51 and fades out by frame 60. */
    char buf[32];
    uint32_t i, f, level;

    if (g_overlayScreen == NULL) {
        for (i = 0; i < OVL_NUM_ITEMS; i++)
            if (!(g_overlayItems[i].flags & OVL_ITEM_HIDDEN) && g_overlayItems[i].draw != NULL)
                g_overlayItems[i].draw();
    } else {
        if (g_overlayScreen->draw != NULL)
            g_overlayScreen->draw();
        else if (g_overlayScreen->count != 0)
            zoveriiSetTextColour(g_overlayText[g_overlayScreen->sel], &g_overlaySmallFont, overlayPulseColour(), 0);
        if (g_gameSaveDone) {
            f = g_overlayFrame;
            if (f < 7)
                level = (uint8_t)(int)((float)f * 27.428572f);
            else if (f < 0x34)
                level = 0xC0;
            else if (f < 0x3C)
                level = (uint8_t)(int)((float)(0x3C - f) * 27.428572f);
            else {
                g_gameSaveDone = 0;
                zoveriiFlush();
                return 1;
            }
            if (level != 0) {
                zstringPrintf(buf, "%s", "SAVING GAME DATA...");
                zoveriiDrawText(&g_overlayTinyFont, buf, 0x2D, 0x21, ZOVER_GREY(3, level), 0);
                zoveriiDrawRect(0x28, 0x1E, (uint16_t)(tinyWidth(buf) + 10), 0x1A, 0xC0B060);
            }
        }
    }
    zoveriiFlush();
    return 1;
}

/* VA 0x00415140, 345 bytes, [I] between, status draft */
int overlayUpdate(void)
{
    /* actionUpdate while a screen is open: the screen's own input first (after g_overlayInputDelay
     * frames), then generic up/down over the menu items */
    OverlayScreen *s = g_overlayScreen;
    uint32_t p;
    int r;

    if (s == NULL)
        return 0;
    g_overlayBlink++;
    if (g_overlayFrame >= g_overlayInputDelay) {
        r = s->update();
        if (r != 0)
            return r;
        s = g_overlayScreen;
        if (s == NULL)
            return 0;
        if (s->count > 1) {
            for (p = 0; p < g_numPlayers; p++) {
                if (g_overlayCurrentOnly && p != g_currentPlayer)
                    continue;
                if (contrlerTestButtons(p, PAD_DOWN, 0)) {
                    if (s->draw == NULL)
                        zoveriiSetTextColour(g_overlayText[s->sel], &g_overlaySmallFont, OVL_COL_ITEM, 0);
                    s->sel = (uint8_t)((int)s->sel < s->count - 1 ? s->sel + 1 : 0);
                    bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
                    g_overlayBlink = 0;
                    return 1;
                }
                if (contrlerTestButtons(p, PAD_UP, 0)) {
                    if (s->draw == NULL)
                        zoveriiSetTextColour(g_overlayText[s->sel], &g_overlaySmallFont, OVL_COL_ITEM, 0);
                    s->sel = (uint8_t)((s->sel != 0 ? s->sel : s->count) - 1);
                    bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
                    g_overlayBlink = 0;
                    return 1;
                }
            }
        }
    }
    g_overlayFrame++;
    return 0;
}

/* VA 0x00415300, 69 bytes, [I] between, status draft */
void overlayShowItem(int item, uint32_t on)
{
    OverlayItem *it = &g_overlayItems[item];

    if (on != (~it->flags & 1)) {
        if (it->show != NULL)
            it->show((int)on);
        if (on)
            it->flags &= ~OVL_ITEM_HIDDEN;
        else
            it->flags |= OVL_ITEM_HIDDEN;
    }
}

/* VA 0x00415370, 30 bytes, [I] between, status draft */
void overlaySetItem(int item, const void *data)
{
    if (g_overlayItems[item].set != NULL)
        g_overlayItems[item].set(data);
}

/* VA 0x004153a0, 30 bytes, [I] between, status draft */
void overlayGetItem(int item, void *data)
{
    if (g_overlayItems[item].get != NULL)
        g_overlayItems[item].get(data);
}

/* VA 0x004153d0, 24 bytes, [I] between, status draft */
uint8_t overlayGetScreen(void)
{
    /* the confirm question counts as the screen under it */
    if (g_overlayScreenId == OVL_SCREEN_CONFIRM)
        return g_overlayConfirmReturn;
    return g_overlayScreenId;
}

/* VA 0x00415400, 524 bytes, [I] between, status draft */
void overlaySetScreen(uint32_t screen)
{
    /* Close the open screen and open another (0: back to the HUD, resume the music, unpause).
     * Entering a screen pauses the game (actionSetMode(1)); switching between screens goes through
     * actionSetMode(-1); the confirm question keeps the screen under it paused. */
    if (screen == OVL_SCREEN_BACK)
        screen = g_overlayLastScreen;
    else if (screen != OVL_SCREEN_NONE)
        g_overlayLastScreen = (uint8_t)screen;
    if (g_overlayScreen != NULL && g_overlayScreen->enter != NULL)
        g_overlayScreen->enter(0);
    g_overlayInputDelay = 0;
    g_overlayCurrentOnly = 1;
    g_gameSaveDone = 0;
    overlaySetPage(0);
    if (screen == OVL_SCREEN_NONE) {
        bmxsoundEvent(SND_MUSIC, 2, 0, 0, 0);
        zoveriiSetLayer(ZOVER_LAYER_HUD);
        if (g_overlayScreenId == OVL_SCREEN_CONFIRM)
            g_overlayScreenId = g_overlayConfirmReturn;
        actionSetMode(0);
        g_overlayScreenId = OVL_SCREEN_NONE;
        g_overlayScreen = g_overlayScreens[0];
        return;
    }
    if (g_overlayScreenId == OVL_SCREEN_CONFIRM && screen == g_overlayConfirmReturn) {
        /* "no": back to the screen under the question, still paused */
        g_overlayScreen = g_overlayScreens[screen];
        g_overlayScreenId = (uint8_t)screen;
        g_overlayScreen->sel = 0;
    } else {
        if (g_overlayScreenId != OVL_SCREEN_NONE && screen != OVL_SCREEN_CONFIRM)
            actionSetMode((uint32_t)-1);
        g_overlayScreen = g_overlayScreens[screen];
        g_overlayScreenId = (uint8_t)screen;
        g_overlayScreen->sel = 0;
        if (screen != OVL_SCREEN_CONFIRM)
            actionSetMode(1);
    }
    switch (screen) {
    case OVL_SCREEN_PAUSE:
        /* "view challenges" only in proquest on a challenge level */
        g_overlayScreen->count = (gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_PROQUEST &&
                                  g_currentLevel < 6) ? 6 : 5;
        break;
    case OVL_SCREEN_RUN_OVER:
        g_overlayInputDelay = 0xF;
        if (gameGetMode() != GAME_MODE_SINGLE)
            g_overlayCurrentOnly = 0;
        break;
    case OVL_SCREEN_MP_SELECT:
        g_overlayCurrentOnly = 0;
        break;
    case OVL_SCREEN_INTRO:
    case OVL_SCREEN_MP_WORD:
        g_overlayCurrentOnly = 0;
        g_overlayInputDelay = 0xF;
        break;
    case OVL_SCREEN_NAME_ENTRY:
    case OVL_SCREEN_MP_STANDINGS:
        g_overlayInputDelay = 0xF;
        break;
    case OVL_SCREEN_MP_TURN:
        g_overlayScreen->count = (gameGetMode() == GAME_MODE_BMX && !gameTestFlag(GAME_FLAG_SETTER)) ? 3 : 2;
        g_overlayInputDelay = 0xF;
        break;
    }
    zoveriiSetLayer(ZOVER_LAYER_MENU);
    freeTexts(g_overlayText, 24);
    if (g_overlayScreen->enter != NULL)
        g_overlayScreen->enter(1);
    overlaySetBackdrop(0);
}

/* VA 0x004156f0, 36 bytes, [I] row, status draft */
void overlayDrawTinyText(const char *s, int16_t x, int16_t y, uint32_t colour)
{
    /* [I] reached only through its ILT thunk 0x00402374, which nothing calls */
    zoveriiDrawText(&g_overlayTinyFont, s, x, y, colour, 0);
}

/* VA 0x00415730, 36 bytes, [I] between, status draft */
void overlaySetPage(uint8_t page)
{
    g_overlayPage = page;
    g_overlayFrame = 0;
    g_overlayBlink = 0;
    if (g_overlayScreen != NULL)
        g_overlayScreen->sel = 0;
}

/* VA 0x00415770, 69 bytes, [I] between, status draft */
void overlaySetBackdrop(uint32_t mode)
{
    /* mode 0: the dark backdrop is shown; anything else hides it */
    if (mode != g_overlayBackdropMode) {
        if (g_overlayBackdropMode == 0)
            zoveriiShow(g_overlayBackdrop, 0);
        if (mode == 0)
            zoveriiShow(g_overlayBackdrop, 1);
        g_overlayBackdropMode = (uint8_t)mode;
    }
}

/* ================================================================== pause, options, audio */

enum { PAUSE_CONTINUE, PAUSE_RESTART, PAUSE_GOALS, PAUSE_OPTIONS, PAUSE_TRICKS, PAUSE_RECORDS, PAUSE_END_RUN };

/* VA 0x004157e0, 426 bytes, [I] row, status draft */
static void overlayPauseEnter(int on)
{
    /* PAUSE: continue / (view challenges) / options / trick list / view records / end run.
     * The text 0x107B "restart run" has an action (1) but is never put in the menu. */
    uint32_t i;
    int y;
    const char *s;

    if (!on)
        return;
    s = zTextGet(0x1079);
    g_overlayText[23] = addCentred(s, 0x3C, OVL_COL_TITLE);
    g_overlayScreen->count = 0;
    menuAdd(g_overlayPauseMenu, PAUSE_CONTINUE, 0x107A);
    if (gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_PROQUEST && g_currentLevel < 6)
        menuAdd(g_overlayPauseMenu, PAUSE_GOALS, 0x107C);
    for (i = 0; i < 4; i++)
        menuAdd(g_overlayPauseMenu, (uint16_t)(PAUSE_OPTIONS + i), (uint16_t)(0x107D + i));
    y = 0xF0 - (g_overlayScreen->count * 0x1D >> 1);
    for (i = 0; i < g_overlayScreen->count; i++, y += 0x1D)
        g_overlayText[i] = addCentred(zTextGet(g_overlayPauseMenu[i].text), y, OVL_COL_ITEM);
}

/* VA 0x00415a00, 257 bytes, [I] row, status draft */
static int overlayPauseUpdate(void)
{
    uint32_t next;

    if (contrlerTestButtons(g_currentPlayer, PAD_PAUSE, 0)) {
        overlaySetScreen(OVL_SCREEN_NONE);
        bmxsoundEvent(SND_MUSIC, 2, 0, 0, 0);
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    if (!contrlerTestButtons(g_currentPlayer, PAD_FIRE4 | PAD_FIRE6, 0))
        return 0;
    switch (g_overlayPauseMenu[g_overlayScreen->sel].action) {
    case PAUSE_CONTINUE:
        overlaySetScreen(OVL_SCREEN_NONE);
        bmxsoundEvent(SND_MUSIC, 2, 0, 0, 0);
        goto done;
    case PAUSE_RESTART:
        gameStart(gameGetMode());
        gameSetState(GAME_STATE_TURN);
        goto done;
    case PAUSE_GOALS:   next = OVL_SCREEN_GOALS_PAUSE;   break;
    case PAUSE_OPTIONS: next = OVL_SCREEN_OPTIONS;       break;
    case PAUSE_TRICKS:  next = OVL_SCREEN_TRICKS;        break;
    case PAUSE_RECORDS: next = OVL_SCREEN_RECORDS_PAUSE; break;
    case PAUSE_END_RUN:
        g_overlayConfirmReturn = OVL_SCREEN_PAUSE;
        next = OVL_SCREEN_CONFIRM;
        break;
    default:
        goto done;
    }
    overlaySetScreen(next);
done:
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* VA 0x00415b70, 295 bytes, [I] between, status draft */
static void overlayOptionsEnter(int on)
{
    /* OPTIONS: camera type, tutorial hints (values drawn by overlayOptionsRefresh), audio options */
    static const uint16_t labels[2] = { 0x1083, 0x1085 };
    char buf[32];
    uint32_t i;
    int y;

    if (!on)
        return;
    g_overlayText[3] = addCentred(zTextGet(0x1082), 0x3C, OVL_COL_TITLE);
    y = 0xF0 - (g_overlayScreen->count * 0x21 >> 1);
    for (i = 0; i < 2; i++, y += 0x21) {
        zstringPrintf(buf, "%s:", zTextGet(labels[i]));
        g_overlayText[4 + i] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlaySmallFont, buf, 0x56, (int16_t)y,
                                              OVL_COL_ITEM, 0);
    }
    g_overlayText[2] = addCentred(zTextGet(0x1086), y, OVL_COL_ITEM);
    overlayOptionsRefresh();
}

/* VA 0x00415cf0, 701 bytes, [I] row, status draft */
static int overlayOptionsUpdate(void)
{
    SaveProfile *pf = &g_saveProfiles[g_currentPlayer];
    uint8_t maxCam, flags;
    BmxPad *pad;

    if (contrlerTestButtons(g_currentPlayer, PAD_RIGHT, 0) || contrlerTestButtons(g_currentPlayer, PAD_LEFT, 0)) {
        if (g_overlayScreen->sel == 0) {
            /* camera 0..2, 3 (first person) once the cheat is unlocked */
            maxCam = (g_saveBlockA.cheats_unlocked & 1) ? 3 : 2;
            if (contrlerTestButtons(g_currentPlayer, PAD_RIGHT, 0)) {
                if (++pf->camera > maxCam)
                    pf->camera = 0;
            } else {
                pf->camera = pf->camera == 0 ? maxCam : pf->camera - 1;
            }
            overlayOptionsRefresh();
            cameraSetView(g_currentPlayer, g_saveProfiles[g_currentPlayer].camera);
            bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
            return 1;
        }
        if (g_overlayScreen->sel == 1) {
            OverlayTimedText hintOff;

            hintOff.id = 0xFF;
            hintOff.pad_01 = 0;
            hintOff.timer = 0;
            flags = pf->flags;
            pf->flags = (flags & 2) ? (uint8_t)(flags & ~2) : (uint8_t)(flags | 2);
            overlaySetItem(OVL_ITEM_HINT, &hintOff);    /* clear the hint on screen */
            overlayOptionsRefresh();
            bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
            return 1;
        }
    }
    if (contrlerTestButtons(g_currentPlayer, PAD_FIRE4 | PAD_FIRE6, 0)) {
        overlaySetScreen(g_overlayScreen->sel == 2 ? OVL_SCREEN_AUDIO : OVL_SCREEN_PAUSE);
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    if (contrlerTestButtons(g_currentPlayer, PAD_START, 0)) {
        overlaySetScreen(OVL_SCREEN_PAUSE);
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    if (contrlerTestButtons(g_currentPlayer, PAD_FIRE1 | PAD_FIRE5, 0)) {
        overlaySetScreen(OVL_SCREEN_PAUSE);
        bmxsoundEvent(SND_BACK, 0, 0, 0, 0);
        return 1;
    }
    /* a pad that was unplugged / plugged in clears the profile's flag bit 0 [I] */
    pad = contrlerGetPad(g_saveProfiles[g_currentPlayer].index);
    if (pad->flags & (BMXPAD_DISCONNECTED | BMXPAD_RECONNECTED)) {
        g_saveProfiles[g_currentPlayer].flags &= ~1;
        overlayOptionsRefresh();
        if (contrlerGetPad(g_saveProfiles[g_currentPlayer].index)->flags & BMXPAD_RECONNECTED)
            bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
    }
    return 0;
}

/* VA 0x00416060, 316 bytes, [I] between, status draft */
static void overlayOptionsRefresh(void)
{
    /* the values, right-aligned: camera (text 0x1087 + camera), tutorial hints off / on (0x101A +
     * profile flag bit 1); white on the selected line */
    const char *s;
    int y = -(g_overlayScreen->count * 0x21 >> 1);

    s = zTextGet(0x1087u + g_saveProfiles[g_currentPlayer].camera);
    if (g_overlayText[0] != ZOVER_NO_HANDLE)
        zoveriiFree(g_overlayText[0]);
    g_overlayText[0] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlaySmallFont, s, (int16_t)(0x229 - (uint16_t)smallWidth(s)),
                                      (int16_t)(y + 0xF0), g_overlayScreen->sel == 0 ? 0x02F0F0F0u : OVL_COL_ITEM, 0);
    s = zTextGet(0x101Au + ((g_saveProfiles[g_currentPlayer].flags >> 1) & 1));
    if (g_overlayText[1] != ZOVER_NO_HANDLE)
        zoveriiFree(g_overlayText[1]);
    g_overlayText[1] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlaySmallFont, s, (int16_t)(0x229 - (uint16_t)smallWidth(s)),
                                      (int16_t)(y + 0x111), g_overlayScreen->sel == 1 ? 0x02F0F0F0u : OVL_COL_ITEM, 0);
}

/* VA 0x004161f0, 384 bytes, [I] row, status draft */
static void overlayAudioEnter(int on)
{
    /* AUDIO OPTIONS: SFX volume, music volume (bars of overlayAudioDraw), music track */
    char buf[64];
    uint32_t i;
    int y;
    const char *label;

    if (!on) {
        freeTexts(g_overlayAudioText, 7);
        return;
    }
    zstringMemSet(g_overlayAudioText, 0xFF, sizeof(g_overlayAudioText));
    g_saveBlockA.music_track = (uint8_t)bmxsoundEvent(SND_GET_TRACK, 0, 0, 0, 0);
    g_overlayAudioText[0] = addCentred(zTextGet(0x108C), 0x3C, OVL_COL_TITLE);
    y = 0xF0 - ((g_overlayScreen->count * 0x1D + 0x2C) >> 1);
    for (i = 0; i < g_overlayScreen->count; i++, y += 0x21) {
        label = zTextGet(0x108D + i);
        zstringPrintf(buf, "%s:", label);
        g_overlayAudioText[1 + i] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlaySmallFont, buf,
                                                   (int16_t)(i < 2 ? 0x35 : CENTRE_X((uint16_t)smallWidth(buf))),
                                                   (int16_t)y, OVL_COL_ITEM, 0);
    }
    overlayAudioRefresh();
}

static void setVolume(uint32_t channel, uint8_t level)
{
    /* level 0..18 -> 0..127 (the exe multiplies by the reciprocal of 18) */
    uint32_t v = (uint32_t)level * 0x7F / 0x12;

    if (v > 0x7E)
        v = 0x7F;
    bmxsoundEvent(SND_VOLUME, channel, v, 0, 0);
}

/* VA 0x004163d0, 852 bytes, [I] row, status draft */
static int overlayAudioUpdate(void)
{
    /* up / down: line; left / right: volume 0..18 (volume_0c = SFX, ch 0; volume_0d = music, ch 1)
     * or the music track 0..9 (0x3ED plays it at once); back / ok / start: to the options */
    uint8_t *vol;
    int right;

    if (contrlerTestButtons(g_currentPlayer, PAD_DOWN, 0)) {
        g_overlayScreen->sel = (uint8_t)((int)g_overlayScreen->sel < g_overlayScreen->count - 1 ? g_overlayScreen->sel + 1 : 0);
        goto moved;
    }
    if (contrlerTestButtons(g_currentPlayer, PAD_UP, 0)) {
        g_overlayScreen->sel = (uint8_t)((g_overlayScreen->sel != 0 ? g_overlayScreen->sel : g_overlayScreen->count) - 1);
        goto moved;
    }
    right = contrlerTestButtons(g_currentPlayer, PAD_RIGHT, 0);
    if (!right && !contrlerTestButtons(g_currentPlayer, PAD_LEFT, 0)) {
        if (!contrlerTestButtons(g_currentPlayer, PAD_FIRE1 | PAD_FIRE5, 0) &&
            !contrlerTestButtons(g_currentPlayer, PAD_FIRE4 | PAD_FIRE6, 0) &&
            !contrlerTestButtons(g_currentPlayer, PAD_START, 0))
            return 0;
        overlaySetScreen(OVL_SCREEN_OPTIONS);
        bmxsoundEvent(SND_SELECT + (contrlerTestButtons(g_currentPlayer, PAD_FIRE1 | PAD_FIRE5, 0) != 0),
                          0, 0, 0, 0);
        return 1;
    }
    switch (g_overlayScreen->sel) {
    case 0:
    case 1:
        vol = g_overlayScreen->sel == 0 ? &g_saveBlockA.volume_0c : &g_saveBlockA.volume_0d;
        if (right ? *vol >= 0x12 : *vol == 0) {
            bmxsoundEvent(SND_ERROR, 0, 0, 0, 0);
            break;
        }
        *vol = right ? (uint8_t)(*vol + 1) : (uint8_t)(*vol - 1);
        setVolume(g_overlayScreen->sel, *vol);
        bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
        break;
    case 2:
        if (right)
            g_saveBlockA.music_track = g_saveBlockA.music_track < 9 ? g_saveBlockA.music_track + 1 : 0;
        else
            g_saveBlockA.music_track = g_saveBlockA.music_track == 0 ? 9 : g_saveBlockA.music_track - 1;
        bmxsoundEvent(SND_SET_TRACK, g_saveBlockA.music_track, 0, 0, 0);
        bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
        break;
    }
    overlayAudioRefresh();
    return 1;
moved:
    bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
    g_overlayBlink = 0;
    overlayAudioRefresh();
    return 1;
}

/* VA 0x00416810, 391 bytes, [I] between, status draft */
static void overlayAudioRefresh(void)
{
    /* the cursor bar on a volume line, or the track's title (0x102F + 4 t) and artist (0x102E + 4 t)
     * under "music track" (tiny font, brighter when selected) */
    const char *s;
    uint32_t i, colour;
    int y = 0xF0 - ((g_overlayScreen->count * 0x1D + 0x2C) >> 1);

    freeTexts(&g_overlayAudioText[4], 3);
    for (i = 0; i < g_overlayScreen->count; i++) {
        if (i == 2) {
            colour = g_overlayScreen->sel == 2 ? 0x02D0D0D0u : 0x02404040u;
            s = zTextGet(0x102Fu + g_saveBlockA.music_track * 4u);
            g_overlayAudioText[5] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s,
                                                   (int16_t)CENTRE_X((uint16_t)tinyWidth(s)), (int16_t)(y + 0x21),
                                                   colour, 0);
            s = zTextGet(0x102Eu + g_saveBlockA.music_track * 4u);
            g_overlayAudioText[6] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s,
                                                   (int16_t)CENTRE_X((uint16_t)tinyWidth(s)), (int16_t)(y + 0x35),
                                                   colour, 0);
            y += 0x49;
        } else {
            if (i == g_overlayScreen->sel)
                g_overlayAudioText[4] = zoveriiAddRect(ZOVER_LAYER_MENU, 0x1D2, (int16_t)(y - 3), 0x75, 0x12, 0x01000000);
            y += 0x21;
        }
    }
}

/* VA 0x00416a10, 156 bytes, [I] row, status draft */
static void overlayAudioDraw(void)
{
    /* the two volume bars: one 3 x 12 block per step, highlighted on the selected line */
    uint32_t i, n;
    int x, y = 0xF0 - ((g_overlayScreen->count * 0x1D + 0x2C) >> 1);

    for (i = 0; i < g_overlayScreen->count; i++) {
        if (i >= 2)
            continue;
        n = i == 0 ? g_saveBlockA.volume_0c : g_saveBlockA.volume_0d;
        for (x = 0x1D8; n != 0; n--, x += 6)
            zoveriiDrawRect((int16_t)x, (int16_t)y, 3, 0xC, itemColour(i, g_overlayScreen->sel, OVL_COL_ITEM));
        y += 0x21;
    }
}

/* ================================================================== trick list */

/* VA 0x00416ae0, 316 bytes, [I] row, status draft */
static void overlayTrickListEnter(int on)
{
    /* TRICK LIST: every trick and modifier the rider has landed (career bits, SaveRiderRecord
     * .tricks_landed: bit t = trick t, bit 46 + m = modifier m) plus every one that is not a
     * signature trick (flag 0x10); landed ones get the icon-font mark "c". */
    const SaveRiderRecord *rec;
    OverlayTrickEntry *e;
    uint32_t t;
    uint8_t bit, landed;

    if (!on) {
        freeTexts(g_overlayTrickText, 21);
        return;
    }
    g_overlayText[0] = addCentred(zTextGet(0x1090), 0x3C, OVL_COL_TITLE);
    zstringMemSet(g_overlayTrickText, 0xFF, sizeof(g_overlayTrickText));
    g_overlayTrickCount = 0;
    rec = &g_saveBlockA.riders[g_saveProfiles[g_currentPlayer].rider];
    for (t = 0; t < 0x54; t++) {
        bit = (uint8_t)(1 << (t & 7));
        landed = rec->tricks_landed[t >> 3] & bit;
        if (!landed && !overlayTrickIsListed(t))
            continue;
        e = &g_overlayTrickList[g_overlayTrickCount++];
        if (t < 0x2E) {
            e->kind = 0;
            e->id = (uint8_t)t;
        } else {
            e->kind = 1;
            e->id = (uint8_t)(t - 0x2E);
        }
        if (landed)
            e->kind |= 0x80;
    }
    overlayTrickListRefresh();
}

static void trickListBack(uint32_t sound)
{
    if (g_overlayScreenId == OVL_SCREEN_TRICKS)
        overlaySetScreen(OVL_SCREEN_PAUSE);
    else if (g_overlayScreenId == OVL_SCREEN_TRICKS_JUDGES)
        overlaySetScreen(OVL_SCREEN_JUDGES);
    else if (g_overlayScreenId == OVL_SCREEN_TRICKS_RUN)
        overlaySetScreen(OVL_SCREEN_RUN_OVER);
    bmxsoundEvent(sound, 0, 0, 0, 0);
}

/* VA 0x00416c70, 461 bytes, [I] row, status draft */
static int overlayTrickListUpdate(void)
{
    /* 10 lines visible; the window follows the cursor */
    uint32_t p;

    if (overlayConfirmed(0)) {
        trickListBack(SND_SELECT);
        return 1;
    }
    for (p = 0; p < g_numPlayers; p++) {
        if (g_overlayCurrentOnly && p != g_currentPlayer)
            continue;
        if (contrlerTestButtons(p, PAD_FIRE1 | PAD_FIRE5, 0)) {
            trickListBack(SND_BACK);
            return 1;
        }
        if (contrlerTestButtons(p, PAD_DOWN, 0)) {
            if (g_overlayTrickSel < g_overlayTrickCount - 1) {
                if ((uint32_t)g_overlayTrickTop + 10 <= (uint32_t)g_overlayTrickSel + 1)
                    g_overlayTrickTop++;
                g_overlayTrickSel++;
                g_overlayBlink = 0;
                bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
                overlayTrickListRefresh();
            } else {
                bmxsoundEvent(SND_ERROR, 0, 0, 0, 0);
            }
            return 1;
        }
        if (contrlerTestButtons(p, PAD_UP, 0)) {
            if (g_overlayTrickSel != 0) {
                if ((uint16_t)(g_overlayTrickSel - 1) < g_overlayTrickTop)
                    g_overlayTrickTop--;
                g_overlayTrickSel--;
                g_overlayBlink = 0;
                overlayTrickListRefresh();
                bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
            } else {
                bmxsoundEvent(SND_ERROR, 0, 0, 0, 0);
            }
            return 1;
        }
    }
    return 0;
}

/* VA 0x00416ec0, 13 bytes, [I] row, status draft */
static void overlayTrickListDraw(void)
{
    overlayDrawMenu(8, 0);
}

/* VA 0x00416ee0, 536 bytes, [I] between, status draft */
static void overlayTrickListRefresh(void)
{
    /* Ten names in the tiny font, coloured by kind (air blue, grind green, stall purple, plant
     * orange, manual / nose wheelie dark blue, modifier orange), the cursor line grey; then the
     * selected entry's input (texts 0x10BB + trick / 0x10E9 + modifier: button-font glyph strings
     * such as "x C" = down + modifier button). Names come straight from g_trickNames. */
    const OverlayTrickEntry *e;
    const char *name;
    uint32_t row, i, colour;
    int y = 0xF0 - ((g_overlayButtonFont.height + 0xCE) >> 1), yMark = y + 1;

    freeTexts(g_overlayTrickText, 21);
    for (row = 0; row < 10; row++, y += 0x14, yMark += 0x14) {
        i = g_overlayTrickTop + row;
        e = &g_overlayTrickList[i];
        if ((e->kind & 0x3F) == 1) {
            name = g_trickNames[0x2E + e->id];
            colour = 0x02A07010;
        } else {
            name = g_trickNames[e->id];
            if (e->id < 0x1B)       colour = 0x022030D0;   /* air */
            else if (e->id < 0x24)  colour = 0x0220A040;   /* grind */
            else if (e->id < 0x2A)  colour = 0x02703090;   /* stall */
            else if (e->id < 0x2C)  colour = 0x02900010;   /* fast plant / wall tap */
            else                    colour = 0x02200080;   /* manual / nose wheelie */
        }
        if (i == g_overlayTrickSel)
            colour = (e->kind & 0x40) ? 0x03808080u : 0x02A0A0A0u;
        else if (e->kind & 0x40)
            colour = 0x02202020;
        g_overlayTrickText[row * 2] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, name,
                                                     (int16_t)CENTRE_X((uint16_t)tinyWidth(name)), (int16_t)y, colour, 0);
        if (e->kind & 0x80)
            g_overlayTrickText[row * 2 + 1] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayIconFont, "c", 0x20E,
                                                             (int16_t)yMark, 0x02B04010, 0);
    }
    e = &g_overlayTrickList[g_overlayTrickSel];
    name = zTextGet((e->kind & 0x3F) == 1 ? 0x10E9u + e->id : 0x10BBu + e->id);
    g_overlayTrickText[20] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayButtonFont, name,
                                            (int16_t)CENTRE_X((uint16_t)zoveriiTextWidth(name, &g_overlayButtonFont)),
                                            (int16_t)(y - 0x14 + 0x1A), OVL_COL_VALUE, 0);
}

/* VA 0x00417190, 50 bytes, [I] between, status draft */
static uint32_t overlayTrickIsListed(uint32_t t)
{
    /* everything except the signature tricks / modifiers (flag 0x10) is listed before it is landed */
    if (t >= 0x2E)
        return (~g_modFlags[t - 0x2E] & 0x10) >> 4;
    if (t < 0x1B)
        return (~g_trickFlags[t] & 0x10) >> 4;
    return 1;
}

/* ================================================================== level intro, challenges */

/* VA 0x004171e0, 297 bytes, [I] row, status draft */
static void overlayIntroEnter(int on)
{
    /* before a run: level name + 5 lines of description (single player, texts 0x2000 + 5 level), or
     * the game mode + its 5-line rules (multiplayer, 0x3011 + 5 mode) */
    const char *s;
    uint32_t i, id;
    int y;

    if (!on)
        return;
    s = zTextGet(gameGetMode() == GAME_MODE_SINGLE ? 0x101Eu + g_currentLevel : 0x3001u + gameGetMode());
    g_overlayText[5] = addCentred(s, 0x3C, OVL_COL_TITLE);
    for (i = 0, y = 0xBE; i < 5; i++, y += 0x14) {
        if (gameGetMode() == GAME_MODE_SINGLE)
            id = g_currentLevel < 0xB ? 0x2000u + g_currentLevel * 5u + i : 0x2037u + i;
        else
            id = (gameGetMode() + 0x99Du) * 5u + i;
        s = zTextGet(id);
        g_overlayText[i] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s,
                                          (int16_t)CENTRE_X((uint16_t)tinyWidth(s)), (int16_t)y, OVL_COL_GREY, 0);
    }
}

/* VA 0x00417360, 252 bytes, [I] row, status draft */
static int overlayIntroUpdate(void)
{
    /* continue: start the run (B-M-X: the word entry first); exit: single player asks EXIT?,
     * multiplayer goes back to the game select */
    uint32_t next;

    if (overlayConfirmed(0)) {
        if (g_overlayScreen->sel == 0) {
            if (gameGetMode() != GAME_MODE_BMX) {
                overlaySetScreen(OVL_SCREEN_NONE);
                gameSetState(GAME_STATE_TURN);
                goto done;
            }
            next = OVL_SCREEN_MP_WORD;
        } else if (g_overlayScreen->sel == 1) {
            if (gameGetMode() == GAME_MODE_SINGLE) {
                g_overlayConfirmReturn = OVL_SCREEN_INTRO;
                overlaySetScreen(OVL_SCREEN_CONFIRM);
                g_gameNextMode = 3;
                goto done;
            }
            next = OVL_SCREEN_MP_SELECT;
        } else {
            goto back;
        }
        overlaySetScreen(next);
done:
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
back:
    if (gameGetMode() != GAME_MODE_SINGLE && anyPlayerPressed(PAD_FIRE1 | PAD_FIRE5)) {
        overlaySetScreen(OVL_SCREEN_MP_SELECT);
        bmxsoundEvent(SND_BACK, 0, 0, 0, 0);
        return 1;
    }
    return 0;
}

/* VA 0x004174a0, 13 bytes, [I] row, status draft */
static void overlayIntroDraw(void)
{
    overlayDrawMenu(0, 0);          /* continue / exit */
}

/* VA 0x004174c0, 673 bytes, [I] row, status draft */
static void overlayGoalsEnter(int on)
{
    /* CHALLENGES: the tier ("pro challenges:"), the four goals of the tier (3 text lines each,
     * 0x2089 + (12 level + 4 tier + goal) * 3, joined with spaces) and 3 lines of what the tier
     * unlocks (0x2041 + (4 level + tier) * 3); tier 3 = all done (0x2040). */
    const SaveGoalInfo *gi = comdataGetGoalInfo(g_currentPlayer, g_currentLevel);
    char buf[128];
    const char *s;
    uint32_t tier, g, k, id, n;
    int y;

    if (!on)
        return;
    g_overlayText[0] = addCentred(zTextGet(0x203C), 0x3C, OVL_COL_TITLE);
    tier = (gi->tier == 2 && gi->tier_goals_done == 4) ? 3 : gi->tier;
    zstringPrintf(buf, "%s%s", zTextGet(0x203D + tier), tier < 3 ? ":" : g_overlayEmpty);
    g_overlayText[1] = addCentred(buf, 0x8E, OVL_COL_ITEM);
    n = 2;
    y = 0xB3;
    if (tier < 3) {
        id = ((uint32_t)g_currentLevel * 3 + gi->tier) * 12 + 0x2089;
        for (g = 0; g < 4; g++, y += 0x16) {
            buf[0] = '\0';
            for (k = 0; k < 3; k++) {
                s = zTextGet(id++);
                if (*s != '\0')
                    zstringPrintf(buf + zstringLength(buf), "%s%s", k > 0 ? " " : g_overlayEmpty, s);
            }
            g_overlayText[2 + g] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, buf,
                                                  (int16_t)CENTRE_X((uint16_t)tinyWidth(buf)), (int16_t)y, OVL_COL_GREY, 0);
        }
        n = 6;
    } else {
        y = 0x10B;
    }
    y += 0xC;
    id = (tier + g_currentLevel * 4u) * 3 + 0x2041;
    for (k = 0; k < 3; k++, y += 0x14) {
        s = zTextGet(id + k);
        g_overlayText[n + k] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s,
                                              (int16_t)CENTRE_X((uint16_t)tinyWidth(s)), (int16_t)y, OVL_COL_ORANGE, 0);
    }
}

/* VA 0x00417820, 198 bytes, [I] row, status draft */
static int overlayGoalsUpdate(void)
{
    /* 13: start the run; 14: back to the pause menu; 15 (level end): write SAVE.DAT, then RUN FINISHED */
    if (!overlayConfirmed(0) &&
        (!contrlerTestButtons(g_currentPlayer, PAD_FIRE1 | PAD_FIRE5, 0) || g_overlayScreenId == OVL_SCREEN_GOALS_START))
        return 0;
    if (g_overlayScreenId == OVL_SCREEN_GOALS_START) {
        overlaySetScreen(OVL_SCREEN_NONE);
        gameSetState(GAME_STATE_RUN);
    } else if (g_overlayScreenId == OVL_SCREEN_GOALS_PAUSE) {
        overlaySetScreen(OVL_SCREEN_PAUSE);
    } else if (g_overlayScreenId == OVL_SCREEN_GOALS_END) {
        memcard_WriteSave();
        overlaySetScreen(OVL_SCREEN_RUN_OVER);
        g_gameSaveDone = 1;
    }
    bmxsoundEvent(contrlerTestButtons(g_currentPlayer, PAD_FIRE1 | PAD_FIRE5, 0) ? SND_BACK : SND_SELECT,
                      0, 0, 0, 0);
    return 1;
}

/* VA 0x00417930, 454 bytes, [I] row, status draft */
static void overlayGoalsDraw(void)
{
    /* At the run start (screen 13) the four goals light up one by one (16 frames each, a flash
     * curve from the local table, a tick sound); a done goal gets a bar drawn under it, growing
     * over 16 frames when it was done in this session (goal byte bit 0x40). */
    static const uint8_t flash[16] = { 0x20, 0x40, 0x60, 0x78, 0x90, 0xA8, 0xC0, 0xD0,
                                       0xE0, 0xF0, 0xF0, 0xC0, 0xA0, 0x98, 0x90, 0x88 };
    const SaveGoalInfo *gi = comdataGetGoalInfo(g_currentPlayer, g_currentLevel);
    uint32_t g, w;
    uint8_t tier = gi->tier, level, done;
    int y = 0xBF;

    if (tier == 2 && gi->tier_goals_done == 4)
        tier = 3;
    if (tier < 3) {
        for (g = 0; g < 4; g++, y += 0x16) {
            level = 0x80;
            if (g_overlayScreenId == OVL_SCREEN_GOALS_START) {
                if ((g_overlayFrame >> 4) < g) {
                    level = 0;
                } else if ((g_overlayFrame >> 4) == g) {
                    level = flash[g_overlayFrame & 0xF];
                    if ((g_overlayFrame & 0xF) == 1)
                        bmxsoundEvent(SND_TICK, 0, 0, 0, 0);
                }
            }
            zoveriiSetTextColour(g_overlayText[2 + g], &g_overlayTinyFont, ZOVER_GREY(2, level), 0);
            done = gi->goal[gi->tier][g][0];
            if (done & 0x80) {
                w = (!(done & 0x40) || g_overlayFrame > 0xF) ? 0x1F5 : g_overlayFrame * 0x1F5 >> 4;
                zoveriiDrawRect(0x45, (int16_t)y, (uint16_t)w, 1,
                                ZOVER_COLOUR(2, ZOVER_RGB(level >> 4, (level * 0xC0) >> 8, level >> 2)));
            }
        }
    }
    overlayDrawMenu(8, 0);
}

/* ================================================================== results and rewards */

/* VA 0x00417b80, 93 bytes, [I] row, status draft */
static void overlayResultsEnter(int on)
{
    if (on) {
        g_overlayText[0] = addCentred(zTextGet(0x2161), 0x3C, OVL_COL_TITLE);
        overlayResultsPage(0);
    }
}

/* VA 0x00417c00, 962 bytes, [I] between, status draft */
static void overlayResultsPage(uint32_t page)
{
    /* RESULTS, page by page: 0 the level's tier (challenges) or placing (competitions), 1 the park,
     * 2 all levels hardcore, 3 every rider (Amish Boy unlocked), 4 Amish Boy's own ending. Three
     * congratulation lines, then one entry per reward earned in this run (BmxRider.runRewards bits:
     * 2 tier + k level rewards, 6 + tier park rewards, 9/10/11 the finals). */
    const BmxRider *r = &g_riders[g_currentPlayer];
    const SaveGoalInfo *gi;
    uint32_t tier, k, id, n = 0, rider = g_saveProfiles[g_currentPlayer].rider, place;
    int y = 0x6C, idx;

    overlaySetPage((uint8_t)page);
    freeTexts(&g_overlayText[1], 23);
    if (page == 2) {
        id = 0x2198;
    } else if (page < 3 || page > 4) {
        if (g_currentLevel < 6) {
            gi = comdataGetGoalInfo(g_currentPlayer, g_currentLevel);
            tier = gi->tier + (gi->tier_goals_done == 4);
            id = page == 0 ? tier * 3 + 0x215F : (tier + (g_currentLevel / 3u) * 3) * 3 + 0x2171;
        } else {
            place = 0;
            for (k = 0; k < 3; k++)
                if (g_compRanking[k] == rider || (rider > 9 && g_compRanking[k] == 5)) {
                    place = 3 - k;
                    break;
                }
            id = page == 0 ? place * 3 + 0x2168 : (place + (g_currentLevel / 3u) * 3) * 3 + 0x2171;
        }
    } else {
        id = page * 3 + 0x2192;
    }
    for (k = 0; k < 3; k++, y += 0x1D)
        g_overlayText[1 + k] = addCentred(zTextGet(id + k), y, OVL_COL_ITEM);

    switch (page) {
    case 0:
        for (tier = 0; tier < 3; tier++)
            for (k = 0; k < 2; k++)
                if ((r->runRewards & 1u << (tier * 2 + k)) && g_rewards.levelTier[g_currentLevel][tier][k].type != 0)
                    n++;
        break;
    case 1:
        for (tier = 0; tier < 3; tier++)
            if (r->runRewards & 1u << (tier + 6))
                n++;
        break;
    case 2: case 3: case 4:
        if (r->runRewards & 1u << (page + 7))
            n = 1;
        break;
    }
    idx = 4;
    y = y + 0x23 + ((0x187 - (y + 0x23)) >> 1) - ((n * 0x2E) >> 1);
    switch (page) {
    case 0:
        for (tier = 0; tier < 3; tier++)
            for (k = 0; k < 2; k++)
                if (r->runRewards & 1u << (tier * 2 + k))
                    overlayAddReward(g_currentPlayer, &g_rewards.levelTier[g_currentLevel][tier][k], &idx, &y, n == 1);
        break;
    case 1:
        for (tier = 0; tier < 3; tier++)
            if (r->runRewards & 1u << (tier + 6))
                overlayAddReward(g_currentPlayer, &g_rewards.parkTier[g_currentLevel / 3][tier], &idx, &y, n == 1);
        break;
    case 2: case 3: case 4:
        if (r->runRewards & 1u << (page + 7))
            overlayAddReward(g_currentPlayer, &g_rewards.final[page - 2], &idx, &y, n == 1);
        break;
    }
    bmxsoundEvent(SND_REWARDS, page, 0, 0, 0);
}

/* VA 0x004180f0, 724 bytes, [I] between, status draft */
static void overlayAddReward(int player, const GameReward *rw, int *idx, int *y, int single)
{
    /* One reward line: an icon-font letter (with a shadow copy) and two lines of tiny text.
     * The text says what the reward is (answers the game.c pass's open question on types 5-7):
     *   1 bike 0x21BB, 2 outfit 0x21C5 (0x21CF for riders >= 10), 3 secret rider 0x21BD,
     *   4 level 0x21A1, 5 sponsor / magazine / Slim Jim / Amish texts 0x21D5 + 2 g_rewardSponsor
     *   [rider][param], 6 a TIP! 0x2243 (param 12, 13: the Slim Jim sweepstakes, the DMFB2 code),
     *   7 the rider's signature-trick input 0x225F + 2 (2 rider + param), 8 movie 0x228F,
     *   9 -> riderFinal[rider][param], 10 cheat 0x2291 (2 lines each, + 2 param). */
    const SaveProfile *pf = &g_saveProfiles[player];
    char icon[2];
    const char *s;
    uint32_t id, k, w, wmax;
    int x;

    switch (rw->type) {
    case GAME_REWARD_BIKE:   id = rw->param * 2u + 0x21BB; break;
    case GAME_REWARD_STYLE:
        if (pf->rider >= 10) {
            id = 0x21CF;
            goto show;
        }
        id = rw->param * 2u + 0x21C5;
        break;
    case GAME_REWARD_RIDER:  id = rw->param * 2u + 0x21BD; break;
    case GAME_REWARD_LEVEL:  id = rw->param * 2u + 0x21A1; break;
    case GAME_REWARD_SPONSOR: id = g_rewardSponsor[pf->rider][rw->param] * 2u + 0x21D5; break;
    case GAME_REWARD_TIP:    id = rw->param * 2u + 0x2243; break;
    case GAME_REWARD_TRICK_TIP: id = (rw->param + pf->rider * 2u) * 2u + 0x225F; break;
    case GAME_REWARD_MOVIE:  id = rw->param * 2u + 0x228F; break;
    case GAME_REWARD_PER_RIDER:
        overlayAddReward(player, &g_rewards.riderFinal[pf->rider][rw->param], idx, y, single);
        return;
    case GAME_REWARD_CHEAT:  id = rw->param * 2u + 0x2291; break;
    default:
        return;
    }
show:
    icon[0] = s_rewardIcon[rw->type];
    icon[1] = '\0';
    if (!single) {
        x = 0x56;
    } else {
        for (k = 0, wmax = 0; k < 2; k++) {
            w = (uint16_t)tinyWidth(zTextGet(id + k));
            if (wmax <= w)
                wmax = w;
        }
        x = 0x13F - ((wmax + 0x28) >> 1);
    }
    /* drawn black (blend 2 / 3), faded in by overlayResultsDraw */
    g_overlayText[(*idx)++] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayIconFont, icon, (int16_t)x,
                                             (int16_t)(*y - (g_overlayIconFont.height >> 1) + 0x15), 0x02000000, 2);
    g_overlayText[(*idx)++] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayIconFont, icon, (int16_t)x,
                                             (int16_t)(*y - (g_overlayIconFont.height >> 1) + 0x15), 0x03000000, 0);
    for (k = 0; k < 2; k++) {
        s = zTextGet(id + k);
        g_overlayText[(*idx)++] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s, (int16_t)(x + 0x28),
                                                 (int16_t)*y, 0x02000000, 0);
        *y += k == 0 ? 0x14 : 0x1A;
    }
}

/* VA 0x004184b0, 281 bytes, [I] row, status draft */
static int overlayResultsUpdate(void)
{
    /* once every reward line is shown: the next page with rewards, else WRITE SAVE.DAT
     * (memcard_WriteSave) and go to RUN FINISHED with "SAVING GAME DATA..." */
    const BmxRider *r = &g_riders[g_currentPlayer];
    uint32_t lines = 0, rider = g_saveProfiles[g_currentPlayer].rider;
    const uint16_t *h;

    for (h = &g_overlayText[4]; *h != ZOVER_NO_HANDLE; h += 4)
        lines++;
    if ((g_overlayFrame >> 4) < lines || !overlayConfirmed(0))
        return 0;
    switch (g_overlayPage) {
    case 0:
        if (r->runRewards & 0x1C0) { overlayResultsPage(1); goto done; }
        break;
    case 1:
        if (r->runRewards & 0x200) { overlayResultsPage(2); goto done; }
        break;
    case 2:
        if (rider == 10)
            break;
        if (rider == 11) {
            if (r->runRewards & 0x800) { overlayResultsPage(4); goto done; }
        } else if (r->runRewards & 0x400) {
            overlayResultsPage(3);
            goto done;
        }
        break;
    }
    memcard_WriteSave();
    overlaySetScreen(OVL_SCREEN_RUN_OVER);
    g_gameSaveDone = 1;
done:
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* VA 0x00418620, 384 bytes, [I] row, status draft */
static void overlayResultsDraw(void)
{
    /* reveal the reward lines one per 16 frames (a tick each), then the "continue" item */
    uint32_t row = 0, step = g_overlayFrame >> 4, l, grey;
    uint16_t *h;
    int set;

    for (h = &g_overlayText[4]; h[0] != ZOVER_NO_HANDLE; h += 4, row++) {
        step = g_overlayFrame >> 4;
        set = 1;
        if (step < row) {
            l = 0;
        } else if (step == row) {
            l = (uint32_t)(int)((float)(int16_t)(g_overlayFrame & 0xF) * 0.6f) * 0x20;
            if (l > 0xF0)
                l = 0xF0;
            if ((g_overlayFrame & 0xF) == 1)
                bmxsoundEvent(SND_TICK, 0, 0, 0, 0);
        } else if (step == row + 1 && (g_overlayFrame & 0xF) == 0) {
            l = 0xF0;
        } else {
            set = 0;
        }
        if (!set)
            continue;
        zoveriiSetTextColour(h[0], &g_overlayIconFont, ((0x400u | l) << 8 | l) << 7 | l >> 1, 2);
        zoveriiSetTextColour(h[1], &g_overlayIconFont, ((0xC00u | l) << 8 | l) << 6 | l >> 2, 0);
        grey = ZOVER_GREY(2, l);
        zoveriiSetTextColour(h[2], &g_overlayTinyFont, grey, 0);
        zoveriiSetTextColour(h[3], &g_overlayTinyFont, grey, 0);
    }
    if (row <= step)
        overlayDrawMenu(8, 0);
}

/* ================================================================== name entry and records */

/* VA 0x00418800, 65 bytes, [I] row, status draft */
static void overlayNameEnter(int on)
{
    /* start from the profile's name; the cursor on its last letter */
    if (on) {
        zstringPrintf(g_overlayName, "%s", g_saveProfiles[g_currentPlayer].name);
        g_overlayNameCursor = (uint8_t)(zstringLength(g_overlayName) - 1);
    }
}

/* VA 0x00418870, 695 bytes, [I] row, status draft */
static int overlayNameUpdate(void)
{
    /* NEW RECORD! ENTER YOUR NAME. Up / down cycle the letter under the cursor through
     * ' ' A..Z a..z; right copies it one place on (at most 10 letters); left or square deletes.
     * Start / ok: every beaten all-time record of this mode (live slot 0, flag bit 0) is copied into
     * SAVE.DAT block B (comdataGetRecordPtr: value, the player's rider / options +6 / +7, the name),
     * and the name becomes the profile name too. Then the RECORDS screen (19). */
    GameLiveRecord *live;
    SaveRecordEntry *rec;
    const uint8_t *opt;
    uint32_t p, stat, snd;
    char *c;

    for (p = 0; p < g_numPlayers; p++) {
        if (g_overlayCurrentOnly && p != g_currentPlayer)
            continue;
        if (contrlerTestButtons(p, PAD_START, 0) || contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0)) {
            for (stat = 0; stat < 10; stat++) {
                live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
                if (live == NULL || !(live->flags & 1))
                    continue;
                rec = comdataGetRecordPtr(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
                rec->value = live->value;
                opt = zrollerGetPlayer(p);
                rec->rider = opt[4];
                rec->b5 = opt[6];
                rec->b6 = opt[7];
                zstringPrintf(rec->name, "%s", g_overlayName);
                zstringPrintf(g_saveProfiles[g_currentPlayer].name, "%s", g_overlayName);
            }
            overlaySetScreen(OVL_SCREEN_RECORDS_NEW);
            bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        c = &g_overlayName[g_overlayNameCursor];
        if (contrlerTestButtons(p, PAD_LEFT, 0) || contrlerTestButtons(p, PAD_FIRE2, 0)) {
            if (g_overlayNameCursor == 0) {
                snd = SND_ERROR;
            } else {
                *c = '\0';
                g_overlayNameCursor--;
                snd = SND_DELETE;
            }
        } else if (contrlerTestButtons(p, PAD_RIGHT, 0)) {
            if (g_overlayNameCursor >= 9) {
                snd = SND_ERROR;
            } else {
                c[1] = c[0];
                g_overlayNameCursor++;
                snd = SND_SELECT;
            }
        } else if (contrlerTestButtons(p, PAD_UP, 0)) {
            *c = *c == ' ' ? 'A' : *c == 'Z' ? 'a' : *c == 'z' ? ' ' : (char)(*c + 1);
            snd = SND_CHANGE;
        } else if (contrlerTestButtons(p, PAD_DOWN, 0)) {
            *c = *c == ' ' ? 'z' : *c == 'A' ? ' ' : *c == 'a' ? 'Z' : (char)(*c - 1);
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

/* VA 0x00418be0, 318 bytes, [I] row, status draft */
static void overlayNameDraw(void)
{
    /* the letters 0x35 apart, the cursor letter pulsing */
    char buf[64];
    uint32_t n, i;
    int x;

    drawCentred(&g_overlaySmallFont, zTextGet(0x1091), 0x6C, OVL_COL_ITEM);
    zstringPrintf(buf, "%s:", zTextGet(0x1092));
    drawCentred(&g_overlaySmallFont, buf, 0x89, OVL_COL_ITEM);
    n = (uint32_t)zstringLength(g_overlayName);
    x = 0x13F - (n * 0x35 >> 1);
    for (i = 0; i < n; i++, x += 0x35) {
        zstringPrintf(buf, "%c", g_overlayName[i]);
        zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(x - (smallWidth(buf) >> 1) + 0x1A), 0xE2,
                        itemColour(i, g_overlayNameCursor, OVL_COL_ITEM), 0);
    }
}

/* VA 0x00418d70, 623 bytes, [I] row, status draft */
static void overlayRecordsEnter(int on)
{
    /* RECORDS of this mode, stats 1..9 (best run ... longest skid): label, the rider's full name
     * from the PLYR table (zresGetPlayer(rider * 5): style 0's entry), the value (a distance
     * for stats 4..9 and the stat games), and the name typed at the name entry. */
    const SaveRecordEntry *rec;
    char buf[32];
    uint32_t stat;
    int y = 0x8D;

    if (!on) {
        for (stat = 1; stat < 10; stat++)
            freeTexts(g_overlayRecordText[stat], 4);
        return;
    }
    g_overlayText[0] = addCentred(zTextGet(0x1093), 0x3C, OVL_COL_TITLE);
    for (stat = 1; stat < 10; stat++, y += 0x16) {
        rec = comdataGetRecordPtr(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
        zstringPrintf(buf, "%s:", zTextGet(0x1094 + stat));
        g_overlayRecordText[stat][0] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, buf, 0x35, (int16_t)y,
                                                      OVL_COL_LABEL, 0);
        g_overlayRecordText[stat][3] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont,
                                                      (const char *)zresGetPlayer((uint16_t)(rec->rider * 5)) + 8,
                                                      0xD0, (int16_t)y, 0x02D09010, 0);
        if (stat == 1 ? (gameGetMode() >= GAME_MODE_LONGEST_GRIND && gameGetMode() <= GAME_MODE_LONGEST_MANUAL)
                      : (stat >= 4 && stat <= 9))
            overlayFormatDistance(buf, (float)rec->value);
        else
            zstringPrintf(buf, "%ld", rec->value);
        g_overlayRecordText[stat][1] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, buf,
                                                      (int16_t)RIGHT_X((uint16_t)tinyWidth(buf)), (int16_t)y, OVL_COL_GREY, 0);
        zstringPrintf(buf, "%s", rec->name);
        g_overlayRecordText[stat][2] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, buf,
                                                      (int16_t)(0x1AF - ((uint16_t)tinyWidth(buf) >> 1)), (int16_t)y,
                                                      OVL_COL_GREY, 0);
    }
}

/* VA 0x00419080, 429 bytes, [I] row, status draft */
static int overlayRecordsUpdate(void)
{
    /* 18/20/21: back where we came from. 19 (after the name entry): competitions -> STANDINGS,
     * a completed tier -> RESULTS, multiplayer -> standings / next turn; otherwise write SAVE.DAT
     * (memcard_WriteSave) and go to RUN FINISHED. */
    uint32_t next, mode;

    if (!overlayConfirmed(0)) {
        if (!anyPlayerPressed(PAD_FIRE1 | PAD_FIRE5))
            return 0;
        if (g_overlayScreenId == OVL_SCREEN_RECORDS_PAUSE)
            overlaySetScreen(OVL_SCREEN_PAUSE);
        else if (g_overlayScreenId == OVL_SCREEN_RECORDS_JUDGES)
            overlaySetScreen(OVL_SCREEN_JUDGES);
        else if (g_overlayScreenId == OVL_SCREEN_RECORDS_RUN)
            overlaySetScreen(OVL_SCREEN_RUN_OVER);
        bmxsoundEvent(SND_BACK, 0, 0, 0, 0);
        return 1;
    }
    switch (g_overlayScreenId) {
    case OVL_SCREEN_RECORDS_PAUSE:  next = OVL_SCREEN_PAUSE;    break;
    case OVL_SCREEN_RECORDS_JUDGES: next = OVL_SCREEN_JUDGES;   break;
    case OVL_SCREEN_RECORDS_RUN:    next = OVL_SCREEN_RUN_OVER; break;
    case OVL_SCREEN_RECORDS_NEW:
        mode = gameGetMode();
        if (mode == GAME_MODE_SINGLE) {
            if (gameGetSubMode() == GAME_SUB_PROQUEST) {
                if (g_currentLevel > 5) {
                    overlaySetScreen(OVL_SCREEN_STANDINGS);
                    goto done;
                }
                if (gameIsTierComplete(g_currentPlayer)) {
                    overlaySetScreen(OVL_SCREEN_RESULTS);
                    goto done;
                }
            }
        } else if (mode < GAME_MODE_WIPEOUT || mode > GAME_MODE_LONGEST_MANUAL) {
            if (!gameTestFlag(GAME_FLAG_OVER)) {
                overlaySetScreen(OVL_SCREEN_MP_STANDINGS);
                goto done;
            }
        } else if (!gameTestFlag(GAME_FLAG_OVER)) {
            overlaySetScreen(OVL_SCREEN_NONE);
            gameSetState(GAME_STATE_TURN);
            goto done;
        }
        memcard_WriteSave();
        overlaySetScreen(OVL_SCREEN_RUN_OVER);
        g_gameSaveDone = 1;
        goto done;
    default:
        goto done;
    }
    overlaySetScreen(next);
done:
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* VA 0x004192b0, 148 bytes, [I] row, status draft */
static void overlayRecordsDraw(void)
{
    /* the records beaten in this run blink red (value and name) */
    const GameLiveRecord *live;
    uint32_t stat;

    if (g_overlayScreenId != OVL_SCREEN_RECORDS_PAUSE) {
        for (stat = 1; stat < 10; stat++) {
            live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), stat, 0, 0));
            if (live->flags & 1) {
                zoveriiSetTextColour(g_overlayRecordText[stat][1], &g_overlayTinyFont,
                                     (g_overlayFrame & 1) ? OVL_COL_NEW : OVL_COL_GREY, 0);
                zoveriiSetTextColour(g_overlayRecordText[stat][2], &g_overlayTinyFont,
                                     (g_overlayFrame & 1) ? OVL_COL_NEW : OVL_COL_GREY, 0);
            }
        }
    }
    overlayDrawMenu(8, 0);
}

/* ================================================================== confirm, competition */

/* VA 0x00419380, 251 bytes, [I] row, status draft */
static void overlayConfirmEnter(int on)
{
    /* "END RUN?" during a run, else "EXIT?"; no / yes */
    uint32_t i;
    int y;

    if (!on)
        return;
    g_overlayText[23] = addCentred(zTextGet(gameGetState() == GAME_STATE_RUN ? 0x109F : 0x109E), 0x3C, OVL_COL_TITLE);
    y = 0xF0 - (g_overlayScreen->count * 0x1D >> 1);
    for (i = 0; i < g_overlayScreen->count; i++, y += 0x1D)
        g_overlayText[i] = addCentred(zTextGet(0x10A0 + i), y, OVL_COL_ITEM);
}

/* VA 0x004194c0, 237 bytes, [I] row, status draft */
static int overlayConfirmUpdate(void)
{
    /* no / back: the screen under the question. yes: during a run end it (gameEndRun); otherwise
     * leave the level (zsystemSetMenuMode, back to the menus) - in multiplayer from the game screens
     * first back to the game select with the riders reset. */
    uint32_t p, slot;

    for (p = 0; p < g_numPlayers; p++) {
        if (g_overlayCurrentOnly && p != g_currentPlayer)
            continue;
        if (contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0) || contrlerTestButtons(p, PAD_START, 0)) {
            if (g_overlayScreen->sel == 0)
                goto no;
            if (g_overlayScreen->sel != 1)
                return 1;
            switch (gameGetState()) {
            case GAME_STATE_START:
            case GAME_STATE_TURN:
            case GAME_STATE_RUN_OVER:
            case GAME_STATE_LEVEL_END:
                if (gameGetMode() == GAME_MODE_SINGLE) {
                    overlaySetScreen(OVL_SCREEN_NONE);
                    if (g_currentLevel < 6)
                        gameAdvanceGoalTier(g_currentPlayer);
                } else if (g_overlayConfirmReturn != OVL_SCREEN_MP_SELECT) {
                    for (slot = 0; slot < g_numPlayers; slot++)
                        riderReset((int)slot, 0);
                    cameraSetPlayer(0);
                    cameraSetMode(0, 4);
                    overlaySetScreen(OVL_SCREEN_MP_SELECT);
                    goto done;
                } else {
                    overlaySetScreen(OVL_SCREEN_NONE);
                }
                break;
            case GAME_STATE_RUN:
                gameEndRun();
                goto done;
            }
            zsystemSetMenuMode();
done:
            bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        if (contrlerTestButtons(p, PAD_FIRE1 | PAD_FIRE5, 0))
            goto no;
    }
    return 0;
no:
    overlaySetScreen(g_overlayConfirmReturn);
    bmxsoundEvent(SND_BACK, 0, 0, 0, 0);
    return 1;
}

/* the judges' scores are bytes 0..255 shown as 0.0 .. 100.0 ("%02d.%d") */
static void formatJudge(char *buf, uint8_t v)
{
    float f = (float)v * 0.39215687f;
    int whole = (int)f;

    zstringPrintf(buf, "%02d.%d", whole, (int)((f - (float)whole) * 10.0f));
}

/* fade-in level of reveal step 'i' (16 frames each) */
static uint8_t revealLevel(uint32_t i)
{
    int l;

    if ((g_overlayFrame >> 4) < i)
        return 0;
    if ((g_overlayFrame >> 4) > i)
        return 0xF0;
    l = (int)((float)(int16_t)(g_overlayFrame & 0xF) * 1.5f);
    if (l > 0xF)
        l = 0xF;
    if ((g_overlayFrame & 0xF) == 1)
        bmxsoundEvent(SND_TICK, 0, 0, 0, 0);
    return (uint8_t)(l << 4);
}

/* VA 0x004196c0, 1193 bytes, [I] row, status draft */
static void overlayStandingsDraw(void)
{
    /* STANDINGS: POS, NAME (the PLYR full name of each competitor, zresGetPlayer(rider * 5)),
     * the run scores and the average, ten lines revealed one by one; the player's rider in orange.
     * Riders 10/11 (Slim Jim Guy, Amish Boy) take rider 5's place in the field. */
    char buf[32];
    uint32_t pos, rider, run, me = g_saveProfiles[g_currentPlayer].rider, colour;
    uint8_t l;
    int y = 0x89, x;

    overlympDrawTitle(zTextGet(0x10A2));
    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A3), 0x35, 0x72, OVL_COL_LABEL, 0);
    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A4), (int16_t)(0xBA - (tinyWidth(zTextGet(0x10A4)) >> 1)), 0x72,
                    OVL_COL_LABEL, 0);
    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A5), (int16_t)(0x189 - (tinyWidth(zTextGet(0x10A5)) >> 1)), 0x72,
                    OVL_COL_LABEL, 0);
    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A6), (int16_t)RIGHT_X(tinyWidth(zTextGet(0x10A6))), 0x72,
                    OVL_COL_LABEL, 0);
    for (pos = 0; pos < 10; pos++, y += 0x17) {
        rider = g_compRanking[pos];
        if (me > 9 && rider == 5)
            rider = me;
        l = revealLevel(pos);
        colour = rider == me ? ZOVER_COLOUR(2, ZOVER_RGB(l * 0xD0 >> 8, l * 0x90 >> 8, l >> 4)) : ZOVER_GREY(2, l);
        zstringPrintf(buf, "%ld", pos + 1);
        zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(0x40 - (tinyWidth(buf) >> 1)), (int16_t)y, colour, 0);
        zstringPrintf(buf, "%s", (const char *)zresGetPlayer((uint16_t)(rider * 5)) + 8);
        zoveriiDrawText(&g_overlayTinyFont, buf, 0x8D, (int16_t)y, colour, 0);
        for (run = 0, x = 0x171; run < (uint32_t)gameGetRound() + 1; run++, x += 0x2D) {
            formatJudge(buf, g_judgeScores[rider > 9 ? 5 : rider][run]);
            zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(x - (tinyWidth(buf) >> 1) + 1), (int16_t)y,
                            ZOVER_GREY(3, l), 0);
            if (l != 0)
                zoveriiDrawRect((int16_t)(x - 0x15), (int16_t)y, 0x2B, 0x16, 0xC0C0C0);
        }
        formatJudge(buf, g_judgeScores[rider > 9 ? 5 : rider][2]);
        zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(0x235 - (tinyWidth(buf) >> 1)), (int16_t)y,
                        ZOVER_GREY(3, l), 0);
        if (l != 0)
            zoveriiDrawRect(0x21F, (int16_t)y, 0x2B, 0x16, 0x903030);
    }
    overlayDrawMenu(8, 0);
}

/* VA 0x00419ca0, 191 bytes, [I] row, status draft */
static int overlayStandingsUpdate(void)
{
    /* after the last run: placed in the top three -> RESULTS; else write SAVE.DAT, RUN FINISHED.
     * After the first run: the judges' screen. */
    uint32_t k;
    uint8_t rider;

    if (!overlayConfirmed(0))
        return 0;
    rider = g_saveProfiles[g_currentPlayer].rider;
    if (rider > 9)
        rider = 5;
    if (!gameTestFlag(GAME_FLAG_OVER)) {
        overlaySetScreen(OVL_SCREEN_JUDGES);
    } else {
        for (k = 0; k < 3; k++)
            if (g_compRanking[k] == rider) {
                overlaySetScreen(OVL_SCREEN_RESULTS);
                goto done;
            }
        memcard_WriteSave();
        overlaySetScreen(OVL_SCREEN_RUN_OVER);
        g_gameSaveDone = 1;
    }
done:
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* VA 0x00419d90, 222 bytes, [I] row, status draft */
static void overlayJudgesDraw(void)
{
    /* FIRST RUN: the judges' scores, then next run / view replay / trick list / view records / exit */
    uint32_t i;
    int y;

    gameGetRound();
    overlympDrawTitle(zTextGet(0x10A7));
    y = 0x6C;
    overlayDrawJudgeScores(&y);
    y = 0x1A4 - g_overlayScreen->count * 0x1D;
    if (g_overlayInputDelay < g_overlayFrame)
        for (i = 0; i < g_overlayScreen->count; i++, y += 0x1D)
            drawCentred(&g_overlaySmallFont, zTextGet(0x10AA + i), y, itemColour(i, g_overlayScreen->sel, OVL_COL_ITEM));
}

/* VA 0x00419eb0, 946 bytes, [I] between, status draft */
static void overlayDrawJudgeScores(int *y)
{
    /* JUDGES' SCORES ... AVG: one line per run (BmxJudgeRun: five judges + average), the
     * current run revealed column by column; with two runs, the overall average in orange */
    const BmxRider *r = &g_riders[g_currentPlayer];
    char buf[32];
    uint32_t run, k, me;
    uint8_t l;
    int x;

    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A8), (int16_t)(0xF0 - (tinyWidth(zTextGet(0x10A8)) >> 1)),
                    (int16_t)*y, OVL_COL_GREY, 0);
    zoveriiDrawText(&g_overlayTinyFont, zTextGet(0x10A9), (int16_t)(0x222 - (tinyWidth(zTextGet(0x10A9)) >> 1)),
                    (int16_t)*y, OVL_COL_GREY, 0);
    *y += 0x17;
    if (gameGetRound() == 0xFF)
        return;
    for (run = 0; run < (uint32_t)gameGetRound() + 1; run++) {
        for (k = 0, x = 0x53; k < 6; k++) {
            formatJudge(buf, ((const uint8_t *)&r->judge[run].judge[0])[k]);  /* [5] = the average (+9) */
            l = run == gameGetRound() ? revealLevel(k) : 0xF0;
            zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(x - (tinyWidth(buf) >> 1) + 1), (int16_t)*y,
                            ZOVER_GREY(3, l), 0);
            if (l != 0)
                zoveriiDrawRect((int16_t)(x - 0x15), (int16_t)*y, 0x2B, 0x16, k < 5 ? 0xC0C0C0 : 0x903030);
            x += k < 4 ? 0x4A : 0x94;
        }
        *y += 0x17;
    }
    if (run < 2)
        return;
    me = g_saveProfiles[g_currentPlayer].rider;
    formatJudge(buf, g_judgeScores[me > 9 ? 5 : me][2]);
    if ((g_overlayFrame & ~0xFu) < 0x60) {
        l = 0;
    } else if ((g_overlayFrame & ~0xFu) == 0x60) {
        l = (uint8_t)(((g_overlayFrame & 0xF) < 0xD ? (g_overlayFrame & 0xF) : 0xC) << 4);
        if ((g_overlayFrame & 0xF) == 1)
            bmxsoundEvent(SND_TICK, 0, 0, 0, 0);
    } else {
        l = 0xC0;
    }
    zoveriiDrawText(&g_overlayTinyFont, buf, (int16_t)(0x210 - (tinyWidth(buf) >> 1)), (int16_t)*y, ZOVER_GREY(3, l), 0);
    if (l != 0)
        zoveriiDrawRect(0x1FA, (int16_t)*y, 0x2B, 0x16, 0xA07010);
    *y += 0x17;
}

/* VA 0x0041a360, 105 bytes, [I] row, status draft */
static int overlayJudgesUpdate(void)
{
    if (!contrlerTestButtons(g_currentPlayer, PAD_FIRE4 | PAD_FIRE6, 0) &&
        !contrlerTestButtons(g_currentPlayer, PAD_START, 0))
        return 0;
    switch (g_overlayScreen->sel) {
    case 0:                                         /* next run */
        overlaySetScreen(OVL_SCREEN_NONE);
        gameSetState(GAME_STATE_TURN);
        break;
    case 1:                                         /* view replay */
        overlaySetScreen(OVL_SCREEN_NONE);
        actionSetMode(2);
        g_actionPausePlayer = g_currentPlayer;
        break;
    case 2: overlaySetScreen(OVL_SCREEN_TRICKS_JUDGES); break;
    case 3: overlaySetScreen(OVL_SCREEN_RECORDS_JUDGES); break;
    case 4:                                         /* exit to main menu */
        g_overlayConfirmReturn = OVL_SCREEN_JUDGES;
        overlaySetScreen(OVL_SCREEN_CONFIRM);
        g_gameNextMode = 0;
        break;
    }
    bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
    return 1;
}

/* ================================================================== run finished */

enum { RUN_AGAIN, RUN_LEVEL_SELECT, RUN_GEAR_UP, RUN_GEAR_UP2, RUN_SAVE, RUN_REPLAY, RUN_TRICKS, RUN_RECORDS, RUN_EXIT };

/* VA 0x0041a480, 748 bytes, [I] row, status draft */
static void overlayRunOverEnter(int on)
{
    /* RUN FINISHED (single player) / winner! / tie: another run or play again, level select
     * (two levels unlocked), gear up (new bike / style: notice bits 0x40 / 0x80, or more than one
     * unlocked), view replay, trick list, view records, exit. "save game" (0x10B4) is never added. */
    const SaveRiderRecord *rec;
    uint32_t id;

    if (!on)
        return;
    if (gameGetMode() == GAME_MODE_SINGLE)
        id = 0x10AF;
    else
        id = gameTestFlag(GAME_FLAG_TIE) ? 0x1019 : 0x3052;
    g_overlayText[23] = addCentred(zTextGet(id), 0x3C, OVL_COL_TITLE);
    g_overlayScreen->count = 0;
    if (gameGetMode() == GAME_MODE_SINGLE) {
        rec = &g_saveBlockA.riders[g_saveProfiles[g_currentPlayer].rider];
        menuAdd(g_overlayRunOverMenu, RUN_AGAIN, 0x10B0);
        if (gameGetSubMode() == GAME_SUB_PROQUEST) {
            if (rec->levels_unlocked > 1)
                menuAdd(g_overlayRunOverMenu, RUN_LEVEL_SELECT, 0x10B2);
            if (rec->flags & 0x40) {
                menuAdd(g_overlayRunOverMenu, RUN_GEAR_UP, 0x10B3);
            } else if (rec->flags & 0x80) {
                menuAdd(g_overlayRunOverMenu, RUN_GEAR_UP2, 0x10B3);
            }
        } else {
            if (rec->levels_unlocked > 1)
                menuAdd(g_overlayRunOverMenu, RUN_LEVEL_SELECT, 0x10B2);
            if (rec->bikes_unlocked > 1)
                menuAdd(g_overlayRunOverMenu, RUN_GEAR_UP, 0x10B3);
            else if (rec->styles_unlocked > 1)
                menuAdd(g_overlayRunOverMenu, RUN_GEAR_UP2, 0x10B3);
        }
        menuAdd(g_overlayRunOverMenu, RUN_REPLAY, 0x10B5);
    } else {
        menuAdd(g_overlayRunOverMenu, RUN_AGAIN, 0x10B1);
        if (!gameTestFlag(GAME_FLAG_TIE))
            menuAdd(g_overlayRunOverMenu, RUN_REPLAY, 0x10B5);
    }
    menuAdd(g_overlayRunOverMenu, RUN_TRICKS, 0x10B6);
    menuAdd(g_overlayRunOverMenu, RUN_RECORDS, 0x10B7);
    menuAdd(g_overlayRunOverMenu, RUN_EXIT, gameGetMode() == GAME_MODE_SINGLE ? 0x10B8 : 0x10B9);
}

/* VA 0x0041a830, 969 bytes, [V] direct, status draft */
static void overlayRunOverDraw(void)
{
    /* the scores of the run (per player in multiplayer: green for the winner, the record holder
     * blinking red), competitions: the judges' scores; then the menu */
    const GameLiveRecord *live;
    char buf[32];
    uint32_t i, winner, colour;
    int y;

    if (gameGetMode() == GAME_MODE_HIGH_FIVE) {
        overlympDrawTitle(zTextGet(gameTestFlag(GAME_FLAG_TIE) ? 0x1019 : 0x3052));
        y = 0x89;
        overlympDrawRoundLeader(0x89);
    } else if (gameGetMode() == GAME_MODE_BMX) {
        overlympDrawTitle(zTextGet(0x3052));
        y = 0x6C;
        overlympDrawLetters(&y);
    } else {
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), g_gameModeStat[gameGetMode()], 1, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAY_SRC, 0x125D, NULL);
        winner = live->player;
        if (gameGetMode() != GAME_MODE_SINGLE && gameTestFlag(GAME_FLAG_TIE))
            winner = 0xFFFFFFFF;
        y = 0x6C;
        live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), g_gameModeStat[gameGetMode()], 0, 0));
        if (live == NULL)
            zAssertFail(g_overlayEmpty, OVERLAY_SRC, 0x126A, NULL);
        if (gameGetMode() != GAME_MODE_SINGLE || gameGetSubMode() != GAME_SUB_FREE_RIDE) {
            if (gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_PROQUEST && g_currentLevel > 5) {
                overlayDrawJudgeScores(&y);
            } else {
                for (i = 0; i < g_numPlayers; i++, y += 0x1D) {
                    if (gameGetMode() == GAME_MODE_SINGLE)
                        zstringPrintf(buf, "%s:", zTextGet(0x1002));
                    else
                        zstringPrintf(buf, "%s %s:", zTextGet(0x1000), zTextGet(0x100C + i));
                    zoveriiDrawText(&g_overlaySmallFont, buf, 0x35, (int16_t)y, OVL_COL_ITEM, 0);
                    if (gameGetMode() >= GAME_MODE_LONGEST_GRIND && gameGetMode() <= GAME_MODE_LONGEST_MANUAL)
                        overlayFormatDistance(buf, (float)g_riders[i].score.totalScore);
                    else
                        zstringPrintf(buf, "%ld", g_riders[i].score.totalScore);
                    colour = i == winner ? 0x00E0A040u : 0x00D0D0D0u;
                    if ((live->flags & 1) && live->player == i && (g_overlayFrame & 1))
                        colour = OVL_COL_NEW;
                    zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)RIGHT_X(smallWidth(buf)), (int16_t)y, colour, 0);
                }
            }
        }
    }
    y = 0x1A4 - g_overlayScreen->count * 0x1D;
    if (g_overlayInputDelay < g_overlayFrame)
        for (i = 0; i < g_overlayScreen->count; i++, y += 0x1D)
            drawCentred(&g_overlaySmallFont, zTextGet(g_overlayRunOverMenu[i].text), y,
                        itemColour(i, g_overlayScreen->sel, OVL_COL_ITEM));
}

/* VA 0x0041ad00, 691 bytes, [I] row, status draft */
static int overlayRunOverUpdate(void)
{
    /* L1 + Start dumps the run's replay to s:\bmx\assets\PC\replay\demo<level><rider>.rpy (a
     * developer path: how the demo replays in replay.zal were made); the menu otherwise */
    char path[64];
    void *start;
    uint32_t size, p, slot;
    zfileHandle *h;

    for (p = 0; p < g_numPlayers; p++) {
        if (contrlerTestButtons(g_currentPlayer, PAD_LTRIG1, 2) && contrlerTestButtons(g_currentPlayer, PAD_START, 0)) {
            bmxreplayGetSaveArea(gameGetViewReplay(), &start, &size);
            zstringPrintf(path, "s:\\bmx\\assets\\%s\\replay\\demo%02d%02d.rpy", "PC", g_currentLevel,
                          g_saveProfiles[g_currentPlayer].rider);
            h = zfileOpen(path, 2, NULL);
            if (h == NULL || (uint32_t)zfileWrite(h, start, (int)size, NULL) != size)
                bmxsoundEvent(SND_ERROR, 0, 0, 0, 0);
            else
                bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        if (!contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0) && !contrlerTestButtons(p, PAD_START, 0))
            continue;
        switch (g_overlayRunOverMenu[g_overlayScreen->sel].action) {
        case RUN_AGAIN:
            overlaySetScreen(OVL_SCREEN_NONE);
            if (gameGetMode() != GAME_MODE_SINGLE)
                gameSetSubMode(g_overlympSubMode);
            gameStart(gameGetMode());
            gameSetState(GAME_STATE_TURN);
            break;
        case RUN_LEVEL_SELECT:      /* EXIT?, then the frontend opens on page g_gameNextMode */
            g_overlayConfirmReturn = OVL_SCREEN_RUN_OVER;
            overlaySetScreen(OVL_SCREEN_CONFIRM);
            g_gameNextMode = 3;
            g_overlayCurrentOnly = 0;
            break;
        case RUN_GEAR_UP:
            g_overlayConfirmReturn = OVL_SCREEN_RUN_OVER;
            overlaySetScreen(OVL_SCREEN_CONFIRM);
            g_gameNextMode = 1;
            g_overlayCurrentOnly = 0;
            break;
        case RUN_GEAR_UP2:
            g_overlayConfirmReturn = OVL_SCREEN_RUN_OVER;
            overlaySetScreen(OVL_SCREEN_CONFIRM);
            g_gameNextMode = 2;
            g_overlayCurrentOnly = 0;
            break;
        case RUN_SAVE:
            overlaySetScreen(OVL_SCREEN_SAVE);
            break;
        case RUN_REPLAY:
            overlaySetScreen(OVL_SCREEN_NONE);
            actionSetMode(2);
            g_actionPausePlayer = (uint8_t)p;
            break;
        case RUN_TRICKS:
            overlaySetScreen(OVL_SCREEN_TRICKS_RUN);
            break;
        case RUN_RECORDS:
            overlaySetScreen(OVL_SCREEN_RECORDS_RUN);
            break;
        case RUN_EXIT:
            if (gameGetMode() == GAME_MODE_SINGLE) {
                g_overlayConfirmReturn = OVL_SCREEN_RUN_OVER;
                overlaySetScreen(OVL_SCREEN_CONFIRM);
                g_gameNextMode = 0;
            } else {
                for (slot = 0; slot < g_numPlayers; slot++)
                    riderReset((int)slot, 0);
                cameraSetPlayer(0);
                cameraSetMode(0, 4);
                overlaySetScreen(OVL_SCREEN_MP_SELECT);
            }
            g_overlayCurrentOnly = 0;
            break;
        }
        bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
        return 1;
    }
    return 0;
}

/* ================================================================== save (memory card) */

/* VA 0x0041b0a0, 148 bytes, [I] row, status draft */
static void overlaySaveEnter(int on)
{
    /* SAVE: the PSX memory-card dialogue, kept on PC with stub card functions. Unreachable: the
     * run-over menu never offers "save game" (action 4). */
    if (!on) {
        zcardStop();
        bmxsoundEvent(SND_MUSIC, 2, 0, 0, 0);
        return;
    }
    bmxsoundEvent(SND_MUSIC, 3, 0, 0, 0);
    g_overlayText[0] = addCentred(zTextGet(0x110F), 0x3C, OVL_COL_TITLE);
    g_overlayPage = 0;
    overlaySaveRefresh();
    zcardStart();
}

/* VA 0x0041b170, 852 bytes, [I] row, status draft */
static int overlaySaveUpdate(void)
{
    /* Pages (texts 0x1112 + 4 page): 0 choose slot, 1 no card, 2 not formatted (format?), 3 format
     * complete, 4 not enough blocks, 5 save game?, 6 overwrite?, 7 save complete, 8 card error,
     * 9 checking..., 10 formatting..., 11 saving data... The busy pages run the card calls. */
    const OverlaySavePage *pg = &g_overlaySavePages[g_overlayPage];
    uint32_t p, next;
    int r, ok;

    for (p = 0; p < g_numPlayers; p++) {
        if (g_overlayCurrentOnly && p != g_currentPlayer)
            continue;
        ok = contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0);
        if (ok && g_overlayScreen->sel == 0) {
            switch (g_overlayPage) {
            case 0:                 next = 9;  break;
            case 1: case 4: case 8: next = 0;  break;
            case 2:                 next = 10; break;
            case 3:                 next = 5;  break;
            case 5: case 6:         next = 11; break;
            case 7:
                overlaySetScreen(OVL_SCREEN_RUN_OVER);
                goto selected;
            default:
                goto other;
            }
            overlaySetPage((uint8_t)next);
            overlaySaveRefresh();
selected:
            bmxsoundEvent(SND_SELECT, 0, 0, 0, 0);
            return 1;
        }
        if (contrlerTestButtons(p, PAD_FIRE1 | PAD_FIRE5, 0) || (ok && g_overlayScreen->sel == 1)) {
            switch (g_overlayPage) {
            case 0:
                overlaySetScreen(OVL_SCREEN_RUN_OVER);
                break;
            case 1: case 2: case 4: case 5: case 6: case 8:
                overlaySetPage(0);
                overlaySaveRefresh();
                break;
            default:
                goto other;
            }
            bmxsoundEvent(SND_SELECT + (contrlerTestButtons(p, PAD_FIRE1 | PAD_FIRE5, 0) != 0), 0, 0, 0, 0);
            return 1;
        }
other:
        if ((contrlerTestButtons(p, PAD_LEFT, 0) || contrlerTestButtons(p, PAD_RIGHT, 0)) && g_overlayPage == 0) {
            g_overlaySaveSlot = (uint8_t)!contrlerTestButtons(p, PAD_LEFT, 0);
            overlaySaveRefresh();
            bmxsoundEvent(SND_CHANGE, 0, 0, 0, 0);
            g_overlayBlink = 0;
            return 1;
        }
        if (pg->count > 1 && (contrlerTestButtons(p, PAD_DOWN, 0) || contrlerTestButtons(p, PAD_UP, 0))) {
            zoveriiSetTextColour(g_overlayText[5 + g_overlayScreen->sel], &g_overlaySmallFont, OVL_COL_ITEM, 0);
            if (contrlerTestButtons(p, PAD_DOWN, 0))
                g_overlayScreen->sel = (uint8_t)((int)g_overlayScreen->sel < pg->count - 1 ? g_overlayScreen->sel + 1 : 0);
            else
                g_overlayScreen->sel = (uint8_t)((g_overlayScreen->sel != 0 ? g_overlayScreen->sel : pg->count) - 1);
            bmxsoundEvent(SND_MOVE, 0, 0, 0, 0);
            g_overlayBlink = 0;
            return 1;
        }
    }

    switch (g_overlayPage) {
    case 9:                                          /* checking... */
        if (g_overlayFrame == 1) {
            zcardSelectPort(g_overlaySaveSlot);
            g_overlaySaveResult = -1;
            zcardCheck(overlaySaveCallback);
            return 0;
        }
        if (g_overlayFrame <= 1)
            return 0;
        if ((uint32_t)g_overlaySaveResult < 4) {
            if (g_overlaySaveResult == 3 || g_overlaySaveResult == 0) {
                switch (memcard_CheckSaveExists()) {
                case 0: case 5: goto noFile;
                case 4: next = 2; goto withNo;
                case 6: next = 6; goto withNo;
                default: goto error;
                }
            }
            if (g_overlaySaveResult == 1) {
                overlaySetPage(1);
                overlaySaveRefresh();
                return 0;
            }
            goto error;
        }
        if (g_overlaySaveResult == 4) {
            next = 2;
            goto withNo;
        }
        if (g_overlaySaveResult != -1)
            goto error;
        if (g_overlayFrame > 0x96)
            g_overlaySaveResult = 9;                /* time out */
        return 0;
    case 10:                                         /* formatting... */
        if (g_overlayFrame != 1)
            return 0;
        switch (memcard_CheckSaveExists()) {
        case 0: case 5:
noFile:
            r = memcard_Stub0();
            if (r == 0) { overlaySetPage(5); overlaySaveRefresh(); return 0; }
            if (r == 7) { overlaySetPage(4); overlaySaveRefresh(); return 0; }
            goto error;
        case 1: overlaySetPage(1); overlaySaveRefresh(); return 0;
        case 3: overlaySetPage(9); overlaySaveRefresh(); return 0;
        case 4:
            if (zcardFormat() == 0) { overlaySetPage(11); overlaySaveRefresh(); return 0; }
            goto error;
        case 6:
            next = 6;
            goto withNo;
        default:
            goto error;
        }
    case 11:                                         /* saving data... */
        if (g_overlayFrame != 2)
            return 0;
        if (g_currentLevel < 6)
            gameAdvanceGoalTier(g_currentPlayer);
        switch (memcard_CheckSaveExists()) {
        case 1: overlaySetPage(1); overlaySaveRefresh(); return 0;
        case 3: overlaySetPage(9); overlaySaveRefresh(); return 0;
        case 4: next = 2; goto withNo;
        case 5:
            r = memcard_Stub0();
            if (r == 7) { overlaySetPage(4); overlaySaveRefresh(); return 0; }
            if (r != 0)
                goto error;
            /* fall through */
        case 0: case 6:
            if (memcard_WriteSave() == 0) { overlaySetPage(7); overlaySaveRefresh(); return 0; }
            goto error;
        default:
            goto error;
        }
    default:
        return 0;
    }
withNo:
    overlaySetPage((uint8_t)next);
    g_overlayScreen->sel = 1;                        /* default to the second item */
    overlaySaveRefresh();
    return 0;
error:
    overlaySetPage(8);
    overlaySaveRefresh();
    return 0;
}

/* VA 0x0041b880, 315 bytes, [I] row, status draft */
static void overlaySaveDraw(void)
{
    /* the item cursor; on page 0 a box around the chosen slot */
    uint32_t page = g_overlayPage, slot, edge, x0, colour;
    int x;

    if (g_overlaySavePages[page].count != 0)
        zoveriiSetTextColour(g_overlayText[5 + g_overlayScreen->sel], &g_overlaySmallFont, overlayPulseColour(), 0);
    if (page != 0)
        return;
    for (slot = 0, x0 = 0x215; x0 < 0xA69; slot++, x0 += 0x42A) {
        x = (int)(x0 >> 2);
        zoveriiSetTextColour(g_overlayText[5 + g_overlaySavePages[page].count + slot], &g_overlayTinyFont,
                             slot == g_overlaySaveSlot ? overlayPulseColour() : OVL_COL_LABEL, 0);
        for (edge = 0; edge < 4; edge++) {
            colour = itemColour(slot, g_overlaySaveSlot, OVL_COL_ITEM);
            if (edge == 2)
                zoveriiDrawRect((int16_t)(x - 0x1B), 0x138, 0xA0, 1, colour);
            else if (edge == 3)
                zoveriiDrawRect((int16_t)(x + 0x83), 0x120, 1, 0x18, colour);
            else
                zoveriiDrawRect((int16_t)(x - 0x1B), 0x120, (edge & 1) ? 1 : 0xA0, (edge & 1) ? 0x18 : 1, colour);
        }
    }
}

/* VA 0x0041ba10, 479 bytes, [I] between, status draft */
static void overlaySaveRefresh(void)
{
    /* four message lines (0x1112 + 4 page, "%d" = the slot number), the page's items, and on page 0
     * "SLOT 1" / "SLOT 2" (0x1110 / 0x1111) */
    char buf[64];
    const OverlaySavePage *pg = &g_overlaySavePages[g_overlayPage];
    uint32_t k, x0;
    const char *s;
    int y;

    freeTexts(&g_overlayText[1], 23);
    for (k = 0, y = 0xB6; k < 4; k++, y += 0x1D) {
        zstringPrintf(buf, zTextGet(0x1112u + k + g_overlayPage * 4u), g_overlaySaveSlot + 1);
        g_overlayText[1 + k] = addCentred(buf, y, OVL_COL_ITEM);
    }
    for (k = 0, y = 0x1A4 - pg->count * 0x1D; k < pg->count; k++, y += 0x1D) {
        s = zTextGet((uint16_t)(pg->text + k));
        g_overlayText[5 + k] = addCentred(s, y, OVL_COL_ITEM);
    }
    if (g_overlayPage == 0)
        for (k = 0, x0 = 0x215; x0 < 0xA69; k++, x0 += 0x42A) {
            s = zTextGet(0x1110 + k);
            g_overlayText[5 + pg->count + k] = zoveriiAddText(ZOVER_LAYER_MENU, &g_overlayTinyFont, s,
                                                              (int16_t)((x0 >> 2) - ((uint16_t)tinyWidth(s) >> 1) + 0x35),
                                                              0x121, OVL_COL_ITEM, 0);
        }
}

/* VA 0x0041bc70, 10 bytes, [I] row, status draft */
static void overlaySaveCallback(int unused, int result)
{
    /* the card check's completion callback (zcardCheck) */
    (void)unused;
    g_overlaySaveResult = result;
}

/* ================================================================== HUD items */

/* VA 0x0041bc90, 49 bytes, [I] row, status draft */
static void overlayLetterboxDraw(void)
{
    /* item 0: two black 640 x 60 bars (replays, the attract demo) */
    psxDrawRect(0, 0, 0, 0x200, s_letterboxSize);
    psxDrawRect(0, 0, 0x1A4, 0x200, s_letterboxSize);
}

/* VA 0x0041bce0, 163 bytes, [I] row, status draft */
static void overlayFadeDraw(void)
{
    /* item 16: a full-screen colour quad, blend mode 2 (3 without flag bit 1), stepped every frame */
    uint32_t mode;

    if (g_hudFade.frames == 0)
        return;
    g_hudFade.rgb[0] += g_hudFade.step[0];
    g_hudFade.rgb[1] += g_hudFade.step[1];
    g_hudFade.rgb[2] += g_hudFade.step[2];
    mode = ((uint32_t)(~g_hudFade.flags & 2) | 4) << 23;
    psxDrawRect(mode | (uint32_t)(int)g_hudFade.rgb[0] << 16 | (uint32_t)(int)g_hudFade.rgb[1] << 8 |
                     (uint32_t)(int)g_hudFade.rgb[2], 0, 0, 0x200, s_screenSize);
    g_hudFade.frames--;
}

/* VA 0x0041bdc0, 253 bytes, [I] row, status draft */
static void overlayFadeSet(const void *data)
{
    /* flags bit 0: start at the colour and fade to nothing (fade in), else from nothing to it */
    const OverlayFadeReq *req = (const OverlayFadeReq *)data;
    float n;

    g_hudFade.frames = req->frames;
    if (req->frames == 0)
        return;
    n = (float)req->frames;
    g_hudFade.step[0] = (float)req->r / n;
    g_hudFade.step[1] = (float)req->g / n;
    g_hudFade.step[2] = (float)req->b / n;
    g_hudFade.flags = req->flags;
    if (g_hudFade.flags & 1) {
        g_hudFade.rgb[0] = (float)req->r;
        g_hudFade.rgb[1] = (float)req->g;
        g_hudFade.rgb[2] = (float)req->b;
        g_hudFade.step[0] = -g_hudFade.step[0];
        g_hudFade.step[1] = -g_hudFade.step[1];
        g_hudFade.step[2] = -g_hudFade.step[2];
    } else {
        g_hudFade.rgb[0] = g_hudFade.rgb[1] = g_hudFade.rgb[2] = 0.0f;
    }
}

/* VA 0x0041bf00, 144 bytes, [I] split, status draft */
static void overlayScoreInit(int on)
{
    /* item 1: the run score, right-aligned at (0x24B, 400) */
    char buf[16];

    if (!on) {
        zoveriiFree(g_hudScoreText);
        g_hudScoreText = ZOVER_NO_HANDLE;
        return;
    }
    zstringPrintf(buf, "%ul", g_hudScoreShown);
    g_hudScoreText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, buf, (int16_t)(0x24B - (uint16_t)smallWidth(buf)),
                                    400, OVL_COL_VALUE, 0);
    zoveriiShow(g_hudScoreText, 0);
}

/* VA 0x0041bfc0, 325 bytes, [I] row, status draft */
static void overlayScoreDraw(void)
{
    /* the shown score climbs towards the target by a tenth of the gap + 10 per frame (or jumps with
     * flag bit 0); the stat games show their value as a distance instead (no roll-up) */
    char buf[16];
    uint32_t target;

    switch (gameGetMode()) {
    case GAME_MODE_SINGLE:
        if (gameGetSubMode() == GAME_SUB_FREE_RIDE)
            return;
        /* fall through */
    default:
        target = g_hudScoreTarget;
        if (!(g_hudScoreFlags & 1)) {
            if (target <= g_hudScoreShown)
                return;
            g_hudScoreShown = g_hudScoreShown - (uint32_t)(int)((float)(target - g_hudScoreShown) * -0.1f) + 10;
            if (g_hudScoreShown >= target)
                g_hudScoreShown = target;
        } else {
            g_hudScoreFlags &= ~1;
            g_hudScoreShown = target;
        }
        zstringPrintf(buf, "%ld", g_hudScoreShown);
        break;
    case GAME_MODE_BMX:
        return;
    case GAME_MODE_LONGEST_GRIND: case GAME_MODE_HIGHEST_WALLTAP: case GAME_MODE_BIG_AIR:
    case GAME_MODE_FURTHEST_JUMP: case GAME_MODE_LONGEST_MANUAL:
        if (!(g_hudScoreFlags & 1) && g_hudScoreTarget == g_hudScoreShown)
            return;
        g_hudScoreShown = g_hudScoreTarget;
        overlayFormatDistance(buf, (float)g_hudScoreTarget);
        break;
    }
    zoveriiFree(g_hudScoreText);
    g_hudScoreText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, buf, (int16_t)(0x24B - (uint16_t)smallWidth(buf)),
                                    400, OVL_COL_VALUE, 0);
}

/* VA 0x0041c190, 83 bytes, [I] row, status draft */
static void overlayScoreShow(int on)
{
    /* never in free ride or B-M-X */
    if ((gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_FREE_RIDE) || gameGetMode() == GAME_MODE_BMX)
        on = 0;
    zoveriiShow(g_hudScoreText, on);
}

/* VA 0x0041c210, 87 bytes, [I] row, status draft */
static void overlayScoreSet(const void *data)
{
    const OverlayScoreReq *req = (const OverlayScoreReq *)data;
    uint32_t visible = ~g_overlayItems[OVL_ITEM_SCORE].flags & 1;

    g_hudScoreTarget = req->score;
    g_hudScoreFlags |= req->flags;
    if (gameGetMode() == GAME_MODE_SINGLE) {
        if (gameGetSubMode() == GAME_SUB_FREE_RIDE)
            visible = 0;
    } else if (gameGetMode() == GAME_MODE_BMX) {
        return;
    }
    zoveriiShow(g_hudScoreText, (int)visible);
}

/* VA 0x0041c290, 89 bytes, [I] row, status draft */
static void overlayTimerInit(int on)
{
    /* item 2: m:ss at (0x35, 400) */
    g_hudTimerStr[0] = '\0';
    if (!on) {
        zoveriiFree(g_hudTimerText);
        g_hudTimerText = ZOVER_NO_HANDLE;
        return;
    }
    g_hudTimerText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, g_hudTimerStr, 0x35, 400, OVL_COL_VALUE, 0);
    zoveriiShow(g_hudTimerText, 0);
}

/* VA 0x0041c310, 316 bytes, [I] between, status draft */
static void overlayTimerDraw(void)
{
    /* frames left -> "m:ss" (30 frames a second, rounded up; the colon blinks in the first half of
     * every second, red for the last 5 seconds); the text is rebuilt only when it changes */
    char buf[16];
    uint32_t t, min = 0, sec = 0;

    if (gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_FREE_RIDE)
        return;
    t = g_hudTimerFrames + 0x1D;
    if (t > 0x708) {
        min = (t - 1) / 0x708;
        t -= min * 0x708;
    }
    if (t > 0x1E) {
        sec = (t - 1) / 0x1E;
        t -= sec * 0x1E;
    }
    zstringPrintf(buf, "%1ld%c%02ld", min, t < 0xF ? ' ' : ':', sec);
    if (zstringCompare(buf, g_hudTimerStr) != 0) {
        zoveriiFree(g_hudTimerText);
        g_hudTimerText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, buf, 0x35, 400,
                                        g_hudTimerFrames > 0x97 ? OVL_COL_VALUE : 0x00C05050u, 0);
        g_hudTimerShown = g_hudTimerFrames;
        zstringCopy(g_hudTimerStr, buf);
    }
}

/* VA 0x0041c4a0, 41 bytes, [I] between, status draft */
static void overlayTimerShow(int on)
{
    if (gameGetMode() == GAME_MODE_SINGLE && gameGetSubMode() == GAME_SUB_FREE_RIDE)
        return;
    zoveriiShow(g_hudTimerText, on);
}

/* VA 0x0041c4e0, 13 bytes, [I] between, status draft */
static void overlayTimerSet(const void *data)
{
    g_hudTimerFrames = *(const uint32_t *)data;
}

/* VA 0x0041c500, 101 bytes, [I] row, status draft */
static void overlayInputDraw(void)
{
    /* item 3: the stick directions of the trick being entered (BmxRider.dirBuf), as button-font
     * arrows "adwxqezc" (g_hudDirGlyph), top left */
    const BmxRider *r = &g_riders[g_currentPlayer];
    char buf[8];
    uint32_t i;

    for (i = 0; i < r->dirCount; i++)
        buf[i] = g_hudDirGlyph[r->dirBuf[i]];
    buf[i] = '\0';
    zoveriiDrawText(&g_overlayButtonFont, buf, 0x35, 0x3C, OVL_COL_VALUE, 0);
}

/* VA 0x0041c590, 1060 bytes, [I] row, status draft */
static void overlayTricksDraw(void)
{
    /* item 4: the trick set, one name per trick (overlayTrickName(.., full = 1)), sliding up from
     * y 360 to y 160 as the trick ages (cosine over 45 frames: age * 364, capped at 0x4000) and
     * fading in near the bottom; "TO" two thirds of the way between a trick and the one before it;
     * a modified trick in upper case in palette 3; a name wider than 480 px is split at the first
     * space after its middle. Below: the set's score ("%ld") at y 400 (not in free ride, B-M-X or
     * the stat games). */
    const BmxRider *r = &g_riders[g_currentPlayer];
    const BmxTrickRec *t;
    char buf[64], *second;
    int i, cur, prevY = 0;
    uint32_t a, l, colour, fade, w, f;
    uint16_t clut;
    int16_t y;

    for (i = 0; i < r->trickCount; i++) {
        t = &r->score.trick[i];
        a = t->age * 0x16Cu;
        if (a > 0x4000)
            a = 0x4000;
        cur = 0x168 - (int)(200.0f - zmathCos((int)a) * 200.0f);
        if (i > 0) {
            y = (int16_t)(prevY + (int)((float)(cur - prevY) * 0.67f));
            if (y >= 0xF0) {
                if (y < 0x110) {
                    l = (y - 0xF0) < 0x18 ? (uint32_t)((y - 0xF0) >> 1) : 0xC;
                    colour = 0x02000000 | l * 5 | (l * 0xB) << 8 | (l * 0xD) << 16;
                } else {
                    colour = 0x00D0B050;
                }
                zstringPrintf(buf, "TO");
                zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(0x140 - ((uint16_t)smallWidth(buf) >> 1)), y,
                                colour, 5);
            }
        }
        prevY = cur;
        if ((int16_t)cur < 0xD2)
            continue;
        overlayTrickName(buf, t, 1);
        if ((int16_t)cur < 0xF2) {
            colour = 0x02000000;
            l = (cur - 0xD2) < 0x28 ? (uint32_t)((cur - 0xD2) >> 2) : 10;
        } else {
            colour = 0;
            l = 0xC;
        }
        if (t->mod == 0xFF || t->trick == 0xFF) {
            clut = 0;
            fade = ((l << 8 | l) << 8 | l) << 4;
        } else {
            f = (uint32_t)(int)((float)t->modCount * 1.5f);     /* [I] operand of this __ftol */
            if ((int)f > 0xE)
                f = 0xF;
            zstringUpper(buf);
            clut = 3;
            fade = l << 2 | (f & 0xFF) << 10 | (f & 0xFF) * 0xD0000;
        }
        second = NULL;
        w = (uint16_t)smallWidth(buf);
        if (w < 0x1E0) {
            y = (int16_t)(cur - 0xE);
        } else {
            y = (int16_t)cur;
            second = buf + (zstringLength(buf) >> 1);
            while (*second != '\0') {
                if (*second == ' ') {
                    *second++ = '\0';
                    break;
                }
                second++;
            }
            w = (uint16_t)smallWidth(buf);
        }
        zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(0x140 - (w >> 1)), y, colour | fade, clut);
        if (second != NULL)
            zoveriiDrawText(&g_overlaySmallFont, second, (int16_t)(0x140 - ((uint16_t)smallWidth(second) >> 1)),
                            (int16_t)(y + 0x1D), colour | fade, clut);
    }
    if (r->trickCount != 0) {
        switch (gameGetMode()) {
        case GAME_MODE_SINGLE:
            if (gameGetSubMode() == GAME_SUB_FREE_RIDE)
                return;
            break;
        case GAME_MODE_BMX: case GAME_MODE_WIPEOUT: case GAME_MODE_LONGEST_GRIND: case GAME_MODE_HIGHEST_WALLTAP:
        case GAME_MODE_BIG_AIR: case GAME_MODE_FURTHEST_JUMP: case GAME_MODE_LONGEST_MANUAL:
            return;
        }
        zstringPrintf(buf, "%ld", r->score.setScore);
        zoveriiDrawText(&g_overlaySmallFont, buf, (int16_t)(0x140 - ((uint16_t)smallWidth(buf) >> 1)), 400, OVL_COL_VALUE, 0);
    }
}

/* VA 0x0041cae0, 289 bytes, [I] row, status draft */
static void overlayMeterInit(int on)
{
    /* item 5: a 0x26 x 0x12A panel at the right edge, a black frame and a white marker */
    uint32_t i;

    if (!on) {
        freeTexts(g_hudMeterText, 6);
        return;
    }
    zstringMemSet(g_hudMeterText, 0xFF, sizeof(g_hudMeterText));
    g_hudMeterText[0] = zoveriiAddRect(ZOVER_LAYER_HUD, 0x224, 0x51, 0x26, 0x12A, 0x01B0B040);
    zoveriiShow(g_hudMeterText[0], 0);
    for (i = 0; i < 4; i++) {
        g_hudMeterText[1 + i] = zoveriiAddRect(ZOVER_LAYER_HUD, i != 1 ? 0x223 : 0x24A, i != 3 ? 0x50 : 0x17B,
                                               (i & 2) ? 0x28 : 1, (i & 2) ? 1 : 300, 0x01000000);
        zoveriiShow(g_hudMeterText[1 + i], 0);
    }
    g_hudMeterText[5] = zoveriiAddRect(ZOVER_LAYER_HUD, 0x213, 0x17A, 0x10, 2, 0x00FFFFFF);
    g_hudMeter.value = 0.0f;
    g_hudMeterBase = 0.0f;
}

/* VA 0x0041cc60, 654 bytes, [V] direct, status draft */
static void overlayMeterDraw(void)
{
    /* The scale: 20 feet visible from g_hudMeterBase up, 14.95 px per foot (y 379 = the base), a
     * long tick and a "%ldft" label every 5 feet (the hundreds digit dropped above 99). The best
     * value of the record stat (all-time, live slot 0) is marked with a red line when on the scale. */
    const GameLiveRecord *live;
    char buf[8], *text;
    uint32_t foot;
    int k, i, y = 0;
    float feet;

    if (g_hudMeter.value == 0.0f)
        return;
    foot = (uint32_t)(int)g_hudMeterBase;
    if ((float)(int)foot - g_hudMeterBase != 0.0f)
        foot++;
    k = foot % 5 ? 5 - (int)(foot % 5) : 0;
    for (i = 0; i < 20; i++) {
        y = (int)(379.0f - ((float)foot - g_hudMeterBase) * 14.95f);
        zoveriiDrawRect(0x224, (int16_t)y, k ? 0x13 : 0x26, 1, k ? 0x01404040u : 0x01202020u);
        if (k == 0 && y < 0x17A - g_overlayTinyFont.height) {
            zstringPrintf(buf, "%ldft", foot);
            text = foot > 99 ? buf + 1 : buf;
            zoveriiDrawText(&g_overlayTinyFont, text, 0x226, (int16_t)(y + 2), foot < 100 ? 0x03D0D0D0u : 0x0300FFFFu, 0);
        }
        foot++;
        k = k ? k - 1 : 4;
    }
    y = (int)(379.0f - ((float)foot - g_hudMeterBase) * 14.95f);
    if (k == 0 && y > 0x4E) {
        zstringPrintf(buf, "%ldft", foot);
        text = foot > 99 ? buf + 1 : buf;
        zoveriiDrawText(&g_overlayTinyFont, text, 0x226, (int16_t)(y + 2), foot < 100 ? 0x03D0D0D0u : 0x0300FFFFu, 0);
    }
    live = gameGetLiveRecord(GAME_RECORD_ID(gameGetMode(), gameGetSubMode(), g_hudMeter.stat, 0, 0));
    if (live == NULL)
        zAssertFail("invalid record type in display setup data", OVERLAY_SRC, 0x1897, NULL);
    feet = (float)live->value * 0.0328084f;
    if (feet < g_hudMeterBase + 20.0f && g_hudMeterBase <= feet)
        zoveriiDrawRect(0x224, (int16_t)(0x17B - (int)((feet - g_hudMeterBase) * 14.95f)), 0x26, 2, 0x703020);
}

/* VA 0x0041cfa0, 70 bytes, [I] row, status draft */
static void overlayMeterShow(int on)
{
    uint32_t i;

    for (i = 0; i < 6; i++)
        zoveriiShow(g_hudMeterText[i], on && g_hudMeter.value != 0.0f);
}

/* VA 0x0041d010, 323 bytes, [I] row, status draft */
static void overlayMeterSet(const void *data)
{
    /* value in cm; 0 hides the meter. The marker shows the height; the scale scrolls once it is
     * above 15 feet. */
    const OverlayMeterReq *req = (const OverlayMeterReq *)data;
    uint32_t i;

    if (req->value == g_hudMeter.value)
        return;
    if (req->value == 0.0f) {
        for (i = 0; i < 6; i++)
            zoveriiShow(g_hudMeterText[i], 0);
        g_hudMeterBase = 0.0f;
        g_hudMeter = *req;
        return;
    }
    if (g_hudMeter.value == 0.0f && !(g_overlayItems[OVL_ITEM_METER].flags & OVL_ITEM_HIDDEN))
        for (i = 0; i < 6; i++)
            zoveriiShow(g_hudMeterText[i], 1);
    g_hudMeter.value = req->value;
    g_hudMeter.feet = req->value * 0.0328084f;
    g_hudMeter.unk_08 = req->unk_08;
    g_hudMeterBase = g_hudMeter.feet >= 15.0f ? g_hudMeter.feet - 15.0f : 0.0f;
    zoveriiSetRect(g_hudMeterText[5], 0x213, (int16_t)(0x17B - (int)((g_hudMeter.feet - g_hudMeterBase) * 14.95f)), 0x10, 2);
    g_hudMeter.stat = req->stat;
}

/* VA 0x0041d1c0, 343 bytes, [I] row, status draft */
static void overlayBalanceInit(int on)
{
    /* item 6: a vertical bar, red at both ends and green in the middle, a frame and a white marker */
    uint32_t i;

    if (!on) {
        freeTexts(g_hudBalanceText, 7);
        return;
    }
    zstringMemSet(g_hudBalanceText, 0xFF, sizeof(g_hudBalanceText));
    for (i = 0; i < 2; i++) {
        g_hudBalanceText[i] = zoveriiAddGradient(ZOVER_LAYER_HUD, 0xD6, (int16_t)(i * 0x4F + 0xA1), 0x1C, 0x4F,
                                                 i ? 0x0120B010u : 0x01B02010u, i ? 0x01B02010u : 0x0120B010u);
        zoveriiShow(g_hudBalanceText[i], 0);
    }
    for (i = 0; i < 4; i++) {
        g_hudBalanceText[2 + i] = zoveriiAddRect(ZOVER_LAYER_HUD, i != 1 ? 0xD5 : 0xF2, i != 3 ? 0xA0 : 0x13F,
                                                 (i & 2) ? 0x1E : 1, (i & 2) ? 1 : 0xA0, 0x01000000);
        zoveriiShow(g_hudBalanceText[2 + i], 0);
    }
    g_hudBalanceText[6] = zoveriiAddRect(ZOVER_LAYER_HUD, 0xD6, 0xEE, 0x1C, 2, 0x00FFFFFF);
    g_hudBalance = 0.0f;
}

/* VA 0x0041d380, 1 bytes, [I] row, status draft */
static void overlayBalanceDraw(void)
{
}

/* VA 0x0041d3a0, 70 bytes, [I] row, status draft */
static void overlayBalanceShow(int on)
{
    uint32_t i;

    for (i = 0; i < 7; i++)
        zoveriiShow(g_hudBalanceText[i], on && g_hudBalance != 0.0f);
}

/* VA 0x0041d410, 224 bytes, [I] row, status draft */
static void overlayBalanceSet(const void *data)
{
    /* the grind / manual balance (BmxRider.grindBalance); 0 hides the bar; marker +-80 px */
    float v = *(const float *)data;
    uint32_t i;
    int y;

    if (v == g_hudBalance)
        return;
    if (v == 0.0f) {
        for (i = 0; i < 7; i++)
            zoveriiShow(g_hudBalanceText[i], 0);
        g_hudBalance = v;
        return;
    }
    if (g_hudBalance == 0.0f && !(g_overlayItems[OVL_ITEM_BALANCE].flags & OVL_ITEM_HIDDEN))
        for (i = 0; i < 7; i++)
            zoveriiShow(g_hudBalanceText[i], 1);
    g_hudBalance = v;
    y = (int)(v * 90.28571f);
    if (y > 0x50)
        y = 0x50;
    if (y < -0x50)
        y = -0x50;
    zoveriiSetRect(g_hudBalanceText[6], 0xD6, (int16_t)(y + 0xEE), 0x1C, 2);
}

/* VA 0x0041d530, 97 bytes, [I] row, status draft */
static void overlayCrashInit(int on)
{
    /* item 7: why the rider bailed (text 0x1056 + id: "look where you're going", "landed sideways" ...) */
    if (!on) {
        zoveriiFree(g_hudCrashText);
        g_hudCrashText = ZOVER_NO_HANDLE;
    } else {
        g_hudCrashText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, g_overlayEmpty, 0x140, 0x132, 0x00C09020, 0);
    }
    g_hudTimerShown = 0xFF;          /* sic: 0x0056BF54, the timer's variable */
}

/* VA 0x0041d5c0, 141 bytes, [I] row, status draft */
static void overlayCrashDraw(void)
{
    const char *s;

    if (g_hudCrashShown == g_hudCrashId)
        return;
    g_hudCrashShown = g_hudCrashId;
    if (g_hudCrashId != 0xFF) {
        s = zTextGet(0x1056u + g_hudCrashId);
        zoveriiFree(g_hudCrashText);
        g_hudCrashText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, s,
                                        (int16_t)(0x140 - ((uint16_t)smallWidth(s) >> 1)), 0x132, 0x00C09020, 0);
        return;
    }
    zoveriiShow(g_hudCrashText, 0);
}

/* VA 0x0041d680, 58 bytes, [I] row, status draft */
static void overlayCrashShow(int on)
{
    zoveriiShow(g_hudCrashText, on && g_hudCrashShown != 0xFF);
}

/* VA 0x0041d6d0, 13 bytes, [I] row, status draft */
static void overlayCrashSet(const void *data)
{
    g_hudCrashId = *(const uint8_t *)data;
}

/* VA 0x0041d6f0, 1 bytes, [I] row, status draft */
static void overlayUnused8Init(int on)
{
    (void)on;
}

/* VA 0x0041d710, 1 bytes, [I] row, status draft */
static void overlayUnused8Draw(void)
{
}

/* VA 0x0041d730, 1 bytes, [I] row, status draft */
static void overlayUnused8Show(int on)
{
    (void)on;
}

/* VA 0x0041d750, 13 bytes, [I] row, status draft */
static void overlayUnused8Set(const void *data)
{
    /* item 8: game.c sends a byte here; nothing draws it */
    g_hudUnused8 = *(const uint8_t *)data;
}

/* VA 0x0041d770, 8 bytes, [I] row, status draft */
static void overlayReplayInit(int on)
{
    (void)on;
    g_hudReplayIcon[0] = (uint16_t)(g_hudReplayIcon[0] & 0xFF00);
}

/* VA 0x0041d790, 93 bytes, [I] row, status draft */
static void overlayReplayDraw(void)
{
    /* item 9: "R" record (red), "P" play (green), "p" pause (blue) in the icon font at x 0x240,
     * blinking every 16 frames (the width is taken with the small font) */
    uint32_t i = g_hudReplayIcon[0] & 0xFF;
    const char *s = g_hudReplayIconText[i];

    if (*s != '\0' && !(g_hudReplayIcon[1] & 0x10))
        zoveriiDrawText(&g_overlayIconFont, s, (int16_t)(0x240 - ((uint16_t)smallWidth(s) >> 1)), 0x30,
                        g_hudReplayIconColour[i], 0);
    g_hudReplayIcon[1]++;
}

/* VA 0x0041d810, 1 bytes, [I] row, status draft */
static void overlayReplayShow(int on)
{
    (void)on;
}

/* VA 0x0041d830, 22 bytes, [I] row, status draft */
static void overlayReplaySet(const void *data)
{
    g_hudReplayIcon[0] = *(const uint16_t *)data;
    g_hudReplayIcon[1] = 0;
}

/* VA 0x0041d860, 93 bytes, [I] row, status draft */
static void overlayHintInit(int on)
{
    if (on)
        zstringMemSet(g_hudHintText, 0xFF, sizeof(g_hudHintText));
    else
        freeTexts(g_hudHintText, 7);
    g_hudHint.timer = 0;
}

/* VA 0x0041d8e0, 324 bytes, [I] row, status draft */
static void overlayHintDraw(void)
{
    /* item 10: 90 frames: fade in, hold, fade out; the 3 lines in palette 5 of the small font, the
     * button glyphs grey. (Ghidra cannot decompile this one; written from the disassembly.) */
    uint32_t i, l, colour;
    int v;

    if (g_hudHint.timer == 0)
        return;
    if (--g_hudHint.timer == 0) {
        freeTexts(g_hudHintText, 7);
        return;
    }
    if (g_hudHint.timer >= 0x3C)
        v = (int)((float)(int16_t)(0x5A - g_hudHint.timer) * 1.2f);
    else if (g_hudHint.timer >= 0x1E)
        v = (int)((float)(int16_t)(g_hudHint.timer - 0x1E) * 1.2f);
    else
        return;
    l = (uint8_t)((v > 10 ? 10 : v) << 4);
    if (l == 0xFF)
        return;
    for (i = 0; i < 7; i++) {
        if (i < 3)
            zoveriiSetTextColour(g_hudHintText[i], &g_overlaySmallFont,
                                 (l * 0x50 >> 8) | (l * 0xD) << 12 | (l * 0xB) << 4 | 0x02000000, 5);
        else {
            colour = l >= 0x40 ? 0x00D0D0D0 : ZOVER_GREY(2, l);
            zoveriiSetTextColour(g_hudHintText[i], &g_overlayButtonFont, colour, 0);
        }
    }
}

/* VA 0x0041da90, 44 bytes, [I] row, status draft */
static void overlayHintShow(int on)
{
    uint32_t i;

    for (i = 0; i < 7; i++)
        if (g_hudHintText[i] != ZOVER_NO_HANDLE)
            zoveriiShow(g_hudHintText[i], on);
}

/* VA 0x0041dad0, 577 bytes, [V] direct, status draft */
static void overlayHintSet(const void *data)
{
    /* Tutorial hint 'id' (0..21, game.c's hint mask): 3 lines, texts 0x114A + 3 id. Every upper-case
     * letter in a line is a button: it is replaced by 3 spaces (4 after 'D') and the button glyph
     * (g_hudButtonGlyph[c - 'A']: A..L -> "adwxXSTC2143") is drawn there with the button font. */
    char buf[64], glyph[2];
    const char *s, *c;
    char *o;
    uint32_t k, nb = 0;
    int y = 0x83, x;

    *(uint32_t *)&g_hudHint = *(const uint32_t *)data;
    freeTexts(g_hudHintText, 7);
    if (g_hudHint.id == 0xFF) {
        g_hudHint.timer = 0;
        return;
    }
    g_hudHint.timer = 0x5A;
    for (k = 0; k < 3; k++, y += 0x1D) {
        s = zTextGet((g_hudHint.id & 0x7Fu) * 3 + 0x114A + k);
        if (*s == '\0')
            continue;
        for (c = s, o = buf; *c != '\0'; c++) {
            if (*c < 'A' || *c > 'Z') {
                *o++ = *c;
            } else {
                *o++ = ' '; *o++ = ' '; *o++ = ' ';
                if (*c > 'D')
                    *o++ = ' ';
            }
        }
        *o = '\0';
        x = 0x140 - ((uint16_t)smallWidth(buf) >> 1);
        for (c = s, o = buf; *c != '\0'; c++) {
            if (*c < 'A' || *c > 'Z') {
                *o++ = *c;
                continue;
            }
            glyph[0] = g_hudButtonGlyph[*c - 'A'];
            glyph[1] = '\0';
            *o = '\0';
            if (3 + nb > 6)
                zAssertFail("too many buttons in hint string, increase allocation", OVERLAY_SRC, 0x1B06, NULL);
            g_hudHintText[3 + nb++] = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlayButtonFont, glyph,
                                                     (int16_t)(x + (uint16_t)smallWidth(buf)), (int16_t)y, 0x02000000, 0);
            *o++ = ' '; *o++ = ' '; *o++ = ' ';
            if (*c > 'D')
                *o++ = ' ';
        }
        *o = '\0';
        g_hudHintText[k] = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, buf, (int16_t)x, (int16_t)y, 0x02000000, 5);
    }
}

/* VA 0x0041ddc0, 13 bytes, [I] row, status draft */
static void overlayHintGet(void *data)
{
    *(uint32_t *)data = *(const uint32_t *)&g_hudHint;      /* timer != 0: still showing */
}

/* VA 0x0041dde0, 83 bytes, [I] row, status draft */
static void overlayGoalInit(int on)
{
    if (on)
        zstringMemSet(g_hudGoalText, 0xFF, sizeof(g_hudGoalText));
    else
        freeTexts(g_hudGoalText, 3);
    g_hudGoal.timer = 0;
}

/* VA 0x0041de60, 265 bytes, [I] row, status draft */
static void overlayGoalDraw(void)
{
    /* item 11: 120 frames, palette 6 */
    uint32_t i, l;
    int v;

    if (g_hudGoal.timer == 0)
        return;
    if (--g_hudGoal.timer == 0) {
        freeTexts(g_hudGoalText, 3);
        return;
    }
    if (g_hudGoal.timer >= 0x5A)
        v = (int)((float)(int16_t)(0x78 - g_hudGoal.timer) * 1.2f);
    else if (g_hudGoal.timer < 0x3C && g_hudGoal.timer >= 0x1E)
        v = (int)((float)(int16_t)(g_hudGoal.timer - 0x1E) * 1.2f);
    else
        return;
    l = (uint8_t)((v > 10 ? 10 : v) << 4);
    if (l == 0xFF)
        return;
    for (i = 0; i < 3; i++)
        zoveriiSetTextColour(g_hudGoalText[i], &g_overlaySmallFont, (l * 0x60 >> 8) | l << 14 | l * 0xD0 | 0x02000000, 6);
}

/* VA 0x0041dfc0, 44 bytes, [I] row, status draft */
static void overlayGoalShow(int on)
{
    uint32_t i;

    for (i = 0; i < 3; i++)
        if (g_hudGoalText[i] != ZOVER_NO_HANDLE)
            zoveriiShow(g_hudGoalText[i], on);
}

/* VA 0x0041e000, 230 bytes, [I] row, status draft */
static void overlayGoalSet(const void *data)
{
    /* goal 'id' of this level (0..11 = tier * 4 + goal): its 3 text lines (0x2089 + 3 (12 level + id))
     * at the top of the screen when the rider enters the goal's area */
    const char *s;
    uint32_t k;
    int y = 0x28;

    *(uint32_t *)&g_hudGoal = *(const uint32_t *)data;
    freeTexts(g_hudGoalText, 3);
    if (g_hudGoal.id == 0xFF) {
        g_hudGoal.timer = 0;
        return;
    }
    g_hudGoal.timer = 0x78;
    for (k = 0; k < 3; k++, y += 0x1D) {
        s = zTextGet(((g_hudGoal.id & 0x7Fu) + g_currentLevel * 12u) * 3 + 0x2089 + k);
        if (*s != '\0')
            g_hudGoalText[k] = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, s,
                                              (int16_t)(0x140 - ((uint16_t)smallWidth(s) >> 1)), (int16_t)y, 0x02000000, 6);
    }
}

/* VA 0x0041e130, 13 bytes, [I] row, status draft */
static void overlayGoalGet(void *data)
{
    *(uint32_t *)data = *(const uint32_t *)&g_hudGoal;
}

/* VA 0x0041e150, 48 bytes, [I] row, status draft */
static void overlayMessageInit(int on)
{
    if (!on && g_hudMessageText != ZOVER_NO_HANDLE)
        zoveriiFree(g_hudMessageText);
    g_hudMessageText = ZOVER_NO_HANDLE;
    g_hudMessage.timer = 0;
}

/* VA 0x0041e190, 178 bytes, [I] row, status draft */
static void overlayMessageDraw(void)
{
    /* item 12: 90 frames, fading out over the last 60, palette 4 */
    uint32_t l;
    int v;

    if (g_hudMessage.timer == 0)
        return;
    if (--g_hudMessage.timer != 0) {
        if (g_hudMessage.timer < 0x3C) {
            v = (int)((float)(int16_t)g_hudMessage.timer * 0.3f);
            l = (uint8_t)((v > 0xF ? 0xF : v) << 4);
        } else {
            l = 0xF0;
        }
        zoveriiSetTextColour(g_hudMessageText, &g_overlaySmallFont, (l * 0xD0 >> 8) | l << 14 | l * 0x50 | 0x02000000, 4);
        return;
    }
    if (g_hudMessageText != ZOVER_NO_HANDLE) {
        zoveriiFree(g_hudMessageText);
        g_hudMessageText = ZOVER_NO_HANDLE;
    }
}

/* VA 0x0041e280, 27 bytes, [I] row, status draft */
static void overlayMessageShow(int on)
{
    if (g_hudMessageText != ZOVER_NO_HANDLE)
        zoveriiShow(g_hudMessageText, on);
}

/* VA 0x0041e2b0, 339 bytes, [I] row, status draft */
static void overlayMessageSet(const void *data)
{
    /* gameSubmitRecord & co: text 0x1063 + id ("NEW HIGH SCORE!", "HUGE TRANSFER!" ...); id 12 is
     * "<n>-TRICK COMBO!", ids 16..21 (LONGEST GRIND ... LONGEST JUMP) add ": <distance>" */
    const OverlayMessage *req = (const OverlayMessage *)data;
    char buf[64];

    g_hudMessage = *req;
    if (g_hudMessageText != ZOVER_NO_HANDLE) {
        zoveriiFree(g_hudMessageText);
        g_hudMessageText = ZOVER_NO_HANDLE;
    }
    if (g_hudMessage.id == 0xFF) {
        g_hudMessage.timer = 0;
        return;
    }
    g_hudMessage.timer = 0x5A;
    if (g_hudMessage.id == 0xC)
        zstringPrintf(buf, "%ld-%s", g_hudMessage.value, zTextGet(0x106F));
    else
        zstringPrintf(buf, "%s", zTextGet(0x1063u + g_hudMessage.id));
    if (g_hudMessage.id > 0xF && g_hudMessage.id < 0x16) {
        zstringPrintf(buf + zstringLength(buf), ": ");
        overlayFormatDistance(buf + zstringLength(buf), (float)g_hudMessage.value);
    }
    g_hudMessageText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, buf,
                                      (int16_t)(0x140 - ((uint16_t)smallWidth(buf) >> 1)), 0xE2, 0x02000000, 4);
}

/* VA 0x0041e470, 22 bytes, [I] row, status draft */
static void overlayMessageGet(void *data)
{
    *(OverlayMessage *)data = g_hudMessage;
}

/* VA 0x0041e4a0, 73 bytes, [I] row, status draft */
static void overlayTrackInit(int on)
{
    g_hudTrack.timer = 0;
    if (on)
        zstringMemSet(g_hudTrackText, 0xFF, sizeof(g_hudTrackText));
    else
        freeTexts(g_hudTrackText, 4);
}

/* VA 0x0041e510, 198 bytes, [I] row, status draft */
static void overlayTrackDraw(void)
{
    /* item 13: 360 frames, fading in and out over 16 frames, grey */
    uint32_t l, i, t = g_hudTrack.timer;

    if (t == 0)
        return;
    if (t < 0x1E)
        l = (t & 0xFFFE) < 0x11 ? (uint8_t)((t >> 1) << 4) : 0x80;
    else if (t < 0x14A)
        l = 0x80;
    else
        l = ((0x168 - t) & ~1u) < 0x11 ? (uint8_t)(((0x168 - t) >> 1) << 4) : 0x80;
    for (i = 0; i < 4; i++)
        zoveriiSetTextColour(g_hudTrackText[i], &g_overlayTinyFont, ZOVER_GREY(2, l), 0);
    g_hudTrack.timer--;
}

/* VA 0x0041e620, 38 bytes, [I] row, status draft */
static void overlayTrackShow(int on)
{
    uint32_t i;

    for (i = 0; i < 4; i++)
        zoveriiShow(g_hudTrackText[i], on);
}

/* VA 0x0041e660, 182 bytes, [I] row, status draft */
static void overlayTrackSet(const void *data)
{
    /* the soundtrack title when a song starts: 4 lines from 0x102E + 4 track (artist, song, ...),
     * bottom left */
    uint32_t k;
    int y = 0x150;

    *(uint32_t *)&g_hudTrack = *(const uint32_t *)data;
    if (g_hudTrack.id == 0xFF) {
        freeTexts(g_hudTrackText, 4);
        g_hudTrack.timer = 0;
        return;
    }
    for (k = 0; k < 4; k++, y += 0x14) {
        if (g_hudTrackText[k] != ZOVER_NO_HANDLE)
            zoveriiFree(g_hudTrackText[k]);
        g_hudTrackText[k] = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlayTinyFont, zTextGet(0x102Eu + k + g_hudTrack.id * 4u),
                                           0x19, (int16_t)y, 0x02000000, 0);
    }
    g_hudTrack.timer = 0x168;
}

/* VA 0x0041e750, 73 bytes, [I] row, status draft */
static void overlayLogoInit(int on)
{
    /* item 14: image resource 0x39 as a sprite at (0x1B1, 0x148), bottom right [I: a logo] */
    if (!on) {
        zoveriiFree(g_overlayLogo);
        g_overlayLogo = ZOVER_NO_HANDLE;
        return;
    }
    g_overlayLogo = zoveriiAddSprite(ZOVER_LAYER_HUD, g_overlayLogoSlot, 0x1B1, 0x148, 0x01808080);
}

/* VA 0x0041e7c0, 22 bytes, [I] row, status draft */
static void overlayLogoShow(int on)
{
    zoveriiShow(g_overlayLogo, on);
}

/* ================================================================== shared helpers */

/* VA 0x0041e7f0, 232 bytes, [I] split, status draft */
void overlayDrawMenu(uint32_t flags, int first)
{
    /* The common bottom menu "continue / view replay / exit" (texts 0x1006..0x1008), drawn each
     * frame after the input delay. flags: 8 = only "continue"; 1 = with "view replay"; 2 = without
     * "continue". 'first' = the screen's index of the first of these items. */
    uint32_t i, n, row = 0;
    int y;

    if (g_overlayFrame <= g_overlayInputDelay)
        return;
    if (flags & 8) {
        n = 1;
    } else {
        n = (flags & 1) ? 3 : 2;
        if (flags & 2)
            n--;
    }
    y = 0x1A4 - (int)n * 0x1D;
    for (i = 0; ; ) {
        if (!((i == 0 && (flags & 2)) || (i == 1 && !(flags & 1)))) {
            drawCentred(&g_overlaySmallFont, zTextGet(0x1006 + i), y,
                        (uint32_t)g_overlayScreen->sel - (uint32_t)first == row ? overlayPulseColour() : OVL_COL_ITEM);
            y += 0x1D;
            row++;
        }
        if (flags & 8)
            break;
        if (++i >= 3)
            break;
    }
}

/* VA 0x0041e920, 116 bytes, [I] between, status draft */
int overlayConfirmed(uint32_t minSel)
{
    /* ok or start from an allowed player, with the cursor at item >= minSel */
    uint32_t p;

    for (p = 0; p < g_numPlayers; p++)
        if ((!g_overlayCurrentOnly || p == g_currentPlayer) &&
            (contrlerTestButtons(p, PAD_FIRE4 | PAD_FIRE6, 0) || contrlerTestButtons(p, PAD_START, 0)) &&
            minSel <= g_overlayScreen->sel)
            return 1;
    return 0;
}

/* VA 0x0041e9c0, 87 bytes, [I] split, status draft */
uint32_t overlayPulseColour(void)
{
    /* the highlight: grey 64..255 on a sine of g_overlayBlink (0x888 per frame), blend mode 2 */
    uint8_t v = (uint8_t)(0x40 - (int)((zmathSin((int)(g_overlayBlink * 0x888)) + 1.0f) * 0.5f * -191.0f));

    return ZOVER_GREY(2, v);
}

/* VA 0x0041ea40, 65 bytes, [I] split, status draft */
void overlayFormatDistance(char *out, float cm)
{
    /* cm -> feet and inches: 5486 -> "180' 0\"" */
    uint32_t in = (uint32_t)(int)(cm * 0.39370078f + 0.5f);

    zstringPrintf(out, "%0ld' %ld\"", in / 12, in % 12);
}

/* VA 0x0041eab0, 2656 bytes, [I] callers, status draft */
void overlayTrickName(char *out, const BmxTrickRec *t, int full)
{
    /* The name of one trick of a set, as the HUD and the B-M-X target list show it (ANIM.md
     * 7.1). Words joined with single spaces:
     *   [half-cab (full, flag 4; + spin if > 180) | spin "180".."extreme"]
     *   [opposite (full, flag 1)] [alley-oop (full, flag 2)]
     *   [double/triple/quad/sick (2..5+ stacked modifiers)] [modifier name] [trick name | " spin"]
     * then named combinations replace the whole name:
     *   can-can + one footer = "<spin...> no footer can-can"; superman + seat grab = "superman seat grab"
     *   spelled the other way round; backflip with 1 half spin = "<mods> flair"; backflip + barspin
     *   = "flip driver" / "limo driver" / "triple flip driver" / "vette driver" by modifier count;
     *   front flip with 2 half spins = "<mods> flail"; a bare barspin with a spin = "<spin> truck driver". */
    uint32_t n;

    out[0] = '\0';
    if (t->spins != 0) {
        if (full && (t->flags & 4)) {
            cat(out, "half-cab");
            if (t->spins > 1)
                cat(out, g_trickSpinNames[t->spins]);
        } else {
            cat(out, g_trickSpinNames[t->spins]);
        }
    }
    if (full) {
        if (t->flags & 1)
            cat(out, "opposite");
        if (t->flags & 2)
            cat(out, "alley-oop");
    }
    if (t->mod != 0xFF) {
        if (t->modCount > 1) {
            n = (uint32_t)t->modCount - 2;
            cat(out, g_trickMultiNames[n > 3 ? 3 : n]);
        }
        cat(out, g_trickNames[0x2E + t->mod]);
    }
    if (t->trick != 0xFF)
        cat(out, g_trickNames[t->trick]);
    else if (t->spins != 0 && t->mod == 0xFF)
        zstringPrintf(out + zstringLength(out), " %s", "spin");

    switch (t->trick) {
    case 1:                                             /* can-can */
        if (t->mod != 1)
            return;
        out[0] = '\0';
        if (t->spins != 0) {
            if (full && (t->flags & 4)) {
                cat(out, "half-cab");
                if (t->spins > 1)
                    cat(out, g_trickSpinNames[t->spins]);
            } else {
                cat(out, g_trickSpinNames[t->spins]);
            }
        }
        if (full) {
            if (t->flags & 1)
                cat(out, "opposite");
            if (t->flags & 2)
                cat(out, "alley-oop");
        }
        cat(out, g_trickNames[0x2E + 7]);               /* "no footer" */
        cat(out, g_trickNames[t->trick]);
        return;
    case 3:                                             /* backflip */
        if (t->spins == 1) {
            out[0] = '\0';
            if (t->mod != 0xFF) {
                if (t->modCount > 1) {
                    n = (uint32_t)t->modCount - 2;
                    cat(out, g_trickMultiNames[n > 3 ? 3 : n]);
                }
                cat(out, g_trickNames[0x2E + t->mod]);
            }
            cat(out, "flair");
            return;
        }
        if (t->mod != 0)                                /* + barspin */
            return;
        out[0] = '\0';
        switch (t->modCount) {
        case 0: case 1: cat(out, "flip");        break;
        case 2:         cat(out, "limo");        break;
        case 3:         cat(out, "triple flip"); break;
        default:        cat(out, "vette");       break;
        }
        cat(out, " driver");                            /* sic: two spaces */
        return;
    case 4:                                             /* superman */
        if (t->mod != 6)
            return;
        out[0] = '\0';
        if (t->spins != 0) {
            if (full && (t->flags & 4)) {
                cat(out, "half-cab");
                if (t->spins > 1)
                    cat(out, g_trickSpinNames[t->spins]);
            } else {
                cat(out, g_trickSpinNames[t->spins]);
            }
        }
        if (full) {
            if (t->flags & 1)
                cat(out, "opposite");
            if (t->flags & 2)
                cat(out, "alley-oop");
        }
        cat(out, g_trickNames[t->trick]);
        cat(out, g_trickNames[0x2E + t->mod]);
        return;
    case 10:                                            /* front flip */
        if (t->spins != 2)
            return;
        out[0] = '\0';
        if (t->mod != 0xFF) {
            if (t->modCount > 1) {
                n = (uint32_t)t->modCount - 2;
                cat(out, g_trickMultiNames[n > 3 ? 3 : n]);
            }
            cat(out, g_trickNames[0x2E + t->mod]);
        }
        cat(out, "flail");
        return;
    case 0xFF:                                          /* no trick: barspin + spin */
        if (t->mod != 0 || t->spins <= 1)
            return;
        out[0] = '\0';
        if (full) {
            if (t->flags & 4)
                cat(out, "half-cab");
            if (t->flags & 1)
                cat(out, "opposite");
            if (t->flags & 2)
                cat(out, "alley-oop");
        }
        if (t->spins != 2)
            cat(out, g_trickSpinNames[t->spins]);
        if (t->modCount > 1) {
            n = (uint32_t)t->modCount - 2;
            cat(out, g_trickMultiNames[n > 3 ? 3 : n]);
        }
        cat(out, "truck driver");
        return;
    }
}

/* VA 0x0041f930, 22 bytes, [I] row, status draft */
static void overlayDemoShow(int on)
{
    zoveriiShow(g_overlayDemoText, on);
}

/* VA 0x0041f960, 118 bytes, [I] row, status draft */
static void overlayDemoInit(int on)
{
    /* item 15: "DEMO PLAY" (0x10BA) during the attract demo */
    const char *s;

    if (!on) {
        zoveriiFree(g_overlayDemoText);
        g_overlayDemoText = ZOVER_NO_HANDLE;
        return;
    }
    s = zTextGet(0x10BA);
    g_overlayDemoText = zoveriiAddText(ZOVER_LAYER_HUD, &g_overlaySmallFont, s,
                                       (int16_t)(0x210 - ((uint16_t)smallWidth(s) >> 1)), 0x3C, 0x00C09020, 0);
    zoveriiShow(g_overlayDemoText, 0);
}

/* VA 0x0041fa00, 7 bytes, [I] row, status draft */
static void overlayDemoDraw(void)
{
    g_overlayDemoFrames++;
}

/*
 * overlay.h - Bmx/Src/com/action/overlay.c + overlymp.c: the in-game HUD and the overlay screens
 * (DM-054). Drawn through zoverii.c (zoverii.h).
 *
 * Offsets and tables [V] (read back from the exe by tests/test_decomp_dm054.py), names [I].
 * Notes and the screen / item catalogue: docs/re/hud.md.
 *
 * Two mechanisms:
 *  - 17 HUD items (g_overlayItems, 0x00556F00): independent widgets (score, timer, meters, trick
 *    names, hints, messages ...). The game talks to them by number: overlayShowItem(item, on),
 *    overlaySetItem(item, &data), overlayGetItem(item, &data); overlayDraw calls every visible
 *    item's draw function once per frame.
 *  - 26 overlay screens (g_overlayScreens, 0x005571B8): pause menu, options, results, name entry,
 *    records, the multiplayer screens (overlymp.c) ... overlaySetScreen(n) opens one; while a screen
 *    is open the game is paused (actionSetMode) and overlayUpdate feeds it the pad.
 * Pads: "ok" = PAD_FIRE4 | PAD_FIRE6 (cross / Enter), "back" = PAD_FIRE1 | PAD_FIRE5 (triangle / Esc),
 * PAD_PAUSE = Start / Esc (contrler.h).
 */
#ifndef OVERLAY_H
#define OVERLAY_H
#include "dmtypes.h"
#include "zoverii.h"

/* HUD items (overlayShowItem / overlaySetItem / overlayGetItem) */
enum {
    OVL_ITEM_LETTERBOX = 0,     /* black bars above and below (replays, demo) */
    OVL_ITEM_SCORE     = 1,     /* run score, rolls up; OverlayScoreReq */
    OVL_ITEM_TIMER     = 2,     /* m:ss of the run timer; u32 frames */
    OVL_ITEM_INPUT     = 3,     /* the stick directions entered so far (button font) */
    OVL_ITEM_TRICKS    = 4,     /* the names of the current trick set, scrolling up */
    OVL_ITEM_METER     = 5,     /* height / distance meter in feet; OverlayMeterReq */
    OVL_ITEM_BALANCE   = 6,     /* grind / manual balance bar; float -80..80 */
    OVL_ITEM_CRASH     = 7,     /* bail reason (text 0x1056 + id); u8 id, 0xFF none */
    OVL_ITEM_UNUSED8   = 8,     /* stubs; keeps one byte */
    OVL_ITEM_REPLAY    = 9,     /* replay icon R / P / p (icon font); u16 icon */
    OVL_ITEM_HINT      = 10,    /* tutorial hint, 3 lines with button glyphs; OverlayTimedText */
    OVL_ITEM_GOAL      = 11,    /* challenge text when entering its area; OverlayTimedText */
    OVL_ITEM_MESSAGE   = 12,    /* "NEW HIGH SCORE!" etc. (text 0x1063 + id); OverlayMessage */
    OVL_ITEM_TRACK     = 13,    /* now playing: artist / title (text 0x102E + 4 track); OverlayTimedText */
    OVL_ITEM_LOGO      = 14,    /* a sprite (image slot loaded from resource 0x39) */
    OVL_ITEM_DEMO      = 15,    /* "DEMO PLAY" */
    OVL_ITEM_FADE      = 16,    /* full-screen fade in / out; OverlayFadeReq */
    OVL_NUM_ITEMS      = 17
};

/* overlay screens (overlaySetScreen) */
enum {
    OVL_SCREEN_NONE          = 0,
    OVL_SCREEN_PAUSE         = 1,
    OVL_SCREEN_OPTIONS       = 2,
    OVL_SCREEN_AUDIO         = 3,
    OVL_SCREEN_TRICKS        = 4,   /* trick list, back to the pause menu */
    OVL_SCREEN_TRICKS_JUDGES = 5,   /* ... back to the judges' scores */
    OVL_SCREEN_TRICKS_RUN    = 6,   /* ... back to "run finished" */
    OVL_SCREEN_CONFIRM       = 7,   /* "EXIT?" / "END RUN?" yes / no */
    OVL_SCREEN_STANDINGS     = 8,   /* competition standings */
    OVL_SCREEN_JUDGES        = 9,   /* judges' scores after a competition run */
    OVL_SCREEN_RUN_OVER      = 10,  /* "RUN FINISHED" */
    OVL_SCREEN_SAVE          = 11,  /* memory card save pages */
    OVL_SCREEN_INTRO         = 12,  /* level / game-mode description before the run */
    OVL_SCREEN_GOALS_START   = 13,  /* the challenge list, then start the run */
    OVL_SCREEN_GOALS_PAUSE   = 14,  /* ... from the pause menu */
    OVL_SCREEN_GOALS_END     = 15,  /* ... at level end, then save */
    OVL_SCREEN_RESULTS       = 16,  /* challenge / competition results and rewards */
    OVL_SCREEN_NAME_ENTRY    = 17,  /* "NEW RECORD!  ENTER YOUR NAME" */
    OVL_SCREEN_RECORDS_PAUSE = 18,  /* records, back to the pause menu */
    OVL_SCREEN_RECORDS_NEW   = 19,  /* records after the name entry, then save / continue */
    OVL_SCREEN_RECORDS_JUDGES = 20,
    OVL_SCREEN_RECORDS_RUN   = 21,
    OVL_SCREEN_MP_SELECT     = 22,  /* overlymp.c: multiplayer game select */
    OVL_SCREEN_MP_WORD       = 23,  /* overlymp.c: B-M-X word entry */
    OVL_SCREEN_MP_TURN       = 24,  /* overlymp.c: "READY!" / player n */
    OVL_SCREEN_MP_STANDINGS  = 25,  /* overlymp.c: "TIME UP" / winner */
    OVL_SCREEN_BACK          = 26,  /* reopen the last screen (g_overlayLastScreen) */
    OVL_NUM_SCREENS          = 26
};

/* text colours (zoverii colour words) */
#define OVL_COL_TITLE    0x00F0F0F0u
#define OVL_COL_ITEM     0x013040E0u    /* menu items: blue, blend 1 */
#define OVL_COL_LABEL    0x023040E0u
#define OVL_COL_GREY     0x02D0D0D0u
#define OVL_COL_VALUE    0x00D0D0D0u
#define OVL_COL_ORANGE   0x02A07010u
#define OVL_COL_NEW      0x02C05050u    /* a beaten record blinks red */

typedef void (__cdecl *OverlayOnFn)(int on);
typedef int  (__cdecl *OverlayUpdateFn)(void);
typedef void (__cdecl *OverlayDrawFn)(void);
typedef void (__cdecl *OverlaySetFn)(const void *data);
typedef void (__cdecl *OverlayGetFn)(void *data);

/* one overlay screen (g_overlayScreens[n] -> 0x00557098 ...) */
typedef struct OverlayScreen {
    OverlayOnFn     enter;          /* +0x00 build (1) / tear down (0) the screen's text */
    OverlayUpdateFn update;         /* +0x04 pad handling; nonzero = input used */
    OverlayDrawFn   draw;           /* +0x08 per-frame drawing; NULL: highlight item 'sel' */
    uint8_t  count;                 /* +0x0C menu items */
    uint8_t  sel;                   /* +0x0D selected item */
    uint16_t pad_0e;
} OverlayScreen;                    /* 0x10 */

/* one HUD item (g_overlayItems[17], 0x00556F00) */
typedef struct OverlayItem {
    uint32_t      flags;            /* +0x00 bit 0: hidden */
    OverlayOnFn   init;             /* +0x04 overlayInit (1) / overlayShutdown (0) */
    OverlayDrawFn draw;             /* +0x08 every frame while not hidden (no screen open) */
    OverlayOnFn   show;             /* +0x0C overlayShowItem */
    OverlaySetFn  set;              /* +0x10 overlaySetItem */
    OverlayGetFn  get;              /* +0x14 overlayGetItem */
} OverlayItem;                      /* 0x18 */

#define OVL_ITEM_HIDDEN 1u

/* a menu line: what it does and its text (pause menu 0x0056BEFC, run-over menu 0x0056C028) */
typedef struct OverlayMenuEntry {
    uint16_t action;                /* +0x00 */
    uint16_t text;                  /* +0x02 text id */
} OverlayMenuEntry;                 /* 0x04 */

/* a memory-card page of the save screen (g_overlaySavePages[12], 0x00557224) */
typedef struct OverlaySavePage {
    uint8_t  count;                 /* +0x00 menu items (0: none) */
    uint8_t  pad_01;
    uint16_t text;                  /* +0x02 first item text; the 4 message lines are 0x1112 + 4 page */
} OverlaySavePage;                  /* 0x04 */

/* a trick-list line (g_overlayTrickList[84], 0x0056BD6C) */
typedef struct OverlayTrickEntry {
    uint8_t  kind;                  /* +0x00 low 6 bits: 0 trick, 1 modifier; 0x80 landed */
    uint8_t  id;                    /* +0x01 trick id or modifier id */
} OverlayTrickEntry;                /* 0x02 */

/* data of OVL_ITEM_SCORE */
typedef struct OverlayScoreReq {
    uint32_t score;                 /* +0x00 */
    uint8_t  flags;                 /* +0x04 bit 0: show at once (no roll-up) */
    uint8_t  pad_05[3];
} OverlayScoreReq;                  /* 0x08 */

/* data of OVL_ITEM_METER (gameUpdateTricks: big air, skids) */
typedef struct OverlayMeterReq {
    float    value;                 /* +0x00 cm; 0 hides the meter */
    float    feet;                  /* +0x04 (computed by the item) */
    uint16_t unk_08;
    uint16_t stat;                  /* +0x0A record stat whose all-time best is marked (4 big air, 9 skid) */
} OverlayMeterReq;                  /* 0x0C */

/* data of OVL_ITEM_HINT / GOAL / TRACK: an id (0xFF: clear) and the frames left */
typedef struct OverlayTimedText {
    uint8_t  id;                    /* +0x00 */
    uint8_t  pad_01;
    uint16_t timer;                 /* +0x02 set by the item: 90 (hint), 120 (goal), 360 (track) */
} OverlayTimedText;                 /* 0x04 */

/* data of OVL_ITEM_MESSAGE */
typedef struct OverlayMessage {
    uint8_t  id;                    /* +0x00 text 0x1063 + id; 12 = "TRICK COMBO!" with a count; 0xFF clear */
    uint8_t  pad_01;
    uint16_t timer;                 /* +0x02 90 frames */
    uint32_t value;                 /* +0x04 shown as a distance for ids 16..21 */
} OverlayMessage;                   /* 0x08 */

/* data of OVL_ITEM_FADE */
typedef struct OverlayFadeReq {
    uint8_t  flags;                 /* +0x00 bit 0: fade in (from the colour), else out; bit 1: additive */
    uint8_t  r, g, b;               /* +0x01 */
    uint16_t frames;                /* +0x04 */
} OverlayFadeReq;                   /* 0x06 */

/* state of OVL_ITEM_FADE (0x0056C050) */
typedef struct OverlayFade {
    uint8_t  flags;                 /* +0x00 */
    uint8_t  pad_01[3];
    uint32_t frames;                /* +0x04 left */
    float    rgb[3];                /* +0x08 */
    float    step[3];               /* +0x14 per frame */
} OverlayFade;                      /* 0x20 */

/* GHIDRA-SKIP-BEGIN */
typedef char overlay_h_size_check[(sizeof(OverlayScreen) == 0x10 && sizeof(OverlayItem) == 0x18 &&
                                   sizeof(OverlayMenuEntry) == 0x04 && sizeof(OverlaySavePage) == 0x04 &&
                                   sizeof(OverlayTrickEntry) == 0x02 && sizeof(OverlayScoreReq) == 0x08 &&
                                   sizeof(OverlayMeterReq) == 0x0C && sizeof(OverlayTimedText) == 0x04 &&
                                   sizeof(OverlayMessage) == 0x08 && sizeof(OverlayFadeReq) == 0x06 &&
                                   sizeof(OverlayFade) == 0x20) ? 1 : -1];

/* overlay.c (DM-054): the interface used by action.c / game.c */
void     overlayInit(void);                                    /* 0x004147E0 */
void     overlayShutdown(void);                                /* 0x004148B0 */
void     overlayLoadFonts(void);                               /* 0x00414970 */
void     overlayLoadStrings(uint8_t language);                 /* 0x00414DD0 */
int      overlayDraw(void);                                    /* 0x00414F60 */
int      overlayUpdate(void);                                  /* 0x00415140 */
void     overlayShowItem(int item, uint32_t on);               /* 0x00415300 */
void     overlaySetItem(int item, const void *data);           /* 0x00415370 */
void     overlayGetItem(int item, void *data);                 /* 0x004153A0 */
uint8_t  overlayGetScreen(void);                               /* 0x004153D0 */
void     overlaySetScreen(uint32_t screen);                    /* 0x00415400 */
void     overlayDrawTinyText(const char *s, int16_t x, int16_t y, uint32_t colour); /* 0x004156F0 */
void     overlaySetPage(uint8_t page);                         /* 0x00415730 */
void     overlaySetBackdrop(uint32_t mode);                    /* 0x00415770 */
void     overlayDrawMenu(uint32_t flags, int first);           /* 0x0041E7F0 */
int      overlayConfirmed(uint32_t minSel);                    /* 0x0041E920 */
uint32_t overlayPulseColour(void);                             /* 0x0041E9C0 */
void     overlayFormatDistance(char *out, float cm);           /* 0x0041EA40 */
void     overlayTrickName(char *out, const BmxTrickRec *t, int full); /* 0x0041EAB0 */

/* overlymp.c (DM-054) */
void     overlympDrawTitle(const char *s);                     /* 0x00412620 */
void     overlympDrawRoundLeader(int y);                       /* 0x004134A0 */
void     overlympDrawLetters(int *y);                          /* 0x00414580 */

extern OverlayScreen *g_overlayScreen;     /* 0x0056BF20 */
extern OverlayScreen *g_overlayScreens[OVL_NUM_SCREENS]; /* 0x005571B8 */
extern ZoverFont g_overlaySmallFont;       /* 0x0056BF38 */
extern ZoverFont g_overlayTinyFont;        /* 0x0056BE98 */
extern ZoverFont g_overlayButtonFont;      /* 0x0056C008 */
extern ZoverFont g_overlayIconFont;        /* 0x0056BF98 */
extern uint8_t   g_overlayConfirmReturn;   /* 0x0056BE32 */
extern uint32_t  g_overlayFrame;           /* 0x0056BE14 */
extern uint32_t  g_overlayBlink;           /* 0x0056BF68 */
extern uint32_t  g_overlayCurrentOnly;     /* 0x0056BE3C */
/* GHIDRA-SKIP-END */

#endif /* OVERLAY_H */

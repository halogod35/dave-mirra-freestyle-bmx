/*
 * gui.h - the front-end GUI: Bmx/Src/com/menu/gui/*.c and the SCRN / EDEF screen files
 * of the art library (intart.zal; on PC the nested archive in Intwrap.zal group 0).
 *
 * Offsets [V] (the screen loader guiLoadScreenFile 0x00439A00 and the element code read them;
 * tests/test_decomp_frontend.py checks them against the exe and the data), names [I].
 * Notes: docs/re/frontend.md; file format: docs/formats/ZAL.md section 3.2.
 *
 * Model. A screen is a SCRN asset (id 0xA000 | n) inside zprim group 0xE000 | n. Loading it
 * creates one GuiElement per EDEF record (at most 100, g_guiElements). Everything the menus do is
 * a message: guiSendMessage(msg, elem, a, b) calls the element's custom handler (set by
 * frontend.c, elem->handler) or else its type handler. Every frame guiUpdate turns the pad into
 * input messages (GUI_MSG_UP ...), offers each to the screen handler (g_guiStyle.handler, a
 * frontend.c function) and the elements, then broadcasts GUI_MSG_FRAME and GUI_MSG_DRAW.
 */
#ifndef GUI_H
#define GUI_H
#include "dmtypes.h"
#include "zprim.h"
#include "contrler.h"

#define GUI_MAX_ELEMENTS   100
#define GUI_ID_SCREEN(id)  ((uint32_t)(id) >> 16)         /* 0xA0050003 -> screen 0xA005 */

/* element types (EDEF +0x0E, guiCreateElement). [V] every EDEF in the shipped screens is type 1;
 * the frontend gives elements behaviour through custom handlers instead. */
enum {
    GUI_TYPE_SPRITE = 1,   /* sprite + text: guiElementHandler */
    GUI_TYPE_BUTTON = 2,   /* focusable: guiButtonHandler (focus moves along link[]) */
    GUI_TYPE_TOGGLE = 3,   /* "a|b|c" option text cycled with left / right: guiToggleHandler */
    GUI_TYPE_SPRITE5 = 5,  /* as 1 */
    GUI_TYPE_NULL   = 6,   /* handler returns 0 */
    GUI_TYPE_LINE   = 7,   /* a line (x, y) - (x1, y1): guiLineHandler */
    GUI_TYPE_BOX    = 8    /* a double rectangle: guiBoxHandler */
};

/* GuiElement.flags (EDEF +0x0C) */
enum {
    GUIF_HIDDEN    = 0x0001,  /* skipped by guiSendMessage (guiHide / guiShow) */
    GUIF_FOCUS     = 0x0002,  /* has the focus: gets the input messages, drawn "selected" */
    GUIF_DEFAULT   = 0x0010,  /* EDEF only: focused when the screen loads */
    GUIF_SCALED    = 0x0020,  /* scaleX / scaleY apply */
    GUIF_WIDE      = 0x0040,  /* width * 160 / 100 */
    GUIF_CENTRE_X  = 0x0080,
    GUIF_CENTRE_Y  = 0x0100,
    GUIF_PULSE     = 0x0200,  /* focused sprite drawn with the pulsing colour (guiSetPulse) */
    GUIF_HIGHLIGHT = 0x0400   /* sprite / text tinted with highlight + alpha (guiSetHighlight) */
};

/* GuiElement.spriteFlags (EDEF +0xA4) */
enum {
    GUIS_NOSPRITE  = 0x0003,  /* bits 0-1: no sprite part */
    GUIS_MODE3     = 0x0004,  /* ZPRIM_ATTR_SPR_MODE3 */
    GUIS_MODE1     = 0x0008,  /* ZPRIM_ATTR_SPR_MODE1 + spriteParam */
    GUIS_ANIM      = 0x0100,  /* frames[] / times[] animation runs */
    GUIS_BACKWARDS = 0x0200,
    GUIS_LOOP      = 0x0400,
    GUIS_PINGPONG  = 0x0800
};

/* GuiElement.textFlags (EDEF +0xAC) */
enum {
    GUIT_NOTEXT   = 0x0003,
    GUIT_MODE3    = 0x0004,   /* ZPRIM_ATTR_FONT_MODE3 */
    GUIT_HIGHLIGHT= 0x0008,   /* text uses the element highlight */
    GUIT_RGB      = 0x0010,   /* ZPRIM_ATTR_FONT_MODE2 + rgb */
    GUIT_CENTRE_X = 0x0020,
    GUIT_CENTRE_Y = 0x0040,
    GUIT_RIGHT    = 0x0080,
    GUIT_FIXED    = 0x2000    /* monospaced: zprimDrawTextFixed, width = length * spacing */
};

/* messages: guiSendMessage(msg, elem, a, b) */
enum {
    GUI_MSG_CREATE   = 0x0A,  /* the element was created */
    GUI_MSG_UPDATE   = 0x0B,  /* per frame (from GUI_MSG_FRAME) */
    GUI_MSG_DRAW_ELEM= 0x0C,  /* draw; a = 1: draw as selected */
    GUI_MSG_DESTROY  = 0x0D,
    GUI_MSG_FOCUS    = 0x0E,
    GUI_MSG_BLUR     = 0x0F,
    GUI_MSG_TEXT     = 0x10,  /* the text changed; a = the new text */
    GUI_MSG_LOADED   = 0x14,  /* broadcast after a screen was loaded */
    GUI_MSG_DRAW     = 0x15,  /* broadcast each frame: the default screen handler sends DRAW_ELEM */
    GUI_MSG_FRAME    = 0x16,  /* broadcast each frame before DRAW: screen logic; default sends UPDATE */
    GUI_MSG_UNLOAD   = 0x17,  /* broadcast before the elements are destroyed */
    GUI_MSG_SLID_IN  = 0x18,  /* every element finished sliding in */
    /* input from pad 0 (guiinput.c); d-pad messages auto-repeat */
    GUI_MSG_UP       = 0x1E,
    GUI_MSG_DOWN     = 0x1F,
    GUI_MSG_LEFT     = 0x20,
    GUI_MSG_RIGHT    = 0x21,
    GUI_MSG_ACCEPT   = 0x22,  /* Cross (D) or Enter */
    GUI_MSG_CIRCLE   = 0x23,  /* Circle (F) */
    GUI_MSG_START    = 0x24,  /* Start (Space) */
    GUI_MSG_BACK     = 0x25,  /* Triangle (E) or Esc */
    GUI_MSG_SQUARE   = 0x26,  /* Square (S): help mode only */
    GUI_MSG_L1       = 0x27,
    GUI_MSG_R1       = 0x28,
    GUI_MSG_L2       = 0x29,
    GUI_MSG_R2       = 0x2A,
    /* 0x2B..0x37 = 0x1E..0x2A + 13: the second pad of the PSX version. [V] nothing on PC sends
     * them (guiInputGetPad asserts pad 0), but every handler still accepts both sets. */
    GUI_MSG_PAD2     = 0x0D,
    GUI_MSG_AUTO_BACK= 0x32,  /* = pad-2 BACK; guiUpdate sends it every frame while
                                 g_guiAutoBack (0x005AB474) is set, to unwind the menus */
    GUI_MSG_LEFT_HELD  = 0x3E,/* left / right held alone, every frame */
    GUI_MSG_RIGHT_HELD = 0x3F
};

struct GuiElement;   /* file scope; without it the next line declares a prototype-scoped struct */
typedef int (*GuiHandler)(uint32_t msg, struct GuiElement *elem, uint32_t a, uint32_t b);
typedef void (*GuiTask)(void);    /* frontend.c's per-frame task (g_feTask) */

/* One element (g_guiElements[100] 0x0056D2C0). */
typedef struct GuiElement {
    uint16_t type;            /* +0x00 GUI_TYPE_*, 0 = free slot */
    uint16_t flags;           /* +0x02 GUIF_* */
    uint32_t id;              /* +0x04 screen << 16 | number (EDEF +0x04) */
    uint8_t  link[4];         /* +0x08 up, down, left, right: low byte of the neighbour's id */
    int16_t  x;               /* +0x0C */
    int16_t  y;               /* +0x0E */
    int16_t  w;               /* +0x10 */
    int16_t  h;               /* +0x12 */
    int16_t  slideX;          /* +0x14 current slide-in offset */
    int16_t  slideY;          /* +0x16 */
    int16_t  slideX0;         /* +0x18 start offset (EDEF +0x1E) */
    int16_t  slideY0;         /* +0x1A */
    int16_t  delayX;          /* +0x1C frames (x4 / x3 px) to wait, so all elements arrive together */
    int16_t  delayY;          /* +0x1E */
    float    scaleX;          /* +0x20 */
    float    scaleY;          /* +0x24 */
    uint16_t priority;        /* +0x28 draw order: g_guiList is sorted by it, highest first */
    uint16_t pad_2a;          /* +0x2A */
    uint32_t highlight;       /* +0x2C 0x00BBGGRR (ZPRIM_ATTR_CLUT38) */
    uint32_t alpha;           /* +0x30 (ZPRIM_ATTR_CLUT34) */
    ZprimAsset *sprite;       /* +0x34 */
    ZprimAsset *selSprite;    /* +0x38 drawn instead while selected */
    int16_t  spriteX;         /* +0x3C sprite offset (line / box: the second corner) */
    int16_t  spriteY;         /* +0x3E */
    uint16_t spriteFlags;     /* +0x40 GUIS_* */
    uint16_t frame;           /* +0x42 sprite frame drawn */
    int16_t  animIndex;       /* +0x44 */
    uint16_t animTimer;       /* +0x46 */
    uint32_t palette;         /* +0x48 ZPRIM_ATTR_SPR_ARG30 */
    uint16_t spriteParam;     /* +0x4C ZPRIM_ATTR_SPR_ARG24 */
    uint16_t animCount;       /* +0x4E */
    uint16_t animFrames[30];  /* +0x50 */
    uint16_t animTimes[30];   /* +0x8C frames each */
    uint16_t textFlags;       /* +0xC8 GUIT_* */
    int16_t  spacing;         /* +0xCA ZPRIM_ATTR_SPACING */
    int16_t  textX;           /* +0xCC */
    int16_t  textY;           /* +0xCE */
    ZprimAsset *font;         /* +0xD0 */
    uint32_t textPalette;     /* +0xD4 ZPRIM_ATTR_FONT_ARG40: 0 normal, 1 selected, 2 grey ... */
    int      text;            /* +0xD8 DMM handle of the text (zmemDMMAlloc) */
    uint32_t textSize;        /* +0xDC bytes allocated */
    uint8_t  rgb[3];          /* +0xE0 text / line colour */
    uint8_t  pad_e3;          /* +0xE3 */
    GuiHandler handler;       /* +0xE4 custom handler (frontend.c), tried first */
    GuiHandler typeHandler;   /* +0xE8 */
    int      data;            /* +0xEC DMM handle of the type data (toggle: GuiToggleData) */
} GuiElement;                 /* 0xF0 */

/* Type data of a toggle (GUI_TYPE_TOGGLE, guitoggl.c), in a DMM block. */
typedef struct GuiToggleData {
    uint32_t unk_00;          /* +0x00 */
    uint16_t index;           /* +0x04 selected option */
    uint16_t count;           /* +0x06 number of options (guiToggleSetCount) */
    int      text;            /* +0x08 DMM handle: the selected option's text */
    uint16_t flags;           /* +0x0C GUIT_* */
    int16_t  spacing;         /* +0x0E */
    int16_t  textX;           /* +0x10 */
    int16_t  textY;           /* +0x12 */
    ZprimAsset *font;         /* +0x14 */
    uint32_t palette;         /* +0x18 */
    int      options;         /* +0x1C DMM handle: "a|b|c" */
    uint8_t  rgb[3];          /* +0x20 */
    uint8_t  pad_23;          /* +0x23 */
} GuiToggleData;              /* 0x24 */

/* The current screen's style (g_guiStyle 0x00573088; guiInitializeMenuMode clears 6 dwords). */
typedef struct GuiStyle {
    ZprimAsset *backdrop;     /* +0x00 picture behind the screen (SCRN +0x0C or guiLoadBackdropImage) */
    uint32_t unk_04;          /* +0x04 */
    uint16_t focusFont;       /* +0x08 font of a focused button (SCRN +0x20) */
    uint8_t  focusRGB[3];     /* +0x0A (SCRN +0x22) */
    uint8_t  pad_0d;          /* +0x0D */
    uint16_t blurFont;        /* +0x0E (SCRN +0x26) */
    uint8_t  blurRGB[3];      /* +0x10 (SCRN +0x28) */
    uint8_t  pad_13;          /* +0x13 */
    GuiHandler handler;       /* +0x14 screen handler (NULL: guiDefaultScreenHandler) */
} GuiStyle;                   /* 0x18 */

/* The menu pad (g_guiPad 0x0056D298; guiinput.c). */
typedef struct GuiPad {
    ZInputDevice *device;     /* +0x00 zinputOpen(2), else (0) */
    uint32_t held;            /* +0x04 device->buttons */
    uint32_t pressed;         /* +0x08 device->changed; frontendUpdateApp re-presses the d-pad */
    uint16_t flags;           /* +0x0C bit 0: disconnected */
    uint16_t repeat;          /* +0x0E auto-repeat timer (frontendUpdateApp) */
    int32_t  unk_10;          /* +0x10 */
    int32_t  unk_14;          /* +0x14 */
    int32_t  unk_18;          /* +0x18 */
    int32_t  unk_1c;          /* +0x1C */
} GuiPad;                     /* 0x20 */

/* ---- screen file (zprim asset type 0xA000) ---- */

/* SCRN header; the asset is: header, EDEF[edefCount] at edefOffset, "STRS" + strings at strsOffset. */
typedef struct GuiScrnHeader {
    char     tag[4];          /* +0x00 "SCRN" (checked) */
    uint32_t size;            /* +0x04 whole record, STRS included */
    uint32_t id;              /* +0x08 0xA000 | n */
    uint16_t backdrop;        /* +0x0C picture asset, 0 none (guiLoadBackdrop) */
    uint16_t unk_0e;          /* +0x0E */
    int32_t  edefCount;       /* +0x10 */
    int32_t  edefOffset;      /* +0x14 from the record start (0x2C) */
    int32_t  textCount;       /* +0x18 strings in STRS (not read by the loader) */
    int32_t  strsOffset;      /* +0x1C text offsets are relative to the "STRS" tag */
    uint16_t focusFont;       /* +0x20 */
    uint8_t  focusRGB[3];     /* +0x22 */
    uint8_t  pad_25;          /* +0x25 */
    uint16_t blurFont;        /* +0x26 */
    uint8_t  blurRGB[3];      /* +0x28 */
    uint8_t  pad_2b;          /* +0x2B */
} GuiScrnHeader;              /* 0x2C */

/* EDEF: one element. */
typedef struct GuiEdef {
    char     tag[4];          /* +0x00 "EDEF" (checked) */
    uint32_t id;              /* +0x04 */
    uint8_t  link[4];         /* +0x08 */
    uint16_t flags;           /* +0x0C GUIF_* */
    uint16_t type;            /* +0x0E GUI_TYPE_* */
    uint16_t spriteParam;     /* +0x10 used when spriteFlags & GUIS_MODE1 */
    int16_t  x;               /* +0x12 */
    int16_t  y;               /* +0x14 */
    int16_t  w;               /* +0x16 */
    int16_t  h;               /* +0x18 */
    int16_t  scaleX;          /* +0x1A 8.8 fixed point (GUIF_SCALED) */
    int16_t  scaleY;          /* +0x1C */
    int16_t  slideX;          /* +0x1E */
    int16_t  slideY;          /* +0x20 */
    uint16_t priority;        /* +0x22 */
    uint16_t sprite;          /* +0x24 asset id, 0 none */
    uint16_t frame;           /* +0x26 */
    uint16_t animCount;       /* +0x28 */
    uint16_t animFrames[30];  /* +0x2A */
    uint16_t animTimes[30];   /* +0x66 */
    uint16_t selSprite;       /* +0xA2 */
    uint16_t spriteFlags;     /* +0xA4 GUIS_*; bit 1 set = no sprite fields */
    int16_t  spriteX;         /* +0xA6 */
    int16_t  spriteY;         /* +0xA8 */
    uint16_t font;            /* +0xAA */
    uint16_t textFlags;       /* +0xAC GUIT_*; bit 1 set = no text fields */
    int16_t  textX;           /* +0xAE */
    int16_t  textY;           /* +0xB0 */
    uint16_t pad_b2;          /* +0xB2 */
    uint32_t text;            /* +0xB4 offset into STRS, 0 none */
    uint8_t  textRGB[3];      /* +0xB8 */
    uint8_t  pad_bb;          /* +0xBB */
    int16_t  spacing;         /* +0xBC */
    uint16_t pad_be;          /* +0xBE */
    uint16_t toggleFont;      /* +0xC0 (type 3 only) */
    uint16_t toggleFlags;     /* +0xC2 */
    int16_t  toggleX;         /* +0xC4 */
    int16_t  toggleY;         /* +0xC6 */
    uint32_t toggleText;      /* +0xC8 offset into STRS: "a|b|c" */
    uint8_t  toggleRGB[3];    /* +0xCC */
    uint8_t  pad_cf;          /* +0xCF */
    int16_t  toggleSpacing;   /* +0xD0 */
    uint16_t pad_d2;          /* +0xD2 */
} GuiEdef;                    /* 0xD4 */

/* GHIDRA-SKIP-BEGIN */
typedef char gui_h_size_check[(sizeof(GuiElement) == 0xF0 && sizeof(GuiToggleData) == 0x24 &&
                               sizeof(GuiStyle) == 0x18 && sizeof(GuiPad) == 0x20 &&
                               sizeof(GuiScrnHeader) == 0x2C && sizeof(GuiEdef) == 0xD4) ? 1 : -1];

extern GuiElement  g_guiElements[GUI_MAX_ELEMENTS];   /* 0x0056D2C0 */
extern GuiElement *g_guiList[GUI_MAX_ELEMENTS];       /* 0x005730A4 sorted by priority */
extern int32_t     g_guiCount;                        /* 0x0057323C */
extern GuiStyle    g_guiStyle;                        /* 0x00573088 */
extern uint32_t    g_guiPulse;                        /* 0x00573254 16..240, the "selected" glow */
extern uint32_t    g_guiGroupLoaded;                  /* 0x00573250 zprim group slot 1 holds a screen */
extern uint32_t    g_guiAutoBack;                     /* 0x005AB474 */
extern const char  g_overlayEmpty[];                  /* 0x0056BD44 "" (overlay.c) */

/* gap_rider__guiinput.c (buttons, text markup, the help-system stubs) */
int   guiButtonHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
void  guiDrawMarkup(const char *s, int16_t x, int16_t y);
void  guiSetMarkupTarget(uint32_t id);
int   guiHelpInit(void);
int   guiNullHandler(void);
int   guiHelpActive(void);
void  guiHelpReset(void);
void  guiHelpInput(uint32_t msg);
void  guiHelpUpdate(void);
/* guiinput.c */
int   guiInputUp(int pad);
int   guiInputDown(int pad);
int   guiInputLeft(int pad);
int   guiInputRight(int pad);
int   guiInputLeftHeld(int pad);
int   guiInputRightHeld(int pad);
int   guiInputAccept(int pad);
int   guiInputCircle(int pad);
int   guiInputStart(int pad);
int   guiInputBack(int pad);
int   guiInputL1(int pad);
int   guiInputL2(int pad);
int   guiInputR1(int pad);
int   guiInputR2(int pad);
int   guiInputSquare(int pad);
void  guiInputInit(void);
void  guiInputShutdown(void);
uint32_t guiInputUpdate(void);
void  guiInputClear(void);
GuiPad *guiInputGetPad(int pad);
/* guimain.c */
int   guiLineHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
void  guiSetScreenHandler(GuiHandler handler);
int   guiInitializeApp(void);
void  guiShutdownApp(void);
int   guiInitializeMenuMode(void);
void  guiShutdownMenuMode(void);
const char *guiSetArtLibrary(const char *name);
void  guiDestroyScreen(void);
void  guiLoadScreen(uint16_t screen, GuiHandler handler);
void  guiLoadScreenNow(void);
int   guiUpdate(void);
float guiFabs(float v);
void  guiUpdatePulse(void);
GuiStyle *guiGetStyle(void);
void  guiSortElements(void);
void  guiFreeMaterial(int32_t material);
void  guiSetFlagsById(uint32_t id, uint16_t mask, int16_t on);
GuiElement *guiFindElement(uint32_t id);
GuiElement *guiGetElement(uint32_t index);
GuiElement *guiCreateElement(uint16_t type, uint16_t priority, uint32_t id);
void  guiDestroyElement(GuiElement *e);
void  guiSetFlags(GuiElement *e, uint16_t flags);
void  guiSetLink(GuiElement *e, uint32_t dir, uint8_t id);
void  guiSetPos(GuiElement *e, int16_t x, int16_t y);
void  guiSetPriority(GuiElement *e, uint16_t priority);
void  guiSetScale(GuiElement *e, float x, float y);
void  guiSetSize(GuiElement *e, int16_t w, int16_t h);
void  guiSetSlide(GuiElement *e, int16_t x, int16_t y);
void  guiSetSpriteFlags(GuiElement *e, uint16_t flags);
void  guiSetSpriteOffset(GuiElement *e, int16_t x, int16_t y);
void  guiLoadSprite(GuiElement *e, uint32_t id);
void  guiSetSprite(GuiElement *e, ZprimAsset *sprite);
void  guiLoadSelSprite(GuiElement *e, uint32_t id);
void  guiSetFrame(GuiElement *e, uint16_t frame);
void  guiSetAnimCount(GuiElement *e, uint16_t count);
void  guiSetPalette(GuiElement *e, uint32_t palette);
void  guiSetSpriteParam(GuiElement *e, uint16_t param);
void  guiSetAnim(GuiElement *e, uint32_t count, const uint16_t *frames, const uint16_t *times);
void  guiSetTextFlags(GuiElement *e, uint16_t flags);
void  guiSetTextRGB(GuiElement *e, uint8_t r, uint8_t g, uint8_t b);
void  guiSetTextPos(GuiElement *e, int16_t x, int16_t y);
void  guiSetTextPalette(GuiElement *e, uint32_t palette);
void  guiLoadFont(GuiElement *e, uint32_t id);
void  guiSetText(GuiElement *e, const char *text);
void  guiSetSpacing(GuiElement *e, int16_t spacing);
int16_t guiGetTextWidth(GuiElement *e);
void  guiLoadBackdropImage(uint16_t screen, uint16_t image);
void  guiLoadBackdrop(uint32_t id);
void  guiSetFocusFont(uint16_t font);
void  guiSetBlurFont(uint16_t font);
void  guiSetFocusRGB(uint8_t r, uint8_t g, uint8_t b);
void  guiSetBlurRGB(uint8_t r, uint8_t g, uint8_t b);
uint16_t guiGetFocusFont(void);
uint16_t guiGetBlurFont(void);
void  guiGetFocusRGB(uint8_t *r, uint8_t *g, uint8_t *b);
void  guiGetBlurRGB(uint8_t *r, uint8_t *g, uint8_t *b);
int   guiSendMessage(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
int   guiBroadcast(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
int   guiPostInput(uint32_t msg, uint32_t a, uint32_t b);
int   guiDefaultScreenHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
void  guiFocus(uint32_t id);
void  guiShow(GuiElement *e);
void  guiHide(GuiElement *e);
void  guiSetPulse(GuiElement *e, int on);
void  guiSetHighlight(GuiElement *e, uint8_t r, uint8_t g, uint8_t b, uint32_t alpha);
const char *guiTextLookup(const char *text);
int   guiBoxHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
/* guisfile.c */
int   guiLoadScreenFile(uint32_t id);
/* guitoggl.c: the generic element (types 1, 5) */
int   guiElementHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
int   guiElementDraw(uint32_t msg, GuiElement *e, uint32_t selected, uint32_t b);
/* guitoggl.c */
int   guiToggleHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
int   guiToggleVHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b);
void  guiToggleSetFlags(GuiElement *e, uint16_t flags);
void  guiToggleSetTextPos(GuiElement *e, int16_t x, int16_t y);
void  guiToggleSetPalette(GuiElement *e, uint32_t palette);
void  guiToggleLoadFont(GuiElement *e, uint32_t id);
void  guiToggleSetRGB(GuiElement *e, uint8_t r, uint8_t g, uint8_t b);
void  guiToggleSetSpacing(GuiElement *e, int16_t spacing);
void  guiToggleSetOptions(GuiElement *e, const char *options);
void  guiToggleSelect(GuiElement *e, uint16_t index);
uint16_t guiToggleGetIndex(GuiElement *e);
void  guiToggleSetCount(GuiElement *e, uint16_t count);
uint16_t guiToggleGetCount(GuiElement *e);
/* frontend.c (the app entry points NgApp calls) */
int   frontendInitApp(void);
void  frontendShutdownApp(void);
int   frontendUpdateApp(void);
void  frontendRequestExit(int quit);         /* 0: leave the menus for the game, 1: quit */
/* GHIDRA-SKIP-END */

#endif /* GUI_H */

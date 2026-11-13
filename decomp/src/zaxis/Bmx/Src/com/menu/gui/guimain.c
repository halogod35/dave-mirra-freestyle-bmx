/*
 * zaxis/Bmx/Src/com/menu/gui/guimain.c
 * Original: (no path string; the name follows its "GUIMAIN:" messages, DM-040)
 * Evidence: [V] the "GUIMAIN:" literals 0x00559D64-0x0055A128 are referenced only from
 *           0x004375D0-0x00439460; [I] the element accessors / line / box code between them by
 *           link order (0x00437460-0x004399E6, after guiinput.c, before guisfile.c), by their
 *           callers and because they touch only g_guiElements / g_guiStyle.
 * 86 functions, VA 0x00437460-0x004399e6.
 * DM-056 (draft). Non-matching C (AD-8). Types: gui.h, zprim.h, zmem.h. Notes: docs/re/frontend.md.
 *
 * The element manager: the element pool, the sorted draw list, screen loading (guiLoadScreen,
 * guiLoadScreenNow + guisfile.c), the slide-in animation, the message plumbing (guiSendMessage,
 * guiBroadcast, guiPostInput) and the per-frame update. 24 of these functions were not in the
 * Ghidra project (reached only through the /INCREMENTAL thunks): decomp/symbols/symbols.csv creates them.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "decomp.h"
#include "zmem.h"
#include "gui.h"

void psx2DRequestDraw(void);       /* NgIFpsx.c: per-frame hook of the PSX layer */
void ngappEmptyHook(void);           /* gap_zoverii__MissingZGE.c: a bare ret */
void zrwMatRelease(int32_t material);   /* zrwMat.c: destroy a material (through 0x00561024) */
void zprimCopyToPicture(ZprimAsset *sprite, ZprimAsset *picture, uint32_t a, uint32_t b);
                                   /* 0x00479260 zprim.c: a bare ret on PC */

GuiElement  g_guiElements[GUI_MAX_ELEMENTS];   /* 0x0056D2C0 */
uint16_t    g_guiPendingScreen;                /* 0x00573080 screen id for guiLoadScreenNow */
GuiStyle    g_guiStyle;                        /* 0x00573088 */
GuiHandler  g_guiPendingHandler;               /* 0x005730A0 its screen handler */
GuiElement *g_guiList[GUI_MAX_ELEMENTS];       /* 0x005730A4 sorted by priority, highest first */
uint32_t    g_guiAppInit;                      /* 0x00573234 */
uint32_t    g_guiMenuInit;                     /* 0x00573238 */
int32_t     g_guiCount;                        /* 0x0057323C */
uint32_t    g_guiScreenActive;                 /* 0x00573240 a screen is up */
uint32_t    g_guiLoadPending;                  /* 0x00573244 guiLoadScreen while a screen is up */
uint32_t    g_guiLoadTimer;                    /* 0x00573248 ... loads after 6 frames */
uint32_t    g_guiJustLoaded;                   /* 0x0057324C input is ignored for that frame */
uint32_t    g_guiGroupLoaded;                  /* 0x00573250 zprim group slot 1 holds the screen */
uint32_t    g_guiPulse;                        /* 0x00573254 16..240 */
uint32_t    g_guiPulseHold;                    /* 0x00573258 (always 0) */
uint32_t    g_guiSlideState;                   /* 0x0057325C 1 sliding in; 2 (sliding out) is never set */
int32_t     g_guiSlideMax;                     /* 0x00573260 */
const char *g_guiArtLibrary = "intart.zal";    /* 0x00559D48 -> 0x00559D54 (NgApp overrides it) */
int32_t     g_guiPulseStep = 1;                /* 0x00559D4C sign = direction of the pulse */

/* ================================================================== line element (type 7) */

/* VA 0x00437460, 93 bytes, [I] row, status draft */
int guiLineHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    if (msg != GUI_MSG_DRAW_ELEM)
        return 0;
    zprimSetRGB(e->rgb[0], e->rgb[1], e->rgb[2], ZPRIM_ATTR_COLOUR);
    zprimSetAttr(ZPRIM_ATTR_PRIM_MODE1, 1);
    zprimDrawLine(e->x, e->y, e->spriteX, e->spriteY);
    return 1;
}

/* VA 0x004374e0, 23 bytes, [I] row, status draft */
void guiLineSetStart(GuiElement *e, int16_t x, int16_t y)
{
    e->x = x;
    e->y = y;
}

/* VA 0x00437510, 23 bytes, [I] row, status draft */
void guiLineSetEnd(GuiElement *e, int16_t x, int16_t y)
{
    e->spriteX = x;
    e->spriteY = y;
}

/* VA 0x00437540, 27 bytes, [I] row, status draft */
void guiLineGetX(GuiElement *e, int16_t *x0, int16_t *x1)
{
    *x0 = e->x;
    *x1 = e->spriteX;
}

/* VA 0x00437570, 27 bytes, [I] row, status draft */
void guiLineGetY(GuiElement *e, int16_t *y0, int16_t *y1)
{
    *y0 = e->y;
    *y1 = e->spriteY;
}

/* ================================================================== app / menu mode */

/* VA 0x004375a0, 20 bytes, [I] row, status draft */
void guiSetScreenHandler(GuiHandler handler)
{
    /* the frontend swaps the screen handler for a sub-page of the same screen */
    g_guiGroupLoaded = 1;
    g_guiStyle.handler = handler;
}

/* VA 0x004375d0, 39 bytes, [I] row, status draft */
int guiInitializeApp(void)
{
    if (!g_guiAppInit) {
        g_guiAppInit = 1;
        return 0;
    }
    g_pfnDebugPrintf("GUIMAIN: Error - reinitializing app\n");
    return 0;
}

/* VA 0x00437610, 20 bytes, [I] row, status draft */
void guiShutdownApp(void)
{
    if (g_guiAppInit)
        g_guiAppInit = 0;
}

/* VA 0x00437640, 313 bytes, [I] row, status draft */
int guiInitializeMenuMode(void)
{
    memset(g_guiElements, 0, sizeof(g_guiElements));
    memset(g_guiList, 0, sizeof(g_guiList));
    memset(&g_guiStyle, 0, sizeof(g_guiStyle));
    g_guiGroupLoaded = 0;
    g_guiStyle.handler = g_guiPendingHandler;
    if (!g_guiAppInit) {
        g_pfnDebugPrintf("GUIMAIN: Error - initializing menu mode before app\n");
        return 0;
    }
    if (g_guiMenuInit) {
        g_pfnDebugPrintf("GUIMAIN: Error - reinitializing menu mode\n");
        return 0;
    }
    if (zmemDMMInit(NULL, 0x300000) != 0) {             /* 3 MB movable-block heap */
        g_pfnDebugPrintf("GUIMAIN: Failed to initialize ZMEMDMM\n");
        zprimShutdown();
        return 1;
    }
    if (zprimInit() != 0) {
        g_pfnDebugPrintf("GUIMAIN: Failed to initialize ZPRIM\n");
        return 1;
    }
    zprimOpenLibrary(g_guiArtLibrary);
    if (zprimLoadGroup(0, 0xE000) != 0) {               /* fonts and shared sprites */
        g_pfnDebugPrintf("GUIMAIN: Failed to load permanent screen assets\n");
        return 1;
    }
    if (guiHelpInit() != 0) {
        g_pfnDebugPrintf("GUIMAIN: Failed to init help system\n");
        return 1;
    }
    g_guiScreenActive = 0;
    g_guiLoadPending = 0;
    g_guiJustLoaded = 0;
    g_guiPulseHold = 0;
    g_guiMenuInit = 1;
    return 0;
}

/* VA 0x004377d0, 94 bytes, [I] row, status draft */
void guiShutdownMenuMode(void)
{
    int i;

    if (!g_guiMenuInit)
        return;
    for (i = 0; i < GUI_MAX_ELEMENTS; i++)
        if (g_guiElements[i].type != 0)
            guiDestroyElement(&g_guiElements[i]);
    zprimFreeAsset(g_guiStyle.backdrop);
    guiDestroyScreen();
    zprimUnloadGroup(0);
    zprimShutdown();
    zmemDMMShutdown();
    g_guiMenuInit = 0;
}

/* VA 0x00437850, 16 bytes, [I] row, status draft */
const char *guiSetArtLibrary(const char *name)
{
    const char *old = g_guiArtLibrary;

    g_guiArtLibrary = name;         /* NgApp: the Intwrap.zal group 0 image in memory */
    return old;
}

/* ================================================================== screens */

/* VA 0x00437870, 141 bytes, [I] row, status draft */
void guiDestroyScreen(void)
{
    int i;

    guiBroadcast(GUI_MSG_UNLOAD, NULL, 0, 0);
    for (i = 0; i < GUI_MAX_ELEMENTS; i++)
        if (g_guiElements[i].type != 0)
            guiDestroyElement(&g_guiElements[i]);
    if (g_guiCount != 0) {
        g_pfnDebugPrintf("GUIMAIN: Failed to destroy all elements\n");
        return;
    }
    memset(&g_guiStyle, 0, sizeof(g_guiStyle));
    if (g_guiGroupLoaded) {
        zprimUnloadGroup(1);
        g_guiGroupLoaded = 0;
    }
    zmemDMMOptimize();
}

/* VA 0x00437930, 56 bytes, [I] row, status draft */
void guiLoadScreen(uint16_t screen, GuiHandler handler)
{
    /* screen 0: no SCRN, just the handler. While a screen is up the change waits 6 frames. */
    g_guiPendingScreen = screen;
    g_guiPendingHandler = handler;
    if (!g_guiScreenActive) {
        guiLoadScreenNow();
        return;
    }
    g_guiLoadPending = 1;
    g_guiLoadTimer = 0;
}

/* VA 0x00437980, 528 bytes, [I] row, status draft */
void guiLoadScreenNow(void)
{
    GuiElement *e;
    int i;

    guiHelpReset();
    guiDestroyScreen();
    zprimFlush();
    g_guiStyle.handler = g_guiPendingHandler;
    if (g_guiPendingScreen != 0) {
        /* the screen's assets are zprim group 0xE000 | n, its SCRN is asset 0xA000 | n */
        if (zprimLoadGroup(1, (uint16_t)((g_guiPendingScreen & 0xFFF) | 0xE000)) != 0) {
            g_pfnDebugPrintf("GUISFILE: Failed to load screen assets\n");
            return;
        }
        g_guiGroupLoaded = 1;
        if (guiLoadScreenFile(g_guiPendingScreen) != 0) {
            g_pfnDebugPrintf("GUIMAIN: Error - failed to load screen\n");
            return;
        }
    }
    /* slide in: every element starts at its EDEF offset; the ones with a shorter way wait so all
     * arrive at the same time */
    g_guiSlideState = 1;
    g_guiSlideMax = 0;
    for (i = 0; i < GUI_MAX_ELEMENTS; i++) {
        e = &g_guiElements[i];
        if (e->type == 0)
            continue;
        e->slideX = e->slideX0;
        e->slideY = e->slideY0;
        if ((float)g_guiSlideMax < guiFabs((float)e->slideX0))
            g_guiSlideMax = (int32_t)guiFabs((float)e->slideX0);
        if ((float)g_guiSlideMax < guiFabs((float)e->slideY0))
            g_guiSlideMax = (int32_t)guiFabs((float)e->slideY0);
    }
    for (i = 0; i < GUI_MAX_ELEMENTS; i++) {
        e = &g_guiElements[i];
        if (e->type == 0)
            continue;
        e->delayX = e->slideX0 == 0 ? 0 : (int16_t)((float)g_guiSlideMax - guiFabs((float)e->slideX0));
        e->delayY = e->slideY0 == 0 ? 0 : (int16_t)((float)g_guiSlideMax - guiFabs((float)e->slideY0));
    }
    guiBroadcast(GUI_MSG_LOADED, NULL, 0, 0);
    g_guiJustLoaded = 1;
    g_guiScreenActive = 1;
}

/* VA 0x00437c20, 7 bytes, [I] row, status draft */
float guiFabs(float v)
{
    return (float)fabs(v);
}

/* step v toward target by 'step', clamped */
static int16_t approach(int16_t v, int16_t target, int16_t step)
{
    if (target < v) {
        v = (int16_t)(v - step);
        if (v < target)
            v = target;
    } else if (v < target) {
        v = (int16_t)(v + step);
        if (target < v)
            v = target;
    }
    return v;
}

/* VA 0x00437c40, 1346 bytes, [I] row, status draft */
int guiUpdate(void)
{
    /* frontendUpdateApp, once per frame in menu mode */
    GuiElement *e;
    int moving, i;

    guiUpdatePulse();
    if (!g_guiMenuInit) {
        g_pfnDebugPrintf("GUIMAIN: Error - calling update before initialization\n");
        return 1;
    }
    if (g_guiJustLoaded) {
        g_guiJustLoaded = 0;
    } else if (g_guiLoadPending && g_guiSlideState != 2) {
        if (++g_guiLoadTimer < 6)
            return 0;
        g_guiLoadPending = 0;
        g_guiScreenActive = 0;
        guiLoadScreenNow();
    }

    if (g_guiStyle.backdrop == NULL)
        zprimSetAttr(ZPRIM_ATTR_BG_NONE, 0);
    else
        zprimSetAttr(ZPRIM_ATTR_BG_PICTURE, (uint32_t)g_guiStyle.backdrop);
    zprimDrawBackground();

    /* input -> messages (pad 0) */
    if (g_guiScreenActive && !g_guiJustLoaded && !g_guiLoadPending) {
        if (g_guiAutoBack) {
            guiPostInput(GUI_MSG_AUTO_BACK, 0, 0);
        } else if (!guiHelpActive()) {
            if (guiInputStart(0))     guiPostInput(GUI_MSG_START, 0, 0);
            if (guiInputAccept(0))    guiPostInput(GUI_MSG_ACCEPT, 0, 0);
            if (guiInputCircle(0))    guiPostInput(GUI_MSG_CIRCLE, 0, 0);
            if (guiInputBack(0))      guiPostInput(GUI_MSG_BACK, 0, 0);
            if (guiInputUp(0))        guiPostInput(GUI_MSG_UP, 0, 0);
            if (guiInputDown(0))      guiPostInput(GUI_MSG_DOWN, 0, 0);
            if (guiInputLeft(0))      guiPostInput(GUI_MSG_LEFT, 0, 0);
            if (guiInputRight(0))     guiPostInput(GUI_MSG_RIGHT, 0, 0);
            if (guiInputLeftHeld(0))  guiPostInput(GUI_MSG_LEFT_HELD, 0, 0);
            if (guiInputRightHeld(0)) guiPostInput(GUI_MSG_RIGHT_HELD, 0, 0);
            if (guiInputL1(0))        guiPostInput(GUI_MSG_L1, 0, 0);
            if (guiInputR1(0))        guiPostInput(GUI_MSG_R1, 0, 0);
            if (guiInputL2(0))        guiPostInput(GUI_MSG_L2, 0, 0);
            if (guiInputR2(0))        guiPostInput(GUI_MSG_R2, 0, 0);
        } else {
            /* help mode (never on PC): both pads, Square backs out, Triangle is 0x26 */
            if (guiInputStart(0) || guiInputStart(1))   guiHelpInput(GUI_MSG_START);
            if (guiInputAccept(0) || guiInputAccept(1)) guiHelpInput(GUI_MSG_ACCEPT);
            if (guiInputLeft(0) || guiInputLeft(1))     guiHelpInput(GUI_MSG_LEFT);
            if (guiInputRight(0) || guiInputRight(1))   guiHelpInput(GUI_MSG_RIGHT);
            if (guiInputUp(0) || guiInputUp(1))         guiHelpInput(GUI_MSG_UP);
            if (guiInputDown(0) || guiInputDown(1))     guiHelpInput(GUI_MSG_DOWN);
            if (guiInputSquare(0) || guiInputSquare(1)) guiHelpInput(GUI_MSG_BACK);
            if (guiInputBack(0) || guiInputBack(1))     guiHelpInput(GUI_MSG_SQUARE);
        }
    }

    /* slide animation: x moves 4 px a frame, y 3 px */
    moving = 0;
    for (i = 0; i < g_guiCount; i++) {
        e = g_guiList[i];
        if (e->type == 0)
            continue;
        if (g_guiSlideState == 1) {
            e->slideX = approach(e->slideX, 0, 4);
            e->slideY = approach(e->slideY, 0, 3);
            if (e->slideX == 0 && e->slideY == 0)
                continue;
        } else if (g_guiSlideState == 2) {
            if (e->delayX == 0)
                e->slideX = approach(e->slideX, e->slideX0, 4);
            else
                e->delayX = (uint16_t)e->delayX < 5 ? 0 : (int16_t)(e->delayX - 4);
            if (e->delayY == 0)
                e->slideY = approach(e->slideY, e->slideY0, 3);
            else
                e->delayY = (uint16_t)e->delayY < 4 ? 0 : (int16_t)(e->delayY - 3);
            if (e->slideX == e->slideX0 && e->slideY == e->slideY0 && e->delayX == 0 && e->delayY == 0)
                continue;
        } else {
            continue;
        }
        moving++;
    }
    if (!moving) {
        if (g_guiSlideState == 1) {
            g_guiSlideState = 0;
            guiBroadcast(GUI_MSG_SLID_IN, NULL, 0, 0);
        }
        if (g_guiSlideState == 2)
            g_guiSlideState = 0;
    }

    guiBroadcast(GUI_MSG_FRAME, NULL, 0, 0);
    guiBroadcast(GUI_MSG_DRAW, NULL, 0, 0);
    if (guiHelpActive())
        guiHelpUpdate();
    psx2DRequestDraw();
    if (g_guiGroupLoaded)
        ngappEmptyHook();
    return 0;
}

/* VA 0x004382f0, 111 bytes, [I] row, status draft */
void guiUpdatePulse(void)
{
    /* the "selected" glow: 16 -> 240 -> 16 in steps of 8 */
    if (g_guiPulseHold)
        return;
    if (g_guiPulseStep < 1) {
        g_guiPulse -= 8;
        if ((int32_t)g_guiPulse < 0x10) {
            g_guiPulseStep = -g_guiPulseStep;
            g_guiPulse = 0x10;
        }
    } else {
        g_guiPulse += 8;
        if (g_guiPulse > 0xF0) {
            g_guiPulse = 0xF0;
            g_guiPulseStep = -g_guiPulseStep;
        }
    }
    g_guiPulseHold = 0;
}

/* VA 0x00438380, 6 bytes, [I] row, status draft */
GuiStyle *guiGetStyle(void)
{
    return &g_guiStyle;
}

/* ================================================================== elements */

/* VA 0x004383a0, 43 bytes, [I] row, status draft */
GuiElement *guiFindElement(uint32_t id)
{
    int i;

    for (i = 0; i < g_guiCount; i++)
        if (g_guiList[i]->id == id)
            return g_guiList[i];
    return NULL;
}

/* VA 0x004383e0, 25 bytes, [I] row, status draft */
GuiElement *guiGetElement(uint32_t index)
{
    return index < (uint32_t)g_guiCount ? g_guiList[index] : NULL;
}

/* VA 0x00438410, 27 bytes, [I] row, status draft */
static int guiCompareElements(const void *a, const void *b)
{
    return (int)(*(GuiElement *const *)b)->priority - (int)(*(GuiElement *const *)a)->priority;
}

/* VA 0x00438440, 32 bytes, [I] row, status draft */
void guiSortElements(void)
{
    if (g_guiCount > 1)
        qsort(g_guiList, (size_t)g_guiCount, sizeof(GuiElement *), guiCompareElements);
}

/* VA 0x00438470, 283 bytes, [I] row, status draft */
GuiElement *guiCreateElement(uint16_t type, uint16_t priority, uint32_t id)
{
    GuiElement *e;
    void *data;
    int i;

    for (i = 0; i < GUI_MAX_ELEMENTS; i++) {
        e = &g_guiElements[i];
        if (e->type != 0)
            continue;
        memset(e, 0, sizeof(*e));
        e->type = type;
        e->priority = priority;
        e->id = id;
        switch (type) {
        case GUI_TYPE_SPRITE:
            e->typeHandler = guiElementHandler;
            break;
        case GUI_TYPE_BUTTON:
            e->typeHandler = guiButtonHandler;
            break;
        case GUI_TYPE_TOGGLE:
            e->typeHandler = guiToggleHandler;
            e->data = zmemDMMAlloc(sizeof(GuiToggleData));
            data = zmemDMMLock(e->data);
            memset(data, 0, sizeof(GuiToggleData));
            zmemDMMUnlock(e->data);
            break;
        case GUI_TYPE_SPRITE5:
            e->typeHandler = guiElementHandler;
            e->animTimer = 0;
            e->frame = 0;
            break;
        case GUI_TYPE_NULL:
            e->typeHandler = (GuiHandler)guiNullHandler;
            break;
        case GUI_TYPE_LINE:
            e->typeHandler = guiLineHandler;
            break;
        case GUI_TYPE_BOX:
            e->typeHandler = guiBoxHandler;
            break;
        }
        g_guiList[g_guiCount++] = e;
        guiSortElements();
        guiSendMessage(GUI_MSG_CREATE, e, 0, 0);
        return e;
    }
    g_pfnDebugPrintf("GUIMAIN: Error - failed to allocate element\n");
    return NULL;
}

/* VA 0x00438600, 257 bytes, [I] row, status draft */
void guiDestroyElement(GuiElement *e)
{
    GuiToggleData *d;
    int i;

    if (e == NULL)
        return;
    guiSendMessage(GUI_MSG_DESTROY, e, 0, 0);
    if (e->text != 0) {
        zmemDMMDealloc(e->text);
        e->text = 0;
        e->textSize = 0;
    }
    if (e->data != 0) {
        if (e->type == GUI_TYPE_TOGGLE) {
            d = (GuiToggleData *)zmemDMMLock(e->data);
            if (d->options != 0) {
                zmemDMMDealloc(d->options);
                d->options = 0;
            }
            if (d->text != 0) {
                zmemDMMDealloc(d->text);
                d->text = 0;
            }
            zmemDMMUnlock(e->data);
        }
        zmemDMMDealloc(e->data);
        e->data = 0;
    }
    memset(e, 0, sizeof(*e));
    for (i = g_guiCount - 1; i >= 0; i--) {
        if (g_guiList[i] == e) {
            g_guiCount--;
            g_guiList[i] = g_guiList[g_guiCount];
            e->type = 0;
            guiSortElements();
            return;
        }
    }
    g_pfnDebugPrintf("GUIMAIN: Error - freeing element not in list\n");
}

/* VA 0x00438760, 14 bytes, [I] row, status draft */
void guiSetFlags(GuiElement *e, uint16_t flags)
{
    e->flags = flags;
}

/* VA 0x00438780, 22 bytes, [I] row, status draft */
void guiSetLink(GuiElement *e, uint32_t dir, uint8_t id)
{
    e->link[dir & 0xFF] = id;
}

/* VA 0x004387b0, 46 bytes, [I] row, status draft */
void guiSetFlagsById(uint32_t id, uint16_t mask, int16_t on)
{
    GuiElement *e = guiFindElement(id);

    if (e == NULL)
        return;
    if (on == 1)
        e->flags |= mask;
    else
        e->flags &= (uint16_t)~mask;
}

/* VA 0x004387f0, 23 bytes, [I] row, status draft */
void guiSetPos(GuiElement *e, int16_t x, int16_t y)
{
    e->x = x;
    e->y = y;
}

/* VA 0x00438820, 35 bytes, [I] row, status draft */
void guiGetPos(GuiElement *e, int16_t *x, int16_t *y)
{
    if (x != NULL)
        *x = e->x;
    if (y != NULL)
        *y = e->y;
}

/* VA 0x00438860, 14 bytes, [I] row, status draft */
void guiSetPriority(GuiElement *e, uint16_t priority)
{
    e->priority = priority;         /* the list is not re-sorted */
}

/* VA 0x00438880, 20 bytes, [I] row, status draft */
void guiGetPriority(GuiElement *e, uint16_t *priority)
{
    if (priority != NULL)
        *priority = e->priority;
}

/* VA 0x004388b0, 64 bytes, [I] row, status draft */
void guiSetScale(GuiElement *e, float x, float y)
{
    e->scaleX = x;
    e->scaleY = y;
    if (x == 1.0f && y == 1.0f)
        e->flags &= ~GUIF_SCALED;
    else
        e->flags |= GUIF_SCALED;
}

/* VA 0x00438900, 31 bytes, [I] row, status draft */
void guiGetScale(GuiElement *e, float *x, float *y)
{
    if (x != NULL)
        *x = e->scaleX;
    if (y != NULL)
        *y = e->scaleY;
}

/* VA 0x00438930, 23 bytes, [I] row, status draft */
void guiSetSize(GuiElement *e, int16_t w, int16_t h)
{
    e->w = w;
    e->h = h;
}

/* VA 0x00438960, 23 bytes, [I] row, status draft */
void guiSetSlide(GuiElement *e, int16_t x, int16_t y)
{
    e->slideX0 = x;
    e->slideY0 = y;
}

/* VA 0x00438990, 14 bytes, [I] row, status draft */
void guiSetSpriteFlags(GuiElement *e, uint16_t flags)
{
    e->spriteFlags = flags;
}

/* VA 0x004389b0, 87 bytes, [I] row, status draft */
void guiSetAnimBytes(GuiElement *e, uint16_t count, const uint8_t *pairs)
{
    /* (frame, time) byte pairs */
    uint32_t i;

    for (i = 0; i < count; i++) {
        e->animFrames[i] = pairs[2 * i];
        e->animTimes[i] = pairs[2 * i + 1];
    }
    e->animCount = count;
    e->animIndex = 0;
}

/* VA 0x00438a30, 23 bytes, [I] row, status draft */
void guiSetSpriteOffset(GuiElement *e, int16_t x, int16_t y)
{
    e->spriteX = x;
    e->spriteY = y;
}

/* VA 0x00438a60, 50 bytes, [I] row, status draft */
void guiLoadSprite(GuiElement *e, uint32_t id)
{
    ZprimAsset *a = zprimLoadAsset(id);

    if (a == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load sprite %04.4X\n", id & 0xFFFF);
        return;
    }
    e->sprite = a;
}

/* VA 0x00438ab0, 12 bytes, [I] row, status draft */
void guiSetSprite(GuiElement *e, ZprimAsset *sprite)
{
    e->sprite = sprite;
}

/* VA 0x00438ad0, 50 bytes, [I] row, status draft */
void guiLoadSelSprite(GuiElement *e, uint32_t id)
{
    ZprimAsset *a = zprimLoadAsset(id);

    if (a == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load selected sprite %04.4X\n", id & 0xFFFF);
        return;
    }
    e->selSprite = a;
}

/* VA 0x00438b20, 14 bytes, [I] row, status draft */
void guiSetFrame(GuiElement *e, uint16_t frame)
{
    e->frame = frame;
}

/* VA 0x00438b40, 14 bytes, [I] row, status draft */
void guiSetAnimIndex(GuiElement *e, int16_t index)
{
    e->animIndex = index;
}

/* VA 0x00438b60, 14 bytes, [I] row, status draft */
void guiSetAnimCount(GuiElement *e, uint16_t count)
{
    e->animCount = count;
}

/* VA 0x00438b80, 17 bytes, [I] row, status draft */
void guiSetPalette(GuiElement *e, uint32_t palette)
{
    e->palette = palette & 0xFFFF;
}

/* VA 0x00438bb0, 8 bytes, [I] row, status draft */
uint32_t guiGetPalette(GuiElement *e)
{
    return e->palette;
}

/* VA 0x00438bd0, 14 bytes, [I] row, status draft */
void guiSetSpriteParam(GuiElement *e, uint16_t param)
{
    e->spriteParam = param;
}

/* VA 0x00438bf0, 81 bytes, [I] row, status draft */
void guiSetAnim(GuiElement *e, uint32_t count, const uint16_t *frames, const uint16_t *times)
{
    uint32_t i;

    count &= 0xFFFF;
    for (i = 0; i < count; i++) {
        e->animFrames[i] = frames[i];
        e->animTimes[i] = times[i];
    }
    e->animIndex = 0;
}

/* VA 0x00438c70, 17 bytes, [I] row, status draft */
void guiSetTextFlags(GuiElement *e, uint16_t flags)
{
    e->textFlags = flags;
}

/* VA 0x00438ca0, 35 bytes, [I] row, status draft */
void guiSetTextRGB(GuiElement *e, uint8_t r, uint8_t g, uint8_t b)
{
    e->rgb[0] = r;
    e->rgb[1] = g;
    e->rgb[2] = b;
}

/* VA 0x00438ce0, 29 bytes, [I] row, status draft */
void guiSetTextPos(GuiElement *e, int16_t x, int16_t y)
{
    e->textX = x;
    e->textY = y;
}

/* VA 0x00438d10, 20 bytes, [I] row, status draft */
void guiSetTextPalette(GuiElement *e, uint32_t palette)
{
    e->textPalette = palette & 0xFFFF;
}

/* VA 0x00438d40, 11 bytes, [I] row, status draft */
uint32_t guiGetTextPalette(GuiElement *e)
{
    return e->textPalette;
}

/* VA 0x00438d60, 53 bytes, [I] row, status draft */
void guiLoadFont(GuiElement *e, uint32_t id)
{
    ZprimAsset *a = zprimLoadAsset(id);

    if (a == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load font %04.4X\n", id & 0xFFFF);
        return;
    }
    e->font = a;
}

/* VA 0x00438db0, 540 bytes, [I] row, status draft */
void guiSetText(GuiElement *e, const char *text)
{
    /* The text lives in a DMM block that only grows. Unchanged text costs nothing (and sends no
     * GUI_MSG_TEXT); "" on an existing block just truncates it (and sets textSize to 1). */
    const char *s;
    char *buf;
    uint32_t len;
    int same;

    if (e == NULL || text == NULL || (s = guiTextLookup(text)) == NULL)
        return;
    if (e->text != 0) {
        buf = (char *)zmemDMMLock(e->text);
        same = strcmp(s, buf) == 0;
        zmemDMMUnlock(e->text);
        if (same)
            return;
    }
    len = (uint32_t)strlen(s) + 1;
    if (len == 1 && e->text != 0) {
        buf = (char *)zmemDMMLock(e->text);
        buf[0] = g_overlayEmpty[0];
        zmemDMMUnlock(e->text);
        e->textSize = 1;
        return;
    }
    if (e->text != 0) {
        buf = (char *)zmemDMMLock(e->text);
        if (e->textSize < len) {
            zmemDMMUnlock(e->text);
            zmemDMMDealloc(e->text);
            e->text = 0;
            e->text = zmemDMMAlloc(len);
            e->textSize = len;
            buf = (char *)zmemDMMLock(e->text);
        }
    } else {
        e->text = zmemDMMAlloc(len);
        e->textSize = len;
        buf = (char *)zmemDMMLock(e->text);
    }
    memcpy(buf, s, len);
    zmemDMMUnlock(e->text);
    guiSendMessage(GUI_MSG_TEXT, e, (uint32_t)s, 0);
}

/* VA 0x00439060, 17 bytes, [I] row, status draft */
void guiSetSpacing(GuiElement *e, int16_t spacing)
{
    e->spacing = spacing;
}

/* VA 0x00439090, 12 bytes, [I] row, status draft */
int16_t guiGetSpacing(GuiElement *e)
{
    return e->spacing;
}

/* VA 0x004390b0, 30 bytes, [I] row, status draft */
void guiSetTextFixed(GuiElement *e, int on)
{
    if (on)
        e->textFlags |= GUIT_FIXED;
    else
        e->textFlags &= ~GUIT_FIXED;
}

/* VA 0x004390e0, 54 bytes, [I] row, status draft */
int16_t guiGetTextWidth(GuiElement *e)
{
    const uint8_t *s = (const uint8_t *)zmemDMMLock(e->text);
    int16_t w = zprimGetTextWidth(e->font, s);

    zmemDMMUnlock(e->text);
    return w;
}

/* VA 0x00439130, 109 bytes, [I] row, status draft */
void guiDrawSpriteToBackdrop(uint32_t sprite, uint32_t backdrop, uint32_t a, uint32_t b)
{
    /* not called; the zprim routine it ends in is empty on PC */
    ZprimAsset *s = zprimLoadAsset(sprite), *p;

    if (s == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load sprite %04.4X\n", sprite & 0xFFFF);
        return;
    }
    p = zprimLoadAsset(backdrop);
    if (p == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load backdrop %04.4X\n", backdrop & 0xFFFF);
        return;
    }
    zprimCopyToPicture(s, p, a, b);
}

/* VA 0x004391c0, 140 bytes, [I] row, status draft */
void guiLoadBackdropImage(uint16_t screen, uint16_t image)
{
    /* a picture of screen group 0xE000 | screen becomes the backdrop (a RenderWare material) */
    ZprimAsset *a = zprimLoadGroupImage((uint32_t)((screen & 0xFFF) | 0xE000), image);
    ZprimImage *pic;

    if (g_guiStyle.backdrop != NULL)
        zprimFreeAsset(g_guiStyle.backdrop);
    g_guiStyle.backdrop = a;
    pic = (ZprimImage *)zprimLockAsset(a);
    if (pic->frame[0].material != 0)
        guiFreeMaterial(pic->frame[0].material);
    pic->frame[0].material = zprimMakeMaterial(pic->width, pic->height, pic->bpp,
                                               (const uint8_t *)pic + pic->pixels,
                                               (const ZprimPalette *)((const uint8_t *)pic + pic->palette));
    zprimUnlockAsset(a);
    zmemDMMOptimize();
}

/* VA 0x00439280, 13 bytes, [I] row, status draft */
void guiFreeMaterial(int32_t material)
{
    zrwMatRelease(material);
}

/* VA 0x004392a0, 48 bytes, [I] row, status draft */
void guiLoadBackdrop(uint32_t id)
{
    ZprimAsset *a = zprimLoadAsset(id);

    if (a == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - failed to load backdrop %04.4X\n", id & 0xFFFF);
        return;
    }
    g_guiStyle.backdrop = a;
}

/* VA 0x004392e0, 12 bytes, [I] row, status draft */
void guiSetFocusFont(uint16_t font)
{
    g_guiStyle.focusFont = font;
}

/* VA 0x00439300, 12 bytes, [I] row, status draft */
void guiSetBlurFont(uint16_t font)
{
    g_guiStyle.blurFont = font;
}

/* VA 0x00439320, 30 bytes, [I] row, status draft */
void guiSetFocusRGB(uint8_t r, uint8_t g, uint8_t b)
{
    g_guiStyle.focusRGB[0] = r;
    g_guiStyle.focusRGB[1] = g;
    g_guiStyle.focusRGB[2] = b;
}

/* VA 0x00439350, 30 bytes, [I] row, status draft */
void guiSetBlurRGB(uint8_t r, uint8_t g, uint8_t b)
{
    g_guiStyle.blurRGB[0] = r;
    g_guiStyle.blurRGB[1] = g;
    g_guiStyle.blurRGB[2] = b;
}

/* VA 0x00439380, 7 bytes, [I] row, status draft */
uint16_t guiGetFocusFont(void)
{
    return g_guiStyle.focusFont;
}

/* VA 0x004393a0, 7 bytes, [I] row, status draft */
uint16_t guiGetBlurFont(void)
{
    return g_guiStyle.blurFont;
}

/* VA 0x004393c0, 49 bytes, [I] row, status draft */
void guiGetFocusRGB(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r != NULL)
        *r = g_guiStyle.focusRGB[0];
    if (g != NULL)
        *g = g_guiStyle.focusRGB[1];
    if (b != NULL)
        *b = g_guiStyle.focusRGB[2];
}

/* VA 0x00439410, 49 bytes, [I] row, status draft */
void guiGetBlurRGB(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r != NULL)
        *r = g_guiStyle.blurRGB[0];
    if (g != NULL)
        *g = g_guiStyle.blurRGB[1];
    if (b != NULL)
        *b = g_guiStyle.blurRGB[2];
}

/* ================================================================== messages */

/* VA 0x00439460, 95 bytes, [I] row, status draft */
int guiSendMessage(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    GuiHandler h;

    if (e == NULL) {
        g_pfnDebugPrintf("GUIMAIN: Error - sending message %d to NULL element\n", msg);
        return 0;
    }
    if (e->flags & GUIF_HIDDEN)
        return 0;
    h = e->handler != NULL ? e->handler : e->typeHandler;
    if (h != NULL)
        return h(msg, e, a, b);
    g_pfnDebugPrintf("POST MESSAGE: No type handler\n");
    return 0;
}

/* VA 0x004394e0, 45 bytes, [I] row, status draft */
int guiSendMessageById(uint32_t msg, uint32_t id, uint32_t a, uint32_t b)
{
    GuiElement *e = guiFindElement(id);

    if (e == NULL)
        return 0;
    return guiSendMessage(msg, e, a, b);
}

/* VA 0x00439520, 138 bytes, [I] row, status draft */
int guiBroadcast(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* the screen handler first (e = the element the message is about), then every element,
     * until one handles it */
    int handled, i;

    if (g_guiStyle.handler == NULL)
        handled = guiDefaultScreenHandler(msg, e, a, b);
    else
        handled = g_guiStyle.handler(msg, e, a, b);
    if (handled)
        return 1;
    for (i = 0; i < g_guiCount; i++)
        if (guiSendMessage(msg, g_guiList[i], a, b))
            return 1;
    return 0;
}

/* VA 0x004395e0, 154 bytes, [I] row, status draft */
int guiPostInput(uint32_t msg, uint32_t a, uint32_t b)
{
    /* an input message: for each visible focused element (screen handler, then the element),
     * else once with no element */
    GuiElement *e;
    int i;

    for (i = 0; i < g_guiCount; i++) {
        e = g_guiList[i];
        if (!(e->flags & GUIF_HIDDEN) && (e->flags & GUIF_FOCUS)) {
            if (guiBroadcast(msg, e, a, b))
                return 1;
            if (guiSendMessage(msg, e, a, b))
                return 1;
        }
    }
    guiBroadcast(msg, NULL, a, b);
    return 0;
}

/* VA 0x004396b0, 159 bytes, [I] row, status draft */
int guiDefaultScreenHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int i;

    if (msg == GUI_MSG_DRAW) {
        for (i = 0; i < g_guiCount; i++)
            if (g_guiList[i]->type != 0)
                guiSendMessage(GUI_MSG_DRAW_ELEM, g_guiList[i], a, b);
    } else if (msg == GUI_MSG_FRAME) {
        for (i = 0; i < g_guiCount; i++)
            if (g_guiList[i]->type != 0)
                guiSendMessage(GUI_MSG_UPDATE, g_guiList[i], a, b);
    } else {
        return 0;
    }
    return 1;
}

/* VA 0x00439780, 33 bytes, [I] row, status draft */
void guiFocus(uint32_t id)
{
    GuiElement *e = guiFindElement(id);

    if (e != NULL)
        guiSendMessage(GUI_MSG_FOCUS, e, 0, 0);
}

/* VA 0x004397c0, 11 bytes, [I] row, status draft */
void guiShow(GuiElement *e)
{
    e->flags &= ~GUIF_HIDDEN;
}

/* VA 0x004397e0, 9 bytes, [I] row, status draft */
void guiHide(GuiElement *e)
{
    e->flags |= GUIF_HIDDEN;
}

/* VA 0x00439800, 18 bytes, [I] row, status draft */
int guiIsVisible(GuiElement *e)
{
    return ~e->flags & GUIF_HIDDEN;
}

/* VA 0x00439830, 24 bytes, [I] row, status draft */
void guiSetPulse(GuiElement *e, int on)
{
    if (on)
        e->flags |= GUIF_PULSE;
    else
        e->flags &= ~GUIF_PULSE;
}

/* VA 0x00439860, 55 bytes, [I] row, status draft */
void guiSetHighlight(GuiElement *e, uint8_t r, uint8_t g, uint8_t b, uint32_t alpha)
{
    if (alpha == 0) {
        e->flags &= ~GUIF_HIGHLIGHT;
        return;
    }
    e->alpha = alpha;
    e->flags |= GUIF_HIGHLIGHT;
    e->highlight = (uint32_t)b << 16 | (uint32_t)g << 8 | r;
}

/* VA 0x004398b0, 5 bytes, [I] row, status draft */
const char *guiTextLookup(const char *text)
{
    return text;            /* identity on PC (a text-id / macro hook) */
}

/* ================================================================== box element (type 8) */

/* VA 0x004398d0, 157 bytes, [I] row, status draft */
int guiBoxHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    int16_t x0, y0, x1, y1;

    if (msg != GUI_MSG_DRAW_ELEM)
        return 0;
    x0 = e->x;
    y0 = e->y;
    x1 = e->spriteX;
    y1 = e->spriteY;
    zprimSetRGB(e->rgb[0], e->rgb[1], e->rgb[2], ZPRIM_ATTR_COLOUR);
    zprimDrawLine(x0, y0, x1, y0);
    zprimDrawLine(x1, y0, x1, y1);
    zprimDrawLine(x1, y1, x0, y1);
    zprimDrawLine(x0, y1, x0, y0);
    zprimDrawLine(x0 + 1, y0 + 1, x1 - 1, y0 + 1);
    zprimDrawLine(x1 - 1, y0 + 1, x1 - 1, y1 - 1);
    zprimDrawLine(x1 - 1, y1 - 1, x0 + 1, y1 - 1);
    zprimDrawLine(x0 + 1, y1 - 1, x0 + 1, y0 + 1);
    return 1;
}

/* VA 0x004399a0, 23 bytes, [I] row, status draft */
void guiBoxSetStart(GuiElement *e, int16_t x, int16_t y)
{
    e->x = x;
    e->y = y;
}

/* VA 0x004399d0, 23 bytes, [I] row, status draft */
void guiBoxSetEnd(GuiElement *e, int16_t x, int16_t y)
{
    e->spriteX = x;
    e->spriteY = y;
}

/*
 * NgEngine/Zge/zprofile.c
 * Original: C:\NgEngine\Zge\zprofile.c
 * Evidence: [V] path string referenced by zprofileInit and SetLayout / CreateBar; the
 * others [I]: the globals 0x00676438-0x0067649F (ZprofileGlobals) and the callbacks Init/Create*
 * register. The ZGE object pass created the draw callback 0x004A35F0 (between zrwRamDisk.c and zprofileInit)
 * and the twelve functions after zprofileInit.
 * 15 functions, VA 0x004a35f0-0x004a4225 (the clean-up created zprofileResetPeak 0x004A4190).
 * Draft. Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 7.
 *
 * On-screen CPU bars, timed with the performance counter (pcProfile.c): each bar accumulates the
 * ticks between Start and Stop during a frame; the draw callback (ZGL priority 4) draws, per bar,
 * the peak (grey), the current value (the bar's colour) and tick marks every 1 / 0.1 / 0.01 / 0.001
 * frame (white / red / green / blue), the full width being `scale` frames of 1/60 s. "App" spans
 * the frame callbacks 7..8, "EngCPU" starts at callback 0. Nothing in the shipped game creates a
 * bar [V, no caller of the Create functions], so the profiler only registers its draw callback.
 */
#include "decomp.h"
#include "ztools.h"
#include "ngif.h"

#define ZPROFILE_FILE "C:\\NgEngine\\Zge\\zprofile.c"

extern uint32_t g_zglScreenWidth;                                         /* 0x00697770 (zrwZgl.c) */

static const uint32_t s_zprofilePeakColour   = 0x80808080u;   /* 0x00563904 grey */
static const uint32_t s_zprofileTickColour[4] = {             /* 0x00563908 */
    0x80FFFFFFu, 0x800000FFu, 0x8000FF00u, 0x80FF0000u        /* white 1, red 0.1, green 0.01, blue */
};
static const uint32_t s_zprofileAppColour    = 0x8000FF00u;   /* 0x00563918 green */
static const uint32_t s_zprofileEngineColour = 0x800000FFu;   /* 0x0056391C red */

static ZprofileGlobals g_zprofile;                            /* 0x00676438 */

/* A screen-space rectangle as a ZGL quad. */
static void zprofileQuad(float x0, float y0, float x1, float y1, uint32_t colour)
{
    ZglVertex v[4];
    ZglQuad q;
    int i;

    zstringMemSet(v, 0, sizeof(v));
    v[0].pos.x = x0; v[0].pos.y = y0;
    v[1].pos.x = x0; v[1].pos.y = y1;
    v[2].pos.x = x1; v[2].pos.y = y1;
    v[3].pos.x = x1; v[3].pos.y = y0;
    q.material = 0;
    for (i = 0; i < 4; i++) {
        *(uint32_t *)&v[i].colour = colour;
        q.v[i] = &v[i];
    }
    g_zglDrawQuads(ZGL_PRIM_2D, &q, 1, 0, 0);
}

/* VA 0x004a35f0, 1384 bytes, [I] row, status draft */
int zprofileDraw(void *arg)
{
    ZprofileLayout *L = &g_zprofile.layout;
    float x = g_zprofile.x, y = g_zprofile.y;
    float width = (float)(g_zglScreenWidth - L->margin * 2) - g_zprofile.x;
    ZmemFreeNode *node;

    for (node = zmemFreeListFirst((ZmemFreeList *)g_zprofile.pool); node; node = zmemFreeListNext(node)) {
        ZprofileBar *bar = (ZprofileBar *)zmemFreeListNodeData(node);
        float len, step, t, yFill;

        if (bar->flags & 1)
            zprofileBarStop(bar);               /* inlined: a bar still running at draw time */
        if (g_zprofile.peakDelay == 0) {
            len = pcProfileTicksToUnits(bar->peak, (float)L->fps) * width / bar->scale;
            zprofileQuad(x, y, x + len, y + L->barHeight, s_zprofilePeakColour);
        }
        yFill = y + (float)(L->barHeight - L->fillHeight) * 0.5f;   /* (abs) */
        if (yFill < y)
            yFill = y + (y - yFill);
        len = pcProfileTicksToUnits(bar->ticks, (float)L->fps) * width / bar->scale;
        zprofileQuad(x, yFill, x + len, yFill + L->fillHeight, bar->colour);

        /* tick marks across the bar, finer for small scales */
        step = bar->scale >= 1.0f ? 1.0f : bar->scale > 0.1f ? 0.1f : bar->scale > 0.01f ? 0.01f : 0.001f;
        for (t = 0.0f; t <= bar->scale; t += step) {
            ZglVertex v[3];
            ZglTri tri;
            float px = x + t * width / bar->scale;
            uint32_t colour;
            int k;

            if (t - (int)t < 5e-05f)
                colour = s_zprofileTickColour[0];
            else if (t * 10.0f - (int)(t * 10.0f) < 5e-05f)
                colour = s_zprofileTickColour[1];
            else if (t * 100.0f - (int)(t * 100.0f) < 5e-05f)
                colour = s_zprofileTickColour[2];
            else
                colour = s_zprofileTickColour[3];
            zstringMemSet(v, 0, sizeof(v));
            v[0].pos.x = px + L->tickWidth / 2;  v[0].pos.y = y - L->tickLength;
            v[1].pos.x = px - L->tickWidth / 2;  v[1].pos.y = y - L->tickLength;
            v[2].pos.x = px;                     v[2].pos.y = y - L->tickLength + L->tickHeight;
            tri.material = 0;
            for (k = 0; k < 3; k++) {
                *(uint32_t *)&v[k].colour = colour;
                tri.v[k] = &v[k];
            }
            g_zglDrawTris(ZGL_PRIM_2D, &tri, 1, 0, 0);
        }
        if (bar->name)
            g_pfnDebugPrintAt(0, (int16_t)((int)y - 3), bar->name);   /* [I] the label left of the bar */
        bar->ticks = 0;
        zstringMemSet(bar->start, 0, sizeof(bar->start));
        y += (float)(L->barGap + L->barHeight);
    }
    if (g_zprofile.peakDelay)
        g_zprofile.peakDelay--;
    if (g_zprofile.peakPeriod && g_zprofile.peakCountdown-- == 0) {
        for (node = zmemFreeListFirst((ZmemFreeList *)g_zprofile.pool); node; node = zmemFreeListNext(node))
            ((ZprofileBar *)zmemFreeListNodeData(node))->peak = 0;
        g_zprofile.peakCountdown = g_zprofile.peakPeriod;
    }
    return 1;
}

/* VA 0x004a3b60, 333 bytes, [V] direct, status draft */
int zprofileInit(void)
{
    ZmemFreeNode *node;

    if (g_zprofile.initialised)
        zAssertFail("Already initialized", ZPROFILE_FILE, 0x141, 0);
    zstringMemSet(&g_zprofile, 0, sizeof(g_zprofile));
    if (!pcProfileInit())
        return 0;
    g_zprofile.layout.margin = 0x20;
    g_zprofile.layout.offsetX = 0;
    g_zprofile.layout.offsetY = 0;
    g_zprofile.layout.barHeight = 8;
    g_zprofile.layout.fillHeight = 6;
    g_zprofile.layout.barGap = 2;
    g_zprofile.layout.tickHeight = 4;
    g_zprofile.layout.tickWidth = 8;
    g_zprofile.layout.tickLength = 4;
    g_zprofile.layout.defaultScale = 2;
    g_zprofile.layout.peakPeriod = 0x78;
    g_zprofile.layout.fps = 0x3C;
    g_zprofile.peakDelay = 10;
    g_zprofile.x = 32.0f;
    g_zprofile.y = 32.0f;
    g_zprofile.defaultScale = 2.0f;
    g_zprofile.initialised = 1;
    g_zprofile.pool = zmemFreeListCreate(sizeof(ZprofileBar), 0, zprofileFreeBar);
    if (g_zprofile.drawCb == 0)
        g_zprofile.drawCb = g_zglAddFrameCallback(4, zprofileDraw, 0, 0);
    zprofileSetPeakPeriod(g_zprofile.layout.peakPeriod);    /* inlined */
    return 1;
}

/* VA 0x004a3cb0, 115 bytes, [I] row, status draft */
int zprofileShutdown(void)
{
    if (g_zprofile.initialised) {
        if (g_zprofile.drawCb)
            g_zglRemoveFrameCallback(g_zprofile.drawCb);
        if (g_zprofile.appStartCb)
            g_zglRemoveFrameCallback(g_zprofile.appStartCb);
        if (g_zprofile.appStopCb)
            g_zglRemoveFrameCallback(g_zprofile.appStopCb);
        if (g_zprofile.engineCb)
            g_zglRemoveFrameCallback(g_zprofile.engineCb);
        zmemFreeListDestroy((ZmemFreeList *)g_zprofile.pool);
        g_zprofile.initialised = 0;
    }
    return 1;
}

/* VA 0x004a3d30, 149 bytes, [I] row, status draft */
void zprofileSetLayout(const ZprofileLayout *layout)
{
    if (layout == 0)
        zAssertFail("Invalid parameter to zprofileSetLayout().", ZPROFILE_FILE, 0x19F, 0);
    g_zprofile.layout = *layout;
    g_zprofile.x = (float)(g_zprofile.layout.margin + g_zprofile.layout.offsetX);
    g_zprofile.y = (float)(g_zprofile.layout.offsetY + g_zprofile.layout.margin);
    g_zprofile.defaultScale = (float)g_zprofile.layout.defaultScale;
}

static ZprofileBar *zprofileNewBar(int type, uint32_t colour, const char *name)
{
    ZprofileBar *bar = (ZprofileBar *)zmemFreeListAlloc((ZmemFreeList *)g_zprofile.pool);

    if (bar == 0) {
        zAssertFail("Failed to allocate obj\n", ZPROFILE_FILE, 0x1C4, 0);
        return 0;
    }
    if (zmemFreeListGetUsedCount((ZmemFreeList *)g_zprofile.pool) == 1 && g_zprofile.drawCb == 0)
        g_zprofile.drawCb = g_zglAddFrameCallback(4, zprofileDraw, 0, 0);
    bar->type = type;
    bar->colour = colour;
    bar->name = name;
    bar->scale = g_zprofile.defaultScale;
    bar->peak = 0;
    bar->ticks = 0;
    zstringMemSet(bar->start, 0, sizeof(bar->start));
    return bar;
}

/* VA 0x004a3dd0, 206 bytes, [I] row, status draft */
ZprofileBar *zprofileCreateBar(int unused, int type, const uint32_t *colour, const char *name)
{
    if (type < 0 || type > 2)
        zAssertFail("Unknown bar type", ZPROFILE_FILE, 0x1BF, 0);
    return zprofileNewBar(type, *colour, name);
}

/* VA 0x004a3ea0, 254 bytes, [I] row, status draft */
ZprofileBar *zprofileCreateAppBar(void)
{
    if (g_zprofile.appBar)
        return 0;
    g_zprofile.appBar = zprofileNewBar(ZPROFILE_BAR_APP, s_zprofileAppColour, "App");     /* inlined */
    if (g_zprofile.appBar) {
        g_zprofile.appStartCb = g_zglAddFrameCallback(7, (ZglFrameFn)zprofileBarStart, g_zprofile.appBar, 1);
        g_zprofile.appStopCb = g_zglAddFrameCallback(8, (ZglFrameFn)zprofileBarStop, g_zprofile.appBar, 1);
        g_zprofile.appBar->scale = 1.0f;
    }
    return g_zprofile.appBar;
}

/* VA 0x004a3fa0, 215 bytes, [I] row, status draft */
ZprofileBar *zprofileCreateEngineBar(void)
{
    if (g_zprofile.engineBar)
        return 0;
    g_zprofile.engineBar = zprofileNewBar(ZPROFILE_BAR_ENGINE, s_zprofileEngineColour, "EngCPU"); /* inlined */
    if (g_zprofile.engineBar)
        g_zprofile.engineCb = g_zglAddFrameCallback(0, (ZglFrameFn)zprofileBarStart, g_zprofile.engineBar, 1);
    return g_zprofile.engineBar;
}

/* VA 0x004a4080, 21 bytes, [I] row, status draft */
void zprofileFreeBar(void *bar)
{
    zmemFreeListFree((ZmemFreeList *)g_zprofile.pool, bar);
}

/* VA 0x004a40a0, 68 bytes, [I] row, status draft */
void zprofileEnable(int on)
{
    if (!on) {
        if (g_zprofile.drawCb) {
            g_zglRemoveFrameCallback(g_zprofile.drawCb);
            g_zprofile.drawCb = 0;
        }
    } else if (g_zprofile.drawCb == 0) {
        g_zprofile.drawCb = g_zglAddFrameCallback(4, zprofileDraw, 0, 0);
    }
}

/* VA 0x004a40f0, 30 bytes, [I] row, status draft */
int zprofileBarStart(ZprofileBar *bar)
{
    pcProfileStart(bar->start);
    bar->flags |= 1;
    return 1;
}

/* VA 0x004a4110, 59 bytes, [I] row, status draft */
int zprofileBarStop(ZprofileBar *bar)
{
    bar->ticks += pcProfileElapsed(bar->start);
    if (g_zprofile.peakDelay == 0 && bar->ticks > bar->peak)
        bar->peak = bar->ticks;
    bar->flags &= ~1u;
    return 1;
}

/* VA 0x004a4150, 64 bytes, [I] row, status draft */
void zprofileSetPeakPeriod(int frames)
{
    ZmemFreeNode *node;

    g_zprofile.peakPeriod = frames;
    if (frames == 0) {
        for (node = zmemFreeListFirst((ZmemFreeList *)g_zprofile.pool); node; node = zmemFreeListNext(node))
            ((ZprofileBar *)zmemFreeListNodeData(node))->peak = 0;
    }
}

/* VA 0x004a4190, 68 bytes, [I] row, status draft */
void zprofileResetPeak(ZprofileBar *bar)
{
    /* (new function; no caller) Clears the peak of one bar, or of every bar when bar is NULL. */
    ZmemFreeNode *node;

    if (bar != NULL) {
        bar->peak = 0;
        return;
    }
    for (node = zmemFreeListFirst((ZmemFreeList *)g_zprofile.pool); node; node = zmemFreeListNext(node))
        ((ZprofileBar *)zmemFreeListNodeData(node))->peak = 0;
}

/* VA 0x004a41e0, 12 bytes, [I] row, status draft */
void zprofileBarSetScale(ZprofileBar *bar, float frames)
{
    bar->scale = frames;
}

/* VA 0x004a41f0, 54 bytes, [I] row, status draft */
void zprofileSetPosition(int x, int y)
{
    g_zprofile.x = (float)x + (float)g_zprofile.layout.margin;
    g_zprofile.y = (float)y + (float)g_zprofile.layout.margin;
}

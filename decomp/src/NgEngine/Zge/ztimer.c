/*
 * NgEngine/Zge/ztimer.c
 * Original: unknown (no path string; name invented in the original style from the "ZTIMER assert
 * failed" message)
 * Evidence: [I] new file (DM-061): the literals 0x005639A0-0x00563A0F lie between zprofile.c's and
 * pcProfile.c's, the functions share the globals 0x006764A0-0x006764C8, and zgeInit / zgeShutdown
 * call Init / Shutdown between zsceneInit and zprofileInit.
 * 9 functions, VA 0x004a4230-0x004a45d9.
 * DM-061 (draft). Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 7.
 *
 * Timed callbacks: events (a pool of maxEvents, 0x40 from zgeInit) wait in a min-heap keyed by
 * their due time (timeGetTime ms); the frame callback (priority 8) fires the due ones.
 * [V] It never works: every Add / Create function tests the free-list head *before* calling
 * ztimerAlloc, which allocates the pool lazily, so they all return 0. Nothing calls them anyway.
 * [V] A fired event is put back on the free list without ever having been taken off it.
 */
#include "decomp.h"
#include "ztools.h"
#include "ngif.h"

static uint16_t     g_ztimerMaxEvents;   /* 0x006764A0 */
static Zheap        g_ztimerQueue;       /* 0x006764A4 min-heap of (due, event) */
static zList        g_ztimerFree;        /* 0x006764B4 ZtimerEvent.node */
static int          g_ztimerFrameCb;     /* 0x006764C0 */
static int          g_ztimerInitialised; /* 0x006764C4 */
static int          g_ztimerAllocated;   /* 0x006764C8 */

/* VA 0x004a4230, 73 bytes, [I] row, status draft */
int ztimerInit(int maxEvents)
{
    g_ztimerInitialised = 1;
    g_ztimerMaxEvents = (uint16_t)maxEvents;
    g_ztimerFrameCb = g_zglAddFrameCallback(8, ztimerUpdate, 0, 0);
    if (g_ztimerFrameCb == 0) {
        g_pfnDebugPrintf("ZTIMER assert failed : unable to register update function\n");
        return 0;
    }
    return 1;
}

/* VA 0x004a4280, 179 bytes, [I] row, status draft */
int ztimerUpdate(void *arg)
{
    uint32_t now = g_zglGetTime();
    ZheapItem *top;
    ZtimerEvent *ev;

    while ((top = zheapPeek(&g_ztimerQueue)) != 0) {
        ev = (ZtimerEvent *)top->value;
        if (now < ev->due)
            break;
        zheapPop(&g_ztimerQueue);
        switch (ev->type) {
        case ZTIMER_CALLBACK:
            ev->fn(now, ev->arg1, ev->arg2);
            break;
        case ZTIMER_EVENT:
            g_pfnDebugPrintf("Event type timer for obj %d\n", ev->fn);
            break;
        case ZTIMER_SCRIPT:
            g_pfnDebugPrintf("Script type timer\n");
            break;
        default:
            return 0;
        }
        zListAddTail(&g_ztimerFree, &ev->node);
    }
    return 1;
}

/* VA 0x004a4340, 151 bytes, [I] row, status draft */
int ztimerShutdown(void)
{
    ZheapItem *item;
    zListNode *node;

    if (!g_ztimerInitialised)
        return 0;
    g_zglRemoveFrameCallback(g_ztimerFrameCb);
    if (g_ztimerAllocated) {
        while ((item = zheapPop(&g_ztimerQueue)) != 0)
            zListAddTail(&g_ztimerFree, (zListNode *)item->value);
        zheapFree(&g_ztimerQueue);
        while ((node = g_ztimerFree.head) != 0) {
            zListRemove(&g_ztimerFree, node);
            zmemFree(node);
        }
    }
    return 1;
}

static ZtimerEvent *ztimerQueue(uint32_t ms, int type, ZtimerFn fn, void *arg1, void *arg2, int insert)
{
    uint32_t now = g_zglGetTime();
    ZtimerEvent *ev = (ZtimerEvent *)g_ztimerFree.head;   /* (not unlinked) */

    if (ev == 0)
        return 0;                       /* always, see the header */
    ztimerAlloc();
    ev->interval = ms;
    ev->due = now + ms;
    ev->type = type;
    ev->arg1 = arg1;
    ev->arg2 = arg2;
    ev->fn = fn;
    if (insert)
        zheapInsert(now + ms, ev, &g_ztimerQueue);
    return ev;
}

/* VA 0x004a43e0, 93 bytes, [I] row, status draft */
int ztimerAddCallback(uint32_t ms, ZtimerFn fn, void *arg1, void *arg2)
{
    return ztimerQueue(ms, ZTIMER_CALLBACK, fn, arg1, arg2, 1) != 0;
}

/* VA 0x004a4440, 107 bytes, [I] row, status draft */
void ztimerAlloc(void)
{
    uint32_t i;

    if (g_ztimerAllocated)
        return;
    zheapInit(&g_ztimerQueue, 0, g_ztimerMaxEvents);
    zListInit(&g_ztimerFree);
    for (i = 0; i < g_ztimerMaxEvents; i++)
        zListAddTail(&g_ztimerFree, (zListNode *)zmemAlloc(sizeof(ZtimerEvent)));
    g_ztimerAllocated = 1;
}

/* VA 0x004a44b0, 93 bytes, [I] row, status draft */
int ztimerAddEvent(uint32_t ms, ZtimerFn objId, void *arg1, void *arg2)
{
    return ztimerQueue(ms, ZTIMER_EVENT, objId, arg1, arg2, 1) != 0;
}

/* VA 0x004a4510, 75 bytes, [I] row, status draft */
ZtimerEvent *ztimerCreateCallback(uint32_t ms, ZtimerFn fn, void *arg1, void *arg2)
{
    return ztimerQueue(ms, ZTIMER_CALLBACK, fn, arg1, arg2, 0);   /* polled, not queued */
}

/* VA 0x004a4560, 75 bytes, [I] row, status draft */
ZtimerEvent *ztimerCreateCountdown(uint32_t ms)
{
    return ztimerQueue(ms, ZTIMER_COUNTDOWN, 0, 0, 0, 0);
}

/* VA 0x004a45b0, 45 bytes, [I] row, status draft */
int ztimerPoll(ZtimerEvent *ev)
{
    uint32_t now = g_zglGetTime();

    if (now < ev->due)
        return 0;
    if (ev->type == ZTIMER_CALLBACK)
        ev->fn(now, ev->arg1, ev->arg2);
    return 1;
}

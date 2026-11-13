/*
 * NgEngine/Zge/ZgeMods/zscnpts.c
 * Original: C:\NgEngine\Zge\ZgeMods\zscnpts.c
 * Evidence: [V] path string at 0x00561d64 (asserts in zscnptsLoad); the rest [I]: they share the
 * globals 0x00674000-0x00674017 (right after zScene.c's 0x00673F70-0x00673FFB) and the 0x50-byte
 * point pool. DM-040 had put 0x00491D80-0x00491EE0 in zScene.c; they are this file's (DM-043).
 * 11 functions, VA 0x00491d80-0x004921af.
 * DM-043 (draft). Non-matching C (AD-8). Types: decomp/include/zscnpts.h.
 * Format: docs/formats/LEVEL.md 5.3; Python reference tools/dmbmx/formats/level.py SceneSd (agrees).
 */
#include "decomp.h"

/* other modules (current names until their cards land) */
int      zrwZglGetValue(int what, void *out, int arg);                 /* zgl getter; 1 = world scale */
void    *zrwZglAddCallback(int list, int (__cdecl *fn)(void), void *arg, int on); /* add render callback */
void     zrwZglRemoveCallback(void *task);                                   /* remove render callback */
/* debug draw table of zrwDbg.c (0x00560060..) */
extern void (__cdecl *g_pfnDbgDrawMatrix)(const RwMatrix *m, int a, int b);              /* 0x00560060 */
extern void (__cdecl *g_pfnDbgDrawSphere)(const float *centre, float radius, const RwRGBA *c, int f); /* 0x0056006C */

#define ZSCNPTS_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zscnpts.c"
#define ID_SDPT 0x54504453u   /* 'SDPT' */
#define ID_OBJI 0x494A424Fu   /* 'OBJI' */
#define ZGL_WORLD_SCALE 1

ZscnptsGlobals g_zscnpts;     /* 0x00674000 */

/* VA 0x00491d80, 50 bytes, [I] row, status draft */
int zscnptsReset(void)
{
    g_zscnpts.unk08 = 0;
    g_zscnpts.points = 0;
    if (!g_zscnpts.pool)
        g_zscnpts.pool = zmemFreeListCreate(sizeof(ZscnPoint), 0, (ZmemFreeListCallback)zscnptsFreePoint);
    return 1;
}

/* VA 0x00491dc0, 43 bytes, [I] row, status draft */
int zscnptsFreePoint(ZscnPoint *p)
{
    /* pool element destructor */
    if (!p)
        return 0;
    if (p->obji) {
        zmemFree(p->obji);
        p->obji = 0;
    }
    return 1;
}

/* VA 0x00491df0, 50 bytes, [I] row, status draft */
int zscnptsInit(void)
{
    g_zscnpts.debugTask = 0;
    g_zscnpts.pool = 0;
    g_zscnpts.unk08 = 0;
    g_zscnpts.points = 0;
    g_zscnpts.pool = zmemFreeListCreate(sizeof(ZscnPoint), 0, (ZmemFreeListCallback)zscnptsFreePoint);
    return 1;
}

/* VA 0x00491e30, 84 bytes, [I] row, status draft */
void zscnptsSetDebugDraw(int on)
{
    if (on == 1) {
        if (!g_zscnpts.debugTask)
            g_zscnpts.debugTask = zrwZglAddCallback(2, zscnptsDebugRender, 0, 1);
    } else if (g_zscnpts.debugTask) {
        zrwZglRemoveCallback(g_zscnpts.debugTask);
        g_zscnpts.debugTask = 0;
    }
    g_zscnpts.debugDraw = on;
}

/* VA 0x00491e90, 72 bytes, [I] row, status draft */
int zscnptsShutdown(void)
{
    if (g_zscnpts.debugTask) {
        zrwZglRemoveCallback(g_zscnpts.debugTask);
        g_zscnpts.debugTask = 0;
    }
    g_zscnpts.debugDraw = 0;
    g_zscnpts.unk08 = 0;
    if (g_zscnpts.pool)
        zmemFreeListDestroy(g_zscnpts.pool);
    return 1;
}

/* VA 0x00491ee0, 43 bytes, [I] row, status draft */
void zscnptsUnload(void)
{
    /* called by zsceneClose: free every point, then start an empty list */
    g_zscnpts.unk08 = 0;
    zmemFreeListForEach(g_zscnpts.pool, (ZmemFreeListIterFn)zscnptsFreeOne, 0);
    zscnptsReset();            /* the exe passes debugDraw as an (ignored) argument */
}

/* VA 0x00491f10, 56 bytes, [I] row, status draft */
int zscnptsFreeOne(ZscnPoint *p)
{
    int ok = 0;

    if (p) {
        if (p->obji) {
            zmemFree(p->obji);
            p->obji = 0;
        }
        ok = 1;
    }
    zmemFreeListFree(g_zscnpts.pool, p);
    return ok;
}

/* VA 0x00491f50, 382 bytes, [I] row, status draft */
int zscnptsLoad(ZiffFile *iff)
{
    ZscnSdptRecord rec;
    ZobjiHeader hdr;
    ZscnPoint *p;
    float scale;

    if (!ziffChunkOpen(iff))
        return 1;
    if (!ziffChunkFindFirst(iff, ID_SDPT)) {
        zAssertFail("zscnpts : Point has no data!!!", ZSCNPTS_FILE, 0xE3, 0);
        return 0;
    }
    ziffChunkReadData(iff, &rec, sizeof(rec));
    if (rec.size != sizeof(rec)) {
        zAssertFail("zscnpts : Point data size mismatch", ZSCNPTS_FILE, 0xE7, 0);
        return 0;
    }
    p = (ZscnPoint *)zmemFreeListAlloc(g_zscnpts.pool);
    p->id = rec.id;
    p->type = rec.type;
    /* rec.name is not kept: points are found by (id, type) */
    zrwZglGetValue(ZGL_WORLD_SCALE, &scale, 0);
    /*
     * [V] right and at are negated, up is kept: M' = diag(-1, 1, -1) * M (rows). This is not the
     * F*M*F (F = diag(1, -1, -1)) conjugation props get through zrwModelQuatToRwMatrix: the scene
     * point matrix is only used by the debug draw and as a direction source by the game
     * (ngappCreateLevelLights uses matrix.right as the SpotLight direction when there is no SpotLightT;
     * zrollerGetPlayerStartPos turns it into a heading when there is no PlayerT).
     * The flags/pad words of the RwMatrix are left as the pool left them.
     */
    p->matrix.right.x = -rec.matrix[0];
    p->matrix.right.y = -rec.matrix[1];
    p->matrix.right.z = -rec.matrix[2];
    p->matrix.up.x = rec.matrix[3];
    p->matrix.up.y = rec.matrix[4];
    p->matrix.up.z = rec.matrix[5];
    p->matrix.at.x = -rec.matrix[6];
    p->matrix.at.y = -rec.matrix[7];
    p->matrix.at.z = -rec.matrix[8];
    p->matrix.pos.x = rec.pos[0] * scale;    /* ZGE/100 -> ZGE units; no origin shift */
    p->matrix.pos.y = rec.pos[1] * scale;
    p->obji = 0;
    p->matrix.pos.z = rec.pos[2] * scale;
    if (ziffChunkFindFirst(iff, ID_OBJI)) {
        ziffChunkReadData(iff, &hdr, sizeof(hdr));
        p->obji = (uint8_t *)zmemAlloc(hdr.size);
        ziffChunkReadData(iff, p->obji, hdr.size);
    }
    p->next = g_zscnpts.points;               /* newest first */
    g_zscnpts.points = p;
    ziffChunkClose(iff);
    return 1;
}

/* VA 0x004920d0, 94 bytes, [I] row, status draft */
int zscnptsDebugRender(void)
{
    RwRGBA colour;
    ZscnPoint *p;

    if (g_zscnpts.debugDraw) {
        colour.red = 0xFF; colour.green = 0xFF; colour.blue = 0xFF; colour.alpha = 0x50;
        for (p = g_zscnpts.points; p; p = p->next) {
            g_pfnDbgDrawSphere(&p->matrix.pos.x, 10.0f, &colour, 0);
            g_pfnDbgDrawMatrix(&p->matrix, 0, 0);
        }
    }
    return 1;
}

/* VA 0x00492130, 43 bytes, [I] row, status draft */
ZscnPoint *zscnptsFind(uint32_t id, uint32_t type)
{
    /* id = zHashName(name). Callers: "Player"/"PlayerT" with type = player + 1,
     * "SpotLight"/"SpotLightT" with type 1..3, "Globals" with type 1. */
    ZscnPoint *p;

    for (p = g_zscnpts.points; p; p = p->next) {
        if (p->id == id && p->type == type)
            return p;
    }
    return 0;
}

/* VA 0x00492160, 80 bytes, [I] row, status draft */
int16_t zscnptsCollect(void *list, uint32_t id)
{
    /* Appends a ZscnPointRef for every point with this id (any type) to a zList. */
    ZscnPoint *p = g_zscnpts.points;
    int16_t n = 0;

    zListInit((zList *)list);
    if (!p)
        return 0;
    do {
        if (p->id == id) {
            ZscnPointRef *ref = (ZscnPointRef *)zmemAlloc(sizeof(ZscnPointRef));
            ref->point = p;
            zListAddTail((zList *)list, (zListNode *)ref);
            n++;
        }
        p = p->next;
    } while (p);
    return n;
}

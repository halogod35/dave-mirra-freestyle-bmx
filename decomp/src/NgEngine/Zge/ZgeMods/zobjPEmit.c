/*
 * NgEngine/Zge/ZgeMods/zobjPEmit.c
 * Original: C:\NgEngine\Zge\ZgeMods\zobjPEmit.c
 * Evidence: [V] path string referenced by 1 function(s); the others [I]: slots 2-5 of the emitter
 * vtable 0x00562098 and the global 0x006741B4. DM-061 created the five hole functions.
 * DM-064 moved DM-059's drafts in: vtable slots 0-1 (0x00495A10 update, 0x00495A60 destroy) from
 * gap_zrwCol__zobjPEmit.c, and the draw callback, zobjPEmitCreate and zpemitSpawn
 * (0x00495BD0-0x00495DE5) from gap_zobjPEmit__zrwWorld.c [I]: the vtable, the 0x3C data block.
 * 12 functions, VA 0x00495a10-0x00495de5 (DM-064 also created zpemitSpawnType 0x00495CC0).
 * DM-061 / DM-059 (draft). Non-matching C (AD-8). Types: zobj.h, ngif.h (ZfxType, ZfxSystem).
 * Description: docs/re/zge.md 2.5.
 *
 * An emitter keeps one zfx particle system per zfx type it has spawned (created on first use with
 * 0x100 particles); every frame its update runs each system's type update, a ZGL list-2 callback
 * draws them.
 */
#include "decomp.h"
#include "zobj.h"
#include "ngif.h"
#include "zglrw.h"

#define PEMIT(o) ((ZobjPEmitData *)(o)->data)

#define ZOBJPEMIT_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zobjPEmit.c"

static int g_zobjPEmitInitialised;   /* 0x006741B4 */

const ZobjVtbl g_zobjPEmitVtbl = {    /* 0x00562098 */
    zobjPEmitDestroy, zobjPEmitUpdate, zobjPEmitSetPosition, zobjPEmitSetRotation, zobjPEmitSetScale,
    zobjPEmitSetActive, 0, 0, 0
};

/* VA 0x00495a10, 71 bytes, [I] row, status draft */
int zobjPEmitUpdate(ZObj *obj)
{
    /* vtable slot 1: update every system; DM-059 */
    ZfxSystem *s;

    for (s = (ZfxSystem *)zHeapListFirst(PEMIT(obj)->systems); s != 0;
         s = (ZfxSystem *)zHeapListNext(PEMIT(obj)->systems))
        ((ZfxType *)s->owner)->update(s->owner, s);   /* the result only picks the same list [V] */
    return 1;
}

/* VA 0x00495a60, 99 bytes, [I] row, status draft */
int zobjPEmitDestroy(ZObj *obj)
{
    /* vtable slot 0: destroy; DM-059 */
    ZfxSystem *s;

    for (s = (ZfxSystem *)zHeapListFirst(PEMIT(obj)->systems); s != 0;
         s = (ZfxSystem *)zHeapListNext(PEMIT(obj)->systems))
        ((void (__cdecl *)(void *, ZfxSystem *))((ZfxType *)s->owner)->fn14)(s->owner, s);
    zHeapListDestroy(PEMIT(obj)->systems);
    zrwZglRemoveCallback(PEMIT(obj)->drawCallback);
    if (PEMIT(obj) != 0)
        zmemFree(PEMIT(obj));
    return 1;
}

/* VA 0x00495ad0, 39 bytes, [I] row, status draft */
int zobjPEmitSetPosition(ZObj *obj, const RwV3d *pos)
{
    ZobjPEmitData *d = (ZobjPEmitData *)obj->data;

    if (d == 0)
        return 0;
    d->pos = *pos;
    return 1;
}

/* VA 0x00495b00, 47 bytes, [I] row, status draft */
int zobjPEmitSetRotation(ZObj *obj, const Zquat *rot)
{
    ZobjPEmitData *d = (ZobjPEmitData *)obj->data;

    if (d == 0)
        return 0;
    d->rot = *rot;
    return 1;
}

/* VA 0x00495b30, 39 bytes, [I] row, status draft */
int zobjPEmitSetScale(ZObj *obj, const RwV3d *scale)
{
    ZobjPEmitData *d = (ZobjPEmitData *)obj->data;

    if (d == 0)
        return 0;
    d->scale = *scale;
    return 1;
}

/* VA 0x00495b60, 25 bytes, [I] row, status draft */
int zobjPEmitSetActive(ZObj *obj, int active)
{
    if (obj->data == 0)
        return 0;
    ((ZobjPEmitData *)obj->data)->active = active;
    return 1;
}

/* VA 0x00495b80, 45 bytes, [V] direct, status draft */
int zobjPEmitInit(void)
{
    if (g_zobjPEmitInitialised)
        zAssertFail("Already initialized", ZOBJPEMIT_FILE, 0xD3, 0);
    g_zobjPEmitInitialised = 1;
    return 1;
}

/* VA 0x00495bb0, 25 bytes, [I] row, status draft */
int zobjPEmitShutdown(void)
{
    if (g_zobjPEmitInitialised)
        g_zobjPEmitInitialised = 0;
    return 1;
}

/* VA 0x00495bd0, 58 bytes, [I] row, status draft */
int zobjPEmitDraw(ZObj *obj)
{
    /* ZGL callback: draw every system; DM-059 */
    ZfxSystem *s;

    for (s = (ZfxSystem *)zHeapListFirst(PEMIT(obj)->systems); s != 0;
         s = (ZfxSystem *)zHeapListNext(PEMIT(obj)->systems))
        ((int (__cdecl *)(void *, ZfxSystem *, int))((ZfxType *)s->owner)->draw)(s->owner, s, 0);
    return 1;
}

/* VA 0x00495c10, 162 bytes, [I] row, status draft */
void *zobjPEmitCreate(void)
{
    /* DM-059; the field offsets checked against the disassembly by DM-064: rotation = identity
     * quaternion at +0x18 */
    ZObj *obj = zobjAlloc();
    ZobjPEmitData *d;

    if (obj == 0)
        return 0;
    zobjConstruct(obj, ZOBJ_TYPE_PEMIT);
    obj->vtbl = &g_zobjPEmitVtbl;
    d = (ZobjPEmitData *)zmemAlloc(sizeof(ZobjPEmitData));
    obj->data = d;
    d->systems = zHeapListCreate(4);
    d->drawCallback = zrwZglAddCallback(2, (void *)zobjPEmitDraw, obj, 1);
    d->rot.x = d->rot.y = d->rot.z = 0.0f;
    d->rot.w = 1.0f;
    d->pos.x = d->pos.y = d->pos.z = 0.0f;
    d->scale.x = d->scale.y = d->scale.z = 1.0f;
    d->active = 1;
    return obj;
}

/* VA 0x00495cc0, 130 bytes, [I] row, status draft */
uint32_t zpemitSpawnType(ZObj *emitter, ZfxType *type, uint32_t count, void *arg, const void *params)
{
    /* (DM-064 created; no caller) zpemitSpawn with the zfx type already looked up: spawns `count`
     * particles of `type` into the emitter's system for it (created on first use with 0x100
     * particles). */
    ZfxSystem *s;

    for (s = (ZfxSystem *)zHeapListFirst(PEMIT(emitter)->systems); s != 0;
         s = (ZfxSystem *)zHeapListNext(PEMIT(emitter)->systems))
        if (s->owner == type)
            break;
    if (s == 0) {
        s = type->create(type, 0x100);
        zHeapListAdd(PEMIT(emitter)->systems, s);
        if (s == 0)
            return 0;
    }
    type->spawn(type, emitter, s, count, arg, params);
    return count & 0xFF;
}

/* VA 0x00495d50, 150 bytes, [I] row, status draft */
uint32_t zpemitSpawn(void *emitter, const char *type, uint32_t count, void *arg, const void *params)
{
    /* (DM-059) Spawns `count` particles of the named zfx type; the emitter keeps one system per type
     * (created on first use with 0x100 particles). Returns count & 0xFF, 0 if the type is unknown. */
    ZfxType *t = (ZfxType *)zfxFindType(type);
    ZfxSystem *s;

    if (t == 0)
        return 0;
    for (s = (ZfxSystem *)zHeapListFirst(PEMIT((ZObj *)emitter)->systems); s != 0;
         s = (ZfxSystem *)zHeapListNext(PEMIT((ZObj *)emitter)->systems))
        if (s->owner == t)
            break;
    if (s == 0) {
        s = t->create(t, 0x100);
        zHeapListAdd(PEMIT((ZObj *)emitter)->systems, s);
        if (s == 0)
            return 0;
    }
    t->spawn(t, emitter, s, count, arg, params);
    return count & 0xFF;
}

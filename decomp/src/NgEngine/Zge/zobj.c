/*
 * NgEngine/Zge/zobj.c
 * Original: C:\NgEngine\Zge\zobj.c
 * Evidence: [V] path string referenced by 3 function(s); the others [I] (link order, the shared
 * globals 0x006722D0-0x006722E0, the ZObj layout). The ZGE object pass created 19 functions in the holes of the
 * file (Ghidra had only 11) and moved 0x00486EE0 in from the zprop/zobj gap (it is the hierarchy
 * update that zobjUpdateAll passes to the child iterator).
 * 33 functions, VA 0x00486ee0-0x00487acd.
 * Draft; the clean-up moved zobjPrintCB / zobjDumpAll (0x00487A00 / 0x00487A50, the ZglRw pass's
 * draft) in from gap_zobj__zrwModel.c. Non-matching C (AD-8). Types: zobj.h. Description: docs/re/zge.md 2.
 *
 * The ZGE object: a position + rotation + optional parent link + a vtable that forwards to the
 * renderer. zobjUpdateAll (a ZGL frame callback, priority 8) pushes the transforms of every active
 * root object into the renderer, and through the hierarchy those of the attached children (world =
 * parent matrix * local).
 *
 * The model frame helper 0x004891B0 is in the ZglRw pass's gap file and keeps its current name here.
 * zobjForEachChild 0x004879C0 came over from gap_zobj__zrwModel.c (attributed by link order).
 */
#include "decomp.h"
#include "zobj.h"
#include "ngif.h"

#define ZOBJ_FILE "C:\\NgEngine\\Zge\\zobj.c"

void zobjModelGetPartMatrix(ZObj *model, void *frame, RwMatrix *out, int update);           /* 0x004891B0 a model frame's world matrix (gap_zobjModel__zrwMat.c) */
void zrwModelCreateRunLinks(ZObj *obj);                                            /* 0x0049C570 (zrwModelCreate.c) */

static int           g_zobjInitialised;   /* 0x006722D0 */
static ZmemFreeList *g_zobjPool;          /* 0x006722D4 ZObj, 0x100 per block, destructor zobjDestroy */
static ZmemFreeList *g_zobjLinkPool;      /* 0x006722D8 ZobjLink, 4 per block */
static int           g_zobjDebugCb;       /* 0x006722DC zrwDbg callback handle (zobjPrintStats) */
static int           g_zobjFrameCb;       /* 0x006722E0 ZGL frame callback handle (zobjUpdateAll) */

/* The parent's world matrix: the parent object's own transform, or a frame of the parent's model. */
static void zobjParentMatrix(ZObj *obj, RwMatrix *m)
{
    ZobjLink *link = obj->link;

    if (link->frame == 0) {
        zquatToMatrix(m, &link->parent->rot);
        m->pos = link->parent->pos;
    } else {
        zobjModelGetPartMatrix(link->parent, link->frame, m, 1);
    }
}

/* VA 0x00486ee0, 318 bytes, [I] row, status draft */
int zobjUpdateChild(ZObj *obj, void *arg)
{
    RwMatrix parent, local, world;
    RwV3d pos;
    Zquat rot;
    ZObj *child;

    if (!(obj->flags & ZOBJF_ACTIVE))
        return 1;
    zobjParentMatrix(obj, &parent);
    zmathTransformPoint(&pos, &parent, &obj->pos);
    zmathMatrixClearTranslation(&local);
    zquatToMatrix(&local, &obj->rot);
    zmathMatrixMultiply(&world, &parent, &local);
    zquatFromMatrix(&rot, &world);
    if (obj->vtbl->setPosition)
        obj->vtbl->setPosition(obj, &pos);
    if (obj->vtbl->setRotation)
        obj->vtbl->setRotation(obj, &rot);
    zrwModelCreateRunLinks(obj);
    if (obj->vtbl->update)
        obj->vtbl->update(obj);
    if (obj->link) {
        for (child = obj->link->child; child; child = child->link->next) {
            zobjUpdateChild(child, arg);
            if (child->link == 0)
                return 1;
        }
    }
    return 1;
}

/* VA 0x00487020, 440 bytes, [I] row, status draft */
int zobjUpdateAll(void *arg)
{
    ZmemFreeNode *node;
    ZObj *obj, *child;

    for (node = zmemFreeListFirst(g_zobjPool); node; node = zmemFreeListNext(node)) {
        obj = (ZObj *)zmemFreeListNodeData(node);
        if (!(obj->flags & ZOBJF_ACTIVE) || (obj->flags & ZOBJF_ATTACHED))
            continue;                              /* children move with their root */
        zrwModelCreateRunLinks(obj);
        if (obj->vtbl->update)
            obj->vtbl->update(obj);
        if (obj->link == 0)
            continue;
        for (child = obj->link->child; child; child = child->link->next) {
            /* the exe inlines zobjUpdateChild for the first level, then iterates the grandchildren
             * with 0x004879C0 */
            if (child->flags & ZOBJF_ACTIVE) {
                RwMatrix parent, local, world;
                RwV3d pos;
                Zquat rot;

                zobjParentMatrix(child, &parent);
                zmathTransformPoint(&pos, &parent, &child->pos);
                zmathMatrixClearTranslation(&local);
                zquatToMatrix(&local, &child->rot);
                zmathMatrixMultiply(&world, &parent, &local);
                zquatFromMatrix(&rot, &world);
                if (child->vtbl->setPosition)
                    child->vtbl->setPosition(child, &pos);
                if (child->vtbl->setRotation)
                    child->vtbl->setRotation(child, &rot);
                zrwModelCreateRunLinks(child);
                if (child->vtbl->update)
                    child->vtbl->update(child);
                zobjForEachChild(child, zobjUpdateChild, arg);
            }
            if (child->link == 0)
                break;
        }
    }
    return 1;
}

/* VA 0x004871e0, 36 bytes, [I] row, status draft */
int zobjPrintStats(void *arg)
{
    g_pfnDebugPrintAt(0, -1, "ZObj: Used:%d", zmemFreeListGetUsedCount(g_zobjPool));
    return 1;
}

/* VA 0x00487210, 210 bytes, [V] direct, status draft */
int zobjInit(int maxObjects)
{
    if (g_zobjInitialised)
        zAssertFail("Already initialized", ZOBJ_FILE, 0xAE, 0);
    if (maxObjects == 0)
        zAssertFail("Can't initialize with 0", ZOBJ_FILE, 0xAF, 0);
    g_zobjInitialised = 1;
    /* maxObjects is only checked: the pool grows by 0x100 objects (zgeInit passes 0x32) */
    g_zobjPool = zmemFreeListCreate(sizeof(ZObj), 0x100, (ZmemFreeListCallback)zobjDestroy);
    g_zobjLinkPool = zmemFreeListCreate(sizeof(ZobjLink), 4, 0);
    g_zobjFrameCb = g_zglAddFrameCallback(8, zobjUpdateAll, 0, 0);
    if (g_zobjFrameCb == 0)
        return 0;
    g_zobjDebugCb = g_zrwDbgAddCallback(0, zobjPrintStats, 0);
    if (!zobjModelInit())
        return 0;
    if (!zobjLightInit())
        return 0;
    if (!zobjCameraInit())
        return 0;
    return zobjPEmitInit() != 0;
}

/* VA 0x004872f0, 116 bytes, [I] row, status draft */
int zobjShutdown(void)
{
    if (!g_zobjInitialised)
        return 1;
    g_zrwDbgRemoveCallback(g_zobjDebugCb);
    g_zglRemoveFrameCallback(g_zobjFrameCb);
    zmemFreeListDestroy(g_zobjPool);
    zmemFreeListDestroy(g_zobjLinkPool);
    if (!zobjPEmitShutdown())
        return 0;
    if (!zobjCameraShutdown())
        return 0;
    if (!zobjLightShutdown())
        return 0;
    if (!zobjModelShutdown())
        return 0;
    g_zobjInitialised = 0;
    return 1;
}

/* VA 0x00487370, 39 bytes, [I] row, status draft */
ZObj *zobjAlloc(void)
{
    ZObj *obj = (ZObj *)zmemFreeListAlloc(g_zobjPool);

    if (obj == 0)
        g_pfnDebugPrintf("ZOBJ: Error - failed to allocate obj\n");
    return obj;
}

/* VA 0x004873a0, 85 bytes, [V] direct, status draft */
ZObj *zobjConstruct(ZObj *obj, uint32_t type)
{
    if (!g_zobjInitialised)
        zAssertFail("Not initialized", ZOBJ_FILE, 0x119, 0);
    zstringMemSet(obj, 0, sizeof(ZObj));
    zquatIdentity(&obj->rot);
    obj->flags = (obj->flags & ~ZOBJ_TYPE_MASK) | (type & ZOBJ_TYPE_MASK) | ZOBJF_ACTIVE;
    return obj;
}

/* VA 0x00487400, 59 bytes, [I] row, status draft */
int zobjDestroy(ZObj *obj)
{
    if (obj->vtbl->destroy)
        obj->vtbl->destroy(obj);
    if (obj->link)
        zobjDetach(obj);
    zmemFreeListFree(g_zobjPool, obj);
    return 1;
}

/* VA 0x00487440, 8 bytes, [I] row, status draft */
void *zobjGetHandle(ZObj *obj)
{
    return obj->handle;
}

/* VA 0x00487450, 24 bytes, [I] row, status draft */
void *zobjGetRwObject(ZObj *obj)
{
    if (obj->vtbl->getRwObject == 0)
        return 0;
    return obj->vtbl->getRwObject(obj);
}

/* VA 0x00487470, 43 bytes, [I] row, status draft */
void zobjSetPosition(ZObj *obj, const RwV3d *pos)
{
    obj->pos = *pos;
    if (obj->vtbl->setPosition)
        obj->vtbl->setPosition(obj, pos);
}

/* VA 0x004874a0, 8 bytes, [I] row, status draft */
RwV3d *zobjGetPosition(ZObj *obj)
{
    return &obj->pos;
}

/* VA 0x004874b0, 26 bytes, [I] row, status draft */
void zobjSetScale(ZObj *obj, const RwV3d *scale)
{
    if (obj->vtbl->setScale)
        obj->vtbl->setScale(obj, scale);
}

/* VA 0x004874d0, 57 bytes, [I] row, status draft */
void zobjSetRotation(ZObj *obj, const Zquat *rot)
{
    obj->rot = *rot;
    if (obj->vtbl->setRotation)
        obj->vtbl->setRotation(obj, rot);
}

/* VA 0x00487510, 8 bytes, [I] row, status draft */
Zquat *zobjGetRotation(ZObj *obj)
{
    return &obj->rot;
}

/* VA 0x00487520, 83 bytes, [I] row, status draft */
void zobjSetRotationEuler(ZObj *obj, const int16_t *angles)
{
    Zquat q;

    zquatFromEuler(&q, angles, 0);
    obj->rot = q;
    if (obj->vtbl->setRotation)
        obj->vtbl->setRotation(obj, &q);
}

/* VA 0x00487580, 81 bytes, [I] row, status draft */
void zobjSetRotationMatrix(ZObj *obj, const RwMatrix *m)
{
    Zquat q;

    zquatFromMatrix(&q, m);
    obj->rot = q;
    if (obj->vtbl->setRotation)
        obj->vtbl->setRotation(obj, &q);
}

/* VA 0x004875e0, 12 bytes, [I] row, status draft */
void zobjSetUserData(ZObj *obj, void *user)
{
    obj->user = user;
}

/* VA 0x004875f0, 8 bytes, [I] row, status draft */
void *zobjGetUserData(ZObj *obj)
{
    return obj->user;
}

/* VA 0x00487600, 42 bytes, [I] row, status draft */
void zobjSetActive(ZObj *obj, int active)
{
    if (active)
        obj->flags |= ZOBJF_ACTIVE;
    else
        obj->flags &= ~ZOBJF_ACTIVE;
    if (obj->vtbl->setActive)
        obj->vtbl->setActive(obj, active);
}

/* VA 0x00487630, 26 bytes, [I] row, status draft */
void zobjSetFlag10(ZObj *obj, int on)
{
    if (on)
        obj->flags |= ZOBJF_FLAG10;
    else
        obj->flags &= ~ZOBJF_FLAG10;
}

/* VA 0x00487650, 93 bytes, [I] row, status draft */
int zobjForEachOfType(int (__cdecl *fn)(ZObj *obj, void *arg), void *arg, uint32_t type)
{
    ZmemFreeNode *node;

    for (node = zmemFreeListFirst(g_zobjPool); node; node = zmemFreeListNext(node)) {
        ZObj *obj = (ZObj *)zmemFreeListNodeData(node);

        if ((obj->flags & ZOBJ_TYPE_MASK) == type)
            fn(obj, arg);
    }
    return 1;
}

/* VA 0x004876b0, 114 bytes, [I] row, status draft */
ZObj *zobjFind(uint32_t id, void *unused, ZObj *after)
{
    ZmemFreeNode *node;

    node = after ? zmemFreeListNext(zmemFreeListDataNode(after)) : zmemFreeListFirst(g_zobjPool);
    for (; node; node = zmemFreeListNext(node)) {
        ZObj *obj = (ZObj *)zmemFreeListNodeData(node);
        uint32_t objId = obj->vtbl->getId ? obj->vtbl->getId(obj) : 0;

        if (objId == id)
            return obj;
    }
    return 0;
}

/* VA 0x00487730, 41 bytes, [I] row, status draft */
void zobjAllocLink(ZObj *obj)
{
    if (obj->link == 0) {
        obj->link = (ZobjLink *)zmemFreeListAlloc(g_zobjLinkPool);
        zstringMemSet(obj->link, 0, sizeof(ZobjLink));
    }
}

/* VA 0x00487760, 36 bytes, [I] row, status draft */
void zobjFreeLink(ZObj *obj)
{
    if (obj->link) {
        zmemFreeListFree(g_zobjLinkPool, obj->link);
        obj->link = 0;
    }
}

/* VA 0x00487790, 146 bytes, [I] row, status draft */
void zobjAttach(ZObj *obj, ZObj *parent, void *frame)
{
    if (obj->link && obj->link->parent) {
        g_pfnDebugPrintf("ZOBJ: Object is already attached to another\n");
        return;
    }
    zobjAllocLink(obj);                     /* inlined */
    zobjAllocLink(parent);                  /* inlined */
    obj->flags |= ZOBJF_ATTACHED;
    obj->link->parent = parent;
    obj->link->frame = frame;
    obj->link->next = parent->link->child;  /* push front */
    parent->link->child = obj;
}

/* VA 0x00487830, 298 bytes, [V] direct, status draft */
void zobjDetach(ZObj *obj)
{
    ZobjLink *link = obj->link;
    ZObj *prev, *child, *next;

    if (link == 0)
        return;
    if (link->parent) {
        ZobjLink *plink = link->parent->link;

        if (plink->child == obj) {
            plink->child = link->next;
            obj->flags &= ~ZOBJF_ATTACHED;
            obj->link->parent = 0;
            obj->link->frame = 0;
            obj->link->next = 0;
            /* back to the local transform */
            if (obj->vtbl->setPosition)
                obj->vtbl->setPosition(obj, &obj->pos);
            if (obj->vtbl->setRotation)
                obj->vtbl->setRotation(obj, &obj->rot);
            return;
        }
        for (prev = plink->child; prev; prev = prev->link->next) {
            if (prev->link->next == obj) {
                prev->link->next = link->next;
                /* [V] original bug: the rest of this branch resets the *previous sibling* (flag,
                 * link, transform with obj's position / rotation) instead of obj, which stays
                 * flagged as attached with a stale parent */
                prev->flags &= ~ZOBJF_ATTACHED;
                prev->link->parent = 0;
                prev->link->frame = 0;
                prev->link->next = 0;
                if (prev->vtbl->setPosition)
                    prev->vtbl->setPosition(prev, &obj->pos);
                if (prev->vtbl->setRotation)
                    prev->vtbl->setRotation(prev, &obj->rot);
                return;
            }
        }
        zAssertFail("Error in hierarchy", ZOBJ_FILE, 0x345, 0);
        /* falls through: orphan the children as if obj had no parent */
    }
    if (obj->link && obj->link->child) {
        for (child = obj->link->child; child; child = next) {
            next = child->link ? child->link->next : 0;
            child->link->parent = 0;        /* (crashes on a child without a link, as the exe) */
            child->link->frame = 0;
            child->link->next = 0;
        }
        obj->link->child = 0;
    }
}

/* VA 0x00487960, 18 bytes, [I] row, status draft */
ZObj *zobjGetNextSibling(ZObj *obj)
{
    if (obj->link == 0)
        return 0;
    return obj->link->next;
}

/* VA 0x00487980, 18 bytes, [I] row, status draft */
ZObj *zobjGetFirstChild(ZObj *obj)
{
    if (obj->link == 0)
        return 0;
    return obj->link->child;
}

/* VA 0x004879a0, 17 bytes, [I] row, status draft */
ZObj *zobjGetParent(ZObj *obj)
{
    if (obj->link == 0)
        return 0;
    return obj->link->parent;
}

/* VA 0x004879c0, 59 bytes, [I] row, status draft */
int zobjForEachChild(ZObj *obj, int (__cdecl *fn)(ZObj *child, void *arg), void *arg)
{
    ZObj *child;

    /* The ZGE object pass moved this from gap_zobj__zrwModel.c: it walks the zobj hierarchy */
    if (obj->link == 0)
        return 1;
    for (child = obj->link->child; child; child = child->link->next) {
        fn(child, arg);
        if (child->link == 0)
            return 1;
    }
    return 1;
}

/* VA 0x00487a00, 73 bytes, [I] row, status draft */
int zobjPrintCB(ZObj *obj, void *unused)
{
    /* (zmemFreeListForEach callback of zobjDumpAll) the clean-up moved it here from gap_zobj__zrwModel.c
     * (the ZglRw pass's code): it prints one pool entry. Original bug [V, disassembly]: when the type's slot 8
     * returns non-NULL the code prints "%s\n" without pushing the result, so the format reads a stale
     * stack word. The ZglRw pass read slot 8 as a name getter; the ZGE object pass names it getBounds (a model's def +
     * 0x14) [I]. */
    (void)unused;
    if (obj->vtbl->getBounds != 0 && obj->vtbl->getBounds(obj) != 0) {
        g_pfnDebugPrintf("%s\n");   /* sic: no argument */
        return 1;
    }
    g_pfnDebugPrintf("NONAME: kzobjType=%d\n", obj->flags & ZOBJ_TYPE_MASK);
    return 1;
}

/* VA 0x00487a50, 126 bytes, [I] row, status draft */
void zobjDumpAll(void)
{
    /* (zge.c's debug dump) the clean-up moved it here from gap_zobj__zrwModel.c. Memory = element size x
     * used count of both pools (objects and hierarchy links), then one line per object. */
    g_pfnDebugPrintf("ZOBJ: MemAllocated:%d, Objects in use:%d\n",
                     zmemFreeListGetElemSize(g_zobjPool) * zmemFreeListGetUsedCount(g_zobjPool) +
                     zmemFreeListGetElemSize(g_zobjLinkPool) * zmemFreeListGetUsedCount(g_zobjLinkPool),
                     zmemFreeListGetUsedCount(g_zobjPool));
    g_pfnDebugIndent(1);
    zmemFreeListForEach(g_zobjPool, (ZmemFreeListIterFn)zobjPrintCB, 0);
    g_pfnDebugIndent(-1);
}

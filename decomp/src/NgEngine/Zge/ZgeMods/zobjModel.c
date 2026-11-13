/*
 * NgEngine/Zge/ZgeMods/zobjModel.c
 * Original: C:\NgEngine\Zge\ZgeMods\zobjModel.c
 * Evidence: [V] path string referenced by 1 function(s); the others [I]: they are the slots of the
 * two model vtables 0x00560FA8 (animated) / 0x00560FD0 (static) that zobjModelCreate installs, and
 * zobjModelInit/Shutdown share the global 0x00672308. The ZGE object pass created all 15 others in the holes
 * 0x00488D60-0x00488F3F and 0x00488F80.
 * 35 functions, VA 0x00488d30-0x004892af.
 * Draft: 0x00488D60-0x00488FA2. The clean-up moved the ZglRw pass's drafts in from the gap files
 * gap_zrwModel__zobjModel.c (0x00488D30, the animated destroy) and gap_zobjModel__zrwMat.c
 * (0x00488FB0-0x004892AF: the constructor, the loaders and the public wrappers around the ZGL model
 * driver), [I] by the vtable 0x00560FA8 and the globals 0x00672308-0x00672310 they share.
 * Non-matching C (AD-8). Types: zobj.h, zglrw.h, zanim.h. Description: docs/re/zge.md 2.2.
 *
 * A model zobj wraps a zrwModel (handle). Models with fewer than two frames get the static vtable,
 * the others the animated one (whose destroy, 0x00488D30, also frees the zanim node table).
 * The earlier names zrwModelFindFrame / FrameToWorld / FrameOffset / UpdateObj are kept (their
 * callers use them) although these are zobjModel functions. The wrappers keep the ZglRw pass's parameter
 * types (void * / ZanimObj *, as their callers declare them); ZanimObj is the first 0x34 bytes of a
 * ZObj (+0x2C handle = model, +0x30 data = node table).
 */
#include "decomp.h"
#include "zobj.h"
#include "ngif.h"
#include "zanim.h"
#include "zglrw.h"

ZanimNodeTable *zanimCreateNodeTable(ZanimObj *obj);                 /* zanim.c */
void            zanimDestroyNodeTable(ZanimObj *obj);                /* zanim.c */
ZanimNode      *zanimFindNode(ZanimNodeTable *table, uint32_t hash); /* zanim.c */

#define ZOBJMODEL(o) ((ZglModel *)((ZObj *)(o))->handle)

int zanimInit(void);       /* 0x00499330 */
int zanimShutdown(void);   /* 0x004993B0 */

#define ZOBJMODEL_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zobjModel.c"
#define ZRWMODEL_HIDDEN 2u    /* zrwModel flag: not drawn */

int zobjModelDestroyAnimated(ZObj *obj);  /* 0x00488D30 animated-model destroy */

static int   g_zobjModelInitialised;   /* 0x00672308 */
static int   s_skeletonMode;           /* 0x0067230C zobjModelSetSkeletonMode */
static void *s_skeletonCallback;       /* 0x00672310 its ZGL callback (list 2) */

const ZobjVtbl g_zobjModelAnimVtbl = {   /* 0x00560FA8 */
    zobjModelDestroyAnimated, zobjModelUpdate, zobjModelSetPosition, zobjModelSetRotation, zobjModelSetScale,
    zobjModelSetActive, zobjModelGetId, zobjModelGetRwObject, zobjModelGetBounds
};
const ZobjVtbl g_zobjModelVtbl = {       /* 0x00560FD0 */
    zobjModelDestroy, zobjModelUpdateStatic, zobjModelSetPosition, zobjModelSetRotation,
    zobjModelSetScale, zobjModelSetActive, zobjModelGetId, zobjModelGetRwObject, zobjModelGetBounds
};

/* VA 0x00488d30, 38 bytes, [I] row, status draft */
int zobjModelDestroyAnimated(ZObj *obj)
{
    /* animated vtable slot 0: destroy */
    zanimDestroyNodeTable((ZanimObj *)obj);
    if (obj->handle != 0)
        g_zglModelApi.release((ZglModel *)obj->handle);
    return 1;
}

/* VA 0x00488d60, 23 bytes, [I] row, status draft */
int zobjModelUpdate(ZObj *obj)
{
    g_zrwModelSync(obj->handle);
    return 1;
}

/* VA 0x00488d80, 23 bytes, [I] row, status draft */
int zobjModelSetPosition(ZObj *obj, const RwV3d *pos)
{
    return g_zrwModelSetPosition(obj->handle, pos);
}

/* VA 0x00488da0, 23 bytes, [I] row, status draft */
int zobjModelSetRotation(ZObj *obj, const Zquat *rot)
{
    return g_zrwModelSetRotation(obj->handle, rot);
}

/* VA 0x00488dc0, 23 bytes, [I] row, status draft */
int zobjModelSetScale(ZObj *obj, const RwV3d *scale)
{
    return g_zrwModelSetScale(obj->handle, scale);
}

/* VA 0x00488de0, 62 bytes, [I] row, status draft */
int zobjModelSetActive(ZObj *obj, int active)
{
    if (!active)
        g_zrwModelSetFlags(obj->handle, ZRWMODEL_HIDDEN, 0);
    else
        g_zrwModelSetFlags(obj->handle, 0, ZRWMODEL_HIDDEN);
    return 1;
}

/* VA 0x00488e20, 20 bytes, [I] row, status draft */
uint32_t zobjModelGetId(ZObj *obj)
{
    return g_zrwModelGetId(obj->handle, 0);     /* the model's name hash */
}

/* VA 0x00488e40, 18 bytes, [I] row, status draft */
void *zobjModelGetRwObject(ZObj *obj)
{
    return g_zrwModelGetRwObject(obj->handle);
}

/* VA 0x00488e60, 20 bytes, [I] row, status draft */
void *zobjModelGetBounds(ZObj *obj)
{
    return g_zrwModelGetBounds(obj->handle, 0);
}

/* VA 0x00488e80, 23 bytes, [I] row, status draft */
int zobjModelUpdateStatic(ZObj *obj)
{
    g_zrwModelSync(obj->handle);                /* same code as zobjModelUpdate */
    return 1;
}

/* VA 0x00488ea0, 27 bytes, [I] row, status draft */
int zobjModelDestroy(ZObj *obj)
{
    if (obj->handle)
        g_zrwModelDestroy(obj->handle);
    return 1;
}

/* VA 0x00488ec0, 23 bytes, [I] row, status draft */
int zobjModelRender(ZObj *obj, void *arg)
{
    g_zrwModelRender(obj->handle);
    return 1;
}

/* VA 0x00488ee0, 23 bytes, [I] row, status draft */
int zobjModelRenderAll(void)
{
    zobjForEachOfType(zobjModelRender, 0, ZOBJ_TYPE_MODEL);
    return 1;
}

/* VA 0x00488f00, 27 bytes, [I] row, status draft */
int zobjModelHide(ZObj *obj)
{
    g_zrwModelSetFlags(obj->handle, ZRWMODEL_HIDDEN, 0);
    return 1;
}

/* VA 0x00488f20, 27 bytes, [I] row, status draft */
int zobjModelShow(ZObj *obj)
{
    g_zrwModelSetFlags(obj->handle, 0, ZRWMODEL_HIDDEN);
    return 1;
}

/* VA 0x00488f40, 55 bytes, [V] direct, status draft */
int zobjModelInit(void)
{
    if (g_zobjModelInitialised)
        zAssertFail("Already initialized", ZOBJMODEL_FILE, 0x19D, 0);
    if (!zanimInit())
        return 0;
    g_zobjModelInitialised = 1;
    return 1;
}

/* VA 0x00488f80, 35 bytes, [I] row, status draft */
int zobjModelShutdown(void)
{
    if (g_zobjModelInitialised) {
        if (!zanimShutdown())
            return 0;
        g_zobjModelInitialised = 0;
    }
    return 1;
}

/* VA 0x00488fb0, 156 bytes, [I] row, status draft */
void *zobjModelCreate(void *model)
{
    /* A model object for a ZGL model: one-part models get the static class and model flag 1,
     * others the animated class and a zanim node table. In skeleton debug mode it is taken out of the
     * world. */
    ZObj *obj;

    if (model == 0)
        return 0;
    obj = zobjAlloc();
    if (obj == 0)
        return 0;
    zobjConstruct(obj, ZOBJ_TYPE_MODEL);
    obj->handle = model;
    if ((uint32_t)g_zglModelApi.getNumParts((ZglModel *)model) < 2) {
        obj->vtbl = &g_zobjModelVtbl;
        g_zglModelApi.setFlags((ZglModel *)obj->handle, 0, 1);
    } else {
        obj->vtbl = &g_zobjModelAnimVtbl;
        g_zglModelApi.setFlags((ZglModel *)obj->handle, 1, 0);
        zanimCreateNodeTable((ZanimObj *)obj);
    }
    g_zglModelApi.setUser((ZglModel *)obj->handle, obj);
    if (s_skeletonMode)
        g_zglModelApi.setFlags((ZglModel *)obj->handle, 2, 0);
    return obj;
}

/* VA 0x00489050, 20 bytes, [I] row, status draft */
int zobjModelSave(void *model, const char *path)
{
    /* model slot 4 "save": not coded */
    return g_zglModelApi.save(model, path);
}

/* VA 0x00489070, 17 bytes, [I] row, status draft */
void *zobjModelLoadFile(const char *path, int flags)
{
    (void)flags;
    return g_zglModelApi.loadFile(path, 0);
}

/* VA 0x00489090, 17 bytes, [I] row, status draft */
void *zobjModelLoadIff(ZiffFile *iff, int flags)
{
    (void)flags;
    return g_zglModelApi.loadIff(iff, 0);
}

/* VA 0x004890b0, 15 bytes, [I] row, status draft */
void zobjModelFree(void *model)
{
    g_zglModelApi.release((ZglModel *)model);
}

/* VA 0x004890c0, 15 bytes, [I] row, status draft */
void *zobjModelCreateInstance(void *model)
{
    return g_zglModelApi.createInstance((ZglModel *)model);
}

/* VA 0x004890d0, 18 bytes, [I] row, status draft */
int32_t zobjModelGetNumParts(ZanimObj *obj)
{
    /* zanim.c */
    return g_zglModelApi.getNumParts(ZOBJMODEL(obj));
}

/* VA 0x004890f0, 23 bytes, [I] row, status draft */
void zobjModelGetPartIds(ZanimObj *obj, uint32_t *out)
{
    /* zanim.c */
    g_zglModelApi.getPartIds(ZOBJMODEL(obj), out);
}

/* VA 0x00489110, 23 bytes, [I] row, status draft */
void *zrwModelFindFrame(void *model, uint32_t hash)
{
    return g_zglModelApi.findPart(ZOBJMODEL(model), hash);
}

/* VA 0x00489130, 23 bytes, [I] row, status draft */
void *zobjModelGetPartParent(void *obj, void *part)
{
    /* no caller found */
    return g_zglModelApi.getPartParent(ZOBJMODEL(obj), (ZglModelPart *)part);
}

/* VA 0x00489150, 38 bytes, [I] row, status draft */
void zrwModelFrameToWorld(void *model, void *frame, const float *local, float *out, int update)
{
    g_zglModelApi.partToWorld(ZOBJMODEL(model), (ZglModelPart *)frame, local, out, update);
}

/* VA 0x00489180, 33 bytes, [I] row, status draft */
void zrwModelFrameOffset(void *model, void *frame, RwV3d *out, int update)
{
    /* the 4th argument is passed but unused */
    (void)update;
    g_zglModelApi.partBindOffset(ZOBJMODEL(model), (ZglModelPart *)frame, (float *)out);
}

/* VA 0x004891b0, 33 bytes, [I] row, status draft */
void zobjModelGetPartMatrix(ZObj *obj, void *part, RwMatrix *out, int flags)
{
    g_zglModelApi.getPartMatrix(ZOBJMODEL(obj), (ZglModelPart *)part, (float *)out, flags);
}

/* VA 0x004891e0, 15 bytes, [I] row, status draft */
int zobjModelHasNodes(void *obj)
{
    /* no caller found */
    return ((ZanimObj *)obj)->nodes != 0;
}

/* VA 0x004891f0, 22 bytes, [I] row, status draft */
ZanimNode *zobjModelFindNode(void *obj, uint32_t hash)
{
    /* no caller found */
    return zanimFindNode(((ZanimObj *)obj)->nodes, hash);
}

/* VA 0x00489210, 26 bytes, [I] row, status draft */
void zobjModelSetNodes(ZanimObj *obj, ZanimNodeTable *nodes)
{
    /* zanimCreateNodeTable */
    obj->nodes = nodes;
    g_zglModelApi.bindParts(ZOBJMODEL(obj), nodes);
}

/* VA 0x00489230, 111 bytes, [I] row, status draft */
void zobjModelSetSkeletonMode(int on)
{
    /* (no caller found; the ZglRw pass) Skeleton debug mode: a ZGL callback (list 2) draws all skeletons. Both
     * directions run zobjModelHide over every model object [V, disassembly: both call 0x00488F00]. */
    if (on == 0) {
        if (s_skeletonMode) {
            zrwZglRemoveCallback(s_skeletonCallback);
            zobjForEachOfType((int (__cdecl *)(ZObj *, void *))zobjModelHide, 0, ZOBJ_TYPE_MODEL);
            s_skeletonMode = 0;
        }
    } else if (s_skeletonMode == 0) {
        s_skeletonCallback = zrwZglAddCallback(2, (void *)zobjModelRenderAll, 0, 1);
        zobjForEachOfType((int (__cdecl *)(ZObj *, void *))zobjModelHide, 0, ZOBJ_TYPE_MODEL);
        s_skeletonMode = 1;
    }
}

/* VA 0x004892a0, 16 bytes, [I] row, status draft */
void zrwModelUpdateObj(void *model)
{
    g_zglModelApi.update(ZOBJMODEL(model));
}

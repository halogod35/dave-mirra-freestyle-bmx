/*
 * NgEngine/Zge/Com/ZglRw/zrwModel.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwModel.c
 * Evidence: [V] path string 0x00560F1C (asserts of zrwModelInit 0x2D1, zrwModelGetMaterial 0x5F0);
 *           the model driver table 0x00560E78 (slots 0, 1, 8-31 point here); [I] the rest by link
 *           order, the globals 0x006722E8-0x00672303 and the RpClump plugin offset 0x00560E70.
 * 51 functions, VA 0x00487AD0-0x00488D2B (18 new functions; zrwModelFindDefEntry 0x00487AD0 and
 * zrwModelSetColour / ApplyColour 0x00488CA0 / 0x00488CC0 came from the gap files on both sides).
 * Draft. Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 2.
 *
 * Coordinates: the engine (ZGE) works in its own units with y and z pointing the other way; the RW
 * world position of a ZGE point p is (p.x * s + o.x, o.y - p.y * s, o.z - p.z * s) with
 * s = g_zrwScale (0x00697720) and o = the world origin (0x006976CC) [V: zrwModelUpdate].
 */
#include "decomp.h"
#include "zanim.h"
#include "zmath.h"
#include "zglrw.h"

#define ZRWMODEL_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwModel.c"
#define ZRWMODEL_CLUMP_PLUGIN_ID 0xFF02

/* RpAtomic flags */
#define RPATOMIC_COLLISIONTEST 0x01
#define RPATOMIC_RENDER        0x04
#define RPATOMIC_STATIC        0x80   /* [I] engine flag (zrwModelSetFlags 1; zobjModelCreate sets it for one-part models) */

/* ------------------------------------------------------------------ zrwZgl.c globals */
extern void    *g_zrwWorld;            /* 0x006976C8 RpWorld * */
extern float    g_zrwWorldOrigin[3];   /* 0x006976CC */
extern float    g_zrwScale;            /* 0x00697720 ZGE -> RW */
extern float    g_zrwInvScale;         /* 0x00697724 RW -> ZGE */
extern uint32_t g_zrwZglFlags;         /* 0x00697728 bit 1: mirrored quaternion convention */

/* ------------------------------------------------------------------ globals */
int32_t g_zrwModelPluginOffset = -1;   /* 0x00560E70 RpClump plugin: the owning ZglModel */
int32_t g_zrwModelPlugin2Offset = -1;  /* 0x00560E74 a second plugin slot, never registered */
static struct {
    int32_t   initialized;             /* 0x006722E8 */
    RwMatrix *rot;                     /* 0x006722EC scratch: model rotation */
    RwMatrix *tmp;                     /* 0x006722F0 scratch: model matrix */
} s_zrwModel;
static const RwV3d s_zero;             /* 0x006722F8 */

#define ZABS(f) ((f) < 0.0f ? -(f) : (f))

/* identity rotation, zero position, rwMATRIXTYPEORTHONORMAL | rwMATRIXINTERNALIDENTITY (inline) */
static void zrwModelMatrixIdentity(RwMatrix *m)
{
    m->right.x = 1.0f; m->right.y = 0.0f; m->right.z = 0.0f;
    m->up.x = 0.0f;    m->up.y = 1.0f;    m->up.z = 0.0f;
    m->at.x = 0.0f;    m->at.y = 0.0f;    m->at.z = 1.0f;
    m->pos.x = 0.0f;   m->pos.y = 0.0f;   m->pos.z = 0.0f;
    m->flags |= 0x20003;
}

/* ------------------------------------------------------------------ functions */

/* VA 0x00487ad0, 66 bytes, [I] (from gap_zobj__zrwModel), status draft
 * The MDEF entry (name, id) with this id, NULL if none. */
ZglModelDefEntry *zrwModelFindDefEntry(ZglModel *model, uint32_t id)
{
    int i;
    for (i = 0; i < model->def->numEntries; i++)
        if (model->def->entries[i].id == id)
            return &model->def->entries[i];
    return NULL;
}

/* VA 0x00487b20, 21 bytes, [I] new function, status draft (App_Open) */
void zrwModelOpen(void)
{
    s_zrwModel.rot = RwMatrixCreate();
    s_zrwModel.tmp = RwMatrixCreate();
}

/* VA 0x00487b40, 27 bytes, [I] new function, status draft (App_Open's shutdown) */
void zrwModelClose(void)
{
    RwMatrixDestroy(s_zrwModel.rot);
    RwMatrixDestroy(s_zrwModel.tmp);
}

/* VA 0x00487b60, 34 bytes, [I] new function, status draft
 * RpClumpForAllAtomics callback: flags = flags & mask[0] | mask[1]; bumps the atomic's +0x14. */
void *zrwModelAtomicFlagsCB(void *atomic, uint32_t *mask)
{
    uint8_t *a = (uint8_t *)atomic;
    a[2] = (uint8_t)((a[2] & mask[0]) | mask[1]);
    a[0x14]++;
    return atomic;
}

/* VA 0x00487b90, 45 bytes, [I] callers, status draft */
void zrwModelSetAtomicFlags(void *clump, uint32_t clear, uint32_t set)
{
    uint32_t mask[2];
    mask[0] = ~clear;
    mask[1] = set;
    RpClumpForAllAtomics(clump, (void *)zrwModelAtomicFlagsCB, mask);
}

/* VA 0x00487bc0, 43 bytes, [I] new function, status draft (geometry flags callback) */
void *zrwModelGeometryFlagsCB(void *atomic, uint32_t *mask)
{
    void *geom = *(void **)((uint8_t *)atomic + 0x1C);
    RpGeometrySetFlags(geom, (RpGeometryGetFlags(geom) & mask[0]) | mask[1]);
    return atomic;
}

/* VA 0x00487bf0, 45 bytes, [I] callers, status draft */
void zrwModelSetGeometryFlags(void *clump, uint32_t clear, uint32_t set)
{
    uint32_t mask[2];
    mask[0] = ~clear;
    mask[1] = set;
    RpClumpForAllAtomics(clump, (void *)zrwModelGeometryFlagsCB, mask);
}

static void zrwModelClumpInWorld(void *clump, int on)
{
    if (on) {
        if (RpClumpGetWorld(clump) == NULL)
            RpWorldAddClump(g_zrwWorld, clump);
    } else if (RpClumpGetWorld(clump) != NULL)
        RpWorldRemoveClump(g_zrwWorld, clump);
}

/* VA 0x00487c20, 164 bytes, [I] callers, status draft
 * Adds the model's clumps to the world (on) or removes them. */
void zrwModelAddToWorld(ZglModel *model, int on)
{
    zrwModelClumpInWorld(model->clump, on);
    if (model->colClump != NULL)
        zrwModelClumpInWorld(model->colClump, on);
}

/* VA 0x00487cd0, 191 bytes, [I] callers, status draft
 * Collision on/off: the atomics' collision-test flag, or the MCOL clump in / out of the world. */
void zrwModelSetCollision(ZglModel *model, int on)
{
    uint32_t mask[2];
    if (model->colClump == NULL) {
        mask[0] = on ? 0xFFFFFFFFu : ~(uint32_t)RPATOMIC_COLLISIONTEST;
        mask[1] = on ? RPATOMIC_COLLISIONTEST : 0;
        RpClumpForAllAtomics(model->clump, (void *)zrwModelAtomicFlagsCB, mask);
    } else
        zrwModelClumpInWorld(model->colClump, on);
}

/* VA 0x00487d90, 22 bytes, [I] new function, status draft (RpClump plugin constructor) */
void *zrwModelClumpPluginCtor(void *clump, int offset)
{
    (void)offset;
    if (g_zrwModelPluginOffset > 0)
        *(void **)((uint8_t *)clump + g_zrwModelPluginOffset) = NULL;
    return clump;
}

/* VA 0x00487db0, 5 bytes, [I] new function, status draft (destructor) */
void *zrwModelClumpPluginDtor(void *clump)
{
    return clump;
}

/* VA 0x00487dc0, 29 bytes, [I] callers, status draft */
int zrwModelSetClumpOwner(void *clump, ZglModel *model)
{
    if (g_zrwModelPluginOffset < 1)
        return 0;
    *(ZglModel **)((uint8_t *)clump + g_zrwModelPluginOffset) = model;
    return 1;
}

/* VA 0x00487de0, 20 bytes, [I] new function, status draft (no caller found) */
ZglModel *zrwModelGetClumpOwner(void *clump)
{
    if (g_zrwModelPluginOffset < 1)
        return NULL;
    return *(ZglModel **)((uint8_t *)clump + g_zrwModelPluginOffset);
}

/* VA 0x00487e00, 22 bytes, [I] new function, status draft (second plugin: never registered) */
void *zrwModelPlugin2Ctor(void *obj, int offset)
{
    (void)offset;
    if (g_zrwModelPlugin2Offset > 0)
        *(void **)((uint8_t *)obj + g_zrwModelPlugin2Offset) = NULL;
    return obj;
}

/* VA 0x00487e20, 5 bytes, [I] new function, status draft */
void *zrwModelPlugin2Dtor(void *obj)
{
    return obj;
}

/* VA 0x00487e30, 29 bytes, [I] new function, status draft */
int zrwModelPlugin2Set(void *obj, void *value)
{
    if (g_zrwModelPlugin2Offset < 1)
        return 0;
    *(void **)((uint8_t *)obj + g_zrwModelPlugin2Offset) = value;
    return 1;
}

/* VA 0x00487e50, 20 bytes, [I] new function, status draft */
void *zrwModelPlugin2Get(void *obj)
{
    if (g_zrwModelPlugin2Offset < 1)
        return NULL;
    return *(void **)((uint8_t *)obj + g_zrwModelPlugin2Offset);
}

/* VA 0x00487e70, 322 bytes, [I] row, status draft
 * ZGE quaternion (x, y, z, w) -> the rotation part of an RwMatrix. y and z flip sign (ZGE axes). */
void zrwModelQuatToRwMatrix(const float *quat, RwMatrix *m)
{
    Zquat q;
    float n, s, ys, zs, xw, xx;

    q.x = quat[0];
    q.w = quat[3];
    q.y = -quat[1];
    q.z = -quat[2];
    n = zquatNormalize(&q);
    s = (n <= 0.0f) ? 0.0f : 2.0f / n;
    ys = s * q.y;
    zs = s * q.z;
    xw = s * q.x * q.w;
    xx = s * q.x * q.x;
    m->right.x = 1.0f - (zs * q.z + ys * q.y);
    m->right.y = ys * q.x + zs * q.w;
    m->right.z = zs * q.x - ys * q.w;
    m->up.x    = ys * q.x - zs * q.w;
    m->up.y    = 1.0f - (zs * q.z + xx);
    m->up.z    = zs * q.y + xw;
    m->at.x    = zs * q.x + ys * q.w;
    m->at.y    = zs * q.y - xw;
    m->at.z    = 1.0f - (ys * q.y + xx);
    RwMatrixUpdate(m);
}

/* VA 0x00487fc0, 330 bytes, [I] callers, status draft
 * As zrwModelQuatToRwMatrix with the x-coupled terms negated (mirrored convention, used when
 * g_zrwZglFlags bit 1 is set). */
void zrwModelQuatToRwMatrixMirror(const float *quat, RwMatrix *m)
{
    Zquat q;
    float n, s, ys, zs, xw, xx;

    q.x = quat[0];
    q.w = quat[3];
    q.y = -quat[1];
    q.z = -quat[2];
    n = zquatNormalize(&q);
    s = (n <= 0.0f) ? 0.0f : 2.0f / n;
    ys = s * q.y;
    zs = s * q.z;
    xw = s * q.x * q.w;
    xx = s * q.x * q.x;
    m->right.x = 1.0f - (zs * q.z + ys * q.y);
    m->right.y = -(ys * q.x + zs * q.w);
    m->right.z = -(zs * q.x - ys * q.w);
    m->up.x    = -(ys * q.x - zs * q.w);
    m->up.y    = 1.0f - (zs * q.z + xx);
    m->up.z    = zs * q.y + xw;
    m->at.x    = -(zs * q.x + ys * q.w);
    m->at.y    = zs * q.y - xw;
    m->at.z    = 1.0f - (ys * q.y + xx);
    RwMatrixUpdate(m);
}

/* The animation pose of the current LOD: every bound part's frame gets its node's rotation and
 * bind position + node translation. */
static void zrwModelPoseParts(ZglModel *model)
{
    ZglModelLod *lod = &model->lod[model->curLod];
    ZglModelPart *part = lod->parts;
    int n;

    for (n = lod->numParts; n != 0; n--, part++) {
        ZanimNode *node = (ZanimNode *)part->node;
        if (node != NULL) {
            RwMatrix *m = RwFrameGetMatrix(part->frame);
            zrwModelQuatToRwMatrix(node->quat, m);
            m->pos.x = node->trans[0] * g_zrwScale + part->bindPos.x;
            m->pos.y = part->bindPos.y - node->trans[1] * g_zrwScale;
            m->pos.z = part->bindPos.z - node->trans[2] * g_zrwScale;
        }
    }
}

/* VA 0x00488110, 126 bytes, [I] new function, status draft (no caller found; inlined in Update) */
void zrwModelUpdateParts(ZglModel *model)
{
    zrwModelPoseParts(model);
}

/* VA 0x00488190, 107 bytes, [V] direct, status draft (model driver slot 0) */
int zrwModelInit(void)
{
    if (s_zrwModel.initialized)
        zAssertFail("Already initialized", ZRWMODEL_SRC, 0x2D1, 0);
    zstringMemSet(&s_zrwModel, 0, sizeof(s_zrwModel));
    if (!zrwModelCreateInit())
        return 0;
    if (!zrwModelRenderInit())
        return 0;
    s_zrwModel.initialized = 1;
    zrwModelOpen();
    return 1;
}

/* VA 0x00488200, 71 bytes, [I] new function, status draft (slot 1) */
int zrwModelShutdown(void)
{
    if (s_zrwModel.initialized) {
        zrwModelClose();
        if (!zrwModelCreateShutdown())
            return 0;
        if (!zrwModelRenderShutdown())
            return 0;
        s_zrwModel.initialized = 0;
    }
    return 1;
}

/* VA 0x00488250, 186 bytes, [I] callers, status draft (slot 21)
 * Binds every part of every LOD to the animation node with the same id (zobjModel attaches its
 * zanim node table). Unmatched parts print "BodyPart not matched". */
void zrwModelBindParts(ZglModel *model, ZanimNodeTable *table)
{
    int l, p, i;

    for (l = 0; l < model->def->numLods; l++) {
        ZglModelPart *part = model->lod[l].parts;
        for (p = model->lod[l].numParts; p != 0; p--, part++) {
            ZanimNode *node = table->nodes;
            part->node = NULL;
            for (i = 0; i < table->count; i++, node++)
                if (part->id == node->hash) {
                    part->node = node;
                    break;
                }
            if (part->node == NULL)
                g_pfnDebugPrintf("BodyPart not matched Hash:%x\n", part->id);
        }
    }
    model->flags |= 2;
}

/* VA 0x00488310, 8 bytes, [I] callers, status draft (slot 12: parts of LOD 0) */
int32_t zrwModelGetNumParts(ZglModel *model)
{
    return model->lod[0].numParts;
}

/* VA 0x00488320, 40 bytes, [I] callers, status draft (slot 20: the part ids of LOD 0) */
void zrwModelGetPartIds(ZglModel *model, uint32_t *out)
{
    ZglModelPart *part = model->lod[0].parts;
    int n;
    for (n = model->lod[0].numParts; n != 0; n--, part++)
        *out++ = part->id;
}

/* VA 0x00488350, 67 bytes, [I] callers, status draft (slot 13) */
ZglModelPart *zrwModelFindPart(ZglModel *model, uint32_t id)
{
    ZglModelPart *part = model->lod[0].parts;
    int n;
    for (n = model->lod[0].numParts; n != 0; n--, part++)
        if (part->id == id)
            return part;
    g_pfnDebugPrintf("Part (0x%x)not found in model:%s\n", id, model->def->name);
    return NULL;
}

/* VA 0x004883a0, 220 bytes, [I] callers, status draft (slot 14)
 * A ZGE-space offset in a part's space -> ZGE world position. flags bit 0: use the part's LTM
 * (world) matrix, else its modelling matrix. */
void zrwModelPartToWorld(ZglModel *model, ZglModelPart *part, const float *local, float *out, int flags)
{
    RwMatrix *m = (flags & 1) ? RwFrameGetLTM(part->frame) : RwFrameGetMatrix(part->frame);
    RwV3d v, o;
    (void)model;

    v.x = g_zrwScale * local[0];
    v.y = -(local[1] * g_zrwScale);
    v.z = -(local[2] * g_zrwScale);
    RwV3dTransformVectors(&v, &v, 1, m);
    RwV3dTransformPoints(&o, &s_zero, 1, m);
    out[0] = ((o.x + v.x) - g_zrwWorldOrigin[0]) * g_zrwInvScale;
    out[1] = -(((o.y + v.y) - g_zrwWorldOrigin[1]) * g_zrwInvScale);
    out[2] = -(((o.z + v.z) - g_zrwWorldOrigin[2]) * g_zrwInvScale);
}

/* VA 0x00488480, 48 bytes, [I] callers, status draft (slot 15: the bind position in ZGE units) */
void zrwModelPartBindOffset(ZglModel *model, ZglModelPart *part, float *out)
{
    (void)model;
    out[0] = part->bindPos.x * g_zrwInvScale;
    out[1] = -(part->bindPos.y * g_zrwInvScale);
    out[2] = -(part->bindPos.z * g_zrwInvScale);
}

/* VA 0x004884b0, 63 bytes, [I] callers, status draft (slot 16: the part matrix in ZGE form) */
void zrwModelGetPartMatrix(ZglModel *model, ZglModelPart *part, float *out, int flags)
{
    (void)model;
    zrwZglMatrixFromRw(out, (flags & 1) ? RwFrameGetLTM(part->frame) : RwFrameGetMatrix(part->frame));
}

/* VA 0x004884f0, 8 bytes, [I] new function, status draft (slot 17) */
ZglModelPart *zrwModelGetPartParent(ZglModel *model, ZglModelPart *part)
{
    (void)model;
    return part->parent;
}

/* VA 0x00488500, 25 bytes, [I] callers, status draft (slot 18; a non-NULL key returns garbage [V]) */
uint32_t zrwModelGetNameHash(ZglModel *model, const uint32_t *key)
{
    if (key == NULL)
        return model->def->nameId;
    return 0;
}

/* VA 0x00488520, 87 bytes, [I] callers, status draft (slot 19)
 * key NULL: the model name; else the MDEF entry with id *key. */
void *zrwModelGetDefEntry(ZglModel *model, const uint32_t *key)
{
    if (key == NULL)
        return model->def->name;
    return zrwModelFindDefEntry(model, *key);
}

/* VA 0x00488580, 31 bytes, [I] callers, status draft (slot 9) */
int zrwModelSetPosition(ZglModel *model, const RwV3d *pos)
{
    model->pos = *pos;
    return 1;
}

/* VA 0x004885a0, 39 bytes, [I] callers, status draft (slot 10) */
int zrwModelSetRotation(ZglModel *model, const float *quat)
{
    model->rot[0] = quat[0];
    model->rot[1] = quat[1];
    model->rot[2] = quat[2];
    model->rot[3] = quat[3];
    return 1;
}

/* VA 0x004885d0, 125 bytes, [I] callers, status draft (slot 11; flag 1 = not unit scale) */
int zrwModelSetScale(ZglModel *model, const RwV3d *scale)
{
    model->scale = *scale;
    if (ZABS(model->scale.x - 1.0f) < 1e-05f && ZABS(model->scale.y - 1.0f) < 1e-05f &&
        ZABS(model->scale.z - 1.0f) < 1e-05f)
        model->flags &= ~1u;
    else
        model->flags |= 1u;
    return 1;
}

/* VA 0x00488650, 566 bytes, [I] callers, status draft (slot 8)
 * Per-frame transform: root frame = scale * rotation(quat) + RW position of model->pos; the parts
 * get the animation pose; skinned models update their bones; then the update callback, and the MCOL
 * clump copies the root matrix. */
int zrwModelUpdate(ZglModel *model)
{
    void *clump = model->clump;
    RwMatrix *root;
    float x, y, z;

    if ((g_zrwZglFlags & 2) == 0)
        zrwModelQuatToRwMatrix(model->rot, s_zrwModel.rot);
    else
        zrwModelQuatToRwMatrixMirror(model->rot, s_zrwModel.rot);
    x = model->pos.x * g_zrwScale + g_zrwWorldOrigin[0];
    y = g_zrwWorldOrigin[1] - model->pos.y * g_zrwScale;
    z = g_zrwWorldOrigin[2] - model->pos.z * g_zrwScale;
    if (model->flags & 1)
        RwMatrixScale(s_zrwModel.rot, &model->scale, 1 /* rwCOMBINEPRECONCAT */);

    root = RwFrameGetMatrix(RpClumpGetFrame(clump));
    zrwModelMatrixIdentity(root);
    if (model->flags & 2)
        zrwModelPoseParts(model);
    RwMatrixMultiply(s_zrwModel.tmp, root, s_zrwModel.rot);
    s_zrwModel.tmp->pos.x = root->pos.x + x;
    s_zrwModel.tmp->pos.y = root->pos.y + y;
    s_zrwModel.tmp->pos.z = root->pos.z + z;
    *root = *s_zrwModel.tmp;
    RwFrameUpdateObjects(RpClumpGetFrame(clump));
    if (model->def->mdefFlags & 2)
        RpBoneClumpUpdate(clump);
    if (model->updateCB != NULL)
        model->updateCB(model);
    if (model->colClump != NULL) {
        void *f = RpClumpGetFrame(model->colClump);
        *RwFrameGetMatrix(f) = *root;
        RwFrameUpdateObjects(f);
    }
    return 1;
}

/* VA 0x00488890, 43 bytes, [I] callers, status draft (App_Open, before RwEngineStart) */
int zrwModelRegisterPlugin(void)
{
    g_zrwModelPluginOffset = RpClumpRegisterPlugin(sizeof(void *), ZRWMODEL_CLUMP_PLUGIN_ID,
                                                   zrwModelClumpPluginCtor, zrwModelClumpPluginDtor,
                                                   NULL);
    return g_zrwModelPluginOffset != -1;
}

static void zrwModelAtomicMask(void *clump, uint32_t and, uint32_t or)
{
    uint32_t mask[2];
    mask[0] = and;
    mask[1] = or;
    RpClumpForAllAtomics(clump, (void *)zrwModelAtomicFlagsCB, mask);
}

/* VA 0x004888c0, 701 bytes, [I] callers, status draft (slot 24)
 * Model flags (clear first, then set):
 *   1 static      def flag bit 0 and atomic flag 0x80 (zobjModelCreate: set for one-part models) [I]
 *   2 in world    zrwModelAddToWorld
 *   4 show MCOL   collision clump rendered (RENDER | COLLISIONTEST) or collision only
 *   8 collision   zrwModelSetCollision */
int zrwModelSetFlags(ZglModel *model, uint32_t clear, uint32_t set)
{
    if (model->colClump != NULL) {
        if (clear & 4)
            zrwModelAtomicMask(model->colClump, ~5u, RPATOMIC_COLLISIONTEST);
        if (set & 4)
            zrwModelAtomicMask(model->colClump, ~5u, RPATOMIC_COLLISIONTEST | RPATOMIC_RENDER);
    }
    if (model->colClump != NULL) {
        if (clear & 4)
            zrwModelAtomicMask(model->colClump, ~(uint32_t)RPATOMIC_RENDER, 0);
        if (set & 4)
            zrwModelAtomicMask(model->colClump, 0xFFFFFFFFu, RPATOMIC_RENDER);
    }
    if (clear & 2)
        zrwModelAddToWorld(model, 0);
    if (set & 2)
        zrwModelAddToWorld(model, 1);
    if (clear & 8) {
        if (model->colClump == NULL)
            zrwModelSetAtomicFlags(model->clump, RPATOMIC_COLLISIONTEST, 0);
        else
            zrwModelClumpInWorld(model->colClump, 0);
    }
    if (set & 8) {
        if (model->colClump == NULL)
            zrwModelAtomicMask(model->clump, 0xFFFFFFFFu, RPATOMIC_COLLISIONTEST);
        else
            zrwModelClumpInWorld(model->colClump, 1);
    }
    if (clear & 1) {
        model->def->flags &= ~1u;
        zrwModelAtomicMask(model->clump, ~(uint32_t)RPATOMIC_STATIC, 0);
        if (model->colClump != NULL)
            zrwModelAtomicMask(model->colClump, ~(uint32_t)RPATOMIC_STATIC, 0);
    }
    if (set & 1) {
        model->def->flags |= 1u;
        zrwModelAtomicMask(model->clump, 0xFFFFFFFFu, RPATOMIC_STATIC);
        if (model->colClump != NULL)
            zrwModelAtomicMask(model->colClump, 0xFFFFFFFFu, RPATOMIC_STATIC);
    }
    return 0;
}

/* VA 0x00488b80, 23 bytes, [I] callers, status draft (the model of an atomic, via its clump) */
ZglModel *zrwModelFromAtomic(void *atomic)
{
    if (g_zrwModelPluginOffset < 1)
        return NULL;
    return *(ZglModel **)(*(uint8_t **)((uint8_t *)atomic + 0x40) + g_zrwModelPluginOffset);
}

/* VA 0x00488ba0, 12 bytes, [I] callers, status draft (slot 25) */
void zrwModelSetUser(ZglModel *model, void *user) { model->user = user; }

/* VA 0x00488bb0, 8 bytes, [I] callers, status draft (slot 26) */
void *zrwModelGetUser(ZglModel *model) { return model->user; }

/* VA 0x00488bc0, 15 bytes, [I] callers, status draft (slot 27: the collision clump, else the clump) */
void *zrwModelGetClump(ZglModel *model)
{
    return model->colClump != NULL ? model->colClump : model->clump;
}

/* VA 0x00488bd0, 82 bytes, [I] callers, status draft (slot 28: material by texture name hash) */
ZglMaterial *zrwModelFindMaterial(ZglModel *model, uint32_t textureHash)
{
    ZglMaterial *mat;
    for (mat = (ZglMaterial *)zHeapListFirst(model->materials); mat != NULL;
         mat = (ZglMaterial *)zHeapListNext(model->materials))
        if (g_texDriverVtbl.getNameHash(g_zglMaterialApi.getTexture(mat, 0)) == textureHash)
            return mat;
    return NULL;
}

/* VA 0x00488c30, 17 bytes, [I] callers, status draft (slot 29) */
int32_t zrwModelGetNumMaterials(ZglModel *model)
{
    return zHeapListCount(model->materials);
}

/* VA 0x00488c50, 67 bytes, [V] direct, status draft (slot 30) */
ZglMaterial *zrwModelGetMaterial(ZglModel *model, uint32_t index)
{
    if (index < (uint32_t)g_zglModelApi.getNumMaterials(model))
        return (ZglMaterial *)zHeapListGet(model->materials, index);
    zAssertFail("Material index out of range", ZRWMODEL_SRC, 0x5F0, 0);
    return NULL;
}

/* VA 0x00488ca0, 31 bytes, [I] (from gap_zrwModel__zobjModel), status draft (slot 31)
 * Model tint: kept in model->colour and applied by zrwModelRenderColouredCB at draw time. */
void zrwModelSetColour(ZglModel *model, const RwRGBA *colour)
{
    model->colour = *colour;
    zrwModelForEachMaterial(model, (void *)zrwModelRenderColouredCB);
}

/* VA 0x00488cc0, 108 bytes, [I] (from gap_zrwModel__zobjModel), status draft
 * rpGEOMETRYMODULATEMATERIALCOLOR (0x40) on, and every material of the first geometry gets the
 * colour. */
void zrwModelApplyColour(ZglModel *model, const RwRGBA *colour)
{
    void *geom = *(void **)((uint8_t *)zrwModelGetFirstAtomic(model->clump) + 0x1C);
    int i;

    { uint32_t mask[2] = { 0xFFFFFFFFu, 0x40 };
      RpClumpForAllAtomics(model->clump, (void *)zrwModelGeometryFlagsCB, mask); }
    for (i = RpGeometryGetNumMaterials(geom); i != 0;)
        RpMaterialSetColor(RpGeometryGetMaterial(geom, --i), colour);
}

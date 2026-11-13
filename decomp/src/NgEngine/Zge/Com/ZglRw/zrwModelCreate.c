/*
 * NgEngine/Zge/Com/ZglRw/zrwModelCreate.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwModelCreate.c
 * Evidence: [V] path string 0x00562E78 (asserts of zrwModelReadMDEF 0x495, ChunkMLOD 0x3FC,
 *           ChunkMCOL 0x441, Init 0x52D, LoadMemory 0x577-0x57E, Save 0x5BA); "zrwModelCreate..."
 *           / "zrwModelSetRwInfo" messages; model driver slots 2-7; [I] the rest by link order and the
 *           globals 0x00675218-0x0067523B. zrwModelRelease .. zrwModelPrintInfo (0x0049DCD0-
 *           0x0049DF8F, before zrwModelGeom.c's first assert) use this file's pool [I].
 * 51 functions, VA 0x0049C440-0x0049DF8F (DM-064 created the 7 link functions 0x0049C440-0x0049C56B) (DM-059 created 28; 0x0049D895 is the tail of
 * zrwModelChunkMODL that Ghidra keeps as its own function).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 2.
 *
 * Loading a .ZMD (docs/formats/ZMD.md): IFF handlers 'MODL', 'MLOD', 'MCOL' (registered by
 * zrwModelCreateInit). MODL: read MDEF (kind/flags, name, name id, entry table; the four params are
 * not read), process every MATI (zrwMat: textures -> materials, collected through the material
 * creation callback), then every MLOD (max 4: MLDF word 1 = LOD distance, MDFF = a RenderWare
 * clump stream read with RpClumpStreamRead from memory), then MCOL if MDEF flag 0x80. The first
 * LOD's clump is the model; each LOD gets a part table built from its frame hierarchy.
 */
#include "decomp.h"
#include "zglrw.h"

#define ZRWMC_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwModelCreate.c"
#define IFF_MODL 0x4C444F4Du
#define IFF_MLOD 0x444F4C4Du
#define IFF_MLDF 0x46444C4Du
#define IFF_MDFF 0x4646444Du
#define IFF_MDEF 0x4645444Du
#define IFF_MATI 0x4954414Du
#define IFF_MCOL 0x4C4F434Du
#define RWID_CLUMP 0x10
#define RWSTREAM_MEMORY 3
#define RWSTREAM_READ   1

extern void *g_zrwWorld;           /* 0x006976C8 */

/* MDEF header as read (0x2C bytes, ZMD.md 4) */
typedef struct ZrwMdefHeader {
    uint32_t kind;        /* +0x00 -> def->mdefFlags */
    char     name[16];    /* +0x04 */
    uint32_t nameId;      /* +0x14 */
    float    params[4];   /* +0x18 not used */
    uint16_t count;       /* +0x28 */
    uint16_t unk2a;
} ZrwMdefHeader;

/* ------------------------------------------------------------------ globals */
static struct {
    int32_t       initialized;     /* 0x00675218 */
    ZmemFreeList *pool;            /* 0x0067521C ZglModel free list */
    uint32_t      unk08;
    ZglModelPart *nextPart;        /* 0x00675224 zrwModelAddPartCB cursor */
    void         *lodClump[4];     /* 0x00675228 */
    ZglModel     *lastLoaded;      /* 0x00675238 the MODL handler's result */
} s_zrwMc;                          /* memset 0x24 by Init (to 0x0067523B) */
static const float s_lodDistance[4] = { 0.0f, 50.0f, 100.0f, 150.0f };   /* 0x00562CC4 */

/* ------------------------------------------------------------------ links (obj + 0x38) */
typedef struct ZrwLink { int32_t type; int32_t id; void (__cdecl *fn)(int32_t *item); } ZrwLink;
typedef struct ZrwLinkSet { zHeapList *pending; zHeapList *links; } ZrwLinkSet;
#define LINKSET(o) (*(ZrwLinkSet **)((uint8_t *)(o) + 0x38))

static int32_t       s_zrwLinkInitialised;   /* 0x00675210 */
static ZmemFreeList *s_zrwLinkItemPool;      /* 0x00675214 8-byte link items, 32 per block */

/* VA 0x0049c440, 35 bytes, [I] row, status draft */
int zrwModelLinkInit(void)
{
    /* (DM-064 created; no caller) The link module: an 8-byte item pool (32 per block) and per-object
     * link sets (obj + 0x38). [I] attribution: the same ZrwLinkSet as zrwModelCreateRunLinks /
     * zrwModelAddLink, and its globals 0x00675210 / 0x00675214 sit right before this file's s_zrwMc. */
    s_zrwLinkInitialised = 1;
    s_zrwLinkItemPool = zmemFreeListCreate(8, 0x20, NULL);
    return 1;
}

/* VA 0x0049c470, 39 bytes, [I] row, status draft */
int zrwModelLinkShutdown(void)
{
    /* DM-064 created; no caller */
    if (s_zrwLinkInitialised) {
        zmemFreeListDestroy(s_zrwLinkItemPool);
        s_zrwLinkInitialised = 0;
    }
    return 1;
}

/* VA 0x0049c4a0, 27 bytes, [I] row, status draft */
void *zrwModelLinkAllocItem(void)
{
    /* DM-064 created; no caller */
    if (!s_zrwLinkInitialised)
        return NULL;
    return zmemFreeListAlloc(s_zrwLinkItemPool);
}

/* VA 0x0049c4c0, 33 bytes, [I] row, status draft */
void zrwModelLinkFreeItem(void *item)
{
    /* DM-064 created; no caller */
    if (s_zrwLinkInitialised && item != NULL)
        zmemFreeListFree(s_zrwLinkItemPool, item);
}

/* VA 0x0049c4f0, 48 bytes, [I] row, status draft */
void *zrwModelLinkCreateSet(void)
{
    /* DM-064 created; no caller */
    ZrwLinkSet *set;

    if (!s_zrwLinkInitialised)
        return NULL;
    set = (ZrwLinkSet *)zmemAlloc(sizeof(ZrwLinkSet));
    set->pending = zHeapListCreate(4);
    set->links = zHeapListCreate(4);
    return set;
}

/* VA 0x0049c520, 46 bytes, [I] row, status draft */
void zrwModelLinkDestroySet(void *p)
{
    /* DM-064 created; no caller */
    ZrwLinkSet *set = (ZrwLinkSet *)p;

    if (s_zrwLinkInitialised && set != NULL) {
        zHeapListDestroy(set->pending);
        zHeapListDestroy(set->links);
        zmemFree(set);
    }
}

/* VA 0x0049c550, 28 bytes, [I] row, status draft */
void zrwModelLinkQueue(void *obj, void *item)
{
    /* (DM-064 created; no caller): queue an item for zrwModelCreateRunLinks */
    if (LINKSET(obj) != NULL)
        zHeapListAdd(LINKSET(obj)->pending, item);
}

/* VA 0x0049c570, 131 bytes, [I] row, status draft */
void zrwModelCreateRunLinks(void *obj)
{
    /* (no caller found) For every pending item, call the type-1 links registered for its id, then drop the item. */
    ZrwLinkSet *set = LINKSET(obj);
    int32_t *item;

    if (set == NULL || set->pending == NULL)
        return;
    item = (int32_t *)zHeapListFirst(set->pending);
    while (item != NULL) {
        if (LINKSET(obj)->links != NULL) {
            ZrwLink *l;
            for (l = (ZrwLink *)zHeapListFirst(LINKSET(obj)->links); l != NULL;
                 l = (ZrwLink *)zHeapListNext(LINKSET(obj)->links))
                if (l->id == item[0] && l->type == 1)
                    l->fn(item);
        }
        zHeapListRemoveCurrent(LINKSET(obj)->pending);
        item = (int32_t *)zHeapListCurrent(LINKSET(obj)->pending);
    }
}

/* VA 0x0049c600, 120 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwModelAddLink(void *obj, int32_t id, void (__cdecl *fn)(int32_t *item))
{
    ZrwLink *l;
    if (LINKSET(obj) == NULL || LINKSET(obj)->links == NULL)
        return;
    for (l = (ZrwLink *)zHeapListFirst(LINKSET(obj)->links); l != NULL;
         l = (ZrwLink *)zHeapListNext(LINKSET(obj)->links))
        if (l->id == id) {
            zHeapListRemoveCurrent(LINKSET(obj)->links);
            break;
        }
    l = (ZrwLink *)zmemAlloc(sizeof(ZrwLink));
    l->type = 1;
    l->id = id;
    l->fn = fn;
    zHeapListAdd(LINKSET(obj)->links, l);
}

/* VA 0x0049c680, 82 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwModelRemoveLink(void *obj, int32_t id)
{
    ZrwLink *l;
    if (LINKSET(obj) == NULL || LINKSET(obj)->links == NULL)
        return;
    for (l = (ZrwLink *)zHeapListFirst(LINKSET(obj)->links); l != NULL;
         l = (ZrwLink *)zHeapListNext(LINKSET(obj)->links))
        if (l->id == id) {
            zHeapListRemoveCurrent(LINKSET(obj)->links);
            return;
        }
}

/* RpClumpClone does not copy the Animation plugin id of the frames, so createInstance parks it in
 * the (about to be rebuilt) frame matrix: at.x = low 16 bits, at.y = high 16 bits [V]. Harmless:
 * zrwModelUpdate rewrites every bound frame matrix. */

/* VA 0x0049c6e0, 93 bytes, [I] DM-059 created, status draft */
void *zrwModelStashFrameIdCB(void *frame, void *data)
{
    RwMatrix *m = RwFrameGetMatrix(frame);
    uint32_t id = RpAnimFrameGetID(frame);
    m->at.x = (float)(id & 0xFFFF);
    m->at.y = (float)(id >> 16);
    RwFrameForAllChildren(frame, zrwModelStashFrameIdCB, data);
    return frame;
}

/* VA 0x0049c740, 90 bytes, [I] DM-059 created, status draft (unreferenced copy for a clump) */
void zrwModelStashFrameIds(void *clump)
{
    zrwModelStashFrameIdCB(RpClumpGetFrame(clump), NULL);
}

/* VA 0x0049c7a0, 82 bytes, [I] DM-059 created, status draft */
void *zrwModelRestoreFrameIdCB(void *frame, void *data)
{
    RwMatrix *m = RwFrameGetMatrix(frame);
    RpAnimFrameSetID(frame, ((uint32_t)(int32_t)m->at.y << 16) | ((uint32_t)(int32_t)m->at.x & 0xFFFF));
    RwFrameForAllChildren(frame, zrwModelRestoreFrameIdCB, data);
    return frame;
}

/* VA 0x0049c800, 80 bytes, [I] DM-059 created, status draft */
void zrwModelRestoreFrameIds(void *clump)
{
    zrwModelRestoreFrameIdCB(RpClumpGetFrame(clump), NULL);
}

/* VA 0x0049c850, 18 bytes, [I] DM-059 created, status draft */
void *zrwModelFrameSetIdentity(void *frame)
{
    return RwFrameSetIdentity(frame);
}

/* VA 0x0049c870, 15 bytes, [I] DM-059 created, status draft */
void zrwModelClumpSetIdentity(void *clump)
{
    RwFrameSetIdentity(RpClumpGetFrame(clump));
}

/* VA 0x0049c880, 237 bytes, [V] direct, status draft
 * Clump owner plugin = the model; then every ZGL material of the model is linked to the RpMaterial
 * of the LOD-0 geometry that uses the same RwTexture (zrwMatSetRpMaterial). */
void zrwModelSetRwInfo(ZglModel *model)
{
    void *geom = model->def->geometry0;
    ZglMaterial *mat;

    zrwModelSetClumpOwner(model->clump, model);
    if (model->colClump != NULL)
        zrwModelSetClumpOwner(model->colClump, model);
    for (mat = (ZglMaterial *)zHeapListFirst(model->materials); mat != NULL;
         mat = (ZglMaterial *)zHeapListNext(model->materials)) {
        ZglTexture *tex = g_zglMaterialApi.getTexture(mat, 0);
        void *rwTex = zrwTxrGetRwTexture(tex);
        const char *name = g_texDriverVtbl.getName(tex);
        int i;
        for (i = RpGeometryGetNumMaterials(geom) - 1; i >= 0; i--) {
            void *rpMat = RpGeometryGetMaterial(geom, i);
            if (RpMaterialGetTexture(rpMat) == rwTex) {
                zrwMatSetRpMaterial(mat, rpMat);
                break;
            }
        }
        if (i < 0)
            g_pfnDebugPrintf("zrwModelSetRwInfo: Failed to match RpMaterial:%s\n", name);
    }
}

/* VA 0x0049c970, 76 bytes, [I] callers, status draft (atomic statistics callback) */
void *zrwModelPrintAtomicCB(void *atomic, int32_t *totals)
{
    void *geom = *(void **)((uint8_t *)atomic + 0x1C);
    totals[0] += RpGeometryGetNumTriangles(geom);
    totals[1] += RpGeometryGetNumVertices(geom);
    g_pfnDebugPrintf(" [F:%d, V:%d]", RpGeometryGetNumTriangles(geom), RpGeometryGetNumVertices(geom));
    return atomic;
}

/* VA 0x0049c9c0, 296 bytes, [I] callers, status draft (statistics of a loaded clump) */
void zrwModelPrintClump(void *clump)
{
    void *atomic = NULL;
    void *geom;
    int32_t totals[2];
    uint32_t flags;

    RpClumpForAllAtomics(clump, (void *)zrwModelFirstAtomicCB, &atomic);
    geom = *(void **)((uint8_t *)atomic + 0x1C);
    totals[0] = totals[1] = 0;
    g_pfnDebugPrintf("Num atomics:%d (", RpClumpGetNumAtomics(clump));
    RpClumpForAllAtomics(clump, (void *)zrwModelPrintAtomicCB, totals);
    g_pfnDebugPrintf(" )\n");
    g_pfnDebugPrintf("Num triangles:%d\n", totals[0]);
    g_pfnDebugPrintf("Num vertices :%d\n", totals[1]);
    g_pfnDebugPrintf("Flags (Atomic0):");
    flags = RpGeometryGetFlags(geom);
    if (flags & 0x04) g_pfnDebugPrintf("rpGEOMETRYTEXTURED ");
    if (flags & 0x08) g_pfnDebugPrintf("rpGEOMETRYPRELIT ");
    if (flags & 0x10) g_pfnDebugPrintf("rpGEOMETRYNORMALS ");
    if (flags & 0x20) g_pfnDebugPrintf("rpGEOMETRYLIGHT ");
    if (flags & 0x01) g_pfnDebugPrintf("rpGEOMETRYTRISTRIP ");
    g_pfnDebugPrintf("\n");
}

static void zrwModelPrintFrameId(ZglModel *model, uint32_t id)
{
    ZglModelDefEntry *e = zrwModelFindDefEntry(model, id);
    if (e == NULL)
        g_pfnDebugPrintf("0x%x\n", id);
    else
        g_pfnDebugPrintf("0x%x (%s)\n", id, e->name);
}

/* VA 0x0049caf0, 103 bytes, [I] callers, status draft (RwFrameForAllChildren callback) */
void *zrwModelPrintFrameCB(void *frame, ZglModel *model)
{
    zrwModelPrintFrameId(model, RpAnimFrameGetID(frame));
    g_pfnDebugIndent(1);
    RwFrameForAllChildren(frame, (void *)zrwModelPrintFrameCB, model);
    g_pfnDebugIndent(-1);
    return frame;
}

/* VA 0x0049cb60, 130 bytes, [I] callers, status draft (the frame tree with MDEF names) */
void zrwModelPrintHierarchy(ZglModel *model)
{
    void *root = RpClumpGetFrame(model->clump);
    g_pfnDebugPrintf("HIERARCHY:-\n");
    g_pfnDebugIndent(1);
    zrwModelPrintFrameId(model, RpAnimFrameGetID(root));
    g_pfnDebugIndent(1);
    RwFrameForAllChildren(root, (void *)zrwModelPrintFrameCB, model);
    g_pfnDebugIndent(-1);
    g_pfnDebugIndent(-1);
}

/* VA 0x0049cbf0, 97 bytes, [I] DM-059 created, status draft */
void zrwModelPrintMaterials(ZglModel *model)
{
    ZglMaterial *mat;
    g_pfnDebugPrintf("MATERIALS:-\n");
    g_pfnDebugIndent(1);
    for (mat = (ZglMaterial *)zHeapListFirst(model->materials); mat != NULL;
         mat = (ZglMaterial *)zHeapListNext(model->materials))
        g_pfnDebugPrintf("%s\n", g_texDriverVtbl.getName(g_zglMaterialApi.getTexture(mat, 0)));
    g_pfnDebugIndent(-1);
}

/* VA 0x0049cc60, 13 bytes, [I] DM-059 created, status draft (stops at the first atomic) */
void *zrwModelFirstAtomicCB(void *atomic, void **out)
{
    *out = atomic;
    return NULL;
}

/* VA 0x0049cc70, 37 bytes, [I] callers, status draft */
void *zrwModelGetFirstAtomic(void *clump)
{
    void *atomic = NULL;
    RpClumpForAllAtomics(clump, (void *)zrwModelFirstAtomicCB, &atomic);
    return atomic;
}

/* VA 0x0049cca0, 14 bytes, [I] DM-059 created, status draft */
void *zrwModelCountAtomicCB(void *atomic, int32_t *count)
{
    (*count)++;
    return atomic;
}

/* VA 0x0049ccb0, 37 bytes, [I] DM-059 created, status draft (no caller found) */
int32_t zrwModelCountAtomics(void *clump)
{
    int32_t n = 0;
    RpClumpForAllAtomics(clump, (void *)zrwModelCountAtomicCB, &n);
    return n;
}

static void zrwModelInitPart(ZglModelPart *p, void *frame, ZglModelPart *parent)
{
    RwMatrix *m = RwFrameGetMatrix(frame);
    p->id = RpAnimFrameGetID(frame);
    p->node = NULL;
    p->frame = frame;
    p->bindPos = m->pos;
    p->parent = parent;
}

/* VA 0x0049cce0, 110 bytes, [I] DM-059 created, status draft
 * RwFrameForAllChildren callback: the next part of the table for this frame, linked under its parent. */
void *zrwModelAddPartCB(void *frame, ZglModelPart *parent)
{
    ZglModelPart *p = s_zrwMc.nextPart++;
    zrwModelInitPart(p, frame, parent);
    if (parent != NULL) {
        p->next = parent->child;
        parent->child = p;
    }
    RwFrameForAllChildren(frame, (void *)zrwModelAddPartCB, p);
    return frame;
}

/* VA 0x0049cd50, 142 bytes, [I] DM-059 created, status draft (inlined in its callers)
 * One part per frame of the model's clump, root first (count = the frame hierarchy size). */
ZglModelPart *zrwModelBuildParts(ZglModel *model, int lod)
{
    void *root = RpClumpGetFrame(model->clump);
    int32_t n = RwFrameCount(root);
    ZglModelPart *parts;

    model->lod[lod].numParts = n;
    parts = (ZglModelPart *)zstringMemSet(zmemAlloc(n * sizeof(ZglModelPart)), 0, n * sizeof(ZglModelPart));
    s_zrwMc.nextPart = parts + 1;
    zrwModelInitPart(parts, root, NULL);
    RwFrameForAllChildren(root, (void *)zrwModelAddPartCB, parts);
    return parts;
}

/* VA 0x0049cde0, 141 bytes, [I] DM-059 created, status draft (inlined in zrwModelChunkMODL) */
ZglModel *zrwModelAlloc(void)
{
    ZglModel *m = (ZglModel *)zmemFreeListAlloc(s_zrwMc.pool);
    zstringMemSet(m, 0, sizeof(ZglModel));
    m->type &= ~0xFFu;
    m->def = (ZglModelDef *)zmemAlloc(sizeof(ZglModelDef));
    zstringMemSet(m->def, 0, sizeof(ZglModelDef));
    m->def->refCount = 1;
    m->materials = zHeapListCreate(0x10);
    zquatIdentity((Zquat *)m->rot);
    m->scale.x = m->scale.y = m->scale.z = 1.0f;
    m->colour.red = m->colour.green = m->colour.blue = m->colour.alpha = 0xFF;
    return m;
}

/* VA 0x0049ce70, 21 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwModelFree(ZglModel *model)
{
    zmemFreeListFree(s_zrwMc.pool, model);
}

/* VA 0x0049ce90, 637 bytes, [V] direct, status draft (model driver slot 6)
 * Another instance of a loaded model: shares the def and the materials (references taken), clones
 * the clumps (frame ids carried over, see above) and builds its own part tables. Skinned models
 * (MDEF flag 2) cannot be cloned and are loaded again from def->path. */
ZglModel *zrwModelCreateInstance(ZglModel *src)
{
    ZglModel *m;
    ZglMaterial *mat;
    int l;

    if (src->def->mdefFlags & 2) {
        g_pfnDebugPrintf("zrwModelCreateInstance: Not instancing skinned model - ReLoading\n");
        return (ZglModel *)g_zglModelApi.loadFile(src->def->path, 0);
    }
    m = (ZglModel *)zmemFreeListAlloc(s_zrwMc.pool);
    zstringMemCopy(m, src, sizeof(ZglModel));
    m->def->refCount++;
    zrwModelStashFrameIdCB(RpClumpGetFrame(src->clump), NULL);
    m->clump = RpClumpClone(src->clump);
    zrwModelRestoreFrameIdCB(RpClumpGetFrame(m->clump), NULL);
    RwFrameSetIdentity(RpClumpGetFrame(m->clump));
    RwFrameSetIdentity(RpClumpGetFrame(src->clump));
    if (src->colClump != NULL)
        m->colClump = RpClumpClone(src->colClump);
    zrwModelSetRwInfo(m);
    for (l = 0; l < m->def->numLods; l++)
        m->lod[l].parts = zrwModelBuildParts(m, l);   /* every LOD gets the LOD-0 hierarchy [V] */
    m->materials = zHeapListCreate(0x10);
    for (mat = (ZglMaterial *)zHeapListFirst(src->materials); mat != NULL;
         mat = (ZglMaterial *)zHeapListNext(src->materials)) {
        zHeapListAdd(m->materials, mat);
        g_zglMaterialApi.addRef(mat);
    }
    zrwModelAddToWorld(m, 1);
    if ((m->def->mdefFlags & 0x40) == 0)
        zrwModelSetCollision(m, 1);
    return m;
}

/* VA 0x0049d110, 6 bytes, [I] DM-059 created, status draft (unreferenced; returns 1) */
int zrwModelCreateNop(void)
{
    return 1;
}

/* VA 0x0049d120, 558 bytes, [I] callers, status draft
 * Reads the RW clump stream of an MDFF chunk from memory (*buf = { data, size }, freed with the RW
 * allocator afterwards). isCol: the MCOL clump. Otherwise it becomes LOD def->numLods; LOD 0 is the
 * model's clump (first atomic / geometry / render callback kept in the def). MDEF flag 8 clears
 * rpGEOMETRYLIGHT; flags 0xC0 clear the collision test, else it is set. */
int zrwModelLoadClump(ZglModel *model, void **buf, const char *imagePath, int isCol)
{
    void *clump = NULL;
    void *stream;
    int n;

    if (imagePath != NULL)
        RwImageSetPath(imagePath);
    stream = RwStreamOpen(RWSTREAM_MEMORY, RWSTREAM_READ, buf);
    if (stream == NULL)
        return 0;
    if (RwStreamFindChunk(stream, RWID_CLUMP, NULL, NULL))
        clump = RpClumpStreamRead(stream);
    RwStreamClose(stream, NULL);
    ((void (__cdecl *)(void *))RwOsGetMemoryInterface()[1])(*buf);   /* rwfree */
    if (clump == NULL)
        return 0;
    if (isCol == 1) {
        model->colClump = clump;
        return 1;
    }
    n = model->def->numLods;
    s_zrwMc.lodClump[n] = clump;
    if (n == 0) {
        void *atomic;
        model->clump = clump;
        RwFrameSetIdentity(RpClumpGetFrame(clump));
        atomic = zrwModelGetFirstAtomic(clump);
        model->def->atomic0 = atomic;
        model->def->renderCB = *(int (__cdecl **)(void *))((uint8_t *)atomic + 0x4C);
        model->def->geometry0 = *(void **)((uint8_t *)atomic + 0x1C);
    }
    model->lod[n].parts = zrwModelBuildParts(model, n);
    model->def->numLods++;
    if (model->def->mdefFlags & 8)
        zrwModelSetGeometryFlags(clump, 0x20, 0);
    if (model->def->mdefFlags & 0xC0)
        zrwModelSetAtomicFlags(clump, 1, 0);
    if ((model->def->mdefFlags & 0xC0) == 0)
        zrwModelSetAtomicFlags(clump, 0, 1);
    zrwModelPrintClump(clump);
    return 1;
}

static int zrwModelReadMdffChunk(ZiffFile *iff, ZglModel *model, int isCol, int line)
{
    struct { void *data; uint32_t size; } mem;
    mem.size = ziffChunkGetSize(iff);
    mem.data = ((void *(__cdecl *)(uint32_t))RwOsGetMemoryInterface()[0])(mem.size);   /* rwmalloc */
    ziffChunkReadData(iff, mem.data, mem.size);
    if (!zrwModelLoadClump(model, &mem.data, NULL, isCol)) {
        zAssertFail("Clump failed to load", ZRWMC_SRC, line, 0);
        g_pfnDebugPrintf("Clump failed to load\n");
        return 0;
    }
    return 1;
}

/* VA 0x0049d350, 303 bytes, [I] DM-059 created, status draft (IFF handler 'MLOD')
 * MLDF (8 bytes: word 0 flags, ignored; word 1 = LOD switch distance as a float, 0 -> the default
 * 0/50/100/150), then MDFF. A fifth LOD is ignored. */
int zrwModelChunkMLOD(ZiffFile *iff, ZglModel *model)
{
    uint32_t mldf[2];
    int n = model->def->numLods;

    if (n == 4) {
        g_pfnDebugPrintf("Ignoring extra LOD in %s\n", model->def->name);
        return 0;
    }
    if (ziffChunkOpen(iff)) {
        if (ziffChunkFindFirst(iff, IFF_MLDF)) {
            ziffChunkReadData(iff, mldf, 8);
            *(uint32_t *)&model->lod[n].distance = mldf[1];
            if (model->lod[n].distance == 0.0f)
                model->lod[n].distance = s_lodDistance[n];
        }
        if (ziffChunkFindFirst(iff, IFF_MDFF))
            zrwModelReadMdffChunk(iff, model, 0, 0x3FC);
        ziffChunkClose(iff);
    }
    return 1;
}

/* VA 0x0049d480, 73 bytes, [I] DM-059 created, status draft (inlined copy: every MLOD) */
int zrwModelReadLods(ZiffFile *iff, void *model)
{
    int ok = ziffChunkFindFirst(iff, IFF_MLOD);
    while (ok) {
        if (!ziffChunkProcess(iff, model))
            return 0;
        ok = ziffChunkFindNext(iff, IFF_MLOD);
    }
    return 1;
}

/* VA 0x0049d4d0, 198 bytes, [I] DM-059 created, status draft (IFF handler 'MCOL')
 * The collision clump: rendered never, collision only, always in the world. */
int zrwModelChunkMCOL(ZiffFile *iff, ZglModel *model)
{
    if (ziffChunkOpen(iff)) {
        if (ziffChunkFindFirst(iff, IFF_MDFF)) {
            zrwModelReadMdffChunk(iff, model, 1, 0x441);
            zrwModelSetAtomicFlags(model->colClump, 4 /* render */, 1 /* collision test */);
            RpWorldAddClump(g_zrwWorld, model->colClump);
        }
        ziffChunkClose(iff);
    }
    return 1;
}

/* VA 0x0049d5a0, 65 bytes, [I] DM-059 created, status draft (inlined copy) */
int zrwModelReadMcol(ZiffFile *iff, ZglModel *model)
{
    if ((model->def->mdefFlags & 0x80) && ziffChunkFindFirst(iff, IFF_MCOL))
        if (!ziffChunkProcess(iff, model))
            return 0;
    return 1;
}

/* VA 0x0049d5f0, 342 bytes, [I] callers, status draft
 * MDEF: header (kind -> mdefFlags, name, name id; params unused) and count x { name[16], u32 id }. */
int zrwModelReadMDEF(ZiffFile *iff, ZglModel *model)
{
    ZrwMdefHeader h;
    ZglModelDef *d = model->def;
    int i;

    if (!ziffChunkFindFirst(iff, IFF_MDEF)) {
        zAssertFail("Missing model def block", ZRWMC_SRC, 0x495, 0);
        return 0;
    }
    ziffChunkReadData(iff, &h, sizeof(h));
    zstringCopyN(d->name, h.name, 0x10);
    d->nameId = h.nameId;
    d->mdefFlags = h.kind;
    d->numEntries = h.count;
    d->entries = (ZglModelDefEntry *)zmemAlloc(h.count * sizeof(ZglModelDefEntry));
    for (i = 0; i < d->numEntries; i++) {
        ziffChunkReadData(iff, d->entries[i].name, 0x10);
        ziffChunkReadLong(iff, (int32_t *)&d->entries[i].id);
        d->entries[i].name[15] = 0;
    }
    g_pfnDebugPrintf("-------------------------------------------------\n");
    g_pfnDebugPrintf("Loading model:%s\n", d->name);
    g_pfnDebugIndent(1);
    return 1;
}

/* VA 0x0049d750, 27 bytes, [I] DM-059 created, status draft (material creation callback) */
int zrwModelAddMaterialCB(ZglMaterial *mat, void *model)
{
    zHeapListAdd(((ZglModel *)model)->materials, mat);
    return 1;
}

/* VA 0x0049d770, 109 bytes, [I] callers, status draft (every MATI of the MODL) */
int zrwModelReadMaterials(ZiffFile *iff, void *model)
{
    void *old = g_zglMaterialApi.setCreateCallback((void *)zrwModelAddMaterialCB);
    int ok = ziffChunkFindFirst(iff, IFF_MATI);
    while (ok) {
        if (!ziffChunkProcess(iff, model)) {
            g_zglMaterialApi.setCreateCallback(old);
            return 0;
        }
        ok = ziffChunkFindNext(iff, IFF_MATI);
    }
    g_zglMaterialApi.setCreateCallback(old);
    return 1;
}

/* VA 0x0049d895, 306 bytes, [I] row, status draft (the tail of zrwModelChunkMODL, see above) */
int zrwModelChunkMODLBody(ZiffFile *iff, ZglModel *model)
{
    void *old;
    int ok;

    zrwModelReadMDEF(iff, model);
    old = g_zglMaterialApi.setCreateCallback((void *)zrwModelAddMaterialCB);
    for (ok = ziffChunkFindFirst(iff, IFF_MATI); ok && ziffChunkProcess(iff, model);)
        ok = ziffChunkFindNext(iff, IFF_MATI);
    g_zglMaterialApi.setCreateCallback(old);
    for (ok = ziffChunkFindFirst(iff, IFF_MLOD); ok && ziffChunkProcess(iff, model);)
        ok = ziffChunkFindNext(iff, IFF_MLOD);
    if ((model->def->mdefFlags & 0x80) && ziffChunkFindFirst(iff, IFF_MCOL))
        ziffChunkProcess(iff, model);
    if (!ziffChunkClose(iff))
        return 0;
    if (model->def->mdefFlags & 2)
        RpBoneClumpInitialize(model->clump);
    zrwModelApplyRenderMode(model);
    if (model->def->mdefFlags & 0x10)
        zrwModelAddShadowCallback(model);
    else if ((model->def->mdefFlags & 4) == 0)
        RpWorldAddClump(g_zrwWorld, model->clump);
    zrwModelSetRwInfo(model);
    g_zglModelApi.update(model);
    g_pfnDebugIndent(-1);
    s_zrwMc.lastLoaded = model;
    return 1;
}

/* VA 0x0049d7e0, 181 bytes, [I] DM-059 created, status draft (IFF handler 'MODL') */
int zrwModelChunkMODL(ZiffFile *iff)
{
    ZglModel *model;

    s_zrwMc.lastLoaded = NULL;
    model = zrwModelAlloc();
    if (model == NULL)
        return 0;
    if (!ziffChunkOpen(iff))
        return 0;
    return zrwModelChunkMODLBody(iff, model);
}

/* VA 0x0049d9d0, 143 bytes, [V] direct, status draft (called by zrwModelInit) */
int zrwModelCreateInit(void)
{
    if (s_zrwMc.initialized)
        zAssertFail("Already initialized", ZRWMC_SRC, 0x52D, 0);
    zstringMemSet(&s_zrwMc, 0, sizeof(s_zrwMc));
    s_zrwMc.pool = zmemFreeListCreate(sizeof(ZglModel), 0x100, (ZmemFreeListCallback)zrwModelRelease);
    s_zrwMc.initialized = 1;
    ziffRegisterHandler(IFF_MODL, (ZiffHandlerFn)zrwModelChunkMODL);
    ziffRegisterHandler(IFF_MLOD, (ZiffHandlerFn)zrwModelChunkMLOD);
    ziffRegisterHandler(IFF_MCOL, (ZiffHandlerFn)zrwModelChunkMCOL);
    return 1;
}

/* VA 0x0049da60, 39 bytes, [I] callers, status draft */
int zrwModelCreateShutdown(void)
{
    if (s_zrwMc.initialized) {
        zmemFreeListDestroy(s_zrwMc.pool);
        s_zrwMc.initialized = 0;
    }
    return 1;
}

/* VA 0x0049da90, 21 bytes, [I] DM-059 created, status draft (slot 5: the MODL chunk of an open IFF) */
ZglModel *zrwModelLoadIff(ZiffFile *iff, int flags)
{
    (void)flags;
    ziffChunkProcess(iff, NULL);
    return s_zrwMc.lastLoaded;
}

static ZglModel *zrwModelParseModl(uint32_t *buf)
{
    uint32_t size = buf[1] + 8;
    zfileHandle *file;
    ZiffFile *iff;
    ZglModel *model = NULL;

    if (buf[0] != IFF_MODL) {
        zAssertFail("Not a MODL", ZRWMC_SRC, 0x577, 0);
        return NULL;
    }
    file = zfileOpenMemory(buf, size);
    if (file == NULL) {
        zAssertFail("Couldn't open MODL", ZRWMC_SRC, 0x57B, 0);
        return NULL;
    }
    iff = ziffOpen(file, 0xFFFFFFFF, size);
    if (iff == NULL) {
        zAssertFail("Couldn't open MODL", ZRWMC_SRC, 0x57E, 0);
        return NULL;
    }
    if (ziffChunkFindFirst(iff, IFF_MODL)) {
        ziffChunkProcess(iff, NULL);
        model = s_zrwMc.lastLoaded;
    }
    ziffClose(iff);
    zfileClose(file, NULL);
    return model;
}

/* VA 0x0049dab0, 209 bytes, [I] DM-059 created, status draft (slot 2: a whole ZMD in memory) */
ZglModel *zrwModelLoadMemory(uint32_t *buf)
{
    return zrwModelParseModl(buf);
}

/* VA 0x0049db90, 276 bytes, [V] direct, status draft (slot 3: a ZMD file) */
ZglModel *zrwModelLoadFile(const char *path, int flags)
{
    int size;
    uint32_t *buf = (uint32_t *)zfileLoadAll(path, &size);
    ZglModel *model;
    (void)flags;

    if (buf == NULL)
        return NULL;
    model = zrwModelParseModl(buf);
    zfileFreeLoaded(buf);
    if (model != NULL)
        zstringCopyN(model->def->path, path, 0x80);
    return model;
}

/* VA 0x0049dcb0, 28 bytes, [I] DM-059 created, status draft (slot 4: not implemented on the PC) */
int zrwModelSave(void)
{
    zAssertFail("Not coded", ZRWMC_SRC, 0x5BA, 0);
    return 0;
}

/* VA 0x0049dcd0, 357 bytes, [I] gap, status draft (slot 7; also the pool destructor)
 * Out of the world, clumps destroyed, shadow callback removed, materials released (the textures too
 * when this is the last instance), part tables freed; the def is freed with its last instance. */
int zrwModelRelease(ZglModel *model)
{
    ZglMaterial *mat;
    int last, l;

    if (model->clump != NULL) {
        if (RpClumpGetWorld(model->clump) != NULL)
            RpWorldRemoveClump(g_zrwWorld, model->clump);
        RpClumpDestroy(model->clump);
    }
    if (model->colClump != NULL) {
        if (RpClumpGetWorld(model->colClump) != NULL)
            RpWorldRemoveClump(g_zrwWorld, model->colClump);
        RpClumpDestroy(model->colClump);
    }
    if (model->def->mdefFlags & 0x10)
        zrwModelRemoveShadowCallback();
    last = model->def->refCount;
    for (mat = (ZglMaterial *)zHeapListFirst(model->materials); mat != NULL;
         mat = (ZglMaterial *)zHeapListFirst(model->materials)) {
        if (last == 1)
            g_texDriverVtbl.release(g_zglMaterialApi.getTexture(mat, 0));
        g_zglMaterialApi.release(mat);
        zHeapListRemoveCurrent(model->materials);
    }
    zHeapListDestroy(model->materials);
    for (l = 0; l < model->def->numLods; l++)
        zmemFree(model->lod[l].parts);
    if (--model->def->refCount == 0) {
        zmemFree(model->def->entries);
        zmemFree(model->def);
    }
    zmemFreeListFree(s_zrwMc.pool, model);
    return 1;
}

/* VA 0x0049de40, 27 bytes, [I] DM-059 created, status draft (model table +0xA0) */
void zrwModelPrintStats(void)
{
    g_pfnDebugPrintf("ZGLMODEL: Objects in use:%d\n", zmemFreeListGetUsedCount(s_zrwMc.pool));
}

/* VA 0x0049de60, 296 bytes, [I] DM-059 created, status draft (model table +0x9C) */
void zrwModelPrintInfo(ZglModel *model)
{
    g_pfnDebugPrintf("-------------------------------------------------\n");
    g_pfnDebugPrintf("Info for model:%s\n", model->def->name);
    g_pfnDebugPrintf("-------------------------------------------------\n");
    g_pfnDebugIndent(1);
    zrwModelPrintClump(model->clump);
    zrwModelPrintHierarchy(model);
    zrwModelPrintMaterials(model);
    g_pfnDebugIndent(-1);
    g_pfnDebugPrintf("-------------------------------------------------\n");
}

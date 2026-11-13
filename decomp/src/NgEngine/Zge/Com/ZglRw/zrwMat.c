/*
 * NgEngine/Zge/Com/ZglRw/zrwMat.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwMat.c
 * Evidence: [V] path string 0x00561094 (asserts of ChunkMADF 0xF0/0xF6, Init 0x114, SetTexture
 *           0x184, AddTexture 0x1A5, ColourPass 0x1C6, GetTexture 0x27B, GetRaster 0x2DE,
 *           CurrentMadfFlag0 0x315); "RwMat:" / "zrwMat..." / "ZGLMAT:" messages; the material
 *           driver table 0x00561018 (15 slots, all in this file).
 * 25 functions, VA 0x004892B0-0x00489CAC (DM-059 created 8).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 4,
 * docs/formats/TEXTURE.md 2.
 *
 * ZGL materials: up to six texture passes plus the MADF words of a MATI chunk. The IFF handlers
 * 'MATI' and 'MADF' (registered by zrwMat_Init) turn every MATI of a ZMD, Mat._sd or ZAL into a
 * material: MADF is read first (its name goes to zrwTxr), the TBMP/TPNG handler creates the texture
 * and its material (zrwMat_CreateFromTexture), then the MATI handler copies six MADF words into it
 * and calls the creation callback (zrwWorld.c / zrwModel.c collect the materials that way).
 */
#include "decomp.h"
#include "zglrw.h"

#define ZRWMAT_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwMat.c"
#define IFF_MATI 0x4954414Du   /* 'MATI' */
#define IFF_MADF 0x4644414Du   /* 'MADF' */

/* ------------------------------------------------------------------ globals */
static uint8_t s_madfChunk[8 + sizeof(ZrwMadf)]; /* 0x00672318: the chunk header slot + the payload
                                                   * (g_madfBuffer 0x00672320, 0x88 bytes; the size
                                                   * check allows up to 0x8F, which would run into
                                                   * g_zrwMat) */
ZrwMatGlobals g_zrwMat;                          /* 0x006723A8 */

#define MADF(m) ((ZrwMadf *)((m) + 8))

/* ------------------------------------------------------------------ functions */

/* VA 0x004892b0, 254 bytes, [I] row, status draft
 * IFF handler 'MATI': parse the children (MADF, then TBMP / TPNG), then copy the MADF words into the
 * material the image created. Returns the material. */
int zrwMat_ChunkMATI(void *iff, void *ctx)
{
    ZglMaterial *mat;

    if (ziffChunkOpen((ZiffFile *)iff)) {
        g_zrwMat.curMadf = NULL;
        g_zrwMat.lastCreated = NULL;
        ziffParse((ZiffFile *)iff, NULL);
        if (g_zrwMat.curMadf == NULL || g_zrwMat.lastCreated == NULL) {
            g_pfnDebugPrintf("RwMat: problem loading image\n");
            ziffChunkClose((ZiffFile *)iff);
            return 0;
        }
        mat = g_zrwMat.lastCreated;
        mat->madfFlags  = MADF(g_zrwMat.curMadf)->flags;
        mat->madfWord08 = MADF(g_zrwMat.curMadf)->word08;
        mat->madfWord0c = MADF(g_zrwMat.curMadf)->word0c;
        mat->surface    = MADF(g_zrwMat.curMadf)->surface;
        mat->anim1      = MADF(g_zrwMat.curMadf)->anim1;
        mat->anim2      = MADF(g_zrwMat.curMadf)->anim2;
        mat->unk20 = 0;
        ziffChunkClose((ZiffFile *)iff);
    }
    mat = g_zrwMat.lastCreated;
    if (g_zrwMat.createCallback != NULL)
        g_zrwMat.createCallback(mat, ctx);
    return (int)(size_t)mat;
}

/* VA 0x004893b0, 133 bytes, [V] direct, status draft
 * IFF handler 'MADF': read the payload (< 0x90 bytes, nameOffset 0x48) and pass the name on. */
int zrwMat_ChunkMADF(void *iff)
{
    if (ziffChunkGetSize((ZiffFile *)iff) >= 0x90) {
        zAssertFail("Data too big", ZRWMAT_SRC, 0xF0, 0);
        return 0;
    }
    ziffChunkRead((ZiffFile *)iff, MADF(s_madfChunk));
    if (MADF(s_madfChunk)->nameOffset != 0x48) {
        zAssertFail("MADF Mismatch", ZRWMAT_SRC, 0xF6, 0);
        return 0;
    }
    g_zrwMat.curMadf = s_madfChunk;
    zrwTxr_SetPendingName(MADF(s_madfChunk)->name);
    return 1;
}

/* VA 0x00489440, 133 bytes, [V] direct, status draft (material driver slot 0) */
int zrwMat_Init(void)
{
    if (g_zrwMat.initialized)
        zAssertFail("Already initialized", ZRWMAT_SRC, 0x114, 0);
    zstringMemSet(&g_zrwMat, 0, sizeof(g_zrwMat));
    g_zrwMat.initialized = 1;
    g_zrwMat.pool = zmemFreeListCreate(sizeof(ZglMaterial), 0x100,
                                       (ZmemFreeListCallback)zrwMatRelease);
    g_zrwMat.unk04 = 0;
    ziffRegisterHandler(IFF_MATI, (ZiffHandlerFn)zrwMat_ChunkMATI);
    ziffRegisterHandler(IFF_MADF, (ZiffHandlerFn)zrwMat_ChunkMADF);
    return 1;
}

/* VA 0x004894d0, 39 bytes, [I] DM-059 created, status draft (slot 1) */
int zrwMatShutdown(void)
{
    if (g_zrwMat.initialized) {
        zmemFreeListDestroy(g_zrwMat.pool);
        g_zrwMat.initialized = 0;
    }
    return 1;
}

static ZglMaterial *zrwMatNewObject(void)
{
    ZglMaterial *mat = (ZglMaterial *)zmemFreeListAlloc(g_zrwMat.pool);
    zstringMemSet(mat, 0, sizeof(ZglMaterial));
    mat->refCount = 1;
    mat->type = (mat->type & ~0xFFu) | ZGL_TYPE_MATERIAL;
    return mat;
}

/* VA 0x00489500, 47 bytes, [I] DM-059 created, status draft (slot 2) */
ZglMaterial *zrwMatCreate(void)
{
    return zrwMatNewObject();
}

/* VA 0x00489530, 81 bytes, [I] callers, status draft (slot 3; also the free-list destructor)
 * Releases the textures of every pass; frees the material when its count reaches 0. */
void zrwMatRelease(ZglMaterial *mat)
{
    int i;
    for (i = 0; i < mat->numPasses; i++)
        g_texDriverVtbl.release(mat->pass[i].texture);
    if (--mat->refCount == 0) {
        mat->numPasses = 0;
        zmemFreeListFree(g_zrwMat.pool, mat);
    }
}

/* VA 0x00489590, 120 bytes, [V] direct, status draft (slot 5) */
void zrwMatSetTexture(ZglMaterial *mat, ZglTexture *tex, uint32_t pass)
{
    if (pass >= mat->numPasses) {
        zAssertFail("No texture has been added for this render-pass", ZRWMAT_SRC, 0x184, 0);
        return;
    }
    if (mat->pass[pass].texture != NULL)
        g_texDriverVtbl.release(mat->pass[pass].texture);
    mat->pass[pass].texture = tex;
    g_texDriverVtbl.addRef(tex);
    if (mat->rpMaterial != NULL)
        RpMaterialSetTexture(mat->rpMaterial, zrwTxrGetRwTexture(tex));
}

/* VA 0x00489610, 103 bytes, [I] DM-059 created, status draft (slot 4) */
void zrwMatAddTexture(ZglMaterial *mat, ZglTexture *tex)
{
    if (mat->numPasses > 5) {
        zAssertFail("Too many textures added", ZRWMAT_SRC, 0x1A5, 0);
        return;
    }
    mat->pass[mat->numPasses].texture = tex;
    mat->numPasses++;
    g_texDriverVtbl.addRef(tex);
    if (mat->rpMaterial != NULL)
        RpMaterialSetTexture(mat->rpMaterial, zrwTxrGetRwTexture(tex));
}

/* VA 0x00489680, 51 bytes, [I] DM-059 created, status draft (slot 6; the pass is ignored) */
void zrwMatSetColour(ZglMaterial *mat, int pass, const RwRGBA *colour)
{
    (void)pass;
    if (mat->rpMaterial == NULL) {
        zAssertFail("pMat->pRwMat not set", ZRWMAT_SRC, 0x1C6, 0);
        return;
    }
    RpMaterialSetColor(mat->rpMaterial, colour);
}

/* VA 0x004896c0, 16 bytes, [I] callers, status draft (slot 11; returns the old callback) */
void *zrwMatSetCreateCallback(void *fn)
{
    void *old = (void *)g_zrwMat.createCallback;
    g_zrwMat.createCallback = (int (__cdecl *)(ZglMaterial *, void *))fn;
    return old;
}

/* a new material holding one texture; the texture's owner is the material */
static ZglMaterial *zrwMatWrapTexture(ZglTexture *tex)
{
    ZglMaterial *mat = zrwMatNewObject();
    if (mat->numPasses < 6) {
        mat->pass[mat->numPasses].texture = tex;
        mat->numPasses++;
        g_texDriverVtbl.addRef(tex);
        if (mat->rpMaterial != NULL)
            RpMaterialSetTexture(mat->rpMaterial, zrwTxrGetRwTexture(tex));
    } else
        zAssertFail("Too many textures added", ZRWMAT_SRC, 0x1A5, 0);
    zrwTxrSetOwner(tex, mat);
    g_zrwMat.lastCreated = mat;
    return mat;
}

/* VA 0x004896d0, 231 bytes, [V] direct, status draft (slot 12)
 * A material from a complete image file in memory (NgIFzres overlay pictures, zroller). */
ZglMaterial *zrwMatCreateFromImage(void *buf, int len, const char *name, int type)
{
    ZglTexture *tex;
    ZglMaterial *mat;

    g_zrwMat.lastCreated = NULL;
    tex = g_texDriverVtbl.createFromMemory(buf, len, name, type, 0);
    if (tex == NULL) {
        g_pfnDebugPrintf("zrwMatCreateFromImage: Error loading:%s\n", name);
        return NULL;
    }
    mat = zrwMatWrapTexture(tex);
    g_texDriverVtbl.release(tex);          /* the material holds the reference */
    return mat;
}

/* VA 0x004897c0, 228 bytes, [I] DM-059 created, status draft (no direct caller found) */
ZglMaterial *zrwMatCreateProcedural(int width, int height, int depth, const char *name)
{
    ZglTexture *tex;
    ZglMaterial *mat;

    g_zrwMat.lastCreated = NULL;
    tex = zrwTxrCreateProceduralTexture(width, height, depth, name);
    if (tex == NULL) {
        g_pfnDebugPrintf("zrwMatCreateProcedural: Error Creating:%s\n", name);
        return NULL;
    }
    mat = zrwMatWrapTexture(tex);
    g_texDriverVtbl.release(tex);
    return mat;
}

/* VA 0x004898b0, 230 bytes, [I] DM-059 created, status draft (no direct caller found) */
ZglMaterial *zrwMatCreatePaletteFromMemory(const uint8_t *pixels, int width, int height,
                                           const RwRGBA *palette, const char *name)
{
    ZglTexture *tex;
    ZglMaterial *mat;

    g_zrwMat.lastCreated = NULL;
    tex = zrwTxrLoadPalettedImageFromMemory(pixels, width, height, palette, name);
    if (tex == NULL) {
        g_pfnDebugPrintf("zrwMatCreatePaletteFromMemory: Error\n");
        return NULL;
    }
    mat = zrwMatWrapTexture(tex);
    g_texDriverVtbl.release(tex);
    return mat;
}

/* VA 0x004899a0, 87 bytes, [V] direct, status draft
 * zmemFreeListForEach callback of FindByTextureHash: args = { hash, index, result }. */
int zrwMatFindByTextureHashCB(ZglMaterial *mat, void *args)
{
    int32_t *a = (int32_t *)args;
    ZglTexture *tex;

    if (mat->numPasses == 0) {
        zAssertFail("Out of range", ZRWMAT_SRC, 0x27B, 0);
        tex = NULL;
    } else
        tex = mat->pass[0].texture;
    if (g_texDriverVtbl.getNameHash(tex) == (uint32_t)a[0]) {
        if (a[1]-- == 0) {
            a[2] = (int32_t)(size_t)mat;
            return 0;                       /* stop */
        }
    }
    return 1;
}

/* VA 0x00489a00, 60 bytes, [I] callers, status draft (slot 13)
 * The index-th material whose first texture has this name hash (NULL if none). */
ZglMaterial *zrwMatFindByTextureHash(uint32_t hash, int index)
{
    g_zrwMat.findHash = hash;
    g_zrwMat.findIndex = index;
    g_zrwMat.findResult = NULL;
    zmemFreeListForEach(g_zrwMat.pool, (ZmemFreeListIterFn)zrwMatFindByTextureHashCB, &g_zrwMat.findHash);
    return g_zrwMat.findResult;
}

/* VA 0x00489a40, 54 bytes, [V] direct, status draft (slot 7) */
ZglTexture *zrwMatGetTexture(ZglMaterial *mat, uint32_t pass)
{
    if (pass < mat->numPasses)
        return mat->pass[pass].texture;
    zAssertFail("Out of range", ZRWMAT_SRC, 0x27B, 0);
    return NULL;
}

/* VA 0x00489a80, 112 bytes, [I] callers, status draft (slot 9)
 * A copy of the material (all fields incl. the reference count); its textures get a reference. */
ZglMaterial *zrwMatCopy(ZglMaterial *mat)
{
    ZglMaterial *copy = zrwMatNewObject();
    int i;

    if (copy == NULL)
        return NULL;
    zstringMemCopy(copy, mat, sizeof(ZglMaterial));
    for (i = 0; i < mat->numPasses; i++)
        g_texDriverVtbl.addRef(g_zglMaterialApi.getTexture(copy, i));
    return copy;
}

/* VA 0x00489af0, 52 bytes, [I] callers, status draft (slot 10) */
ZglMaterial *zrwMatAddRef(ZglMaterial *mat)
{
    int i;
    for (i = 0; i < mat->numPasses; i++)
        g_texDriverVtbl.addRef(g_zglMaterialApi.getTexture(mat, i));
    mat->refCount++;
    return mat;
}

/* VA 0x00489b30, 8 bytes, [I] callers, status draft (slot 8: the MADF surface / animation words) */
uint32_t *zrwMatGetSurface(ZglMaterial *mat)
{
    return &mat->surface;
}

/* VA 0x00489b40, 63 bytes, [V] direct, status draft */
void *zrwMatGetRaster(ZglMaterial *mat, uint32_t pass)
{
    if (pass < mat->numPasses)
        return zrwTxrGetRaster(mat->pass[pass].texture);
    zAssertFail("Out of range", ZRWMAT_SRC, 0x2DE, 0);
    return NULL;
}

/* VA 0x00489b80, 12 bytes, [I] callers, status draft (zrwWorld.c / zrwModel.c link an RpMaterial) */
void zrwMatSetRpMaterial(ZglMaterial *mat, void *rpMaterial)
{
    mat->rpMaterial = rpMaterial;
}

/* VA 0x00489b90, 8 bytes, [I] DM-059 created, status draft (no caller found) */
void *zrwMatGetRpMaterial(ZglMaterial *mat)
{
    return mat->rpMaterial;
}

/* VA 0x00489ba0, 56 bytes, [V] direct, status draft
 * Bit 0 of MADF +0x04 of the MADF being loaded. zrwTxr_ChunkTBMP calls it and ignores the result. */
int zrwMat_CurrentMadfFlag0(void)
{
    if (g_zrwMat.curMadf == NULL)
        zAssertFail("Not loadedMADF", ZRWMAT_SRC, 0x315, 0);
    return (MADF(g_zrwMat.curMadf)->flags & 1) != 0;
}

/* VA 0x00489be0, 173 bytes, [I] row, status draft
 * The TBMP/TPNG handlers: a material for the texture just created (it becomes lastCreated, which
 * the MATI handler completes). The texture's creation reference is kept. */
ZglMaterial *zrwMat_CreateFromTexture(ZglTexture *tex)
{
    return zrwMatWrapTexture(tex);
}

/* VA 0x00489c90, 27 bytes, [I] DM-059 created, status draft (slot 14) */
void zrwMatPrintStats(void)
{
    g_pfnDebugPrintf("ZGLMAT: Objects in use:%d\n", zmemFreeListGetUsedCount(g_zrwMat.pool));
}

const ZglMaterialApi g_zglMaterialApi = {                                    /* 0x00561018 */
    zrwMat_Init, zrwMatShutdown, zrwMatCreate, zrwMatRelease, zrwMatAddTexture, zrwMatSetTexture,
    zrwMatSetColour, zrwMatGetTexture, zrwMatGetSurface, zrwMatCopy, zrwMatAddRef,
    zrwMatSetCreateCallback, zrwMatCreateFromImage, zrwMatFindByTextureHash, zrwMatPrintStats
};

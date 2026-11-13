/*
 * NgEngine/Zge/Com/ZglRw/zrwWorld.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwWorld.c
 * Evidence: [V] path string referenced by zrwWorldFindScfi/LoadMaterials/Init/Open; the rest [I]: the
 * globals 0x006741B8-0x0067428B, the API table g_zrwWorldApi 0x005620E0 (11 slots, all in this
 * range). The zScene pass moved here: 0x00495DF0-0x00496240 (from the zobjPEmit/zrwWorld gap) and
 * 0x00497200-0x00497620 (the skeleton had them in zrwTxr.c by the weak "split" pass).
 * 33 functions, VA 0x00495df0-0x00497777 (the ZglRw pass added the two zfx affectors 0x00497640-0x00497777:
 * their tables g_zfxAffectorGravity 0x005623D8 / g_zfxAffectorFloorColl 0x005623E0 and defaults
 * 0x005623C0 / 0x005623D0 lie in this file's data, before zrwTxr.c's 0x005623E8).
 * Draft. Non-matching C (AD-8). Types: decomp/include/zrwworld.h.
 * Formats: docs/formats/LEVEL.md 3 (ZSD), 4 (BSP._sd); TEXTURE.md (Mat._sd MATI).
 *
 * The world is a RenderWare RpWorld kept in zrwZgl.c's globals; RenderWare calls are library code
 * and appear under their RW 3.x SDK names [I: by use] with their VA.
 */
#include "decomp.h"
#include "zmath.h"

/* ---- RenderWare 3.1 (library code, names [I] from their use here) */
typedef struct RpWorld RpWorld;
typedef struct RwStream RwStream;
typedef struct RwBBox { float sup[3]; float inf[3]; } RwBBox;
RwStream *RwStreamOpen(int type, int access, const void *data);        /* 0x004E23E0 (2 = file, 1 read / 2 write) */
void     *RwStreamFindChunk(RwStream *s, uint32_t type, uint32_t *len, uint32_t *ver); /* 0x004E2EC0 */
int       RwStreamClose(RwStream *s, void *data);                       /* 0x004E2660 */
RpWorld  *RpWorldStreamRead(RwStream *s);                               /* 0x004C3E60 */
RpWorld  *RpWorldStreamWrite(RpWorld *w, RwStream *s);                  /* 0x004C3AE0 */
RpWorld  *RpWorldCreate(RwBBox *box);                                   /* 0x004BB080 */
int       RpWorldDestroy(RpWorld *w);                                   /* 0x004BA9C0 */
float    *RpWorldGetOrigin(RpWorld *w);                                 /* 0x004BA050 */
RwBBox   *RpWorldGetBBox(RpWorld *w);                                   /* 0x004B9FE0 */
int       RpWorldGetNumMaterials(RpWorld *w);                           /* 0x004BA0C0 */
void     *RpWorldGetMaterial(RpWorld *w, int i);                        /* 0x004BA140 */
int       RpWorldGetNumClumps(RpWorld *w);                              /* 0x004BA1F0 */
void      RpWorldForAllLights(RpWorld *w, void *(__cdecl *fn)(void *, void *), void *data);   /* 0x004BBA60 */
void      RpWorldForAllWorldSectors(RpWorld *w, void *(__cdecl *fn)(void *, void *), void *d);/* 0x004BBB80 */
void      RpWorldForAllClumps(RpWorld *w, void *(__cdecl *fn)(void *, void *), void *data);   /* 0x004BB7D0 */
int       RpWorldRender(RpWorld *w);                                    /* 0x004BA460 */
void     *RpMaterialGetTexture(void *mat);                              /* 0x004B93D0 */
char     *RwTextureGetName(void *tex);                                  /* 0x004DF990 */
int       RpWorldSectorGetNumPolygons(void *sector);                    /* 0x004C2060 */
int       RpWorldSectorGetNumVertices(void *sector);                    /* 0x004C2050 */
void      RpClumpForAllAtomics(void *clump, void *(__cdecl *fn)(void *, void *), void *data);  /* 0x004B1990 */
int       RpGeometryGetNumTriangles(void *geom);                        /* 0x004B4720 */
int       RpGeometryGetNumVertices(void *geom);                         /* 0x004B4550 */
uint32_t  RpLightGetType(void *light);                                  /* 0x004C2BF0 */
float    *RpLightGetColor(void *light);                                 /* 0x004C2560 */
uint32_t  RpLightGetFlags(void *light);                                 /* 0x004C2C20 */
float     RpLightGetRadius(void *light);                                /* 0x004C2520 */
float     RpLightGetConeAngle(void *light);                             /* 0x004C25D0 */
int       RpPVSQuery(RpWorld *w);                                       /* 0x004CC640 world has PVS data */
void      RpPVSDestroy(RpWorld *w);                                     /* 0x004CC320 */
int       RpPVSGetProgressCallBack(RpWorld *w);                         /* 0x004CEF40 */
void      RpPVSSetProgressCallBack(RpWorld *w, int (__cdecl *fn)(int, float)); /* 0x004CC0F0 */
int       RpPVSConstruct(RpWorld *w, void *raster, void *zraster, float, float, void *cb, void *d); /* 0x004CE960 */
void      RwEngineSetSomething(uint32_t value);                         /* 0x004DC6E0 called with 0x200000 [I] */
void     *RwCameraGetRaster(void *cam);                                 /* 0x004DE5A0 */
void     *RwCameraGetZRaster(void *cam);                                /* 0x004DE5D0 */
void      RwCameraClear(void *cam, const void *colour, int mode);       /* 0x004DE8A0 */
extern char RpPVSGeneric[];                                            /* 0x004CD510 sample callback */

/* ---- other modules (current names until they are decompiled) */
void    *zrwCamGetRwCamera(void *cam);                     /* the RwCamera of a zcam */
void    *zrwCamGetFirst(void);                       /* the current camera */
int      zrwCamBeginUpdate(void *cam);                  /* begin update */
int      zrwCamEndUpdate(void);                       /* end update */
void     zrwZglShowFrame(int);                        /* show the frame */
void     zrwDbgDrawText(void);                       /* debug text */
int      zrwZglSetValue(int what, void *value, void *arg); /* zgl setter; 2 = world bounds (ZGE) */
int      zrwZglRunCallbacks(int what);
/* zrwMat.c, through its table at 0x00561024.. */
void    *zrwMatSetCreateCallback(int (__cdecl *addCallback)(void *mat));   /* sets the MATI-created callback, returns the old one */
void    *zrwMatGetTexture(void *mat, int i);           /* the material's texture */
void     zrwMatRelease(void *mat);                  /* destroy */
void     zrwMatSetRpMaterial(void *mat, void *rpMaterial);/* attach to an RpMaterial */
uint32_t zrwTxrGetRwTexture(void *tex);                  /* the RwTexture of a zrwTxr texture */
extern void *(__cdecl *g_texDriverVtbl[])(void *);    /* [4] destroy, [8] name, [9] name hash */
extern void (__cdecl *g_pfnDebugIndent)(int delta);   /* 0x00560078 */
/* zrwZgl.c globals */
extern RpWorld *g_zrwWorld;                           /* 0x006976C8 */
extern float    g_zrwWorldOrigin[3];                  /* 0x006976CC (= RpWorldGetOrigin) */
extern float    g_zrwWorldOriginScaled[3];            /* 0x006976D8 origin * scale */
extern int      g_zrwPVSBuilt;                        /* 0x006976E4 */
extern int      g_zrwPVSEnabled;                      /* 0x006976E8 */
extern float    g_zglInvScale;                        /* 0x00697720 1 / scale */
extern float    g_zglScale;                           /* 0x00697724 100.0 */
extern RwRGBA   g_zglClearColour;                     /* 0x00697730 (Globals OBJI) */

#define ZRWWORLD_FILE "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwWorld.c"
#define ID_SCEN 0x4E454353u   /* 'SCEN' */
#define ID_SCFI 0x49464353u   /* 'SCFI' */
#define RW_ID_WORLD 0x0B
#define ZGL_WORLD_BOUNDS 2

ZsdScfiRecord   g_zrwWorldScfi;     /* 0x006741B8 */
ZrwWorldGlobals g_zrwWorldState;    /* 0x006741E0 */

static const char *s_lightTypeNames[7] = {   /* 0x0056210C */
    "rpLIGHTDIRECTIONAL", "rpLIGHTAMBIENT", "rpLIGHTPOINT", "rpLIGHTSPOT", "rpLIGHTSPOTSOFT",
    "rpNALIGHTTYPE", "ERROR"
};
static const char *s_pathNames[1] = { "PATH" };   /* 0x00562128/0x0056212C zfileMakeFileName keys */

const ZrwWorldApi g_zrwWorldApi = {           /* 0x005620E0 */
    zrwWorldOpen, zrwWorldClose, zrwWorldFindScfi, zrwWorldFindMaterial, zrwWorldGetNumMaterials,
    zrwWorldGetMaterial, (void *(__cdecl *)(void))zrwWorldPrintInfo /* returns ZrwWorldStats * */, zrwWorldHasPVS, zrwWorldCreatePVS, zrwWorldEnablePVS,
    zrwWorldSavePVS
};

/* ------------------------------------------------------------------ statistics */

/* VA 0x00495df0, 52 bytes, [I] row, status draft */
void *zrwWorldCountSectorCB(void *sector, void *data)
{
    g_zrwWorldState.stats.sectorPolys += RpWorldSectorGetNumPolygons(sector);
    g_zrwWorldState.stats.sectorVerts += RpWorldSectorGetNumVertices(sector);
    return sector;
}

/* VA 0x00495e30, 57 bytes, [I] row, status draft */
void *zrwWorldCountAtomicCB(void *atomic, void *data)
{
    void *geom = ((void **)atomic)[0x1C / 4];   /* RpAtomic.geometry */
    g_zrwWorldState.stats.clumpPolys += RpGeometryGetNumTriangles(geom);
    g_zrwWorldState.stats.clumpVerts += RpGeometryGetNumVertices(geom);
    return atomic;
}

/* VA 0x00495e70, 28 bytes, [I] row, status draft */
void *zrwWorldCountClumpCB(void *clump, void *data)
{
    RpClumpForAllAtomics(clump, zrwWorldCountAtomicCB, data);
    return clump;
}

/* the geometry half of zrwWorldPrintInfo (also inlined in it and in zrwWorldLoad) */
static void zrwWorldPrintCounts(void)
{
    zstringMemSet(&g_zrwWorldState.stats, 0, sizeof(g_zrwWorldState.stats));
    RpWorldForAllWorldSectors(g_zrwWorld, zrwWorldCountSectorCB, 0);
    g_pfnDebugPrintf("- STATIC GEOM\n");
    g_pfnDebugIndent(1);
    g_pfnDebugPrintf("Number of polygons:%d\n", g_zrwWorldState.stats.sectorPolys);
    g_pfnDebugPrintf("Number of vertices:%d\n", g_zrwWorldState.stats.sectorVerts);
    g_pfnDebugIndent(-1);
    RpWorldForAllClumps(g_zrwWorld, zrwWorldCountClumpCB, 0);
    g_pfnDebugPrintf("- CLUMPS (Props etc)\n");
    g_pfnDebugIndent(1);
    g_pfnDebugPrintf("Number of clumps  :%d\n", RpWorldGetNumClumps(g_zrwWorld));
    g_pfnDebugPrintf("Number of polygons:%d\n", g_zrwWorldState.stats.clumpPolys);
    g_pfnDebugPrintf("Number of vertices:%d\n", g_zrwWorldState.stats.clumpVerts);
    g_pfnDebugIndent(-1);
}

/* VA 0x00495e90, 206 bytes, [I] row, status draft */
void zrwWorldPrintGeometry(void)
{
    zrwWorldPrintCounts();
}

/* VA 0x00495f60, 445 bytes, [I] row, status draft */
ZrwWorldStats *zrwWorldPrintInfo(void)
{
    /* g_zrwWorldApi.printInfo; also inlined at the end of zrwWorldLoad. Needs FixupZrwWorldPrintInfo.java on a
     * Ghidra project from before the zScene pass (the auto-analysis disassembled it from 0x00495F70). */
    int n, i;

    g_pfnDebugPrintf("--------------------------------------------------------------\n");
    g_pfnDebugPrintf("- SCENE INFO FOR:%s\n", g_zrwWorldState.path);
    g_pfnDebugPrintf("--------------------------------------------------------------\n");
    g_pfnDebugIndent(1);
    g_zrwWorldState.stats.lightIndex = 0;
    g_pfnDebugPrintf("- LIGHTS\n");
    g_pfnDebugIndent(1);
    RpWorldForAllLights(g_zrwWorld, zrwWorldPrintLightCB, 0);
    g_pfnDebugIndent(-1);
    g_pfnDebugPrintf("- MATERIALS\n");
    g_pfnDebugIndent(1);
    n = RpWorldGetNumMaterials(g_zrwWorld);
    for (i = 0; i < n; i++) {
        void *mat = RpWorldGetMaterial(g_zrwWorld, i);
        void *tex = mat ? RpMaterialGetTexture(mat) : 0;
        if (tex)
            g_pfnDebugPrintf("%3d: %s\n", i, RwTextureGetName(tex));
    }
    g_pfnDebugIndent(-1);
    zrwWorldPrintCounts();
    g_pfnDebugIndent(-1);
    g_pfnDebugPrintf("--------------------------------------------------------------\n");
    return &g_zrwWorldState.stats;
}

/* VA 0x00496120, 288 bytes, [I] row, status draft */
void *zrwWorldPrintLightCB(void *light, void *data)
{
    uint32_t type = RpLightGetType(light);
    uint32_t flags;
    float *rgb;

    if (type > 6)
        type = 6;     /* [V] the clamped value is used below, so the 0x80.. tests never hold */
    g_pfnDebugPrintf("Light:%3d: %s\n", g_zrwWorldState.stats.lightIndex, s_lightTypeNames[type]);
    g_pfnDebugIndent(1);
    rgb = RpLightGetColor(light);
    g_pfnDebugPrintf("R:%f G:%f B:%f\n", (double)rgb[0], (double)rgb[1], (double)rgb[2]);
    flags = RpLightGetFlags(light);
    if (flags) {
        if (flags & 1)
            g_pfnDebugPrintf("LightAtomics ");
        if (flags & 2)
            g_pfnDebugPrintf("LightWorld ");
        g_pfnDebugPrintf("\n");
    }
    if (type == 0x80 || type == 0x81 || type == 0x82)
        g_pfnDebugPrintf("Radius:%f\n", (double)RpLightGetRadius(light));
    if (type == 0x81 || type == 0x82)
        g_pfnDebugPrintf("ConeAngle:%f\n", (double)RpLightGetConeAngle(light));
    g_pfnDebugIndent(-1);
    g_zrwWorldState.stats.lightIndex++;
    return light;
}

/* VA 0x00496240, 154 bytes, [I] row, status draft */
void zrwWorldSetZglBounds(void)
{
    /* the world box in ZGE units: (p - origin) * scale with y and z negated (LEVEL.md 2); the
     * first corner is RW (inf.x, sup.y, sup.z), the second (sup.x, inf.y, inf.z). Inlined 4 times. */
    RwBBox *b = RpWorldGetBBox(g_zrwWorld);
    float box[6];

    box[0] = (b->inf[0] - g_zrwWorldOrigin[0]) * g_zglScale;
    box[1] = -((b->sup[1] - g_zrwWorldOrigin[1]) * g_zglScale);
    box[2] = -((b->sup[2] - g_zrwWorldOrigin[2]) * g_zglScale);
    box[3] = (b->sup[0] - g_zrwWorldOrigin[0]) * g_zglScale;
    box[4] = -((b->inf[1] - g_zrwWorldOrigin[1]) * g_zglScale);
    box[5] = -((b->inf[2] - g_zrwWorldOrigin[2]) * g_zglScale);
    zrwZglSetValue(ZGL_WORLD_BOUNDS, box, 0);
}

/* ------------------------------------------------------------------ the ZSD index */

/* VA 0x004962e0, 224 bytes, [V] direct, status draft */
ZsdScfiRecord *zrwWorldFindScfi(void *zsd, int type, uint32_t *size)
{
    /*
     * LEVEL.md 3: opens the ZSD buffer as an IFF with root SCEN (its length from the SCEN header at
     * zsd+4) and returns a static copy of the first SCFI whose type byte matches; SCDF and the
     * junk bytes are never read. A SCFI that is not 0x28 bytes asserts "Chunk out of sync".
     */
    zfileHandle *file = zfileOpenMemory(zsd, ((int *)zsd)[1]);
    ZiffFile *iff;
    int more;

    if (!file)
        return 0;
    iff = ziffOpen(file, ID_SCEN, 0);
    if (iff) {
        for (more = ziffChunkFindFirst(iff, ID_SCFI); more; more = ziffChunkFindNext(iff, ID_SCFI)) {
            if (ziffChunkGetSize(iff) != sizeof(ZsdScfiRecord)) {
                zAssertFail("Chunk out of sync", ZRWWORLD_FILE, 0x1D0, 0);
                continue;
            }
            ziffChunkRead(iff, &g_zrwWorldScfi);
            if ((char)g_zrwWorldScfi.type == (char)type) {
                ziffClose(iff);
                zfileClose(file, 0);
                if (size)
                    *size = g_zrwWorldScfi.size;
                return &g_zrwWorldScfi;
            }
        }
        ziffClose(iff);
    }
    zfileClose(file, 0);
    return 0;
}

/* ------------------------------------------------------------------ materials (Mat._sd) */

/* VA 0x004963c0, 34 bytes, [I] row, status draft */
int zrwWorldAddMaterialCB(void *material)
{
    /* zrwMat calls this for every material the MATI handlers create while Mat._sd is parsed */
    zrwZglRunCallbacks(6);
    zHeapListAdd(g_zrwWorldState.materials, material);
    return 1;
}

/* VA 0x004963f0, 246 bytes, [V] direct, status draft */
int zrwWorldLoadMaterials(void *zsd, const char *dir)
{
    /*
     * Mat._sd (SCFI 1): a headerless IFF of MATI forms parsed with the globally registered zrwMat
     * handlers; its SCFI size is the IFF length (bytes past it are not read). Size 0 = no textures.
     * Inlined in zrwWorldOpen.
     */
    uint32_t size;
    ZsdScfiRecord *scfi = g_zrwWorldApi.findScfi(zsd, ZSD_TYPE_MAT, &size);
    char path[0x80];
    zfileHandle *file;
    void *old;
    ZiffFile *iff;

    if (!scfi)
        return 0;
    if (size == 0)
        return 1;
    zstringPrintf(path, "%s%s", dir, scfi->name);
    file = zfileOpen(path, 1, 0);
    if (!file) {
        zAssertFail(zstringFormat("Couldn't open:%s", path), ZRWWORLD_FILE, 0x221, 0);
        return 0;
    }
    old = zrwMatSetCreateCallback(zrwWorldAddMaterialCB);
    iff = ziffOpen(file, ZIFF_ANY_ID, size);
    if (iff) {
        ziffParse(iff, 0);
        ziffClose(iff);
    }
    zrwMatSetCreateCallback((int (__cdecl *)(void *))old);
    zfileClose(file, 0);
    return 1;
}

/* VA 0x004964f0, 198 bytes, [V] direct, status draft */
void zrwWorldHookRpMaterials(void)
{
    /*
     * Attaches every loaded MATI material to the world's RpMaterial that uses the same RwTexture
     * (searched from the last world material down). The texture match is by RwTexture pointer: the
     * MATI texture and the BSP material texture are the same RW dictionary entry by name. Prints
     * "Failed to match RpMaterial" for a MATI that no world material uses. Inlined in zrwWorldOpen.
     */
    void *mat;

    if (!g_zrwWorldState.materials)
        return;
    for (mat = zHeapListFirst(g_zrwWorldState.materials); mat; mat = zHeapListNext(g_zrwWorldState.materials)) {
        void *txr = zrwMatGetTexture(mat, 0);
        uint32_t rwTex = zrwTxrGetRwTexture(txr);
        void *name = g_texDriverVtbl[8](txr);
        int i = RpWorldGetNumMaterials(g_zrwWorld) - 1;

        for (; i >= 0; i--) {
            void *rpMat = RpWorldGetMaterial(g_zrwWorld, i);
            if (rpMat && (uint32_t)RpMaterialGetTexture(rpMat) == rwTex) {
                zrwMatSetRpMaterial(mat, rpMat);
                break;
            }
        }
        if (i < 0)
            g_pfnDebugPrintf("zrwWorldHookRpMaterials: Failed to match RpMaterial:%s\n", name);
    }
}

/* VA 0x004965c0, 87 bytes, [I] row, status draft */
int zrwWorldFreeMaterials(void)
{
    void *mat;

    if (g_zrwWorldState.materials) {
        while ((mat = zHeapListFirst(g_zrwWorldState.materials)) != 0) {
            g_texDriverVtbl[4](zrwMatGetTexture(mat, 0));   /* release the texture */
            zrwMatRelease(mat);
            zHeapListRemoveCurrent(g_zrwWorldState.materials);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ PVS */

/* VA 0x00496620, 134 bytes, [I] row, status draft */
int zrwWorldPVSProgressCB(int message, float percent)
{
    /* RpPVS progress callback: on message 12 (progress) shows "Percent complete" on screen */
    void *cam;

    if (message == 12 && (cam = zrwCamGetFirst()) != 0) {
        RwCameraClear(zrwCamGetRwCamera(cam), &g_zglClearColour, 1);
        if (zrwCamBeginUpdate(cam)) {
            zstringFormat("Percent complete: %f\n", (double)percent);
            zrwDbgDrawText();
            if (!zrwCamEndUpdate())
                return 1;
        }
        zrwZglShowFrame(1);
        g_pfnDebugPrintf("Percent complete: %f\n", (double)percent);
    }
    return 1;
}

/* VA 0x004966b0, 46 bytes, [I] row, status draft */
void zrwWorldDestroyPVS(void)
{
    if (RpPVSQuery(g_zrwWorld)) {
        RpPVSDestroy(g_zrwWorld);
        g_zrwPVSBuilt = 0;
        g_zrwPVSEnabled = 0;
    }
}

/* ------------------------------------------------------------------ the world (BSP._sd) */

/* sets the origin globals and the zgl bounds for a new world (inlined 4 times) */
static void zrwWorldSetCurrent(RpWorld *world)
{
    float *o;

    g_zrwWorld = world;
    o = RpWorldGetOrigin(world);
    g_zrwWorldOrigin[0] = o[0];
    g_zrwWorldOrigin[1] = o[1];
    g_zrwWorldOriginScaled[0] = g_zglScale * g_zrwWorldOrigin[0];
    g_zrwWorldOrigin[2] = o[2];
    g_zrwWorldOriginScaled[1] = g_zrwWorldOrigin[1] * g_zglScale;
    g_zrwWorldOriginScaled[2] = g_zrwWorldOrigin[2] * g_zglScale;
    zrwWorldSetZglBounds();
}

/* VA 0x004966e0, 959 bytes, [I] row, status draft */
int zrwWorldLoad(void *zsd, const char *dir)
{
    /*
     * BSP._sd (SCFI 4): the SCFI *size is not used* (NULL size pointer); the file is opened by name
     * through a RenderWare file stream, the first World chunk (0x0B) is read with RpWorldStreamRead
     * and replaces the current world. Then the scene info is printed (zrwWorldPrintInfo inlined).
     */
    ZsdScfiRecord *scfi = g_zrwWorldApi.findScfi(zsd, ZSD_TYPE_BSP, 0);
    char path[0x80];
    const char *values[1];
    char *file;
    RwStream *s;
    RpWorld *world;

    if (!scfi)
        return 0;
    zstringPrintf(path, "%s%s", dir, scfi->name);
    values[0] = path;
    file = zfileMakeFileName(zfileGetDefaultDevice(), 1, s_pathNames, (void **)values);
    if (!file)
        return 0;
    zstringCopy(g_zrwWorldState.path, file);
    s = RwStreamOpen(2, 1, file);
    if (!s)
        return 0;
    if (RwStreamFindChunk(s, RW_ID_WORLD, 0, 0)) {
        world = RpWorldStreamRead(s);
        RwStreamClose(s, 0);
        if (world) {
            if (g_zrwWorld)
                RpWorldDestroy(g_zrwWorld);
            zrwWorldSetCurrent(world);
            if (RpPVSQuery(g_zrwWorld)) {    /* the BSP carries PVS data */
                g_zrwPVSBuilt = 1;
                g_zrwPVSEnabled = 1;
            }
            zrwWorldPrintInfo();             /* inlined */
            return 1;
        }
    }
    RwStreamClose(s, 0);             /* [V] closed a second time when RpWorldStreamRead failed */
    return 0;
}

/* builds the 200 x 200 x 200 placeholder world (+-100 RW units around the origin) */
static int zrwWorldCreateDefault(void)
{
    RwBBox box;
    RpWorld *world;

    box.sup[0] = 100.0f; box.sup[1] = 100.0f; box.sup[2] = 100.0f;
    box.inf[0] = -100.0f; box.inf[1] = -100.0f; box.inf[2] = -100.0f;
    world = RpWorldCreate(&box);
    if (!world)
        return 0;
    zrwWorldSetCurrent(world);
    return 1;
}

/* VA 0x00496aa0, 409 bytes, [I] row, status draft */
int zrwWorldCreateEmpty(void)
{
    /* drops the current world (and its PVS) and installs the empty placeholder world */
    zrwWorldDestroyPVS();
    if (g_zrwWorld) {
        RpWorldDestroy(g_zrwWorld);
        g_zrwWorld = 0;
    }
    return zrwWorldCreateDefault();
}

/* VA 0x00496c40, 412 bytes, [V] direct, status draft */
int zrwWorldInit(void)
{
    if (g_zrwWorldState.initialized)
        zAssertFail("Already initialized", ZRWWORLD_FILE, 0x348, 0);
    zstringMemSet(&g_zrwWorldState, 0, sizeof(g_zrwWorldState));
    g_zrwWorldState.materials = zHeapListCreate(0x100);
    if (!zrwWorldCreateDefault())
        return 0;
    RwEngineSetSomething(0x200000);
    g_zrwWorldState.initialized = 1;
    return 1;
}

/* VA 0x00496de0, 526 bytes, [I] row, status draft */
int zrwWorldShutdown(void)
{
    if (g_zrwWorldState.initialized) {
        zrwWorldClose();             /* inlined */
        zHeapListDestroy(g_zrwWorldState.materials);
        g_zrwWorldState.initialized = 0;
    }
    return 1;
}

/* VA 0x00496ff0, 513 bytes, [V] direct, status draft */
int zrwWorldOpen(void *zsd, const char *dir)
{
    /*
     * g_zrwWorldApi.open, called by zsceneOpen: Mat._sd, then BSP._sd, then the material hook-up
     * (LoadMaterials and HookRpMaterials inlined). The skeleton named this function
     * zrwWorldHookRpMaterials after the message it prints; that name now belongs to 0x004964F0.
     */
    if (g_zrwWorldState.isOpen) {
        zAssertFail("There is already a world open", ZRWWORLD_FILE, 899, 0);
        return 0;
    }
    if (!zrwWorldLoadMaterials(zsd, dir))
        return 0;
    if (!zrwWorldLoad(zsd, dir))
        return 0;
    zrwWorldHookRpMaterials();
    g_zrwWorldState.isOpen = 1;
    return 1;
}

/* VA 0x00497200, 506 bytes, [I] row, status draft */
int zrwWorldClose(void)
{
    /* g_zrwWorldApi.close (zsceneClose): back to the placeholder world, materials released */
    if (!g_zrwWorldState.isOpen)
        return 1;
    if (!zrwWorldCreateEmpty())      /* inlined */
        return 0;
    zrwWorldFreeMaterials();         /* inlined */
    g_zrwWorldState.isOpen = 0;
    return 1;
}

/* VA 0x00497400, 80 bytes, [I] row, status draft */
void *zrwWorldFindMaterial(uint32_t textureHash)
{
    /* the loaded MATI material whose texture name hashes to textureHash (zfxInitTexAnims) */
    void *mat;

    for (mat = zHeapListFirst(g_zrwWorldState.materials); mat; mat = zHeapListNext(g_zrwWorldState.materials)) {
        if ((uint32_t)g_texDriverVtbl[9](zrwMatGetTexture(mat, 0)) == textureHash)
            return mat;
    }
    return 0;
}

/* VA 0x00497450, 15 bytes, [I] row, status draft */
int zrwWorldGetNumMaterials(void)
{
    return zHeapListCount(g_zrwWorldState.materials);
}

/* VA 0x00497460, 21 bytes, [I] row, status draft */
void *zrwWorldGetMaterial(int index)
{
    return zHeapListGet(g_zrwWorldState.materials, index);
}

/* VA 0x00497480, 21 bytes, [I] row, status draft */
int zrwWorldHasPVS(void)
{
    return RpPVSQuery(g_zrwWorld) != 0;
}

/* VA 0x004974a0, 16 bytes, [I] row, status draft */
void zrwWorldCreatePVS(void)
{
    /* the 16 bytes at 0x004974A0 get the camera and fall through into 0x004974B0, which Ghidra
     * split off as its own function; the C is the whole function */
    void *cam = zrwCamGetFirst();
    void *rwCam;

    if (!cam)
        return;
    rwCam = zrwCamGetRwCamera(cam);
    g_zrwWorldState.stats.pvsOldCallback = RpPVSGetProgressCallBack(g_zrwWorld);
    RpPVSSetProgressCallBack(g_zrwWorld, zrwWorldPVSProgressCB);
    zrwWorldDestroyPVS();            /* inlined */
    if (RpPVSConstruct(g_zrwWorld, RwCameraGetRaster(rwCam), RwCameraGetZRaster(rwCam),
                       *(float *)((char *)cam + 0xB0), 0.0f, RpPVSGeneric, 0)) {
        g_zrwPVSBuilt = 1;
        g_zrwPVSEnabled = 1;
    }
}

/* VA 0x004974b0, 177 bytes, [I] row, status draft */
void zrwWorldCreatePVSBody(void)
{
    /* not a real entry point: the tail of zrwWorldCreatePVS (camera in EDI), see there */
    zrwWorldCreatePVS();
}

/* VA 0x00497570, 61 bytes, [I] row, status draft */
int zrwWorldEnablePVS(int on)
{
    if (g_zrwPVSBuilt) {
        g_zrwPVSEnabled = on;
        return 1;
    }
    if (!on) {
        g_zrwPVSEnabled = 0;
        return 1;
    }
    g_pfnDebugPrintf("ZRWWORLD: No PVS data\n");
    return 0;
}

/* VA 0x004975b0, 104 bytes, [I] row, status draft */
int zrwWorldSavePVS(void)
{
    /* writes the world (with its PVS) back to the file it was loaded from */
    const char *values[1];
    char *file;
    RwStream *s;
    RpWorld *ok;

    values[0] = g_zrwWorldState.path;
    file = zfileMakeFileName(zfileGetDefaultDevice(), 1, s_pathNames, (void **)values);
    if (!file)
        return 0;
    s = RwStreamOpen(2, 2, file);
    if (!s)
        return 0;
    ok = RpWorldStreamWrite(g_zrwWorld, s);
    RwStreamClose(s, 0);
    return ok != 0;
}

/* VA 0x00497620, 24 bytes, [I] row, status draft */
int zrwWorldRender(void)
{
    /* called by zrwZgl_0048B690 (the frame render) */
    if (g_zrwWorld)
        RpWorldRender(g_zrwWorld);
    return 1;
}

/* ------------------------------------------------------------------ zfx affectors
 * zfx affectors = { init, apply } pairs registered by name (NgIFfx.c). apply(enabled, system, params):
 * system word 0 = the zHeapList of particles, word 10 = the system's start time (ms); a particle has
 * its position at +0x04 and its velocity at +0x24. */
extern uint32_t (__cdecl *g_zglGetTime)(void);       /* 0x00561480 (timeGetTime) */
static const float s_gravityDefault[3] = { 0.0f, 9.81f, 0.0f };   /* 0x005623C0 */
static const float s_floorDefault[2] = { 0.0f, 0.8f };            /* 0x005623D0 floor y, bounce */

/* VA 0x00497640, 1 bytes, [I] new function, status draft */
void zfxGravityInit(void)
{
    /* g_zfxAffectorGravity[0] */
}

/* VA 0x00497650, 152 bytes, [I] new function, status draft */
void zfxGravityApply(int enabled, uint32_t *system, const float *g)
{
    /* (g_zfxAffectorGravity[1]) velocity += g * (now - start) / 1000 for every particle (sic: the whole elapsed time). */
    float t = (float)(g_zglGetTime() - system[10]);
    RwV3d dv;
    uint8_t *p;

    if (g == NULL)
        g = s_gravityDefault;
    dv.x = t * g[0] * 0.001f;
    dv.y = t * g[1] * 0.001f;
    dv.z = t * g[2] * 0.001f;
    if (enabled == 0 || system == NULL)
        return;
    for (p = (uint8_t *)zHeapListFirst((zHeapList *)(size_t)system[0]); p != NULL;
         p = (uint8_t *)zHeapListNext((zHeapList *)(size_t)system[0]))
        zmathVecAdd((RwV3d *)(p + 0x24), (RwV3d *)(p + 0x24), &dv);
}

/* VA 0x004976f0, 1 bytes, [I] new function, status draft */
void zfxFloorCollInit(void)
{
    /* g_zfxAffectorFloorColl[0] */
}

/* VA 0x00497700, 114 bytes, [I] new function, status draft */
void zfxFloorCollApply(int enabled, uint32_t *system, const float *params)
{
    /* (g_zfxAffectorFloorColl[1]) A particle at or below the floor (y >= floor, y down) is put on it and bounces: vy = -vy * k. */
    float floorY, bounce;
    uint8_t *p;

    if (params == NULL)
        params = s_floorDefault;
    floorY = params[0];
    bounce = params[1];
    if (enabled == 0 || system == NULL)
        return;
    for (p = (uint8_t *)zHeapListFirst((zHeapList *)(size_t)system[0]); p != NULL;
         p = (uint8_t *)zHeapListNext((zHeapList *)(size_t)system[0]))
        if (floorY <= *(float *)(p + 8)) {
            *(float *)(p + 8) = floorY;
            *(float *)(p + 0x28) = -(bounce * *(float *)(p + 0x28));
        }
}

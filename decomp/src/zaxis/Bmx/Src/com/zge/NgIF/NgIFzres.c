/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFzres.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFzres.c
 * Evidence: [V] path string 0x0055DA8C: the asserts of zresLoadAsset (0x65), zresLoadMapTable
 *           (0x95, 0x96), zresLoadPlayerTable (0x117, 0x118), zresGetPlayer (0x135),
 *           zresGetBikeEntry (0x199), zresOpenLibraries (0x1E9 .. 0x201) and
 *           zresAddMaterialToLoadList (0x28C .. 0x2B0); [I] the rest by link order and globals.
 * 19 functions, VA 0x0046b7c0-0x0046c207 (DM-057 created 8 functions; the three after
 * zresFreeImages that the module map gave this file - zrollerStartup / Init / Shutdown - print
 * "ZROLLER:" and use NgIFzroller's globals, so they moved to NgIFzroller.c).
 * DM-057 (draft). Non-matching C (AD-8). Types / prototypes: ngif.h. Description: docs/re/ngif.md.
 *
 * "zres": the game's fixed resources. The asset libraries (MiscData, Tables, Replay, Material
 * .ZAL), two IFF tables read once - the map table (Tables.ZAL asset 0: FORM { ZMHD count, 100;
 * ZMAP count x 100 bytes }, a level's resource group) and the player table (MiscData.ZAL group 1:
 * FORM { PLHD count, 0x18; PLYR count x 0x18 } = PLYR records, OVERLAY.md / hud.md) - and the
 * overlay images (Material.ZAL / MiscData.ZAL pictures made into ZGL materials in the PSX image
 * slots g_psxTextures).
 */
#include "decomp.h"
#include "ngif.h"

#define NGIFZRES_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFzres.c"

#define IFF_FORM 0x4D524F46u   /* 'FORM' */
#define IFF_ZMHD 0x44484D5Au   /* 'ZMHD' map table header */
#define IFF_ZMAP 0x50414D5Au   /* 'ZMAP' */
#define IFF_PLHD 0x44484C50u   /* 'PLHD' player table header */
#define IFF_PLYR 0x52594C50u   /* 'PLYR' */

/* ------------------------------------------------------------------ other modules */
void zasset_DumpLibraryInfo(ZassetLib *lib);                           /* 0x00480450 */
extern void *(*g_texDriverVtbl[15])(void *);                           /* 0x005623F0; [5] width, [6] height */
extern ZassetLib *g_materialZal;                                       /* 0x0067848C */
extern ZassetLib *g_miscDataZal;                                       /* 0x00678490 */
extern ZassetLib *g_replayZal;                                         /* 0x0068FFA8 */
extern ZmemFreeList *g_zbspFacePool;                                   /* 0x005F34AC NgIFzbsp.c */
extern ZmemFreeList *g_zbspEdgePool;                                   /* 0x005F34B0 */

/* ------------------------------------------------------------------ the tables (data) */
/* zresAddMaterialToLoadList's resources: 11 overlay pictures 0x30..0x3A = Material.ZAL (library
 * slot 0) assets 9..0x13 [V] 0x0055D978 */
const ZresImageDef g_zresImages[11] = {                                 /* 0x0055D978 */
    { 0x30, 0, 0x09, 0x08000000 }, { 0x31, 0, 0x0A, 0x10800000 }, { 0x32, 0, 0x0B, 0 },
    { 0x33, 0, 0x0C, 0 },          { 0x34, 0, 0x0D, 0x08000000 }, { 0x35, 0, 0x0E, 0x10800000 },
    { 0x36, 0, 0x0F, 0 },          { 0x37, 0, 0x10, 0 },          { 0x38, 0, 0x11, 0 },
    { 0x39, 0, 0x12, 0 },          { 0x3A, 0, 0x13, 0 }
};
/* 0x0055D8C8: 44 bike entries (u16 a, u16 b), e.g. (0x46, 0x0C) [V]; meaning [I] */
extern const uint32_t g_zresBikes[44];

/* ------------------------------------------------------------------ globals (this file) */
static uint8_t  s_map[100];             /* 0x005F8F60 the last zresGetMap record */
static uint8_t  s_bufferA[0x1000];      /* 0x005F8FC4 FIXED_DEFS_SIZE */
static ZassetLib *s_libs[3];            /* 0x005F9FC4 image libraries: [0] Material, [1] MiscData, [2] (none) */
static void    *s_mapData;              /* 0x005F9FD0 */
static void    *s_playerData;           /* 0x005F9FD8 */
static ZresPlayerHeader s_playerHeader; /* 0x005F9FE8 count, entry size 0x18 */
static zfileHandle *s_mapFile;          /* 0x005F9FF0 */
static ZiffFile *s_playerIff;           /* 0x005F9FF4 */
static uint8_t  s_bufferB[0x1000];      /* 0x005F9FF8 */
static ZiffFile *s_mapIff;              /* 0x005FAFF8 */
static int      s_overlayCount;         /* 0x005FAFFC "Overlay%d" material names */
static uint8_t  s_player[0x18];         /* 0x005FB000 the last zresGetPlayer record */
static zfileHandle *s_playerFile;       /* 0x005FB018 */
static ZresMapHeader s_mapHeader;        /* 0x005FB028 count, entry size 100 */
static int      s_imagesCleared;        /* 0x005FB030 */
ZassetLib      *g_tablesZal;            /* 0x0067847C */

/* ------------------------------------------------------------------ functions */

/* VA 0x0046b7c0, 188 bytes, [V] direct, status draft */
int zresLoadMapTable(void)
{
    int size;

    if (s_mapData == NULL) {
        s_mapData = zresLoadAsset(g_tablesZal, 0, 0, (uint32_t *)&size, 0);
        s_mapFile = zfileOpenMemory(s_mapData, size);
        s_mapIff = ziffOpen(s_mapFile, IFF_FORM, 0);
        ziffChunkFindFirst(s_mapIff, IFF_ZMHD);
        ziffChunkReadData(s_mapIff, &s_mapHeader, 8);
        if (ziffChunkGetSize(s_mapIff) != 8)
            zAssertFail("Out of sync map data", NGIFZRES_SRC, 0x95, 0);
        if (s_mapHeader.entrySize != 100)
            zAssertFail("Out of sync map data", NGIFZRES_SRC, 0x96, 0);
    }
    return 0;
}

/* VA 0x0046b8b0, 128 bytes, [V] direct, status draft */
void *zresLoadAsset(ZassetLib *lib, uint32_t group, uint32_t asset, uint32_t *size, int second)
{
    /* one asset into one of the two fixed 4 KB buffers */
    uint32_t n;
    uint8_t *dest;

    if (zassetFindAsset(lib, group, asset) == 0) {
        zasset_DumpLibraryInfo(lib);
        return NULL;
    }
    n = zassetGetAssetSize(lib);
    dest = second ? s_bufferA : s_bufferB;
    if (n > 0xFFF)
        zAssertFail("Memory overflow in zres()!!! Increase FIXED_DEFS_SIZE.", NGIFZRES_SRC, 0x65, 0);
    zassetLoadAsset(lib, dest);
    if (size != NULL)
        *size = n;
    return dest;
}

/* VA 0x0046b950, 6 bytes, [I] between, status draft */
ZassetLib *zresGetLibrary2(void)
{
    return s_libs[2];       /* unreferenced; never set */
}

/* VA 0x0046b970, 6 bytes, [I] between, status draft */
uint32_t zresGetMapCount(void)
{
    return s_mapHeader.count;   /* unreferenced */
}

/* VA 0x0046b990, 84 bytes, [I] between, status draft */
const uint8_t *zresGetMap(uint32_t index)
{
    /* the index-th ZMAP record (100 bytes; out of range -> 0); game.c: the level's resource group */
    if (index >= s_mapHeader.count)
        index = 0;
    ziffChunkFindFirst(s_mapIff, IFF_ZMAP);
    ziffSeek(s_mapIff, 1, (int32_t)(index * 100));
    ziffChunkReadData(s_mapIff, s_map, 100);
    return s_map;
}

/* VA 0x0046ba10, 189 bytes, [V] direct, status draft */
int zresLoadPlayerTable(void)
{
    int size;

    if (s_playerData == NULL) {
        s_playerData = zresLoadAsset(g_miscDataZal, 1, 0, (uint32_t *)&size, 1);
        s_playerFile = zfileOpenMemory(s_playerData, size);
        s_playerIff = ziffOpen(s_playerFile, IFF_FORM, 0);
        ziffChunkFindFirst(s_playerIff, IFF_PLHD);
        ziffChunkReadData(s_playerIff, &s_playerHeader, 8);
        if (ziffChunkGetSize(s_playerIff) != 8)
            zAssertFail("Out of sync player data", NGIFZRES_SRC, 0x117, 0);
        if (s_playerHeader.entrySize != 0x18)
            zAssertFail("Out of sync player data", NGIFZRES_SRC, 0x118, 0);
    }
    return 0;
}

/* VA 0x0046bb00, 114 bytes, [V] direct, status draft */
const uint8_t *zresGetPlayer(uint16_t index)
{
    /* the index-th PLYR record (0x18 bytes, rider * 5 + style; name at +8) - overlay.c */
    if (index < s_playerHeader.count) {
        ziffChunkFindFirst(s_playerIff, IFF_PLYR);
        ziffSeek(s_playerIff, 1, (int32_t)index * 0x18);
        ziffChunkReadData(s_playerIff, s_player, 0x18);
        return s_player;
    }
    zAssertFail("Invalid player", NGIFZRES_SRC, 0x135, 0);
    return NULL;
}

/* VA 0x0046bba0, 11 bytes, [I] between, status draft */
uint32_t zresGetPlayerCount(void)
{
    return s_playerHeader.count;   /* unreferenced */
}

/* VA 0x0046bbc0, 6 bytes, [I] between, status draft */
void *zresGetPlayerHeaderArea(void)
{
    return (uint8_t *)&s_playerHeader - 8;   /* unreferenced: 0x005F9FE0 */
}

/* VA 0x0046bbe0, 44 bytes, [I] between, status draft */
const uint32_t *zresGetBikeEntry(uint32_t bike)
{
    if (bike > 0x2B)
        zAssertFail("bike number out of range", NGIFZRES_SRC, 0x199, 0);
    return &g_zresBikes[bike];
}

/* VA 0x0046bc20, 13 bytes, [I] between, status draft */
int zresLoadTables(void)
{
    zresLoadPlayerTable();
    zresLoadMapTable();
    return 0;
}

/* VA 0x0046bc40, 101 bytes, [I] between, status draft */
void zresCloseLibraries(void)
{
    if (g_materialZal != NULL) {
        zassetCloseLibrary(g_materialZal);
        g_materialZal = NULL;
    }
    if (g_miscDataZal != NULL) {
        zassetCloseLibrary(g_miscDataZal);
        g_miscDataZal = NULL;
    }
    if (g_tablesZal != NULL) {
        zassetCloseLibrary(g_tablesZal);
        g_tablesZal = NULL;
    }
    if (g_replayZal != NULL) {
        zassetCloseLibrary(g_replayZal);
        g_replayZal = NULL;
    }
}

/* VA 0x0046bcd0, 256 bytes, [V] direct, status draft */
void zresOpenLibraries(void)
{
    g_miscDataZal = zassetOpenLibrary("Assets\\MiscData.ZAL");
    if (g_miscDataZal == NULL) {
        g_pfnDebugPrintf("ZRES: Failed to open:%s\n", "Assets\\MiscData.ZAL");
        zAssertFail("Could not open ZAL", NGIFZRES_SRC, 0x1E9, 0);
        return;
    }
    g_tablesZal = zassetOpenLibrary("Assets\\Tables.ZAL");
    if (g_tablesZal == NULL) {
        g_pfnDebugPrintf("ZRES: Failed to open:%s\n", "Assets\\Tables.ZAL");
        zAssertFail("Could not open ZAL", NGIFZRES_SRC, 0x1F1, 0);
        return;
    }
    g_replayZal = zassetOpenLibrary("Assets\\Replay.ZAL");
    if (g_replayZal == NULL) {
        g_pfnDebugPrintf("ZRES: Failed to open:%s\n", "Assets\\Replay.ZAL");
        zAssertFail("Could not open ZAL", NGIFZRES_SRC, 0x1F9, 0);
        return;
    }
    g_materialZal = zassetOpenLibrary("Assets\\Material.ZAL");
    if (g_materialZal == NULL) {
        g_pfnDebugPrintf("ZRES: Failed to open:%s\n", "Assets\\Material.ZAL");
        zAssertFail("Could not open ZAL", NGIFZRES_SRC, 0x201, 0);
    }
}

/* VA 0x0046be10, 13 bytes, [I] between, status draft */
int zresInit(void)
{
    zresOpenLibraries();
    zresLoadTables();
    return 0;
}

/* VA 0x0046be30, 118 bytes, [I] callers, status draft */
void zresShutdown(void)
{
    /* ngappShutdown: the two tables, the asset libraries, NgIFzbsp's record pools */
    if (s_playerData != NULL) {
        zfileClose(s_playerFile, NULL);
        s_playerData = NULL;
    }
    if (s_mapData != NULL) {
        zfileClose(s_mapFile, NULL);
        s_mapData = NULL;
    }
    zresCloseLibraries();
    if (g_zbspEdgePool != NULL) {
        zmemFreeListDestroy(g_zbspEdgePool);
        g_zbspEdgePool = NULL;
    }
    if (g_zbspFacePool != NULL) {
        zmemFreeListDestroy(g_zbspFacePool);
        g_zbspFacePool = NULL;
    }
}

/* VA 0x0046bed0, 1 bytes, [I] between, status draft */
void zresNop(void)
{
}

/* VA 0x0046bef0, 35 bytes, [I] between, status draft */
int zresReset(void)
{
    s_libs[0] = g_materialZal;
    s_libs[1] = g_miscDataZal;
    s_overlayCount = 0;
    return 0;
}

/* VA 0x0046bf30, 526 bytes, [V] direct, status draft */
void zresAddMaterialToLoadList(int res, int unused, uint32_t slot)
{
    /* overlay.c: picture resource `res` into image slot `slot` (once): the asset becomes a ZGL
     * material "Overlay<n>", the slot's size is the texture's (at most 255 x 255) */
    const ZresImageDef *d;
    ZassetLib *lib;
    uint32_t size, mat, w, h;
    ZoverTexture *t;
    char name[32];
    void *img;
    int mark, i;

    (void)unused;
    if (s_imagesCleared == 0) {
        zstringMemSet(g_psxTextures, 0, 0x2800);
        s_imagesCleared = 1;
    }
    for (i = 0; g_zresImages[i].id != res; i++) {
        if (i + 1 >= 11) {
            zAssertFail("zresAddMaterialToLoadList() Not found", NGIFZRES_SRC, 0x2B0, 0);
            return;
        }
    }
    d = &g_zresImages[i];
    lib = s_libs[d->lib];
    if (zassetFindAsset(lib, d->asset, 0) == 0) {
        NGIF_MISSING(NGIFZRES_SRC, 0x28C, "zresAddMaterialToLoadList() Not found");
        return;
    }
    slot &= 0xFFFF;
    t = &g_psxTextures[slot];
    if (t->texture != 0)
        return;
    mark = zmemTempMark();
    size = zassetGetAssetSize(lib);
    img = zmemTempAlloc(size);
    zassetLoadAsset(lib, img);
    zstringPrintf(name, "Overlay%d", s_overlayCount);
    mat = g_zglMatCreateFromImage(img, size, name, 0, 0);
    zmemTempRelease(mark);
    if (mat == 0) {
        zAssertFail("ERROR loading overlay material", NGIFZRES_SRC, 0x2AA, 0);
        return;
    }
    s_overlayCount++;
    zstringMemSet(t, 0, sizeof *t);
    t->texture = mat;
    w = (uint32_t)(int32_t)g_texDriverVtbl[5](g_zglMatGetTexture((void *)mat, 0));
    t->w = (uint8_t)(w < 0x100 ? w : 0xFF);
    h = (uint32_t)(int32_t)g_texDriverVtbl[6](g_zglMatGetTexture((void *)mat, 0));
    t->h = (uint8_t)(h < 0x100 ? h : 0xFF);
    t->u = 0;
    t->v = 0;
    t->u1 = t->w;
    t->v1 = t->h;
}

/* VA 0x0046c1d0, 55 bytes, [I] callers, status draft */
void zresFreeImages(void)
{
    /* ngappShutdown: destroy every image slot's material */
    int i;

    for (i = 0; i < 0x200; i++)
        if (g_psxTextures[i].texture != 0)
            g_pfnZglMatDestroy(g_psxTextures[i].texture);
    zstringMemSet(g_psxTextures, 0, 0x2800);
}

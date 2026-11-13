/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFzroller.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFzroller.c
 * Evidence: [V] path string 0x0055E0B4: zrollerLoadAssets (line 0x168), zrollerCreateRider
 *           (0x19B), zrollerCreateBike (0x233), zrollerGetPlayerStartPos (0x563, 0x564),
 *           zrollerAddDrawCallback (0x712) and the printing stubs (lines 282 .. 1785, in address
 *           order); [I] zrollerStartup / Init / Shutdown (0x0046C220..: "ZROLLER: Failed to
 *           intialize ZRES", this file's globals), the gap functions and the silent stubs by link
 *           order, up to winChan.c.
 * 92 functions, VA 0x0046c220-0x0046d923 (the NgIF pass took 3 functions from NgIFzres.c and the 2 + 5
 * of the zres/zroller and zroller/winChan gaps, and created the 48 printing stubs, 15 silent
 * stubs and zrollerFreeRecordCb).
 * Draft. Non-matching C (AD-8). Types / prototypes: ngif.h. Description: docs/re/ngif.md.
 *
 * "zroller" is the PSX engine's level runner; on the PC it keeps: the rider / bike actors and
 * their shadow models, the player descriptors (rider, style, bike), the prop rails, the world
 * bounds, the player start points, a table of 8 per-frame draw callbacks (prop.c, physics.c,
 * game.c, overlay.c, camera.c register theirs), the image-slot allocator - and stubs for the
 * rest. The 17 silent stubs at the end (0x0046D720..) are the PlayStation memory-card calls the
 * save dialogues still make (overlay.c's save page, frontend.c's load / save / format pages).
 */
#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "ngif.h"

#define NGIFZROLLER_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFzroller.c"

/* ------------------------------------------------------------------ other modules */
void *zobjGetHandle(void *obj);                                        /* the object's ZGL handle */
void  zobjSetActive(void *obj, int visible);
extern int zrwShadowDrawCb(void *params);                              /* 0x0049B520 (a label: engine shadow pass) */
extern ZmemFreeList *g_zbspFacePool;                                   /* 0x005F34AC NgIFzbsp.c */
extern ZmemFreeList *g_zbspEdgePool;                                   /* 0x005F34B0 */
extern const char *const g_riderNames[12];                             /* 0x0055DC50 */

/* ------------------------------------------------------------------ data */
const char *const g_zrollerBikeModels[49] = {                      /* 0x0055DC80 */
    "DMbike01", "DMbike02", "DMbike03", "DMbike04", "RNbike01", "RNbike02", "RNbike03", "RNbike04",
    "TMcbike01", "TMcbike02", "TMcbike03", "TMcbike04", "MLbike01", "MLbike02", "MLbike03", "MLbike04",
    "CKbike01", "CKbike02", "CKbike03", "CKbike04", "TMibike01", "TMibike02", "TMibike03", "TMibike04",
    "KHbike01", "KHbike02", "KHbike03", "KHbike04", "SBbike01", "SBbike02", "SBbike03", "SBbike04",
    "LRbike01", "LRbike02", "LRbike03", "LRbike04", "JGbike01", "JGbike02", "JGbike03", "JGbike04",
    "SJbike01", "SJbike02", "SJbike03", "SJbike04", "ABbike01", "ABbike02", "ABbike03", "ABbike04",
    "XXbike01"
};

/* ------------------------------------------------------------------ globals (this file) */
static ZrollerDrawFn s_drawCallbacks[8];   /* 0x005FB6C8 */
static int        s_imageSlots;            /* 0x005FB6E8 next free image slot */
static ZrollerPlayer s_players[2];         /* 0x005FB6F0 */
static void      *s_shadowModels[2];       /* 0x005FB710 rider, bike shadow handles */
static float      s_worldBounds[8];        /* 0x005FB718 min[4], max[4] */
static uint32_t   s_numPlayers;            /* 0x005FB738 */
int               g_zrollerLevelLoaded;    /* 0x005FB73C */
int               g_zrollerFrameHandle;    /* 0x005FB740 the shadow pass callback */
ZrollerShadows    g_zrollerShadows;        /* 0x00678440 */
zHeapList        *g_zrollerEdges;          /* 0x00678494 the prop rails (NgColEdge) */

/* ------------------------------------------------------------------ functions */

/* VA 0x0046c220, 184 bytes, [I] split, status draft */
int zrollerStartup(void)
{
    /* actionStartup: shadow parameters, the record pools, the rail list, then zres (1 = failed) */
    g_zrollerShadows.count = 2;
    g_zrollerShadows.unk_0c = 0;
    g_zrollerShadows.unk_10 = -500.0f;
    g_zrollerShadows.unk_14 = 0;
    g_zrollerShadows.unk_1c = 0;
    g_zrollerShadows.unk_20 = -5.0f;
    g_zrollerShadows.unk_24 = 0;
    if (g_zbspEdgePool == NULL)
        g_zbspEdgePool = zmemFreeListCreate(sizeof(NgColEdge), 0x200, NULL);
    if (g_zbspFacePool == NULL)
        g_zbspFacePool = zmemFreeListCreate(sizeof(NgColFace), 0x200, NULL);
    g_zrollerShadows.models = s_shadowModels;
    if (g_zrollerEdges == NULL)
        g_zrollerEdges = zHeapListCreate(0x80);
    return zresInit() != 0;
}

/* VA 0x0046c310, 61 bytes, [I] between, status draft */
int zrollerInit(void)
{
    /* each level / menu start: image slots, zres, no draw callbacks, no players (1 = failed) */
    int i;

    zrollerResetImageSlots();
    if (zresReset() != 0) {
        g_pfnDebugPrintf("ZROLLER: Failed to intialize ZRES\n");
        return 1;
    }
    for (i = 0; i < 8; i++)
        s_drawCallbacks[i] = NULL;
    s_numPlayers = 0;
    return 0;
}

/* VA 0x0046c360, 8 bytes, [I] split, status draft */
int zrollerShutdown(void)
{
    zresNop();
    return 0;
}

/* VA 0x0046c380, 19 bytes, [I] between, status draft */
void zrollerFreeRecordCb(void *elem, void *pool)
{
    zmemFreeListFree((ZmemFreeList *)pool, elem);
}

/* VA 0x0046c3b0, 71 bytes, [I] between, status draft */
void zrollerUnloadLevel(void)
{
    /* level end: forget the rails, free every cached face / edge record */
    if (g_zrollerEdges != NULL)
        zHeapListClear(g_zrollerEdges, NULL, NULL);
    if (g_zbspFacePool != NULL)
        zmemFreeListForEach(g_zbspFacePool, zrollerFreeRecordCb, g_zbspFacePool);
    if (g_zbspEdgePool != NULL)
        zmemFreeListForEach(g_zbspEdgePool, zrollerFreeRecordCb, g_zbspEdgePool);
}

/* VA 0x0046c420, 29 bytes, [I] between, status draft */
int zrollerRunDrawCallbacks(void)
{
    /* action.c, each frame */
    int i;

    for (i = 0; i < 8; i++)
        if (s_drawCallbacks[i] != NULL)
            s_drawCallbacks[i]();
    return 0;
}

/* VA 0x0046c450, 32 bytes, [V] direct, status draft */
int zrollerUpdateZGL(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 282, "zrollerUpdateZGL");
    return 0;
}

/* VA 0x0046c480, 32 bytes, [V] direct, status draft */
int zrollerCreateChaser(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 300, "zrollerCreateChaser");
    return 0;
}

/* VA 0x0046c4b0, 146 bytes, [V] direct, status draft */
int zrollerLoadAssets(void)
{
    /* actionLoad: the rails of every prop whose def's OBJI byte 0 bit 0 is set (props-replay-fx.md 2) */
    zListNode *node;
    NgColEdge *rails[6];
    int n, i;

    for (node = zpropGetList()->head; node != NULL; node = node->next) {
        Zprop *z = (Zprop *)node;

        if (z->info.user == NULL)
            zbspPropColCreate(z);
        if (z->info.def == NULL || z->info.def->obji == NULL || !(z->info.def->obji[0] & 1))
            continue;
        n = zcolCreatePropRails((NgPropCol *)z->info.user, rails);
        for (i = 0; i < n; i++)
            zHeapListAdd(g_zrollerEdges, rails[i]);
    }
    NGIF_MISSING(NGIFZROLLER_SRC, 0x168, "zrollerLoadAssets");
    return 0;
}

/* VA 0x0046c580, 347 bytes, [V] direct, status draft */
ZActor *zrollerCreateRider(const ZrollerPlayerDesc *desc)
{
    /* rider.c: register the player (rider, style, bike), load "<rider name with style digit>" and
     * "Rider Shadow" (white, hidden; the shadow pass draws it), start the shadow pass, make the
     * actor (skeleton 0, the 15 rider channels) */
    char name[64];
    uint32_t white = 0xFFFFFFFF;
    void *model, *shadow;
    ZActor *a;

    if (s_numPlayers > 1)
        zAssertFail("Out of player defs", NGIFZROLLER_SRC, 0x19B, 0);
    zstringMemCopy(&s_players[s_numPlayers].desc, desc, sizeof *desc);
    s_numPlayers++;
    NgIFzactor_ResetPartTables();
    zstringCopy(name, g_riderNames[desc->rider]);
    name[zstringLength(name) - 1] = (char)(desc->style + '1');
    model = zsceneCreateModel(0, name);
    shadow = zsceneCreateModel(0, "Rider Shadow");
    g_zrollerShadows.models[0] = zobjGetHandle(shadow);
    g_zglModelSetTint(zobjGetHandle(shadow), &white);
    g_zrwModelSetFlags(*(void **)((uint8_t *)model + 0x2C), 8, 0);
    zobjSetActive(shadow, 0);
    if (g_zrollerFrameHandle != 0)
        g_zglRemoveFrameCallback(g_zrollerFrameHandle);
    g_zrollerFrameHandle = g_zglAddFrameCallback(5, zrwShadowDrawCb, &g_zrollerShadows, 1);
    a = zactorCreate(1);
    if (a == NULL)
        return NULL;
    zactorSetSkeleton(a, 0);
    a->model = model;
    a->model2 = shadow;
    NgIFzactor_SetRiderChannels(a);
    return a;
}

/* VA 0x0046c740, 210 bytes, [V] direct, status draft */
ZActor *zrollerCreateBike(const uint32_t *bike)
{
    /* rider.c: the bike model (g_zrollerBikeModels[*bike]) and "Bike Shadow", skeleton 1, 10 channels */
    uint32_t white = 0xFFFFFFFF;
    void *model, *shadow;
    ZActor *a = zactorCreate(1);

    if (a == NULL)
        return NULL;
    if (*bike > 0x30)
        zAssertFail("Invalid bike selected!", NGIFZROLLER_SRC, 0x233, 0);
    model = zsceneCreateModel(0, g_zrollerBikeModels[*bike]);
    shadow = zsceneCreateModel(0, "Bike Shadow");
    g_zrollerShadows.bikeModel = zobjGetHandle(model);
    g_zrollerShadows.models[1] = zobjGetHandle(shadow);
    g_zglModelSetTint(zobjGetHandle(shadow), &white);
    g_zrwModelSetFlags(*(void **)((uint8_t *)model + 0x2C), 8, 0);
    zobjSetActive(shadow, 0);
    zactorSetSkeleton(a, 1);
    a->model = model;
    a->model2 = shadow;
    NgIFzactor_SetBikeChannels(a);
    return a;
}

/* VA 0x0046c860, 32 bytes, [V] direct, status draft */
int zrollerCreateProp(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 625, "zrollerCreateProp");
    return 0;
}

/* VA 0x0046c890, 32 bytes, [V] direct, status draft */
int zrollerSetEngineState(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 643, "zrollerSetEngineState");
    return 0;
}

/* VA 0x0046c8c0, 32 bytes, [V] direct, status draft */
int zrollerGetNumPlayerDefs(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 678, "zrollerGetNumPlayerDefs");
    return 0;
}

/* VA 0x0046c8f0, 32 bytes, [V] direct, status draft */
int zrollerLoadMdlPlayerCB(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 699, "zrollerLoadMdlPlayerCB");
    return 0;
}

/* VA 0x0046c920, 26 bytes, [I] between, status draft */
ZrollerPlayer *zrollerGetPlayer(uint32_t index)
{
    /* the player descriptor (+4 rider, +6 style, +7 bike); game.c, overlay.c, rider.c */
    return index < s_numPlayers ? &s_players[index] : NULL;
}

/* VA 0x0046c950, 32 bytes, [V] direct, status draft */
int zrollerIsPlayerClose(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 739, "zrollerIsPlayerClose");
    return 0;
}

/* VA 0x0046c980, 30 bytes, [V] direct, status draft */
void zrollerChangePlayerModeLook(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 758, "ChangePlayerModeLook");
}

/* VA 0x0046c9b0, 30 bytes, [V] direct, status draft */
void zrollerAddPlayerMatsToLoadList(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 777, "zrollerAddPlayerMatsToLoadList");
}

/* VA 0x0046c9e0, 30 bytes, [V] direct, status draft */
void zrollerChangePlayerSponsor(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 795, "zrollerChangePlayerSponsor");
}

/* VA 0x0046ca10, 30 bytes, [V] direct, status draft */
void zrollerChangeBoardSponsor(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 812, "Zrollerchangeboardsponsor");
}

/* VA 0x0046ca40, 32 bytes, [V] direct, status draft */
int zrollerLoadMdlGenericCB(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 828, "zrollerLoadMdlGenericCB");
    return 0;
}

/* VA 0x0046ca70, 32 bytes, [V] direct, status draft */
int zrollerLoadMdlCommon(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 851, "zrollerLoadMdlCommon");
    return 0;
}

/* VA 0x0046caa0, 30 bytes, [V] direct, status draft */
void zrollerLoadMdlCommonMat(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 873, "LoadMdlCommonMat");
}

/* VA 0x0046cad0, 30 bytes, [V] direct, status draft */
void zrollerProcessModelFlags(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 891, "zrollerProcessModelFlags");
}

/* VA 0x0046cb00, 30 bytes, [V] direct, status draft */
void zrollerProcessActorDrawFlags(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 907, "zrollerProcessActorDrawFlags");
}

/* VA 0x0046cb30, 30 bytes, [V] direct, status draft */
void zrollerSetAnimMaterialPalette(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 944, "zrollerSetAnimMaterialPalette");
}

/* VA 0x0046cb60, 30 bytes, [V] direct, status draft */
void zrollerInitializeLoadingScreen(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x3C0, "zrollerInitializeLoadingScreen");
}

/* VA 0x0046cb90, 30 bytes, [V] direct, status draft */
void zrollerShutdownLoadingScreen(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x3D0, "zrollerShutdownLoadingScreen");
}

/* VA 0x0046cbc0, 30 bytes, [V] direct, status draft */
void zrollerDisplayLoadingScreen(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x3E0, "zrollerDisplayLoadingScreen");
}

/* VA 0x0046cbf0, 30 bytes, [V] direct, status draft */
void zrollerSetLocal(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x3E8, "zrollerSetLocal");
}

/* VA 0x0046cc20, 32 bytes, [V] direct, status draft */
int zrollerLoadModule(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1014, "zrollerLoadModule");
    return 0;
}

/* VA 0x0046cc50, 32 bytes, [V] direct, status draft */
int zrollerLoadCodeModule(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x405, "LoadCodeModule");
    return 0;
}

/* VA 0x0046cc80, 11 bytes, [I] between, status draft */
void zrollerResetImageSlots(void)
{
    s_imageSlots = 0;
}

/* VA 0x0046cca0, 30 bytes, [V] direct, status draft */
void zrollerAllocMaterials(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1065, "zrollerAllocMaterials");
}

/* VA 0x0046ccd0, 19 bytes, [I] between, status draft */
int zrollerAllocImageSlots(int count)
{
    /* overlay.c: reserve `count` image slots, returns the first */
    s_imageSlots += count;
    return s_imageSlots - count;
}

/* VA 0x0046cd00, 32 bytes, [V] direct, status draft */
int zrollerAddGenericMaterialsToLoad(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1098, "zrollerAddGenericMaterialsToLoad");
    return 0;
}

/* VA 0x0046cd30, 30 bytes, [V] direct, status draft */
void zrollerInitializeMaps(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1135, "zrollerInitializeMaps");
}

/* VA 0x0046cd60, 30 bytes, [V] direct, status draft */
void zrollerSetMap(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x47F, "zrollerSetMap");
}

/* VA 0x0046cd90, 32 bytes, [V] direct, status draft */
int zrollerLoadMaps(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1168, "zrollerLoadMaps");
    return 0;
}

/* VA 0x0046cdc0, 32 bytes, [V] direct, status draft */
int zrollerLoadMatTerrainFirstCB(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1186, "zrollerLoadMatTerrainFirstCB");
    return 0;
}

/* VA 0x0046cdf0, 32 bytes, [V] direct, status draft */
int zrollerLoadMatTerrainLastCB(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1205, "zrollerLoadMatTerrainLastCB");
    return 0;
}

/* VA 0x0046ce20, 30 bytes, [V] direct, status draft */
void zrollerAssociateMapMaterials(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1223, "zrollerAssociateMapMaterials");
}

/* VA 0x0046ce50, 30 bytes, [V] direct, status draft */
void zrollerAssociateMaterial(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1242, "zroolerAssociateMaterial llljlkfsdjflsjdklf");
}

/* VA 0x0046ce80, 30 bytes, [V] direct, status draft */
void zrollerAssociateMaterialPalAnimation(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1258, "Associate material pal animation");
}

/* VA 0x0046ceb0, 85 bytes, [I] between, status draft */
float *zrollerGetWorldBounds(void)
{
    /* the world's bounding box: min[4], max[4] (camera, game, physics, rider) */
    float box[8];

    g_zglGet(2, box, 0);
    s_worldBounds[0] = box[0];
    s_worldBounds[1] = box[1];
    s_worldBounds[2] = box[2];
    s_worldBounds[4] = box[4];
    s_worldBounds[5] = box[5];
    s_worldBounds[6] = box[6];
    return s_worldBounds;
}

/* VA 0x0046cf30, 567 bytes, [V] direct, status draft */
void zrollerGetPlayerStartPos(int player, float *pos, int16_t *rot)
{
    /* The scene point "Player" n+1 (assert, then (0, -1000, 0) if missing); facing "PlayerT"
     * n+1 when present (yaw from +x, flat), else the point's own orientation turned round. */
    ZscnPoint *start = zscnptsFind(zHashName("Player"), (uint32_t)player + 1);
    ZscnPoint *target = zscnptsFind(zHashName("PlayerT"), (uint32_t)player + 1);
    RwV3d dir, xAxis = { 1.0f, 0.0f, 0.0f };
    RwMatrix m;
    Zquat q;
    int16_t e[3];

    if (start == NULL) {
        zAssertFail("Player start point not found - Using default value", NGIFZROLLER_SRC, 0x563, 0);
        NGIF_MISSING(NGIFZROLLER_SRC, 0x564, "zrollerGetPlayerStartPos: HARDCODED");
        pos[0] = 0.0f;
        pos[1] = -1000.0f;
        pos[2] = 0.0f;
        rot[0] = rot[1] = rot[2] = 0;
        return;
    }
    pos[0] = start->matrix.pos.x;
    pos[1] = start->matrix.pos.y;
    pos[2] = start->matrix.pos.z;
    if (target != NULL) {
        zmathVecSub(&dir, &target->matrix.pos, &start->matrix.pos);
        dir.y = 0.0f;
        zmathVecNormalize(&dir, &dir);
        rot[0] = 0;
        rot[2] = 0;
        if (dir.z < 0.0f)
            rot[1] = zmathAcos(zmathVecDot(&dir, &xAxis));
        else
            rot[1] = (int16_t)-zmathAcos(zmathVecDot(&dir, &xAxis));
        return;
    }
    zquatFromMatrix(&q, &start->matrix);
    if (fabsf(q.y) >= 0.01f) {
        q.w = -q.w;
    } else {
        q.x = -q.x;
        q.z = -q.z;
    }
    zquatToMatrix(&m, &q);
    zmathMatrixToEuler(e, &m);
    rot[0] = e[0];
    rot[1] = e[1];
    rot[2] = e[2];
}

/* VA 0x0046d200, 32 bytes, [V] direct, status draft */
int zrollerGetPlayerRestartPos(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1411, "zrollerGetPlayerRestartPos");
    return 0;
}

/* VA 0x0046d230, 30 bytes, [V] direct, status draft */
void zrollerInitPropLoadList(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1429, "zrollerInitPropLoadList");
}

/* VA 0x0046d260, 30 bytes, [V] direct, status draft */
void zrollerAddPropToLoadList(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1445, "zrollerAddPropToLoadList");
}

/* VA 0x0046d290, 30 bytes, [V] direct, status draft */
void zrollerCreateProps(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1461, "Create Props");
}

/* VA 0x0046d2c0, 30 bytes, [V] direct, status draft */
void zrollerLoadPropMaterials(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1477, "zrollerLoadPropMaterials");
}

/* VA 0x0046d2f0, 32 bytes, [V] direct, status draft */
int zrollerCreatePropActor(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1494, "zrollerCreatePropActor");
    return 0;
}

/* VA 0x0046d320, 32 bytes, [V] direct, status draft */
int zrollerDestroyActor(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1512, "zrollerDestroyActor");
    return 0;
}

/* VA 0x0046d350, 3 bytes, [I] between, status draft */
int zrollerUnusedStub1(void)
{
    return 0;
}

/* VA 0x0046d370, 32 bytes, [V] direct, status draft */
int zrollerGetAsyncKeyState(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1544, "zrollerGetAsyncKeyState");
    return 0;
}

/* VA 0x0046d3a0, 1 bytes, [I] between, status draft */
void zrollerNop1(void)
{
    /* frontend.c calls it */
}

/* VA 0x0046d3c0, 1 bytes, [I] between, status draft */
void zrollerNop2(void)
{
    /* frontend.c calls it */
}

/* VA 0x0046d3e0, 32 bytes, [V] direct, status draft */
int zrollerSelectMap(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1585, "zrollerSelectMap");
    return 0;
}

/* VA 0x0046d410, 32 bytes, [V] direct, status draft */
int zrollerSelectPlayer(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1599, "zrollerSelectPlayer");
    return 0;
}

/* VA 0x0046d440, 32 bytes, [V] direct, status draft */
int zrollerSelectBikeNum(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1614, "zrollerSelectBikeNum");
    return 0;
}

/* VA 0x0046d470, 30 bytes, [V] direct, status draft */
void zrollerSetFogDistance(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1637, "set fog distance");
}

/* VA 0x0046d4a0, 30 bytes, [V] direct, status draft */
void zrollerGetFogDistance(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1647, "get fog distance");
}

/* VA 0x0046d4d0, 30 bytes, [V] direct, status draft */
void zrollerDisableFog(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1657, "zrollerDisableFog");
}

/* VA 0x0046d500, 30 bytes, [V] direct, status draft */
void zrollerSetFogColor(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1667, "zrollerSetFogColor");
}

/* VA 0x0046d530, 30 bytes, [V] direct, status draft */
void zrollerReplaceMaterial(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1677, "zrollerReplaceMaterial");
}

/* VA 0x0046d560, 30 bytes, [V] direct, status draft */
void zrollerSetShadeMode(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1692, "zrollerSetShadeMode");
}

/* VA 0x0046d590, 30 bytes, [V] direct, status draft */
void zrollerStartCrossFade(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 0x6AD, "zrollerStartCrossFade");
}

/* VA 0x0046d5c0, 46 bytes, [V] direct, status draft */
int zrollerGetSceneState(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1731, "zrollerGetSceneState: NO DATA");
    return 0;
}

/* VA 0x0046d600, 30 bytes, [V] direct, status draft */
void zrollerFreeSceneState(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1755, "FreeSceneState");
}

/* VA 0x0046d630, 30 bytes, [V] direct, status draft */
void zrollerSetSceneState(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1770, "SetSceneState");
}

/* VA 0x0046d660, 32 bytes, [V] direct, status draft */
int zrollerSelectLook(void)
{
    NGIF_MISSING(NGIFZROLLER_SRC, 1785, "zrollerSelectLook()");
    return 0;
}

/* VA 0x0046d690, 61 bytes, [V] direct, status draft */
void zrollerAddDrawCallback(ZrollerDrawFn fn)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (s_drawCallbacks[i] == NULL) {
            s_drawCallbacks[i] = fn;
            return;
        }
    }
    zAssertFail("registered too many draw callbacks to zroller", NGIFZROLLER_SRC, 0x712, 0);
}

/* VA 0x0046d6e0, 3 bytes, [I] between, status draft */
int zrollerUnusedStub2(void)
{
    return 0;
}

/* VA 0x0046d700, 1 bytes, [I] between, status draft */
void zrollerUnusedStub3(void)
{
}

/* ------------------------------------------------------------------ the PSX memory-card stubs */
/* VA 0x0046d720 .. 0x0046d920: 17 silent stubs ("return" / "return 0"), 0x20 apart. Five are
 * called by the save dialogues; the names follow the calls (libmcrd style) [I]; the other twelve
 * are unreferenced. */

/* VA 0x0046d720, 1 bytes, [I] between, status draft */
void zcardSelectPort(uint8_t port)
{
    (void)port;    /* before every check / load / save / format */
}

/* VA 0x0046d740, 3 bytes, [I] between, status draft */
int zcardStub1(void)
{
    return 0;
}

/* VA 0x0046d760, 3 bytes, [I] between, status draft */
int zcardStub2(void)
{
    return 0;
}

/* VA 0x0046d780, 3 bytes, [I] between, status draft */
int zcardStub3(void)
{
    return 0;
}

/* VA 0x0046d7a0, 3 bytes, [I] between, status draft */
int zcardCheck(ZcardDoneFn done)
{
    /* overlaySaveUpdate: start the asynchronous card check; the callback never comes */
    (void)done;
    return 0;
}

/* VA 0x0046d7c0, 3 bytes, [I] between, status draft */
int zcardStub5(void)
{
    return 0;
}

/* VA 0x0046d7e0, 3 bytes, [I] between, status draft */
int zcardStub6(void)
{
    return 0;
}

/* VA 0x0046d800, 3 bytes, [I] between, status draft */
int zcardStub7(void)
{
    return 0;
}

/* VA 0x0046d820, 3 bytes, [I] between, status draft */
int zcardStub8(void)
{
    return 0;
}

/* VA 0x0046d840, 3 bytes, [I] between, status draft */
int zcardStub9(void)
{
    return 0;
}

/* VA 0x0046d860, 3 bytes, [I] between, status draft */
int zcardStub10(void)
{
    return 0;
}

/* VA 0x0046d880, 3 bytes, [I] between, status draft */
int zcardStub11(void)
{
    return 0;
}

/* VA 0x0046d8a0, 3 bytes, [I] between, status draft */
int zcardFormat(void)
{
    return 0;      /* "formatted" */
}

/* VA 0x0046d8c0, 1 bytes, [I] between, status draft */
void zcardStart(void)
{
    /* entering a card page */
}

/* VA 0x0046d8e0, 1 bytes, [I] between, status draft */
void zcardStop(void)
{
    /* leaving it */
}

/* VA 0x0046d900, 1 bytes, [I] between, status draft */
void zcardStub15(void)
{
}

/* VA 0x0046d920, 3 bytes, [I] between, status draft */
int zcardStub16(void)
{
    return 0;
}

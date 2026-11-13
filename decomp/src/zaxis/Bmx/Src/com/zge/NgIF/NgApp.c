/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgApp.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgApp.c
 * Evidence: [V] path string 0x0055BFD0, asserts of ngappLoadInterfaceArt (lines 0x3C2..0x3EC) and
 *           ngappFrame (0x86B, 0x91F); [V] ngappInit is called by App_Start (zrwZgl.c) and
 *           ngappFrame by App_Frame: the application hooks; [I] the rest by link order (between
 *           MissingZGE.c and NgIFfx.c) and their callers.
 * 28 functions, VA 0x0045a340-0x0045c93c (DM-057 absorbed the MissingZGE/NgApp gap and created 11
 * functions Ghidra had not made: 0x0045A380, 0x0045A3A0, 0x0045A4D0, 0x0045AB50, 0x0045AB90,
 * 0x0045ABC0, 0x0045B4E0, 0x0045C150, 0x0045C690, 0x0045C6D0, 0x0045C6F0).
 * DM-057 (draft). Non-matching C (AD-8). Types / prototypes: ngif.h. Description: docs/re/ngif.md.
 *
 * The application: start-up (window title, memory, intro movies, sound, actors, front end), the
 * per-frame function that runs either the front end (g_ngappMode MENU) or the game (ACTION) and
 * switches between them (level load with the loading bar; unload back to the menus), the level
 * lights (SpotLight scene points + an ambient light that follows the ground colour under the
 * rider), the adaptive far clip, and the debug tag drawing.
 */
#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "zsound.h"
#include "ngif.h"

#define NGAPP_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgApp.c"

/* ------------------------------------------------------------------ other modules */
void     *zobjCameraCreate(int flags);                      /* create a camera object */
RwMatrix *zobjCameraGetMatrix(void *cam, int which);           /* its view matrix */
void      zobjDestroy(void *obj);                            /* destroy a scene object */
void     *zobjGetHandle(void *obj);                            /* the object's ZGL handle */

int       zinputSetKeyLayout(ZInputDevice *device, const void *layout); /* 0x00493CC0 (pcinput.c) */
int       zrwColLine(const RwV3d *from, const RwV3d *to, int a, int b, void *hit); /* RW line test */
uint8_t  *zrwColGetHitColour(void *hit);                          /* the hit's prelit colour */
void     *zrwCamGetFirst(void);                               /* main camera */
void      zrwCamBeginUpdate(void *cam);                          /* begin update */
void      zrwCamEndUpdate(void);                               /* end update */
void      zrwZglShowFrame(int flip);                           /* show the frame */
int       pcinputPollDevices(void);                               /* 0x00493910: one frame of the ZGL callback list */
void      zrwModelForEachMaterial(void *model, void *fn);      /* 0x0048FEC0 */
void      zobjLightSetFlags(void *light, uint32_t flags);      /* 0x004900F0 */
void      zobjLightSetColour(void *light, const uint8_t *rgb); /* 0x004900D0 */
void      zobjLightSetDirection(void *light, const RwV3d *dir);/* 0x00490120 */
void      zrwZglPlayMovie(const char *path);                   /* 0x0048B040 */
int __stdcall SetWindowTextA(HWND hwnd, const char *text);   /* USER32 */
int       App_Frame(void);                                     /* 0x0048B570 */
void      zactorCloseMotionLibrary(void);                      /* 0x00475000 zactor.c */
void      actionStartup(void);                                 /* action.c */
void      actionLoad(void);
void      actionUpdate(void);
void      actionShutdown(void);
int       actionGetMode(void);
void      actionPickDemoLevel(void);
int       frontendInitApp(void);                                /* frontend.c */
void      frontendShutdownApp(void);
int       frontendUpdateApp(void);
int       frontendAllocMacros(void);                           /* 0x0044D940 */
void      psxBeginScene(void);                                 /* NgIFpsx.c 0x00463820 */
void      psxGetScreenSize(void);                              /* 0x004638B0 */
void      psxEnableDraw(void);                                 /* 0x004638F0 */
void      psxDisableDraw(void);                                /* 0x00463930 */
int       psx2DInit(void);                                     /* 0x004639F0 */
void      psx2DShutdown(void);                                 /* 0x00463A60 */
void      psx2DBeginFrame(void);                               /* 0x00463AB0 */
void      zrollerUnloadLevel(void);                            /* NgIFzroller.c 0x0046C3B0 */
void      zresFreeImages(void);                                /* NgIFzres.c 0x0046C1D0 */
void      zresShutdown(void);                                  /* 0x0046BE30 */
void      NgIFzactor_PosePlayer(void);                             /* NgIFzactor.c 0x004659E0 */
extern const uint8_t g_keyLayouts[][32];                       /* contrler.c key layouts */
extern uint32_t  g_appQuit;                                    /* 0x00551FD4 (action.c's name) */
extern uint32_t  g_guiGroupLoaded;                             /* 0x00573250 guimain.c */
extern const char *g_assetsDir;                                /* 0x0055BA04 "assets\" */
extern int       g_zrollerFrameHandle;                         /* 0x005FB740 NgIFzroller.c */
extern int       g_zrollerLevelLoaded;                         /* 0x005FB73C */
extern zHeapList *g_zrollerEdges;                              /* 0x00678494 NgIFzbsp.c rail edges */
extern void      ngappLightPropMaterialCb;                     /* 0x0048F3E0 (a label in zrwModelRender) */

/* ------------------------------------------------------------------ globals (this file) */
uint8_t     g_ngappMode;              /* 0x005AB3A0 NGAPP_MODE_* (zsystemSetMenuMode / ActionMode) */
static void *s_spotLights[3];         /* 0x005AB3AC */
static char  s_intArtName[128];       /* 0x005AB3B8 RAM-disk file name of the interface art */
static void *s_dirLights[3];          /* 0x005AB438 ngappCreateDefaultLights */
static float s_appTime;               /* 0x005AB454 sum of the frame times */
static void *s_ambientLight;          /* 0x005AB458 */
uint32_t     g_ngappIdleReset;        /* 0x005AB460 set by the front end: restart it (attract timeout) */
static int   s_levelMark;             /* 0x005AB468 zmemTempMark before the level */
static void *s_intArt;                /* 0x005AB46C intwrap.zal group 0 (menu art), on the RAM disk */
static int   s_loadingHandle;         /* 0x005AB470 loading-bar frame callback */
static uint32_t s_resetting;          /* 0x005AB474 */
static uint32_t s_resetCountdown;     /* 0x005AB478 frames until the front end restarts */
static uint32_t s_ambientFrame;       /* 0x005AB47C */
static uint16_t s_loadProgress;       /* 0x005AB480 += 4 per loading-bar draw */
static uint32_t s_matLegal;           /* 0x005AB488 */
static uint32_t s_matLoading;         /* 0x005AB48C */
static uint32_t s_matLegal2;          /* 0x005AB490 */
static uint32_t s_fpsTime;            /* 0x005AB49C */
static int32_t  s_fpsFrames;          /* 0x005AB4A0 */
static uint32_t s_fpsAdjusting;       /* 0x005AB4A4 */
static float    s_farClipStep;        /* 0x005AB4A8 */
static zfileDevice *s_ramDisk = (zfileDevice *)-1; /* 0x0055BC38 */
static int32_t  s_firstFrame = 1;     /* 0x0055BC3C 1 until the legal screens have been shown */
static const char *s_intwrapName = "intwrap.zal";  /* 0x0055BC40 */
/* 0x0055BC44: the 14 level (scene) names, "Greenville Dirt" .. "Acclaim Vert", "Test A", "Test B" */
extern const char *const g_levelNames[14];
static const RwRGBA s_defaultAmbient = { 0x80, 0x80, 0x80, 0 };             /* 0x0055BC8C */
static const float  s_defaultLightPos[3][4] = {                              /* 0x0055BC90 */
    { -5200.0f, -7200.0f, 0.0f, 0.0f }, { 4218.0f, -9100.0f, 3408.0f, 0.0f }, { 5600.0f, 2400.0f, 0.0f, 0.0f } };
static const float  s_defaultLightTarget[3][4] = {                           /* 0x0055BCC0 */
    { -5148.0f, -7116.0f, 0.0f, 0.0f }, { 4170.0f, -9022.0f, 3366.0f, 0.0f }, { 5522.0f, 2340.0f, 20.0f, 0.0f } };
static const RwRGBA s_defaultLightColour[3] = {                              /* 0x0055BCF0 */
    { 0x99, 0xAD, 0xB4, 0 }, { 0xF9, 0xF7, 0xE3, 0 }, { 0xB1, 0x99, 0x99, 0 } };
static const RwRGBA s_levelAmbient = { 0x30, 0x30, 0x30, 0 };                /* 0x0055BCFC */
static uint32_t s_legalTimer;         /* 0x0055BD04 */
static float    s_farClip = 200.0f;   /* 0x0055BD08 */
static float    s_fps;                /* 0x0055BD0C measured frame rate */
static const RwRGBA s_tagColours[3] = { /* 0x0055BC7C: tagged face, edge, rail */
    { 0x80, 0x00, 0x00, 0x80 }, { 0x00, 0x80, 0x00, 0x80 }, { 0x00, 0x00, 0x80, 0x80 } };
static const RwRGBA s_untaggedColour = { 0x80, 0x80, 0x80, 0x80 };           /* 0x0055BC84 */

/* 0x0068FF84..: the application block */
void       *g_ngappCamera;            /* 0x0068FF98 the scene camera object */
ZmemFreeList *g_ngappFreeList;        /* 0x0068FF94 0x80 x 0x100 */
uint8_t     g_ngappShowFaces;         /* 0x0068FFA0 debug: 1 tagged faces, 2 all faces */
uint8_t     g_ngappShowEdges;         /* 0x0068FFA1 debug: 1 tagged edges, 2 all edges (+ rails) */
uint8_t     g_ngappWireframe;         /* 0x0068FFA2 debug: models drawn with flag 4 */
static uint8_t s_wireframeShown;      /* 0x0068FFA3 */
static int  s_debugHandle;            /* 0x0068FFA4 */

static void waitMs(uint32_t start, uint32_t ms)
{
    while (g_zglGetTime() - start < ms)
        pcinputPollDevices();
}

/* intwrap.zal: group 0 menu art, 1 LEGAL, 2 LOADING, 3 LOADING (attract mode), 4 LEGAL2 */
static uint32_t loadIntwrapImage(ZassetLib *lib, int skip, const char *name, int mark)
{
    uint32_t size, mat = 0;
    void *img;

    (void)mark;
    while (skip-- > 0)
        zassetNextGroup(lib);
    size = zassetGetAssetSize(lib);
    img = zmemTempAlloc(size);
    zassetLoadAsset(lib, img);
    if (img != NULL)
        mat = g_zglMatCreateFromImage(img, size, name, 0, 0);
    return mat;
}

static void showImage(uint32_t mat)
{
    zrwCamBeginUpdate(zrwCamGetFirst());
    ngappDrawFullScreen(mat);
    zrwCamEndUpdate();
    zrwZglShowFrame(1);
}

/* ------------------------------------------------------------------ functions */

/* VA 0x0045a340, 33 bytes, [I] callers, status draft */
void ngappWorldToCamera(const RwV3d *world, RwV3d *out)
{
    /* zsndsrc.c: a 3-D sound's position relative to the listener (the scene camera) */
    zmathTransformPoint(out, zobjCameraGetMatrix(g_ngappCamera, 0), world);
}

/* VA 0x0045a380, 3 bytes, [I] between, status draft */
int ngappUnusedStub1(void)
{
    return 0;   /* unreferenced */
}

/* VA 0x0045a3a0, 3 bytes, [I] between, status draft */
int ngappUnusedStub2(void)
{
    return 0;   /* unreferenced */
}

/* VA 0x0045a3c0, 198 bytes, [I] callers, status draft */
int ngappPlayMovie(const char *const *const *movie)
{
    /* frontend.c (movies menu): the name's extension becomes "AVI"; the 2-D layer and the sound are
     * shut down around the movie, with 3 s pauses before and after. */
    char path[128];
    uint32_t t;

    psx2DShutdown();
    zstringCopy(path, **movie);
    zstringCopy(path + zstringLength(path) - 3, "AVI");
    t = g_zglGetTime();
    waitMs(t, 3000);
    bmxsoundClose();
    zrwZglPlayMovie(path);
    t = g_zglGetTime();
    waitMs(t, 3000);
    bmxsoundOpen();
    psx2DInit();
    psx2DBeginFrame();
    return 0;
}

/* VA 0x0045a4d0, 1317 bytes, [I] between, status draft */
int ngappDebugDrawTags(void *arg)
{
    /* Debug view (frame callback of ngappDebugDrawOn): Tag._sd faces near the rider (within 1500)
     * as translucent triangles lifted 9 along the normal and 10 up, tagged red / untagged grey;
     * tagged edges within 2000 as green lines, prop rails (g_zrollerEdges) as blue lines; and the
     * wireframe toggle of every prop model. Never switched on in the release build. */
    ZtagFace *f = ztaginfGetFaces();
    uint32_t n = ztaginfGetFaceCount();
    ZglVertex v[3], rail[2];
    ZglTri tri;
    ZglLine line;
    const RwV3d *rider;
    RwV3d d, centre;
    zListNode *node;
    int i;

    (void)arg;
    tri.material = 0;
    line.material = 0;
    if (g_ngappShowFaces || g_ngappShowEdges) {
        for (; n != 0; n--, f++) {
            centre.x = centre.y = centre.z = 0.0f;
            for (i = 0; i < 3; i++) {
                v[i].colour = s_tagColours[0];
                v[i].pos.x = f->normal.x * 9.0f + f->v[i].x;
                v[i].pos.y = f->normal.y * 9.0f + f->v[i].y - 10.0f;
                v[i].pos.z = f->normal.z * 9.0f + f->v[i].z;
                v[i].u = v[i].v = 0.0f;
                centre.x += f->v[i].x * (1.0f / 3.0f);
                centre.y += f->v[i].y * (1.0f / 3.0f);
                centre.z += f->v[i].z * (1.0f / 3.0f);
            }
            rider = &((ZActor *)g_riders[g_currentPlayer].riderActor)->pos;
            if (g_ngappShowFaces && f->faceTag != NULL &&
                ((*(uint16_t *)f->faceTag != 0 && g_ngappShowFaces == 1) || g_ngappShowFaces == 2)) {
                zmathVecSub(&d, rider, &centre);
                if (zmathVecLength(&d) < 1500.0f) {
                    if (*(uint16_t *)f->faceTag == 0)
                        v[0].colour = v[1].colour = v[2].colour = s_untaggedColour;
                    tri.v[0] = &v[0]; tri.v[1] = &v[1]; tri.v[2] = &v[2];
                    g_zglDrawTris(0x2202, &tri, 1, 0, 0);
                }
            }
            if (g_ngappShowEdges) {
                for (i = 0; i < 3; i++) {
                    const uint8_t *e = f->edgeTag[i];
                    int j = (i + 1) % 3;

                    if (e == NULL || !((e[2] != 0 && g_ngappShowEdges == 1) || g_ngappShowEdges == 2))
                        continue;
                    d.x = rider->x - (f->v[i].x + f->v[j].x) * 0.5f;
                    d.y = rider->y - (f->v[i].y + f->v[j].y) * 0.5f;
                    d.z = rider->z - (f->v[i].z + f->v[j].z) * 0.5f;
                    if (zmathVecLength(&d) < 2000.0f) {
                        v[0].colour = v[1].colour = v[2].colour = s_tagColours[1];
                        line.v[0] = &v[i];
                        line.v[1] = &v[j];
                        g_zglDrawLines(0x2202, &line, 1, 0, 0);
                    }
                }
            }
        }
        if (g_ngappShowEdges) {
            NgColEdge *e;

            for (e = (NgColEdge *)zHeapListFirst(g_zrollerEdges); e != NULL;
                 e = (NgColEdge *)zHeapListNext(g_zrollerEdges)) {
                uint16_t flags = ((NgPropCol *)e->owner)->flags;

                if ((flags & 0xC0) != 0 || (flags & 0x300) != 0)
                    continue;                          /* moving / off / hidden props */
                rail[0].pos.x = (float)e->v0[0];
                rail[0].pos.y = (float)e->v0[1] - 20.0f;
                rail[0].pos.z = (float)e->v0[2];
                rail[1].pos.x = (float)e->v1[0];
                rail[1].pos.y = (float)e->v1[1] - 20.0f;
                rail[1].pos.z = (float)e->v1[2];
                rider = &((ZActor *)g_riders[g_currentPlayer].riderActor)->pos;
                d.x = rider->x - (rail[1].pos.x + rail[0].pos.x) * 0.5f;
                d.y = rider->y - (rail[1].pos.y + rail[0].pos.y) * 0.5f;
                d.z = rider->z - (rail[1].pos.z + rail[0].pos.z) * 0.5f;
                if (zmathVecLength(&d) < 2000.0f) {
                    rail[0].colour = rail[1].colour = v[2].colour = s_tagColours[2];
                    line.v[0] = &rail[0];
                    line.v[1] = &rail[1];
                    g_zglDrawLines(0x2202, &line, 1, 0, 0);
                }
            }
        }
    }
    if (g_ngappWireframe != s_wireframeShown) {
        for (node = zpropGetList()->head; node != NULL; node = node->next) {
            void *model = *(void **)((uint8_t *)((Zprop *)node)->info.obj + 0x2C);

            if (g_ngappWireframe == 0)
                g_zrwModelSetFlags(model, 4, 0);
            else
                g_zrwModelSetFlags(model, 0, 4);
        }
        s_wireframeShown = g_ngappWireframe;
    }
    return 1;
}

/* VA 0x0045ab50, 35 bytes, [I] between, status draft */
void ngappDebugDrawOn(void)
{
    if (s_debugHandle == 0)
        s_debugHandle = g_zglAddFrameCallback(2, ngappDebugDrawTags, NULL, 1);
}

/* VA 0x0045ab90, 30 bytes, [I] between, status draft */
void ngappDebugDrawOff(void)
{
    if (s_debugHandle != 0) {
        g_zglRemoveFrameCallback(s_debugHandle);
        s_debugHandle = 0;
    }
}

/* VA 0x0045abc0, 96 bytes, [I] between, status draft */
void ngappPrintVersion(void)
{
    /* "NextGen BMX : Nov 22 2000,15:54:17" (__DATE__ "," __TIME__) at (320, 0); unreferenced */
    char text[256];

    zstringCopy(text, "NextGen BMX : ");
    zstringCat(text, "Nov 22 2000");
    zstringCat(text, ",");
    zstringCat(text, "15:54:17");
    g_pfnDebugPrintAt(0x140, 0, "%s", text);      /* "%s" at 0x00557430 (DM-064: not a colour) */
}

/* VA 0x0045ac40, 60 bytes, [I] strings, status draft */
void ngappSetupLightProps(void)
{
    /* props of the def "light_": every material of the model gets the light-material callback */
    zListNode *node;

    for (node = zpropGetList()->head; node != NULL; node = node->next) {
        Zprop *p = (Zprop *)node;

        if (zHashName("light_") == p->info.defId)
            zrwModelForEachMaterial(*(void **)((uint8_t *)p->info.obj + 0x2C), &ngappLightPropMaterialCb);
    }
}

/* VA 0x0045ac90, 53 bytes, [I] between, status draft */
void ngappApplyPropTints(void)
{
    /* Prop OBJI bytes 0..3 = an RGBA tint (props-replay-fx.md 2): alpha forced to 0xFF; skipped when
     * the first four bytes are all 0. */
    zListNode *node;

    for (node = zpropGetList()->head; node != NULL; node = node->next) {
        uint8_t *obji = ((Zprop *)node)->info.obji;

        if (obji != NULL && *(uint32_t *)obji != 0) {
            obji[3] = 0xFF;
            g_zglModelSetTint(*(void **)((uint8_t *)((Zprop *)node)->info.obj + 0x2C), obji);
        }
    }
}

/* VA 0x0045ace0, 167 bytes, [I] between, status draft */
void ngappCreateDefaultLights(void)
{
    /* an ambient light and three directional lights from fixed positions (unreferenced: levels use
     * ngappCreateLevelLights) */
    int i;
    RwV3d dir;

    s_ambientLight = zsceneCreateLight(0, 1, &s_defaultAmbient);
    zobjLightSetFlags(s_ambientLight, 1);
    for (i = 0; i < 3; i++) {
        s_dirLights[i] = zsceneCreateLight(0, 0, &s_defaultLightColour[i]);
        dir.x = s_defaultLightPos[i][0];
        dir.y = s_defaultLightPos[i][1];
        dir.z = s_defaultLightPos[i][2];
        zmathVecSub(&dir, &dir, (const RwV3d *)s_defaultLightTarget[i]);
        zmathVecNormalize(&dir, &dir);
        zobjLightSetDirection(s_dirLights[i], &dir);
    }
}

/* VA 0x0045adc0, 302 bytes, [I] strings, status draft */
void ngappCreateLevelLights(void)
{
    /* Ambient (0x30, 0x30, 0x30) plus up to three directional lights from the scene points
     * "SpotLight" n (colour = its OBJI) aimed at "SpotLightT" n, or along the point's right axis. */
    uint32_t n;
    RwV3d dir;

    s_ambientLight = zsceneCreateLight(0, 1, &s_levelAmbient);
    zobjLightSetFlags(s_ambientLight, 1);
    for (n = 1; n <= 3; n++) {
        ZscnPoint *light = zscnptsFind(zHashName("SpotLight"), n);
        ZscnPoint *target = zscnptsFind(zHashName("SpotLightT"), n);
        void **slot = &s_spotLights[n - 1];

        if (light == NULL || light->obji == NULL)
            continue;
        if (target == NULL) {
            dir = light->matrix.right;
            *slot = zsceneCreateLight(0, 0, (const RwRGBA *)light->obji);
            zmathVecNormalize(&dir, &dir);
            zobjLightSetDirection(*slot, &dir);
        } else {
            zmathVecSub(&dir, &target->matrix.pos, &light->matrix.pos);
            zmathVecNormalize(&dir, &dir);
            *slot = zsceneCreateLight(0, 0, (const RwRGBA *)light->obji);
            zobjLightSetDirection(*slot, &dir);
            zobjLightSetFlags(*slot, 1);
        }
    }
}

/* VA 0x0045af40, 69 bytes, [I] between, status draft */
ZtagFace *ngappFindFaceWithEdge(ZtagFace *faces, uint32_t code)
{
    /* the first face with an edge whose tag code (u16) equals code */
    uint32_t n = ztaginfGetFaceCount(), i;
    int e;

    for (i = 0; i < n; i++, faces++)
        for (e = 0; e < 3; e++)
            if (faces->edgeTag[e] != NULL && *(uint16_t *)faces->edgeTag[e] == code)
                return faces;
    return NULL;
}

/* VA 0x0045afb0, 70 bytes, [I] between, status draft */
void ngappLinkSpineFaces(void)
{
    /* Level load: each tagged face record gets, at +8, the face that carries the edge its id
     * names (the coping of class 2/3 faces; physics.md 2). Searched from the face itself on. */
    uint32_t n = ztaginfGetFaceCount();
    ZtagFace *f = ztaginfGetFaces();

    for (; n != 0; n--, f++) {
        uint16_t *tag = (uint16_t *)f->faceTag;

        if (tag != NULL && *tag != 0)
            *(ZtagFace **)(tag + 4) = ngappFindFaceWithEdge(f, *tag & 0xFFF);
    }
}

/* VA 0x0045b020, 420 bytes, [V] direct, status draft */
void ngappLoadInterfaceArt(void)
{
    /* intwrap.zal group 0 (the menu art) into memory, published as a RAM-disk file
     * "rd00:ADDRESS=..,SIZE=.." for guimain.c */
    zfileDeviceList *list;
    ZassetLib *lib;
    uint32_t size, i;
    const char *keys[2];
    void *values[2];
    char *name;

    if (s_intArt != NULL)
        zAssertFail("Interface art already loaded!", NGAPP_SRC, 0x3C2, 0);
    lib = zassetOpenLibrary(zfileBuildPath(g_assetsDir, s_intwrapName));
    if (lib == NULL)
        zAssertFail("Interface asset wrapper does not exist!", NGAPP_SRC, 0x3C5, 0);
    size = zassetGetAssetSize(lib);
    s_intArt = zmemAlloc(size);
    zassetLoadAsset(lib, s_intArt);
    if (s_intArt == NULL)
        zAssertFail("Could not load menu art asset file", NGAPP_SRC, 0x3D1, 0);
    zassetCloseLibrary(lib);
    list = zfileGetDeviceList();
    if (list == NULL)
        zAssertFail("No devices!", NGAPP_SRC, 0x3D5, 0);
    for (i = 0; i < list->count; i++) {
        if (zstringCompare(zfileGetDeviceName(list->devices[i]), "rd00") == 0) {
            s_ramDisk = list->devices[i];
            break;
        }
    }
    if (s_ramDisk == (zfileDevice *)-1)
        zAssertFail("Could not locate a RAM disk device on this system!", NGAPP_SRC, 0x3E1, 0);
    keys[0] = "ADDRESS";
    keys[1] = "SIZE";
    values[0] = s_intArt;
    values[1] = (void *)(uint32_t)size;
    name = zfileMakeFileName(s_ramDisk, 2, keys, values);
    if (name == NULL)
        zAssertFail("Could not generate file name for RAM disk", NGAPP_SRC, 0x3EC, 0);
    zstringCopy(s_intArtName, name);
    guiSetArtLibrary(s_intArtName);
}

/* VA 0x0045b240, 29 bytes, [I] between, status draft */
void ngappFreeInterfaceArt(void)
{
    if (s_intArt != NULL) {
        zmemFree(s_intArt);
        s_intArt = NULL;
    }
}

/* VA 0x0045b270, 485 bytes, [I] strings, status draft */
int ngappInit(void)
{
    /* App_Start's user hook: render state, the scene camera (far clip 200, fog), the window title,
     * 4 MB static memory, the three intro movies (any key skips: input device 2, else 0), sound,
     * actors, the game and the front end. */
    RwRGBA black = { 0, 0, 0, 0 };
    float fps;
    float clip = 200.0f, fogEnd = 200.0f, fogStart = 100.0f;
    HWND *hwnd;
    ZInputDevice *dev;

    g_ngappShowFaces = g_ngappShowEdges = g_ngappWireframe = s_wireframeShown = 0;
    s_debugHandle = 0;
    g_zglSet(1, &fogStart, 0);
    g_zglSet(0x14, 1, 0);
    g_zglSet(0x10, 1, 0);
    g_ngappCamera = zobjCameraCreate(0);
    g_zglSet(7, zobjGetHandle(g_ngappCamera), 0);
    g_zglSet(8, zobjGetHandle(g_ngappCamera), &black);
    g_zglSet(5, zobjGetHandle(g_ngappCamera), &clip);
    g_zglSet(9, zobjGetHandle(g_ngappCamera), &fogEnd);
    g_zglSet(3, zobjGetHandle(g_ngappCamera), 1);
    g_zglSet(4, zobjGetHandle(g_ngappCamera), &black);
    g_zglGet(0x15, &hwnd, 0);
    SetWindowTextA(*hwnd, "Dave Mirra Freestyle BMX");
    fps = 60.0f;
    g_zglSet(0, &fps, 0);
    s_appTime = 0.0f;
    zmemStaticInit(0x400000);
    g_ngappIdleReset = 0;
    dev = zinputOpen(2);
    if (dev == NULL)
        dev = zinputOpen(0);
    zrwZglPlayMovie("bin\\movie\\ACCLAIM.AVI");
    zrwZglPlayMovie("bin\\movie\\ZAXIS.AVI");
    zrwZglPlayMovie("bin\\movie\\INTRO.AVI");
    if (dev != NULL)
        zinputClose(dev);
    g_ngappFreeList = zmemFreeListCreate(0x80, 0x100, NULL);
    bmxsoundOpen();
    zactorInit();
    actionStartup();
    frontendAllocMacros();
    return 1;
}

/* VA 0x0045b4e0, 172 bytes, [I] callers, status draft */
int ngappShutdown(void)
{
    /* App_Stop's user hook: tear down whichever side is running, then the shared systems */
    psxDisableDraw();
    if (g_ngappCamera != NULL)
        zobjDestroy(g_ngappCamera);
    if (g_ngappMode == NGAPP_MODE_ACTION) {
        if (g_zrollerFrameHandle != 0)
            g_zglRemoveFrameCallback(g_zrollerFrameHandle);
        g_zrollerFrameHandle = 0;
        actionShutdown();
        zsceneClose();
        g_zrollerLevelLoaded = 0;
        psxDisableDraw();
        zrollerUnloadLevel();
    }
    zresFreeImages();
    if (g_ngappMode == NGAPP_MODE_MENU) {
        frontendShutdownApp();
        ngappFreeInterfaceArt();
    }
    zHeapListDestroy(g_zrollerEdges);
    g_zrollerEdges = NULL;
    zactorCloseMotionLibrary();
    zresShutdown();
    zmemFreeListDestroy(g_ngappFreeList);
    bmxsoundClose();
    return 1;
}

/* VA 0x0045b5c0, 212 bytes, [I] strings, status draft */
void ngappShowLoadingScreen(void)
{
    /* intwrap.zal group 2 ("LOADING") shown on both buffers while the level unloads */
    ZassetLib *lib = zassetOpenLibrary(zfileBuildPath(g_assetsDir, s_intwrapName));
    uint32_t mat = 0;
    int mark;

    if (lib == NULL)
        return;
    mark = zmemTempMark();
    mat = loadIntwrapImage(lib, 2, "LOADING", mark);
    zassetCloseLibrary(lib);
    zmemTempRelease(mark);
    if (mat != 0) {
        showImage(mat);
        showImage(mat);
        g_pfnZglMatDestroy(mat);
    }
}

/* VA 0x0045b6e0, 343 bytes, [I] between, status draft */
int ngappDrawFullScreen(uint32_t material)
{
    /* one screen-sized textured quad, white, uv 0..1 */
    float w, h;
    ZglVertex v[4];
    ZglQuad q;

    g_zglGet(10, &w, 0);
    g_zglGet(11, &h, 0);
    zstringMemSet(v, 0, sizeof v);
    v[0].pos.x = 0.0f; v[0].pos.y = 0.0f; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].pos.x = 0.0f; v[1].pos.y = h;    v[1].u = 0.0f; v[1].v = 1.0f;
    v[2].pos.x = w;    v[2].pos.y = h;    v[2].u = 1.0f; v[2].v = 1.0f;
    v[3].pos.x = w;    v[3].pos.y = 0.0f; v[3].u = 1.0f; v[3].v = 0.0f;
    v[0].colour.red = v[0].colour.green = v[0].colour.blue = v[0].colour.alpha = 0xFF;
    v[1].colour = v[2].colour = v[3].colour = v[0].colour;
    q.material = material;
    q.v[0] = &v[0]; q.v[1] = &v[1]; q.v[2] = &v[2]; q.v[3] = &v[3];
    g_zglDrawQuads(ZGL_PRIM_2D, &q, 1, 0, 0);
    return 1;
}

/* VA 0x0045b8a0, 1541 bytes, [V] direct, status draft */
int ngappFrame(float dt)
{
    /* App_Frame's user hook. Returns 0 to quit. */
    RwRGBA zero = { 0, 0, 0, 0 };
    ZassetLib *lib;
    ZscnPoint *globals;
    const void *fog;
    float fps;
    int mark, i;
    int dev;

    s_appTime += dt;
    if (s_firstFrame == 1) {
        /* first frame: LEGAL for 3 s, then LEGAL2 while the front end loads (at least 3 s) */
        uint32_t size2;
        void *img;

        mark = zmemTempMark();
        lib = zassetOpenLibrary(zfileBuildPath(g_assetsDir, s_intwrapName));
        s_matLegal = loadIntwrapImage(lib, 1, "LEGAL", mark);
        s_legalTimer = g_zglGetTime();
        showImage(s_matLegal);
        waitMs(s_legalTimer, 3000);
        g_pfnZglMatDestroy(s_matLegal);
        s_legalTimer = g_zglGetTime();
        zassetNextGroup(lib);
        zassetNextGroup(lib);
        zassetNextGroup(lib);
        size2 = zassetGetAssetSize(lib);
        img = zmemTempAlloc(size2);     /* the exe allocates LEGAL's size here */
        zassetLoadAsset(lib, img);
        s_matLegal2 = g_zglMatCreateFromImage(img, size2, "LEGAL2", 0, 0);
        zassetCloseLibrary(lib);
        showImage(s_matLegal2);
        g_pfnZglMatDestroy(s_matLegal2);
        zmemTempRelease(mark);
        ngappLoadInterfaceArt();
        psxGetScreenSize();
        frontendInitApp();
        waitMs(s_legalTimer, 3000);
        s_firstFrame = -1;
        return 1;
    }

    ngappUpdateFarClip();
    pcinputPollDevices();

    /* the front end asks for a restart (g_ngappIdleReset): 60 frames later it is shut down and
     * started again */
    if (g_ngappMode == NGAPP_MODE_MENU) {
        if (g_ngappIdleReset == 0) {
            for (dev = 0; dev < 4; dev++)
                zinputGetDevice(dev);
            if (s_resetCountdown == 0) {
                s_resetting = 0;
                goto run;
            }
        } else {
            g_ngappIdleReset = 0;
            s_resetCountdown = 60;
        }
        s_resetCountdown--;
        s_resetting = 1;
        if (s_resetCountdown == 0) {
            frontendShutdownApp();
            for (i = 0; i < 8; i++)
                bmxsoundUpdate();
            g_appQuit = 1;
            frontendInitApp();
            for (i = 0; i < 8; i++)
                bmxsoundUpdate();
            return 1;
        }
    } else {
        s_resetCountdown = 0;
        s_resetting = 0;
    }

run:
    if (g_ngappMode == NGAPP_MODE_MENU) {
        for (;;) {
            if (frontendUpdateApp() != 0)
                return 0;                               /* quit */
            if (g_guiGroupLoaded != 0 || g_ngappMode == NGAPP_MODE_ACTION)
                break;
        }
        if (g_ngappMode == NGAPP_MODE_ACTION) {
            /* menu -> game: load the level behind the loading bar */
            frontendUpdateApp();
            frontendUpdateApp();
            frontendUpdateApp();
            g_zglSet(3, zobjGetHandle(g_ngappCamera), 1);
            frontendShutdownApp();
            s_levelMark = zmemTempMark();
            psxEnableDraw();
            psxBeginScene();
            ngappFreeInterfaceArt();
            lib = zassetOpenLibrary(zfileBuildPath(g_assetsDir, s_intwrapName));
            if (lib != NULL) {
                mark = zmemTempMark();
                s_matLoading = loadIntwrapImage(lib, g_numPlayers == 0 ? 3 : 2, "LOADING", mark);
                zassetCloseLibrary(lib);
                zmemTempRelease(mark);
            }
            s_loadProgress = 0;
            s_loadingHandle = g_zglAddFrameCallback(6, ngappDrawLoadingBar, (void *)(uint32_t)s_matLoading, 0);
            zmemCheck();
            if (g_numPlayers == 0)
                actionPickDemoLevel();                 /* attract mode */
            if (zsceneOpen(g_levelNames[g_currentLevel]) != 1)
                zAssertFail("Error loading level", NGAPP_SRC, 0x91F, 0);
            globals = zscnptsFind(zHashName("Globals"), 1);
            fog = (globals != NULL && globals->obji != NULL) ? (const void *)globals->obji : (const void *)&zero;
            g_zglSet(4, zobjGetHandle(g_ngappCamera), fog);   /* "Globals" OBJI: the fog colour */
            g_zglSet(8, zobjGetHandle(g_ngappCamera), &zero);
            ngappLinkSpineFaces();
            ngappCreateLevelLights();
            ngappApplyPropTints();
            ngappSetupLightProps();
            zactorInit();
            actionLoad();
            fps = 30.0f;
            g_zglSet(0, &fps, 0);
            zmemCheck();
            if (s_matLoading != 0)
                g_pfnZglMatDestroy(s_matLoading);
            g_zglRemoveFrameCallback(s_loadingHandle);
            zinputSetKeyLayout(contrlerGetPad(0)->device, g_keyLayouts[(int8_t)g_saveProfiles[0].unk_40[0xB]]);
            App_Frame();
            App_Frame();
            App_Frame();
            return 1;
        }
    } else {
        if (g_ngappMode != NGAPP_MODE_ACTION)
            return 1;
        actionGetMode();
        psxBeginScene();
        actionUpdate();
        ngappUpdateAmbientLight();
        if (g_ngappMode == NGAPP_MODE_MENU) {
            /* game -> menu */
            if (g_zrollerFrameHandle != 0)
                g_zglRemoveFrameCallback(g_zrollerFrameHandle);
            g_zrollerFrameHandle = 0;
            actionShutdown();
            zsceneClose();
            g_zrollerLevelLoaded = 0;
            ngappShowLoadingScreen();
            zmemCheck();
            psxDisableDraw();
            zrollerUnloadLevel();
            ngappLoadInterfaceArt();
            if (s_intArt == NULL)
                zAssertFail("Failed to load interface assets", NGAPP_SRC, 0x86B, 0);
            g_zglSet(3, zobjGetHandle(g_ngappCamera), 0);
            zmemTempRelease(s_levelMark);
            frontendInitApp();
            fps = 60.0f;
            g_zglSet(0, &fps, 0);
            zmemCheck();
            return 1;
        }
        NgIFzactor_PosePlayer();
    }
    bmxsoundUpdate();
    bmxsoundService();
    return 1;
}

/* VA 0x0045c040, 207 bytes, [I] callers, status draft */
void ngappUpdateAmbientLight(void)
{
    /* every third frame: the prelit colour of the ground below the rider, (c / 2 + 32), becomes
     * the ambient light colour */
    RwV3d from, to;
    uint8_t hit[92];
    uint8_t *c;

    if (++s_ambientFrame != 3)
        return;
    s_ambientFrame = 0;
    from = ((ZActor *)g_riders[g_currentPlayer].riderActor)->pos;
    to = from;
    to.y = from.y + 1000.0f;
    if (zrwColLine(&from, &to, 1, 0, hit) != 0 && s_ambientLight != NULL) {
        c = zrwColGetHitColour(hit);
        c[0] = (uint8_t)((c[0] >> 1) + 0x20);
        c[1] = (uint8_t)((c[1] >> 1) + 0x20);
        c[2] = (uint8_t)((c[2] >> 1) + 0x20);
        zobjLightSetColour(s_ambientLight, c);
    }
}

/* VA 0x0045c150, 1060 bytes, [I] between, status draft */
int ngappDrawLoadingBar(void *arg)
{
    /* Frame callback while the level loads: the LOADING picture and a progress bar (a dark frame
     * 100..540 x 320..360 and a light bar from 104 growing to 536 x 324..356). The bar is full
     * after the level's expected number of calls (s_loadSteps[level] callbacks, 4 per call). */
    static const uint16_t s_loadSteps[12] = { 0x180, 0x158, 0x13C, 0x124, 0x108, 0xE4,
                                              300, 0xDC, 0x10C, 0x114, 0x114, 0x11C };
    const RwRGBA dark = { 0x18, 0x29, 0x39, 0xFF };   /* 0xFF392918 */
    const RwRGBA light = { 0xB4, 0xAD, 0x42, 0xFF };  /* 0xFF42ADB4 */
    ZglVertex frame[4], bar[4];
    ZglQuad q[2];
    float sx, sy, right;
    int i;

    zrwCamBeginUpdate(zrwCamGetFirst());
    s_loadProgress = (uint16_t)(s_loadProgress + 4);
    right = (float)s_loadProgress * 436.0f / (float)s_loadSteps[g_currentLevel] + 104.0f;
    if (right > 536.0f)
        right = 536.0f;
    zstringMemSet(frame, 0, sizeof frame);
    zstringMemSet(bar, 0, sizeof bar);
    frame[0].pos.x = 100.0f; frame[0].pos.y = 360.0f;
    frame[1].pos.x = 540.0f; frame[1].pos.y = 360.0f;
    frame[2].pos.x = 540.0f; frame[2].pos.y = 320.0f;
    frame[3].pos.x = 100.0f; frame[3].pos.y = 320.0f;
    bar[0].pos.x = 104.0f;   bar[0].pos.y = 356.0f;
    bar[1].pos.x = right;    bar[1].pos.y = 356.0f;
    bar[2].pos.x = right;    bar[2].pos.y = 324.0f;
    bar[3].pos.x = 104.0f;   bar[3].pos.y = 324.0f;
    g_zglGet(10, &sx, 0);
    sx *= 1.0f / 640.0f;                               /* 0.0015625 */
    g_zglGet(11, &sy, 0);
    sy *= 1.0f / 480.0f;                               /* 0.0020833334 */
    for (i = 0; i < 4; i++) {
        frame[i].colour = dark;
        bar[i].colour = light;
        frame[i].pos.x *= sx; frame[i].pos.y *= sy;
        bar[i].pos.x *= sx;   bar[i].pos.y *= sy;
    }
    q[0].material = 0;
    q[0].v[0] = &frame[0]; q[0].v[1] = &frame[1]; q[0].v[2] = &frame[2]; q[0].v[3] = &frame[3];
    q[1].material = 0;
    q[1].v[0] = &bar[0]; q[1].v[1] = &bar[1]; q[1].v[2] = &bar[2]; q[1].v[3] = &bar[3];
    ngappDrawFullScreen((uint32_t)(uint32_t)arg);
    g_zglDrawQuads(ZGL_PRIM_2D, &q[0], 1, 0, 0);
    g_zglDrawQuads(ZGL_PRIM_2D, &q[1], 1, 0, 0);
    zrwCamEndUpdate();
    zrwZglShowFrame(1);
    return 1;
}

/* VA 0x0045c690, 40 bytes, [I] between, status draft */
void ngappLoadAnimTable(const char *name, int a, int b, void **table, int size)
{
    /* unreferenced: load a scene animation library and fill a table from it */
    ZsceneEntry *lib = zsceneLoadAnim(0, name);

    (void)a;
    (void)b;
    if (lib != NULL)
        zsceneAnimGetTable(lib, table, size);
}

/* VA 0x0045c6d0, 7 bytes, [I] between, status draft */
float ngappGetTime(void)
{
    return s_appTime;   /* unreferenced */
}

/* VA 0x0045c6f0, 1 bytes, [I] between, status draft */
void ngappNop(void)
{
}

/* VA 0x0045c710, 402 bytes, [I] callers, status draft */
void ngappUpdateFarClip(void)
{
    /* Adaptive draw distance: every 100 ms the frame rate is measured; above 26 fps with the clip
     * near 200 nothing changes, otherwise the far clip moves (fast while below 28 fps at first,
     * then by (fps - 28) * 0.2, or back toward 200 by 1/75 of the gap), clamped to 60..200. */
    uint32_t now = g_zglGetTime(), elapsed;
    int frames;

    if (s_fpsTime == 0) {
        ngappSetFarClip(s_farClip);
        s_fpsTime = now;
        return;
    }
    s_fpsFrames++;
    elapsed = now - s_fpsTime;
    if (elapsed < 100)
        return;
    frames = s_fpsFrames;
    s_fpsFrames = 0;
    s_fpsTime = now;
    s_fps = (float)frames / ((float)(int16_t)elapsed * 0.001f);
    if (s_fps > 26.0f && fabsf(s_farClip - 200.0f) < 10.0f) {
        s_fpsAdjusting = 0;
        return;
    }
    if (s_fpsAdjusting == 0)
        s_farClipStep = (s_fps - 28.0f) * 10.0f;
    else if (s_fps - 26.0f >= 0.0f)
        s_farClipStep = (200.0f - s_farClip) * 0.013333334f;
    else
        s_farClipStep = (s_fps - 28.0f) * 0.2f;
    s_farClip += s_farClipStep;
    s_fpsAdjusting = 1;
    if (s_farClip >= 200.0f) {
        s_farClip = 200.0f;
        ngappSetFarClip(200.0f);
    } else if (s_farClip > 60.0f) {
        ngappSetFarClip(s_farClip);
    } else {
        s_farClip = 60.0f;
        ngappSetFarClip(60.0f);
    }
}

/* VA 0x0045c920, 29 bytes, [I] callers, status draft */
void ngappSetFarClip(float distance)
{
    g_zglSet(5, zobjGetHandle(g_ngappCamera), &distance);
}

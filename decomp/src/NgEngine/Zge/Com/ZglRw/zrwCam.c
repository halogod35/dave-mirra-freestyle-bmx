/*
 * NgEngine/Zge/Com/ZglRw/zrwCam.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwCam.c
 * Evidence: [V] path string 0x00561E04 (asserts of zrwCamCreateFrameBuffers 0xF2/0xF8, zrwCamInit
 *           0x10A, SetResolution 0x149); "RwCam:" message; the camera driver table 0x00561D88 (15
 *           slots, all here). DM-059 moved here the frame-buffer helpers 0x004921B0-0x0049233F (the
 *           hole after zscnpts.c, same asserts) and the accessors 0x00493060-0x00493159 (from
 *           gap_zrwCam__pcinput, table slots 10, 11, 14).
 * 29 functions, VA 0x004921B0-0x00493159 (DM-059 created 11).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 7.
 *
 * ZGL cameras wrap RwCameras. The screen has one frame buffer + z buffer (g_zrwFrameBuffer /
 * g_zrwZBuffer, the resolution g_zrwScreenW x H); each camera gets sub-rasters of them for its
 * window. Defaults: fov 0x271C (55 degrees), view window tan(fov)/2 x 3/4 of that, near 0.5,
 * far 75, fog off (flags 2 + colour + distance from the far plane).
 */
#include "decomp.h"
#include "zmath.h"
#include "zglrw.h"

#define ZRWCAM_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwCam.c"
#define RWRASTER_ZBUFFER 1
#define RWRASTER_CAMERA  2

extern void    *g_zrwWorld;            /* 0x006976C8 */
extern void    *g_zrwFrameBuffer;      /* 0x006976C0 */
extern void    *g_zrwZBuffer;          /* 0x006976C4 */
extern float    g_zrwWorldOriginScaled[3]; /* 0x006976D8 origin / scale */
extern void    *g_zrwCamera;           /* 0x006976F4 the RwCamera being rendered */
extern int32_t  g_zrwInUpdate;         /* 0x006976F8 */
extern float    g_zrwScale;            /* 0x00697720 */
extern int32_t  g_zrwClearMode;        /* 0x0069772C */
extern RwRGBA   g_zrwClearColour;      /* 0x00697730 */
extern int32_t  g_zrwScreenW;          /* 0x00697770 */
extern int32_t  g_zrwScreenH;          /* 0x00697774 */
float zmathTan(int angle);             /* zmath.c */

static int32_t       s_initialized;   /* 0x00674018 */
static ZmemFreeList *s_pool;          /* 0x0067401C ZglCamera, 2 */
static ZglCamera    *s_current;       /* 0x00674020 */

/* VA 0x004921b0, 28 bytes, [I] DM-059 created, status draft (no caller found) */
int zrwCamBeginUpdateRaw(ZglCamera *cam)
{
    g_zrwCamera = cam->rwCamera;
    return RwCameraBeginUpdate(g_zrwCamera) != NULL;
}

/* VA 0x004921d0, 13 bytes, [I] DM-059 created, status draft */
void zrwCamEndUpdateRaw(void)
{
    RwCameraEndUpdate(g_zrwCamera);
}

/* VA 0x004921e0, 99 bytes, [I] DM-059 created, status draft (no caller found)
 * A sub-raster of `parent` for the rectangle (ints from the four floats of `rect`). */
void *zrwCamCreateSubRaster(void *parent, const float *rect, int32_t type)
{
    int32_t r[4];
    void *sub;
    r[0] = (int32_t)rect[0]; r[1] = (int32_t)rect[1];
    r[2] = (int32_t)rect[2]; r[3] = (int32_t)rect[3];
    sub = RwRasterCreate(0, 0, 0, type);
    RwRasterSubRaster(sub, parent, r);
    return sub;
}

/* VA 0x00492250, 35 bytes, [I] DM-059 created, status draft */
void zrwCamSetRasters(ZglCamera *cam, void *raster, void *zRaster)
{
    RwCameraSetRaster(cam->rwCamera, raster);
    RwCameraSetZRaster(cam->rwCamera, zRaster);
}

/* VA 0x00492280, 62 bytes, [I] DM-059 created, status draft (App_Open shutdown path) */
int zrwCamDestroyFrameBuffers(void)
{
    if (g_zrwZBuffer != NULL) {
        RwRasterDestroy(g_zrwZBuffer);
        g_zrwZBuffer = NULL;
    }
    if (g_zrwFrameBuffer != NULL) {
        RwRasterDestroy(g_zrwFrameBuffer);
        g_zrwFrameBuffer = NULL;
    }
    return 1;
}

/* VA 0x004922c0, 128 bytes, [I] DM-059 created, status draft (App_Open) */
int zrwCamCreateFrameBuffers(void)
{
    g_zrwFrameBuffer = RwRasterCreate(g_zrwScreenW, g_zrwScreenH, 0, RWRASTER_CAMERA);
    if (g_zrwFrameBuffer == NULL) {
        zAssertFail("Error creating camera raster", ZRWCAM_SRC, 0xF2, 0);
        return 0;
    }
    g_zrwZBuffer = RwRasterCreate(g_zrwScreenW, g_zrwScreenH, 0, RWRASTER_ZBUFFER);
    if (g_zrwZBuffer == NULL) {
        zAssertFail("Error creating camera z buffer", ZRWCAM_SRC, 0xF8, 0);
        return 0;
    }
    return 1;
}

/* VA 0x004924f0, 252 bytes, [I] DM-059 created, status draft (slot 13; only with no camera alive) */
int zrwCamSetResolution(int32_t width, int32_t height)
{
    ZmemFreeNode *n = zmemFreeListFirst(s_pool);
    if (n != NULL && zmemFreeListNodeData(n) != NULL) {
        zAssertFail("Can't set screen resoltion if cameras exist", ZRWCAM_SRC, 0x149, 0);
        return 0;
    }
    if (g_zrwZBuffer != NULL) {
        RwRasterDestroy(g_zrwZBuffer);
        g_zrwZBuffer = NULL;
        if (g_zrwFrameBuffer != NULL) {
            RwRasterDestroy(g_zrwFrameBuffer);
            g_zrwFrameBuffer = NULL;
        }
    }
    g_zrwScreenW = width;
    g_zrwScreenH = height;
    zrwCamCreateFrameBuffers();
    return 1;
}

/* VA 0x00492340, 317 bytes, [V] direct, status draft (slot 0; frame buffers at the current size,
 * then zrwShadowInit) */
int zrwCamInit(void)
{
    if (s_initialized)
        zAssertFail("Already initialized", ZRWCAM_SRC, 0x10A, 0);
    s_pool = zmemFreeListCreate(sizeof(ZglCamera), 2, (ZmemFreeListCallback)zrwCamRelease);
    zrwCamSetResolution(g_zrwScreenW, g_zrwScreenH);
    s_initialized = 1;
    return zrwShadowInit() != 0;
}

/* VA 0x00492480, 105 bytes, [I] DM-059 created, status draft (slot 1) */
int zrwCamShutdown(void)
{
    if (s_initialized) {
        if (!zrwShadowShutdown())
            return 0;
        zrwCamDestroyFrameBuffers();
        zmemFreeListDestroy(s_pool);
        s_initialized = 0;
    }
    return 1;
}

static void zrwCamDestroyRw(void *rwCam)
{
    void *r;
    if (RwCameraGetFrame(rwCam) != NULL)
        RwFrameDestroy(RwCameraGetFrame(rwCam));
    if ((r = RwCameraGetRaster(rwCam)) != NULL)
        RwRasterDestroy(r);
    if ((r = RwCameraGetZRaster(rwCam)) != NULL)
        RwRasterDestroy(r);
    RwCameraDestroy(rwCam);
}

/* VA 0x004925f0, 885 bytes, [V] direct, status draft (slot 2)
 * flags 1: parallel projection; 2: no rasters (the caller attaches them). */
ZglCamera *zrwCamCreate(uint32_t flags, void *user)
{
    ZglCamera *cam = (ZglCamera *)zmemFreeListAlloc(s_pool);
    void *rw, *raster, *zRaster;
    float rect[4], win[2];

    zstringMemSet(cam, 0, sizeof(ZglCamera));
    cam->user = user;
    cam->type = (cam->type & ~0xFFu) | 1;
    rw = RwCameraCreate();
    if (rw == NULL) {
        zmemFreeListFree(s_pool, cam);
        return NULL;
    }
    cam->rwCamera = rw;
    RwCameraSetFrame(rw, RwFrameCreate());
    if (RwCameraGetFrame(rw) == NULL)
        goto fail;
    RwFrameSetIdentity(RwCameraGetFrame(rw));
    if (flags & 1)
        RwCameraSetProjection(rw, 2 /* rwPARALLEL */);
    if (flags & 2)
        return cam;
    cam->flags |= 4;
    rect[0] = 0.0f; rect[1] = 0.0f;
    rect[2] = (float)g_zrwScreenW; rect[3] = (float)g_zrwScreenH;
    raster = zrwCamCreateSubRaster(g_zrwFrameBuffer, rect, RWRASTER_CAMERA);
    zRaster = zrwCamCreateSubRaster(g_zrwZBuffer, rect, RWRASTER_ZBUFFER);
    RwCameraSetRaster(cam->rwCamera, raster);
    RwCameraSetZRaster(cam->rwCamera, zRaster);
    if (RwCameraGetRaster(rw) != NULL && RwRasterGetParent(RwCameraGetRaster(rw)) != NULL &&
        RwCameraGetZRaster(rw) != NULL && RwRasterGetParent(RwCameraGetZRaster(rw)) != NULL) {
        zrwCamResize(cam);
        cam->fov = 0x271C;
        win[0] = zmathTan(0x271C) * 0.5f;
        win[1] = win[0] * 0.75f;
        RwCameraSetViewWindow(cam->rwCamera, win);
        cam->fogDistance = 1.0f;
        RwCameraSetFogDistance(cam->rwCamera, cam->farClip - 1.0f);
        cam->nearClip = 0.5f;
        RwCameraSetNearClipPlane(cam->rwCamera, 0.5f);
        cam->farClip = 75.0f;
        RwCameraSetFarClipPlane(cam->rwCamera, 75.0f);
        RwCameraSetFogDistance(cam->rwCamera, 75.0f - cam->fogDistance);
        return cam;
    }
fail:
    if (cam->rwCamera != NULL)
        zrwCamDestroyRw(cam->rwCamera);
    zmemFreeListFree(s_pool, cam);
    return NULL;
}

/* VA 0x00492970, 125 bytes, [I] callers, status draft (slot 3; pool destructor) */
int zrwCamRelease(ZglCamera *cam)
{
    if (cam->rwCamera != NULL) {
        zrwCamDestroyRw(cam->rwCamera);
        zmemFreeListFree(s_pool, cam);
    }
    return 1;
}

/* VA 0x004929f0, 112 bytes, [I] callers, status draft (0 keeps a plane; fog follows the far plane) */
void zrwCamSetClipPlanes(ZglCamera *cam, float nearClip, float farClip)
{
    if (nearClip != 0.0f) {
        cam->nearClip = nearClip;
        RwCameraSetNearClipPlane(cam->rwCamera, nearClip);
    }
    if (farClip != 0.0f) {
        cam->farClip = farClip;
        RwCameraSetFarClipPlane(cam->rwCamera, farClip);
        RwCameraSetFogDistance(cam->rwCamera, farClip - cam->fogDistance);
    }
}

/* VA 0x00492a60, 72 bytes, [I] callers, status draft (slot 4: 16-bit angle; 0 keeps it) */
void zrwCamSetFov(ZglCamera *cam, int fov)
{
    float win[2];
    if ((int16_t)fov != 0) {
        cam->fov = (int16_t)fov;
        win[0] = zmathTan(fov) * 0.5f;
        win[1] = win[0] * 0.75f;
        RwCameraSetViewWindow(cam->rwCamera, win);
    }
}

/* VA 0x00492ab0, 12 bytes, [I] DM-059 created, status draft (slot 5) */
int16_t zrwCamGetFov(ZglCamera *cam)
{
    return cam->fov;
}

/* VA 0x00492ac0, 324 bytes, [I] callers, status draft (slot 6)
 * ZGE camera matrix + position -> the RW frame (axes re-signed for RW's handedness, position scaled
 * from the world origin); the RW matrix is also kept at +0x14. */
void zrwCamSetMatrix(ZglCamera *cam, const RwMatrix *m, const float *pos)
{
    void *frame;
    RwMatrix *f;

    zmathMatrixCopy(&cam->zgeMatrix, m);
    cam->zgeMatrix.pos.x = pos[0];
    cam->zgeMatrix.pos.y = pos[1];
    cam->zgeMatrix.pos.z = pos[2];
    frame = RwCameraGetFrame(cam->rwCamera);
    f = RwFrameGetMatrix(frame);
    RwFrameSetIdentity(frame);
    f->right.x = -m->right.x; f->right.y = -m->up.x;  f->right.z = m->at.x;
    f->up.x    = -m->right.y; f->up.y    = m->up.y;   f->up.z    = m->at.y;
    f->at.x    = m->right.z;  f->at.y    = -m->up.z;  f->at.z    = -m->at.z;
    f->pos.x = (pos[0] + g_zrwWorldOriginScaled[0]) * g_zrwScale;
    f->pos.y = g_zrwScale * (g_zrwWorldOriginScaled[1] - pos[1]);
    f->pos.z = g_zrwScale * (g_zrwWorldOriginScaled[2] - pos[2]);
    RwMatrixUpdate(f);
    cam->rwMatrix = *f;
}

/* VA 0x00492c10, 8 bytes, [I] DM-059 created, status draft (slot 7) */
RwMatrix *zrwCamGetMatrix(ZglCamera *cam)
{
    return &cam->zgeMatrix;
}

/* VA 0x00492c20, 145 bytes, [I] callers, status draft (slot 8)
 * Camera-space point -> screen pixels (perspective divide, centre, RW view offset scale); 0 when
 * outside the clip planes. */
int zrwCamProject(ZglCamera *cam, const float *p, float *out)
{
    const float *vo;
    if (!(cam->nearClip < p[2] && p[2] < cam->farClip))
        return 0;
    vo = (const float *)RwCameraGetViewWindow(cam->rwCamera);
    out[0] = (float)cam->width * (1.0f / p[2]) * p[0];
    out[1] = (float)cam->height * (1.0f / p[2]) * p[1];
    out[0] = cam->centreX + out[0];
    out[1] = cam->centreY + out[1];
    out[0] = vo[0] * out[0];
    out[1] = vo[1] * out[1];
    return 1;
}

/* VA 0x00492cc0, 277 bytes, [I] callers, status draft (slot 9)
 * Re-cuts the camera's sub-rasters for its window (+0x94..+0xA0) and recomputes size and centre. */
void zrwCamResize(ZglCamera *cam)
{
    int32_t r[4];
    void *ras;
    float win[2];

    cam->width = (int32_t)(cam->window[2] - cam->window[0]);
    cam->height = (int32_t)(cam->window[3] - cam->window[1]);
    r[0] = (int32_t)cam->window[0];
    r[1] = (int32_t)cam->window[1];
    r[2] = cam->width;
    r[3] = cam->height;
    if ((ras = RwCameraGetRaster(cam->rwCamera)) != NULL)
        RwRasterSubRaster(ras, RwRasterGetParent(ras), r);
    if ((ras = RwCameraGetZRaster(cam->rwCamera)) != NULL)
        RwRasterSubRaster(ras, RwRasterGetParent(ras), r);
    if (cam->fov != 0) {
        win[0] = zmathTan(cam->fov) * 0.5f;
        win[1] = win[0] * 0.75f;
        RwCameraSetViewWindow(cam->rwCamera, win);
    }
    ras = RwCameraGetRaster(cam->rwCamera);
    cam->centreX = (float)*(int16_t *)((uint8_t *)ras + 0x1C);
    cam->centreY = (float)*(int16_t *)((uint8_t *)ras + 0x1E);
}

/* VA 0x00492de0, 207 bytes, [I] callers, status draft (slot 12)
 * The window: NULL = full screen; flags bit 0 = rect given as fractions of the screen. */
void zrwCamSetWindow(ZglCamera *cam, uint32_t flags, const float *rect)
{
    if (rect == NULL) {
        cam->window[0] = 0.0f;
        cam->window[1] = 0.0f;
        cam->window[2] = (float)cam->width;
        cam->flags &= ~1u;
        cam->window[3] = (float)cam->height;
        return;
    }
    cam->flags |= 1;
    if ((flags & 1) == 0) {
        cam->window[0] = rect[0];
        cam->window[1] = rect[1];
        cam->window[2] = rect[2] + cam->window[0];
        cam->window[3] = rect[3] + cam->window[1];
    } else {
        cam->window[0] = (float)cam->width * rect[0];
        cam->window[1] = (float)cam->height * rect[1];
        cam->window[2] = (float)cam->width * rect[2] + cam->window[0];
        cam->window[3] = (float)cam->height * rect[3] + cam->window[1];
    }
}

/* VA 0x00492eb0, 31 bytes, [I] callers, status draft */
ZglCamera *zrwCamGetFirst(void)
{
    ZmemFreeNode *n = zmemFreeListFirst(s_pool);
    return n == NULL ? NULL : (ZglCamera *)zmemFreeListNodeData(n);
}

/* VA 0x00492ed0, 36 bytes, [I] callers, status draft */
ZglCamera *zrwCamGetNext(ZglCamera *cam)
{
    ZmemFreeNode *n = zmemFreeListNext(zmemFreeListDataNode(cam));
    return n == NULL ? NULL : (ZglCamera *)zmemFreeListNodeData(n);
}

/* VA 0x00492f00, 220 bytes, [I] callers, status draft
 * Starts rendering through this camera: into the world, RwCameraBeginUpdate, raster context, default
 * render state, fog (flag 2: colour +0xC0, linear [I]). */
int zrwCamBeginUpdate(ZglCamera *cam)
{
    void *rw = cam->rwCamera;
    s_current = cam;
    RpWorldAddCamera(g_zrwWorld, rw);
    RpLODAtomicSetCamera(g_zrwCamera);         /* [I] 0x005065D0: a global of the LOD-atomic plugin */
    g_zrwCamera = cam->rwCamera;
    if (RwCameraBeginUpdate(g_zrwCamera) == NULL)
        return 0;
    if (RwRasterPushContext(RwCameraGetRaster(rw)) == NULL)
        return 0;
    zrwZglResetRenderState();
    if ((cam->flags & 2) == 0)
        RwRenderStateSet(0xC /* fog enable */, 0);
    else {
        RwRenderStateSet(0xC, 1);
        RwRenderStateSet(0xD /* fog colour */, (cam->fogColour.alpha << 24) | (cam->fogColour.red << 16) |
                                               (cam->fogColour.green << 8) | cam->fogColour.blue);
        RwRenderStateSet(0xE /* fog type */, 1);
    }
    g_zrwInUpdate = 1;
    return 1;
}

/* VA 0x00492fe0, 114 bytes, [I] callers, status draft */
int zrwCamEndUpdate(void)
{
    if (g_zrwCamera == NULL) {
        g_pfnDebugPrintf("RwCam: No camera\n");
        return 0;
    }
    RwRenderStateSet(0xC, 0);
    g_zrwInUpdate = 0;
    RwRasterPopContext();
    RwCameraEndUpdate(g_zrwCamera);
    RpWorldRemoveCamera(RwCameraGetWorld(g_zrwCamera), g_zrwCamera);
    g_zrwCamera = NULL;
    s_current = NULL;
    return 1;
}

/* VA 0x00493060, 8 bytes, unassigned, status draft (the RwCamera) */
void *zrwCamGetRwCamera(ZglCamera *cam) { return cam->rwCamera; }

/* VA 0x00493070, 6 bytes, unassigned, status draft (slot 10) */
ZglCamera *zrwCamGetCurrent(void) { return s_current; }

/* VA 0x00493080, 8 bytes, unassigned, status draft (the RW matrix copy) */
RwMatrix *zrwCamGetRwMatrix(ZglCamera *cam) { return &cam->rwMatrix; }

/* VA 0x00493090, 8 bytes, unassigned, status draft (slot 11) */
void *zrwCamGetUser(ZglCamera *cam) { return cam->user; }

/* VA 0x004930a0, 10 bytes, unassigned, status draft (the frame clear mode, one global) */
void zrwCamSetClearMode(ZglCamera *cam, int32_t mode) { (void)cam; g_zrwClearMode = mode; }

/* VA 0x004930b0, 13 bytes, unassigned, status draft (the clear colour, one global) */
void zrwCamSetClearColour(ZglCamera *cam, const RwRGBA *c) { (void)cam; g_zrwClearColour = *c; }

/* VA 0x004930c0, 25 bytes, unassigned, status draft (fog on/off = flag 2) */
void zrwCamSetFog(ZglCamera *cam, uint32_t on)
{
    cam->flags = ((on & 1) << 1) | (cam->flags & ~2u);
}

/* VA 0x004930e0, 17 bytes, unassigned, status draft */
void zrwCamSetFogColour(ZglCamera *cam, const RwRGBA *c) { cam->fogColour = *c; }

/* VA 0x00493100, 41 bytes, unassigned, status draft (fog starts `d` before the far plane) */
void zrwCamSetFogDistance(ZglCamera *cam, float d)
{
    cam->fogDistance = d;
    RwCameraSetFogDistance(cam->rwCamera, cam->farClip - d);
}

/* VA 0x00493130, 41 bytes, unassigned, status draft (slot 14: camera shake) */
void zrwCamSetViewOffset(ZglCamera *cam, float x, float y)
{
    float v[2];
    v[0] = x;
    v[1] = y;
    RwCameraSetViewOffset(cam->rwCamera, v);
}

const ZglCameraApi g_zglCameraApi = {                                       /* 0x00561D88 */
    zrwCamInit, zrwCamShutdown, zrwCamCreate, zrwCamRelease, zrwCamSetFov, zrwCamGetFov,
    zrwCamSetMatrix, zrwCamGetMatrix, zrwCamProject, zrwCamResize, zrwCamGetCurrent, zrwCamGetUser,
    zrwCamSetWindow, zrwCamSetResolution, zrwCamSetViewOffset
};

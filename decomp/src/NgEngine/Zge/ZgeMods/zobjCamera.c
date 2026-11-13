/*
 * NgEngine/Zge/ZgeMods/zobjCamera.c
 * Original: C:\NgEngine\Zge\ZgeMods\zobjCamera.c
 * Evidence: [V] path string referenced by 2 function(s); the others [I] (the vtable 0x00560518, the
 * globals 0x006702F8-0x006702FC). The ZGE object pass created the two hole functions 0x00482180 and 0x00482280.
 * 11 functions, VA 0x00482090-0x00482405.
 * Draft. Non-matching C (AD-8). Types: zobj.h. Description: docs/re/zge.md 2.3.
 *
 * A camera zobj wraps a zrwCam. Its data block holds four matrices derived from the object's
 * position and rotation, rebuilt lazily by zobjCameraGetMatrix; setting the position or rotation
 * clears the valid mask. The update method hands the view matrix to the renderer every frame.
 */
#include "decomp.h"
#include "zobj.h"

#define ZOBJCAMERA_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zobjCamera.c"

void zmathMatrixLookAt(RwMatrix *out, const RwV3d *eye, const RwV3d *at, const RwV3d *up); /* 0x00495860 look-at matrix (gap_zrwCol__zobjPEmit.c) */

static int           g_zobjCameraInitialised;  /* 0x006702F8 */
static ZmemFreeList *g_zobjCameraPool;         /* 0x006702FC ZobjCameraData, 4 per block */
static const RwV3d   s_zobjCameraEye = { 0.0f, 0.0f, -100.0f };   /* 0x00560540 */
static RwV3d         s_zobjCameraAt;                               /* 0x00670300 (0, 0, 0) */

const ZobjVtbl g_zobjCameraVtbl = {     /* 0x00560518 */
    zobjCameraDestroy, zobjCameraUpdate, zobjCameraSetPosition, zobjCameraSetRotation,
    0, 0, 0, 0, 0
};

/* VA 0x00482090, 41 bytes, [I] row, status draft */
int zobjCameraDestroy(ZObj *cam)
{
    g_zrwCamDestroy(cam->handle);
    zmemFreeListFree(g_zobjCameraPool, cam->data);
    return 1;
}

/* VA 0x004820c0, 38 bytes, [I] row, status draft */
int zobjCameraUpdate(ZObj *cam)
{
    g_zrwCamSetView(cam->handle, zobjCameraGetMatrix(cam, ZOBJCAM_VIEW), &cam->pos);
    return 1;
}

/* VA 0x004820f0, 23 bytes, [I] row, status draft */
int zobjCameraSetPosition(ZObj *cam, const RwV3d *pos)
{
    ((ZobjCameraData *)cam->data)->valid = 0;
    return 1;
}

/* VA 0x00482110, 23 bytes, [I] row, status draft */
int zobjCameraSetRotation(ZObj *cam, const Zquat *rot)
{
    ((ZobjCameraData *)cam->data)->valid = 0;
    return 1;
}

/* VA 0x00482130, 78 bytes, [V] direct, status draft */
int zobjCameraInit(void)
{
    if (g_zobjCameraInitialised)
        zAssertFail("Already initialized", ZOBJCAMERA_FILE, 0xA9, 0);
    if (!g_zrwCamInit())
        return 0;
    g_zobjCameraPool = zmemFreeListCreate(sizeof(ZobjCameraData), 4, 0);
    g_zobjCameraInitialised = 1;
    return 1;
}

/* VA 0x00482180, 50 bytes, [I] row, status draft */
int zobjCameraShutdown(void)
{
    if (g_zobjCameraInitialised) {
        if (!g_zrwCamShutdown())
            return 0;
        zmemFreeListDestroy(g_zobjCameraPool);
        g_zobjCameraInitialised = 0;
    }
    return 1;
}

/* VA 0x004821c0, 181 bytes, [I] row, status draft */
ZObj *zobjCameraCreate(void)
{
    ZObj *cam;
    RwMatrix m;
    Zquat q;

    cam = zobjAlloc();
    if (cam == 0)
        return 0;
    zobjConstruct(cam, ZOBJ_TYPE_CAMERA);
    cam->vtbl = &g_zobjCameraVtbl;
    cam->handle = g_zrwCamCreate(0, cam);
    if (cam->handle == 0) {
        zobjDestroy(cam);
        return 0;
    }
    cam->data = zmemFreeListAlloc(g_zobjCameraPool);
    zstringMemSet(cam->data, 0, sizeof(ZobjCameraData));
    g_zrwCamSetFov(cam->handle, 0x271C);            /* 55 degrees in 16-bit angle units */
    /* default: at (0, 0, -100) looking at the origin */
    zmathMatrixLookAt(&m, &s_zobjCameraEye, &s_zobjCameraAt, 0);
    zquatFromMatrix(&q, &m);
    zobjSetPosition(cam, &s_zobjCameraEye);
    zobjSetRotation(cam, &q);
    return cam;
}

/* VA 0x00482280, 23 bytes, [I] row, status draft */
void zobjCameraSetViewport(ZObj *cam, const float *rect)
{
    g_zrwCamSetViewport(cam->handle, rect);
}

/* VA 0x004822a0, 23 bytes, [I] row, status draft */
void zobjCameraSetFov(ZObj *cam, int angle)
{
    g_zrwCamSetFov(cam->handle, angle);
}

/* VA 0x004822c0, 74 bytes, [I] row, status draft */
void zobjCameraLookAt(ZObj *cam, const RwV3d *eye, const RwV3d *at, const RwV3d *up)
{
    RwMatrix m;
    Zquat q;

    zmathMatrixLookAt(&m, eye, at, up);
    zquatFromMatrix(&q, &m);
    zobjSetPosition(cam, eye);
    zobjSetRotation(cam, &q);
}

/* VA 0x00482310, 246 bytes, [V] direct, status draft */
RwMatrix *zobjCameraGetMatrix(ZObj *cam, int which)
{
    ZobjCameraData *d = (ZobjCameraData *)cam->data;
    uint32_t bit = 1u << which;

    if (d->valid & bit)
        return &d->matrix[which];
    switch (which) {
    case ZOBJCAM_VIEW:
        zquatToMatrix(&d->matrix[0], &cam->rot);
        zmathTransformVector(&d->matrix[0].pos, &d->matrix[0], &cam->pos);
        zmathVecNegate(&d->matrix[0].pos, &d->matrix[0].pos);
        break;
    case ZOBJCAM_WORLD:
        zobjCameraGetMatrix(cam, ZOBJCAM_VIEW);
        zmathMatrixInvertRigid(&d->matrix[1], &d->matrix[0]);
        break;
    case ZOBJCAM_ROTATION:
        zobjCameraGetMatrix(cam, ZOBJCAM_VIEW);
        zmathMatrixTranspose(&d->matrix[2], &d->matrix[0]);
        zmathMatrixClearTranslation(&d->matrix[2]);
        break;
    case ZOBJCAM_YAW:
        zobjCameraGetMatrix(cam, ZOBJCAM_VIEW);
        zmathMatrixIdentity(&d->matrix[3]);
        zmathMatrixRotateY(&d->matrix[3], &d->matrix[3],
                           -(int16_t)zmathAtan2(d->matrix[0].at.x, d->matrix[0].at.z));
        break;
    default:
        zAssertFail("Unknown type", ZOBJCAMERA_FILE, 0x16A, 0);
        return 0;
    }
    d->valid |= bit;
    return &d->matrix[which];
}

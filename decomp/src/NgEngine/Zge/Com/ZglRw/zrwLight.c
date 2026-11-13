/*
 * NgEngine/Zge/Com/ZglRw/zrwLight.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwLight.c
 * Evidence: [V] path string 0x00563448 (assert of zrwLightInit 0xA9); the light driver table
 *           0x00563420 (10 slots, all here: the ZglRw pass moved 0x0049F550-0x0049F840 from
 *           gap_zrwLight__zge.c and created the other 4).
 * 10 functions, VA 0x0049F4D0-0x0049F8FF. Draft. Non-matching C (AD-8). Types: zglrw.h.
 *
 * ZGL lights = RenderWare RpLights in the world (light list of 2), each with its own frame.
 */
#include "decomp.h"
#include "zglrw.h"

#define ZRWLIGHT_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwLight.c"

extern void *g_zrwWorld;           /* 0x006976C8 */
extern float g_zrwWorldOrigin[3];  /* 0x006976CC */
extern float g_zrwScale;           /* 0x00697720 */

/* ZGL light type -> rpLIGHT*: directional, ambient, point, spot, soft spot [V] 0x00563408 */
static const int32_t s_rwLightType[5] = { 1, 2, 0x80, 0x81, 0x82 };
static int32_t       s_initialized;     /* 0x00675370 */
static ZmemFreeList *s_pool;            /* 0x00675374 */

static void zrwLightColour(void *rpLight, const uint8_t *rgba)
{
    float c[4];
    c[0] = rgba[0] * (1.0f / 255.0f);
    c[1] = rgba[1] * (1.0f / 255.0f);
    c[2] = rgba[2] * (1.0f / 255.0f);
    c[3] = rgba[3] * (1.0f / 255.0f);
    RpLightSetColor(rpLight, c);
}

/* VA 0x0049f4d0, 72 bytes, [V] direct, status draft (light slot 0) */
int zrwLightInit(void)
{
    if (s_initialized)
        zAssertFail("Already initialized", ZRWLIGHT_SRC, 0xA9, 0);
    s_initialized = 1;
    s_pool = zmemFreeListCreate(sizeof(ZglLight), 2, (ZmemFreeListCallback)zrwLightRelease);
    return 1;
}

/* VA 0x0049f520, 39 bytes, [I] new function, status draft (slot 1) */
int zrwLightShutdown(void)
{
    if (s_initialized) {
        zmemFreeListDestroy(s_pool);
        s_initialized = 0;
    }
    return 1;
}

/* VA 0x0049f550, 377 bytes, unassigned, status draft (slot 2)
 * A light of ZGL type 0..4 with an RGBA colour: radius 0.02 ZGE units scaled, cone 45 degrees, at
 * the world origin, lighting atomics and the world, added to the world. */
ZglLight *zrwLightCreate(int type, const uint8_t *rgba)
{
    ZglLight *l = (ZglLight *)zmemFreeListAlloc(s_pool);
    void *frame;
    RwV3d p;

    zstringMemSet(l, 0, sizeof(ZglLight));
    l->type = (l->type & ~0xFFu) | 2;
    if (l == NULL)
        return NULL;
    l->rpLight = RpLightCreate(s_rwLightType[type]);
    if (l->rpLight == NULL) {
        zmemFreeListFree(s_pool, l);
        return NULL;
    }
    RpLightSetRadius(l->rpLight, g_zrwScale * 0.02f);
    RpLightSetConeAngle(l->rpLight, 0.7853982f);
    zrwLightColour(l->rpLight, rgba);
    frame = RwFrameCreate();
    RwFrameSetIdentity(frame);
    RpLightSetFrame(l->rpLight, frame);
    p.x = g_zrwWorldOrigin[0] + 0.0f;
    p.y = g_zrwWorldOrigin[1] - 0.0f;
    p.z = g_zrwWorldOrigin[2] - 0.0f;
    RwFrameTranslate(RpLightGetFrame(l->rpLight), &p, 0);
    RpLightSetFlags(l->rpLight, 3 /* rpLIGHTLIGHTATOMICS | rpLIGHTLIGHTWORLD */);
    RpWorldAddLight(g_zrwWorld, l->rpLight);
    return l;
}

/* VA 0x0049f6d0, 68 bytes, unassigned, status draft (slot 3; pool destructor) */
int zrwLightRelease(ZglLight *l)
{
    RpWorldRemoveLight(g_zrwWorld, l->rpLight);
    RwFrameDestroy(RpLightGetFrame(l->rpLight));
    RpLightDestroy(l->rpLight);
    zmemFreeListFree(s_pool, l);
    return 1;
}

/* VA 0x0049f720, 127 bytes, unassigned, status draft (slot 4) */
int zrwLightSetColour(ZglLight *l, const uint8_t *rgba)
{
    zrwLightColour(l->rpLight, rgba);
    return 1;
}

/* VA 0x0049f7a0, 45 bytes, unassigned, status draft (slot 5: 1 atomics, 2 world) */
int zrwLightSetFlags(ZglLight *l, uint32_t flags)
{
    RpLightSetFlags(l->rpLight, ((flags & 1) ? 1 : 0) | ((flags & 2) ? 2 : 0));
    return 1;
}

/* VA 0x0049f7d0, 100 bytes, [I] new function, status draft (slot 6: ZGE position) */
int zrwLightSetPosition(ZglLight *l, const float *pos)
{
    RwV3d p;
    p.x = g_zrwScale * pos[0] + g_zrwWorldOrigin[0];
    p.y = g_zrwWorldOrigin[1] - pos[1] * g_zrwScale;
    p.z = g_zrwWorldOrigin[2] - pos[2] * g_zrwScale;
    RwFrameTranslate(RpLightGetFrame(l->rpLight), &p, 0);
    return 1;
}

/* VA 0x0049f840, 88 bytes, unassigned, status draft (slot 7: the frame's at vector = ZGE dir) */
int zrwLightSetDirection(ZglLight *l, const float *dir)
{
    RwMatrix *m = RwFrameGetMatrix(RpLightGetFrame(l->rpLight));
    m->at.x = dir[0];
    m->at.y = -dir[1];
    m->at.z = -dir[2];
    RwMatrixUpdate(m);
    return 1;
}

/* VA 0x0049f8a0, 44 bytes, [I] new function, status draft (slot 8: 16-bit angle) */
int zrwLightSetConeAngle(ZglLight *l, int16_t angle)
{
    RpLightSetConeAngle(l->rpLight, (float)angle * 9.58738e-05f /* 2 pi / 65536 */);
    return 1;
}

/* VA 0x0049f8d0, 35 bytes, [I] new function, status draft (slot 9: ZGE units) */
int zrwLightSetRadius(ZglLight *l, float radius)
{
    RpLightSetRadius(l->rpLight, g_zrwScale * radius);
    return 1;
}

const ZglLightApi g_zglLightApi = {                                         /* 0x00563420 */
    zrwLightInit, zrwLightShutdown, zrwLightCreate, zrwLightRelease, zrwLightSetColour,
    zrwLightSetFlags, zrwLightSetPosition, zrwLightSetDirection, zrwLightSetConeAngle,
    zrwLightSetRadius
};

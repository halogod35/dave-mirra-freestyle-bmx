/*
 * _unassigned/gap_zrwCol__zobjPEmit.c
 * Original: (unknown). Two groups, both outside ZglRw [I]:
 *   0x00495370-0x004956EF  the engine's zfx particle base: the default ZfxType slots (tables
 *                          0x0055C278.. point here, NgIFfx.c fills the rest) - a particle system
 *                          object, its affector list, the generic update; no path string
 *   0x004956F0-0x00495A0F  zmath helpers (plane, reflection, look-at, from-to rotation)
 * 14 functions, VA 0x00495370-0x00495A07 (7 new functions). The clean-up moved the zobjPEmit methods
 * 0x00495A10 / 0x00495A60 to zobjPEmit.c.
 * Why this stays a gap: no path string, literal or shared global ties these to one file.
 * The zfx base's callers are NgIFfx.c's type tables, the zmath helpers' callers are zobjCamera.c,
 * zrwCam.c, zcompres.c and NgIFfx.c, and zmath.c (0x00480D20-0x00482081) is far away in link order.
 * The likeliest owner of the zfx base is a stubbed engine file "zfx.c" [I], which would link here.
 * Draft. Non-matching C (AD-8). Types: ngif.h (ZfxType, ZfxSystem), zmath.h.
 */
#include "decomp.h"
#include "zmath.h"
#include "ngif.h"

double fabs(double x);
void  zmathVecLerp(RwV3d *out, const RwV3d *a, const RwV3d *b, float t);            /* zmath.c 0x004817E0 */
extern uint32_t (__cdecl *g_zglGetTime)(void);     /* 0x00561480 */
int zrwZglRemoveCallback(void *node);              /* zrwZgl.c */

/* a zfx affector instance in a type's list (zfx type +0x24, zList) */
typedef struct ZfxAffectorLink {
    zListNode    node;     /* +0x00 */
    void *const *affector; /* +0x08 { init, apply } */
    const void  *params;   /* +0x0C */
} ZfxAffectorLink;         /* 0x10 */
#define ZFX_AFFECTORS(t) ((zList *)((uint8_t *)(t) + 0x24))

/* VA 0x00495370, 15 bytes, [I] new function, status draft (no caller found) */
void zfxTypeInitAffectors(ZfxType *type)
{
    zListInit(ZFX_AFFECTORS(type));
}

/* VA 0x00495380, 10 bytes, unassigned, status draft (ZfxType slot 0: destroy = clear affectors) */
void zfxTypeDestroy(ZfxType *type)
{
    ((void (__cdecl *)(ZfxType *))type->fn0c)(type);
}

/* VA 0x00495390, 77 bytes, [I] new function, status draft (slot 4)
 * A particle system: a list and a free list of `capacity` 0x60-byte particles. */
ZfxSystem *zfxSystemCreate(void *owner, uint32_t capacity)
{
    ZfxSystem *sys = (ZfxSystem *)zmemAlloc(sizeof(ZfxSystem));
    if (sys == NULL)
        return NULL;
    sys->particles = zHeapListCreate(capacity & 0xFFFF);
    sys->pool = zmemFreeListCreate(0x60, capacity & 0xFFFF, NULL);
    sys->owner = owner;
    sys->lastTime = g_zglGetTime();
    return sys;
}

/* VA 0x004953e0, 40 bytes, unassigned, status draft (slot 5) */
void zfxSystemDestroy(ZfxType *type, ZfxSystem *sys)
{
    (void)type;
    if (sys->particles != NULL)
        zHeapListDestroy(sys->particles);
    zmemFreeListDestroy(sys->pool);
    zmemFree(sys);
}

/* VA 0x00495410, 39 bytes, [I] new function, status draft (slot 1) */
void zfxTypeAddAffector(ZfxType *type, void *const *affector, const void *params)
{
    ZfxAffectorLink *l = (ZfxAffectorLink *)zmemAlloc(sizeof(ZfxAffectorLink));
    l->affector = affector;
    l->params = params;
    zListAddTail(ZFX_AFFECTORS(type), &l->node);
}

/* VA 0x00495440, 41 bytes, [I] new function, status draft (slot 3) */
void zfxTypeClearAffectors(ZfxType *type)
{
    zListNode *n;
    while ((n = ZFX_AFFECTORS(type)->head) != NULL) {
        zListRemove(ZFX_AFFECTORS(type), n);
        zmemFree(n);
    }
}

/* VA 0x00495470, 56 bytes, unassigned, status draft (slot 2: an affector registered by name) */
void zfxTypeAddAffectorByName(ZfxType *type, const char *name, const void *params)
{
    void *const *affector = (void *const *)zfxFindAffector(name);
    if (affector != NULL) {
        ZfxAffectorLink *l = (ZfxAffectorLink *)zmemAlloc(sizeof(ZfxAffectorLink));
        l->affector = affector;
        l->params = params;
        zListAddTail(ZFX_AFFECTORS(type), &l->node);
    }
}

/* VA 0x004954b0, 465 bytes, [I] new function, status draft (slot 7: the default update)
 * Moves every live particle (flag 1: pos += vel * dt / 1000, then pos += vel [V]), spins it (flag
 * 2: euler +0x44 applied to +0x34), recomputes the bounding box, frees expired ones (birth +0x5C,
 * life +0x58), then runs the type's affectors. Returns the number of particles visited. */
int zfxSystemUpdate(ZfxType *type, ZfxSystem *sys)
{
    uint32_t now = g_zglGetTime();
    uint32_t last = sys->lastTime;
    int count = 0;
    zListNode *n;

    if (sys != NULL && sys->particles != NULL) {
        uint8_t *p;
        sys->boundMin.x = sys->boundMin.y = sys->boundMin.z = 100000.0f;
        sys->boundMax.x = sys->boundMax.y = sys->boundMax.z = -100000.0f;
        p = (uint8_t *)zHeapListFirst(sys->particles);
        while (p != NULL) {
            count++;
            if (p[0] & 1) {
                RwV3d *pos = (RwV3d *)(p + 4), *vel = (RwV3d *)(p + 0x24);
                float dt = (float)(now - last);
                *(RwV3d *)(p + 0x14) = *pos;            /* previous position */
                pos->x = dt * vel->x * 0.001f + pos->x;
                pos->y = dt * vel->y * 0.001f + pos->y;
                pos->z = dt * vel->z * 0.001f + pos->z;
                zmathVecAdd(pos, pos, vel);
                if (sys->boundMax.x < pos->x) sys->boundMax.x = pos->x;
                if (sys->boundMax.y < pos->y) sys->boundMax.y = pos->y;
                if (sys->boundMax.z < pos->z) sys->boundMax.z = pos->z;
                if (pos->x < sys->boundMin.x) sys->boundMin.x = pos->x;
                if (pos->y < sys->boundMin.y) sys->boundMin.y = pos->y;
                if (pos->z < sys->boundMin.z) sys->boundMin.z = pos->z;
            }
            if (p[0] & 2)
                zmathVecRotateXYZ((RwV3d *)(p + 0x34), (RwV3d *)(p + 0x34), (uint16_t *)(p + 0x44));
            if (now - *(uint32_t *)(p + 0x5C) < *(uint32_t *)(p + 0x58))
                p = (uint8_t *)zHeapListNext(sys->particles);
            else {
                zHeapListRemoveCurrent(sys->particles);
                zmemFreeListFree(sys->pool, p);
                p = (uint8_t *)zHeapListCurrent(sys->particles);
            }
        }
    }
    for (n = ZFX_AFFECTORS(type)->head; n != NULL; n = n->next) {
        ZfxAffectorLink *l = (ZfxAffectorLink *)n;
        ((void (__cdecl *)(void *const *, ZfxSystem *, const void *))l->affector[1])(l->affector, sys, l->params);
    }
    sys->lastTime = now;
    return count;
}

/* VA 0x00495690, 57 bytes, [I] new function, status draft (no caller found): v - 2 (v.n) n */
void zmathVecReflect(float *out, const RwV3d *v, const RwV3d *n)
{
    float d = zmathVecDot(v, n);
    d += d;
    out[0] = v->x - d * n->x;
    out[1] = v->y - d * n->y;
    out[2] = v->z - d * n->z;
}

/* VA 0x004956d0, 24 bytes, [I] new function, status draft (no caller found) */
float zmathPlaneDistance(const float *plane, const RwV3d *p)
{
    return zmathVecDot((const RwV3d *)plane, p) + plane[4];
}

/* Plane helpers: a plane is { normal, pad, d } (5 floats) */

/* VA 0x004956f0, 52 bytes, unassigned, status draft */
float *zmathPlaneFromPointNormal(float *plane, const RwV3d *point, const RwV3d *normal)
{
    plane[0] = normal->x;
    plane[1] = normal->y;
    plane[2] = normal->z;
    plane[4] = zmathVecDot(normal, point) * -1.0f;
    return plane;
}

/* VA 0x00495730, 289 bytes, unassigned, status draft */
int zmathClipSegmentToPlane(float *p0, float *p1, const float *plane)
{
    /* Keeps the part of segment p0-p1 on the front side (dot + d > 0) of the plane. Returns 0 when
     * nothing is left (both behind, or one behind and one on the plane), else 1; a crossing
     * segment has its back end moved onto the plane (all 4 words copied: w is uninitialised). */
    float d[2];
    int side[2];
    float hit[4];
    int i;

    d[0] = zmathVecDot((const RwV3d *)plane, (const RwV3d *)p0) + plane[4];
    d[1] = zmathVecDot((const RwV3d *)plane, (const RwV3d *)p1) + plane[4];
    for (i = 0; i < 2; i++) {
        if ((float)fabs(d[i]) < 1e-05f)
            side[i] = 2;                 /* on the plane */
        else if (d[i] > 0.0f)
            side[i] = 0;                 /* front */
        else
            side[i] = 1;                 /* behind */
    }
    if (side[0] == side[1])
        return side[0] != 1;
    if (side[0] != 0 && side[1] != 0)    /* one behind, one on the plane */
        return 0;
    if (side[0] == 2 || side[1] == 2)    /* one in front, one on the plane */
        return 1;
    zmathVecLerp((RwV3d *)hit, (const RwV3d *)p0, (const RwV3d *)p1, d[0] / (d[0] - d[1]));
    if (side[0] == 0) {
        p1[0] = hit[0]; p1[1] = hit[1]; p1[2] = hit[2]; p1[3] = hit[3];
    } else {
        p0[0] = hit[0]; p0[1] = hit[1]; p0[2] = hit[2]; p0[3] = hit[3];
    }
    return 1;
}

/* VA 0x00495860, 154 bytes, unassigned, status draft (no caller found)
 * A view matrix at `eye` looking at `target` (yaw, then pitch; translation = -(R eye)). */
RwMatrix *zmathMatrixLookAt(RwMatrix *m, const RwV3d *eye, const RwV3d *target)
{
    RwV3d d;
    int16_t pitch, yaw;

    zmathVecSub(&d, target, eye);
    zmathVecNormalize(&d, &d);
    pitch = zmathAcos(d.y);
    yaw = zmathAtan2(d.x, d.z);
    zmathMatrixIdentity(m);
    zmathMatrixRotateY(m, m, -yaw);
    zmathMatrixRotateX(m, m, 0x4000 - pitch);
    zmathTransformPoint(&m->pos, m, eye);
    zmathVecNegate(&m->pos, &m->pos);
    return m;
}

/* VA 0x00495900, 264 bytes, [I] row, status draft
 * The rotation taking unit vector `from` onto `to` (axis = from x to); identity when they are within
 * ~2.6 degrees (dot > 0.999). */
void zmathMatrixFromTo(RwMatrix *m, const RwV3d *from, const RwV3d *to)
{
    RwV3d a;
    float c = zmathVecDot(from, to), k, xy, xz, yz;

    if (c > 0.999f) {
        zmathMatrixIdentity(m);
        return;
    }
    zmathVecCross(&a, from, to);
    k = (1.0f - c) / zmathVecDot(&a, &a);
    m->pos.x = m->pos.y = m->pos.z = 0.0f;
    xy = a.y * a.x * k;
    xz = a.z * a.x * k;
    yz = a.z * a.y * k;
    m->right.x = a.x * a.x * k + c;  m->up.x = xy - a.z;             m->at.x = xz + a.y;
    m->right.y = xy + a.z;           m->up.y = a.y * a.y * k + c;    m->at.y = yz - a.x;
    m->right.z = xz - a.y;           m->up.z = yz + a.x;             m->at.z = a.z * a.z * k + c;
}

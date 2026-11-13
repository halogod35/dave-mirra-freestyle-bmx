/*
 * NgEngine/Zge/Com/ZglRw/zrwCol.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwCol.c
 * Evidence: [V] path string 0x00562070 (asserts of zrwColInit 0x1B9, zrwColLineTest 0x25B); the
 *           collision driver table 0x00562060 (4 slots); [I] the rest by link order and the globals
 *           0x00674158-0x006741B3. The ZglRw pass moved here 0x00493CE0 / 0x00493D30 (triangle -> ZGL
 *           material, from gap_pcinput__zrwCol) and created the triangle helpers 0x00494CD0-
 *           0x0049536F that follow the file's last function.
 * 22 functions, VA 0x00493CE0-0x0049536F (14 new functions).
 * Draft. Non-matching C (AD-8). Types: ngif.h (ZrwColTri, ZrwColHit, ZrwColTriList),
 * zglrw.h. Description: docs/re/zrw.md 5.
 *
 * Collision queries against the RenderWare world: the world sectors' own collision trees
 * (RpCollisionWorldForAllIntersections walks them, dmbmx.formats.rwcoll ports the walk) and the
 * atomics of the world's clumps (props, models: RpWorldForAllAtomicIntersections +
 * RpAtomicForAllIntersections). Queries take ZGE coordinates and return ZGE coordinates.
 *   mode bits: 1 world sectors, 2 / 4 atomics flagged 0x80 ("static") / not flagged (both = all),
 *              8 skip the NULL-terminated clump list `exclude`, 0x20 clip the line to the world box.
 */
#include "decomp.h"
#include "zmath.h"
#include "ngif.h"
#include "zglrw.h"

#define ZRWCOL_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwCol.c"
#define EPS 1e-05f
#define ZABS(f) ((f) < 0.0f ? -(f) : (f))

extern void    *g_zrwWorld;            /* 0x006976C8 */
extern float    g_zrwWorldOrigin[3];   /* 0x006976CC */
extern float    g_zrwWorldBox[2][4];   /* 0x006976FC min, 0x0069770C max (ZGE units) */
extern float    g_zrwScale;            /* 0x00697720 */
extern float    g_zrwInvScale;         /* 0x00697724 */
int zutilClipSegmentToBBox(float *p0, float *p1, const float *box);   /* zutil.c */
int zutilBBoxContainsPoint(const float *box, const float *p);         /* zutil.c */

/* RwCollTri / RwIntersection: zglrw.h */

/* ------------------------------------------------------------------ globals */
static RwRGBA s_hitColour;              /* 0x00674158 */
static struct {
    int32_t    initialized;             /* 0x00674160 */
    ZrwColTri *cursor;                  /* 0x00674164 */
    int32_t    count;                   /* 0x00674168 */
    ZrwColTri *list;                    /* 0x0067416C sphere results, grown by 16 */
    int32_t    capacity;                /* 0x00674170 */
    uint32_t   mode;                    /* 0x00674174 */
    int32_t    found;                   /* 0x00674178 */
    float      bestT;                   /* 0x0067417C */
    RwCollTri  tri;                     /* 0x00674180 the nearest triangle */
    void      *atomic;                  /* 0x006741A8 NULL = world sector */
    void      *sector;                  /* 0x006741AC */
} s_col;                                /* memset 0x50 */
static int32_t s_tagsInitialized;       /* 0x006741B0 */

static void zrwColToZge(float *out, const RwV3d *rw)
{
    out[0] = (rw->x - g_zrwWorldOrigin[0]) * g_zrwInvScale;
    out[1] = -((rw->y - g_zrwWorldOrigin[1]) * g_zrwInvScale);
    out[2] = -((rw->z - g_zrwWorldOrigin[2]) * g_zrwInvScale);
}

static void zrwColToRw(RwV3d *out, const float *p)
{
    out->x = g_zrwScale * p[0] + g_zrwWorldOrigin[0];
    out->y = g_zrwWorldOrigin[1] - p[1] * g_zrwScale;
    out->z = g_zrwWorldOrigin[2] - p[2] * g_zrwScale;
}

/* VA 0x00493ce0, 67 bytes, [I] new function, status draft (no caller found)
 * The ZGL material of triangle `tri->index` of an atomic (texture owner plugin). */
void *zrwColAtomicTriMaterial(void *atomic, const RwCollTri *tri)
{
    void *geom = *(void **)((uint8_t *)atomic + 0x1C);
    const uint16_t *t = (const uint16_t *)RpGeometryGetTriangles(geom);
    void *tex = RpMaterialGetTexture(RpGeometryGetMaterial(geom, t[tri->index * 4 + 3]));
    return tex == NULL ? NULL : zrwTxrGetOwnerOfRwTexture(tex);
}

/* VA 0x00493d30, 67 bytes, [I] new function, status draft (no caller found)
 * The same for a world-sector polygon: material = sector base (+4) + polygon byte 0 (+0xC). */
void *zrwColSectorTriMaterial(void *sector, const RwCollTri *tri)
{
    uint8_t *s = (uint8_t *)sector;
    void *tex = RpMaterialGetTexture(RpWorldGetMaterial(g_zrwWorld,
        (*(uint8_t **)(s + 0xC))[tri->index * 4] + *(int16_t *)(s + 4)));
    return tex == NULL ? NULL : zrwTxrGetOwnerOfRwTexture(tex);
}

/* VA 0x00493d80, 92 bytes, [V] direct, status draft (collision slot 0) */
int zrwColInit(void)
{
    if (s_col.initialized)
        zAssertFail("Already initialized", ZRWCOL_SRC, 0x1B9, 0);
    zstringMemSet(&s_col, 0, sizeof(s_col));
    s_col.initialized = 1;
    s_col.capacity = 0x400;
    s_col.list = (ZrwColTri *)zmemAlloc(0x400 * sizeof(ZrwColTri));
    return 1;
}

/* VA 0x00493de0, 49 bytes, [I] new function, status draft (slot 1) */
int zrwColShutdown(void)
{
    if (s_col.initialized) {
        zmemFree(s_col.list);
        s_col.list = NULL;
        s_col.initialized = 0;
    }
    return 1;
}

/* fill a ZGE triangle record from an RW collision triangle (world: sector, else atomic) */
static void zrwColFillTri(ZrwColTri *out, const RwCollTri *tri, void *sector, void *atomic)
{
    int k;
    if (atomic == NULL) {
        out->normal[0] = tri->normal.x;
        out->normal[1] = -tri->normal.y;
        out->normal[2] = -tri->normal.z;
        for (k = 0; k < 3; k++)
            zrwColToZge(out->v[k], tri->vertices[k]);
        out->model = NULL;
        out->material = zrwColSectorTriMaterial(sector, tri);
    } else {
        RwMatrix *ltm = RwFrameGetLTM(*(void **)((uint8_t *)atomic + 4));
        RwV3d p;
        for (k = 0; k < 3; k++) {
            RwV3dTransformPoints(&p, tri->vertices[k], 1, ltm);
            zrwColToZge(out->v[k], &p);
        }
        RwV3dTransformVectors(&p, &tri->normal, 1, ltm);
        out->normal[0] = p.x;
        out->normal[1] = -p.y;
        out->normal[2] = -p.z;
        out->model = zrwModelFromAtomic(atomic);
        out->material = zrwColAtomicTriMaterial(atomic, tri);
    }
}

/* VA 0x00493e20, 1051 bytes, [V] direct, status draft (slot 2: the line test)
 * Nearest triangle hit by the segment a-b; out = ZrwColHit (triangle, t, model, material, point
 * a + t (b - a)). Returns 1 on a hit. */
int zrwColLineTest(const RwV3d *a, const RwV3d *b, uint32_t mode, void **exclude, ZrwColHit *out)
{
    RwIntersection line;
    RwV3d d;

    zmathVecSubScalar((RwV3d *)g_zrwWorldBox[0], (RwV3d *)g_zrwWorldBox[0], EPS);
    zmathVecAddScalar((RwV3d *)g_zrwWorldBox[1], (RwV3d *)g_zrwWorldBox[1], EPS);
    if ((mode & 0x20) && !zutilClipSegmentToBBox((float *)a, (float *)b, &g_zrwWorldBox[0][0]))
        return 0;
    line.type = 1;
    zrwColToRw((RwV3d *)&line.v[0], &a->x);
    zrwColToRw((RwV3d *)&line.v[3], &b->x);
    s_col.found = 0;
    s_col.mode = mode;
    if (mode & 1)
        RpCollisionWorldForAllIntersections(g_zrwWorld, &line, (void *)zrwColWorldLineCB, NULL);
    if (mode & 6)
        RpWorldForAllAtomicIntersections(g_zrwWorld, &line, (void *)zrwColAtomicLineCB, exclude);
    if (!s_col.found)
        return 0;
    if (s_col.atomic == NULL && s_col.sector == NULL) {
        zAssertFail("Error", ZRWCOL_SRC, 0x25B, 0);
    } else {
        zrwColFillTri(&out->tri, &s_col.tri, s_col.sector, s_col.atomic);
        if (s_col.atomic == NULL)
            out->tri.model = NULL;
    }
    out->tri.t = s_col.bestT;
    zmathVecSub(&d, b, a);
    zmathVecScale(&d, &d, s_col.bestT);
    zmathVecAdd(&out->pos, a, &d);
    return 1;
}

/* VA 0x00494240, 95 bytes, [I] new function, status draft (world triangle callback: keep nearest) */
void *zrwColWorldLineCB(void *isect, void *sector, RwCollTri *tri, float t, void *data)
{
    (void)isect;
    if (s_col.found && s_col.bestT < t)
        return tri;
    s_col.found = 1;
    s_col.atomic = data;
    s_col.sector = sector;
    s_col.bestT = t;
    zstringMemCopy(&s_col.tri, tri, sizeof(RwCollTri));
    return tri;
}

/* atomic filter shared by the line and sphere tests */
static int zrwColAtomicWanted(void *atomic, void **exclude)
{
    uint8_t flags = ((uint8_t *)atomic)[2];
    if ((s_col.mode & 6) != 6) {
        if ((s_col.mode & 2) == 0) {
            if (flags & 0x80)
                return 0;
        } else if ((flags & 0x80) == 0)
            return 0;
    }
    if ((s_col.mode & 8) && exclude != NULL)
        for (; *exclude != NULL; exclude++)
            if (*exclude == *(void **)((uint8_t *)atomic + 0x40))
                return 0;
    return 1;
}

/* VA 0x004942a0, 104 bytes, [I] new function, status draft (atomic callback of the line test) */
void *zrwColAtomicLineCB(void *isect, void *sector, void *atomic, float dist, void **exclude)
{
    (void)sector; (void)dist;
    if (zrwColAtomicWanted(atomic, exclude))
        RpAtomicForAllIntersections(atomic, isect, (void *)zrwColAtomicTriLineCB, atomic);
    return atomic;
}

/* VA 0x00494310, 95 bytes, [I] new function, status draft (triangle callback, keep nearest) */
void *zrwColAtomicTriLineCB(void *isect, RwCollTri *tri, float t, void *atomic)
{
    (void)isect;
    if (s_col.found && s_col.bestT < t)
        return tri;
    s_col.found = 1;
    s_col.atomic = atomic;
    s_col.sector = NULL;
    s_col.bestT = t;
    zstringMemCopy(&s_col.tri, tri, sizeof(RwCollTri));
    return tri;
}

/* VA 0x00494370, 274 bytes, [I] callers, status draft (slot 3: the sphere test)
 * Every triangle touching the sphere (centre, radius in ZGE units) inside the world box; out = the
 * list (owned by zrwCol, valid until the next query). */
int zrwColSphereTest(const float *centre, float radius, uint32_t mode, void **exclude, ZrwColTriList *out)
{
    RwIntersection s;

    if (!zutilBBoxContainsPoint(&g_zrwWorldBox[0][0], centre))
        return 0;
    s_col.count = 0;
    s_col.cursor = s_col.list;
    s.type = 3;
    zrwColToRw((RwV3d *)&s.v[0], centre);
    s.v[3] = g_zrwScale * radius;
    s_col.found = 0;
    s_col.bestT = 9999.0f;
    s_col.mode = mode;
    if (mode & 1)
        RpCollisionWorldForAllIntersections(g_zrwWorld, &s, (void *)zrwColSphereTriCB, NULL);
    if (mode & 6)
        RpWorldForAllAtomicIntersections(g_zrwWorld, &s, (void *)zrwColAtomicSphereCB, exclude);
    if (!s_col.found)
        return 0;
    out->list = s_col.list;
    out->count = s_col.count;
    return 1;
}

/* VA 0x00494490, 704 bytes, [I] callers, status draft (append one triangle to the sphere list) */
void *zrwColSphereTriCB(void *isect, void *sector, RwCollTri *tri, float dist, void *atomic)
{
    ZrwColTri *t;
    (void)isect;

    if (s_col.count == s_col.capacity) {
        s_col.capacity += 0x10;
        s_col.list = (ZrwColTri *)zmemResize(s_col.list, s_col.capacity * sizeof(ZrwColTri));
        t = s_col.list + s_col.count;
    } else
        t = s_col.cursor;
    s_col.cursor = t + 1;
    s_col.count++;
    s_col.found = 1;
    zrwColFillTri(t, tri, sector, atomic);
    t->t = dist;
    return tri;
}

/* VA 0x00494750, 104 bytes, [I] new function, status draft (atomic callback of the sphere test) */
void *zrwColAtomicSphereCB(void *isect, void *sector, void *atomic, float dist, void **exclude)
{
    (void)sector; (void)dist;
    if (zrwColAtomicWanted(atomic, exclude))
        RpAtomicForAllIntersections(atomic, isect, (void *)zrwColAtomicTriSphereCB, atomic);
    return atomic;
}

/* VA 0x004947c0, 31 bytes, [I] new function, status draft */
void *zrwColAtomicTriSphereCB(void *isect, RwCollTri *tri, float dist, void *atomic)
{
    return zrwColSphereTriCB(isect, NULL, tri, dist, atomic);
}

/* VA 0x004947e0, 1086 bytes, [I] callers, status draft (NgApp: the rider's ground tint)
 * The prelit colour at the last line hit on a world sector (0,0,0,255 for atomics): the corners are
 * sorted by height and their vertex colours (sector +0x1C, polygon bytes 2/3 index them)
 * interpolated across the triangle in x/z [I: simplified; the original also writes the sorted
 * corners back into `hit`]. */
const RwRGBA *zrwColGetHitColour(ZrwColHit *hit)
{
    s_hitColour.red = s_hitColour.green = s_hitColour.blue = 0;
    s_hitColour.alpha = 0xFF;
    if (s_col.sector != NULL) {
        uint8_t *s = (uint8_t *)s_col.sector;
        const RwRGBA *prelit = *(const RwRGBA **)(s + 0x1C);
        const uint8_t *poly = *(const uint8_t **)(s + 0xC) + s_col.tri.index * 4;
        RwRGBA c0 = prelit[poly[2]], c1 = prelit[poly[3]];
        float span = hit->tri.v[2][1] - hit->tri.v[0][1];
        float f = (span != 0.0f) ? (hit->pos.y - hit->tri.v[0][1]) / span : 0.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        s_hitColour.red   = (uint8_t)(c0.red   + (c1.red   - c0.red)   * f);
        s_hitColour.green = (uint8_t)(c0.green + (c1.green - c0.green) * f);
        s_hitColour.blue  = (uint8_t)(c0.blue  + (c1.blue  - c0.blue)  * f);
        s_hitColour.alpha = 0;
    }
    return &s_hitColour;
}

/* VA 0x00494c20, 21 bytes, [I] callers, status draft (the tag database: ztaginf) */
int zrwColTagsInit(void)
{
    /* returns ztaginfInit's failure (0), else 1 [V] (zgeInit tests it) */
    if (!ztaginfInit())
        return 0;
    s_tagsInitialized = 1;
    return 1;
}

/* VA 0x00494c40, 35 bytes, [I] new function, status draft */
int zrwColTagsShutdown(void)
{
    if (s_tagsInitialized) {
        if (!ztaginfShutdown())
            return 0;
        s_tagsInitialized = 0;
    }
    return 1;
}

/* VA 0x00494c70, 41 bytes, [I] callers, status draft (NgIFzcollide / NgApp) */
int zrwColLine(const RwV3d *a, const RwV3d *b, uint32_t mode, void **exclude, ZrwColHit *out)
{
    return g_zglColApi.lineTest(a, b, mode, exclude, out) != 0;
}

/* VA 0x00494ca0, 41 bytes, [I] callers, status draft (NgIFzbsp) */
int zrwColSphere(const float *centre, float radius, uint32_t mode, void **exclude, ZrwColTriList *out)
{
    return g_zglColApi.sphereTest(centre, radius, mode, exclude, out) != 0;
}

/* VA 0x00494cd0, 632 bytes, [I] new function, status draft (no caller found)
 * Is p inside triangle v0 v1 v2 (normal n)? Projects on the plane of the normal's dominant axis and
 * checks that p is on the same side of every edge (|cross| < 1e-5 counts as on the edge). */
int zrwColPointInTri(const float *v0, const float *v1, const float *v2, const float *n, const float *p)
{
    const float *v[3];
    int axis, sign, i, a, b;

    v[0] = v0; v[1] = v1; v[2] = v2;
    if (ZABS(n[0]) < ZABS(n[1]))
        axis = (ZABS(n[2]) < ZABS(n[1])) ? 1 : 2;
    else
        axis = (ZABS(n[2]) <= ZABS(n[0])) ? 0 : 2;
    sign = (n[axis] > EPS) ? 1 : (n[axis] < -EPS) ? -1 : 0;
    a = (axis + 1) % 3;
    b = (axis + 2) % 3;
    for (i = 0; i < 3; i++) {
        const float *e0 = v[(i + 2) % 3], *e1 = v[i];
        float c = (p[a] - e1[a]) * (e0[b] - e1[b]) - (p[b] - e1[b]) * (e0[a] - e1[a]);
        if (ZABS(c) >= EPS) {
            int s = (c > EPS) ? 1 : (c < -EPS) ? -1 : 0;
            if (s != -sign)
                return 0;
        }
    }
    return 1;
}

/* VA 0x00494f50, 407 bytes, [I] new function, status draft (no caller found)
 * p inside triangle a b c with normal n: every sub-triangle (a,b,p) (b,c,p) (c,a,p) has normal n. */
int zrwColPointInTriByNormals(const RwV3d *a, const RwV3d *b, const RwV3d *c, const float *n, const RwV3d *p)
{
    const RwV3d *e[4];
    int i;
    e[0] = a; e[1] = b; e[2] = c; e[3] = a;
    for (i = 0; i < 3; i++) {
        RwV3d t;
        zmathTriangleNormal(&t, e[i], e[i + 1], p);
        zmathVecNormalize(&t, &t);
        if (ZABS(t.x - n[0]) >= EPS || ZABS(t.y - n[1]) >= EPS || ZABS(t.z - n[2]) >= EPS)
            return 0;
    }
    return 1;
}

/* VA 0x004950f0, 236 bytes, [I] new function, status draft
 * The point of segment a-b nearest to p. */
void zrwColClosestPointOnSegment(float *out, const RwV3d *a, const RwV3d *b, const RwV3d *p)
{
    RwV3d dir, d;
    float t;

    zmathVecSub(&dir, b, a);
    zmathVecNormalize(&dir, &dir);
    zmathVecSub(&d, a, p);
    if (zmathVecDot(&d, &dir) >= 0.0f) {
        out[0] = a->x; out[1] = a->y; out[2] = a->z;
        return;
    }
    zmathVecSub(&d, b, p);
    t = zmathVecDot(&d, &dir);
    if (t > 0.0f) {
        out[0] = b->x - dir.x * t;
        out[1] = b->y - dir.y * t;
        out[2] = b->z - dir.z * t;
    } else {
        out[0] = b->x; out[1] = b->y; out[2] = b->z;
    }
}

/* VA 0x004951e0, 391 bytes, [I] new function, status draft (no caller found)
 * Sphere (centre c, radius r) against triangle v0 v1 v2 with normal n: the contact point (the
 * projection on the plane if inside, else a corner, else the nearest edge point within r), NULL
 * when the sphere misses. */
RwV3d *zrwColSphereTriContact(RwV3d *out, const RwV3d *c, float r, const RwV3d *v0, const RwV3d *v1,
                              const RwV3d *v2, const RwV3d *n)
{
    const RwV3d *v[3];
    RwV3d proj, d;
    float dist, r2;
    int i;

    v[0] = v0; v[1] = v1; v[2] = v2;
    zmathVecSub(&d, c, v0);
    dist = zmathVecDot(&d, n);
    r2 = r * r - dist * dist;
    proj.x = c->x - dist * n->x;
    proj.y = c->y - dist * n->y;
    proj.z = c->z - dist * n->z;
    if (zrwColPointInTri(&v0->x, &v1->x, &v2->x, &n->x, &proj.x)) {
        *out = proj;
        return out;
    }
    for (i = 2; i >= 0; i--) {
        zmathVecSub(&d, v[i], &proj);
        if (zmathVecLengthSq(&d) < r2) {
            *out = *v[i];
            return out;
        }
    }
    {
        const RwV3d *prev = v0;
        for (i = 2; i >= 0; i--) {
            zrwColClosestPointOnSegment(&d.x, prev, v[i], &proj);
            zmathVecSub(&d, &d, &proj);
            if (zmathVecLengthSq(&d) < r2) {
                zmathVecAdd(out, &d, &proj);
                return out;
            }
            prev = v[i];
        }
    }
    return NULL;
}

/* the table's slots take void * results (casts, the functions keep their real types) */
#define COL_LINE_SLOT(f)   ((int (__cdecl *)(const RwV3d *, const RwV3d *, uint32_t, void **, void *))(f))
#define COL_SPHERE_SLOT(f) ((int (__cdecl *)(const float *, float, uint32_t, void **, void *))(f))
const ZglColApi g_zglColApi = {                                             /* 0x00562060 */
    zrwColInit, zrwColShutdown, COL_LINE_SLOT(zrwColLineTest), COL_SPHERE_SLOT(zrwColSphereTest)
};

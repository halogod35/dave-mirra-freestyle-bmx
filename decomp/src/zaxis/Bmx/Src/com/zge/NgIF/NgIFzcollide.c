/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFzcollide.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFzcollide.c
 * Evidence: [V] path string 0x0055D550: zcolSegmentTest's asserts (0x16F, 0x17C), the "Not coded"
 *           asserts of zcolCreatePropRails (0x4DA) / zcolBuildPropRails (0x5EE) and the 8
 *           "not implemented" stubs (lines 900 .. 1592); [I] the rest by link order.
 * 17 functions, VA 0x00469530-0x0046ac85 (DM-057 created the 8 stubs).
 * DM-057 (draft). Non-matching C (AD-8). Types: ngif.h, physics.h, bmxprop.h. Description:
 * docs/re/ngif.md, docs/re/physics.md 2-3.
 *
 * The collision queries the game uses on the PC: the segment test (RenderWare line test both
 * ways, then the tag of the nearest Tag._sd face within 2 units -> NgColHit; a prop with an OBJI
 * switch id answers as a tagged face with that id), the prop shapes, and the prop grind rails
 * (NgColEdge) built from a def's shape and kept in step with the prop's position / rotation.
 */
#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "ngif.h"

#define NGIFZCOLLIDE_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFzcollide.c"

/* ------------------------------------------------------------------ other modules */
int   zrwColLine(const RwV3d *from, const RwV3d *to, int flags, int arg, ZrwColHit *out); /* RW line test */
extern void *(__cdecl *g_zglModelGetObj)(void *model);                 /* 0x00560EE0 */
extern void *(__cdecl *g_zglMatGetPluginData)(void *material);         /* 0x00561038 */
extern zHeapList *g_zrollerEdges;                                      /* 0x00678494 */
extern const RwV3d g_zmathZero;                                        /* 0x006702D8 (0, 0, 0) */

/* ------------------------------------------------------------------ globals (this file) */
static void     *s_edges[32];          /* 0x005F8520 zcolFindEdgesAlong result (NULL-terminated) */
static NgColEdge s_edgeRecs[32];       /* 0x005F85A8 */
static void     *s_edgeCells[32];      /* 0x005F8CE0 */
static uint32_t  s_probeMaterial[2];   /* 0x005F8518 */
static NgPropShapeInfo s_noShape;      /* 0x005F8D68 class 0 */

/* the 4 top rails of a box shape: corner pairs (and open-end flag) [V] table 0x0055D4F8 */
static const uint8_t s_boxRails[4][3] = { { 0, 1, 1 }, { 1, 5, 1 }, { 5, 4, 1 }, { 4, 0, 1 } };
static const uint8_t s_edgeRail[3] = { 0, 1, 1 };                   /* 0x0055D504 (low end open) */

/* ------------------------------------------------------------------ functions */

/* VA 0x00469530, 32 bytes, [I] split, status draft */
const NgPropShapeInfo *zcolGetPropShape(uint32_t defIndex)
{
    /* the def's shape (class, size, y offset); class 0 if it has none */
    NgPropShape *s = zbspGetPropShape((uint16_t)defIndex);

    if (s == NULL) {
        s_noShape.cls = NGSHAPE_NONE;
        return &s_noShape;
    }
    return &s->info;
}

/* VA 0x00469560, 1138 bytes, [I] between, status draft */
void **zcolFindEdgesAlong(const float *pos, const RwV3d *dir, float radius)
{
    /* Unreferenced: the Tag._sd edges within radius (ztaginfFindEdges) with an end ahead along
     * dir (records built in s_edgeRecs), then the prop rails within radius of the line through
     * them with an end ahead. NULL-terminated list of NgColEdge ** cells. */
    ZtagHitList *hits;
    void **cell = s_edgeCells;
    int n = 0, i;

    zstringMemSet(s_edges, 0, sizeof s_edges);
    hits = ztaginfFindEdges(pos, radius);
    if (hits != NULL) {
        for (i = 0; i < (int)hits->count; i++) {
            const ZtagHit *h = &hits->hits[i];
            RwV3d a, b, d, zero = { 0.0f, 0.0f, 0.0f }, up = { 0.0f, 1.0f, 0.0f }, nrm;
            NgColEdge *e = &s_edgeRecs[n];

            a.x = pos[0]; a.y = pos[1]; a.z = pos[2];
            b = a;
            zmathVecSub(&a, &a, (const RwV3d *)&h->v[0]);
            zmathVecSub(&b, &b, (const RwV3d *)&h->v[1]);
            if (!(zmathVecDot(dir, &a) > 0.0f) && !(zmathVecDot(dir, &b) > 0.0f))
                continue;
            e->flags = 0;
            s_edges[n] = &s_edgeCells[n];
            s_edgeCells[n] = e;
            e->kind = h->tag[2];
            e->v0[0] = (int16_t)h->v[0].x; e->v0[1] = (int16_t)h->v[0].y; e->v0[2] = (int16_t)h->v[0].z;
            e->v1[0] = (int16_t)h->v[1].x; e->v1[1] = (int16_t)h->v[1].y; e->v1[2] = (int16_t)h->v[1].z;
            d.x = h->v[1].x; d.y = h->v[1].y; d.z = h->v[1].z;
            zmathVecSub(&d, &d, (const RwV3d *)&h->v[0]);
            e->length = (int16_t)zmathVecLength(&d);
            e->code = *(const uint16_t *)h->tag;
            zmathVecNormalize(&d, &d);
            e->dir[0] = d.x; e->dir[1] = d.y; e->dir[2] = d.z;
            zmathTriangleNormal(&nrm, &up, &zero, &d);
            e->normal[0] = nrm.x; e->normal[1] = nrm.y; e->normal[2] = nrm.z;
            e->d = -zmathVecDot(&nrm, (const RwV3d *)&h->v[0]);
            n++;
        }
    }
    cell = &s_edgeCells[n];
    {
        NgColEdge *r;

        for (r = (NgColEdge *)zHeapListFirst(g_zrollerEdges); r != NULL;
             r = (NgColEdge *)zHeapListNext(g_zrollerEdges)) {
            RwV3d a, b, p, u, pa, pb;
            float t;

            a.x = r->v0[0]; a.y = r->v0[1]; a.z = r->v0[2];
            b.x = r->v1[0]; b.y = r->v1[1]; b.z = r->v1[2];
            p.x = pos[0]; p.y = pos[1]; p.z = pos[2];
            zmathVecSub(&p, &p, &a);
            zmathVecSub(&u, &b, &a);
            zmathVecNormalize(&u, &u);
            t = zmathVecDot(&p, &u);
            if (sqrtf(p.x * p.x + p.y * p.y + p.z * p.z - t * t) > radius)
                continue;
            pa.x = pos[0]; pa.y = pos[1]; pa.z = pos[2];
            pb = pa;
            zmathVecSub(&pa, &pa, &a);
            zmathVecSub(&pb, &pb, &b);
            if (zmathVecDot(dir, &pa) <= 0.0f && zmathVecDot(dir, &pb) <= 0.0f)
                continue;
            s_edges[n++] = cell;
            *cell++ = r;
        }
    }
    return s_edges;
}

/* VA 0x00469b00, 1244 bytes, [V] direct, status draft */
int zcolSegmentTest(const float *from, const float *to, NgColHit *hit)
{
    /* The collision test of the game (physics, camera, game.c): from -> to against the world and
     * the props. RenderWare is asked both ways (from a unit before `from` to `to`, then from a unit
     * beyond the first hit - or `to` - back to `from`); the first answer wins. World hits get the
     * tag of the nearest Tag._sd face within 2 units (code at +0x16, class 1 records without
     * `extra` keep only the id; flags remapped), the triangle and the material flags; prop hits
     * are type 2 with the prop record - or type 1 with face code = the prop's OBJI switch id
     * (class 9 triggers, prop.c's breakables). Returns 1 on a hit. */
    ZrwColHit fwd, back, h;
    RwV3d d, start, end, a, b;
    int f, r, any;

    d.x = to[0] - from[0]; d.y = to[1] - from[1]; d.z = to[2] - from[2];
    zmathVecNormalize(&d, &d);
    start.x = from[0] - d.x; start.y = from[1] - d.y; start.z = from[2] - d.z;
    f = zrwColLine(&start, (const RwV3d *)to, 3, 0, &fwd);
    a.x = from[0]; a.y = from[1]; a.z = from[2];
    b.x = to[0]; b.y = to[1]; b.z = to[2];
    if (f != 0)
        b = fwd.pos;
    d.x = a.x - b.x; d.y = a.y - b.y; d.z = a.z - b.z;
    zmathVecNormalize(&d, &d);
    end.x = b.x - d.x; end.y = b.y - d.y; end.z = b.z - d.z;
    r = zrwColLine(&end, &a, 3, 0, &back);
    any = f != 0 || r != 0;
    if (any)
        h = f != 0 ? fwd : back;
    if (hit == NULL)
        zAssertFail("Invalid parameter.", NGIFZCOLLIDE_SRC, 0x16F, 0);
    zstringMemSet(hit, 0, sizeof *hit);
    if (!any)
        return 0;
    hit->unk_04 = 0;
    hit->pos[0] = h.pos.x; hit->pos[1] = h.pos.y; hit->pos[2] = h.pos.z;
    if (h.tri.t <= -0.01f || h.tri.t >= 1.01f)
        zAssertFail("Degenerate collision.", NGIFZCOLLIDE_SRC, 0x17C, 0);
    hit->t = h.tri.t;
    hit->normal[0] = h.tri.normal[0];
    hit->normal[1] = h.tri.normal[1];
    hit->normal[2] = h.tri.normal[2];
    hit->d = -zmathVecDot((const RwV3d *)h.tri.normal, (const RwV3d *)h.tri.v[0]);

    if (h.tri.model == NULL) {
        /* the world */
        uint16_t code = 0;
        ZtagHit *tag = ztaginfFindNearestFace(hit->pos, 2.0f);

        if (tag != NULL && tag->tag != NULL) {
            code = *(const uint16_t *)tag->tag;
            if (*(const uint32_t *)(tag->tag + 8) == 0 && (code & 0xF000) == 0x1000)
                code &= 0xFFF;
        }
        hit->type = 1;
        hit->hasFace = 1;
        hit->prop = NULL;
        hit->faceData.normal[0] = hit->normal[0];
        hit->faceData.normal[1] = hit->normal[1];
        hit->faceData.normal[2] = hit->normal[2];
        hit->faceData.d = hit->d;
        hit->faceData.unk_10 = 0;
        hit->faceData.kind = 0x88;
        hit->faceData.flags = 0;
        if (tag != NULL && tag->tag != NULL) {
            uint8_t t = tag->tag[3];

            hit->faceData.flags = (uint8_t)((((t & 1) << 1 | (t & 0xC)) << 1 | (t & 2)) << 2);
        }
        hit->faceData.code = code;
        hit->faceData.v0[0] = h.tri.v[0][0]; hit->faceData.v0[1] = h.tri.v[0][1]; hit->faceData.v0[2] = h.tri.v[0][2];
        hit->faceData.v1[0] = h.tri.v[1][0]; hit->faceData.v1[1] = h.tri.v[1][1]; hit->faceData.v1[2] = h.tri.v[1][2];
        hit->faceData.v2[0] = h.tri.v[2][0]; hit->faceData.v2[1] = h.tri.v[2][1]; hit->faceData.v2[2] = h.tri.v[2][2];
        hit->face = &hit->faceData;
        if (h.tri.material == NULL)
            hit->materialFlags &= 0x3FE00000;
        else
            hit->materialFlags = *(const uint32_t *)g_zglMatGetPluginData(h.tri.material);
        hit->unk_d4 = 0;
        hit->material = &hit->materialFlags;
        return 1;
    }

    /* a prop */
    {
        void *obj = g_zglModelGetObj(h.tri.model);
        zListNode *node;
        Zprop *z = NULL;

        for (node = zpropGetList()->head; node != NULL; node = node->next) {
            Zprop *p = (Zprop *)node;

            if (p->info.user == NULL)
                zbspPropColCreate(p);
            if (p->info.obj == obj) {
                z = p;
                if (p->info.user != NULL) {
                    hit->type = 2;
                    hit->propObj = p->info.user;
                    hit->propId = ((NgPropCol *)p->info.user)->defIndex;
                }
                break;
            }
        }
        hit->prop = hit->propObj;
        hit->type = 2;
        hit->hasFace = 0;
        hit->faceData.normal[0] = hit->normal[0];
        hit->faceData.normal[1] = hit->normal[1];
        hit->faceData.normal[2] = hit->normal[2];
        hit->faceData.d = hit->d;
        hit->faceData.unk_10 = 0;
        hit->faceData.kind = 0x88;
        hit->faceData.flags = 0;
        hit->faceData.code = 0;
        hit->materialFlags &= 0x3FE00000;
        hit->unk_d4 = 0;
        hit->face = &hit->faceData;
        hit->material = &hit->materialFlags;
        if (z != NULL && z->info.obji != NULL && *(const uint16_t *)(z->info.obji + 8) != 0) {
            hit->type = 1;                  /* a switch prop: its id is the face code */
            hit->hasFace = 1;
            hit->faceData.code = *(const uint16_t *)(z->info.obji + 8);
        }
        return 1;
    }
}

/* VA 0x0046a120, 35 bytes, [V] direct, status draft */
int zcolbboxcollision(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 900, "zcolbboxcollision");
    return 1;
}

/* VA 0x0046a160, 260 bytes, [I] between, status draft */
float zcolProbeDown(const float *pos, uint32_t *out)
{
    /* Unreferenced (a PSX-style ground probe): the world below / above (x, z) between y -500 and
     * +500; fills a hit block (type 2, face at +0x30 with the triangle's first corner, material)
     * and returns the line fraction, or 10000 when nothing is there. */
    RwV3d from, to;
    ZrwColHit h;

    from.x = pos[0]; from.y = -500.0f; from.z = pos[2];
    to.x = pos[0];   to.y = 500.0f;    to.z = pos[2];
    if (zrwColLine(&from, &to, 3, 0, &h) == 0)
        return 10000.0f;
    out[0] = 2;
    out[1] = 0;
    out[2] = (uint32_t)(out + 12);
    out[12] = *(const uint32_t *)&h.tri.normal[0];
    out[13] = *(const uint32_t *)&h.tri.normal[1];
    out[14] = *(const uint32_t *)&h.tri.normal[2];
    out[15] = 0;
    out[3] = 1;
    ((uint8_t *)out)[0x55] = 0;
    out[16] = *(const uint32_t *)&h.tri.normal[0];
    out[17] = *(const uint32_t *)&h.tri.normal[1];
    ((uint8_t *)out)[0x54] = 0;
    ((uint16_t *)out)[0x2B] = 0;
    out[18] = *(const uint32_t *)&h.tri.normal[2];
    out[19] = 0;
    out[20] = 0;
    out[4] = (uint32_t)(out + 16);
    out[5] = 0;
    s_probeMaterial[0] &= 0x3FE00000;
    s_probeMaterial[1] = 0;
    ((uint16_t *)out)[16] = 0;
    out[9] = 0;
    out[10] = 0;
    out[11] = 0;
    out[6] = (uint32_t)s_probeMaterial;
    *(float *)&out[7] = h.tri.t;
    return h.tri.t;
}

/* VA 0x0046a2c0, 32 bytes, [V] direct, status draft */
int zcolGetTerrainFace(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 995, "Get terrain face");
    return 0;
}

/* VA 0x0046a2f0, 32 bytes, [V] direct, status draft */
int zcolPropHitBox(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1056, "prop hit box");
    return 0;
}

/* VA 0x0046a320, 36 bytes, [V] direct, status draft */
void zcolGetPlaneHeight(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1074, "GetPlaneHeight");
}

/* VA 0x0046a360, 30 bytes, [V] direct, status draft */
void zcolGenerateBoundBox(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1167, "Generate bound box");
}

/* VA 0x0046a390, 30 bytes, [V] direct, status draft */
void zcolInitPropFacesList(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1183, "Init Prop faces list");
}

/* VA 0x0046a3c0, 30 bytes, [V] direct, status draft */
void zcolGeneratePropBoundingVolume(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1199, "Generate prop bounding volume");
}

/* VA 0x0046a3f0, 158 bytes, [V] direct, status draft */
int zcolCreatePropRails(NgPropCol *col, NgColEdge **out)
{
    /* zrollerLoadLevel: the prop's rails (4 for a box, 1 for a rail / edge shape) into col->rails;
     * the pointers go to out[] for g_zrollerEdges. Returns how many. */
    int n, i, made;

    switch (zcolGetPropShape(col->defIndex)->cls) {
    case NGSHAPE_1:
        zAssertFail("Not coded", NGIFZCOLLIDE_SRC, 0x4DA, 0);
        n = 0;
        break;
    case NGSHAPE_BOX:  n = 4; break;
    case NGSHAPE_RAIL:
    case NGSHAPE_EDGE: n = 1; break;
    default:           n = 0; break;
    }
    col->rails = (NgColEdge *)zstringMemSet(zmemAlloc((uint32_t)n * sizeof(NgColEdge)), 0, (uint32_t)n * sizeof(NgColEdge));
    made = zcolBuildPropRails(col, col->rails);
    for (i = 0; i < made; i++)
        *out++ = &((NgColEdge *)col->rails)[i];
    return made;
}

/* VA 0x0046a4d0, 1094 bytes, [V] direct, status draft */
int zcolBuildPropRails(NgPropCol *col, NgColEdge *rails)
{
    /* The shape's 8 box corners (half width w, height h, half depth d; the +x side's top lowered
     * by extra[3] x 10000 when set), raised by yOffset, scaled, rotated and moved like the prop;
     * then the rails: box = edges 0-1, 1-5, 5-4, 4-0 (both ends open), rail = from the middle of
     * edge 0-1 to the middle of 4-5 (ends closed), edge = 0-1 (the low end open). */
    const NgPropShape *shape = zbspGetPropShape(col->defIndex);
    const ZpropDefInfo *def = col->prop->info.def;
    float w = def->extents[0] * 50.0f, h = def->extents[1] * 100.0f, dd = def->extents[2] * 50.0f;
    float lift = (def->extents[1] * 0.5f + def->extra[1]) * 100.0f;
    float top = def->extra[3] <= 1e-05f ? 0.0f : h - def->extra[3] * 10000.0f;
    RwV3d c[8];
    int16_t a[3], b[3];
    int i;

    c[0].x = -w; c[0].y = -h;       c[0].z = -dd;
    c[1].x = -w; c[1].y = -h;       c[1].z = dd;
    c[2].x = -w; c[2].y = 0.0f;     c[2].z = dd;
    c[3].x = -w; c[3].y = 0.0f;     c[3].z = -dd;
    c[4].x = w;  c[4].y = top - h;  c[4].z = -dd;
    c[5].x = w;  c[5].y = top - h;  c[5].z = dd;
    c[6].x = w;  c[6].y = top;      c[6].z = dd;
    c[7].x = w;  c[7].y = top;      c[7].z = -dd;
    for (i = 0; i < 8; i++)
        c[i].y += lift;
    for (i = 0; i < 8; i++) {
        zmathVecScaleXYZ(&c[i], &c[i], col->scale[0], col->scale[1], col->scale[2]);
        zmathVecRotateXYZ(&c[i], &c[i], (const uint16_t *)col->rot);
        c[i].x += (float)col->pos[0];
        c[i].y += (float)col->pos[1];
        c[i].z += (float)col->pos[2];
    }
    switch (zcolGetPropShape(col->defIndex)->cls) {
    case NGSHAPE_1:
        zAssertFail("Not coded", NGIFZCOLLIDE_SRC, 0x5EE, 0);
        return 0;
    case NGSHAPE_BOX:
        for (i = 0; i < 4; i++) {
            const RwV3d *p = &c[s_boxRails[i][0]], *q = &c[s_boxRails[i][1]];

            a[0] = (int16_t)p->x; a[1] = (int16_t)p->y; a[2] = (int16_t)p->z;
            b[0] = (int16_t)q->x; b[1] = (int16_t)q->y; b[2] = (int16_t)q->z;
            zcolMakeRail(&rails[i], shape->surface, a, b, (int8_t)s_boxRails[i][2]);
            rails[i].owner = col;
        }
        return 4;
    case NGSHAPE_RAIL:
        /* the exe mixes the corners' x, y with the middle z */
        a[0] = (int16_t)c[0].x; a[1] = (int16_t)c[0].y; a[2] = (int16_t)((c[1].z + c[0].z) * 0.5f);
        b[0] = (int16_t)c[4].x; b[1] = (int16_t)c[4].y; b[2] = (int16_t)((c[5].z + c[4].z) * 0.5f);
        zcolMakeRail(rails, shape->surface, a, b, 0);
        break;
    case NGSHAPE_EDGE:
        a[0] = (int16_t)c[s_edgeRail[0]].x; a[1] = (int16_t)c[s_edgeRail[0]].y; a[2] = (int16_t)c[s_edgeRail[0]].z;
        b[0] = (int16_t)c[s_edgeRail[1]].x; b[1] = (int16_t)c[s_edgeRail[1]].y; b[2] = (int16_t)c[s_edgeRail[1]].z;
        zcolMakeRail(rails, shape->surface, a, b, (int8_t)s_edgeRail[2]);
        break;
    default:
        return 0;
    }
    rails->owner = col;
    return 1;
}

/* VA 0x0046aa50, 339 bytes, [I] callers, status draft */
void zcolMakeRail(NgColEdge *e, uint8_t kind, const int16_t *a, const int16_t *b, int open)
{
    /* a prop rail: no code, horizontal normal cross(dir, down), d 0; open > 0 / < 0 = the low /
     * high end may be ridden off */
    RwV3d n, down = { 0.0f, -1.0f, 0.0f };
    int16_t len;

    e->flags = 0;
    e->code = 0;
    e->kind = kind;
    e->v0[0] = a[0]; e->v0[1] = a[1]; e->v0[2] = a[2];
    e->v1[0] = b[0]; e->v1[1] = b[1]; e->v1[2] = b[2];
    len = (int16_t)sqrtf((float)((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) +
                                 (b[2] - a[2]) * (b[2] - a[2])));
    e->length = len;
    e->dir[0] = (float)(int16_t)(b[0] - a[0]) / (float)len;
    e->dir[1] = (float)(int16_t)(b[1] - a[1]) / (float)len;
    e->dir[2] = (float)(int16_t)(b[2] - a[2]) / (float)len;
    zmathTriangleNormal(&n, (const RwV3d *)e->dir, &g_zmathZero, &down);
    e->normal[0] = n.x; e->normal[1] = n.y; e->normal[2] = n.z;
    e->d = 0.0f;
    if (open > 0)
        e->flags |= 1;
    else if (open < 0)
        e->flags |= 2;
}

/* VA 0x0046ac10, 30 bytes, [V] direct, status draft */
void zcolPropMove(void)
{
    NGIF_MISSING(NGIFZCOLLIDE_SRC, 1592, "prop move");
}

/* VA 0x0046ac40, 22 bytes, [I] split, status draft */
void zcolPropRotated(NgPropCol *col, const int16_t *euler)
{
    (void)euler;
    if (col->rails != NULL)
        zcolBuildPropRails(col, col->rails);
}

/* VA 0x0046ac70, 22 bytes, [I] split, status draft */
void zcolPropMoved(NgPropCol *col, const float *pos)
{
    (void)pos;
    if (col->rails != NULL)
        zcolBuildPropRails(col, col->rails);
}

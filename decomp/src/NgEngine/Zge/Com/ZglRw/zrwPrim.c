/*
 * NgEngine/Zge/Com/ZglRw/zrwPrim.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwPrim.c
 * Evidence: [V] path string 0x0056173C (assert of zrwPrimInit 0x248); the primitive driver table
 *           0x00561720 (7 slots); [I] the rest by link order and the globals 0x00672890-0x006728B3.
 *           The ZglRw pass moved here the buffer and clipping helpers 0x0048C5C0-0x0048CC2F (the
 *           zList / zrwPrim gap; they use this file's buffers) and the draw functions 0x0048CCF0-
 *           0x0048D6EF (the zrwPrim / ztaginf gap; table slots 2-6).
 * 12 functions, VA 0x0048C5C0-0x0048D6EF (3 new functions).
 * Draft. Non-matching C (AD-8). Types: ngif.h (ZglVertex, ZglLine/Tri/Quad), zglrw.h.
 * Description: docs/re/zrw.md 8.
 *
 * ZGL immediate mode: lines, triangles, quads and polygons of ZglVertex (ZGE position, u, v,
 * RGBA) drawn through RenderWare Im3D (3-D, ZGE world or camera space) or Im2D (screen space).
 *   flags & 0x00FFFFFF  render state for zrwZglSetRenderState
 *   flags & 0x01000000  vertices are in camera space (transformed by the current camera's matrix)
 *   flags & 0x02000000  2-D: x, y are pixels; drawn at the near screen z (+0.001 per primitive)
 */
#include "decomp.h"
#include "zmath.h"
#include "ngif.h"
#include "zglrw.h"

#define ZRWPRIM_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwPrim.c"
#define ZGLPRIM_CAMERA 0x01000000u
#define ZGLPRIM_2D     0x02000000u

typedef struct ZrwIm3DVertex { RwV3d pos; RwV3d normal; RwRGBA colour; float u, v; } ZrwIm3DVertex;     /* 0x24 */
typedef struct ZrwIm2DVertex { float x, y, z, recipZ; uint32_t colour; float pad; float u, v; } ZrwIm2DVertex; /* 0x20 */


extern void    *g_zrwCamera;           /* 0x006976F4 */
extern float    g_zrwWorldOrigin[3];   /* 0x006976CC */
extern float    g_zrwScale;            /* 0x00697720 */
extern uint32_t g_zrwFrameCount;       /* 0x00697744 */
extern uint32_t g_zrwRenderState;      /* 0x00697778 */
extern void    *g_zrwRenderRaster;     /* 0x0069777C */

static uint32_t s_frame;               /* 0x00672890 */
static struct {
    int32_t   initialized;             /* 0x00672898 */
    int32_t   vertexSize;              /* 0x0067289C 0x24 */
    uint8_t  *im3d;                    /* 0x006728A0 */
    uint32_t  im3dCap;                 /* 0x006728A4 */
    uint8_t  *im2d;                    /* 0x006728A8 */
    uint32_t  im2dCap;                 /* 0x006728AC */
} s_prim;                              /* memset 0x18 */
static float s_zBias;                  /* 0x006728B0 */

#define ALIGN16(p) ((void *)(((size_t)(p) + 0xF) & ~(size_t)0xF))

/* VA 0x0048c5c0, 41 bytes, [I] new function, status draft (no caller found) */
void zrwPrimReserve2D(uint32_t n)
{
    s_prim.im2dCap = (n & ~0x1Fu) + 0x20;
    s_prim.im2d = (uint8_t *)zmemResize(s_prim.im2d, s_prim.im2dCap * sizeof(ZrwIm2DVertex) + 0xF);
}

/* VA 0x0048c5f0, 46 bytes, [I] new function, status draft (no caller found) */
void zrwPrimReserve3D(uint32_t n)
{
    s_prim.im3dCap = (n & ~0x1Fu) + 0x20;
    s_prim.im3d = (uint8_t *)zmemResize(s_prim.im3d, s_prim.im3dCap * sizeof(ZrwIm3DVertex) + 0xF);
}

static void zrwPrimLerpVertex(ZglVertex *out, const ZglVertex *a, const ZglVertex *b, float f)
{
    out->u = (a->u - b->u) * f + b->u;
    out->v = (a->v - b->v) * f + b->v;
    out->colour.red   = (uint8_t)((a->colour.red   - b->colour.red)   * f + b->colour.red);
    out->colour.green = (uint8_t)((a->colour.green - b->colour.green) * f + b->colour.green);
    out->colour.blue  = (uint8_t)((a->colour.blue  - b->colour.blue)  * f + b->colour.blue);
    out->colour.alpha = (uint8_t)((a->colour.alpha - b->colour.alpha) * f + b->colour.alpha);
}

/* VA 0x0048c620, 261 bytes, unassigned, status draft (clip helper: the point of a-b at x) */
void zrwPrimClipX(float x, const ZglVertex *a, const ZglVertex *b, ZglVertex *out)
{
    float f = ((int)a->pos.x == (int)b->pos.x) ? 1.0f : (x - b->pos.x) / (a->pos.x - b->pos.x);
    out->pos.x = x;
    out->pos.y = (a->pos.y - b->pos.y) * f + b->pos.y;
    zrwPrimLerpVertex(out, a, b, f);
}

/* VA 0x0048c730, 263 bytes, unassigned, status draft (the point of a-b at y) */
void zrwPrimClipY(float y, const ZglVertex *a, const ZglVertex *b, ZglVertex *out)
{
    float f = ((int)a->pos.y == (int)b->pos.y) ? 1.0f : (y - b->pos.y) / (a->pos.y - b->pos.y);
    out->pos.y = y;
    out->pos.x = (a->pos.x - b->pos.x) * f + b->pos.x;
    zrwPrimLerpVertex(out, a, b, f);
}

/* one Sutherland-Hodgman pass: keep the side of the edge where `inside` holds */
static uint32_t zrwPrimClipPass(ZglVertex **in, uint32_t n, ZglVertex **out, ZglVertex **spare,
                                int axisY, float edge, int keepGreater)
{
    uint32_t i, m = 0;
    ZglVertex *prev = in[n - 1];
    for (i = 0; i < n; i++) {
        ZglVertex *cur = in[i];
        float pc = axisY ? prev->pos.y : prev->pos.x, cc = axisY ? cur->pos.y : cur->pos.x;
        int pin = keepGreater ? (pc >= edge) : (pc <= edge);
        int cin = keepGreater ? (cc >= edge) : (cc <= edge);
        if (pin && cin)
            out[m++] = cur;
        else if (pin != cin) {
            ZglVertex *v = (*spare)++;
            if (axisY) zrwPrimClipY(edge, cin ? cur : prev, cin ? prev : cur, v);
            else       zrwPrimClipX(edge, cin ? cur : prev, cin ? prev : cur, v);
            out[m++] = v;
            if (cin)
                out[m++] = cur;
        }
        prev = cur;
    }
    return m;
}

/* VA 0x0048c840, 1002 bytes, unassigned, status draft (2-D quads of a windowed camera)
 * Clips a 2-D polygon (optionally transformed first) against the camera window rect
 * { x0, y0, x1, y1 } (y0, x0, y1, x1 in that order) and draws what is left; returns its vertex
 * count (0 = nothing left). */
uint32_t zrwPrimDrawClipped(uint32_t flags, uint32_t material, ZglVertex **verts, int n,
                            const RwMatrix *m, const float *rect)
{
    ZglVertex local[16], extra[32], *bufA[32], *bufB[32], *spare = extra;
    ZglPoly poly;
    uint32_t k;
    int i;

    for (i = 0; i < n; i++) {
        local[i] = *verts[i];
        if (m != NULL)
            zmathTransformPoint(&local[i].pos, m, &verts[i]->pos);
        bufA[i] = &local[i];
    }
    k = zrwPrimClipPass(bufA, n, bufB, &spare, 1, rect[1], 1);
    if (k <= 2) return 0;
    k = zrwPrimClipPass(bufB, k, bufA, &spare, 0, rect[0], 1);
    if (k <= 2) return 0;
    k = zrwPrimClipPass(bufA, k, bufB, &spare, 1, rect[3], 0);
    if (k <= 2) return 0;
    k = zrwPrimClipPass(bufB, k, bufA, &spare, 0, rect[2], 0);
    if (k <= 2) return 0;
    poly.material = material;
    poly.v = bufA;
    poly.count = k;
    zrwPrimDrawPolys(flags, &poly, 1, NULL);
    return k;
}

/* VA 0x0048cc30, 127 bytes, [V] direct, status draft (prim slot 0) */
int zrwPrimInit(void)
{
    if (s_prim.initialized)
        zAssertFail("Already initialized", ZRWPRIM_SRC, 0x248, 0);
    zstringMemSet(&s_prim, 0, sizeof(s_prim));
    s_prim.initialized = 1;
    s_prim.vertexSize = 0x24;
    s_prim.im3dCap = 0x40;
    s_prim.im3d = (uint8_t *)zmemAlloc(0x40 * 0x24 + 0xF);
    s_prim.im2dCap = 0x40;
    s_prim.im2d = (uint8_t *)zmemAlloc(0x40 * 0x20 + 0xF);
    return 1;
}

/* VA 0x0048ccb0, 61 bytes, [I] new function, status draft (slot 1) */
int zrwPrimShutdown(void)
{
    if (s_prim.initialized) {
        if (s_prim.im3d != NULL)
            zmemFree(s_prim.im3d);
        if (s_prim.im2d != NULL)
            zmemFree(s_prim.im2d);
        s_prim.initialized = 0;
    }
    return 1;
}

/* the render state + the texture of the primitive's material */
static void zrwPrimSetState(uint32_t flags, uint32_t material)
{
    void *raster = material ? zrwMatGetRaster((ZglMaterial *)(size_t)material, 0) : NULL;
    if ((flags & 0xFFFFFF) != g_zrwRenderState || raster != g_zrwRenderRaster)
        zrwZglSetRenderState(flags & 0xFFFFFF, raster);
}

/* ZGE vertex -> Im3D vertex (world: origin + scaled, y z flipped; camera: camera matrix) */
static void zrwPrimToIm3D(ZrwIm3DVertex *o, const ZglVertex *v, uint32_t flags, const RwMatrix *m,
                          const RwMatrix *cam)
{
    RwV3d p = v->pos;
    if (m != NULL)
        zmathTransformPoint(&p, m, &v->pos);
    if ((flags & ZGLPRIM_CAMERA) == 0) {
        o->pos.x = g_zrwScale * p.x + g_zrwWorldOrigin[0];
        o->pos.y = g_zrwWorldOrigin[1] - p.y * g_zrwScale;
        o->pos.z = g_zrwWorldOrigin[2] - p.z * g_zrwScale;
    } else {
        o->pos.x = -(g_zrwScale * p.x);
        o->pos.y = -(p.y * g_zrwScale);
        o->pos.z = p.z * g_zrwScale;
        RwV3dTransformVectors(&o->pos, &o->pos, 1, cam);
        o->pos.x += cam->pos.x;
        o->pos.y += cam->pos.y;
        o->pos.z += cam->pos.z;
    }
    o->u = v->u;
    o->v = v->v;
    o->colour = v->colour;
}

static void zrwPrimToIm2D(ZrwIm2DVertex *o, const ZglVertex *v, const RwMatrix *m, float z, float recipZ)
{
    RwV3d p = v->pos;
    if (m != NULL)
        zmathTransformPoint(&p, m, &v->pos);
    o->x = p.x;
    o->y = p.y;
    o->z = z;
    o->recipZ = recipZ;
    o->u = v->u;
    o->v = v->v;
    o->colour = ((uint32_t)v->colour.alpha << 24) | ((uint32_t)v->colour.red << 16) |
                ((uint32_t)v->colour.green << 8) | v->colour.blue;
}

/* shared body of drawLines / drawPolys: prims of `stride` words, vertex pointers from word 1 (count
 * fixed, or word 2 = count when `fixed` is 0), primitive type for Im3D */
static void zrwPrimDraw(uint32_t flags, const uint32_t *prim, int count, const RwMatrix *m,
                        int stride, int fixed, int rwPrimType)
{
    const RwMatrix *cam = NULL;
    if (s_frame != g_zrwFrameCount)
        s_frame = g_zrwFrameCount;
    s_zBias = 0.0f;
    if ((flags & ZGLPRIM_2D) == 0) {
        if (flags & ZGLPRIM_CAMERA)
            cam = zrwCamGetRwMatrix(zrwCamGetCurrent());
        for (; count != 0; count--, prim += stride) {
            int n = fixed ? fixed : (int)prim[2], i;
            ZglVertex *const *v = fixed ? (ZglVertex *const *)&prim[1] : (ZglVertex *const *)(size_t)prim[1];
            ZrwIm3DVertex *o;
            if (s_prim.im3dCap < (uint32_t)n)
                zrwPrimReserve3D(n);
            o = (ZrwIm3DVertex *)ALIGN16(s_prim.im3d);
            for (i = 0; i < n; i++)
                zrwPrimToIm3D(&o[i], v[i], flags, m, cam);
            if (RwIm3DTransform(o, n, NULL, 1)) {
                zrwPrimSetState(flags, prim[0]);
                RwIm3DRenderPrimitive(rwPrimType);
                RwIm3DEnd();
            }
        }
    } else {
        float recip = 1.0f / RwCameraGetNearClipPlane(g_zrwCamera);
        float bias = s_zBias;
        float z = RwIm2DGetNearScreenZ() + s_zBias;
        s_zBias += 0.001f;
        for (; count != 0; count--, prim += stride) {
            int n = fixed ? fixed : (int)prim[2], i;
            ZglVertex *const *v = fixed ? (ZglVertex *const *)&prim[1] : (ZglVertex *const *)(size_t)prim[1];
            ZrwIm2DVertex *o;
            if (s_prim.im2dCap < (uint32_t)n)
                zrwPrimReserve2D(n);
            o = (ZrwIm2DVertex *)ALIGN16(s_prim.im2d);
            for (i = 0; i < n; i++)
                zrwPrimToIm2D(&o[i], v[i], m, z, recip + bias);
            zrwPrimSetState(flags, prim[0]);
            RwIm2DRenderPrimitive(rwPrimType, o, n);
        }
    }
}

/* VA 0x0048ccf0, 1065 bytes, unassigned, status draft (slot 2: ZglLine[count], line lists) */
void zrwPrimDrawLines(uint32_t flags, const ZglLine *lines, int count, const RwMatrix *m)
{
    zrwPrimDraw(flags, (const uint32_t *)lines, count, m, 3, 2, 1 /* rwPRIMTYPELINELIST */);
}

/* VA 0x0048d120, 77 bytes, unassigned, status draft (slot 3: each triangle as a 3-vertex polygon) */
void zrwPrimDrawTris(uint32_t flags, const ZglTri *tris, int count, const RwMatrix *m)
{
    for (; count != 0; count--, tris++) {
        ZglPoly p;
        p.material = tris->material;
        p.v = (ZglVertex **)tris->v;
        p.count = 3;
        zrwPrimDrawPolys(flags, &p, 1, m);
    }
}

/* VA 0x0048d170, 1067 bytes, unassigned, status draft (slot 5: ZglPoly[count] as triangle fans) */
void zrwPrimDrawPolys(uint32_t flags, const ZglPoly *polys, int count, const RwMatrix *m)
{
    zrwPrimDraw(flags, (const uint32_t *)polys, count, m, 3, 0, 5 /* rwPRIMTYPETRIFAN */);
}

/* VA 0x0048d5a0, 185 bytes, unassigned, status draft (slot 4: polygons as triangle strips) */
void zrwPrimDrawStrips(uint32_t flags, const ZglPoly *polys, int count, const RwMatrix *m)
{
    for (; count != 0; count--, polys++) {
        ZglVertex *tri[3];
        ZglPoly p;
        int i;
        p.material = polys->material;
        p.count = 3;
        for (i = 0; i + 2 < polys->count; i += 2) {
            p.v = polys->v + i;
            zrwPrimDrawPolys(flags, &p, 1, m);
            if (i + 3 < polys->count) {
                tri[0] = polys->v[i + 3];
                tri[1] = polys->v[i + 2];
                tri[2] = polys->v[i + 1];
                p.v = tri;
                zrwPrimDrawPolys(flags, &p, 1, m);
            }
        }
    }
}

/* VA 0x0048d660, 135 bytes, unassigned, status draft (slot 6: quads; 2-D quads of a windowed
 * camera are clipped to its window) */
void zrwPrimDrawQuads(uint32_t flags, const ZglQuad *quads, int count, const RwMatrix *m)
{
    ZglCamera *cam;
    if ((flags & ZGLPRIM_2D) && (cam = zrwCamGetCurrent()) != NULL && (cam->flags & 1)) {
        zrwPrimDrawClipped(flags, quads->material, (ZglVertex **)quads->v, 4, m, cam->window);
        return;   /* sic: only the first quad [V] */
    }
    for (; count != 0; count--, quads++) {
        ZglPoly p;
        p.material = quads->material;
        p.v = (ZglVertex **)quads->v;
        p.count = 4;
        zrwPrimDrawPolys(flags, &p, 1, m);
    }
}

/* the table's draw slots take const void * (casts, the functions keep their real types) */
#define PRIM_SLOT(f) ((void (__cdecl *)(uint32_t, const void *, int, const RwMatrix *))(f))
const ZglPrimApi g_zglPrimApi = {                                           /* 0x00561720 */
    zrwPrimInit, zrwPrimShutdown, PRIM_SLOT(zrwPrimDrawLines), PRIM_SLOT(zrwPrimDrawTris),
    PRIM_SLOT(zrwPrimDrawStrips), PRIM_SLOT(zrwPrimDrawPolys), PRIM_SLOT(zrwPrimDrawQuads)
};

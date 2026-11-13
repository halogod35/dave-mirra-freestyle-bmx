/*
 * NgEngine/Zge/Com/ZglRw/zrwShadow.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwShadow.c
 * Evidence: [V] path string 0x00562BDC (assert of zrwShadowInit 0x29F); [I] the rest by link order
 *           and the globals 0x006746F0-0x0067520F (DM-059 created 4).
 * 5 functions, VA 0x0049B330-0x0049B87F. DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h,
 * ngif.h (ZglVertex, ZglQuad). Description: docs/re/zrw.md 6.
 *
 * The blob shadow: a 6 x 6 grid of vertices (5 x 5 quads) laid out in a square of the given size
 * around the owner, oriented by the shadow direction, each vertex dropped onto the world with the
 * collision line test (zrwCol slot 2) and lifted size/10 along the hit normal; vertices that miss
 * drop their quads. The quads are drawn with the "ShadowMap" material (assets\shadowMap.Bmp, alpha
 * from the vertex colour) through zrwPrim slot 6 with primitive flags 0x4103 every frame (ZGL
 * callback list 2).
 */
#include "decomp.h"
#include "zmath.h"
#include "ngif.h"
#include "zglrw.h"

#define ZRWSHADOW_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwShadow.c"
#define GRID 6

int       zrwZglGetValue(int what, void *out, void *arg);    /* zrwZgl.c (system slot 8) */
float    *zobjGetPosition(void *obj);                           /* zobj.c: the object position */
RwMatrix *zmathMatrixLookAt(RwMatrix *m, const RwV3d *eye, const RwV3d *target);
extern const ZglPrimApi g_zglPrimApi;                         /* 0x00561720 */
extern const ZglColApi  g_zglColApi;                          /* 0x00562060 */


static RwV3d     s_world[GRID * GRID][1];   /* 0x006746F0 (0x10 stride: RwV3d + pad) */
static ZglQuad   s_quads[25];               /* 0x00674930 */
static int32_t   s_initialized;             /* 0x00674B28 */
static float     s_grid[GRID * GRID][4];    /* 0x00674B34 x, y, z, 0 in the shadow plane */
static ZglVertex s_verts[GRID * GRID];      /* 0x00674D74 */
static int32_t   s_miss[GRID * GRID];       /* 0x00675164 */
static void     *s_material;                /* 0x006751F4 ShadowMap */
static void     *s_callback;                /* 0x006751F8 */
static int32_t   s_ready;                   /* 0x006751FC */
static float     s_lift;                    /* 0x00675200 size * 0.1 */
static float     s_size;                    /* 0x00675204 */
static int32_t   s_gridInit;                /* 0x0067520C */
static const int32_t s_corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };   /* 0x0054E104 */

/* VA 0x0049b330, 160 bytes, [V] direct, status draft */
int zrwShadowInit(void)
{
    int size;
    void *buf;

    if (s_initialized)
        zAssertFail("Already initialized", ZRWSHADOW_SRC, 0x29F, 0);
    s_initialized = 1;
    buf = zfileLoadAll("assets\\shadowMap.Bmp", &size);
    if (buf == NULL)
        s_material = NULL;
    else {
        s_material = g_zglMaterialApi.createFromImage(buf, size, "ShadowMap", ZGL_IMAGE_BMP);
        zfileFreeLoaded(buf);
    }
    s_ready = 0;
    s_callback = zrwZglAddCallback(2, (void *)zrwShadowDraw, NULL, 1);
    return 1;
}

/* VA 0x0049b3d0, 258 bytes, [I] DM-059 created, status draft (ZGL callback list 2)
 * Draws the quads whose four corners hit the ground (missing corners skip the quad). */
int zrwShadowDraw(void)
{
    int n = 0, row, col, k;

    if (!s_ready)
        return 1;
    for (row = 0; row < GRID - 1; row++)
        for (col = 0; col < GRID - 1; col++) {
            ZglQuad *q = &s_quads[n];
            float maxY = -3.4028235e+38f, minY = 3.4028235e+38f;
            q->material = (uint32_t)(size_t)s_material;
            for (k = 0; k < 4; k++) {
                int i = (row + s_corner[k][0]) * GRID + col + s_corner[k][1];
                if (s_miss[i])
                    break;
                q->v[k] = &s_verts[i];
                if (maxY < s_verts[i].pos.y) maxY = s_verts[i].pos.y;
                if (s_verts[i].pos.y < minY) minY = s_verts[i].pos.y;
            }
            if (k == 4)
                n++;
        }
    if (n != 0)
        g_zglPrimApi.drawQuads(0x4103, s_quads, n, NULL);
    s_ready = 0;
    return 1;
}

/* VA 0x0049b4e0, 44 bytes, [I] DM-059 created, status draft (no caller found) */
int zrwShadowShutdown(void)
{
    if (s_initialized) {
        if (s_callback != NULL)
            zrwZglRemoveCallback(s_callback);
        s_initialized = 0;
    }
    return 1;
}

/* VA 0x0049b510, 1 bytes, [I] DM-059 created, status draft (empty) */
void zrwShadowNop(void)
{
}

/* VA 0x0049b520, 851 bytes, [I] DM-059 created, status draft (NgIFzroller 0x0046C680 calls it)
 * Builds this frame's shadow for one owner. */
void zrwShadowUpdate(ZrwShadow *sh)
{
    RwMatrix view, m;
    RwV3d pos, eye, from, to, t, normal[2];
    float r, half, step;
    int i, row, col;

    if (!s_gridInit) {
        zrwZglGetValue(1, &r, NULL);           /* the shadow radius (ZGL value 1) */
        s_size = r + r;
        half = s_size * 0.5f;
        step = s_size * 0.2f;
        for (row = 0; row < GRID; row++)
            for (col = 0; col < GRID; col++) {
                float *g = s_grid[row * GRID + col];
                g[0] = (float)row * step - half;
                g[1] = ((float)col * s_size * 0.2f - half) * -1.0f;
                g[2] = 0.0f;
            }
        s_lift = r * 0.1f;
        s_gridInit = 1;
    }
    pos = *(RwV3d *)zobjGetPosition(*(void **)(sh->owner + 0x14));
    zmathVecAdd(&eye, &pos, &sh->dir);
    zmathMatrixLookAt(&view, &eye, &pos);
    zmathMatrixInvertRigid(&view, &view);
    t.x = t.y = t.z = 0.0f;
    zmathMatrixIdentity(&m);
    m.pos = pos;
    zmathMatrixCopyRotation(&m, &view);
    zmathTransformVector(&t, &m, &t);
    zmathMatrixTranslate(&m, &m, &t);
    for (i = 0; i < GRID * GRID; i++) {
        row = i / GRID;
        col = i % GRID;
        from = eye;
        s_miss[i] = 1;
        zmathTransformPoint(s_world[i], &m, (RwV3d *)s_grid[i]);
        zmathVecLerp(&to, &from, s_world[i], 10.0f);
        if (g_zglColApi.lineTest(&from, &to, 1, 0, normal)) {
            /* normal[0] = hit point, normal[1] = normal [I: ZrwColHit head] */
            zmathVecScale(&normal[1], &normal[1], s_lift);
            zmathVecAdd(&s_verts[i].pos, &normal[0], &normal[1]);
            s_verts[i].u = (float)row * 0.2f;
            s_verts[i].v = (float)(GRID - 1 - col) * 0.2f;
            s_verts[i].colour.red = s_verts[i].colour.green = s_verts[i].colour.blue =
                s_verts[i].colour.alpha = (uint8_t)0x80;   /* [I] __ftol of a fade value */
            s_miss[i] = 0;
        }
    }
    s_ready = 1;
}

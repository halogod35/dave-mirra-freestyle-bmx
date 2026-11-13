/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFpsx.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFpsx.c
 * Evidence: [V] path string 0x0055C6A0: the DrawPrim "Prim not coded" messages (lines 0x505..0x577)
 *           and the asserts of psxDrawRect (0x63F), psxDrawSpriteList (0x7BD), psxDrawPoly3D
 *           (0x838, 0x853), psx2DDrawGlyph (0xAEA, 0xAEF); [I] the rest by link order (from the
 *           end of NgIFfx.c to NgIFzactor.c's first function) and the shared globals.
 * 39 functions, VA 0x0045fe00-0x004646d6 (DM-057 absorbed the NgIFfx/NgIFpsx and NgIFpsx/NgIFzactor
 * gaps and created psxNop, psxSceneDraw, psx2DDraw, psxNop2, psxUnusedTrue1/2).
 * DM-057 (draft). Non-matching C (AD-8). Types / prototypes: ngif.h, zoverii.h. Description:
 * docs/re/ngif.md.
 *
 * The game's 2-D graphics were written for the PlayStation GPU: they build libgpu primitives
 * (POLY_F3/F4/G4/FT4/GT4, LINE_F2/G2) into an ordering table. On the PC this file keeps that
 * interface and draws the primitives with the ZGL immediate-mode API:
 *   - the overlay / 3-D layer (zoverii.c, overlay.c, the physics / game debug draws): packets at
 *     g_psxPacket into g_psxOrderingTable[0], drawn by psxSceneDraw (frame callback);
 *   - the menu 2-D layer (zprim.c): packets at s_2DPacket into a descending OT pointer (each new
 *     primitive goes one slot further, so later ones are drawn on top), drawn by psx2DDraw.
 * Screen coordinates are PSX 640 x 480 (or 512 wide with PSXM_WIDE512; the menus add 104 to y),
 * scaled to the window. A PC "mode" word per packet selects 3-D float positions, alpha, depth.
 */
#include <stdlib.h>

#include "decomp.h"
#include "ngif.h"

#define NGIFPSX_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFpsx.c"

/* ------------------------------------------------------------------ other modules */
extern void *(*g_texDriverVtbl[15])(void *);                  /* 0x005623F0 zrwTxr.c; [5] width, [6] height */
extern void (__cdecl *g_zglCameraSetViewport)(void *cam, int a, const float *rect); /* 0x00561DB8 -> zrwCam_00492DE0 */
extern ZmemFreeList *g_ngappFreeList;                         /* 0x0068FF94 NgApp.c */

/* ------------------------------------------------------------------ globals (this file) */
float        g_psxScreenW;                 /* 0x0055C690 */
float        g_psxScreenH;                 /* 0x0055C694 */
uint32_t    *g_psxOrderingTable;           /* 0x0055C698 */
static uint32_t s_tintLevel;               /* 0x0055C68C psx2DSetTint level (0 = 255) */
static int   s_sceneDirty;                 /* 0x005D24FC packets queued for psxSceneDraw */
static int   s_sceneHandle;                /* 0x005D2500 */
static int   s_2DDirty;                    /* 0x005D2504 */
static int   s_2DHandle;                   /* 0x005D2508 */
static uint8_t *s_2DBuffer;                /* 0x005D250C */
static uint32_t *s_2DPacket;               /* 0x005D2510 */
static uint8_t s_2DBufferStatic[0x27000];  /* 0x005AB4FC */
static uint8_t s_packetBuffer[0x13E80];    /* 0x00678560 */
static uint32_t *s_ot2D;                   /* 0x0068C3D0 next 2-D OT slot (descending) */
ZoverTexture g_psxTextures[0x200];         /* 0x0068C3E0 (to 0x0068EBE0) */
uint32_t    *g_psxPacket;                  /* 0x0068EBE0 */
static uint32_t s_tint;                    /* 0x0068FC00 psx2DSetTint colour (0 = none) */

/* ------------------------------------------------------------------ helpers */

#define TAG_LEN(n)       ((uint32_t)(n) << 28)

/* PSX addPrim: link packet p (length n words) into the OT entry slot */
static void addPrim(uint32_t *slot, void *p, uint32_t len)
{
    uint32_t *tag = (uint32_t *)p;

    *tag = (*tag & 0x0FFFFFFFu) | TAG_LEN(len);
    *tag = ((*slot ^ *tag) & 0x0FFFFFFFu) ^ *tag;
    *slot = ((*slot ^ (uint32_t)(uint32_t)p) & 0x0FFFFFFFu) ^ *slot;
}

/* one 2-D primitive into the menu layer: the OT pointer moves on so later ones draw on top */
static void add2D(void *p, uint32_t len, uint32_t bytes)
{
    addPrim(s_ot2D, p, len);
    s_ot2D--;
    s_2DPacket = (uint32_t *)((uint8_t *)p + bytes);
}

/* the semi-transparency rate packet (DR_TPAGE 0xE1) that follows a blended primitive */
static uint32_t *addRatePacket(uint32_t *p, uint32_t *slot, int rate)
{
    if (rate < 0)
        rate = 0;
    else if (rate > 3)
        rate = 3;
    p[0] = (p[0] & 0x0FFFFFFFu) | TAG_LEN(1);
    p[1] = ((uint32_t)rate & 3u | 0xFF080030u) << 5;
    addPrim(slot, p, 1);
    return p + 2;
}

static void corner(ZglVertex *v, int16_t x, int16_t y, const float *pos, uint32_t mode)
{
    if (mode & PSXM_3D) {
        v->pos.x = pos[0];
        v->pos.y = pos[1];
        v->pos.z = pos[2];
    } else {
        v->pos.x = (float)x * g_psxScreenW * ((mode & PSXM_WIDE512) ? (1.0f / 512.0f) : (1.0f / 640.0f));
        v->pos.y = (float)y * g_psxScreenH * (1.0f / 480.0f);
        v->pos.z = 0.0f;
    }
}

static void rgba(ZglVertex *v, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    v->colour.red = r;
    v->colour.green = g;
    v->colour.blue = b;
    v->colour.alpha = a;
}

/* ZGL blend flags and alpha of a semi-transparent primitive (blend rate 0..3) */
static uint32_t semiFlags(uint8_t rate, uint8_t *alpha)
{
    switch (rate) {
    case 0:  *alpha = 0x80; return 0x6502;   /* 50% back + 50% front */
    case 2:  *alpha = 0x80; return 0x4102;   /* back - front */
    default: *alpha = 0xFF; return 0x2202;   /* back + front */
    }
}

/* quads: 2-D or 3-D, depth unless PSXM_DEPTH, opaque bits per type */
static uint32_t quadFlags(uint32_t mode, int semi, uint8_t rate, uint32_t opaque, int keepSpace, uint8_t *alpha)
{
    uint32_t f = (mode & PSXM_3D) ? 0 : ZGL_PRIM_2D;

    if (keepSpace && !(mode & PSXM_KEEPSPACE))
        f = ZGL_PRIM_2D | 1;
    if (!(mode & PSXM_DEPTH))
        f |= 4;
    if (!semi) {
        *alpha = 0xFF;
        return f | opaque;
    }
    f |= semiFlags(rate, alpha);
    if (mode & PSXM_ALPHA)
        *alpha = (uint8_t)(mode >> 7);
    return f;
}

static void drawQuad4(uint32_t flags, uint32_t material, ZglVertex v[4])
{
    ZglQuad q;

    /* PSX corner order A B C D (a Z) -> ZGL order B A C D */
    q.material = material;
    q.v[0] = &v[1]; q.v[1] = &v[0]; q.v[2] = &v[2]; q.v[3] = &v[3];
    g_zglDrawQuads(flags, &q, 1, 0, 0);
}

static float texSize(uint32_t material, int which)
{
    void *tex = g_zglMatGetTexture((void *)(uint32_t)material, 0);

    return (float)(int16_t)(int32_t)g_texDriverVtbl[which](tex);
}

/* ------------------------------------------------------------------ the primitive renderers */

/* VA 0x0045fe00, 56 bytes, [I] between, status draft */
void psxSetClipPrim(PsxClip *p, const int16_t *rect)
{
    p->code = 0xF8;
    p->tag = (p->tag & 0x0FFFFFFFu) | TAG_LEN(5);
    p->x = rect[0];
    p->y = rect[1];
    p->w = rect[2];
    p->h = rect[3];
}

/* VA 0x0045fe50, 197 bytes, [I] callers, status draft */
void psxDrawClip(const PsxClip *p)
{
    /* code 0xF8: the camera's viewport (w = h = 0: the whole window) */
    float rect[4], scale = 1.0f;

    if (p->w == 0 && p->h == 0) {
        g_zglCameraSetViewport(g_zglGetCurrentCamera(), 0, NULL);
        return;
    }
    g_zglGet(11, &scale, 0);
    scale *= 1.0f / 480.0f;
    rect[0] = (float)(uint32_t)p->x;
    rect[1] = (float)(uint32_t)p->y - scale * 32.0f;
    rect[2] = (float)(uint32_t)p->w;
    rect[3] = (float)(uint32_t)p->h;
    g_zglCameraSetViewport(g_zglGetCurrentCamera(), 0, rect);
}

/* VA 0x0045ff60, 76 bytes, [I] callers, status draft */
void psxClearOTagR(uint32_t *ot, int16_t n)
{
    /* libgpu ClearOTagR: each entry links to the one before it, ot[0] is the terminator */
    int16_t i;

    for (i = (int16_t)(n - 1); i > 0; i--)
        ot[i] = (uint32_t)(uint32_t)&ot[i - 1] & 0x0FFFFFFFu;
    ot[0] |= 0x0FFFFFFFu;
}

/* VA 0x0045ffd0, 1287 bytes, [I] callers, status draft */
void psxDrawPolyGT4(const PsxPolyGT4 *p, int semi)
{
    float tw = texSize(p->texture, 5), th = texSize(p->texture, 6);
    uint8_t alpha;
    uint32_t f = quadFlags(p->mode, semi, (uint8_t)((p->tpage >> 5) & 3), 0x120A, 0, &alpha);
    ZglVertex v[4];
    int i;

    corner(&v[0], p->x0, p->y0, p->pos[0], p->mode);
    rgba(&v[0], p->r0, p->g0, p->b0, alpha);
    if (p->mode & PSXM_FLOAT_UV) {
        v[0].u = p->uvf[0][0];
        v[0].v = p->uvf[0][1];
    } else {
        v[0].u = ((float)p->u0 + 0.01f) / tw;
        v[0].v = ((float)p->v0 + 0.01f) / th;
    }
    for (i = 0; i < 3; i++) {
        corner(&v[i + 1], p->c[i].x, p->c[i].y, p->pos[i + 1], p->mode);
        rgba(&v[i + 1], p->c[i].r, p->c[i].g, p->c[i].b, alpha);
        if (p->mode & PSXM_FLOAT_UV) {
            v[i + 1].u = p->uvf[i + 1][0];
            v[i + 1].v = p->uvf[i + 1][1];
        } else {
            v[i + 1].u = ((float)p->c[i].u + 0.01f) / tw;
            v[i + 1].v = ((float)p->c[i].v + (i == 0 ? 0.01f : -0.01f)) / th;  /* bottom row pulled in */
        }
    }
    drawQuad4(f, p->texture, v);
}

/* VA 0x00460640, 1005 bytes, [I] callers, status draft */
void psxDrawPolyG4(const PsxPolyG4 *p, int semi)
{
    const ZoverPolyG4 *g = &p->p;
    uint8_t alpha;
    uint32_t f = quadFlags(g->mode, semi, g->blend, 0x120A, 0, &alpha);
    ZglVertex v[4];

    corner(&v[0], g->x0, g->y0, p->pos[0], g->mode); rgba(&v[0], g->r0, g->g0, g->b0, alpha);
    corner(&v[1], g->x1, g->y1, p->pos[1], g->mode); rgba(&v[1], g->r1, g->g1, g->b1, alpha);
    corner(&v[2], g->x2, g->y2, p->pos[2], g->mode); rgba(&v[2], g->r2, g->g2, g->b2, alpha);
    corner(&v[3], g->x3, g->y3, p->pos[3], g->mode); rgba(&v[3], g->r3, g->g3, g->b3, alpha);
    v[0].u = v[0].v = v[1].u = v[1].v = v[2].u = v[2].v = v[3].u = v[3].v = 0.0f;
    drawQuad4(f, 0, v);
}

/* VA 0x00460b40, 1316 bytes, [I] callers, status draft */
void psxDrawPolyFT4(const PsxPolyFT4 *p, int semi)
{
    const ZoverPolyFT4 *t = &p->p;
    float tw = texSize(t->texture, 5), th = texSize(t->texture, 6);
    uint8_t alpha;
    uint32_t f = quadFlags(t->mode, semi, t->blend, 8, 1, &alpha);
    ZglVertex v[4];
    int i;

    corner(&v[0], t->x0, t->y0, p->pos[0], t->mode);
    corner(&v[1], t->x1, t->y1, p->pos[1], t->mode);
    corner(&v[2], t->x2, t->y2, p->pos[2], t->mode);
    corner(&v[3], t->x3, t->y3, p->pos[3], t->mode);
    for (i = 0; i < 4; i++)
        rgba(&v[i], t->r, t->g, t->b, alpha);
    if (t->mode & PSXM_FLOAT_UV) {
        for (i = 0; i < 4; i++) {
            v[i].u = p->uvf[i][0];
            v[i].v = p->uvf[i][1];
        }
    } else {
        v[0].u = ((float)t->u0 + 0.01f) / tw; v[0].v = ((float)t->v0 + 0.01f) / th;
        v[1].u = ((float)t->u1 + 0.01f) / tw; v[1].v = ((float)t->v1 + 0.01f) / th;
        v[2].u = ((float)t->u2 + 0.01f) / tw; v[2].v = ((float)t->v2 - 0.01f) / th;
        v[3].u = ((float)t->u3 + 0.01f) / tw; v[3].v = ((float)t->v3 - 0.01f) / th;
    }
    drawQuad4(f, t->texture, v);
}

/* VA 0x004611d0, 1033 bytes, [I] callers, status draft */
void psxDrawPolyF4(const PsxPolyF4 *p, int semi)
{
    const ZoverPolyF4 *q = &p->p;
    uint8_t alpha;
    uint32_t f = quadFlags(q->mode, semi, q->blend, 8, 1, &alpha);
    ZglVertex v[4];
    int i;

    corner(&v[0], q->x0, q->y0, p->pos[0], q->mode);
    corner(&v[1], q->x1, q->y1, p->pos[1], q->mode);
    corner(&v[2], q->x2, q->y2, p->pos[2], q->mode);
    corner(&v[3], q->x3, q->y3, p->pos[3], q->mode);
    for (i = 0; i < 4; i++) {
        rgba(&v[i], q->r, q->g, q->b, alpha);
        v[i].u = v[i].v = 0.0f;
    }
    drawQuad4(f, 0, v);
}

/* VA 0x00461700, 784 bytes, [I] callers, status draft */
void psxDrawPolyF3(const PsxPolyF3 *p, int semi)
{
    uint32_t f = (p->mode & PSXM_3D) ? 0 : (ZGL_PRIM_2D | 2);
    uint8_t alpha = 0;   /* opaque: the exe leaves it uninitialised */
    ZglVertex v[3];
    ZglTri t;
    int i;

    if (!semi)
        f |= 0x1200;
    else
        f |= semiFlags(p->blend, &alpha);
    for (i = 0; i < 3; i++) {
        corner(&v[i], p->xy[i][0], p->xy[i][1], p->pos[i], p->mode);
        rgba(&v[i], p->r, p->g, p->b, alpha);
        v[i].u = v[i].v = 0.0f;
    }
    t.material = 0;
    t.v[0] = &v[0]; t.v[1] = &v[1]; t.v[2] = &v[2];
    g_zglDrawTris(f, &t, 1, 0, 0);
}

/* VA 0x00461af0, 525 bytes, [I] callers, status draft */
void psxDrawLineF2(const PsxLineF2 *p, int semi)
{
    uint32_t f = (p->mode & PSXM_3D) ? 0 : (ZGL_PRIM_2D | 2);
    uint8_t alpha;
    ZglVertex v[2];
    ZglLine l;

    f |= semi ? semiFlags(p->blend, &alpha) : 0x1200;
    corner(&v[0], p->xy[1][0], p->xy[1][1], p->pos[1], p->mode);
    corner(&v[1], p->xy[0][0], p->xy[0][1], p->pos[0], p->mode);
    rgba(&v[0], p->r, p->g, p->b, 0x80);
    rgba(&v[1], p->r, p->g, p->b, 0x80);
    v[0].u = v[0].v = v[1].u = v[1].v = 0.0f;
    l.material = 0;
    l.v[0] = &v[0];
    l.v[1] = &v[1];
    g_zglDrawLines(f, &l, 1, 0, 0);
}

/* VA 0x00461da0, 515 bytes, [I] callers, status draft */
void psxDrawLineG2(const PsxLineG2 *p, int semi)
{
    /* drawn from point 1 to point 0; the exe gives point 1 the colour of point 0 and vice versa */
    uint32_t f = (p->mode & PSXM_3D) ? 0 : (ZGL_PRIM_2D | 2);
    uint8_t alpha;
    ZglVertex v[2];
    ZglLine l;

    f |= semi ? semiFlags(p->blend, &alpha) : 0x1200;
    corner(&v[0], p->x1, p->y1, p->pos[1], p->mode);
    corner(&v[1], p->x0, p->y0, p->pos[0], p->mode);
    rgba(&v[0], p->r0, p->g0, p->b0, 0x80);
    rgba(&v[1], p->r1, p->g1, p->b1, 0x80);
    v[0].u = v[0].v = v[1].u = v[1].v = 0.0f;
    l.material = 0;
    l.v[0] = &v[0];
    l.v[1] = &v[1];
    g_zglDrawLines(f, &l, 1, 0, 0);
}

/* VA 0x00462050, 309 bytes, [V] direct, status draft */
void psxDrawPrim(const void *p)
{
    /* by GPU code (bit 1 = semi-transparent) */
    uint8_t code = ((const uint8_t *)p)[7];
    const char *what;
    int line;

    switch (code & 0xFD) {
    case 0x20: psxDrawPolyF3((const PsxPolyF3 *)p, code & 2); return;
    case 0x24: what = "DrawPrim: Prim not coded FT3"; line = 0x505; break;
    case 0x28: psxDrawPolyF4((const PsxPolyF4 *)p, code & 2); return;
    case 0x2C: psxDrawPolyFT4((const PsxPolyFT4 *)p, code & 2); return;
    case 0x30: what = "DrawPrim: Prim not coded G3"; line = 0x50B; break;
    case 0x34: what = "DrawPrim: Prim not coded GT3"; line = 0x513; break;
    case 0x38: psxDrawPolyG4((const PsxPolyG4 *)p, code & 2); return;
    case 0x3C: psxDrawPolyGT4((const PsxPolyGT4 *)p, code & 2); return;
    case 0x40: psxDrawLineF2((const PsxLineF2 *)p, code & 2); return;
    case 0x48: what = "DrawPrim: Prim not coded LINEF3"; line = 0x55F; break;
    case 0x4C: what = "DrawPrim: Prim not coded LINEF4"; line = 0x56F; break;
    case 0x50: psxDrawLineG2((const PsxLineG2 *)p, code & 2); return;
    case 0x58: what = "DrawPrim: Prim not coded LINEG3"; line = 0x567; break;
    case 0x5C: what = "DrawPrim: Prim not coded LINEG4"; line = 0x577; break;
    case 0x64: what = "DrawPrim: Prim not coded SPRT"; line = 0x540; break;
    case 0x74: what = "DrawPrim: Prim not coded SPRT8"; line = 0x530; break;
    case 0x7C: what = "DrawPrim: Prim not coded SPRT16"; line = 0x538; break;
    case 0xF8: psxDrawClip((const PsxClip *)p); return;
    default:   return;   /* 0xE1 rate packets and the rest: nothing */
    }
    NGIF_MISSING(NGIFPSX_SRC, line, what);
}

/* VA 0x00462350, 62 bytes, [I] between, status draft */
void psxDrawOTag(const uint32_t *ot)
{
    /* libgpu DrawOTag: walk the list, draw every packet with a non-zero length */
    while ((*ot & 0x0FFFFFFFu) != 0x0FFFFFFFu) {
        ot = (const uint32_t *)(uint32_t)(*ot & 0x0FFFFFFFu);
        if ((*ot & 0xF0000000u) != 0)
            psxDrawPrim(ot);
    }
}

/* VA 0x004623a0, 1144 bytes, [V] direct, status draft */
void psxDrawRect(uint32_t colourOrSlot, int x, int y, uint32_t flags, const int32_t *args)
{
    /* A rectangle into the overlay layer: flat (colour 0xSSBBGGRR, SS = semi rate + 1) or, with
     * flag 0x04, textured from image slot colourOrSlot. Size: 0x200 args[0..1]; 0x80 to the corner
     * args[4..5]; else the slot's size (0x400: args[8..9]). 0x100 scales by args[2..3], 0x08 / 0x10
     * centre x / y, 0x20 / 0x40 right / bottom aligned, 0x01 / 0x02 flip u / v, 0x400 / 0x4000 a
     * sub-rectangle of the slot (args[6..9] / the bytes of args[12]), 0x800 palette offset
     * args[10], 0x1000 OT entry args[11], 0x2000 semi-transparent. */
    const ZoverTexture *tex = NULL;
    int16_t clutOff = 0, x0, y0, x1, y1, t;
    int32_t w = 0, h = 0;
    uint32_t depth = 0, rate;
    uint32_t *p = g_psxPacket;

    if (flags & 4) {
        tex = &g_psxTextures[colourOrSlot];
        if (tex->texture == 0)
            return;
    }
    if (flags & 0x800)
        clutOff = (int16_t)args[10];
    if (flags & 0x1000)
        depth = (uint32_t)args[11];
    if (flags & 0x200) {
        w = args[0];
        h = args[1];
    } else if (flags & 0x80) {
        w = args[4] - x;
        h = args[5] - y;
    } else if (tex == NULL) {
        h = 0;
        zAssertFail("Cannot get size of object", NGIFPSX_SRC, 0x63F, 0);
    } else if (flags & 0x400) {
        w = args[8];
        h = args[9];
    } else {
        w = tex->w;
        h = tex->h;
    }
    if (flags & 0x100) {
        w *= args[2];
        h *= args[3];
    }
    if (flags & 8)
        x -= w >> 1;
    y0 = (int16_t)y;
    if (flags & 0x10)
        y0 = (int16_t)(y0 - (int16_t)(h >> 1));
    x0 = (int16_t)x;
    if (flags & 0x80) {
        x1 = (int16_t)args[4];
        y1 = (int16_t)args[5];
    } else {
        x1 = (int16_t)(x0 + (int16_t)w);
        y1 = (int16_t)(y0 + (int16_t)h);
    }
    if (flags & 0x20) {
        x0 = (int16_t)(x0 - (int16_t)w);
        x1 = (int16_t)(x1 - (int16_t)w);
    }
    if (flags & 0x40) {
        y0 = (int16_t)(y0 - (int16_t)h);
        y1 = (int16_t)(y1 - (int16_t)h);
    }
    if (p == NULL)
        return;

    if (tex != NULL) {
        PsxPolyFT4 *q = (PsxPolyFT4 *)p;
        ZoverPolyFT4 *f = &q->p;

        if (flags & 1) { t = x0; x0 = x1; x1 = t; }
        if (flags & 2) { t = y0; y0 = y1; y1 = t; }
        f->mode = (f->mode & ~0x7Fu) | 0x7F80;
        f->code = 0x2C;
        f->clut = (uint16_t)(tex->clut + clutOff);
        f->tpage = tex->tpage;
        f->texture = tex->texture;
        f->r = f->g = f->b = 0x80;
        if (flags & 0x2000) {
            ((uint8_t *)f)[0x20] = 1;    /* the exe's "blend" store lands in u2 (overwritten below) */
            f->code |= 2;
        }
        f->x0 = x0; f->y0 = y0;
        f->x1 = x1; f->y1 = y0;
        f->x2 = x0; f->y2 = y1;
        f->x3 = x1; f->y3 = y1;
        if (flags & 0x400) {
            f->u0 = (uint16_t)(tex->u + (int16_t)args[6]);
            f->v0 = (uint16_t)(tex->v + (int16_t)args[7]);
            f->u3 = (uint16_t)((int16_t)args[8] + f->u0);
            f->v3 = (uint16_t)((int16_t)args[9] + f->v0);
        } else if (flags & 0x4000) {
            const uint8_t *b = (const uint8_t *)&args[12];

            f->u0 = (uint16_t)(b[0] + tex->u);
            f->v0 = (uint16_t)(b[1] + tex->v);
            f->u3 = (uint16_t)(b[2] + tex->u);
            f->v3 = (uint16_t)(b[3] + tex->v);
        } else {
            f->u0 = tex->u;  f->v0 = tex->v;
            f->u3 = tex->u1; f->v3 = tex->v1;
        }
        f->u1 = f->u3; f->v1 = f->v0;
        f->u2 = f->u0; f->v2 = f->v3;
        addPrim(&g_psxOrderingTable[depth], q, 9);
        g_psxPacket = (uint32_t *)(q + 1);
        return;
    }

    {
        PsxPolyF4 *q = (PsxPolyF4 *)p;
        ZoverPolyF4 *f = &q->p;

        rate = colourOrSlot >> 24;
        f->code = 0x28;
        f->r = (uint8_t)colourOrSlot;
        f->g = (uint8_t)(colourOrSlot >> 8);
        f->b = (uint8_t)(colourOrSlot >> 16);
        f->mode = (f->mode & ~0x7Fu) | 0x7F80;
        if (rate != 0 || (flags & 0x2000)) {
            f->code |= 2;
            f->blend = (uint8_t)(rate - 1);
        }
        f->x0 = x0; f->y0 = y0;
        f->x1 = x1; f->y1 = y0;
        f->x2 = x0; f->y2 = y1;
        f->x3 = x1; f->y3 = y1;
        f->unk_18 = 0;
        addPrim(&g_psxOrderingTable[depth], q, 5);
        g_psxPacket = (uint32_t *)(q + 1);
        if (rate == 0 && !(flags & 0x2000))
            return;
        g_psxPacket = addRatePacket(g_psxPacket, &g_psxOrderingTable[depth], (int)rate - 1);
    }
}

/* VA 0x00462940, 1609 bytes, [V] direct, status draft */
void psxDrawSpriteList(int16_t x, int16_t y, const uint8_t *r)
{
    /* A list of drawing records at (x, y), ended by 0xFF:
     *   0x00 line (12 bytes): u8 type, u8 flags (bit 0 semi), u16 colour 0bbbbbgggggrrrrr-ish
     *        (PSX 15-bit), s16 x0, y0, x1, y1
     *   0x11 flat quad (20): flags, colour, then four corners (x, y)
     *   0x30 / 0x31 / 0x32 textured quad from image slot (12 / 20 / 16): s16 x, y, u16 slot;
     *        0x31 adds a sub-rectangle (u, v, w, h), 0x32 an integer scale (sx, sy); then u16
     *        colour 0x0SRGB (S = semi rate + 1, 4-bit components) and u16 palette offset. */
    uint32_t *p = g_psxPacket;
    float sx = 1.0f, sy = 1.0f;

    if (p == (uint32_t *)-1)
        return;
    while (r != NULL) {
        uint8_t type = r[0];

        if (type == 0x00) {
            PsxLineF2 *l = (PsxLineF2 *)p;
            uint16_t c = *(const uint16_t *)(r + 2);

            l->r = (uint8_t)((c >> 7) & 0xE0);
            l->g = (uint8_t)((c >> 2) & 0xE0);
            l->b = (uint8_t)((c & 0xFC) << 3);
            l->xy[0][0] = (int16_t)(*(const int16_t *)(r + 4) + x);
            l->xy[0][1] = (int16_t)(*(const int16_t *)(r + 6) + y);
            l->xy[1][0] = (int16_t)(*(const int16_t *)(r + 8) + x);
            l->xy[1][1] = (int16_t)(*(const int16_t *)(r + 10) + y);
            l->code = (r[1] & 1) ? 0x42 : 0x40;
            addPrim(g_psxOrderingTable, l, 3);
            p += 0x1F;
            g_psxPacket = p;
            r += 12;
        } else if (type == 0x11) {
            PsxPolyF4 *q = (PsxPolyF4 *)p;
            ZoverPolyF4 *f = &q->p;
            uint16_t c = *(const uint16_t *)(r + 2);

            f->mode = (f->mode & ~0x7Fu) | 0x7F80;
            f->r = (uint8_t)((c >> 7) & 0xE0);
            f->g = (uint8_t)((c >> 2) & 0xE0);
            f->b = (uint8_t)((c & 0xFC) << 3);
            f->code = (r[1] & 1) ? 0x2A : 0x28;
            f->x0 = (int16_t)(*(const int16_t *)(r + 4) + x);  f->y0 = (int16_t)(*(const int16_t *)(r + 6) + y);
            f->x1 = (int16_t)(*(const int16_t *)(r + 8) + x);  f->y1 = (int16_t)(*(const int16_t *)(r + 10) + y);
            f->x2 = (int16_t)(*(const int16_t *)(r + 16) + x); f->y2 = (int16_t)(*(const int16_t *)(r + 18) + y);
            f->x3 = (int16_t)(*(const int16_t *)(r + 12) + x); f->y3 = (int16_t)(*(const int16_t *)(r + 14) + y);
            addPrim(g_psxOrderingTable, q, 5);
            p += 0x21;
            g_psxPacket = p;
            r += 20;
        } else if (type >= 0x30 && type <= 0x32) {
            PsxPolyFT4 *q = (PsxPolyFT4 *)p;
            ZoverPolyFT4 *f = &q->p;
            int16_t ox = *(const int16_t *)(r + 2), oy = *(const int16_t *)(r + 4);
            const ZoverTexture *tex = &g_psxTextures[*(const uint16_t *)(r + 6)];
            uint16_t u0, v0, u1, v1, c, clut;
            int16_t x1, y1;

            if (tex->texture == 0)
                return;
            if (type == 0x32) {
                sx = (float)*(const uint16_t *)(r + 8);
                sy = (float)*(const uint16_t *)(r + 10);
            }
            zstringMemSet(q, 0, sizeof *q);
            f->code = 0x2C;
            f->mode = (f->mode & ~0x7Fu) | 0x7F80;
            if (type == 0x31) {
                x1 = (int16_t)((float)*(const uint16_t *)(r + 12) * sx + (float)(int16_t)(x + ox));
                y1 = (int16_t)((float)*(const uint16_t *)(r + 14) * sy + (float)(int16_t)(y + oy));
                u0 = (uint16_t)(tex->u + *(const int16_t *)(r + 8));
                v0 = (uint16_t)(tex->v + *(const int16_t *)(r + 10));
                u1 = (uint16_t)(*(const int16_t *)(r + 12) + u0);
                v1 = (uint16_t)(*(const int16_t *)(r + 14) + v0);
                c = *(const uint16_t *)(r + 16);
                clut = *(const uint16_t *)(r + 18);
            } else {
                x1 = (int16_t)((float)tex->w * sx + (float)(int16_t)(x + ox));
                y1 = (int16_t)((float)tex->h * sy + (float)(int16_t)(y + oy));
                u0 = tex->u; v0 = tex->v; u1 = tex->u1; v1 = tex->v1;
                c = *(const uint16_t *)(r + (type == 0x32 ? 12 : 8));
                clut = *(const uint16_t *)(r + (type == 0x32 ? 14 : 10));
            }
            f->clut = clut;
            f->r = (uint8_t)((c >> 4) & 0xF0);
            f->g = (uint8_t)(c & 0xF0);
            f->b = (uint8_t)(c << 4);
            if (((c >> 12) & 7) == 0) {
                f->code = 0x2C;
                f->tpage = tex->tpage;
            } else {
                int rate = (int)((c >> 12) & 7) - 1;

                if (rate > 2)
                    rate = 3;
                f->code = 0x2E;
                f->tpage = (uint16_t)(((rate & 3) << 5) | (tex->tpage & 0xFF9F));
                f->blend = (uint8_t)(((c >> 12) & 7) - 1);
            }
            f->texture = tex->texture;
            f->x0 = (int16_t)(x + ox); f->y0 = (int16_t)(y + oy); f->u0 = u0; f->v0 = v0;
            f->x1 = x1;                f->y1 = (int16_t)(y + oy); f->u1 = u1; f->v1 = v0;
            f->x2 = (int16_t)(x + ox); f->y2 = y1;                f->u2 = u0; f->v2 = v1;
            f->x3 = x1;                f->y3 = y1;                f->u3 = u1; f->v3 = v1;
            addPrim(g_psxOrderingTable, q, 9);
            p += 0x27;
            g_psxPacket = p;
            r += type == 0x30 ? 12 : (type == 0x32 ? 16 : 20);
        } else if (type == 0xFF) {
            return;
        } else {
            zAssertFail("Can not get size of object", NGIFPSX_SRC, 0x7BD, 0);
        }
    }
}

/* VA 0x00463130, 1186 bytes, [V] direct, status draft */
void psxDrawPoly3D(uint32_t mode, uint8_t n, const PsxVert3D *v, uint16_t slot)
{
    /* A world-space triangle or quad (n = 3 / 4; fan order v0 v1 v2 v3) in the colour of v0, into
     * the overlay OT: physics / game debug views. mode bits 0-1 = semi rate + 1, bit 3 must be set
     * (screen-space polygons are not coded). With an image slot: a textured quad (whole image). */
    uint32_t *p = g_psxPacket;
    int rate;

    if (n > 4 || p == NULL)
        return;
    if (!(mode & 8))
        zAssertFail("Uncoded for polygons in screen space", NGIFPSX_SRC, 0x838, 0);
    rate = (int)(mode & 3);
    if (slot != 0) {
        PsxPolyFT4 *q = (PsxPolyFT4 *)p;
        ZoverPolyFT4 *f = &q->p;
        const ZoverTexture *tex;
        int i;

        if (n != 4)
            zAssertFail("Not coded for textured tris", NGIFPSX_SRC, 0x853, 0);
        tex = &g_psxTextures[slot];
        if (tex->texture == 0)
            return;
        f->code = 0x2C;
        f->mode = (f->mode & ~0x7Fu) | 0x7F80;
        f->clut = tex->clut;
        f->r = v[0].colour.red; f->g = v[0].colour.green; f->b = v[0].colour.blue;
        if (rate == 0) {
            f->tpage = tex->tpage;
        } else {
            int r2 = rate - 1 > 2 ? 3 : rate - 1;

            f->tpage = (uint16_t)(((r2 & 3) << 5) | (tex->tpage & 0xFF9F));
            f->code |= 2;
        }
        f->texture = tex->texture;
        f->blend = (uint8_t)(rate - 1);
        f->mode |= 0x30;
        for (i = 0; i < 3; i++) {
            q->pos[0][i] = (&v[0].x)[i];
            q->pos[1][i] = (&v[1].x)[i];
            q->pos[3][i] = (&v[2].x)[i];
            q->pos[2][i] = (&v[3].x)[i];
        }
        f->u0 = tex->u;
        f->v0 = tex->v;
        f->u3 = (uint16_t)(int)texSize(tex->texture, 5);
        f->v3 = (uint16_t)(int)texSize(tex->texture, 6);
        f->u1 = f->u3; f->v1 = f->v0;
        f->u2 = f->u0; f->v2 = f->v3;
        addPrim(g_psxOrderingTable, q, 9);
        g_psxPacket = (uint32_t *)(q + 1);
        return;
    }
    if (n == 4) {
        PsxPolyF4 *q = (PsxPolyF4 *)p;
        ZoverPolyF4 *f = &q->p;
        int i;

        f->code = 0x28;
        f->r = v[0].colour.red; f->g = v[0].colour.green; f->b = v[0].colour.blue;
        if (rate != 0)
            f->code |= 2;
        f->mode = (f->mode & ~0x7Fu) | 0x7FB0;
        f->blend = (uint8_t)(rate - 1);
        for (i = 0; i < 3; i++) {
            q->pos[0][i] = (&v[0].x)[i];
            q->pos[1][i] = (&v[1].x)[i];
            q->pos[3][i] = (&v[2].x)[i];
            q->pos[2][i] = (&v[3].x)[i];
        }
        addPrim(g_psxOrderingTable, q, 5);
        g_psxPacket = (uint32_t *)(q + 1);
    } else {
        PsxPolyF3 *q = (PsxPolyF3 *)p;
        int i;

        q->code = 0x20;
        q->mode = (q->mode & ~0x7Fu) | 0x7F80;
        q->r = v[0].colour.red; q->g = v[0].colour.green; q->b = v[0].colour.blue;
        if (rate != 0)
            q->code |= 2;
        q->blend = (uint8_t)(rate - 1);
        q->mode |= 0x30;
        for (i = 0; i < 3; i++) {
            q->pos[0][i] = (&v[0].x)[i];
            q->pos[1][i] = (&v[1].x)[i];
            q->pos[2][i] = (&v[2].x)[i];
        }
        addPrim(g_psxOrderingTable, q, 4);
        g_psxPacket = p + 0x20;
    }
    if (rate != 0)
        g_psxPacket = addRatePacket(g_psxPacket, g_psxOrderingTable, rate - 1);
}

/* VA 0x00463710, 177 bytes, [I] between, status draft */
void psxDrawLine3D(uint8_t mode, const PsxVert3D *a, const PsxVert3D *b)
{
    /* a world-space line, a's colour to b's (mode bits 0-1 = semi rate + 1) */
    PsxLineG2 *l = (PsxLineG2 *)g_psxPacket;

    *(RwRGBA *)&l->r0 = a->colour;
    *(RwRGBA *)&l->r1 = b->colour;
    l->code = 0x50;
    l->blend = (uint8_t)((mode & 3) - 1);
    l->mode |= 0x30;
    l->pos[0][0] = a->x; l->pos[0][1] = a->y; l->pos[0][2] = a->z;
    l->pos[1][0] = b->x; l->pos[1][1] = b->y; l->pos[1][2] = b->z;
    if (mode & 3)
        l->code = 0x52;
    addPrim(g_psxOrderingTable, l, 4);
    g_psxPacket += 0x20;
}

/* ------------------------------------------------------------------ the two layers */

/* VA 0x00463800, 1 bytes, [I] between, status draft */
void psxNop(void)
{
}

/* VA 0x00463820, 40 bytes, [I] between, status draft */
void psxBeginScene(void)
{
    /* every game frame (ngappFrame): empty OT, packets from the start, draw requested */
    psxClearOTagR(g_psxOrderingTable, 0x400);
    s_sceneDirty = 1;
    g_psxPacket = (uint32_t *)s_packetBuffer;
}

/* VA 0x00463860, 49 bytes, [I] between, status draft */
int psxSceneDraw(void *arg)
{
    /* frame callback (priority 2) of psxEnableDraw */
    (void)arg;
    psxGetScreenSize();
    if (s_sceneDirty) {
        psxDrawOTag(&g_psxOrderingTable[0x3FF]);
        s_sceneDirty = 0;
    }
    return 1;
}

/* VA 0x004638b0, 34 bytes, [I] between, status draft */
void psxGetScreenSize(void)
{
    g_zglGet(10, &g_psxScreenW, 0);
    g_zglGet(11, &g_psxScreenH, 0);
}

/* VA 0x004638f0, 35 bytes, [I] between, status draft */
void psxEnableDraw(void)
{
    if (s_sceneHandle == 0)
        s_sceneHandle = g_zglAddFrameCallback(2, psxSceneDraw, NULL, 1);
}

/* VA 0x00463930, 30 bytes, [I] between, status draft */
void psxDisableDraw(void)
{
    if (s_sceneHandle != 0)
        g_zglRemoveFrameCallback(s_sceneHandle);
    s_sceneHandle = 0;
}

/* VA 0x00463960, 6 bytes, [I] between, status draft */
uint32_t *psx2DGetDepth(void)
{
    return s_ot2D;      /* frontend.c saves / restores the menu draw order */
}

/* VA 0x00463980, 10 bytes, [I] between, status draft */
void psx2DSetDepth(uint32_t *ot)
{
    s_ot2D = ot;
}

/* VA 0x004639a0, 49 bytes, [I] between, status draft */
int psx2DDraw(void *arg)
{
    /* frame callback (priority 2) of psx2DInit */
    (void)arg;
    psxGetScreenSize();
    if (s_2DDirty) {
        psxDrawOTag(&g_psxOrderingTable[0x3FF]);
        s_2DDirty = 0;
    }
    return 1;
}

/* VA 0x004639f0, 66 bytes, [I] between, status draft */
int psx2DInit(void)
{
    if (s_2DBuffer == NULL)
        s_2DBuffer = s_2DBufferStatic;
    s_2DPacket = NULL;
    if (s_2DHandle == 0)
        s_2DHandle = g_zglAddFrameCallback(2, psx2DDraw, NULL, 1);
    return 0;
}

/* VA 0x00463a60, 51 bytes, [I] between, status draft */
void psx2DShutdown(void)
{
    if (s_2DBuffer != NULL)
        s_2DBuffer = NULL;
    s_2DPacket = NULL;
    if (s_2DHandle != 0)
        g_zglRemoveFrameCallback(s_2DHandle);
    s_2DHandle = 0;
}

/* VA 0x00463ab0, 52 bytes, [I] between, status draft */
void psx2DBeginFrame(void)
{
    s_2DPacket = (uint32_t *)s_2DBuffer;
    s_ot2D = &g_psxOrderingTable[0x3FF];
    psxClearOTagR(g_psxOrderingTable, 0x400);
    g_psxPacket = (uint32_t *)s_packetBuffer;
}

/* VA 0x00463b00, 11 bytes, [I] between, status draft */
void psx2DRequestDraw(void)
{
    s_2DDirty = 1;
}

/* VA 0x00463b20, 224 bytes, [I] between, status draft */
void psx2DDrawPicture(const ZprimImage *img)
{
    /* a full-screen picture: the image's first frame on 512 x 512 (x 0..511 wide, y -32..480) */
    PsxPolyFT4 *q = (PsxPolyFT4 *)s_2DPacket;
    ZoverPolyFT4 *f = &q->p;

    f->r = f->g = f->b = 0xFF;
    f->code = 0x2C;
    f->mode = (f->mode & ~0x7Du) | 0x7F82;
    f->x0 = 0;      f->y0 = -32;  f->u0 = 0;     f->v0 = 0;
    f->x1 = 0x200;  f->y1 = -32;  f->u1 = 0x200; f->v1 = 0;
    f->x2 = 0;      f->y2 = 0x1E0; f->u2 = 0;    f->v2 = 0x200;
    f->x3 = 0x200;  f->y3 = 0x1E0; f->u3 = 0x200; f->v3 = 0x200;
    f->texture = (uint32_t)img->frame[0].material;
    add2D(q, 9, sizeof *q);
}

/* VA 0x00463c40, 20 bytes, [I] between, status draft */
void psx2DSetTint(uint32_t rgb, uint32_t level)
{
    /* zprim sprites / rectangles: colour x level / 256 instead of white (0, 0 = off) */
    s_tint = rgb;
    s_tintLevel = level;
}

/* VA 0x00463c70, 765 bytes, [V] direct, status draft */
void psx2DDrawGlyph(ZprimFont *font, const ZprimGlyph *g, int mode, uint32_t arg1, uint32_t arg2,
                    int x, int y, int arg14, uint32_t palette)
{
    /* One character of a zprim font into the menu layer. Each of the font's 16 palettes gets its
     * own material the first time it is used (the font's 8-bit bitmap with that 16-colour
     * palette, zprimMakeMaterial), cached in font->materials[palette]. */
    PsxPolyFT4 *q = (PsxPolyFT4 *)s_2DPacket;
    ZoverPolyFT4 *f = &q->p;
    int16_t sy = (int16_t)(y + 0x68);
    uint32_t level;

    (void)arg1;
    (void)arg2;
    if (g->w == 0 && g->h == 0)
        return;
    f->mode = (f->mode & ~0x7Du) | 0x7F82;
    f->code = 0x2C;
    if (s_tint == 0) {
        f->r = f->g = f->b = 0xFF;
    } else {
        f->r = (uint8_t)s_tint;
        f->g = (uint8_t)(s_tint >> 8);
        f->b = (uint8_t)(s_tint >> 16);
    }
    f->x0 = (int16_t)x;            f->y0 = sy;            f->u0 = g->x;        f->v0 = g->y;
    f->x1 = (int16_t)(x + g->w);   f->y1 = sy;            f->u1 = (uint16_t)(g->w + g->x); f->v1 = g->y;
    f->x2 = (int16_t)x;            f->y2 = (int16_t)(sy + g->h); f->u2 = g->x; f->v2 = (uint16_t)(g->y + g->h);
    f->x3 = (int16_t)(x + g->w);   f->y3 = (int16_t)(sy + g->h); f->u3 = (uint16_t)(g->w + g->x);
    f->v3 = (uint16_t)(g->y + g->h);
    if (font->materials == NULL) {
        font->materials = (void **)zmemFreeListAlloc(g_ngappFreeList);
        zstringMemSet(font->materials, 0, 0x80);
    }
    if (font->materials[palette] == NULL) {
        uint8_t pal[0x1000];
        const ZprimPalette *src = (const ZprimPalette *)((const uint8_t *)font + font->palette);
        uint32_t bytes = (uint32_t)src->count * 2 + 4;

        if (bytes > 0xFFF)
            zAssertFail("Buffer overflow in psxprimDrawFontChar()", NGIFPSX_SRC, 0xAEA, 0);
        if (palette > 0xF)
            zAssertFail("Invalid palette index.", NGIFPSX_SRC, 0xAEF, 0);
        zstringMemCopy(pal, src, bytes);
        zstringMemCopy(pal + 4, pal + 4 + palette * 0x20, 0x20);   /* the chosen 16 colours first */
        font->materials[palette] = (void *)(int32_t)zprimMakeMaterial(font->width, font->height,
                                                                       font->bpp,
                                                                       (const uint8_t *)font + font->pixels, (ZprimPalette *)pal);
    }
    f->texture = (uint32_t)(uint32_t)font->materials[palette];
    level = s_tintLevel != 0 ? s_tintLevel : 0xFF;
    f->mode = ((level & 0xFF) << 7) | (f->mode & 0xFFFF807Fu) | PSXM_ALPHA | PSXM_DEPTH;
    if (mode == 1) {
        f->blend = 0;
        f->code |= 2;
    } else {
        f->code &= (uint8_t)~2;
    }
    f->tpage = 0;
    (void)arg14;
    add2D(q, 9, sizeof *q);
}

/* VA 0x00464030, 356 bytes, [I] between, status draft */
void psx2DSetClip(int x0, int y0, int x1, int y1)
{
    /* zprimSetClip: a clip packet for the menu layer; the full 640 x 480 screen means none */
    PsxClip *c = (PsxClip *)s_2DPacket;
    int16_t r[4];
    float kx, ky;

    if (abs((x1 & 0xFFFF) - (x0 & 0xFFFF)) == 639 && abs((y1 & 0xFFFF) - (y0 & 0xFFFF)) == 479) {
        x0 = y0 = 0;
        x1 = y1 = 0;
    } else {
        y0 += 16;
        y1 += 16;
    }
    if (c == NULL)
        return;
    kx = g_psxScreenW * (1.0f / 640.0f);
    ky = g_psxScreenH * (1.0f / 480.0f);
    r[0] = (int16_t)((float)(x0 & 0xFFFF) * kx);
    r[1] = (int16_t)((float)(y0 & 0xFFFF) * ky);
    r[2] = (int16_t)((float)((x1 & 0xFFFF) - (x0 & 0xFFFF)) * kx);
    r[3] = (int16_t)((float)((y1 & 0xFFFF) - (y0 & 0xFFFF)) * ky);
    psxSetClipPrim(c, r);
    add2D(c, 5, sizeof *c);
}

/* VA 0x00464200, 1 bytes, [I] between, status draft */
void psxNop2(void)
{
}

/* VA 0x00464220, 397 bytes, [I] between, status draft */
void psx2DDrawSprite(const ZprimImage *img, uint32_t frame, const void *palette, int mode, int zero,
                     uint32_t arg, int abr, int x, int y, int w, int h, int bpp, uint32_t arg30)
{
    /* one frame of a zprim sprite (its material) at (x, y + 104), w x h */
    PsxPolyFT4 *q = (PsxPolyFT4 *)s_2DPacket;
    ZoverPolyFT4 *f = &q->p;
    int16_t sy = (int16_t)(y + 0x68);
    uint32_t mode0 = (f->mode & ~0x7Du) | 0x7F82, level;

    (void)palette; (void)zero; (void)arg; (void)bpp; (void)arg30;
    f->code = 0x2C;
    f->mode = mode0;
    if (s_tint == 0) {
        f->r = f->g = f->b = 0xFF;
    } else {
        f->r = (uint8_t)(((s_tint & 0xFF) * s_tintLevel) >> 8);
        f->g = (uint8_t)((((s_tint >> 8) & 0xFF) * s_tintLevel) >> 8);
        f->b = (uint8_t)((((s_tint >> 16) & 0xFF) * s_tintLevel) >> 8);
    }
    f->x0 = (int16_t)x;       f->y0 = sy;                  f->u0 = 0;           f->v0 = 0;
    f->x1 = (int16_t)(x + w); f->y1 = sy;                  f->u1 = (uint16_t)w; f->v1 = 0;
    f->x2 = (int16_t)x;       f->y2 = (int16_t)(sy + h);   f->u2 = 0;           f->v2 = (uint16_t)h;
    f->x3 = (int16_t)(x + w); f->y3 = (int16_t)(sy + h);   f->u3 = (uint16_t)w; f->v3 = (uint16_t)h;
    f->texture = (uint32_t)img->frame[frame & 0xFFFF].material;
    level = s_tintLevel != 0 ? s_tintLevel : 0xFF;
    f->mode = ((level & 0xFF) << 7) | (mode0 & 0xFFFF807Fu) | PSXM_ALPHA;
    if (mode == 1) {
        f->code = 0x2E;
        f->tpage = (uint16_t)(abr << 5);
    } else {
        f->code = 0x2C;
        f->tpage = 0;
    }
    add2D(q, 9, sizeof *q);
}

/* VA 0x00464420, 329 bytes, [I] between, status draft */
void psx2DFillRect(uint32_t colour, int mode, int x0, int y0, int x1, int y1)
{
    PsxPolyF4 *q = (PsxPolyF4 *)s_2DPacket;
    ZoverPolyF4 *f = &q->p;
    uint32_t mode0 = (f->mode & ~0x7Du) | 0x7F82, level;

    f->code = 0x28;
    f->mode = mode0;
    if (s_tint == 0) {
        f->r = (uint8_t)colour;
        f->g = (uint8_t)(colour >> 8);
        f->b = (uint8_t)(colour >> 16);
    } else {
        f->r = (uint8_t)(((s_tint & 0xFF) * s_tintLevel) >> 8);
        f->g = (uint8_t)((((s_tint >> 8) & 0xFF) * s_tintLevel) >> 8);
        f->b = (uint8_t)((((s_tint >> 16) & 0xFF) * s_tintLevel) >> 8);
    }
    f->x0 = (int16_t)x0; f->y0 = (int16_t)(y0 + 0x68);
    f->x1 = (int16_t)x1; f->y1 = (int16_t)(y0 + 0x68);
    f->x2 = (int16_t)x0; f->y2 = (int16_t)(y1 + 0x68);
    f->x3 = (int16_t)x1; f->y3 = (int16_t)(y1 + 0x68);
    f->unk_18 = 0;
    level = s_tintLevel != 0 ? s_tintLevel : 0xFF;
    f->mode = ((level & 0xFF) << 7) | (mode0 & 0xFFFF807Fu) | PSXM_ALPHA;
    f->code = mode == 1 ? 0x2A : 0x28;
    add2D(q, 5, sizeof *q);
}

/* VA 0x004645d0, 172 bytes, [I] between, status draft */
void psx2DDrawLine(uint32_t colour, int mode, int x0, int y0, int x1, int y1)
{
    PsxLineF2 *l = (PsxLineF2 *)s_2DPacket;

    (void)mode;
    l->r = (uint8_t)colour;
    l->g = (uint8_t)(colour >> 8);
    l->b = (uint8_t)(colour >> 16);
    l->xy[0][0] = (int16_t)x0; l->xy[0][1] = (int16_t)(y0 + 0x68);
    l->xy[1][0] = (int16_t)x1; l->xy[1][1] = (int16_t)(y1 + 0x68);
    l->code = 0x40;
    l->mode = (l->mode & ~0x24u) | PSXM_WIDE512;
    add2D(l, 3, 0x7C);
}

/* VA 0x004646b0, 6 bytes, [I] between, status draft */
int psxUnusedTrue1(void)
{
    return 1;   /* unreferenced */
}

/* VA 0x004646d0, 6 bytes, [I] between, status draft */
int psxUnusedTrue2(void)
{
    return 1;   /* unreferenced */
}

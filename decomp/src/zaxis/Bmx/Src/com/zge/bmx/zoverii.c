/*
 * zaxis/Bmx/Src/com/zge/bmx/zoverii.c
 * Original: C:\zaxis\Bmx\src\com\zge\bmx\zoverii.c
 * Evidence: [V] path string 0x0055B9AC, referenced by the asserts of 13 functions ("dirty list
 *           overflow", "free list overflow", "Null font in overlay", "Overflow"); [I] the other five
 *           (zoveriiSetLayer, zoveriiGetPool, zoveriiAlloc, zoveriiFree, zoveriiDrawRect) lie between
 *           them and touch only this file's globals.
 * 19 functions, VA 0x004578f0-0x0045a130 (zoveriiInit 0x004578F0 added by DM-058).
 * DM-054 (draft). Non-matching C (AD-8). Types: zoverii.h. Notes: docs/re/hud.md.
 *
 * The "overlay II" primitive layer: rectangles, gradients, sprites and text as PSX GPU primitives in
 * four double-buffered pools (see zoverii.h). Only overlay.c and overlymp.c use it; the menus
 * (frontend.c) draw through zprim.c instead.
 */

#include "decomp.h"

#include "zoverii.h"

#define ZOVERII_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\bmx\\zoverii.c"

/* ------------------------------------------------------------------ other modules */
/* NgIFpsx.c (not decompiled yet): immediate-mode rectangle and glyph draws */
void psxDrawRect(uint32_t bgr, int x, int y, int mode, const void *size);  /* 0x004623A0 */
void psxDrawSpriteList(int16_t x, int y, const ZoverSprite *sprite);               /* 0x00462940 */
extern ZoverTexture g_psxTextures[];        /* 0x0068C3E0 NgIFzres image slots [I] */
extern uint32_t    *g_psxOrderingTable;     /* 0x0055C698 -> the OT entry the overlays go into [I] */

/* ------------------------------------------------------------------ globals (this file) */
ZoverFont  *g_zoverFonts[ZOVER_MAX_FONTS];  /* 0x00574E50 fonts loaded by zoveriiLoadFont */
ZoverPrim   g_zoverGlyphs1[2][400];         /* 0x00574E70 menu layer, glyphs */
ZoverPrim   g_zoverShapes1[2][28];          /* 0x00594270 menu layer, shapes */
uint32_t    g_zoverRectSize[2];             /* 0x00596570 width, height for zoveriiDrawRect */
uint16_t    g_zoverFreeList[ZOVER_MAX_PRIMS];   /* 0x005965A4 handles to clear in the other buffer */
uint32_t    g_zoverModePrim[2][2];          /* 0x00596A20 per buffer: tag, draw mode 0xE1000200 */
uint8_t     g_zoverNumFonts;                /* 0x00596A30 */
ZoverPrim   g_zoverGlyphs0[2][128];         /* 0x00596A38 HUD layer, glyphs */
ZoverPrim   g_zoverShapes0[2][16];          /* 0x005A0A38 HUD layer, shapes */
uint16_t    g_zoverDirtyList[ZOVER_MAX_PRIMS];  /* 0x005A1E38 handles to copy into the other buffer */
int32_t     g_zoverLock;                    /* 0x005A22B0 >0 while a call edits the pools */
uint32_t    g_zoverDirtyCount;              /* 0x005A22B4 */
uint32_t    g_zoverFreeCount;               /* 0x005A22B8 */
uint8_t     g_zoverBuffer;                  /* 0x005A22BC buffer being edited (0/1) */
uint8_t     g_zoverLayer;                   /* 0x005A22BD layer drawn by zoveriiFlush */
ZoverSprite g_zoverGlyphSprite;             /* 0x0055B960 zoveriiDrawText's descriptor */

/* ------------------------------------------------------------------ helpers (inlined in the exe) */

/* PSX addPrims: put the chain first..last at the head of the ordering-table entry */
static void zoverAddChain(uint32_t *ot, uint32_t *first, uint32_t *last)
{
    *last = (*last & ~ZOVER_TAG_ADDR) | (*ot & ZOVER_TAG_ADDR);
    *ot = (*ot & ~ZOVER_TAG_ADDR) | ((uint32_t)first & ZOVER_TAG_ADDR);
}

static void zoverSetLen(ZoverPrim *p, uint32_t words)
{
    *ZOVER_TAGP(p) = (*ZOVER_TAGP(p) & ZOVER_TAG_ADDR) | ZOVER_TAG_LEN(words);
}

static ZoverPrim *zoverPrim(uint16_t handle, uint8_t buffer, uint32_t *count)
{
    ZoverPrim *base;
    uint32_t pool;

    zoveriiGetPool((uint8_t)ZOVER_LAYER(handle), (uint8_t)ZOVER_CLASS(handle), buffer, &base, count, &pool);
    return &base[ZOVER_INDEX(handle)];
}

/* remove a handle from the free list (the exe's loop: it steps past the element it just shifted in) */
static void zoverUnfree(uint16_t handle)
{
    uint32_t i;

    for (i = 0; i < g_zoverFreeCount; i++)
        if (g_zoverFreeList[i] == handle && i < --g_zoverFreeCount)
            zstringMemMove(&g_zoverFreeList[i], &g_zoverFreeList[i + 1], (g_zoverFreeCount - i) * 2);
}

static void zoverUndirty(uint16_t handle)
{
    uint32_t i;

    for (i = 0; i < g_zoverDirtyCount; i++)
        if (g_zoverDirtyList[i] == handle && i < --g_zoverDirtyCount)
            zstringMemMove(&g_zoverDirtyList[i], &g_zoverDirtyList[i + 1], (g_zoverDirtyCount - i) * 2);
}

/* queue a changed primitive for the other buffer; line = the assert's __LINE__ at that call site */
static void zoverPushDirty(uint16_t handle, int line)
{
    g_zoverDirtyList[g_zoverDirtyCount++] = handle;
    if (g_zoverDirtyCount > ZOVER_MAX_PRIMS - 1)
        zAssertFail("dirty list overflow", ZOVERII_SRC, line, NULL);
}

static void zoverMarkDirty(ZoverPrim *p, uint16_t handle, int line)
{
    if (!(p->flags & ZOVERF_DIRTY)) {
        zoverPushDirty(handle, line);
        p->flags |= ZOVERF_DIRTY;
    }
}

/* the texture page of a textured primitive: blend modes 1..3 go into bits 5-6 (4.. clamp to 3) */
static uint16_t zoverTpage(const ZoverTexture *tex, uint32_t colour)
{
    uint32_t rate;

    if (!(colour & 0x07000000))
        return tex->tpage;
    rate = ((colour >> 24) & 7) - 1;
    if (rate > 2)
        rate = 3;
    return (uint16_t)((rate & 3) << 5 | (tex->tpage & 0xFF9F));
}

static void zoverSetRGB(uint8_t *rgb, uint32_t colour)
{
    rgb[0] = (uint8_t)(colour >> 16);
    rgb[1] = (uint8_t)(colour >> 8);
    rgb[2] = (uint8_t)colour;
}

/* ------------------------------------------------------------------ functions */

/* one pool: every primitive's tag links to the next one (the last keeps its link), zero length
 * (hidden), no successor glyph, free */
static void zoverInitPool(ZoverPrim *pool, int count)
{
    int i;

    for (i = 0; i < count - 1; i++)
        *ZOVER_TAGP(&pool[i]) = (*ZOVER_TAGP(&pool[i]) & ~ZOVER_TAG_ADDR) |
                                ((uint32_t)&pool[i + 1] & ZOVER_TAG_ADDR);
    for (i = 0; i < count; i++) {
        *ZOVER_TAGP(&pool[i]) &= ZOVER_TAG_ADDR;
        pool[i].next = 0xFFFF;
        pool[i].type = 0;
    }
}

/* Empty the four pools of both buffers; no fonts, buffer 0, unlocked. action.c actionLoad calls it
 * after loading the overlay fonts' assets. (DM-058: was the last function of the bmxsound / zoverii
 * gap; it touches only this file's globals.) */
/* VA 0x004578f0, 409 bytes, [I] row, status draft */
int zoveriiInit(void)
{
    int b;

    g_zoverNumFonts = 0;
    g_zoverBuffer = 0;
    g_zoverLock = 0;
    for (b = 0; b < 2; b++) {
        zoverInitPool(g_zoverShapes0[b], 16);
        zoverInitPool(g_zoverGlyphs0[b], 128);
        zoverInitPool(g_zoverShapes1[b], 28);
        zoverInitPool(g_zoverGlyphs1[b], 400);
    }
    return 0;
}

/* VA 0x00457b00, 870 bytes, [V] direct, status draft */
int zoveriiFlush(void)
{
    /* Once per frame (overlayDraw): put this buffer's pools of the active layer into the ordering
     * table (glyphs first, then shapes, then the draw-mode word on top), swap buffers, then bring
     * the new edit buffer up to date: clear the slots freed last frame, copy the changed ones. */
    uint8_t buf;
    uint16_t h;
    ZoverPrim *cur, *old;
    uint32_t count, pool;
    ZoverPrim *base, *obase;

    if (g_zoverLock != 0)
        return 0;
    buf = g_zoverBuffer;
    if (g_zoverLayer == ZOVER_LAYER_HUD) {
        zoverAddChain(g_psxOrderingTable, ZOVER_TAGP(&g_zoverGlyphs0[buf][0]), ZOVER_TAGP(&g_zoverGlyphs0[buf][127]));
        zoverAddChain(g_psxOrderingTable, ZOVER_TAGP(&g_zoverShapes0[buf][0]), ZOVER_TAGP(&g_zoverShapes0[buf][15]));
    } else if (g_zoverLayer == ZOVER_LAYER_MENU) {
        zoverAddChain(g_psxOrderingTable, ZOVER_TAGP(&g_zoverGlyphs1[buf][0]), ZOVER_TAGP(&g_zoverGlyphs1[buf][399]));
        zoverAddChain(g_psxOrderingTable, ZOVER_TAGP(&g_zoverShapes1[buf][0]), ZOVER_TAGP(&g_zoverShapes1[buf][27]));
    }
    g_zoverModePrim[buf][0] = (g_zoverModePrim[buf][0] & ZOVER_TAG_ADDR) | ZOVER_TAG_LEN(1);
    g_zoverModePrim[buf][1] = 0xE1000200;
    zoverAddChain(g_psxOrderingTable, g_zoverModePrim[buf], g_zoverModePrim[buf]);
    g_zoverBuffer ^= 1;

    if (g_zoverFreeCount > ZOVER_MAX_PRIMS - 1)
        zAssertFail("free list overflow", ZOVERII_SRC, 0x10C, NULL);
    while (g_zoverFreeCount != 0) {
        h = g_zoverFreeList[--g_zoverFreeCount];
        cur = zoverPrim(h, g_zoverBuffer, &count);
        cur->next = ZOVER_NO_HANDLE;
        cur->type = ZOVER_FREE;
        *ZOVER_TAGP(cur) &= ZOVER_TAG_ADDR;
    }

    if (g_zoverDirtyCount > ZOVER_MAX_PRIMS - 1)
        zAssertFail("dirty list overflow", ZOVERII_SRC, 0x117, NULL);
    while (g_zoverDirtyCount != 0) {
        h = g_zoverDirtyList[--g_zoverDirtyCount];
        zoveriiGetPool((uint8_t)ZOVER_LAYER(h), (uint8_t)ZOVER_CLASS(h), g_zoverBuffer, &base, &count, &pool);
        zoveriiGetPool((uint8_t)ZOVER_LAYER(h), (uint8_t)ZOVER_CLASS(h), (uint8_t)(g_zoverBuffer == 0),
                       &obase, &count, &pool);
        cur = &base[ZOVER_INDEX(h)];
        old = &obase[ZOVER_INDEX(h)];
        old->flags &= ~ZOVERF_DIRTY;
        zstringMemCopy(cur, old, sizeof(ZoverPrim));
        /* the copy carried the other buffer's link: point it back into this buffer's chain */
        if (ZOVER_INDEX(h) != count - 1)
            *ZOVER_TAGP(cur) = (*ZOVER_TAGP(cur) & ~ZOVER_TAG_ADDR) |
                               ((uint32_t)&base[ZOVER_INDEX(h) + 1] & ZOVER_TAG_ADDR);
    }
    return 0;
}

/* VA 0x00457f50, 12 bytes, [I] between, status draft */
int zoveriiSetLayer(uint8_t layer)
{
    /* overlaySetScreen: 1 while a menu screen is open, 0 for the HUD */
    g_zoverLayer = layer;
    return 0;
}

/* VA 0x00457f70, 276 bytes, [I] between, status draft */
int zoveriiGetPool(uint8_t layer, uint8_t cls, uint8_t buffer, ZoverPrim **base, uint32_t *count,
                   uint32_t *pool)
{
    if (layer == ZOVER_LAYER_HUD) {
        if (cls == ZOVER_CLASS_SHAPE) {
            *base = g_zoverShapes0[buffer];
            *count = 16;
            *pool = 0;
        } else if (cls == ZOVER_CLASS_GLYPH) {
            *base = g_zoverGlyphs0[buffer];
            *count = 128;
            *pool = 1;
        }
    } else if (layer == ZOVER_LAYER_MENU) {
        if (cls == ZOVER_CLASS_SHAPE) {
            *base = g_zoverShapes1[buffer];
            *count = 28;
            *pool = 2;
        } else if (cls == ZOVER_CLASS_GLYPH) {
            *base = g_zoverGlyphs1[buffer];
            *count = 400;
            *pool = 3;
        }
    }
    return 0;
}

/* VA 0x004580e0, 130 bytes, [I] between, status draft */
uint16_t zoveriiAlloc(uint8_t layer, uint8_t cls)
{
    /* first free slot of the pool in the edit buffer, or 0xFFFF */
    ZoverPrim *base;
    uint32_t count, pool, i;

    zoveriiGetPool(layer, cls, g_zoverBuffer, &base, &count, &pool);
    for (i = 0; i < count; i++)
        if (base[i].type == ZOVER_FREE)
            return ZOVER_HANDLE(i, cls & 3, layer & 1);
    return ZOVER_NO_HANDLE;
}

/* VA 0x004581a0, 469 bytes, [I] between, status draft */
void zoveriiFree(uint16_t handle)
{
    /* Free a primitive (a whole chain for text). The slot is cleared here and queued so that the
     * flush clears it in the other buffer too; a pending copy is dropped. */
    ZoverPrim *p;
    uint32_t count;
    uint16_t next;

    if (handle == ZOVER_NO_HANDLE)
        return;
    g_zoverLock++;
    p = zoverPrim(handle, g_zoverBuffer, &count);
    if (ZOVER_INDEX(handle) < count && p->type != ZOVER_FREE) {
        if (p->type < ZOVER_GLYPH) {
            p->type = ZOVER_FREE;
            *ZOVER_TAGP(p) &= ZOVER_TAG_ADDR;
            g_zoverFreeList[g_zoverFreeCount++] = handle;
            if (p->flags & ZOVERF_DIRTY) {
                zoverUndirty(handle);
                p->flags = 0;
            }
        } else if (p->type == ZOVER_GLYPH) {
            do {
                p = zoverPrim(handle, g_zoverBuffer, &count);
                next = p->next;
                p->next = ZOVER_NO_HANDLE;
                p->type = ZOVER_FREE;
                *ZOVER_TAGP(p) &= ZOVER_TAG_ADDR;
                g_zoverFreeList[g_zoverFreeCount++] = handle;
                if (p->flags & ZOVERF_DIRTY) {
                    zoverUndirty(handle);
                    p->flags = 0;
                }
                handle = next;
            } while (next != ZOVER_NO_HANDLE);
        }
    }
    g_zoverLock--;
}

/* VA 0x00458400, 958 bytes, [V] direct, status draft */
int zoveriiShow(uint16_t handle, int on)
{
    /* Hide = zero the tag length (the slot stays in the chain); show = restore the length, the
     * primitive code (semi-transparent variant when the flags carry a blend mode) and the mode words. */
    ZoverPrim *p;
    uint32_t count;
    uint16_t h = handle;

    if (handle == ZOVER_NO_HANDLE)
        return 0;
    g_zoverLock++;
    p = zoverPrim(handle, g_zoverBuffer, &count);
    if (!on) {
        if (p->type == ZOVER_GLYPH) {
            do {
                p = zoverPrim(h, g_zoverBuffer, &count);
                *ZOVER_TAGP(p) &= ZOVER_TAG_ADDR;
                zoverUnfree(h);
                zoverMarkDirty(p, h, 0x20D);
                h = p->next;
            } while (p->next != ZOVER_NO_HANDLE);
        } else {
            *ZOVER_TAGP(p) &= ZOVER_TAG_ADDR;
        }
    } else {
        switch (p->type) {
        case ZOVER_SPRITE:
            zoverSetLen(p, 9);
            ZOVER_FT4(p)->code = 0x2C;
            ZOVER_FT4(p)->mode = (ZOVER_FT4(p)->mode & 0xFFFFFF80u) | 0x7F80;
            if (p->flags & ZOVERF_BLEND) {
                ZOVER_FT4(p)->code = 0x2E;
                ZOVER_FT4(p)->blend = p->flags & 6;     /* sic: Add* store mode - 1 */
            }
            break;
        case ZOVER_RECT:
            zoverSetLen(p, 5);
            ZOVER_F4(p)->code = 0x28;
            ZOVER_F4(p)->mode = (ZOVER_F4(p)->mode & 0xFFFFFF80u) | 0x7F80;
            if (p->flags & ZOVERF_BLEND) {
                ZOVER_F4(p)->code = 0x2A;
                ZOVER_F4(p)->blend = p->flags & 6;
            }
            break;
        case ZOVER_GRAD:
            ZOVER_G4(p)->mode = (ZOVER_G4(p)->mode & 0xFFFFFF80u) | 0x7F80;
            zoverSetLen(p, 8);
            ZOVER_G4(p)->code = 0x38;
            if (p->flags & ZOVERF_BLEND) {
                ZOVER_G4(p)->code = 0x3A;
                ZOVER_G4(p)->blend = p->flags & 6;
            }
            break;
        case ZOVER_GLYPH:
            do {
                p = zoverPrim(h, g_zoverBuffer, &count);
                ZOVER_FT4(p)->mode = (ZOVER_FT4(p)->mode & 0xFFFFFF80u) | 0x7F80;
                zoverSetLen(p, 9);
                ZOVER_FT4(p)->code = 0x2C;
                if (p->flags & ZOVERF_BLEND) {
                    ZOVER_FT4(p)->code = 0x2E;
                    p->body[0x2C] = p->flags & 6;       /* sic: +0x2C (v3), not the blend byte +0x38 */
                }
                zoverUnfree(h);
                zoverMarkDirty(p, h, 0x1F9);
                h = p->next;
            } while (p->next != ZOVER_NO_HANDLE);
            break;
        }
    }
    zoverMarkDirty(p, handle, 0x217);
    zoverUnfree(handle);
    g_zoverLock--;
    return 0;
}

/* VA 0x004588d0, 445 bytes, [V] direct, status draft */
uint16_t zoveriiAddRect(uint8_t layer, int16_t x, int16_t y, int16_t w, int16_t h, uint32_t colour)
{
    /* a flat quad; bit 28 of the colour selects mode 0x7FC0 (used by the HUD backdrop) */
    uint16_t handle;
    ZoverPrim *p;
    ZoverPolyF4 *f;
    uint32_t count;

    g_zoverLock++;
    handle = zoveriiAlloc(layer, ZOVER_CLASS_SHAPE);
    if (handle != ZOVER_NO_HANDLE) {
        p = zoverPrim(handle, g_zoverBuffer, &count);
        f = ZOVER_F4(p);
        zoverSetLen(p, 5);
        f->code = 0x28;
        f->mode = (f->mode & 0xFFFFFF80u) | 0x7F80;
        if (colour & 0x10000000) {
            colour &= ~0x10000000u;
            f->mode = (f->mode & 0xFFFFFF80u) | 0x7FC0;
        }
        zoverSetRGB(&f->r, colour);
        if (colour & 0x07000000) {
            f->code = 0x2A;
            f->blend = (uint8_t)(((colour >> 24) & 7) - 1);
        }
        f->x0 = x;     f->y0 = y;
        f->x1 = x + w; f->y1 = y;
        f->x2 = x;     f->y2 = y + h;
        f->x3 = x + w; f->y3 = y + h;
        p->type = ZOVER_RECT;
        p->flags = (uint8_t)((colour >> 24) & 7);
        zoverUnfree(handle);
        zoverPushDirty(handle, 0x256);
        p->flags |= ZOVERF_DIRTY;
    }
    g_zoverLock--;
    return handle;
}

/* VA 0x00458b00, 351 bytes, [V] direct, status draft */
int zoveriiSetRect(uint16_t handle, int16_t x, int16_t y, int16_t w, int16_t h)
{
    ZoverPrim *p;
    ZoverPolyF4 *f;
    uint32_t count;

    g_zoverLock++;
    p = zoverPrim(handle, g_zoverBuffer, &count);
    if (ZOVER_INDEX(handle) >= count || ZOVER_INDEX(handle) == 0xFFFF) {
        g_zoverLock--;
        return 1;
    }
    f = ZOVER_F4(p);
    f->x0 = x;     f->x2 = x;
    f->y0 = y;     f->x1 = x + w;
    f->y1 = y;     f->y2 = y + h;
    f->x3 = x + w; f->y3 = y + h;
    zoverUnfree(handle);
    zoverMarkDirty(p, handle, 0x27C);
    g_zoverLock--;
    return 0;
}

/* VA 0x00458cc0, 380 bytes, [V] direct, status draft */
int zoveriiSetColour(uint16_t handle, uint32_t colour)
{
    /* colour and blend mode of a flat quad */
    ZoverPrim *p;
    ZoverPolyF4 *f;
    uint32_t count;

    g_zoverLock++;
    p = zoverPrim(handle, g_zoverBuffer, &count);
    if (ZOVER_INDEX(handle) >= count || ZOVER_INDEX(handle) == 0xFFFF) {
        g_zoverLock--;
        return 1;
    }
    f = ZOVER_F4(p);
    zoverSetRGB(&f->r, colour);
    if (colour & 0x07000000) {
        f->code |= 2;
        f->blend = (uint8_t)(((colour >> 24) & 7) - 1);
    } else {
        f->code &= ~2;
    }
    p->flags = (uint8_t)((p->flags & ~ZOVERF_BLEND) | ((colour >> 24) & ZOVERF_BLEND));
    zoverUnfree(handle);
    zoverMarkDirty(p, handle, 0x2A6);
    g_zoverLock--;
    return 0;
}

/* VA 0x00458ea0, 92 bytes, [I] between, status draft */
void zoveriiDrawRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t colour)
{
    /* immediate-mode rectangle (this frame only); NgIFpsx wants red and blue swapped */
    g_zoverRectSize[0] = w;
    g_zoverRectSize[1] = h;
    psxDrawRect(((colour >> 16) & 0xFF) | (colour & 0xFF) << 16 | (colour & 0xFF00FF00u), x, y, 0x200,
                     g_zoverRectSize);
}

/* VA 0x00458f20, 480 bytes, [V] direct, status draft */
uint16_t zoveriiAddGradient(uint8_t layer, int16_t x, int16_t y, int16_t w, int16_t h, uint32_t top,
                            uint32_t bottom)
{
    /* a quad shaded from 'top' (upper edge) to 'bottom'; the blend mode comes from 'top' */
    uint16_t handle;
    ZoverPrim *p;
    ZoverPolyG4 *g;
    uint32_t count;

    g_zoverLock++;
    handle = zoveriiAlloc(layer, ZOVER_CLASS_SHAPE);
    if (handle != ZOVER_NO_HANDLE) {
        p = zoverPrim(handle, g_zoverBuffer, &count);
        g = ZOVER_G4(p);
        g->mode = (g->mode & 0xFFFFFF80u) | 0x7F80;
        zoverSetLen(p, 8);
        g->code = 0x38;
        if (top & 0x07000000) {
            g->code = 0x3A;
            g->blend = (uint8_t)(((top >> 24) & 7) - 1);
        }
        zoverSetRGB(&g->r0, top);
        zoverSetRGB(&g->r1, top);
        zoverSetRGB(&g->r2, bottom);
        zoverSetRGB(&g->r3, bottom);
        g->x0 = x;     g->y0 = y;
        g->x1 = x + w; g->y1 = y;
        g->x2 = x;     g->y2 = y + h;
        g->x3 = x + w; g->y3 = y + h;
        p->type = ZOVER_GRAD;
        p->flags = (uint8_t)((top >> 24) & 7);
        zoverUnfree(handle);
        zoverPushDirty(handle, 0x2FF);
        p->flags |= ZOVERF_DIRTY;
    }
    g_zoverLock--;
    return handle;
}

/* VA 0x00459180, 642 bytes, [V] direct, status draft */
uint16_t zoveriiAddSprite(uint8_t layer, uint16_t slot, int16_t x, int16_t y, uint32_t colour)
{
    /* a whole image slot as a textured quad (overlayLogoInit) */
    uint16_t handle, u1, v1;
    ZoverPrim *p;
    ZoverPolyFT4 *t;
    const ZoverTexture *tex = &g_psxTextures[slot];
    uint32_t count;

    g_zoverLock++;
    handle = zoveriiAlloc(layer, ZOVER_CLASS_SHAPE);
    if (handle != ZOVER_NO_HANDLE) {
        p = zoverPrim(handle, g_zoverBuffer, &count);
        t = ZOVER_FT4(p);
        t->code = 0x2C;
        zoverSetLen(p, 9);
        t->mode = (t->mode & 0xFFFFFF80u) | 0x7F80;
        if (colour & 0x07000000) {
            t->code = 0x2E;
            t->blend = (uint8_t)(((colour >> 24) & 7) - 1);
        }
        t->tpage = zoverTpage(tex, colour);
        zoverSetRGB(&t->r, colour);
        u1 = (uint16_t)(tex->u1 + 1); if (u1 > 0xFF) u1 = 0xFF;
        v1 = (uint16_t)(tex->v1 + 1); if (v1 > 0xFF) v1 = 0xFF;
        t->clut = tex->clut;
        t->texture = tex->texture;
        t->x0 = x;          t->y0 = y;          t->u0 = tex->u; t->v0 = tex->v;
        t->x1 = x + tex->w; t->y1 = y;          t->u1 = u1;     t->v1 = tex->v;
        t->x2 = x;          t->y2 = y + tex->h; t->u2 = tex->u; t->v2 = v1;
        t->x3 = x + tex->w; t->y3 = y + tex->h; t->u3 = u1;     t->v3 = v1;
        p->type = ZOVER_SPRITE;
        p->flags = (uint8_t)((colour >> 24) & 7);
        zoverPushDirty(handle, 0x344);
        p->flags |= ZOVERF_DIRTY;
        zoverUnfree(handle);
    }
    g_zoverLock--;
    return handle;
}

/* VA 0x004594c0, 901 bytes, [V] direct, status draft */
uint16_t zoveriiAddText(uint8_t layer, const ZoverFont *font, const char *s, int16_t x, int16_t y,
                        uint32_t colour, int16_t clut)
{
    /* One glyph primitive per printable character, chained through ZoverPrim.next; returns the first
     * handle. Characters outside the font are skipped (no advance). 'clut' is added to the font's
     * palette: the HUD uses palette variants 2..6 of the small font for coloured text. */
    const ZoverTexture *tex;
    const ZoverGlyph *g;
    uint16_t first = ZOVER_NO_HANDLE, prev = ZOVER_NO_HANDLE, h, tpage;
    ZoverPrim *p;
    ZoverPolyFT4 *t;
    uint32_t count;
    int16_t u, v;
    int c;

    g_zoverLock++;
    if (font == NULL)
        zAssertFail("Null font in overlay", ZOVERII_SRC, 0x363, NULL);
    tex = &g_psxTextures[*font->slot];
    tpage = zoverTpage(tex, colour);
    for (; *s != '\0'; s++) {
        c = (uint8_t)*s;
        if (c < font->first || c >= font->first + font->count)
            continue;
        g = &font->glyphs[c - font->first];
        h = zoveriiAlloc(layer, ZOVER_CLASS_GLYPH);
        if (h == ZOVER_NO_HANDLE) {
            if (first != ZOVER_NO_HANDLE)
                zoveriiFree(first);
            g_zoverLock--;
            return ZOVER_NO_HANDLE;
        }
        if (first == ZOVER_NO_HANDLE)
            first = h;
        p = zoverPrim(h, g_zoverBuffer, &count);
        t = ZOVER_FT4(p);
        zoverSetLen(p, 9);
        t->code = 0x2C;
        t->mode = (t->mode & 0xFFFFFF80u) | 0x7F80;
        p->flags = 0;
        if (colour & 0x07000000) {
            t->code = 0x2E;
            p->flags = (uint8_t)((colour >> 24) & 7);
            t->blend = (uint8_t)(((colour >> 24) & 7) - 1);
        }
        zoverSetRGB(&t->r, colour);
        t->tpage = tpage;
        t->clut = (uint16_t)(tex->clut + clut);
        t->texture = tex->texture;
        u = (int16_t)(tex->u + g->u);
        v = (int16_t)(tex->v + g->v);
        t->x0 = x;        t->y0 = y;                t->u0 = u;        t->v0 = v;
        t->x1 = x + g->w; t->y1 = y;                t->u1 = u + g->w; t->v1 = v;
        t->x2 = x;        t->y2 = y + font->height; t->u2 = u;        t->v2 = v + font->height;
        t->x3 = x + g->w; t->y3 = y + font->height; t->u3 = u + g->w; t->v3 = v + font->height;
        p->type = ZOVER_GLYPH;
        p->next = ZOVER_NO_HANDLE;
        if (prev != ZOVER_NO_HANDLE)
            zoverPrim(prev, g_zoverBuffer, &count)->next = h;
        zoverUnfree(h);
        zoverPushDirty(h, 0x3AC);
        p->flags |= ZOVERF_DIRTY;
        x = (int16_t)(x + g->w + 1);
        prev = h;
    }
    g_zoverLock--;
    return first;
}

/* VA 0x00459940, 524 bytes, [V] direct, status draft */
int zoveriiSetTextColour(uint16_t handle, const ZoverFont *font, uint32_t colour, int16_t clut)
{
    /* recolour a whole text chain (menu highlight, fades) */
    const ZoverTexture *tex;
    uint16_t tpage, h = handle;
    ZoverPrim *p;
    ZoverPolyFT4 *t;
    uint32_t count;

    g_zoverLock++;
    if (handle != ZOVER_NO_HANDLE) {
        tex = &g_psxTextures[*font->slot];
        tpage = zoverTpage(tex, colour);
        zoverPrim(handle, g_zoverBuffer, &count);
        if (ZOVER_INDEX(handle) < count) {
            for (;;) {
                p = zoverPrim(h, g_zoverBuffer, &count);
                t = ZOVER_FT4(p);
                zoverSetRGB(&t->r, colour);
                if (colour & 0x07000000) {
                    t->code |= 2;
                    t->blend = (uint8_t)(((colour >> 24) & 7) - 1);
                } else {
                    t->code &= ~2;
                }
                t->tpage = tpage;
                t->clut = (uint16_t)(tex->clut + clut);
                t->texture = tex->texture;
                zoverMarkDirty(p, h, 0x3EF);
                zoverUnfree(h);
                h = p->next;
                if (h == ZOVER_NO_HANDLE)
                    break;
            }
            g_zoverLock--;
            return 0;
        }
    }
    g_zoverLock--;
    return 1;
}

/* VA 0x00459be0, 414 bytes, [V] direct, status draft */
int zoveriiMoveText(uint16_t handle, int16_t x, int16_t y)
{
    /* [I] unused by the overlays: it moves every glyph of the chain to the same x and sets all four
     * corners to the same y (zero height), so it only works for one-glyph chains as written. */
    ZoverPrim *p;
    ZoverPolyFT4 *t;
    uint32_t count;
    int16_t x1;
    uint16_t h = handle;

    g_zoverLock++;
    zoverPrim(handle, g_zoverBuffer, &count);
    if (ZOVER_INDEX(handle) >= count || ZOVER_INDEX(handle) == 0xFFFF) {
        g_zoverLock--;
        return 1;
    }
    if (handle != ZOVER_NO_HANDLE) {
        do {
            p = zoverPrim(h, g_zoverBuffer, &count);
            t = ZOVER_FT4(p);
            x1 = (int16_t)(t->x1 - t->x0 + x);
            t->x0 = x;  t->y0 = y;
            t->x1 = x1; t->y1 = y;
            t->x2 = x;  t->y2 = y;
            t->x3 = x1; t->y3 = y;
            zoverUnfree(h);
            zoverMarkDirty(p, h, 0x423);
            h = p->next;
        } while (p->next != ZOVER_NO_HANDLE);
    }
    g_zoverLock--;
    return 0;
}

/* VA 0x00459df0, 252 bytes, [V] direct, status draft */
void zoveriiDrawText(const ZoverFont *font, const char *s, int16_t x, int16_t y, uint32_t colour,
                     uint16_t clut)
{
    /* immediate-mode text (this frame only): one NgIFpsx sprite per glyph, colour as 0x0RGB */
    const ZoverGlyph *g;
    int c;

    if (font == NULL)
        zAssertFail("Null font in overlay", ZOVERII_SRC, 0x446, NULL);
    g_zoverGlyphSprite.slot = *font->slot;
    g_zoverGlyphSprite.h = font->height;
    g_zoverGlyphSprite.colour = (uint16_t)(((colour >> 12) & 0xFF00) | ((colour >> 8) & 0xF0) |
                                           ((colour >> 4) & 0x0F));
    g_zoverGlyphSprite.clut = clut;
    for (; *s != '\0'; s++) {
        c = (uint8_t)*s;
        if (c < font->first || c >= font->first + font->count)
            continue;
        g = &font->glyphs[c - font->first];
        g_zoverGlyphSprite.u = g->u;
        g_zoverGlyphSprite.v = g->v;
        g_zoverGlyphSprite.w = g->w;
        psxDrawSpriteList(x, y, &g_zoverGlyphSprite);
        x = (int16_t)(x + g->w + 1);
    }
}

/* VA 0x00459f30, 109 bytes, [V] direct, status draft */
int16_t zoveriiTextWidth(const char *s, const ZoverFont *font)
{
    int16_t w = 0;
    int c;

    if (font == NULL)
        zAssertFail("Null font in overlay", ZOVERII_SRC, 0x469, NULL);
    for (; *s != '\0'; s++) {
        c = (int8_t)*s;                     /* signed compare, as in the exe */
        if (font->first <= c && c < font->first + font->count)
            w = (int16_t)(w + font->glyphs[c - font->first].w + 1);
    }
    return w;
}

/* VA 0x00459fc0, 369 bytes, [V] direct, status draft */
int zoveriiLoadFont(const uint8_t *data, ZoverFont *font)
{
    /* Build the glyph table from a font file (miscdata.zal 0x7531/0x7532/0x7534/0x7535, see
     * docs/re/hud.md): the character range is min..max of the records; returns 1 without memory.
     * The glyph images are Material.ZAL groups 9 / 11 / 12 / 13 (DM-066). */
    const ZoverFontHeader *hdr = (const ZoverFontHeader *)data;
    const ZoverFontRecord *rec = (const ZoverFontRecord *)(data + sizeof(ZoverFontHeader));
    uint32_t n = hdr->count, i;
    uint8_t lo = 0xFF, hi = 0;
    ZoverGlyph *g;

    if (font == NULL)
        zAssertFail("Null font in overlay", ZOVERII_SRC, 0x486, NULL);
    if (g_zoverNumFonts > ZOVER_MAX_FONTS - 1)
        zAssertFail("Overflow", ZOVERII_SRC, 0x487, NULL);
    font->height = hdr->height;
    for (i = 0; i < n; i++) {
        if (rec[i].ch <= lo)
            lo = rec[i].ch;
        if (hi <= rec[i].ch)
            hi = rec[i].ch;
    }
    font->first = (int8_t)lo;
    font->count = (uint8_t)(hi - lo + 1);
    font->glyphs = (ZoverGlyph *)zmemTempAlloc((uint32_t)font->count * sizeof(ZoverGlyph));
    if (font->glyphs == NULL)
        return 1;
    zstringMemSet(font->glyphs, 0, (uint32_t)font->count * sizeof(ZoverGlyph));
    for (i = 0; i < n; i++) {
        g = &font->glyphs[rec[i].ch - lo];
        g->u = (int16_t)(rec[i].u[0] | rec[i].u[1] << 8);
        g->v = (int16_t)(rec[i].v[0] | rec[i].v[1] << 8);
        g->w = (int16_t)(rec[i].w[0] | rec[i].w[1] << 8);
    }
    font->unk_04 = 0;
    g_zoverFonts[g_zoverNumFonts++] = font;
    return 0;
}

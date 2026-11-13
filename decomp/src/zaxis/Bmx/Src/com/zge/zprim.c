/*
 * zaxis/Bmx/Src/com/zge/zprim.c
 * Original: C:\zaxis\Bmx\src\com\zge\zprim.c
 * Evidence: [V] path string referenced by zprimFlush, zprimReplaceVRAMGrey, zprimSelectPaletteBank,
 * zprimLoadFont, zprimLoadAsset and zprimMakeMaterial ("zprimReplaceVRAMGrey()" also names one
 * function); the rest [I]: "ZPRIM:" messages, link order 0x004779E0-0x0047C3C2 and the globals
 * 0x006783FC-0x00678433 that no other code touches.
 * 73 functions (DM-053; DM-061 created the two bare rets 0x00479240 / 0x00479260; DM-064 created 21
 * reached only through their /INCREMENTAL thunks: state, background, getters, the SPRITE2 RLE). Non-matching C (AD-8). Types: decomp/include/zprim.h.
 *
 * The 2D layer of the front end (frontend.c, guimain.c): a stack of draw states, an asset table for
 * the pictures / sprites / fonts / screens / groups of the GUI picture library, and primitives
 * (background, lines, rectangles, sprites, text) drawn through NgIFpsx, the PlayStation-style
 * drawing layer of the PC port. Pictures and sprite frames become RenderWare materials through an
 * in-memory BMP built by zprimMakeMaterial.
 *
 * [V] quirks kept from the original: zprimInit leaves g_zprimStates pointing at the unlocked DMM
 * block, so later calls do not lock it again; zprimPushState can reach index 10, one past the
 * 0x348-byte block; the text loops stop at any byte <= 2, so their 2-byte escape (byte 1) is dead.
 */
#include "decomp.h"
#include "zprim.h"

#define ZPRIM_FILE "C:\\zaxis\\Bmx\\src\\com\\zge\\zprim.c"

/* other modules (current names until their cards land) */
int   psx2DInit(void);                                  /* PSX layer init (0 = ok) */
void  psx2DShutdown(void);                                  /* PSX layer shutdown */
void  psx2DBeginFrame(void);                                  /* begin a 2D frame */
void  psx2DDrawPicture(ZprimImage *img, int width, int height); /* full-screen picture */
void  psx2DSetTint(uint32_t a, uint32_t b);                /* sprite CLUT override (0, 0 = off) */
void  psx2DDrawGlyph(ZprimFont *font, const ZprimGlyph *g, int mode, uint32_t arg1, uint32_t arg2,
                       int x, int y, int arg14, uint32_t arg40);  /* one glyph */
void  psx2DSetClip(int x0, int y0, int x1, int y1);            /* NgIFpsx: clip rectangle */
void  psx2DDrawSprite(ZprimImage *img, uint32_t frame, const void *palette, int mode, int zero, uint32_t arg,
                   int arg24, int x, int y, int w, int h, int bpp, uint32_t arg30);  /* one sprite frame */
void  psx2DFillRect(uint32_t colour, int mode, int x0, int y0, int x1, int y1); /* filled rectangle */
void  psx2DDrawLine(uint32_t colour, int mode, int x0, int y0, int x1, int y1); /* line */
int   printf(const char *fmt, ...);                            /* CRT 0x004A51C8 */
extern void  (*g_pfnZglMatDestroy)(int32_t mat);               /* 0x00561024 -> zrwMatRelease */
extern int32_t (*g_pfnZglMatClone)(int32_t mat);               /* 0x0056103C -> zrwMat_00489A80 */
extern int32_t (*g_pfnZglMatCreateFromImage)(const void *bmp, uint32_t size, const char *name, int a, int b);
                                                               /* 0x00561048 -> zrwMatCreateFromImage */
extern ZmemFreeList *g_ngFontMaterials;                        /* 0x0068FF94 NgApp: glyph material tables */

/* ------------------------------------------------------------------ globals */
uint16_t    g_zprimSpriteCount;            /* 0x0064EB34 numbers the "Sprite%d" materials */
int32_t     g_zprimStateHandle;            /* 0x006783FC DMM block: ZprimState[ZPRIM_MAX_STATES] */
ZassetLib  *g_zprimLib;                    /* 0x00678400 the picture library */
ZprimAsset *g_zprimAssetPool;              /* 0x00678404 [ZPRIM_MAX_ASSETS] */
ZprimAsset *g_zprimFreeAssets;             /* 0x00678408 */
ZprimAsset *g_zprimAssets;                 /* 0x0067840C handles in use */
int16_t     g_zprimClip[4];                /* 0x00678410 x0, y0, x1, y1 */
ZprimAsset *g_zprimGroups[ZPRIM_MAX_GROUPS]; /* 0x00678418 */
uint16_t    g_zprimNextId;                 /* 0x00678428 next ZPRIM_TYPE_VSPRITE id */
int32_t     g_zprimStateTop;               /* 0x0067842C current state index */
ZprimState *g_zprimStates;                 /* 0x00678430 locked state block, or NULL */

/* Every state accessor locks the state block unless someone already holds it. */
#define STATE_LOCK(held)   do { (held) = g_zprimStates != 0;                                      \
                                if (!(held))                                                       \
                                    g_zprimStates = (ZprimState *)zmemDMMLock(g_zprimStateHandle); \
                           } while (0)
#define STATE_UNLOCK(held) do { if (!(held)) { g_zprimStates = 0; zmemDMMUnlock(g_zprimStateHandle); } } while (0)
#define CUR_STATE()        (&g_zprimStates[g_zprimStateTop])

/* NgIFpsx draw mode of a ZPRIM_MODE_* flag word */
#define DRAW_MODE(f) (((f) & ZPRIM_MODE_1) ? 1 : ((f) & ZPRIM_MODE_2) ? 2 : ((f) & ZPRIM_MODE_3) ? 3 : 0)

/* ------------------------------------------------------------------ setup */

/* VA 0x004779e0, 275 bytes, [I] row, status draft */
int zprimInit(void)
{
    ZprimState *states;
    int i;

    if (psx2DInit() != 0)
        return 1;
    g_zprimStateHandle = zmemDMMAlloc(ZPRIM_MAX_STATES * sizeof(ZprimState));
    if (g_zprimStateHandle != 0) {
        states = (ZprimState *)zmemDMMLock(g_zprimStateHandle);
        g_zprimStateTop = 0;
        g_zprimStates = states;                     /* [V] left set after the unlock below */
        zstringMemSet(states, 0, ZPRIM_MAX_STATES * sizeof(ZprimState));
        zstringMemSet(states, 0, sizeof(ZprimState));
        zprimResetScale();
        zprimPushState();
        zmemDMMUnlock(g_zprimStateHandle);
        g_zprimLib = 0;                             /* 0x00678400..0x00678427 */
        g_zprimAssetPool = g_zprimFreeAssets = g_zprimAssets = 0;
        g_zprimClip[0] = g_zprimClip[1] = g_zprimClip[2] = g_zprimClip[3] = 0;
        for (i = 0; i < ZPRIM_MAX_GROUPS; i++)
            g_zprimGroups[i] = 0;
        zprimSetClip(0, 0, 639, 479);
        g_zprimAssetPool = (ZprimAsset *)zmemTempAlloc(ZPRIM_MAX_ASSETS * sizeof(ZprimAsset));
        if (g_zprimAssetPool != 0) {
            zstringMemSet(g_zprimAssetPool, 0, ZPRIM_MAX_ASSETS * sizeof(ZprimAsset));
            g_zprimFreeAssets = g_zprimAssetPool;
            for (i = 0; i < ZPRIM_MAX_ASSETS; i++)
                g_zprimAssetPool[i].next = (i == ZPRIM_MAX_ASSETS - 1) ? 0 : &g_zprimAssetPool[i + 1];
            g_zprimAssets = 0;
            states[0].colour = 0xFFFF;
            g_zprimNextId = ZPRIM_TYPE_VSPRITE | 1;
            return 0;
        }
    }
    g_pfnDebugPrintf("ZPRIM: Failed to allocate buffer\n");
    return 1;
}

/* VA 0x00477b50, 61 bytes, [I] row, status draft */
void zprimShutdown(void)
{
    if (g_zprimLib != 0)
        zassetCloseLibrary(g_zprimLib);
    g_zprimLib = 0;
    if (g_zprimStateHandle != 0) {
        zmemDMMDealloc(g_zprimStateHandle);
        g_zprimStateHandle = 0;
    }
    psx2DShutdown();
}

/* VA 0x00477ba0, 47 bytes, [I] row, status draft */
void zprimOpenLibrary(const char *path)
{
    if (g_zprimLib != 0)
        zassetCloseLibrary(g_zprimLib);
    g_zprimLib = 0;
    g_zprimLib = zassetOpenLibrary(path);
}

/* Start a 2D frame and draw the current state's background: a picture (ZPRIM_ATTR_BG_PICTURE),
 * nothing (2, 3, 4), or the clip rectangle filled with the colour. */
/* VA 0x00477be0, 193 bytes, [I] row, status draft */
void zprimDrawBackground(void)
{
    int locked = g_zprimStates == 0;
    ZprimState *st;

    if (locked)
        g_zprimStates = (ZprimState *)zmemDMMLock(g_zprimStateHandle);
    st = CUR_STATE();
    psx2DBeginFrame();
    switch (st->bgType) {
    case ZPRIM_ATTR_BG_PICTURE: {
        ZprimImage *img = (ZprimImage *)zprimLockAsset((ZprimAsset *)st->bgData);
        psx2DDrawPicture(img, img->width, img->height);
        zprimUnlockAsset((ZprimAsset *)st->bgData);
        break;
    }
    case ZPRIM_ATTR_BG_2:
    case ZPRIM_ATTR_BG_3:
    case ZPRIM_ATTR_BG_NONE:
        break;
    default:
        psx2DFillRect(st->colour, 0, g_zprimClip[0], g_zprimClip[1], g_zprimClip[2], g_zprimClip[3]);
        break;
    }
    if (locked) {
        g_zprimStates = 0;
        zmemDMMUnlock(g_zprimStateHandle);
    }
}

/* Free every asset not marked ZPRIM_ASSET_KEEP, reset the state and compact the DMM heap. */
/* VA 0x00477d20, 86 bytes, [V] direct, status draft */
void zprimFlush(void)
{
    ZprimAsset *a = g_zprimAssets, *next;

    while (a != 0) {
        next = a->next;
        if (!(a->flags & ZPRIM_ASSET_KEEP))
            zprimFreeAsset(a);
        a = next;
    }
    g_zprimNextId = ZPRIM_TYPE_VSPRITE | 0x101;
    zprimResetState();
    if (zmemDMMOptimize() != 0)
        zAssertFail("Error Optimizing DMM memory", ZPRIM_FILE, 0x2DB, 0);
}

/* VA 0x00477da0, 58 bytes, [I] row, status draft */
void zprimSetClip(int x0, int y0, int x1, int y1)
{
    g_zprimClip[0] = (int16_t)x0;
    g_zprimClip[1] = (int16_t)y0;
    g_zprimClip[2] = (int16_t)x1;
    g_zprimClip[3] = (int16_t)y1;
    psx2DSetClip(x0, y0, x1, y1);
}

/* The current state becomes a copy of state 0. */
/* VA 0x00477df0, 98 bytes, [I] row, status draft */
void zprimSetBaseState(void)
{
    /* (DM-064 created; no caller, thunk 0x00401906) Makes the current state the base state that
     * zprimResetState returns to. */
    int held;

    STATE_LOCK(held);
    g_zprimStates[0] = *CUR_STATE();
    STATE_UNLOCK(held);
}

/* VA 0x00477e80, 98 bytes, [I] row, status draft */
void zprimResetState(void)
{
    int held;

    STATE_LOCK(held);
    *CUR_STATE() = g_zprimStates[0];
    STATE_UNLOCK(held);
}

/* VA 0x00477f10, 127 bytes, [I] row, status draft */
void zprimPushState(void)
{
    int held;

    STATE_LOCK(held);
    if (g_zprimStateTop < ZPRIM_MAX_STATES) {
        g_zprimStates[g_zprimStateTop + 1] = g_zprimStates[g_zprimStateTop];
        g_zprimStateTop++;
    }
    STATE_UNLOCK(held);
}

/* ------------------------------------------------------------------ state attributes */

#define SETBIT(field, bit, on) ((field) = (uint16_t)((on) ? ((field) | (bit)) : ((field) & ~(bit))))

/* VA 0x00477fb0, 114 bytes, [I] row, status draft */
void zprimDropState(void)
{
    /* (DM-064 created; no caller, thunk 0x0040224D) Pops the state stack but keeps the current
     * attributes (they overwrite the saved state). */
    int held;

    STATE_LOCK(held);
    if (g_zprimStateTop > 0) {
        g_zprimStates[g_zprimStateTop - 1] = g_zprimStates[g_zprimStateTop];
        g_zprimStateTop--;
    }
    STATE_UNLOCK(held);
}

/* VA 0x00478050, 533 bytes, [I] row, status draft */
void zprimSetAttr(int attr, uint32_t value)
{
    int held;
    ZprimState *st;

    STATE_LOCK(held);
    st = CUR_STATE();
    switch ((int16_t)attr) {
    case ZPRIM_ATTR_BG_PICTURE: st->bgType = 1; st->bgData = value; break;
    case ZPRIM_ATTR_BG_2:       st->bgType = 2; st->bgData = value; break;
    case ZPRIM_ATTR_BG_3:       st->bgType = 3; st->bgData = value; break;
    case ZPRIM_ATTR_BG_NONE:    st->bgType = 4; break;
    case ZPRIM_ATTR_COLOUR:     st->colour = value; break;
    case ZPRIM_ATTR_PRIM_MODE1: SETBIT(st->primFlags, ZPRIM_MODE_1, value); break;
    case ZPRIM_ATTR_SPR_MODE1:  SETBIT(st->spriteFlags, ZPRIM_MODE_1, value); break;
    case ZPRIM_ATTR_SPR_MODE3:  SETBIT(st->spriteFlags, ZPRIM_MODE_3, value); break;
    case ZPRIM_ATTR_SPR_MODE2:  SETBIT(st->spriteFlags, ZPRIM_MODE_2, value); break;
    case ZPRIM_ATTR_SPR_ARG20:  st->spriteArg20 = value; break;
    case ZPRIM_ATTR_SPR_ARG1C:  st->spriteArg1c = value; break;
    case ZPRIM_ATTR_SPR_BIT3:   SETBIT(st->spriteFlags, 0x08, value); break;
    case ZPRIM_ATTR_SPR_BIT4:   SETBIT(st->spriteFlags, 0x10, value); break;
    case ZPRIM_ATTR_SPR_ARG30:  st->spriteArg30 = value; break;
    case ZPRIM_ATTR_SPR_ARG24:  st->spriteArg24 = (uint16_t)value; break;
    case ZPRIM_ATTR_CLUT34:     st->clut34 = value; break;
    case ZPRIM_ATTR_CLUT38:     st->clut38 = value; break;
    case ZPRIM_ATTR_FONT_MODE1: SETBIT(st->fontFlags, ZPRIM_MODE_1, value); break;
    case ZPRIM_ATTR_FONT_MODE3: SETBIT(st->fontFlags, ZPRIM_MODE_3, value); break;
    case ZPRIM_ATTR_FONT_MODE2: SETBIT(st->fontFlags, ZPRIM_MODE_2, value); break;
    case ZPRIM_ATTR_TEXT_BG:    st->textBg = value; break;
    case ZPRIM_ATTR_FONT_ARG48: st->fontArg48 = value; break;
    case ZPRIM_ATTR_FONT_ARG40: st->fontArg40 = value; break;
    case ZPRIM_ATTR_SPACING:    st->spacing = (int32_t)value; break;
    }
    STATE_UNLOCK(held);
}

/* The flag attributes read back as 0 / 1; ZPRIM_ATTR_PRIM_MODE1 and ZPRIM_ATTR_BG_NONE read 0. */
/* VA 0x00478370, 984 bytes, [I] row, status draft */
uint32_t zprimGetAttr(int attr)
{
    int held;
    ZprimState *st;
    uint32_t v = 0;

    STATE_LOCK(held);
    st = CUR_STATE();
    switch ((int16_t)attr) {
    case ZPRIM_ATTR_BG_PICTURE: v = st->bgType == 1 ? st->bgData : 0; break;
    case ZPRIM_ATTR_BG_2:       v = st->bgType == 2 ? st->bgData : 0; break;
    case ZPRIM_ATTR_BG_3:       v = st->bgType == 3 ? st->bgData : 0; break;
    case ZPRIM_ATTR_COLOUR:     v = st->colour; break;
    case ZPRIM_ATTR_SPR_MODE1:  v = (st->spriteFlags & ZPRIM_MODE_1) != 0; break;
    case ZPRIM_ATTR_SPR_MODE3:  v = (st->spriteFlags & ZPRIM_MODE_3) != 0; break;
    case ZPRIM_ATTR_SPR_MODE2:  v = (st->spriteFlags & ZPRIM_MODE_2) != 0; break;
    case ZPRIM_ATTR_SPR_ARG20:  v = st->spriteArg20; break;
    case ZPRIM_ATTR_SPR_ARG1C:  v = st->spriteArg1c; break;
    case ZPRIM_ATTR_SPR_BIT3:   v = (st->spriteFlags & 0x08) != 0; break;
    case ZPRIM_ATTR_SPR_BIT4:   v = (st->spriteFlags & 0x10) != 0; break;
    case ZPRIM_ATTR_SPR_ARG30:  v = st->spriteArg30; break;
    case ZPRIM_ATTR_SPR_ARG24:  v = st->spriteArg24; break;
    case ZPRIM_ATTR_CLUT34:     v = st->clut34; break;
    case ZPRIM_ATTR_CLUT38:     v = st->clut38; break;
    case ZPRIM_ATTR_FONT_MODE1: v = (st->fontFlags & ZPRIM_MODE_1) != 0; break;
    case ZPRIM_ATTR_FONT_MODE3: v = (st->fontFlags & ZPRIM_MODE_3) != 0; break;
    case ZPRIM_ATTR_FONT_MODE2: v = (st->fontFlags & ZPRIM_MODE_2) != 0; break;
    case ZPRIM_ATTR_TEXT_BG:    v = st->textBg; break;
    case ZPRIM_ATTR_FONT_ARG48: v = st->fontArg48; break;
    case ZPRIM_ATTR_FONT_ARG40: v = st->fontArg40; break;
    case ZPRIM_ATTR_SPACING:    v = (uint32_t)st->spacing; break;
    default:                    v = 0; break;
    }
    STATE_UNLOCK(held);
    return v;
}

/* VA 0x004788c0, 133 bytes, [I] row, status draft */
void zprimSetBackgroundPicture(ZprimAsset *picture)
{
    /* (DM-064 created; no caller, thunk 0x0040191A) The zprimSetAttr(ZPRIM_ATTR_BG_PICTURE) case as a
     * function; NULL is ignored. */
    int held;

    STATE_LOCK(held);
    if (picture != 0) {
        CUR_STATE()->bgType = ZPRIM_ATTR_BG_PICTURE;
        CUR_STATE()->bgData = (uint32_t)(size_t)picture;
    }
    STATE_UNLOCK(held);
}

/* VA 0x00478980, 153 bytes, [I] row, status draft */
void zprimSetBackgroundSprite(ZprimAsset *sprite, int16_t type, uint16_t frame, int16_t x, int16_t y)
{
    /* (DM-064 created; no caller, thunk 0x00401294) Background = a sprite frame at (x, y): type 3
     * keeps ZPRIM_ATTR_BG_3, anything else is BG_2. */
    int held;
    ZprimState *st;

    if (sprite == 0)
        return;
    STATE_LOCK(held);
    st = CUR_STATE();
    st->bgType = type == ZPRIM_ATTR_BG_3 ? ZPRIM_ATTR_BG_3 : ZPRIM_ATTR_BG_2;
    st->pad_06 = frame;
    st->bgData = (uint32_t)(size_t)sprite;
    st->unk_08 = (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
    STATE_UNLOCK(held);
}

/* VA 0x00478a50, 98 bytes, [I] row, status draft */
void zprimSetFont(ZprimAsset *font)
{
    int held;

    if (font == 0)
        return;
    STATE_LOCK(held);
    CUR_STATE()->font = font;
    STATE_UNLOCK(held);
}

/* VA 0x00478ae0, 92 bytes, [I] row, status draft */
ZprimAsset *zprimGetFont(void)
{
    /* DM-064 created; no caller, thunk 0x004014AB */
    int held;
    ZprimAsset *font;

    STATE_LOCK(held);
    font = CUR_STATE()->font;
    STATE_UNLOCK(held);
    return font;
}

/* VA 0x00478b60, 98 bytes, [I] row, status draft */
void zprimSetSprite(ZprimAsset *sprite)
{
    int held;

    if (sprite == 0)
        return;
    STATE_LOCK(held);
    CUR_STATE()->sprite = sprite;
    STATE_UNLOCK(held);
}

/* VA 0x00478bf0, 99 bytes, [I] row, status draft */
void zprimSetScale(float x, float y)
{
    int held;

    STATE_LOCK(held);
    CUR_STATE()->scaleX = x;
    CUR_STATE()->scaleY = y;
    STATE_UNLOCK(held);
}

/* VA 0x00478c80, 96 bytes, [I] row, status draft */
void zprimResetScale(void)
{
    int held;

    STATE_LOCK(held);
    CUR_STATE()->scaleX = 1.0f;
    CUR_STATE()->scaleY = 1.0f;
    STATE_UNLOCK(held);
}

/* VA 0x00478d00, 92 bytes, [I] row, status draft */
ZprimAsset *zprimGetSprite(void)
{
    /* DM-064 created; no caller, thunk 0x00401DFC */
    int held;
    ZprimAsset *sprite;

    STATE_LOCK(held);
    sprite = CUR_STATE()->sprite;
    STATE_UNLOCK(held);
    return sprite;
}

/* VA 0x00478d80, 42 bytes, [I] row, status draft */
void zprimSetRGB(uint32_t r, uint8_t g, uint8_t b, int attr)
{
    zprimSetAttr(attr, (uint32_t)b << 16 | (uint32_t)g << 8 | (r & 0xFF));
}

/* ------------------------------------------------------------------ primitives */

/* VA 0x00478dc0, 26 bytes, [V] direct, status draft */
void zprimGetColor(void)
{
    /* DM-064 created; no caller, thunk 0x004011EA */
    zAssertFail("ZPRIM: zprimGetColor not implemented\n", ZPRIM_FILE, 0x5F0, 0);
}

/* VA 0x00478df0, 205 bytes, [I] row, status draft */
void zprimDrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int held;
    ZprimState *st;

    STATE_LOCK(held);
    if (x1 < x0) {                     /* draw left to right */
        int16_t t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    st = CUR_STATE();
    psx2DDrawLine(st->colour, DRAW_MODE(st->primFlags), x0, y0, x1, y1);
    STATE_UNLOCK(held);
}

/* VA 0x00478f00, 204 bytes, [I] row, status draft */
void zprimDrawRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int held;
    ZprimState *st;
    int16_t t;

    STATE_LOCK(held);
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    st = CUR_STATE();
    psx2DFillRect(st->colour, DRAW_MODE(st->primFlags), x0, y0, x1, y1);
    STATE_UNLOCK(held);
}

/* ------------------------------------------------------------------ loading */

/* Shared tail of the loaders: take the data from a loaded group (shared) or load the asset into its
 * own DMM block. Returns 0 and prints `findMsg` if the library does not have it. */
#define LOAD_DATA(id, keep, shared, handle, offset, findMsg, allocFail)                          \
    do {                                                                                     \
        ZprimAsset *g_ = zprimFindInGroups(id);                                              \
        (offset) = 0;                                                                        \
        if (g_ == 0) {                                                                       \
            uint32_t size_;                                                                  \
            (shared) = 0;                                                                    \
            if (zassetFindAsset(g_zprimLib, (id) & 0xFFFF, 0) != 1) {                        \
                g_pfnDebugPrintf(findMsg, (id) & 0xFFFF);                                    \
                return 0;                                                                    \
            }                                                                                \
            size_ = zassetGetAssetSize(g_zprimLib);                                          \
            (handle) = zmemDMMAlloc(size_);                                                  \
            if ((handle) == 0)                                                               \
                goto allocFail;                                                              \
            zassetLoadAsset(g_zprimLib, zmemDMMLock(handle));                                \
            zmemDMMUnlock(handle);                                                           \
        } else {                                                                             \
            (shared) = 1;                                                                    \
            if (g_->flags & ZPRIM_ASSET_KEEP)                                                \
                (keep) = 1;                                                                  \
            (handle) = g_->handle;                                                           \
            (offset) = g_->offset;                                                           \
        }                                                                                    \
    } while (0)

/* VA 0x00479010, 328 bytes, [I] row, status draft */
ZprimAsset *zprimLoadPicture(uint32_t id, int keep)
{
    ZprimAsset *a;
    ZprimImage *img;
    int shared, handle;
    uint32_t offset;

    if (id & ZPRIM_TYPE_MASK)
        return 0;
    if ((a = zprimFindAsset((int16_t)id)) != 0)
        return a;
    LOAD_DATA(id, keep, shared, handle, offset, "ZPRIM: Failed to find picture %x\n", fail);
    a = zprimAllocAsset();
    if (a == 0)
        goto fail;
    a->flags = keep ? ZPRIM_ASSET_KEEP : 0;
    if (shared)
        a->flags |= ZPRIM_ASSET_SHARED;
    a->id = (uint16_t)id;
    a->handle = handle;
    a->offset = offset;
    img = (ZprimImage *)zprimLockAsset(a);
    img->frame[0].material = zprimMakeMaterial(img->width, img->height, img->bpp,
                                               (const uint8_t *)img + img->pixels,
                                               (const ZprimPalette *)((const uint8_t *)img + img->palette));
    zprimUnlockAsset(a);
    return a;
fail:
    g_pfnDebugPrintf("ZPRIM: Failed to allocate picture\n");
    return 0;
}

/* VA 0x004791c0, 12 bytes, [I] row, status draft */
void zprimFreePicture(ZprimAsset *picture)
{
    /* DM-064 created; no caller, thunk 0x0040164A */
    zprimFreeAsset(picture);
}

/* VA 0x004791e0, 31 bytes, [I] row, status draft */
uint16_t zprimGetPictureWidth(ZprimAsset *picture)
{
    /* DM-064 created; no caller, thunk 0x00402356 */
    uint16_t w = ((ZprimImage *)zprimLockAsset(picture))->width;

    zprimUnlockAsset(picture);
    return w;
}

/* VA 0x00479210, 31 bytes, [I] row, status draft */
uint16_t zprimGetPictureHeight(ZprimAsset *picture)
{
    /* DM-064 created; no caller, thunk 0x0040201D */
    uint16_t h = ((ZprimImage *)zprimLockAsset(picture))->height;

    zprimUnlockAsset(picture);
    return h;
}

/* VA 0x00479240, 1 bytes, [I] row, status draft */
void zprimUnusedStub(void)
{
    /* a bare ret, never referenced (its /INCREMENTAL thunk 0x00401C08 has no caller); created by DM-061 */
}

/* VA 0x00479260, 1 bytes, [I] row, status draft */
void zprimCopyToPicture(ZprimAsset *sprite, ZprimAsset *picture, uint32_t x, uint32_t y)
{
    /* a bare ret on PC: the PSX build copied a sprite into a picture's VRAM area here [I]. Only
     * the unused guiDrawSpriteToBackdrop calls it; created by DM-061 (DM-056 discovered work) */
}

/* dst's material becomes a copy of src's. */
/* VA 0x00479280, 88 bytes, [I] row, status draft */
void zprimCopyMaterial(ZprimAsset *src, ZprimAsset *dst)
{
    ZprimImage *s = (ZprimImage *)zprimLockAsset(src);
    ZprimImage *d = (ZprimImage *)zprimLockAsset(dst);

    if (d->frame[0].material != 0) {
        g_pfnZglMatDestroy(d->frame[0].material);
        d->frame[0].material = 0;
    }
    d->frame[0].material = g_pfnZglMatClone(s->frame[0].material);
    zprimUnlockAsset(src);
    zprimUnlockAsset(dst);
}

/* dst's material = src's pixels with src's palette in grey, scaled by level / 256 (the transparent
 * colour 0x7C1F stays). */
/* VA 0x00479300, 391 bytes, [V] direct, status draft */
void zprimReplaceVRAMGrey(ZprimAsset *src, ZprimAsset *dst, uint32_t level)
{
    uint8_t buf[0x1000];
    ZprimPalette *pal = (ZprimPalette *)buf;
    ZprimImage *s = (ZprimImage *)zprimLockAsset(src);
    ZprimImage *d = (ZprimImage *)zprimLockAsset(dst);
    const ZprimPalette *sp = (const ZprimPalette *)((const uint8_t *)s + s->palette);
    uint16_t i;

    if ((uint32_t)sp->count * 2 + 4 >= sizeof buf)
        zAssertFail("Buffer overflow in zprimReplaceVRAMGrey()", ZPRIM_FILE, 0x7C4, 0);
    zstringMemCopy(buf, sp, sp->count * 2 + 4);
    for (i = 0; i < sp->count; i++) {
        uint16_t c = pal->colour[i];
        uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;

        if (r == 0x1F && g == 0 && b == 0x1F)
            continue;
        r = ((b + g + r) * (level & 0xFF) / 0x300) & 0x1F;
        pal->colour[i] = (uint16_t)((r << 5 | r) << 5 | r);
    }
    if (d->frame[0].material != 0) {
        g_pfnZglMatDestroy(d->frame[0].material);
        d->frame[0].material = 0;
    }
    d->frame[0].material = zprimMakeMaterial(s->width, s->height, s->bpp,
                                             (const uint8_t *)s + s->frame[0].pixels, pal);
    zprimUnlockAsset(src);
    zprimUnlockAsset(dst);
}

/* dst's material = src's pixels with colours 0-15 taken from 16-colour bank `bank` of src's palette.
 * The assert text is zprimReplaceVRAMGrey's (copied code). */
/* VA 0x00479500, 269 bytes, [V] direct, status draft */
void zprimSelectPaletteBank(ZprimAsset *src, ZprimAsset *dst, uint32_t bank)
{
    uint8_t buf[0x1000];
    ZprimPalette *pal = (ZprimPalette *)buf;
    ZprimImage *s = (ZprimImage *)zprimLockAsset(src);
    ZprimImage *d = (ZprimImage *)zprimLockAsset(dst);
    const ZprimPalette *sp = (const ZprimPalette *)((const uint8_t *)s + s->palette);
    int i;

    if ((uint32_t)sp->count * 2 + 4 >= sizeof buf)
        zAssertFail("Buffer overflow in zprimReplaceVRAMGrey()", ZPRIM_FILE, 0x7F8, 0);
    zstringMemCopy(buf, sp, sp->count * 2 + 4);
    for (i = 0; i < 16; i++)
        pal->colour[i] = pal->colour[(bank & 0xFF) * 16 + i];
    if (d->frame[0].material != 0) {
        g_pfnZglMatDestroy(d->frame[0].material);
        d->frame[0].material = 0;
    }
    d->frame[0].material = zprimMakeMaterial(s->width, s->height, s->bpp,
                                             (const uint8_t *)s + s->frame[0].pixels, pal);
    zprimUnlockAsset(src);
    zprimUnlockAsset(dst);
}

/* VA 0x00479660, 423 bytes, [I] row, status draft */
ZprimAsset *zprimLoadSprite(uint32_t id, int keep)
{
    ZprimAsset *a;
    ZprimImage *img;
    int shared, handle, i;
    uint32_t offset, type = id & ZPRIM_TYPE_MASK;

    if (type != ZPRIM_TYPE_SPRITE && type != ZPRIM_TYPE_SPRITE2 && type != ZPRIM_TYPE_VSPRITE)
        return 0;
    if ((a = zprimFindAsset((int16_t)id)) != 0)
        return a;
    LOAD_DATA(id, keep, shared, handle, offset, "ZPRIM: Failed to find sprite %x\n", fail);
    a = zprimAllocAsset();
    if (a == 0)
        goto fail;
    a->flags = keep ? ZPRIM_ASSET_KEEP : 0;
    if (shared)
        a->flags |= ZPRIM_ASSET_SHARED;
    a->id = (uint16_t)id;
    a->handle = handle;
    a->offset = offset;
    img = (ZprimImage *)zprimLockAsset(a);
    if (img->palette != 0) {
        /* one material per frame, all with the first palette. [V] The loop is wrapped in a second
         * one over the palettes (1, or count with per-frame palettes) whose condition is false after
         * the first pass, so per-frame palettes are never used here. */
        for (i = 0; i < img->count; i++)
            img->frame[i].material = zprimMakeMaterial(img->frame[i].width, img->frame[i].height, img->bpp,
                                                       (const uint8_t *)img + img->frame[i].pixels,
                                                       (const ZprimPalette *)((const uint8_t *)img + img->palette));
    }
    zprimUnlockAsset(a);
    return a;
fail:
    g_pfnDebugPrintf("ZPRIM: Failed to allocate sprite\n");
    return 0;
}

/* An empty 8-bit "virtual sprite" of `frames` frames with a 256-colour grey-ramp palette (data
 * only, no materials). Unreferenced. */
/* VA 0x00479880, 390 bytes, [I] row, status draft */
ZprimAsset *zprimCreateSprite(uint16_t width, uint16_t height, uint16_t frames)
{
    uint32_t frameBytes = (uint32_t)width * height;
    uint32_t pixels = frames * sizeof(ZprimFrame) + 0x14;
    uint32_t size = (frameBytes + 0x10) * frames + 0x318;
    uint32_t palette = frames * frameBytes + pixels;
    ZprimAsset *a;
    ZprimImage *img;
    ZprimPalette *pal;
    int handle, i;

    handle = zmemDMMAlloc(size);
    if (handle == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate buffer in CreateSprite\n");
        return 0;
    }
    a = zprimAllocAsset();
    if (a == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate asset handle\n");
        return 0;
    }
    a->flags = ZPRIM_ASSET_CREATED;
    a->id = g_zprimNextId++;
    a->handle = handle;
    a->offset = 0;
    img = (ZprimImage *)zmemDMMLock(handle);
    zstringMemSet(img, 0, size);
    img->count = frames;
    img->flags = 0;
    img->palette = palette;
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    for (i = 0; i < frames; i++) {
        img->frame[i].width = width;
        img->frame[i].height = height;
        img->frame[i].pixels = pixels;
        img->frame[i].hotX = 0;
        img->frame[i].hotY = 0;
        pixels += frameBytes;
    }
    pal = (ZprimPalette *)((uint8_t *)img + palette);
    pal->unk_00 = 0;
    pal->count = 0x100;
    for (i = 0; i < 0x100; i++)
        pal->colour[i] = (uint16_t)((((i & 0xFFF8) | 0xFF00) << 5 | (i & 0xFFF8)) << 2 | (i >> 3));
    zmemDMMUnlock(handle);
    return a;
}

/* A one-frame image header with an empty palette of `colours` entries (0x8000) and no pixels. */
/* VA 0x00479a80, 28 bytes, [I] row, status draft */
void zprimTouchAsset(ZprimAsset *asset)
{
    /* (DM-064 created; no caller, thunk 0x00402117) Locks and unlocks the asset's DMM block without
     * touching it (a body compiled out [I]). */
    zmemDMMLock(asset->handle);
    zmemDMMUnlock(asset->handle);
}

/* VA 0x00479ab0, 169 bytes, [I] row, status draft */
void zprimResetImage(ZprimAsset *asset, uint16_t width, uint16_t height, uint16_t bpp, uint16_t colours)
{
    /* (DM-064 created; no caller, thunk 0x0040168B) Rewrites a created image as one blank frame of
     * width x height with a palette of `colours` entries (0x8000 each) right after the frame table.
     * Like the exe, the palette is written where the *old* palette offset pointed; the new offset is
     * 0x24. */
    ZprimImage *img = (ZprimImage *)zmemDMMLock(asset->handle);
    ZprimPalette *pal = (ZprimPalette *)((uint8_t *)img + img->palette);   /* sic: before the update */
    unsigned i;

    img->count = 1;
    img->flags = 0;
    img->width = width;
    img->height = height;
    img->pixels = 0;
    img->palette = 0x24;
    img->bpp = bpp;
    img->frame[0].width = width;
    img->frame[0].height = height;
    img->frame[0].pixels = 0;
    img->frame[0].hotX = 0;
    img->frame[0].hotY = 0;
    if (img->frame[0].material != 0) {
        g_pfnZglMatDestroy(img->frame[0].material);     /* 0x00561024 material release */
        img->frame[0].material = 0;
    }
    pal->unk_00 = 0;
    pal->count = colours;
    for (i = 0; i < colours; i++)
        pal->colour[i] = 0x8000;
    zmemDMMUnlock(asset->handle);
}

/* VA 0x00479b90, 247 bytes, [I] row, status draft */
ZprimAsset *zprimCreateImage(uint16_t width, uint16_t height, uint16_t bpp, uint16_t colours)
{
    ZprimAsset *a;
    ZprimImage *img;
    ZprimPalette *pal;
    int handle, i;

    handle = zmemDMMAlloc(0x328);
    if (handle == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate buffer in CreateSprite\n");
        return 0;
    }
    a = zprimAllocAsset();
    if (a == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate asset handle\n");
        return 0;
    }
    a->flags = ZPRIM_ASSET_CREATED;
    a->id = g_zprimNextId++;
    a->handle = handle;
    a->offset = 0;
    img = (ZprimImage *)zmemDMMLock(handle);
    zstringMemSet(img, 0, 0x328);
    img->count = 1;
    img->flags = 0;
    img->width = width;
    img->height = height;
    img->pixels = 0;
    img->palette = 0x24;
    img->bpp = bpp;
    img->frame[0].width = width;
    img->frame[0].height = height;
    img->frame[0].pixels = 0;
    img->frame[0].hotX = img->frame[0].hotY = 0;
    img->frame[0].material = 0;
    pal = (ZprimPalette *)((uint8_t *)img + 0x24);
    pal->unk_00 = 0;
    pal->count = colours;
    for (i = 0; i < colours; i++)
        pal->colour[i] = 0x8000;
    zmemDMMUnlock(handle);
    return a;
}

/* VA 0x00479cd0, 12 bytes, [I] row, status draft */
void zprimFree(ZprimAsset *asset)
{
    zprimFreeAsset(asset);
}

/* Draw frame `frame` of the current sprite at (x, y) (minus the frame's hot spot unless the sprite
 * is ZPRIM_TYPE_SPRITE2). */
/* VA 0x00479cf0, 536 bytes, [I] row, status draft */
void zprimDrawSprite(uint32_t frame, int16_t x, int16_t y)
{
    int held;
    ZprimState *st;
    ZprimImage *img;
    const ZprimFrame *fr;
    const uint8_t *pal = 0;
    uint16_t f = (uint16_t)frame;
    int mode;
    uint32_t arg = 0;

    if (f == 0xFFFF)
        return;
    STATE_LOCK(held);
    st = CUR_STATE();
    if (st->sprite == 0) {
        STATE_UNLOCK(held);
        return;
    }
    img = (ZprimImage *)zprimLockAsset(st->sprite);
    if (f >= img->count) {
        zprimUnlockAsset(st->sprite);
        STATE_UNLOCK(held);
        return;
    }
    fr = &img->frame[f];
    if ((st->sprite->id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_SPRITE2) {
        x = (int16_t)(x - fr->hotX);
        y = (int16_t)(y - fr->hotY);
    }
    mode = DRAW_MODE(st->spriteFlags);
    if (mode == 2 && !(st->spriteFlags & ZPRIM_MODE_3))
        arg = st->spriteArg20;
    if (img->palette != 0)
        pal = (const uint8_t *)img + img->palette + ((img->flags & 1) ? f * 0x204 : 0);
    if (st->clut34 != 0)
        psx2DSetTint(st->clut38, st->clut34);
    psx2DDrawSprite(img, frame, pal, mode, 0, arg, st->spriteArg24, x, y, fr->width, fr->height, img->bpp,
                 st->spriteArg30);
    if (st->clut34 != 0)
        psx2DSetTint(0, 0);
    zprimUnlockAsset(st->sprite);
    STATE_UNLOCK(held);
}

/* VA 0x00479fa0, 150 bytes, [I] row, status draft */
uint16_t zprimGetSpriteWidth(ZprimAsset *sprite, uint16_t frame)
{
    int held;
    uint16_t w = 0;

    STATE_LOCK(held);
    if (sprite != 0 || (sprite = CUR_STATE()->sprite) != 0) {
        ZprimImage *img = (ZprimImage *)zprimLockAsset(sprite);
        if (frame < img->count)
            w = img->frame[frame].width;
        zprimUnlockAsset(sprite);
    }
    STATE_UNLOCK(held);
    return w;
}

/* VA 0x0047a070, 150 bytes, [I] row, status draft */
uint16_t zprimGetSpriteHeight(ZprimAsset *sprite, uint16_t frame)
{
    int held;
    uint16_t h = 0;

    STATE_LOCK(held);
    if (sprite != 0 || (sprite = CUR_STATE()->sprite) != 0) {
        ZprimImage *img = (ZprimImage *)zprimLockAsset(sprite);
        if (frame < img->count)
            h = img->frame[frame].height;
        zprimUnlockAsset(sprite);
    }
    STATE_UNLOCK(held);
    return h;
}

/* VA 0x0047a140, 127 bytes, [I] row, status draft */
uint16_t zprimGetSpriteFrames(ZprimAsset *sprite)
{
    /* (DM-064 created; no caller, thunk 0x00401B72) Frame count of a sprite (NULL = the current
     * sprite; 0 if there is none). */
    int held;
    uint16_t n = 0;

    STATE_LOCK(held);
    if (sprite == 0)
        sprite = CUR_STATE()->sprite;
    if (sprite != 0) {
        n = ((ZprimImage *)zprimLockAsset(sprite))->count;
        zprimUnlockAsset(sprite);
    }
    STATE_UNLOCK(held);
    return n;
}

/* VA 0x0047a1e0, 211 bytes, [I] row, status draft */
void zprimDecompressFrame(ZprimAsset *src, uint16_t srcFrame, ZprimAsset *dst, uint16_t dstFrame)
{
    /* (DM-064 created; no caller, thunk 0x00401744) Decompresses frame srcFrame of a
     * ZPRIM_TYPE_SPRITE2 image into frame dstFrame of a virtual sprite (zprimRleDecode). */
    ZprimImage *s = (ZprimImage *)zprimLockAsset(src);
    ZprimImage *d = (ZprimImage *)zprimLockAsset(dst);

    if ((src->id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_SPRITE2 || (dst->id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_VSPRITE) {
        g_pfnDebugPrintf("ZPRIM: Error - can only decompress from compressed sprite to virtual sprite (%d->%d)\n",
                         src->id, dst->id);
    } else if (srcFrame < s->count && dstFrame < d->count) {
        zprimRleDecode((const uint8_t *)s + s->frame[srcFrame].pixels, (uint8_t *)d + d->frame[dstFrame].pixels);
    } else {
        g_pfnDebugPrintf("ZPRIM: Error - frame out of bounds during decompress\n");
    }
    zprimUnlockAsset(src);
    zprimUnlockAsset(dst);
}

/* VA 0x0047a300, 31 bytes, [I] row, status draft */
void *zprimGetFramePixels(ZprimImage *img, uint16_t frame)
{
    /* (DM-064 created; no caller, thunk 0x0040177B): the pixels of one frame of a locked image, NULL
     * past the last frame */
    if (frame < img->count)
        return (uint8_t *)img + img->frame[frame].pixels;
    return 0;
}

/* VA 0x0047a330, 189 bytes, [I] row, status draft */
int zprimRleDecode(const uint8_t *src, uint8_t *dst)
{
    /* (DM-064 created; zprimDecompressFrame's helper) The SPRITE2 pixel run-length format: u16 total
     * size, then records 0x80 v n16 n copies of byte v 0x00 n16 n zero bytes 0x81..0xFF v (op - 0x80)
     * copies of v 0x01..0x7F literal: that many bytes follow Returns the bytes written. */
    const uint8_t *end = src + *(const uint16_t *)src;
    const uint8_t *p = src + 2;
    uint8_t *out = dst;

    while (p < end) {
        uint8_t op = p[0];

        if (op == 0x80) {
            uint16_t n = *(const uint16_t *)(p + 2);
            zstringMemSet(out, p[1], n);
            out += n;
            p += 4;
        } else if (op == 0) {
            uint16_t n = (uint16_t)(p[1] | (p[2] << 8));
            zstringMemSet(out, 0, n);
            out += n;
            p += 3;
        } else if ((op & 0x80) == 0) {
            zstringMemCopy(out, p + 1, op);
            out += op;
            p += op + 1;
        } else {
            zstringMemSet(out, p[1], op - 0x80);
            out += op - 0x80;
            p += 2;
        }
    }
    return (int)(out - dst);
}

/* VA 0x0047a420, 346 bytes, [V] direct, status draft */
ZprimAsset *zprimLoadFont(uint32_t id, int keep)
{
    ZprimAsset *a;
    ZprimFont *font;
    int shared, handle;
    uint32_t offset;

    if ((id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_FONT)
        return 0;
    if ((a = zprimFindAsset((int16_t)id)) != 0)
        return a;
    LOAD_DATA(id, keep, shared, handle, offset, "ZPRIM: Failed to find font %x\n", nomem);
    a = zprimAllocAsset();
    if (a == 0) {
        zAssertFail("Possible memory leak of fntMemHdl here.", ZPRIM_FILE, 0xD1B, 0);
        goto nomem;
    }
    a->flags = keep ? ZPRIM_ASSET_KEEP : 0;
    if (shared)
        a->flags |= ZPRIM_ASSET_SHARED;
    a->id = (uint16_t)id;
    a->handle = handle;
    a->offset = offset;
    font = (ZprimFont *)zprimLockAsset(a);
    font->materials = 0;
    zprimUnlockAsset(a);
    return a;
nomem:
    g_pfnDebugPrintf("ZPRIM: Failed to allocate font\n");
    return 0;
}

/* ------------------------------------------------------------------ text */

/* The glyph of character c: indexed from glyph[0].code (count < 0x100) or searched by code; a
 * lower-case letter without a glyph falls back to upper case. NULL: no glyph. Not a function of
 * its own in the exe: inlined in zprimDrawTextFixed, zprimDrawText and zprimGetTextWidth. */
static const ZprimGlyph *zprimFindGlyph(const ZprimFont *font, uint32_t c, uint32_t first)
{
    for (;;) {
        if (font->count >= 0x100) {
            int i;
            for (i = 0; i < font->count; i++)
                if (font->glyph[i].code == c)
                    return &font->glyph[i];
        } else if ((int)c >= (int)first && (int)c <= (int)(font->count + first)) {
            return &font->glyph[c - first];        /* [V] accepts c == first + count, one past */
        }
        if ((int)c < 'a' || (int)c > 'z')
            return 0;
        c -= 0x20;
    }
}

/* Decode one character: bytes > 2 are themselves; [V] the escape byte 1 (code
 * (b1 - 2) * 254 - 2 + b2) is unreachable because the callers stop at any byte <= 2. */
#define NEXT_CHAR(s, c) ((*(s) == 1) ? ((s) += 2, (c) = ((s)[-1] - 2) * 0xFE - 2 + (s)[0]) : ((c) = *(s)))

/* Text with a fixed advance (ZprimState.spacing) per character. */
/* VA 0x0047a5e0, 12 bytes, [I] row, status draft */
void zprimFreeFont(ZprimAsset *font)
{
    /* DM-064 created; no caller, thunk 0x00401654 */
    zprimFreeAsset(font);
}

/* VA 0x0047a600, 1 bytes, [I] row, status draft */
void zprimFontStub(void)
{
    /* DM-064 created: a bare ret, thunk 0x0040254F */
}

/* VA 0x0047a620, 849 bytes, [I] row, status draft */
void zprimDrawTextFixed(const uint8_t *s, int x, int16_t y)
{
    int held;
    ZprimState *st;
    ZprimFont *font;
    uint32_t first = 0, c;

    STATE_LOCK(held);
    st = CUR_STATE();
    if (st->font == 0) {
        STATE_UNLOCK(held);
        return;
    }
    font = (ZprimFont *)zprimLockAsset(st->font);
    if (font->count < 0x100)
        first = font->glyph[0].code;
    for (; *s > 2 && (int16_t)x < g_zprimClip[2]; s++) {
        const ZprimGlyph *g;
        int mode;
        uint32_t arg1 = 0, arg2 = 0;

        NEXT_CHAR(s, c);
        g = zprimFindGlyph(font, c, first);
        if (g == 0)
            continue;
        if (!(st->fontFlags & ZPRIM_MODE_3)) {
            /* background box behind the glyph, clipped */
            int16_t w = (int16_t)(g->advance + g->w + g->xoff);
            int16_t h = (int16_t)(g->descent + g->h + g->yoff);
            if (g->w < w && g->h < h) {
                int16_t bx = (int16_t)x, by = y;
                if (bx < g_zprimClip[0]) { w = (int16_t)(w + (bx - g_zprimClip[0])); bx = g_zprimClip[0]; }
                if (g_zprimClip[2] < w + bx) w = (int16_t)(g_zprimClip[2] - bx);
                if (by < g_zprimClip[1]) { h = (int16_t)(h + (by - g_zprimClip[1])); by = g_zprimClip[1]; }
                if (g_zprimClip[3] < h + by) h = (int16_t)(g_zprimClip[3] - by);
                if (w > 0 && h > 0)
                    psx2DFillRect(st->textBg, 0, bx, by, bx + w, by + h);
            }
        }
        mode = DRAW_MODE(st->fontFlags);
        if (mode == 2) {
            arg1 = st->fontArg48;
            if (!(st->fontFlags & ZPRIM_MODE_3))
                arg2 = st->textBg;
        }
        psx2DDrawGlyph(font, g, mode, arg1, arg2, g->xoff + (int16_t)x, g->yoff + y, font->bpp, st->fontArg40);
        x += (uint16_t)st->spacing;
    }
    zprimUnlockAsset(st->font);
    STATE_UNLOCK(held);
}

/* Proportional text: each character advances by xoff + w + advance + spacing. */
/* VA 0x0047aa60, 1 bytes, [I] row, status draft */
void zprimTextStub(void)
{
    /* DM-064 created: a bare ret, thunk 0x00401410 */
}

/* VA 0x0047aa80, 922 bytes, [I] row, status draft */
void zprimDrawText(const uint8_t *s, int x, int16_t y)
{
    int held;
    ZprimState *st;
    ZprimFont *font;
    uint32_t first = 0, c;

    STATE_LOCK(held);
    st = CUR_STATE();
    if (st->font == 0) {
        STATE_UNLOCK(held);
        return;
    }
    font = (ZprimFont *)zprimLockAsset(st->font);
    if (font->count < 0x100)
        first = font->glyph[0].code;
    if ((st->fontFlags & ZPRIM_MODE_1) && st->clut34 != 0)
        psx2DSetTint(st->clut38, st->clut34);
    for (; *s > 2 && (int16_t)x < g_zprimClip[2]; s++) {
        const ZprimGlyph *g;
        int mode;
        uint32_t arg1 = 0, arg2 = 0;

        NEXT_CHAR(s, c);
        g = zprimFindGlyph(font, c, first);
        if (g == 0)
            continue;
        if (!(st->fontFlags & ZPRIM_MODE_3)) {
            int16_t w = (int16_t)(g->advance + g->w + g->xoff);
            int16_t h = (int16_t)(g->descent + g->h + g->yoff);
            if (g->w < w && g->h < h) {
                int16_t bx = (int16_t)x, by = y;
                if (bx < g_zprimClip[0]) { w = (int16_t)(w + (bx - g_zprimClip[0])); bx = g_zprimClip[0]; }
                if (g_zprimClip[2] < w + bx) w = (int16_t)(g_zprimClip[2] - bx);
                if (by < g_zprimClip[1]) { h = (int16_t)(h + (by - g_zprimClip[1])); by = g_zprimClip[1]; }
                if (g_zprimClip[3] < h + by) h = (int16_t)(g_zprimClip[3] - by);
                if (w > 0 && h > 0)
                    psx2DFillRect(st->textBg, 0, bx, by, bx + w, by + h);
            }
        }
        mode = DRAW_MODE(st->fontFlags);
        if (mode == 2) {
            arg1 = st->fontArg48;
            if (!(st->fontFlags & ZPRIM_MODE_3))
                arg2 = st->textBg;
        }
        psx2DDrawGlyph(font, g, mode, arg1, arg2, g->xoff + (int16_t)x, g->yoff + y, font->bpp, st->fontArg40);
        x += (uint16_t)(g->advance + g->w + (int16_t)st->spacing) + (uint16_t)(int16_t)g->xoff;
    }
    if ((st->fontFlags & ZPRIM_MODE_1) && st->clut34 != 0)
        psx2DSetTint(0, 0);
    zprimUnlockAsset(st->font);
    STATE_UNLOCK(held);
}

/* VA 0x0047af10, 135 bytes, [I] row, status draft */
uint16_t zprimGetFontHeight(ZprimAsset *font)
{
    /* The font's line height (ZprimFont +0x02): the GUI centres text and advances lines by it. */
    int held;
    ZprimFont *f;
    uint16_t height;

    STATE_LOCK(held);
    if (font == 0)
        font = CUR_STATE()->font;
    f = (ZprimFont *)zprimLockAsset(font);
    if (f == 0)
        return 0;                       /* [V] leaves the state block locked */
    height = f->lineHeight;
    zprimUnlockAsset(font);
    STATE_UNLOCK(held);
    return height;
}

/* Width zprimDrawText would advance over `s`. */
/* VA 0x0047afd0, 449 bytes, [I] row, status draft */
int16_t zprimGetTextWidth(ZprimAsset *font, const uint8_t *s)
{
    int held;
    ZprimState *st;
    ZprimFont *f;
    uint32_t first = 0, c;
    int16_t w = 0;

    STATE_LOCK(held);
    st = CUR_STATE();
    if (font == 0)
        font = st->font;
    f = (ZprimFont *)zprimLockAsset(font);
    if (f->count < 0x100)
        first = f->glyph[0].code;
    for (; *s > 2; s++) {
        const ZprimGlyph *g;

        NEXT_CHAR(s, c);
        g = zprimFindGlyph(f, c, first);
        if (g != 0)
            w = (int16_t)(w + g->advance + g->xoff + g->w + (int16_t)st->spacing);
    }
    zprimUnlockAsset(font);
    STATE_UNLOCK(held);
    return w;
}

/* ------------------------------------------------------------------ groups */

/* A copy of one picture of a group (kept: ZPRIM_ASSET_KEEP), the group itself is unloaded again.
 * A group = u32 count, then count x {u32 id, u32 offset}. */
/* VA 0x0047b220, 323 bytes, [I] row, status draft */
ZprimAsset *zprimLoadGroupImage(uint32_t groupId, uint16_t imageId)
{
    ZprimAsset *a = 0;
    const uint32_t *group, *e;
    uint32_t i, size;
    int handle, h2;

    if (zassetFindAsset(g_zprimLib, groupId & 0xFFFF, 0) != 1) {
        g_pfnDebugPrintf("ZPRIM: Failed to find group\n");
        return 0;
    }
    handle = zmemDMMAlloc(zassetGetAssetSize(g_zprimLib));
    if (handle == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate group\n");
        return 0;
    }
    group = (const uint32_t *)zmemDMMLock(handle);
    zassetLoadAsset(g_zprimLib, (void *)group);
    for (i = 0, e = group + 1; i < group[0]; i++, e += 2) {
        if (e[0] == imageId && (e[0] & ZPRIM_TYPE_MASK) == 0) {
            const ZprimImage *img = (const ZprimImage *)((const uint8_t *)group + e[1]);
            if (img->count == 1) {
                size = (uint32_t)img->height * img->width;
                if (img->palette != 0)
                    size += 0x204;
                size += 0x24;
                h2 = zmemDMMAlloc(size);
                if (h2 != 0 && (a = zprimAllocAsset()) != 0) {
                    ZprimImage *copy = (ZprimImage *)zmemDMMLock(h2);
                    zstringMemCopy(copy, img, size);
                    copy->frame[0].material = 0;
                    zmemDMMUnlock(h2);
                    a->flags = ZPRIM_ASSET_KEEP;
                    a->id = imageId;
                    a->handle = h2;
                    a->offset = 0;
                }
            }
            break;
        }
    }
    zmemDMMUnlock(handle);
    zmemDMMDealloc(handle);
    return a;
}

/* VA 0x0047b3d0, 26 bytes, [I] row, status draft */
int zprimGroupLoaded(uint32_t slot)
{
    return g_zprimGroups[slot & 0xFFFF] != 0;
}

/* Load group `groupId` into slot 0..3 (retrying until it loads) and every member asset. */
/* VA 0x0047b400, 337 bytes, [I] row, status draft */
int zprimLoadGroup(uint16_t slot, uint16_t groupId)
{
    uint32_t *data = 0;
    const uint32_t *e;
    ZprimAsset *a;
    int handle = 0;
    uint32_t i;

    if (slot > 3) {
        g_pfnDebugPrintf("ZPRIM: Error - bad slot for group\n");
        return 1;
    }
    if (g_zprimGroups[slot] != 0) {
        g_pfnDebugPrintf("ZPRIM: Error - loading group into used slot\n");
        return 1;
    }
    for (;;) {
        while (zassetFindAsset(g_zprimLib, groupId, 0) != 1)
            printf("ZPRIM: Failed to find group\n");
        if (handle == 0) {
            handle = zmemDMMAlloc(zassetGetAssetSize(g_zprimLib));
            if (handle == 0) {
                printf("ZPRIM: Failed to allocate group\n");
                continue;
            }
            data = (uint32_t *)zmemDMMLock(handle);
        }
        if (zassetLoadAsset(g_zprimLib, data) == 1)
            break;
        printf("ZPRIM: Load Failed\n");
    }
    a = zprimAllocAsset();
    if (a == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate group\n");
        return 1;
    }
    a->flags = ZPRIM_ASSET_KEEP;
    a->id = groupId;
    a->handle = handle;
    a->offset = 0;
    g_zprimGroups[slot] = a;
    zmemDMMUnlock(handle);
    for (i = 0, e = data + 1; i < data[0]; i++, e += 2)
        zprimLoadAsset((int16_t)e[0]);
    return 0;
}

/* VA 0x0047b5c0, 183 bytes, [I] row, status draft */
void zprimUnloadGroup(uint16_t slot)
{
    ZprimAsset *g, *a;
    const uint32_t *data;
    uint32_t i;

    if (slot >= ZPRIM_MAX_GROUPS || (g = g_zprimGroups[slot]) == 0)
        return;
    data = (const uint32_t *)zmemDMMLock(g->handle);
    for (i = 0; i < data[0]; i++) {
        a = zprimFindAsset((int16_t)data[1 + i * 2]);
        if (a != 0 && (a->flags & ZPRIM_ASSET_SHARED)) {
            zmemDMMUnlock(g->handle);
            zprimFreeAsset(a);
            data = (const uint32_t *)zmemDMMLock(g->handle);
        }
    }
    zmemDMMUnlock(g->handle);
    zmemDMMDealloc(g->handle);
    g->handle = 0;
    zprimReleaseAsset(g);
    g_zprimGroups[slot] = 0;
}

/* ------------------------------------------------------------------ asset handles */

/* Destroy the asset's materials, free its data (unless shared) and return the handle. */
/* VA 0x0047b6b0, 302 bytes, [I] row, status draft */
void zprimFreeAsset(ZprimAsset *asset)
{
    uint16_t type = asset->id & ZPRIM_TYPE_MASK;
    int i;

    if (type == ZPRIM_TYPE_PICTURE) {
        ZprimImage *img = (ZprimImage *)zprimLockAsset(asset);
        if (img->frame[0].material != 0)
            g_pfnZglMatDestroy(img->frame[0].material);
        zprimUnlockAsset(asset);
    } else if (type == ZPRIM_TYPE_SPRITE || type == ZPRIM_TYPE_SPRITE2 || type == ZPRIM_TYPE_VSPRITE) {
        ZprimImage *img = (ZprimImage *)zprimLockAsset(asset);
        for (i = img->count - 1; i >= 0; i--) {
            if (img->frame[i].material != 0)
                g_pfnZglMatDestroy(img->frame[i].material);
            img->frame[i].material = 0;
        }
        zprimUnlockAsset(asset);
    } else if (type == ZPRIM_TYPE_FONT) {
        ZprimFont *font = (ZprimFont *)zprimLockAsset(asset);
        if (font->materials != 0) {
            for (i = 0; i < 32; i++) {
                if (font->materials[i] != 0) {
                    g_pfnZglMatDestroy((int32_t)font->materials[i]);
                    font->materials[i] = 0;
                }
            }
            zmemFreeListFree(g_ngFontMaterials, font->materials);
            font->materials = 0;
        }
        zprimUnlockAsset(asset);
    }
    /* screens and groups have no materials */
    if (!(asset->flags & ZPRIM_ASSET_SHARED)) {
        zmemDMMDealloc(asset->handle);
        asset->handle = 0;
    }
    zprimReleaseAsset(asset);
}

/* VA 0x0047b830, 49 bytes, [I] row, status draft */
ZprimAsset *zprimAllocAsset(void)
{
    ZprimAsset *a = g_zprimFreeAssets;

    if (a == 0)
        return 0;
    g_zprimFreeAssets = a->next;
    a->id = 0;
    a->flags = 0;
    a->next = 0;
    a->handle = 0;
    a->offset = 0;
    a->next = g_zprimAssets;
    g_zprimAssets = a;
    return a;
}

/* Unlink from the used list and put back on the free list ([V] even if it was not on the used list). */
/* VA 0x0047b880, 93 bytes, [I] row, status draft */
void zprimReleaseAsset(ZprimAsset *asset)
{
    ZprimAsset *p;

    if (asset == g_zprimAssets) {
        g_zprimAssets = asset->next;
    } else if (g_zprimAssets != 0) {
        for (p = g_zprimAssets; p->next != asset; p = p->next)
            if (p->next == 0)
                goto free;
        p->next = asset->next;
    }
free:
    asset->next = g_zprimFreeAssets;
    g_zprimFreeAssets = asset;
}

/* VA 0x0047b900, 26 bytes, [I] row, status draft */
void *zprimLockAsset(ZprimAsset *asset)
{
    return (uint8_t *)zmemDMMLock(asset->handle) + asset->offset;
}

/* VA 0x0047b930, 15 bytes, [I] row, status draft */
void zprimUnlockAsset(ZprimAsset *asset)
{
    zmemDMMUnlock(asset->handle);
}

/* The asset `id`, loading it by its type if needed. */
/* VA 0x0047b950, 139 bytes, [V] direct, status draft */
ZprimAsset *zprimLoadAsset(uint32_t id)
{
    ZprimAsset *a = zprimFindAsset((int16_t)id);
    uint16_t type = (uint16_t)id & ZPRIM_TYPE_MASK;

    if (a != 0)
        return a;
    if (type == ZPRIM_TYPE_PICTURE)
        return zprimLoadPicture(id, 0);
    if (type == ZPRIM_TYPE_SPRITE || type == ZPRIM_TYPE_SPRITE2 || type == ZPRIM_TYPE_VSPRITE)
        return zprimLoadSprite(id, 0);
    if (type == ZPRIM_TYPE_FONT)
        return zprimLoadFont(id, 0);
    if (type == ZPRIM_TYPE_SCREEN)
        return zprimLoadScreen(id, 0);   /* [V] called with one argument: keep is stack garbage */
    zAssertFail("ZPRIM: Failed to load asset", ZPRIM_FILE, 0x14B2, 0);
    return 0;
}

/* Number of members of the group in `slot` (low 16 bits of its count). Unreferenced. */
/* VA 0x0047ba10, 47 bytes, [I] row, status draft */
uint16_t zprimGetGroupCount(uint32_t slot)
{
    ZprimAsset *g = g_zprimGroups[slot & 0xFFFF];
    const uint16_t *data = (const uint16_t *)zmemDMMLock(g->handle);

    zmemDMMUnlock(g->handle);
    return data[0];
}

/* Point the group handle in `slot` at member `index` and return it. Unreferenced. */
/* VA 0x0047ba50, 65 bytes, [I] row, status draft */
ZprimAsset *zprimSelectGroupEntry(uint32_t slot, uint32_t index)
{
    ZprimAsset *g = g_zprimGroups[slot & 0xFFFF];
    const uint8_t *data = (const uint8_t *)zmemDMMLock(g->handle);

    zmemDMMUnlock(g->handle);
    g->offset = *(const uint32_t *)(data + (index & 0xFFFF) * 8 + 8);
    return g;
}

/* VA 0x0047bac0, 29 bytes, [I] row, status draft */
ZprimAsset *zprimFindAsset(int16_t id)
{
    ZprimAsset *a;

    for (a = g_zprimAssets; a != 0; a = a->next)
        if ((int16_t)a->id == id)
            return a;
    return 0;
}

/* The loaded group that has member `id`, pointed at it (offset), or NULL. */
/* VA 0x0047baf0, 117 bytes, [I] row, status draft */
ZprimAsset *zprimFindInGroups(uint32_t id)
{
    int s;
    uint32_t i;

    for (s = 0; s < ZPRIM_MAX_GROUPS; s++) {
        ZprimAsset *g = g_zprimGroups[s];
        const uint32_t *data;

        if (g == 0)
            continue;
        data = (const uint32_t *)zmemDMMLock(g->handle);
        for (i = 0; i < data[0]; i++) {
            if (data[1 + i * 2] == (id & 0xFFFF)) {
                zmemDMMUnlock(g->handle);
                g->offset = data[2 + i * 2];
                return g;
            }
        }
        zmemDMMUnlock(g->handle);
    }
    return 0;
}

/* VA 0x0047bb90, 277 bytes, [I] row, status draft */
ZprimAsset *zprimLoadScreen(uint32_t id, int keep)
{
    ZprimAsset *a;
    int shared, handle;
    uint32_t offset;

    if ((id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_SCREEN)
        return 0;
    if ((a = zprimFindAsset((int16_t)id)) != 0)
        return a;
    {
        ZprimAsset *g = zprimFindInGroups(id);
        if (g == 0) {
            offset = 0;
            shared = 0;
            if (zassetFindAsset(g_zprimLib, id & 0xFFFF, 0) != 1) {
                g_pfnDebugPrintf("ZPRIM: Failed to find screen\n");
                return 0;
            }
            handle = zmemDMMAlloc(zassetGetAssetSize(g_zprimLib));
            if (handle == 0) {
                g_pfnDebugPrintf("ZPRIM: Failed to allocate picture\n");
                return 0;
            }
            zassetLoadAsset(g_zprimLib, zmemDMMLock(handle));
            zmemDMMUnlock(handle);
        } else {
            handle = g->handle;         /* [V] the group's KEEP flag is not inherited here */
            offset = g->offset;
            shared = 1;
        }
    }
    a = zprimAllocAsset();
    if (a == 0) {
        g_pfnDebugPrintf("ZPRIM: Failed to allocate screen\n");
        return 0;
    }
    a->flags = keep ? ZPRIM_ASSET_KEEP : 0;
    if (shared)
        a->flags |= ZPRIM_ASSET_SHARED;
    a->id = (uint16_t)id;
    a->handle = handle;
    a->offset = offset;
    return a;
}

/* Render `s` with `font` into frame `frame` of a virtual sprite at (x, y): glyph pixel 1 becomes
 * `colour`, 0 is transparent, others are copied. Unreferenced. */
/* VA 0x0047bd00, 12 bytes, [I] row, status draft */
void zprimFreeScreen(ZprimAsset *screen)
{
    /* DM-064 created; no caller, thunk 0x0040204A */
    zprimFreeAsset(screen);
}

/* VA 0x0047bd20, 587 bytes, [I] row, status draft */
void zprimDrawTextToSprite(ZprimAsset *font, const uint8_t *s, uint32_t x, uint32_t y,
                           ZprimAsset *sprite, uint16_t frame, uint8_t colour)
{
    ZprimImage *img;
    const ZprimFont *f;
    uint8_t *dst;
    uint32_t stride;
    int held, row, col;

    if ((font->id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_FONT || (sprite->id & ZPRIM_TYPE_MASK) != ZPRIM_TYPE_VSPRITE) {
        g_pfnDebugPrintf("ZPRIM: Error - can only drawtexttosprite from font to virtual sprite (%d->%d)\n",
                         font->id, sprite->id);
        return;
    }
    img = (ZprimImage *)zprimLockAsset(sprite);
    if (frame >= img->count) {
        g_pfnDebugPrintf("ZPRIM: Error - frame out of bounds during drawtexttosprite\n");
        zprimUnlockAsset(sprite);
        return;
    }
    stride = img->frame[frame].width;
    dst = (uint8_t *)img + (x & 0xFFFF) + (y & 0xFFFF) * stride + img->frame[frame].pixels;
    STATE_LOCK(held);
    f = (const ZprimFont *)zprimLockAsset(font);
    for (; *s != 0; s++) {
        const ZprimGlyph *g = &f->glyph[*s - f->glyph[0].code];
        /* [I] reads one byte per pixel with row bytes = width; the shipped font bitmaps are 4-bit
         * (DM-066: row bytes = width / 2), so this path only suits 8-bit fonts */
        const uint8_t *src = (const uint8_t *)f + f->pixels + g->y * f->width + g->x;

        for (row = 0; row < g->h; row++, src += f->width) {
            for (col = 0; col < g->w; col++) {
                uint8_t p = src[col];
                if (p != 0)
                    dst[g->xoff + col + (g->yoff + row) * stride] = (p == 1) ? colour : p;
            }
        }
        dst += g->advance + g->xoff + g->w + CUR_STATE()->spacing;
    }
    zprimUnlockAsset(sprite);
    zprimUnlockAsset(font);
    STATE_UNLOCK(held);
}

/* A RenderWare material from 4/8-bit pixels: an in-memory BMP padded to a square power of two
 * (4..256), palette from the 15-bit colours (index in the alpha byte; 0x7C1F -> white key 0xFF00FF
 * becomes 0xFFxxFF), rows bottom-up, 4-bit nibbles swapped; named "Sprite<n>". */
/* VA 0x0047c010, 1 bytes, [I] row, status draft */
void zprimScreenStub(void)
{
    /* DM-064 created: a bare ret, thunk 0x0040162C */
}

/* VA 0x0047c030, 915 bytes, [V] direct, status draft */
int32_t zprimMakeMaterial(uint32_t width, uint32_t height, uint32_t bpp, const uint8_t *pixels,
                          const ZprimPalette *palette)
{
    char name[32];
    uint32_t w = width, h = height, srcStride, dstStride, imageSize, ncol, size, i;
    uint8_t *bmp, *p;
    int mark;
    int32_t mat;

    zstringPrintf(name, "Sprite%d", g_zprimSpriteCount++);
#define POW2(v) ((v) > 0x80 && (v) < 0x100 ? 0x100 : (v) > 0x40 && (v) < 0x80 ? 0x80 :       \
                 (v) > 0x20 && (v) < 0x40 ? 0x40 : (v) > 0x10 && (v) < 0x20 ? 0x20 :           \
                 (v) > 8 && (v) < 0x10 ? 0x10 : (v) > 4 && (v) < 8 ? 8 : (v) == 3 ? 4 : (v))
    w = POW2(width);
    h = POW2(height);
#undef POW2
    if (w != h) {                                   /* square */
        if (w < h)
            w = h;
        else
            h = w;
    }
    srcStride = width / (8 / bpp);
    dstStride = (w / (8 / bpp) + 3) & ~3u;
    mark = zmemTempMark();
    imageSize = dstStride * h;
    ncol = 1u << bpp;
    size = imageSize + 0x36 + ncol * 4;
    bmp = (uint8_t *)zmemTempAlloc(size);
    zstringMemSet(bmp, 0, size);
    /* BITMAPFILEHEADER + BITMAPINFOHEADER */
    *(uint16_t *)(bmp + 0x00) = 0x4D42;             /* "BM" */
    *(uint32_t *)(bmp + 0x02) = size;
    *(uint32_t *)(bmp + 0x06) = 0;
    *(uint32_t *)(bmp + 0x0A) = ncol * 4 + 0x36;
    *(uint32_t *)(bmp + 0x0E) = 0x28;
    *(uint32_t *)(bmp + 0x12) = w;
    *(uint32_t *)(bmp + 0x16) = h;
    *(uint16_t *)(bmp + 0x1A) = 1;
    *(uint16_t *)(bmp + 0x1C) = (uint16_t)bpp;
    *(uint32_t *)(bmp + 0x1E) = 0;
    *(uint32_t *)(bmp + 0x22) = imageSize;
    *(uint32_t *)(bmp + 0x26) = 0;
    *(uint32_t *)(bmp + 0x2A) = 0;
    *(uint32_t *)(bmp + 0x2E) = 0;
    *(uint32_t *)(bmp + 0x32) = 0;
    for (i = 0, p = bmp + 0x36; i < ncol; i++, p += 4) {
        if (i < palette->count) {
            uint16_t c = palette->colour[i];
            p[2] = (uint8_t)(c << 3);               /* R: low 5 bits */
            p[1] = (uint8_t)((c >> 5) << 3);        /* G */
            p[0] = (uint8_t)((c >> 10) << 3);       /* B */
            p[3] = (uint8_t)i;
            if (p[2] == 0xF8 && p[1] == 0 && p[0] == 0xF8) {
                p[0] = 0xFF;
                p[2] = 0xFF;
            }
        } else {
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }
    /* rows bottom-up */
    p = bmp + 0x36 + ncol * 4 + imageSize;
    for (i = 0; i < height; i++) {
        p -= dstStride;
        zstringMemCopy(p, pixels, srcStride);
        pixels += srcStride;
    }
    if (bpp == 4)
        for (i = 0; i < imageSize; i++) {
            uint8_t b = bmp[0x36 + ncol * 4 + i];
            bmp[0x36 + ncol * 4 + i] = (uint8_t)(b << 4 | b >> 4);
        }
    mat = g_pfnZglMatCreateFromImage(bmp, size, name, 0, 0);
    if (mat == 0)
        zAssertFail("Invalid material handle", ZPRIM_FILE, 0x16A5, 0);
    zmemTempRelease(mark);
    return mat;
}

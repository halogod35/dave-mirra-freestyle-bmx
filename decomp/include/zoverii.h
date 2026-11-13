/*
 * zoverii.h - Bmx/src/com/zge/bmx/zoverii.c: the overlay primitive layer (DM-054).
 *
 * Offsets [V] (field use in zoverii.c and in overlay.c / overlymp.c), names [I].
 *
 * zoverii keeps PSX-style GPU primitives for the 2D overlays in four fixed pools, each double
 * buffered (pool[buffer][n], 0xA0 bytes per slot). A handle is
 *     index (bits 0-11) | class (bits 12-13: 0 shapes, 1 glyphs) | layer (bit 14: 0 HUD, 1 menus).
 * Every frame zoveriiFlush links the pools of the active layer into the NgIFpsx ordering table
 * (PSX addPrim), swaps the buffers and copies every primitive changed this frame ("dirty") into the
 * other buffer, so both buffers stay in step. A text string is a chain of glyph primitives
 * (ZoverPrim.next). Hidden or free slots keep their place in the chain with a zero length.
 *
 * Colours are 0x0BRRGGBB: B = semi-transparency mode 1..7 (0 = opaque); bit 28 on a rectangle
 * selects a second draw mode (0x7FC0 instead of 0x7F80).
 */
#ifndef ZOVERII_H
#define ZOVERII_H
#include "dmtypes.h"

#define ZOVER_MAX_PRIMS     0x23C   /* 16 + 128 + 28 + 400: size of the free and dirty lists */
#define ZOVER_MAX_FONTS     8
#define ZOVER_NO_HANDLE     0xFFFFu

/* handle fields */
#define ZOVER_INDEX(h)      ((h) & 0xFFFu)
#define ZOVER_CLASS(h)      (((h) >> 12) & 3u)
#define ZOVER_LAYER(h)      (((h) >> 14) & 1u)
#define ZOVER_HANDLE(i, cls, layer) ((uint16_t)((i) | ((cls) | (layer) << 2) << 12))

enum { ZOVER_CLASS_SHAPE = 0, ZOVER_CLASS_GLYPH = 1 };
enum { ZOVER_LAYER_HUD = 0, ZOVER_LAYER_MENU = 1 };

/* ZoverPrim.type */
enum {
    ZOVER_FREE   = 0,
    ZOVER_SPRITE = 2,               /* textured quad (zoveriiAddSprite) */
    ZOVER_RECT   = 3,               /* flat quad (zoveriiAddRect) */
    ZOVER_GRAD   = 4,               /* gouraud quad (zoveriiAddGradient) */
    ZOVER_GLYPH  = 5                /* one character of a text chain (zoveriiAddText) */
};

#define ZOVERF_BLEND   0x07u        /* ZoverPrim.flags: semi-transparency mode of the colour */
#define ZOVERF_DIRTY   0x80u        /* on the dirty list */

/* colour helpers: rgb + semi-transparency mode */
#define ZOVER_RGB(r, g, b)         (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ZOVER_COLOUR(blend, rgb)   (((uint32_t)(blend) << 24) | (rgb))
#define ZOVER_GREY(blend, v)       ZOVER_COLOUR(blend, ZOVER_RGB(v, v, v))

/* PSX tag word: next pointer (28 bits) | length in words (4 bits) */
#define ZOVER_TAG_ADDR   0x0FFFFFFFu
#define ZOVER_TAG_LEN(n) ((uint32_t)(n) << 28)

/* flat quad: PSX POLY_F4 + the PC port's draw mode words */
typedef struct ZoverPolyF4 {
    uint32_t tag;                   /* +0x00 */
    uint8_t  r, g, b, code;         /* +0x04 code 0x28 (0x2A semi-transparent) */
    int16_t  x0, y0;                /* +0x08 */
    int16_t  x1, y1;                /* +0x0C */
    int16_t  x2, y2;                /* +0x10 */
    int16_t  x3, y3;                /* +0x14 */
    uint32_t unk_18;
    uint32_t mode;                  /* +0x1C low 15 bits 0x7F80 (0x7FC0 with colour bit 28) */
    uint8_t  blend;                 /* +0x20 semi-transparency mode - 1 */
    uint8_t  pad_21[3];
} ZoverPolyF4;                      /* 0x24 */

/* gouraud quad: PSX POLY_G4 + mode words */
typedef struct ZoverPolyG4 {
    uint32_t tag;                   /* +0x00 */
    uint8_t  r0, g0, b0, code;      /* +0x04 code 0x38 (0x3A) */
    int16_t  x0, y0;                /* +0x08 */
    uint8_t  r1, g1, b1, pad_0f;    /* +0x0C */
    int16_t  x1, y1;                /* +0x10 */
    uint8_t  r2, g2, b2, pad_17;    /* +0x14 */
    int16_t  x2, y2;                /* +0x18 */
    uint8_t  r3, g3, b3, pad_1f;    /* +0x1C */
    int16_t  x3, y3;                /* +0x20 */
    uint32_t unk_24;
    uint32_t mode;                  /* +0x28 */
    uint8_t  blend;                 /* +0x2C */
    uint8_t  pad_2d[3];
} ZoverPolyG4;                      /* 0x30 */

/* textured quad: a POLY_FT4 with 16-bit texture coordinates (PC port), texture handle, mode */
typedef struct ZoverPolyFT4 {
    uint32_t tag;                   /* +0x00 */
    uint8_t  r, g, b, code;         /* +0x04 code 0x2C (0x2E) */
    int16_t  x0, y0;                /* +0x08 */
    uint16_t u0, v0;                /* +0x0C */
    uint16_t clut;                  /* +0x10 palette (+ the text palette offset) */
    int16_t  x1, y1;                /* +0x12 */
    uint16_t u1, v1;                /* +0x16 */
    uint16_t tpage;                 /* +0x1A texture page (bits 5-6: semi-transparency rate) */
    int16_t  x2, y2;                /* +0x1C */
    uint16_t u2, v2;                /* +0x20 */
    uint16_t pad_24;
    int16_t  x3, y3;                /* +0x26 */
    uint16_t u3, v3;                /* +0x2A */
    uint16_t pad_2e;
    uint32_t texture;               /* +0x30 from the texture slot (+0x10) */
    uint32_t mode;                  /* +0x34 */
    uint8_t  blend;                 /* +0x38 */
    uint8_t  pad_39[3];
} ZoverPolyFT4;                     /* 0x3C */

/* one pool slot */
typedef struct ZoverPrim {
    uint8_t  body[0x9C];            /* +0x00 a ZoverPolyF4 / G4 / FT4 (the tag word first) */
    uint8_t  type;                  /* +0x9C ZOVER_* (0 free) */
    uint8_t  flags;                 /* +0x9D ZOVERF_* */
    uint16_t next;                  /* +0x9E next glyph of a text chain (handle), 0xFFFF end */
} ZoverPrim;                        /* 0xA0 */

#define ZOVER_TAGP(p) ((uint32_t *)(p)->body)
#define ZOVER_F4(p)   ((ZoverPolyF4 *)(p)->body)
#define ZOVER_G4(p)   ((ZoverPolyG4 *)(p)->body)
#define ZOVER_FT4(p)  ((ZoverPolyFT4 *)(p)->body)

/* a texture slot of the NgIFpsx / NgIFzres image table (g_psxTextures, 0x0068C3E0) */
typedef struct ZoverTexture {
    uint16_t tpage;                 /* +0x00 */
    uint16_t clut;                  /* +0x02 */
    uint8_t  u, v;                  /* +0x04 top left in the page */
    uint8_t  u1, v1;                /* +0x06 bottom right (inclusive) */
    uint32_t unk_08;
    uint8_t  w, h;                  /* +0x0C */
    uint16_t unk_0e;
    uint32_t texture;               /* +0x10 */
} ZoverTexture;                     /* 0x14 */

typedef struct ZoverGlyph {
    int16_t  u;                     /* +0x00 offset in the font texture */
    int16_t  v;                     /* +0x02 */
    int16_t  w;                     /* +0x04 advance = w + 1 */
    int16_t  pad_06;
} ZoverGlyph;                       /* 0x08 */

typedef struct ZoverFont {
    uint16_t *slot;                 /* +0x00 -> texture slot number (index into g_psxTextures) */
    uint8_t  unk_04;                /* +0x04 cleared by zoveriiLoadFont */
    uint8_t  height;                /* +0x05 line height */
    int8_t   first;                 /* +0x06 first character code */
    uint8_t  count;                 /* +0x07 characters */
    ZoverGlyph *glyphs;             /* +0x08 [count], zmemTempAlloc'd */
} ZoverFont;                        /* 0x0C */

/* font file (miscdata.zal 0x7531..0x7535): header, then 13-byte records */
typedef struct ZoverFontHeader {
    uint8_t  count;                 /* +0x00 records */
    uint8_t  pad_01[3];
    uint8_t  height;                /* +0x04 */
    uint8_t  pad_05[7];
} ZoverFontHeader;                  /* 0x0C */

typedef struct ZoverFontRecord {
    uint8_t  ch;                    /* +0x00 character code */
    uint8_t  u[4];                  /* +0x01 u16 (+ 2 unused bytes) */
    uint8_t  v[4];                  /* +0x05 */
    uint8_t  w[4];                  /* +0x09 */
} ZoverFontRecord;                  /* 0x0D */

/* the immediate-mode glyph descriptor handed to psxDrawSpriteList (g_zoverGlyphSprite 0x0055B960) */
typedef struct ZoverSprite {
    uint32_t unk_00;
    uint16_t unk_04;
    uint16_t slot;                  /* +0x06 texture slot */
    int16_t  u, v;                  /* +0x08 */
    int16_t  w, h;                  /* +0x0C */
    uint16_t colour;                /* +0x10 0x0RGB (4 bits each) */
    uint16_t clut;                  /* +0x12 palette offset */
} ZoverSprite;                      /* 0x14 */

/* GHIDRA-SKIP-BEGIN */
typedef char zoverii_h_size_check[(sizeof(ZoverPolyF4) == 0x24 && sizeof(ZoverPolyG4) == 0x30 &&
                                   sizeof(ZoverPolyFT4) == 0x3C && sizeof(ZoverPrim) == 0xA0 &&
                                   sizeof(ZoverTexture) == 0x14 && sizeof(ZoverGlyph) == 0x08 &&
                                   sizeof(ZoverFont) == 0x0C && sizeof(ZoverFontHeader) == 0x0C &&
                                   sizeof(ZoverFontRecord) == 0x0D && sizeof(ZoverSprite) == 0x14) ? 1 : -1];

/* zoverii.c (DM-054) */
int      zoveriiInit(void);                                                            /* 0x004578F0 */
int      zoveriiFlush(void);                                                           /* 0x00457B00 */
int      zoveriiSetLayer(uint8_t layer);                                               /* 0x00457F50 */
int      zoveriiGetPool(uint8_t layer, uint8_t cls, uint8_t buffer, ZoverPrim **base,
                        uint32_t *count, uint32_t *pool);                              /* 0x00457F70 */
uint16_t zoveriiAlloc(uint8_t layer, uint8_t cls);                                     /* 0x004580E0 */
void     zoveriiFree(uint16_t handle);                                                 /* 0x004581A0 */
int      zoveriiShow(uint16_t handle, int on);                                         /* 0x00458400 */
uint16_t zoveriiAddRect(uint8_t layer, int16_t x, int16_t y, int16_t w, int16_t h,
                        uint32_t colour);                                              /* 0x004588D0 */
int      zoveriiSetRect(uint16_t handle, int16_t x, int16_t y, int16_t w, int16_t h);  /* 0x00458B00 */
int      zoveriiSetColour(uint16_t handle, uint32_t colour);                           /* 0x00458CC0 */
void     zoveriiDrawRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t colour); /* 0x00458EA0 */
uint16_t zoveriiAddGradient(uint8_t layer, int16_t x, int16_t y, int16_t w, int16_t h,
                            uint32_t top, uint32_t bottom);                            /* 0x00458F20 */
uint16_t zoveriiAddSprite(uint8_t layer, uint16_t slot, int16_t x, int16_t y,
                          uint32_t colour);                                            /* 0x00459180 */
uint16_t zoveriiAddText(uint8_t layer, const ZoverFont *font, const char *s, int16_t x, int16_t y,
                        uint32_t colour, int16_t clut);                                /* 0x004594C0 */
int      zoveriiSetTextColour(uint16_t handle, const ZoverFont *font, uint32_t colour,
                              int16_t clut);                                           /* 0x00459940 */
int      zoveriiMoveText(uint16_t handle, int16_t x, int16_t y);                       /* 0x00459BE0 */
void     zoveriiDrawText(const ZoverFont *font, const char *s, int16_t x, int16_t y,
                         uint32_t colour, uint16_t clut);                              /* 0x00459DF0 */
int16_t  zoveriiTextWidth(const char *s, const ZoverFont *font);                       /* 0x00459F30 */
int      zoveriiLoadFont(const uint8_t *data, ZoverFont *font);                        /* 0x00459FC0 */
/* GHIDRA-SKIP-END */

#endif /* ZOVERII_H */

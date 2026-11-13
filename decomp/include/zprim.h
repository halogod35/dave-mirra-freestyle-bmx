/*
 * zprim.h - Bmx/Src/com/zge/zprim.c: 2D primitives for the menus.
 *
 * Offsets [V] (field use in zprim.c and its callers in frontend.c / guimain.c), names [I].
 *
 * zprim keeps a stack of draw states (ZprimState, 0x54 bytes, in one DMM block) and a pool of
 * 128 asset handles (ZprimAsset) for the 2D resources of the open picture library (the GUI ZAL):
 * pictures, sprites, fonts, "screens" and groups. The asset id's top three bits give the type.
 * The data stays in a DMM (movable) block; zprimLockAsset / zprimUnlockAsset pin it. Pictures and
 * sprites are turned into RenderWare materials through an in-memory BMP (zprimMakeMaterial).
 * Drawing goes through the PSX-style layer NgIFpsx (rectangles, lines, sprites, glyphs).
 */
#ifndef ZPRIM_H
#define ZPRIM_H
#include "dmtypes.h"

#define ZPRIM_MAX_ASSETS   128
#define ZPRIM_MAX_STATES   10      /* zprimPushState stops at index 10, which is past the 0x348-byte block */
#define ZPRIM_MAX_GROUPS   4

/* asset id types (id & ZPRIM_TYPE_MASK) */
#define ZPRIM_TYPE_MASK     0xE000u
#define ZPRIM_TYPE_PICTURE  0x0000u
#define ZPRIM_TYPE_SPRITE   0x4000u   /* frames with a hot spot */
#define ZPRIM_TYPE_SPRITE2  0x6000u   /* frames drawn without the hot-spot offset */
#define ZPRIM_TYPE_FONT     0x8000u
#define ZPRIM_TYPE_SCREEN   0xA000u
#define ZPRIM_TYPE_VSPRITE  0xC000u   /* "virtual sprite": created in memory (ids from 0xC001) */

/* ZprimAsset.flags */
#define ZPRIM_ASSET_KEEP     0x0001u  /* survives zprimFlush */
#define ZPRIM_ASSET_SHARED   0x0002u  /* data inside a loaded group: not freed with the handle */
#define ZPRIM_ASSET_CREATED  0x0004u  /* made by zprimCreateSprite / zprimCreateImage */

/* draw mode bits (ZprimState.primFlags / spriteFlags / fontFlags): the NgIFpsx mode passed is
 * 1 for bit 1, else 2 for bit 2, else 3 for bit 0, else 0 */
#define ZPRIM_MODE_3         0x0001u  /* fontFlags: also "no background box" */
#define ZPRIM_MODE_1         0x0002u
#define ZPRIM_MODE_2         0x0004u

/* zprimSetAttr / zprimGetAttr */
enum {
    ZPRIM_ATTR_BG_PICTURE = 1,    /* background = a picture asset (zprimDrawBackground blits it) */
    ZPRIM_ATTR_BG_2       = 2,
    ZPRIM_ATTR_BG_3       = 3,
    ZPRIM_ATTR_BG_NONE    = 4,
    ZPRIM_ATTR_COLOUR     = 5,    /* fill / line colour */
    ZPRIM_ATTR_PRIM_MODE1 = 6,    /* primFlags bit 1 (not readable back) */
    ZPRIM_ATTR_SPR_MODE1  = 7,
    ZPRIM_ATTR_SPR_MODE3  = 8,
    ZPRIM_ATTR_SPR_MODE2  = 9,
    ZPRIM_ATTR_SPR_ARG20  = 10,
    ZPRIM_ATTR_SPR_ARG1C  = 11,
    ZPRIM_ATTR_SPR_BIT3   = 12,
    ZPRIM_ATTR_SPR_BIT4   = 13,
    ZPRIM_ATTR_SPR_ARG30  = 14,
    ZPRIM_ATTR_SPR_ARG24  = 15,
    ZPRIM_ATTR_CLUT34     = 16,
    ZPRIM_ATTR_CLUT38     = 17,
    ZPRIM_ATTR_FONT_MODE1 = 18,
    ZPRIM_ATTR_FONT_MODE3 = 19,
    ZPRIM_ATTR_FONT_MODE2 = 20,
    ZPRIM_ATTR_TEXT_BG    = 21,   /* text background box colour */
    ZPRIM_ATTR_FONT_ARG48 = 22,
    ZPRIM_ATTR_FONT_ARG40 = 23,
    ZPRIM_ATTR_SPACING    = 24    /* extra pixels after each glyph */
};

typedef struct ZprimAsset {
    uint16_t id;                 /* +0x00 asset id (ZPRIM_TYPE_* | number) */
    uint16_t flags;              /* +0x02 ZPRIM_ASSET_* */
    struct ZprimAsset *next;     /* +0x04 used list g_zprimAssets / free list g_zprimFreeAssets */
    int32_t  handle;             /* +0x08 DMM handle of the data */
    uint32_t offset;             /* +0x0C data offset inside the block (group members) */
} ZprimAsset;                    /* 0x10 */

typedef struct ZprimState {
    uint32_t bgData;             /* +0x00 ZprimAsset * for ZPRIM_ATTR_BG_PICTURE */
    uint16_t bgType;             /* +0x04 ZPRIM_ATTR_BG_* */
    uint16_t pad_06;
    uint32_t unk_08;
    uint32_t colour;             /* +0x0C 0xFFFF after zprimInit */
    uint16_t primFlags;          /* +0x10 ZPRIM_MODE_* for lines and rectangles */
    uint16_t pad_12;
    ZprimAsset *sprite;          /* +0x14 current sprite (zprimSetSprite) */
    uint16_t spriteFlags;        /* +0x18 ZPRIM_MODE_* | 8 | 0x10 */
    uint16_t pad_1a;
    uint32_t spriteArg1c;        /* +0x1C */
    uint32_t spriteArg20;        /* +0x20 mode-2 argument of zprimDrawSprite */
    uint16_t spriteArg24;        /* +0x24 */
    uint16_t pad_26;
    float    scaleX;             /* +0x28 1.0 (zprimResetScale) */
    float    scaleY;             /* +0x2C */
    uint32_t spriteArg30;        /* +0x30 */
    uint32_t clut34;             /* +0x34 != 0: psx2DSetTint(clut38, clut34) around sprite draws */
    uint32_t clut38;             /* +0x38 */
    ZprimAsset *font;            /* +0x3C current font (zprimSetFont) */
    uint32_t fontArg40;          /* +0x40 */
    uint16_t fontFlags;          /* +0x44 ZPRIM_MODE_* */
    uint16_t pad_46;
    uint32_t fontArg48;          /* +0x48 mode-2 argument of the glyph draw */
    uint32_t textBg;             /* +0x4C background box colour (and mode-2 argument) */
    int32_t  spacing;            /* +0x50 */
} ZprimState;                    /* 0x54 */

/* Pictures, sprites and screens share one header; a picture is an image with one frame. */
typedef struct ZprimFrame {
    uint16_t width;              /* +0x00 */
    uint16_t height;             /* +0x02 */
    uint32_t pixels;             /* +0x04 offset from the image header */
    int16_t  hotX;               /* +0x08 subtracted from the draw position (not for ZPRIM_TYPE_SPRITE2) */
    int16_t  hotY;               /* +0x0A */
    int32_t  material;           /* +0x0C RenderWare material (zprimMakeMaterial) */
} ZprimFrame;                    /* 0x10 */

typedef struct ZprimImage {
    uint16_t count;              /* +0x00 frames */
    uint16_t flags;              /* +0x02 bit 0: a 256-entry (16-bank) palette for a 4-bit sprite (not one
                                    palette per frame; all 33 such sprites have one frame) */
    uint16_t width;              /* +0x04 */
    uint16_t height;             /* +0x06 */
    uint32_t pixels;             /* +0x08 offset of the first frame's pixels */
    uint32_t palette;            /* +0x0C offset of the ZprimPalette (0: none) */
    uint16_t bpp;                /* +0x10 4 or 8 */
    uint16_t pad_12;
    ZprimFrame frame[1];         /* +0x14 [count] */
} ZprimImage;

typedef struct ZprimPalette {     /* the block is always 0x204 bytes */
    uint16_t unk_00;
    uint16_t count;              /* +0x02 */
    uint16_t colour[1];          /* +0x04 [count] 15-bit colours, red in the low bits; 0x7C1F = transparent */
} ZprimPalette;

typedef struct ZprimGlyph {
    uint8_t  x;                  /* +0x00 position in the font bitmap */
    uint8_t  y;                  /* +0x01 */
    uint8_t  w;                  /* +0x02 */
    uint8_t  h;                  /* +0x03 */
    int8_t   xoff;               /* +0x04 draw offset */
    int8_t   yoff;               /* +0x05 */
    int8_t   advance;            /* +0x06 extra advance after the glyph */
    int8_t   descent;            /* +0x07 extra height of the background box */
    uint16_t code;               /* +0x08 character code */
} ZprimGlyph;                    /* 0x0A */

typedef struct ZprimFont {
    uint16_t count;              /* +0x00 glyphs; >= 0x100: search by code, else indexed from glyph[0].code */
    uint16_t lineHeight;         /* +0x02 line height (zprimGetFontHeight; was "flags") */
    uint32_t pixels;             /* +0x04 offset of the glyph bitmap (bpp bits per pixel) */
    uint16_t width;              /* +0x08 bitmap width in pixels (4-bit: row bytes = width / 2) (was "stride") */
    uint16_t height;             /* +0x0A bitmap rows */
    void   **materials;          /* +0x0C 32 materials made by NgIFpsx (freed with the font) */
    uint32_t palette;            /* +0x10 offset of the 16-bank ZprimPalette */
    uint16_t bpp;                /* +0x14 bits per pixel; passed to the glyph draw */
    uint16_t unk_16;
    ZprimGlyph glyph[1];         /* +0x18 [count] */
} ZprimFont;

/* GHIDRA-SKIP-BEGIN */
typedef char zprim_h_size_check[(sizeof(ZprimAsset) == 0x10 && sizeof(ZprimState) == 0x54 &&
                                 sizeof(ZprimFrame) == 0x10 && sizeof(ZprimGlyph) == 0x0A) ? 1 : -1];

/* zprim.c */
int         zprimInit(void);                                                           /* 0x004779E0 */
void        zprimShutdown(void);                                                       /* 0x00477B50 */
void        zprimOpenLibrary(const char *path);                                        /* 0x00477BA0 */
void        zprimDrawBackground(void);                                                 /* 0x00477BE0 */
void        zprimFlush(void);                                                          /* 0x00477D20 */
void        zprimSetClip(int x0, int y0, int x1, int y1);                              /* 0x00477DA0 */
void        zprimSetBaseState(void);                                                   /* 0x00477DF0 */
void        zprimResetState(void);                                                     /* 0x00477E80 */
void        zprimPushState(void);                                                      /* 0x00477F10 */
void        zprimDropState(void);                                                      /* 0x00477FB0 */
void        zprimSetAttr(int attr, uint32_t value);                                    /* 0x00478050 */
uint32_t    zprimGetAttr(int attr);                                                    /* 0x00478370 */
void        zprimSetBackgroundPicture(ZprimAsset *picture);                            /* 0x004788C0 */
void        zprimSetBackgroundSprite(ZprimAsset *sprite, int16_t type, uint16_t frame, int16_t x, int16_t y); /* 0x00478980 */
void        zprimSetFont(ZprimAsset *font);                                            /* 0x00478A50 */
ZprimAsset *zprimGetFont(void);                                                        /* 0x00478AE0 */
void        zprimSetSprite(ZprimAsset *sprite);                                        /* 0x00478B60 */
void        zprimSetScale(float x, float y);                                           /* 0x00478BF0 */
void        zprimResetScale(void);                                                     /* 0x00478C80 */
ZprimAsset *zprimGetSprite(void);                                                      /* 0x00478D00 */
void        zprimSetRGB(uint32_t r, uint8_t g, uint8_t b, int attr);                   /* 0x00478D80 */
void        zprimGetColor(void);                                                       /* 0x00478DC0 asserts */
void        zprimDrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);             /* 0x00478DF0 */
void        zprimDrawRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1);             /* 0x00478F00 */
ZprimAsset *zprimLoadPicture(uint32_t id, int keep);                                   /* 0x00479010 */
void        zprimFreePicture(ZprimAsset *picture);                                     /* 0x004791C0 */
uint16_t    zprimGetPictureWidth(ZprimAsset *picture);                                 /* 0x004791E0 */
uint16_t    zprimGetPictureHeight(ZprimAsset *picture);                                /* 0x00479210 */
void        zprimCopyMaterial(ZprimAsset *src, ZprimAsset *dst);                       /* 0x00479280 */
void        zprimReplaceVRAMGrey(ZprimAsset *src, ZprimAsset *dst, uint32_t level);    /* 0x00479300 */
void        zprimSelectPaletteBank(ZprimAsset *src, ZprimAsset *dst, uint32_t bank);   /* 0x00479500 */
ZprimAsset *zprimLoadSprite(uint32_t id, int keep);                                    /* 0x00479660 */
ZprimAsset *zprimCreateSprite(uint16_t width, uint16_t height, uint16_t frames);       /* 0x00479880 */
void        zprimTouchAsset(ZprimAsset *asset);                                        /* 0x00479A80 */
void        zprimResetImage(ZprimAsset *asset, uint16_t width, uint16_t height, uint16_t bpp, uint16_t colours); /* 0x00479AB0 */
ZprimAsset *zprimCreateImage(uint16_t width, uint16_t height, uint16_t bpp, uint16_t colours); /* 0x00479B90 */
void        zprimFree(ZprimAsset *asset);                                              /* 0x00479CD0 */
void        zprimDrawSprite(uint32_t frame, int16_t x, int16_t y);                     /* 0x00479CF0 */
uint16_t    zprimGetSpriteWidth(ZprimAsset *sprite, uint16_t frame);                   /* 0x00479FA0 */
uint16_t    zprimGetSpriteHeight(ZprimAsset *sprite, uint16_t frame);                  /* 0x0047A070 */
uint16_t    zprimGetSpriteFrames(ZprimAsset *sprite);                                  /* 0x0047A140 */
void        zprimDecompressFrame(ZprimAsset *src, uint16_t srcFrame, ZprimAsset *dst, uint16_t dstFrame); /* 0x0047A1E0 */
void       *zprimGetFramePixels(ZprimImage *img, uint16_t frame);                      /* 0x0047A300 */
int         zprimRleDecode(const uint8_t *src, uint8_t *dst);                          /* 0x0047A330 */
ZprimAsset *zprimLoadFont(uint32_t id, int keep);                                      /* 0x0047A420 */
void        zprimFreeFont(ZprimAsset *font);                                           /* 0x0047A5E0 */
void        zprimFontStub(void);                                                       /* 0x0047A600 */
void        zprimDrawTextFixed(const uint8_t *s, int x, int16_t y);                    /* 0x0047A620 */
void        zprimTextStub(void);                                                       /* 0x0047AA60 */
void        zprimDrawText(const uint8_t *s, int x, int16_t y);                         /* 0x0047AA80 */
uint16_t    zprimGetFontHeight(ZprimAsset *font);                                      /* 0x0047AF10 (was zprimGetFontFlags) */
int16_t     zprimGetTextWidth(ZprimAsset *font, const uint8_t *s);                     /* 0x0047AFD0 */
ZprimAsset *zprimLoadGroupImage(uint32_t groupId, uint16_t imageId);                   /* 0x0047B220 */
int         zprimGroupLoaded(uint32_t slot);                                           /* 0x0047B3D0 */
int         zprimLoadGroup(uint16_t slot, uint16_t groupId);                           /* 0x0047B400 */
void        zprimUnloadGroup(uint16_t slot);                                           /* 0x0047B5C0 */
void        zprimFreeAsset(ZprimAsset *asset);                                         /* 0x0047B6B0 */
ZprimAsset *zprimAllocAsset(void);                                                     /* 0x0047B830 */
void        zprimReleaseAsset(ZprimAsset *asset);                                      /* 0x0047B880 */
void       *zprimLockAsset(ZprimAsset *asset);                                         /* 0x0047B900 */
void        zprimUnlockAsset(ZprimAsset *asset);                                       /* 0x0047B930 */
ZprimAsset *zprimLoadAsset(uint32_t id);                                               /* 0x0047B950 */
uint16_t    zprimGetGroupCount(uint32_t slot);                                         /* 0x0047BA10 */
ZprimAsset *zprimSelectGroupEntry(uint32_t slot, uint32_t index);                      /* 0x0047BA50 */
ZprimAsset *zprimFindAsset(int16_t id);                                                /* 0x0047BAC0 */
ZprimAsset *zprimFindInGroups(uint32_t id);                                            /* 0x0047BAF0 */
ZprimAsset *zprimLoadScreen(uint32_t id, int keep);                                    /* 0x0047BB90 */
void        zprimFreeScreen(ZprimAsset *screen);                                       /* 0x0047BD00 */
void        zprimScreenStub(void);                                                     /* 0x0047C010 */
void        zprimDrawTextToSprite(ZprimAsset *font, const uint8_t *s, uint32_t x, uint32_t y,
                                  ZprimAsset *sprite, uint16_t frame, uint8_t colour); /* 0x0047BD20 */
int32_t     zprimMakeMaterial(uint32_t width, uint32_t height, uint32_t bpp, const uint8_t *pixels,
                              const ZprimPalette *palette);                            /* 0x0047C030 */
/* GHIDRA-SKIP-END */

#endif /* ZPRIM_H */

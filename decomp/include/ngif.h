/*
 * ngif.h - the NgIF layer (Bmx/src/com/zge/NgIF/*.c): the game's implementation of the ZGE engine
 * interface on top of the PC engine (NgEngine / ZglRw) (DM-057).
 *
 *   NgApp.c        application hooks (init, per-frame, shutdown), level load, lights, loading screen
 *   NgIFfx.c       zfx particle types (spark, shard, splash, dust, poster, ripple, lightning),
 *                  animated textures, camera shake
 *   NgIFpsx.c      PlayStation GPU primitive emulation (POLY_*, SPRT, TILE, LINE_* packets drawn
 *                  through the ZGL immediate-mode API)
 *   NgIFzactor.c   actor <-> model glue: channel -> frame binding, the arm table, bone positions
 *   NgIFzbsp.c     prop and tag collision records (NgPropCol, the edge/face lists), BSP stubs
 *   NgIFzcollide.c segment test (world + props), tag lookup, prop collision shapes and rails
 *   NgIfzmemDMM.c  movable-block heap (DM-044, zmem.h)
 *   NgIFzres.c     resources: asset libraries, PLYR table, images
 *   NgIFzroller.c  level / actor creation ("zroller" = the engine's level runner), stubs, the
 *                  frame callback table and the PSX memory-card stubs
 *   MissingZGE.c   ZGE functions the PC engine lacks (print "%s %d %s")
 *
 * Offsets [V] (field use in the NgIF code and its callers), names and meanings [I] unless noted.
 * Units: ZGE world units, y DOWN, 16-bit angles. Description: docs/re/ngif.md.
 */
#ifndef NGIF_H
#define NGIF_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "zlist.h"
#include "zmem.h"
#include "zprop.h"
#include "ztaginf.h"
#include "physics.h"
#include "bmxprop.h"
#include "zactor.h"
#include "zoverii.h"
#include "zprim.h"
#include "fx.h"

/* ================================================================== ZGL immediate mode */

/* One vertex of the ZGL immediate-mode primitives (0x1C) [V: the stack blocks NgIFfx / NgApp /
 * NgIFpsx build]. */
typedef struct ZglVertex {
    RwV3d    pos;        /* +0x00 world (3-D calls) or screen (2-D calls: x, y in pixels, z 0) */
    float    pad_0c;
    float    u;          /* +0x10 */
    float    v;          /* +0x14 */
    RwRGBA   colour;     /* +0x18 */
} ZglVertex;             /* 0x1C */

typedef struct ZglLine {
    uint32_t   material; /* +0x00 0 = untextured */
    ZglVertex *v[2];     /* +0x04 */
} ZglLine;               /* 0x0C */

typedef struct ZglTri {
    uint32_t   material;
    ZglVertex *v[3];
} ZglTri;                /* 0x10 */

typedef struct ZglQuad {
    uint32_t   material;
    ZglVertex *v[4];     /* corner order 0, 1, 2, 3 around the quad */
} ZglQuad;               /* 0x14 */

/* ZGL render flags passed to the draw calls (bits [I]: 0x0002 = depth test, 0x0200 = alpha blend,
 * 0x2000 / 0x4000 / 0x0100 = blend mode, 0x0001 = additive). */
#define ZGL_PRIM_2D       0x02000000u  /* screen-space vertices */

/* ================================================================== zfx particles (NgIFfx.c) */

/* One particle of the NgIFfx particle types (0xA0, from the system's free list) [V offsets]. */
typedef struct ZfxParticle {
    uint32_t flags;      /* +0x00 ZFXP_* */
    RwV3d    pos;        /* +0x04 */
    uint32_t pad_10;
    RwV3d    prev;       /* +0x14 position before the last step (trail start) */
    uint32_t pad_20;
    RwV3d    vel;        /* +0x24 units per second (x 0.001 per ms) */
    uint32_t pad_30;
    RwV3d    axis;       /* +0x34 shard: rotated by spin each step */
    uint32_t pad_40;
    int16_t  spin[3];    /* +0x44 shard spin per step */
    RwRGBA   colour;     /* +0x4A alpha = fade */
    uint16_t pad_4e;
    float    size;       /* +0x50 half size; ripple: grows at vel.x per second */
    uint32_t material;   /* +0x54 */
    uint32_t lifeMs;     /* +0x58 */
    uint32_t born;       /* +0x5C timeGetTime at spawn */
    FxSkidCorner corner[4]; /* +0x60 poster: the four corners */
} ZfxParticle;           /* 0xA0 */

#define ZFXP_MOVING   0x01u  /* integrate pos += vel */
#define ZFXP_SPINNING 0x02u  /* rotate axis by spin */

/* A particle system instance (0x30, zmemAlloc; the engine emitter's per-type data) [V offsets]. */
typedef struct ZfxSystem {
    zHeapList    *particles; /* +0x00 */
    ZmemFreeList *pool;      /* +0x04 0xA0-byte particles */
    RwV3d         boundMin;  /* +0x08 bounding box of the moving particles (ripple update) */
    uint32_t      pad_14;
    RwV3d         boundMax;  /* +0x18 */
    uint32_t      pad_24;
    uint32_t      lastTime;  /* +0x28 */
    void         *owner;     /* +0x2C */
} ZfxSystem;                 /* 0x30 */

/* A zfx type: the engine particle-emitter vtable (0x38) as NgIFfx fills it for each type
 * (0x0055C278 spark ... 0x0055C3C8 lightning). Slots 0..5 and 7 are the engine defaults
 * (0x00495380.., zobjPEmit gap) unless noted. */
typedef struct ZfxType {
    void (__cdecl *destroy)(struct ZfxType *self);                         /* +0x00 */
    void *fn04;
    void (__cdecl *addAffector)(struct ZfxType *self, const char *name, const void *arg); /* +0x08 */
    void *fn0c;
    ZfxSystem *(__cdecl *create)(void *owner, uint32_t capacity);         /* +0x10 poster: zfxPosterCreate */
    void *fn14;
    void (__cdecl *spawn)(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused,
                          const void *params);                            /* +0x18 */
    int  (__cdecl *update)(void *type, ZfxSystem *sys);                    /* +0x1C ripple: zfxRippleUpdate */
    int  (__cdecl *draw)(void *type, ZfxSystem *sys);                      /* +0x20 */
    void *unused[5];
} ZfxType;                   /* 0x38 */

/* An animated material set (fans, flags): props whose def id is zHashName("ANIM") cycle the
 * textures of the materials named in the table 0x0055C0D8 (10 entries at 0x0068FD00) [V offsets]. */
typedef struct ZfxTexAnim {
    ZpropDef *def;       /* +0x00 0 = unused */
    uint8_t   frame;     /* +0x04 */
    uint8_t   frames;    /* +0x05 0x0055C120 bits 8-15 (3) */
    uint8_t   fps;       /* +0x06 0x0055C120 bits 0-7 (0x1E) */
    uint8_t   pad_07;
    void     *base;      /* +0x08 the world material "FAN1" */
    void     *mat[16];   /* +0x0C one per frame (model material, else world material) */
    uint32_t  lastTime;  /* +0x4C */
} ZfxTexAnim;            /* 0x50 */

/* ================================================================== PSX emulation (NgIFpsx.c) */

/* The PC packets are PSX libgpu primitives (tag word, rgb + code, 16-bit vertices) followed by the
 * port's draw-mode word ("mode"), a blend byte (semi-transparency rate - 1) and, for 3-D packets,
 * float vertices. The F4 / G4 / FT4 heads are zoverii.h's ZoverPolyF4 / G4 / FT4. */
#define PSXM_FLOAT_UV   0x0001u  /* GT4: uvf[] instead of the 16-bit u, v */
#define PSXM_WIDE512    0x0002u  /* 2-D x in 0..511 (else 0..639) */
#define PSXM_ALPHA      0x0004u  /* alpha = mode bits 7-14 (else by blend rate) */
#define PSXM_DEPTH      0x0008u  /* keep depth test (clear: ZGL flag 4) */
#define PSXM_3D         0x0020u  /* pos[] are world positions */
#define PSXM_KEEPSPACE  0x0040u  /* F4 / FT4: without it the packet is drawn as ZGL 0x02000001 */

/* code 0x20 flat triangle (0x70) */
typedef struct PsxPolyF3 {
    uint32_t tag;        /* +0x00 */
    uint8_t  r, g, b, code; /* +0x04 */
    int16_t  xy[3][2];   /* +0x08 */
    uint32_t unk_14;
    uint32_t mode;       /* +0x18 PSXM_* | 0x7F80 */
    uint8_t  blend;      /* +0x1C */
    uint8_t  pad_1d[0x23];
    float    pos[3][4];  /* +0x40 */
} PsxPolyF3;             /* 0x70 */

/* code 0x28 flat quad (0x84) */
typedef struct PsxPolyF4 {
    ZoverPolyF4 p;       /* +0x00 mode +0x1C, blend +0x20 */
    uint8_t  pad_24[0x20];
    float    pos[4][4];  /* +0x44 */
} PsxPolyF4;             /* 0x84 */

/* code 0x38 gouraud quad (0x90) */
typedef struct PsxPolyG4 {
    ZoverPolyG4 p;       /* +0x00 mode +0x28, blend +0x2C */
    uint8_t  pad_30[0x20];
    float    pos[4][4];  /* +0x50 */
} PsxPolyG4;             /* 0x90 */

/* code 0x2C textured quad (0x9C) */
typedef struct PsxPolyFT4 {
    ZoverPolyFT4 p;      /* +0x00 texture +0x30, mode +0x34, blend +0x38 */
    float    uvf[4][2];  /* +0x3C */
    float    pos[4][4];  /* +0x5C */
} PsxPolyFT4;            /* 0x9C */

/* corners 1..3 of a GT4 packet */
typedef struct PsxGT4Corner {
    uint8_t  r, g, b, pad; /* +0x00 */
    int16_t  x, y;       /* +0x04 */
    int16_t  u, v;       /* +0x08 */
} PsxGT4Corner;          /* 0x0C */

/* code 0x3C gouraud textured quad (0xA4); blend = tpage bits 5-6 (byte +0x0A) */
typedef struct PsxPolyGT4 {
    uint32_t tag;        /* +0x00 */
    uint8_t  r0, g0, b0, code; /* +0x04 */
    uint16_t clut;       /* +0x08 */
    uint16_t tpage;      /* +0x0A */
    int16_t  x0, y0;     /* +0x0C */
    int16_t  u0, v0;     /* +0x10 */
    PsxGT4Corner c[3];   /* +0x14 corners 1..3 */
    uint32_t texture;    /* +0x38 */
    uint32_t mode;       /* +0x3C */
    uint32_t unk_40;
    float    uvf[4][2];  /* +0x44 */
    float    pos[4][4];  /* +0x64 */
} PsxPolyGT4;            /* 0xA4 */

/* code 0x40 flat line (0x5C) */
typedef struct PsxLineF2 {
    uint32_t tag;        /* +0x00 */
    uint8_t  r, g, b, code; /* +0x04 */
    int16_t  xy[2][2];   /* +0x08 */
    uint32_t unk_10;
    uint32_t mode;       /* +0x14 */
    uint8_t  blend;      /* +0x18 */
    uint8_t  pad_19[0x23];
    float    pos[2][4];  /* +0x3C */
} PsxLineF2;             /* 0x5C */

/* code 0x50 gouraud line (0x60) */
typedef struct PsxLineG2 {
    uint32_t tag;        /* +0x00 */
    uint8_t  r0, g0, b0, code; /* +0x04 */
    int16_t  x0, y0;     /* +0x08 */
    uint8_t  r1, g1, b1, pad_0f; /* +0x0C */
    int16_t  x1, y1;     /* +0x10 */
    uint32_t unk_14;
    uint32_t mode;       /* +0x18 */
    uint8_t  blend;      /* +0x1C */
    uint8_t  pad_1d[0x23];
    float    pos[2][4];  /* +0x40 */
} PsxLineG2;             /* 0x60 */

/* code 0xF8 (the port's own): clip / viewport rectangle (0x18) */
typedef struct PsxClip {
    uint32_t tag;        /* +0x00 */
    uint8_t  pad_04[3];
    uint8_t  code;       /* +0x07 0xF8 */
    int32_t  x, y, w, h; /* +0x08 screen pixels; w = h = 0: no clipping */
} PsxClip;               /* 0x18 */

/* a vertex of psxDrawPoly3D / psxDrawLine3D (0x14) */
typedef struct PsxVert3D {
    float    x, y, z;    /* +0x00 */
    uint32_t pad_0c;
    RwRGBA   colour;     /* +0x10 */
} PsxVert3D;             /* 0x14 */

/* ================================================================== collision (NgIFzbsp.c / NgIFzcollide.c) */

/* A prop def's collision shape (ZpropDefInfo.collision, 0x38, zbspInitPropDefShape). The info part
 * (+0x24) is what zcolGetPropShape returns: game.c / prop.c / rider.c read class, size, yOffset. */
typedef struct NgPropShapeInfo {
    uint8_t  cls;        /* +0x00 NGSHAPE_* (from OBJI byte 0x16: file 1 -> 2, 2 -> 3, 3 -> 1, 4 -> 4) */
    uint8_t  pad_01[3];
    float    size[3];    /* +0x04 half width (extents.x * 50), height (extents.y * 100), half depth */
    float    yOffset;    /* +0x10 (extents.y / 2 + extra[1] + extra[3]) * 100 */
} NgPropShapeInfo;       /* 0x14 */

typedef struct NgPropShape {
    uint16_t flags;      /* +0x00 low 9 bits cleared at creation, else unused */
    uint8_t  surface;    /* +0x02 rail kind / grind sound (0 at creation; nothing sets it on the PC) */
    uint8_t  pad_03;
    uint16_t unk_04;     /* +0x04 0 */
    uint8_t  unk_06;     /* +0x06 0 */
    uint8_t  unk_07;     /* +0x07 0 */
    uint8_t  unk_08[0x1C];
    NgPropShapeInfo info; /* +0x24 */
} NgPropShape;           /* 0x38 */

enum { NGSHAPE_NONE = 0, NGSHAPE_1 = 1 /* "Not coded" */, NGSHAPE_BOX = 2 /* 4 top rails */,
       NGSHAPE_RAIL = 3 /* one rail along the middle */, NGSHAPE_EDGE = 4 /* one rail on an edge */ };

/* RenderWare collision triangle (zrwCol line / sphere tests, 0x4C) [V: NgIFzcollide / NgIFzbsp reads] */
typedef struct ZrwColTri {
    float    normal[4];  /* +0x00 */
    float    v[3][4];    /* +0x10 corners */
    float    t;          /* +0x40 line tests: fraction along the line */
    void    *model;      /* +0x44 the prop model, NULL for the world */
    void    *material;   /* +0x48 RpMaterial (plugin data = surface flags) */
} ZrwColTri;             /* 0x4C */

typedef struct ZrwColHit {
    ZrwColTri tri;       /* +0x00 */
    RwV3d    pos;        /* +0x4C hit point */
    uint32_t pad_58;
} ZrwColHit;             /* 0x5C zrwCol_00494C70 result */

typedef struct ZrwColTriList {
    ZrwColTri *list;     /* +0x00 */
    int32_t    count;    /* +0x04 */
} ZrwColTriList;         /* 8, zrwCol_00494CA0 result */

/* zbspColSphere result */
typedef struct ZbspSphereHit {
    NgColFace  *face;    /* +0x00 NULL ends the list */
    float       dist;    /* +0x04 centre to the plane */
    RwV3d       point;   /* +0x08 nearest point */
    uint32_t    unk_14;
    NgPropCol  *prop;    /* +0x18 the prop hit, or NULL */
} ZbspSphereHit;         /* 0x1C */

typedef struct ZbspSphereResult {
    uint8_t        unk_00[0x3C];   /* zeroed */
    ZbspSphereHit *hits;           /* +0x3C */
} ZbspSphereResult;      /* 0x40 */

/* ================================================================== resources (NgIFzres.c) */

/* an overlay picture of zresAddMaterialToLoadList (0x10) [V] table 0x0055D978 */
typedef struct ZresImageDef {
    int32_t  id;         /* +0x00 resource id 0x30..0x3A */
    int32_t  lib;        /* +0x04 library slot (0 = Material.ZAL) */
    uint16_t asset;      /* +0x08 asset group */
    uint16_t pad_0a;
    uint32_t extra;      /* +0x0C 0, 0x08000000 or 0x10800000 (not read) */
} ZresImageDef;          /* 0x10 */

typedef struct ZresMapHeader {     /* ZMHD */
    uint32_t count;      /* +0x00 */
    uint32_t entrySize;  /* +0x04 100 */
} ZresMapHeader;         /* 8 */

typedef struct ZresPlayerHeader {  /* PLHD */
    uint16_t count;      /* +0x00 */
    uint16_t entrySize;  /* +0x02 0x18 */
    uint32_t unk_04;
} ZresPlayerHeader;      /* 8 */

/* ================================================================== zroller (NgIFzroller.c) */

/* what rider.c hands zrollerCreateRider (4 bytes, copied to ZrollerPlayer.desc) */
typedef struct ZrollerPlayerDesc {
    uint16_t rider;      /* +0x00 g_riderNames index */
    uint8_t  style;      /* +0x02 the model name's last digit - 1 */
    uint8_t  bike;       /* +0x03 */
} ZrollerPlayerDesc;     /* 4 */

typedef struct ZrollerPlayer {
    uint32_t unk_00;
    ZrollerPlayerDesc desc;  /* +0x04 (+4 rider, +6 style, +7 bike) */
    uint32_t unk_08[2];
} ZrollerPlayer;         /* 0x10, 0x005FB6F0 [2] */

/* the shadow pass parameters (0x00678440, the zrwShadowDrawCb argument) [V stores, I meaning] */
typedef struct ZrollerShadows {
    int32_t  count;      /* +0x00 2 */
    void   **models;     /* +0x04 -> the two shadow models' ZGL handles (rider, bike) */
    void    *bikeModel;  /* +0x08 the bike's ZGL handle */
    uint32_t unk_0c;     /* +0x0C 0 */
    float    unk_10;     /* +0x10 -500 */
    uint32_t unk_14;     /* +0x14 0 */
    uint32_t unk_18;
    uint32_t unk_1c;     /* +0x1C 0 */
    float    unk_20;     /* +0x20 -5 */
    uint32_t unk_24;     /* +0x24 0 */
} ZrollerShadows;        /* 0x28 */

/* GHIDRA-SKIP-BEGIN */
typedef char ngif_h_size_check_fx[(sizeof(ZglVertex) == 0x1C && sizeof(ZglLine) == 0x0C &&
                                   sizeof(ZglTri) == 0x10 && sizeof(ZglQuad) == 0x14 &&
                                   sizeof(ZfxParticle) == 0xA0 && sizeof(ZfxSystem) == 0x30 &&
                                   sizeof(ZfxType) == 0x38 && sizeof(ZfxTexAnim) == 0x50) ? 1 : -1];
typedef char ngif_h_size_check_psx[(sizeof(PsxPolyF3) == 0x70 && sizeof(PsxPolyF4) == 0x84 &&
                                    sizeof(PsxPolyG4) == 0x90 && sizeof(PsxPolyFT4) == 0x9C &&
                                    sizeof(PsxPolyGT4) == 0xA4 && sizeof(PsxLineF2) == 0x5C &&
                                    sizeof(PsxLineG2) == 0x60 && sizeof(PsxClip) == 0x18 &&
                                    sizeof(PsxVert3D) == 0x14 && sizeof(PsxGT4Corner) == 0x0C) ? 1 : -1];
typedef char ngif_h_size_check_col[(sizeof(NgPropShapeInfo) == 0x14 && sizeof(NgPropShape) == 0x38 &&
                                    sizeof(ZrwColTri) == 0x4C && sizeof(ZrwColHit) == 0x5C &&
                                    sizeof(ZrwColTriList) == 0x08 && sizeof(ZbspSphereHit) == 0x1C &&
                                    sizeof(ZbspSphereResult) == 0x40) ? 1 : -1];
typedef char ngif_h_size_check_res[(sizeof(ZresImageDef) == 0x10 && sizeof(ZresMapHeader) == 0x08 &&
                                    sizeof(ZresPlayerHeader) == 0x08) ? 1 : -1];
typedef char ngif_h_size_check_zr[(sizeof(ZrollerPlayerDesc) == 0x04 && sizeof(ZrollerPlayer) == 0x10 &&
                                   sizeof(ZrollerShadows) == 0x28) ? 1 : -1];
typedef void (__cdecl *ZrollerDrawFn)(void);
typedef void (__cdecl *ZcardDoneFn)(int unused, int result);

/* ZGL function table (zrwZgl.c / zrwPrim.c / zrwMat.c / zrwCam.c / zrwModel.c entries) as the NgIF
 * code calls it. Names [I] from the arguments. */
typedef int (__cdecl *ZglFrameFn)(void *arg);
extern int      (__cdecl *g_zglAddFrameCallback)(int priority, ZglFrameFn fn, void *arg, int flags); /* 0x00561470 -> 0x0048BA30 */
extern void     (__cdecl *g_zglRemoveFrameCallback)(int handle);   /* 0x00561474 -> 0x0048BAD0 */
extern uint32_t (__cdecl *g_zglGetTime)(void);                     /* 0x00561480 -> 0x00489CB0 (timeGetTime) */
extern void     (__cdecl *g_zglSet)(int what, ...);                /* 0x00561484 -> 0x0048AB10 */
extern void     (__cdecl *g_zglGet)(int what, void *out, int arg); /* 0x00561488 -> 0x0048AD20 */
extern void     (__cdecl *g_zglDrawLines)(uint32_t flags, const ZglLine *prims, int count, int a, int b); /* 0x00561728 -> 0x0048CCF0 */
extern void     (__cdecl *g_zglDrawTris)(uint32_t flags, const ZglTri *prims, int count, int a, int b);   /* 0x0056172C -> 0x0048D120 */
extern void     (__cdecl *g_zglDrawQuads)(uint32_t flags, const ZglQuad *prims, int count, int a, int b); /* 0x00561738 -> 0x0048D660 */
extern void    *(__cdecl *g_zglGetCurrentCamera)(void);            /* 0x00561DB0 -> 0x00493070 */
extern void    *(__cdecl *g_zglCameraGetObj)(void *cam, int which);/* 0x00561DB4 -> 0x00493090 */
extern void     (__cdecl *g_zglCameraSetShake)(void *cam, float x, float y); /* 0x00561DC0 -> 0x00493130 */
extern uint32_t (__cdecl *g_zglMatCreateFromImage)(const void *image, uint32_t size, const char *name, int a, int b); /* 0x00561048 */
extern void     (__cdecl *g_pfnZglMatDestroy)(uint32_t mat);       /* 0x00561024 -> 0x00489530 */
extern uint32_t (__cdecl *g_zglMatFind)(uint32_t hash, int arg);   /* 0x0056104C -> 0x00489A00 */
extern void    *(__cdecl *g_zglMatGetTexture)(void *mat, int arg); /* 0x00561034 -> 0x00489A40 */
extern void     (__cdecl *g_zglMatSetTexture)(void *mat, void *tex, int arg); /* 0x0056102C -> 0x00489590 */
extern void     (__cdecl *g_zrwModelSetFlags)(void *model, uint32_t set, uint32_t clear); /* 0x00560ED8 -> 0x004888C0 */
extern void    *(__cdecl *g_zglModelFindMaterial)(void *model, uint32_t hash); /* 0x00560EE8 -> 0x00488BD0 */
extern void     (__cdecl *g_zglModelSetTint)(void *model, const void *rgba);   /* 0x00560EF4 -> 0x00488CA0 */
extern int      (__cdecl *g_pfnDebugPrintAt)(int16_t x, int16_t y, const char *fmt, ...); /* 0x00560038 -> zdebugPrint (DM-064: was (x, y, colour, text)) */

/* "not implemented" stubs: MissingZGE.c, NgIFzbsp.c, NgIFzcollide.c, NgIFzroller.c print
 * "%s %d %s\n" with __FILE__, __LINE__ and a description (0x0055BAB8). */
#define NGIF_MISSING(file, line, what) g_pfnDebugPrintf("%s %d %s\n", (file), (line), (what))

/* ------------------------------------------------------------------ MissingZGE.c */
void      ngappEmptyHook(void);                                           /* 0x0045A1A0 */
void      zconsoleLog(const char *msg);                                   /* 0x0045A1C0 */
void      zglEnableHorizon(void);                                         /* 0x0045A1F0 */
void      zglDisableHorizon(void);                                        /* 0x0045A220 */
void      zsystemSetActionMode(void);                                     /* 0x0045A250 */
void      zsystemSetMenuMode(void);                                       /* 0x0045A290 */
void      zfxDestroyAll(uint32_t which);                                  /* 0x0045A2D0 */
int       zfxRegisterFuncTable(void *const *funcs, int count);            /* 0x0045A300 */

/* ------------------------------------------------------------------ NgApp.c */
extern uint8_t g_ngappMode;                                               /* 0x005AB3A0 NGAPP_MODE_* */
#define NGAPP_MODE_MENU   1
#define NGAPP_MODE_ACTION 2
void      ngappWorldToCamera(const RwV3d *world, RwV3d *out);            /* 0x0045A340 */
int       ngappUnusedStub1(void);                                         /* 0x0045A380 */
int       ngappUnusedStub2(void);                                         /* 0x0045A3A0 */
int       ngappPlayMovie(const char *const *const *movie);                /* 0x0045A3C0 */
int       ngappDebugDrawTags(void *arg);                                  /* 0x0045A4D0 */
void      ngappDebugDrawOn(void);                                         /* 0x0045AB50 */
void      ngappDebugDrawOff(void);                                        /* 0x0045AB90 */
void      ngappPrintVersion(void);                                        /* 0x0045ABC0 */
void      ngappSetupLightProps(void);                                     /* 0x0045AC40 */
void      ngappApplyPropTints(void);                                      /* 0x0045AC90 */
void      ngappCreateDefaultLights(void);                                 /* 0x0045ACE0 */
void      ngappCreateLevelLights(void);                                   /* 0x0045ADC0 */
ZtagFace *ngappFindFaceWithEdge(ZtagFace *faces, uint32_t code);          /* 0x0045AF40 */
void      ngappLinkSpineFaces(void);                                      /* 0x0045AFB0 */
void      ngappLoadInterfaceArt(void);                                    /* 0x0045B020 */
void      ngappFreeInterfaceArt(void);                                    /* 0x0045B240 */
int       ngappInit(void);                                                /* 0x0045B270 */
int       ngappShutdown(void);                                            /* 0x0045B4E0 */
void      ngappShowLoadingScreen(void);                                   /* 0x0045B5C0 */
int       ngappDrawFullScreen(uint32_t material);                         /* 0x0045B6E0 */
int       ngappFrame(float dt);                                           /* 0x0045B8A0 */
void      ngappUpdateAmbientLight(void);                                  /* 0x0045C040 */
int       ngappDrawLoadingBar(void *arg);                                 /* 0x0045C150 */
void      ngappLoadAnimTable(const char *name, int a, int b, void **table, int size); /* 0x0045C690 */
float     ngappGetTime(void);                                             /* 0x0045C6D0 */
void      ngappNop(void);                                                 /* 0x0045C6F0 */
void      ngappUpdateFarClip(void);                                       /* 0x0045C710 */
void      ngappSetFarClip(float distance);                                /* 0x0045C920 */

/* ------------------------------------------------------------------ NgIFfx.c */
void      zfxSparkSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045C950 */
int       zfxSparkDraw(void *type, ZfxSystem *sys);                       /* 0x0045CC50 */
void      zfxShardSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045D260 */
int       zfxShardDraw(void *type, ZfxSystem *sys);                       /* 0x0045D420 */
int       zfxSplashDraw(void *type, ZfxSystem *sys);                      /* 0x0045D730 */
void      zfxDustSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045DA40 */
int       zfxDustDraw(void *type, ZfxSystem *sys);                        /* 0x0045DB20 */
ZfxSystem *zfxPosterCreate(void *owner, uint32_t capacity);              /* 0x0045DE30 */
void      zfxPosterSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045DEA0 */
int       zfxPosterDraw(void *type, ZfxSystem *sys);                      /* 0x0045E010 */
void      zfxRippleSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045E210 */
int       zfxRippleDraw(void *type, ZfxSystem *sys);                      /* 0x0045E2D0 */
int       zfxRippleUpdate(void *type, ZfxSystem *sys);                    /* 0x0045E4E0 */
void      zfxLightningSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p); /* 0x0045E750 */
int       zfxLightningDraw(void *type, ZfxSystem *sys);                   /* 0x0045EAA0 */
void      zfxSpawnSpark(void *emitter, void *params);                     /* 0x0045F030 */
void      zfxSpawnShard(void *emitter, void *params);                     /* 0x0045F0A0 */
void      zfxSpawnSplash(void *emitter, void *params);                    /* 0x0045F0D0 */
void      zfxSpawnDust(void *emitter, void *params);                      /* 0x0045F140 */
void      zfxSpawnPoster(void *emitter, void *params);                    /* 0x0045F190 */
void      zfxSpawnRipple(void *emitter, void *params);                    /* 0x0045F1E0 */
void      zfxSpawnSkid(void *emitter, void *params);                      /* 0x0045F2D0 */
void      zfxSpawnLightning(void *emitter);                               /* 0x0045F340 */
void     *zfxGetSystem(void *actor, int kind);                                            /* 0x0045F480 */
int       zfxInitTexAnims(void);                                          /* 0x0045F4A0 */
int       zfxUpdateTexAnims(void *arg);                                   /* 0x0045F650 */
void      zfxInitTypes(void);                                             /* 0x0045F780 */
void      zfxShutdownTypes(void);                                         /* 0x0045FC10 */
int       zfxCreate(int effect, void *params, int arg);                                 /* 0x0045FCB0 */
int       zfxShakeUpdate(void *arg);                                      /* 0x0045FCF0 */
void      zfxFlash(const void *shake);                                 /* 0x0045FDC0 */
/* ------------------------------------------------------------------ NgIFpsx.c */
extern uint32_t    *g_psxOrderingTable;    /* 0x0055C698 the ordering table (0x400 entries, ClearOTagR) */
extern ZoverTexture g_psxTextures[];       /* 0x0068C3E0 image slots (NgIFzres.c fills them) */
extern float        g_psxScreenW;          /* 0x0055C690 */
extern float        g_psxScreenH;          /* 0x0055C694 */
void      psxSetClipPrim(PsxClip *p, const int16_t *rect);                /* 0x0045FE00 */
void      psxDrawClip(const PsxClip *p);                                  /* 0x0045FE50 */
void      psxClearOTagR(uint32_t *ot, int16_t n);                         /* 0x0045FF60 */
void      psxDrawPolyGT4(const PsxPolyGT4 *p, int semi);                  /* 0x0045FFD0 */
void      psxDrawPolyG4(const PsxPolyG4 *p, int semi);                    /* 0x00460640 */
void      psxDrawPolyFT4(const PsxPolyFT4 *p, int semi);                  /* 0x00460B40 */
void      psxDrawPolyF4(const PsxPolyF4 *p, int semi);                    /* 0x004611D0 */
void      psxDrawPolyF3(const PsxPolyF3 *p, int semi);                    /* 0x00461700 */
void      psxDrawLineF2(const PsxLineF2 *p, int semi);                    /* 0x00461AF0 */
void      psxDrawLineG2(const PsxLineG2 *p, int semi);                    /* 0x00461DA0 */
void      psxDrawPrim(const void *p);                                     /* 0x00462050 */
void      psxDrawOTag(const uint32_t *ot);                                /* 0x00462350 */
void      psxDrawRect(uint32_t colourOrSlot, int x, int y, uint32_t flags, const int32_t *args); /* 0x004623A0 */
void      psxDrawSpriteList(int16_t x, int16_t y, const uint8_t *records); /* 0x00462940 */
void      psxDrawPoly3D(uint32_t mode, uint8_t n, const PsxVert3D *v, uint16_t slot); /* 0x00463130 */
void      psxDrawLine3D(uint8_t mode, const PsxVert3D *a, const PsxVert3D *b); /* 0x00463710 */
void      psxNop(void);                                                   /* 0x00463800 */
void      psxBeginScene(void);                                            /* 0x00463820 */
int       psxSceneDraw(void *arg);                                        /* 0x00463860 */
void      psxGetScreenSize(void);                                         /* 0x004638B0 */
void      psxEnableDraw(void);                                            /* 0x004638F0 */
void      psxDisableDraw(void);                                           /* 0x00463930 */
uint32_t *psx2DGetDepth(void);                                            /* 0x00463960 */
void      psx2DSetDepth(uint32_t *ot);                                    /* 0x00463980 */
int       psx2DDraw(void *arg);                                           /* 0x004639A0 */
int       psx2DInit(void);                                                /* 0x004639F0 */
void      psx2DShutdown(void);                                            /* 0x00463A60 */
void      psx2DBeginFrame(void);                                          /* 0x00463AB0 */
void      psx2DRequestDraw(void);                                         /* 0x00463B00 */
void      psx2DDrawPicture(const ZprimImage *img);                        /* 0x00463B20 */
void      psx2DSetTint(uint32_t rgb, uint32_t level);                     /* 0x00463C40 */
void      psx2DDrawGlyph(ZprimFont *font, const ZprimGlyph *g, int mode, uint32_t arg1, uint32_t arg2,
                         int x, int y, int arg14, uint32_t palette);      /* 0x00463C70 */
void      psx2DSetClip(int x0, int y0, int x1, int y1);                   /* 0x00464030 */
void      psxNop2(void);                                                  /* 0x00464200 */
void      psx2DDrawSprite(const ZprimImage *img, uint32_t frame, const void *palette, int mode, int zero,
                          uint32_t arg, int abr, int x, int y, int w, int h, int bpp, uint32_t arg30); /* 0x00464220 */
void      psx2DFillRect(uint32_t colour, int mode, int x0, int y0, int x1, int y1); /* 0x00464420 */
void      psx2DDrawLine(uint32_t colour, int mode, int x0, int y0, int x1, int y1); /* 0x004645D0 */
int       psxUnusedTrue1(void);                                           /* 0x004646B0 */
int       psxUnusedTrue2(void);                                           /* 0x004646D0 */
/* ------------------------------------------------------------------ NgIFzactor.c */
void      NgIFzactor_ResetPartTables(void);                                /* 0x004646F0 */
void      NgIFzactor_CacheRiderParts(ZActor *a, uint32_t player);          /* 0x00464740 */
void      NgIFzactor_CacheBikeParts(ZActor *a, uint32_t player);           /* 0x004649A0 */
void      zactorGetAbsolutePartPosition(ZActor *a, uint16_t part, const float *local, float *out); /* 0x00464B50 */
void      zactorGetAbsolutePartPositionNoPose(ZActor *a, uint16_t part, const float *local, float *out); /* 0x00464D10 */
float     zactorGetPartLength(ZActor *a, uint32_t part);                   /* 0x00464EA0 */
void     *zactorGetPartFrame(ZActor *a, uint32_t part, uint32_t player);   /* 0x00464FB0 */
void      zactorGetPartOffset(ZActor *a, uint32_t part, RwV3d *out, uint32_t player); /* 0x00465040 */
void      NgIFzactor_SetRiderChannels(ZActor *a);                          /* 0x00465130 */
void      NgIFzactor_SetBikeChannels(ZActor *a);                           /* 0x00465340 */
void      NgIFzactor_ApplyMotionPose(ZActor *a);                           /* 0x004654B0 */
void      NgIFzactor_PosePlayer(void);                                     /* 0x004659E0 */
/* ------------------------------------------------------------------ NgIFzbsp.c */
void      zbspPropSetRotation(NgPropCol *col, const int16_t *euler);       /* 0x004661C0 */
void      zbspPropAddRotation(NgPropCol *col, const int16_t *euler);       /* 0x00466230 */
void      zbspPropSetPosition(NgPropCol *col, const float *pos);           /* 0x004662B0 */
void      zbspInitPropDefShape(ZpropDefInfo *def);                         /* 0x00466390 */
int       zbspGetPropDefIndex(const ZpropDefInfo *def);                    /* 0x004664E0 */
ZpropDefInfo *zbspGetPropDefInfo(int index);                               /* 0x00466550 */
NgPropShape *zbspGetPropShape(uint16_t index);                             /* 0x004665C0 */
int       zbspUnusedStub(void);                                            /* 0x004665F0 */
void      zbspPropSetMode(NgPropCol *col, int mode);                       /* 0x00466610 */
const uint32_t *zbspFaceMaterial(const NgColFace *face);                   /* 0x00466BB0 */
void      zbspFaceCentroid(const NgColFace *face, RwV3d *out);             /* 0x00466C50 */
void      zbspClosestPointOnSegment(const float *a, const float *b, const float *p, float *out); /* 0x00466D70 */
NgColEdge **zbspFindEdgesNear(const float *pos, float radius);             /* 0x00466ED0 */
NgColEdge *zbspGetTaggedEdge(uint32_t ctx, int16_t code, int index);       /* 0x00467410 */
int       zbspEdgeOpenEnd(const NgColEdge *e);                             /* 0x00467690 */
int       zbspFacePointInside(const NgColFace *face, float x, float y, float z); /* 0x00467A90 */
ZbspSphereResult *zbspColSphere(const float *pos, float radius, float maxNy); /* 0x00468070 */
void      zbspEdgeClosestPoint(const int16_t *v0, const int16_t *v1, const float *dir, const float *p, float *out); /* 0x00468920 */
NgPropCol *zbspPropColCreate(Zprop *z);                                    /* 0x00468BC0 */
void      zbspPropColFree(NgPropCol *col);                                 /* 0x00468D20 */
NgPropCol **zbspFindPropsByGroup(uint16_t lo, uint16_t hi);                /* 0x00468E30 */
NgColFace **zbspFindTaggedFaces(int lo, int hi);                           /* 0x00468EF0 */
void      zbspMakeTagFace(const ZtagFace *f, const uint16_t *tag, NgColFace *face); /* 0x00469080 */
NgColEdge **zbspFindTaggedEdges(uint16_t lo, uint16_t hi);                 /* 0x004691C0 */
/* ------------------------------------------------------------------ NgIFzcollide.c */
const NgPropShapeInfo *zcolGetPropShape(uint32_t defIndex);                /* 0x00469530 */
void    **zcolFindEdgesAlong(const float *pos, const RwV3d *dir, float radius); /* 0x00469560 */
int       zcolSegmentTest(const float *from, const float *to, NgColHit *hit); /* 0x00469B00 */
float     zcolProbeDown(const float *pos, uint32_t *out);                  /* 0x0046A160 */
int       zcolCreatePropRails(NgPropCol *col, NgColEdge **out);            /* 0x0046A3F0 */
int       zcolBuildPropRails(NgPropCol *col, NgColEdge *rails);            /* 0x0046A4D0 */
void      zcolMakeRail(NgColEdge *e, uint8_t kind, const int16_t *a, const int16_t *b, int open); /* 0x0046AA50 */
void      zcolPropRotated(NgPropCol *col, const int16_t *euler);           /* 0x0046AC40 */
void      zcolPropMoved(NgPropCol *col, const float *pos);                 /* 0x0046AC70 */
/* ------------------------------------------------------------------ NgIFzres.c */
int       zresLoadMapTable(void);                                          /* 0x0046B7C0 */
void     *zresLoadAsset(ZassetLib *lib, uint32_t group, uint32_t asset, uint32_t *size, int second); /* 0x0046B8B0 */
ZassetLib *zresGetLibrary2(void);                                          /* 0x0046B950 */
uint32_t  zresGetMapCount(void);                                           /* 0x0046B970 */
const uint8_t *zresGetMap(uint32_t index);                                 /* 0x0046B990 */
int       zresLoadPlayerTable(void);                                       /* 0x0046BA10 */
const uint8_t *zresGetPlayer(uint16_t index);                              /* 0x0046BB00 */
uint32_t  zresGetPlayerCount(void);                                        /* 0x0046BBA0 */
void     *zresGetPlayerHeaderArea(void);                                   /* 0x0046BBC0 */
const uint32_t *zresGetBikeEntry(uint32_t bike);                           /* 0x0046BBE0 */
int       zresLoadTables(void);                                            /* 0x0046BC20 */
void      zresCloseLibraries(void);                                        /* 0x0046BC40 */
void      zresOpenLibraries(void);                                         /* 0x0046BCD0 */
int       zresInit(void);                                                  /* 0x0046BE10 */
void      zresShutdown(void);                                              /* 0x0046BE30 */
void      zresNop(void);                                                   /* 0x0046BED0 */
int       zresReset(void);                                                 /* 0x0046BEF0 */
void      zresAddMaterialToLoadList(int res, int unused, uint32_t slot);   /* 0x0046BF30 */
void      zresFreeImages(void);                                            /* 0x0046C1D0 */
/* ------------------------------------------------------------------ NgIFzroller.c */
extern ZrollerShadows g_zrollerShadows;                                    /* 0x00678440 */
extern zHeapList *g_zrollerEdges;                                          /* 0x00678494 */
extern int        g_zrollerLevelLoaded;                                    /* 0x005FB73C */
extern int        g_zrollerFrameHandle;                                    /* 0x005FB740 */
int       zrollerStartup(void);                                            /* 0x0046C220 */
int       zrollerInit(void);                                               /* 0x0046C310 */
int       zrollerShutdown(void);                                           /* 0x0046C360 */
void      zrollerFreeRecordCb(void *elem, void *pool);                     /* 0x0046C380 */
void      zrollerUnloadLevel(void);                                        /* 0x0046C3B0 */
int       zrollerRunDrawCallbacks(void);                                   /* 0x0046C420 */
int       zrollerLoadAssets(void);                                         /* 0x0046C4B0 */
ZActor   *zrollerCreateRider(const ZrollerPlayerDesc *desc);               /* 0x0046C580 */
ZActor   *zrollerCreateBike(const uint32_t *bike);                         /* 0x0046C740 */
ZrollerPlayer *zrollerGetPlayer(uint32_t index);                           /* 0x0046C920 */
void      zrollerInitializeLoadingScreen(void);                            /* 0x0046CB60 */
void      zrollerShutdownLoadingScreen(void);                              /* 0x0046CB90 */
void      zrollerDisplayLoadingScreen(void);                               /* 0x0046CBC0 */
void      zrollerSetLocal(void);                                           /* 0x0046CBF0 */
int       zrollerLoadCodeModule(void);                                     /* 0x0046CC50 */
void      zrollerResetImageSlots(void);                                    /* 0x0046CC80 */
int       zrollerAllocImageSlots(int count);                               /* 0x0046CCD0 */
void      zrollerSetMap(void);                                             /* 0x0046CD60 */
float    *zrollerGetWorldBounds(void);                                     /* 0x0046CEB0 */
void      zrollerGetPlayerStartPos(int player, float *pos, int16_t *rot);  /* 0x0046CF30 */
int       zrollerUnusedStub1(void);                                        /* 0x0046D350 */
void      zrollerNop1(void);                                               /* 0x0046D3A0 */
void      zrollerNop2(void);                                               /* 0x0046D3C0 */
void      zrollerStartCrossFade(void);                                     /* 0x0046D590 */
void      zrollerAddDrawCallback(ZrollerDrawFn fn);                        /* 0x0046D690 */
int       zrollerUnusedStub2(void);                                        /* 0x0046D6E0 */
void      zrollerUnusedStub3(void);                                        /* 0x0046D700 */
void      zcardSelectPort(uint8_t port);                                   /* 0x0046D720 */
int       zcardCheck(ZcardDoneFn done);                                      /* 0x0046D7A0 */
int       zcardFormat(void);                                               /* 0x0046D8A0 */
void      zcardStart(void);                                                /* 0x0046D8C0 */
void      zcardStop(void);                                                 /* 0x0046D8E0 */
/* the "not implemented" stubs (print "%s %d %s" and return 0 / 1 / nothing) */
int       zbspInit(void);                                           /* 0x00465A50 */
void      zbspUpdate(void);                                         /* 0x00465A80 */
int       zbspShutdown(void);                                       /* 0x00465AB0 */
int       zbspInitializeScene(void);                                /* 0x00465AE0 */
int       zbspLoadScene(void);                                      /* 0x00465B10 */
void      zbspLoadAndProcessMap(void);                              /* 0x00465B40 */
int       zbspAllocAndLoadMap(void);                                /* 0x00465B70 */
void      zbspFreeMap(void);                                        /* 0x00465BA0 */
int       zbspSetTreeVisible(void);                                 /* 0x00465BD0 */
void      zbspConvertTreeFromLoad(void);                            /* 0x00465C00 */
void      zbspRelativeLodZ(void);                                   /* 0x00465C40 */
void      zbspPreprocessScene(void);                                /* 0x00465C70 */
void      zbspPreprocessTerrainNode(void);                          /* 0x00465CA0 */
void      zbspPreprocessFaceList(void);                             /* 0x00465CD0 */
int       zbspViewerInPosSide(void);                                /* 0x00465D00 */
void      zbspSndRegister(void);                                    /* 0x00465D40 */
void      zbspSndSetState(void);                                    /* 0x00465D70 */
int       zbspSndGetHandle(void);                                   /* 0x00465DA0 */
int       zbspDvlitehandle(void);                                   /* 0x00465DD0 */
int       zbspDvlitegetcolor(void);                                 /* 0x00465E00 */
void      zbspDvlitelite(void);                                     /* 0x00465E40 */
void      zbspDvliteinitialize(void);                               /* 0x00465E70 */
void      zbspDvliteAttachToProps(void);                            /* 0x00465EA0 */
void      zbspPropActivate(void);                                   /* 0x00465ED0 */
void      zbspPropDeactivate(void);                                 /* 0x00465F00 */
void      zbspActivateTree(void);                                   /* 0x00465F30 */
void      zbspDeactivateTree(void);                                 /* 0x00465F60 */
int       zbspInitializeTreeRef(void);                              /* 0x00465F90 */
void      zbspCreateTreeRef(void);                                  /* 0x00465FC0 */
void      zbspDestroyTreeRef(void);                                 /* 0x00466000 */
void      zbspPropAddTreeRef(void);                                 /* 0x00466030 */
void      zbspRemoveTreeRef(void);                                  /* 0x00466060 */
void      zbspUpdateTreeRef(void);                                  /* 0x00466090 */
void      zbspAddToTree(void);                                      /* 0x004660C0 */
void      zbspRemoveFromTree(void);                                 /* 0x004660F0 */
int       zbspTotallyWithinTree(void);                              /* 0x00466120 */
int       zbspOverlapsTree(void);                                   /* 0x00466160 */
int       zbspFindTagged(void);                                     /* 0x00466190 */
void      zbspPropMove(void);                                       /* 0x00466320 */
int       zbspGetNumDefs(void);                                     /* 0x00466350 */
int       zbspGetTypeFromHash(void);                                /* 0x004664B0 */
void      zbspFxCreateTree(void);                                   /* 0x004666E0 */
void      zbspDestroyTree(void);                                    /* 0x00466710 */
void      zbspDrawTerrain(void);                                    /* 0x00466740 */
void      zbspDrawTerrainNode(void);                                /* 0x00466770 */
void      zbspDrawFaceList(void);                                   /* 0x004667A0 */
void      zbspSetDebug(void);                                       /* 0x004667D0 */
void      zbspDrawLineZ(void);                                      /* 0x00466800 */
void      zbspDrawFaceNormal(void);                                 /* 0x00466830 */
void      zbspDrawFaceFlat(void);                                   /* 0x00466860 */
void      zbspShowBoundingBox(void);                                /* 0x00466890 */
void      zbspShowEdges(void);                                      /* 0x004668C0 */
void      zbspShowPropVolume(void);                                 /* 0x004668F0 */
int       zbspGetFaceNum(void);                                     /* 0x00466920 */
void      zbspTrnDumpTree(void);                                    /* 0x00466960 */
void      zbspShowPartition(void);                                  /* 0x00466990 */
void      zbspShowPartition2(void);                                 /* 0x004669C0 */
void      zbspDrawPartition(void);                                  /* 0x004669F0 */
int       zbspGetTerrainFaces(void);                                /* 0x00466A20 */
int       zbspGetTreeObject(void);                                  /* 0x00466A50 */
int       zbspPointWithinTreeObject(void);                          /* 0x00466A80 */
int       zbspGetTreeObjects(void);                                 /* 0x00466AB0 */
void      zbspGetTerrainNode(void);                                 /* 0x00466AE0 */
int       zbspGetMaterialInfo(void);                                /* 0x00466B10 */
int       zbspGetMatRefData(void);                                  /* 0x00466B40 */
int       zbspGetMatIdx(void);                                      /* 0x00466B70 */
int       zbspMatRefList(void);                                     /* 0x00466BF0 */
int       zbspFindTagged2(void);                                    /* 0x00466C20 */
int       zbspDoesVertExist(void);                                  /* 0x00466CA0 */
int       zbspFacesAtSamePos(void);                                 /* 0x00466CD0 */
int       zbspSceneSaveState(void);                                 /* 0x00466D10 */
void      zbspSceneRestoreState(void);                              /* 0x00466D40 */
void      zbspGetNormalVector(void);                                /* 0x004676C0 */
int       zbspGetSideVector(void);                                  /* 0x004676F0 */
int       zbspBspcollide(void);                                     /* 0x00467730 */
void      zbspCreateSpanPlanes(void);                               /* 0x00467760 */
void      zbspAddToFacelist(void);                                  /* 0x00467790 */
void      zbspGetTerrainHeightinf(void);                            /* 0x004677C0 */
void      zbspTestRayCollision(void);                               /* 0x004677F0 */
void      zbspBsptest(void);                                        /* 0x00467820 */
void      zbspTestresetfacecolors(void);                            /* 0x00467850 */
void      zbspHilightnodefaces(void);                               /* 0x00467880 */
void      zbspSetFaceRgb(void);                                     /* 0x004678B0 */
void      zbspPrintFaceListInf(void);                               /* 0x004678E0 */
int       zbspTrntestraycollision(void);                            /* 0x00467910 */
void      zbspCreatenodeshit(void);                                 /* 0x00467940 */
void      zbspAddToNodeslist(void);                                 /* 0x00467970 */
void      zbspAddToFacesList(void);                                 /* 0x004679A0 */
int       zbspLineToNodes(void);                                    /* 0x004679D0 */
int       zbspLineToFaces(void);                                    /* 0x00467A10 */
int       zbspLineplaneintersection(void);                          /* 0x00467A50 */
int       zbspClipRayToObject(void);                                /* 0x00467D30 */
int       zbspClipLineToProps(void);                                /* 0x00467D70 */
int       zbspCliplinetoprop(void);                                 /* 0x00467DB0 */
void      zbspBuildrayboundingbox(void);                            /* 0x00467DE0 */
void      zbspAddpropfaces(void);                                   /* 0x00467E10 */
void      zbspComputeFacePlaneEq(void);                             /* 0x00467E50 */
void      zbspTaginitialize(void);                                  /* 0x00467E80 */
void      zbspTagregisterprofunc(void);                             /* 0x00467EB0 */
int       zbspFacetagregister(void);                                /* 0x00467EE0 */
int       zbspFacetagpreprocess(void);                              /* 0x00467F10 */
int       zbspFacetagprocess(void);                                 /* 0x00467F40 */
int       zbspFacetagfinddef(void);                                 /* 0x00467F70 */
int       zbspFacetagregregister(void);                             /* 0x00467FA0 */
int       zbspFacetagpreattachasref(void);                          /* 0x00467FD0 */
int       zbspFacetagpreattachsmposasref(void);                     /* 0x00468000 */
int       zbspFacetagproswapwindow(void);                           /* 0x00468030 */
void      zbspColspherecreatenodeshit(void);                        /* 0x00468870 */
int       zbspHitnodeslist(void);                                   /* 0x004688A0 */
int       zbspTestwithfacelist(void);                               /* 0x004688E0 */
void      zbspFaceboundingbox(void);                                /* 0x00468B50 */
void      zbspColSphereDraw(void);                                  /* 0x00468B90 */
int       zbspFindFaceByType(void);                                 /* 0x00469500 */
int       zcolbboxcollision(void);                                  /* 0x0046A120 */
int       zcolGetTerrainFace(void);                                 /* 0x0046A2C0 */
int       zcolPropHitBox(void);                                     /* 0x0046A2F0 */
void      zcolGetPlaneHeight(void);                                 /* 0x0046A320 */
void      zcolGenerateBoundBox(void);                               /* 0x0046A360 */
void      zcolInitPropFacesList(void);                              /* 0x0046A390 */
void      zcolGeneratePropBoundingVolume(void);                     /* 0x0046A3C0 */
void      zcolPropMove(void);                                       /* 0x0046AC10 */
int       zrollerUpdateZGL(void);                                   /* 0x0046C450 */
int       zrollerCreateChaser(void);                                /* 0x0046C480 */
int       zrollerCreateProp(void);                                  /* 0x0046C860 */
int       zrollerSetEngineState(void);                              /* 0x0046C890 */
int       zrollerGetNumPlayerDefs(void);                            /* 0x0046C8C0 */
int       zrollerLoadMdlPlayerCB(void);                             /* 0x0046C8F0 */
int       zrollerIsPlayerClose(void);                               /* 0x0046C950 */
void      zrollerChangePlayerModeLook(void);                        /* 0x0046C980 */
void      zrollerAddPlayerMatsToLoadList(void);                     /* 0x0046C9B0 */
void      zrollerChangePlayerSponsor(void);                         /* 0x0046C9E0 */
void      zrollerChangeBoardSponsor(void);                          /* 0x0046CA10 */
int       zrollerLoadMdlGenericCB(void);                            /* 0x0046CA40 */
int       zrollerLoadMdlCommon(void);                               /* 0x0046CA70 */
void      zrollerLoadMdlCommonMat(void);                            /* 0x0046CAA0 */
void      zrollerProcessModelFlags(void);                           /* 0x0046CAD0 */
void      zrollerProcessActorDrawFlags(void);                       /* 0x0046CB00 */
void      zrollerSetAnimMaterialPalette(void);                      /* 0x0046CB30 */
int       zrollerLoadModule(void);                                  /* 0x0046CC20 */
void      zrollerAllocMaterials(void);                              /* 0x0046CCA0 */
int       zrollerAddGenericMaterialsToLoad(void);                   /* 0x0046CD00 */
void      zrollerInitializeMaps(void);                              /* 0x0046CD30 */
int       zrollerLoadMaps(void);                                    /* 0x0046CD90 */
int       zrollerLoadMatTerrainFirstCB(void);                       /* 0x0046CDC0 */
int       zrollerLoadMatTerrainLastCB(void);                        /* 0x0046CDF0 */
void      zrollerAssociateMapMaterials(void);                       /* 0x0046CE20 */
void      zrollerAssociateMaterial(void);                           /* 0x0046CE50 */
void      zrollerAssociateMaterialPalAnimation(void);               /* 0x0046CE80 */
int       zrollerGetPlayerRestartPos(void);                         /* 0x0046D200 */
void      zrollerInitPropLoadList(void);                            /* 0x0046D230 */
void      zrollerAddPropToLoadList(void);                           /* 0x0046D260 */
void      zrollerCreateProps(void);                                 /* 0x0046D290 */
void      zrollerLoadPropMaterials(void);                           /* 0x0046D2C0 */
int       zrollerCreatePropActor(void);                             /* 0x0046D2F0 */
int       zrollerDestroyActor(void);                                /* 0x0046D320 */
int       zrollerGetAsyncKeyState(void);                            /* 0x0046D370 */
int       zrollerSelectMap(void);                                   /* 0x0046D3E0 */
int       zrollerSelectPlayer(void);                                /* 0x0046D410 */
int       zrollerSelectBikeNum(void);                               /* 0x0046D440 */
void      zrollerSetFogDistance(void);                              /* 0x0046D470 */
void      zrollerGetFogDistance(void);                              /* 0x0046D4A0 */
void      zrollerDisableFog(void);                                  /* 0x0046D4D0 */
void      zrollerSetFogColor(void);                                 /* 0x0046D500 */
void      zrollerReplaceMaterial(void);                             /* 0x0046D530 */
void      zrollerSetShadeMode(void);                                /* 0x0046D560 */
int       zrollerGetSceneState(void);                               /* 0x0046D5C0 */
void      zrollerFreeSceneState(void);                              /* 0x0046D600 */
void      zrollerSetSceneState(void);                               /* 0x0046D630 */
int       zrollerSelectLook(void);                                  /* 0x0046D660 */
/* GHIDRA-SKIP-END */

#endif /* NGIF_H */

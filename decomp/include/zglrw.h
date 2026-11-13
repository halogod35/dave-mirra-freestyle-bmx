/*
 * zglrw.h - the ZglRw layer (NgEngine/Zge/Com/ZglRw/zrw*.c): the PC implementation of the engine's
 * "ZGL" graphics interface on top of RenderWare 3.1. Description: docs/re/zrw.md.
 *
 * The engine core never calls RenderWare. Each zrw module fills a driver table (a const array of
 * function pointers in .data) and zrwZglInstallDrivers 0x0048AFE0 hands the tables to the engine:
 *
 *   table       VA          slots  module            engine global (pointer to the table)
 *   texture     0x005623F0  15     zrwTxr.c          g_texDriverVtbl (the table itself, older name)
 *   material    0x00561018  15     zrwMat.c          g_zglMatApi      0x006977A0
 *   model       0x00560E78  37     zrwModel.c (+ zrwModelCreate.c, zrwModelRender.c, zrwModelGeom.c)
 *   light       0x00563420   8     zrwLight.c
 *   collision   0x00562060   4     zrwCol.c
 *   primitives  0x00561720   7     zrwPrim.c
 *   camera      0x00561D88  15     zrwCam.c          (zobjCamera.c binds it)
 *   system      0x00561468  13     zrwZgl.c          (zge.c binds it)
 *   debug       0x00560030  20     zrwDbg.c          (zdebug.c binds it, g_pfnDebug* slots)
 *
 * Every ZGL object (texture, material, model, ...) starts with a 32-bit word whose low byte is the
 * object type (ZGL_TYPE_*) and is reference counted. RenderWare objects are opaque here (void *);
 * the RenderWare 3.1 functions the layer calls are listed at the end with their VAs (library code,
 * names [I] from the RW 3.x SDK and their use).
 *
 * Offsets [V] (field use in the zrw code), names [I] unless noted.
 */
#ifndef ZGLRW_H
#define ZGLRW_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "zmem.h"
#include "zlist.h"
#include "ngif.h"
#include "zanim.h"
#include "zmath.h"

/* ZGL object types (low byte of word 0) [V: the constructors] */
#define ZGL_TYPE_MODEL     0
#define ZGL_TYPE_CAMERA    1
#define ZGL_TYPE_LIGHT     2
#define ZGL_TYPE_MATERIAL  3
#define ZGL_TYPE_TEXTURE   4

/* zrwTxr_CreateTextureFromMemory image types (EzglImageTypes) [V: 0x00497B00] */
#define ZGL_IMAGE_BMP  0
#define ZGL_IMAGE_PNG  1
#define ZGL_IMAGE_DDS  4

/* ================================================================== zrwTxr.c */

/* A ZGL texture: a reference-counted wrapper of one RwTexture (0x28, free list of 256) */
typedef struct ZglTexture {
    uint32_t    type;          /* +0x00 low byte ZGL_TYPE_TEXTURE */
    uint32_t    unk04;         /* +0x04 never written by zrwTxr */
    int32_t     refCount;      /* +0x08 */
    uint32_t    nameHash;      /* +0x0C zHashName(name) */
    const char *name;          /* +0x10 RwTextureGetName (the RW name buffer) */
    int32_t     width;         /* +0x14 */
    int32_t     height;        /* +0x18 */
    int32_t     depth;         /* +0x1C raster depth in bits (palette size = 1 << depth) */
    uint32_t    unk20;
    void       *rwTexture;     /* +0x24 RwTexture * */
} ZglTexture;                  /* 0x28 */

/* zrwTxr.c globals, cleared by zrwTxr_Init (memset 0x430 at 0x00674290) */
typedef struct ZrwTxrGlobals {
    int32_t       initialized;     /* +0x000 0x00674290 */
    ZmemFreeList *pool;            /* +0x004 0x00674294 ZglTexture free list */
    RwRGBA        colorKey;        /* +0x008 0x00674298 g_textureColorKey: FF 00 FF 00 */
    void         *texDict;         /* +0x00C 0x0067429C RwTexDictionary * (the current one) */
    void       *(__cdecl *oldReadCallBack)(const char *name, const char *mask); /* +0x010 0x006742A0 */
    const char   *pendingName;     /* +0x014 0x006742A4 the MADF name (zrwTxr_SetPendingName) */
    RwRGBA        palette[256];    /* +0x018 0x006742A8 zrwTxrGetPalette / SetPalette buffer */
    int32_t       mipmapping;      /* +0x418 0x006746A8 */
    int32_t       lastWidth;       /* +0x41C 0x006746AC size of the last image read */
    int32_t       lastHeight;      /* +0x420 0x006746B0 */
    int32_t       lastDepth;       /* +0x424 0x006746B4 */
    void         *loadBuffer;      /* +0x428 0x006746B8 zrwTxr_SetLoadBuffer */
    uint32_t      loadBufferSize;  /* +0x42C 0x006746BC */
} ZrwTxrGlobals;                   /* 0x430 */

/* The texture driver table 0x005623F0 (g_texDriverVtbl) [V: slot targets] */
typedef struct ZglTextureApi {
    int         (__cdecl *init)(void);                                             /* +0x00 */
    int         (__cdecl *shutdown)(void);                                         /* +0x04 */
    ZglTexture *(__cdecl *create)(void);                                           /* +0x08 */
    ZglTexture *(__cdecl *createFromMemory)(void *buf, int len, const char *name, int type, int unused); /* +0x0C */
    int         (__cdecl *release)(ZglTexture *tex);                               /* +0x10 */
    int32_t     (__cdecl *getWidth)(ZglTexture *tex);                              /* +0x14 */
    int32_t     (__cdecl *getHeight)(ZglTexture *tex);                             /* +0x18 */
    int32_t     (__cdecl *getDepth)(ZglTexture *tex);                              /* +0x1C */
    const char *(__cdecl *getName)(ZglTexture *tex);                               /* +0x20 */
    uint32_t    (__cdecl *getNameHash)(ZglTexture *tex);                           /* +0x24 */
    RwRGBA     *(__cdecl *getPalette)(ZglTexture *tex);                            /* +0x28 */
    int         (__cdecl *setPalette)(ZglTexture *tex);                            /* +0x2C */
    void        (__cdecl *addRef)(ZglTexture *tex);                                /* +0x30 */
    int         (__cdecl *printInfo)(ZglTexture *tex);                             /* +0x34 */
    void        (__cdecl *printAll)(void);                                         /* +0x38 */
} ZglTextureApi;                                                                   /* 0x3C */

/* ================================================================== zrwMat.c */

/* MADF chunk payload as read by zrwMat_ChunkMADF (docs/formats/TEXTURE.md 2) */
typedef struct ZrwMadf {
    uint32_t nameOffset;      /* +0x00 must be 0x48 ("MADF Mismatch") */
    uint32_t flags;           /* +0x04 -> material +0x08; bit 0 = zrwMat_CurrentMadfFlag0 */
    uint32_t word08;          /* +0x08 -> material +0x0C */
    uint32_t word0c;          /* +0x0C -> material +0x10 */
    float    unused10[10];    /* +0x10 exporter colours, never read */
    uint32_t surface;         /* +0x38 -> material +0x14 (surface type, animation bits) */
    uint32_t anim1;           /* +0x3C -> material +0x18 */
    uint32_t anim2;           /* +0x40 -> material +0x1C */
    uint32_t nameHash;        /* +0x44 */
    char     name[0x40];      /* +0x48 */
} ZrwMadf;                    /* 0x88 */

/* One texture pass of a material (0x0C) */
typedef struct ZglMatPass {
    uint32_t    unk00;
    ZglTexture *texture;      /* +0x04 */
    uint32_t    unk08;
} ZglMatPass;                 /* 0x0C */

/* A ZGL material (0x74, free list of 256). The MADF words are copied to +0x08..+0x1C. */
typedef struct ZglMaterial {
    uint32_t    type;         /* +0x00 low byte ZGL_TYPE_MATERIAL */
    uint32_t    unk04;
    uint32_t    madfFlags;    /* +0x08 MADF +0x04 */
    uint32_t    madfWord08;   /* +0x0C MADF +0x08 */
    uint32_t    madfWord0c;   /* +0x10 MADF +0x0C */
    uint32_t    surface;      /* +0x14 MADF +0x38 (zrwMatGetSurface returns &surface) */
    uint32_t    anim1;        /* +0x18 MADF +0x3C */
    uint32_t    anim2;        /* +0x1C MADF +0x40 */
    uint32_t    unk20;        /* +0x20 cleared by the MATI handler */
    uint16_t    numPasses;    /* +0x24 textures added (max 6) */
    uint16_t    refCount;     /* +0x26 */
    ZglMatPass  pass[6];      /* +0x28 */
    void       *rpMaterial;   /* +0x70 RpMaterial * once attached (world / model materials) */
} ZglMaterial;                /* 0x74 */

/* zrwMat.c globals 0x006723A8 (memset 0x24 by zrwMat_Init) */
typedef struct ZrwMatGlobals {
    int32_t       initialized;    /* +0x00 0x006723A8 */
    uint32_t      unk04;
    ZmemFreeList *pool;           /* +0x08 0x006723B0 */
    uint8_t      *curMadf;        /* +0x0C 0x006723B4 the MADF chunk just read (header address, data at +8) */
    ZglMaterial  *lastCreated;    /* +0x10 0x006723B8 */
    int (__cdecl *createCallback)(ZglMaterial *mat, void *ctx); /* +0x14 0x006723BC called for every MATI */
    uint32_t      findHash;       /* +0x18 0x006723C0 zrwMatFindByTextureHash arguments */
    int32_t       findIndex;      /* +0x1C 0x006723C4 */
    ZglMaterial  *findResult;     /* +0x20 0x006723C8 */
} ZrwMatGlobals;                  /* 0x24 */

/* The material driver table 0x00561018 */
typedef struct ZglMaterialApi {
    int          (__cdecl *init)(void);                                            /* +0x00 */
    int          (__cdecl *shutdown)(void);                                        /* +0x04 */
    ZglMaterial *(__cdecl *create)(void);                                          /* +0x08 */
    void         (__cdecl *release)(ZglMaterial *mat);                             /* +0x0C */
    void         (__cdecl *addTexture)(ZglMaterial *mat, ZglTexture *tex);         /* +0x10 */
    void         (__cdecl *setTexture)(ZglMaterial *mat, ZglTexture *tex, uint32_t pass); /* +0x14 */
    void         (__cdecl *setColour)(ZglMaterial *mat, int pass, const RwRGBA *colour); /* +0x18 */
    ZglTexture  *(__cdecl *getTexture)(ZglMaterial *mat, uint32_t pass);           /* +0x1C */
    uint32_t    *(__cdecl *getSurface)(ZglMaterial *mat);                          /* +0x20 */
    ZglMaterial *(__cdecl *copy)(ZglMaterial *mat);                                /* +0x24 */
    ZglMaterial *(__cdecl *addRef)(ZglMaterial *mat);                              /* +0x28 */
    void        *(__cdecl *setCreateCallback)(void *fn);                           /* +0x2C */
    ZglMaterial *(__cdecl *createFromImage)(void *buf, int len, const char *name, int type); /* +0x30 */
    ZglMaterial *(__cdecl *findByTextureHash)(uint32_t hash, int index);           /* +0x34 */
    void         (__cdecl *printStats)(void);                                      /* +0x38 */
} ZglMaterialApi;                                                                  /* 0x3C */

/* ================================================================== zrwModel*.c */

/* MDEF entry as kept in the model def (ZMD.md 4: name[16] + id) */
typedef struct ZglModelDefEntry {
    char     name[16];        /* +0x00 NUL forced at +0x0F */
    uint32_t id;              /* +0x10 zHashName(name) */
} ZglModelDefEntry;           /* 0x14 */

/* Shared, reference-counted part of a loaded model (instances share it) */
typedef struct ZglModelDef {
    uint32_t          flags;        /* +0x00 bit 0 visible (zrwModelSetFlags 1) */
    int32_t           refCount;     /* +0x04 instances */
    uint32_t          mdefFlags;    /* +0x08 MDEF +0x00 ("kind"): 0x01 has custom render, 0x02 skinned
                                     * (RpBone), 0x04 not added to the world, 0x08 unlit (clear
                                     * rpGEOMETRYLIGHT), 0x10 shadow/flat render pass, 0x40 no
                                     * collision flag, 0x80 has an MCOL clump; 0xC0 no collision test */
    uint16_t          numLods;      /* +0x0C MLOD chunks loaded (max 4) */
    uint16_t          pad0e;
    uint32_t          nameId;       /* +0x10 MDEF +0x14 */
    char              name[16];     /* +0x14 MDEF +0x04 */
    char              path[0x80];   /* +0x24 the file it came from (createInstance reloads skinned models) */
    uint16_t          numEntries;   /* +0xA4 MDEF +0x28 */
    uint16_t          padA6;
    ZglModelDefEntry *entries;      /* +0xA8 */
    void             *atomic0;      /* +0xAC first RpAtomic of LOD 0 */
    void             *geometry0;    /* +0xB0 its RpGeometry */
    int  (__cdecl    *renderCB)(void *atomic); /* +0xB4 the atomic's default render callback (atomic +0x4C) */
    RwV3d            *lockedVerts;  /* +0xB8 zrwModelGeomLock */
} ZglModelDef;                      /* 0xBC */

/* One frame of a LOD clump (0x28); a tree built from the RW frame hierarchy */
typedef struct ZglModelPart {
    uint32_t             id;        /* +0x00 frame id (Animation 0x108 plugin = zHashName(bone)) */
    uint32_t             unk04;
    void                *node;      /* +0x08 ZanimNode * bound by zrwModelBindParts */
    void                *frame;     /* +0x0C RwFrame * */
    RwV3d                bindPos;   /* +0x10 frame position at load (RW units) */
    struct ZglModelPart *parent;    /* +0x1C */
    struct ZglModelPart *next;      /* +0x20 next sibling */
    struct ZglModelPart *child;     /* +0x24 first child */
} ZglModelPart;                     /* 0x28 */

typedef struct ZglModelLod {
    float         distance;   /* +0x00 MLDF word 1 (0 -> 0 / 50 / 100 / 150) */
    int32_t       numParts;   /* +0x04 */
    ZglModelPart *parts;      /* +0x08 */
} ZglModelLod;                /* 0x0C */

/* A ZGL model instance (0x8C, free list of 256) */
typedef struct ZglModel {
    uint32_t     type;        /* +0x00 low byte 0 */
    uint32_t     unk04;
    uint32_t     flags;       /* +0x08 1 scaled, 2 parts bound, 4 geometry locked */
    void        *clump;       /* +0x0C RpClump * (LOD 0) */
    void        *colClump;    /* +0x10 RpClump * of the MCOL chunk (never in the corpus) */
    void        *user;        /* +0x14 the zobj that owns it */
    RwV3d        pos;         /* +0x18 ZGE units */
    float        pad24;
    RwV3d        scale;       /* +0x28 */
    float        pad34;
    float        rot[4];      /* +0x38 Zquat x y z w */
    uint16_t     curLod;      /* +0x48 */
    uint16_t     pad4a;
    zHeapList   *materials;   /* +0x4C ZglMaterial * */
    ZglModelLod  lod[4];      /* +0x50 */
    void (__cdecl *updateCB)(struct ZglModel *m); /* +0x80 */
    RwRGBA       colour;      /* +0x84 */
    ZglModelDef *def;         /* +0x88 */
} ZglModel;                   /* 0x8C */

/* the model driver table 0x00560E78 (41 slots incl. the two print slots at +0x9C / +0xA0) */
typedef struct ZglModelApi {
    int      (__cdecl *init)(void);                                                    /* +0x00 */
    int      (__cdecl *shutdown)(void);
    ZglModel *(__cdecl *loadMemory)(uint32_t *buf);                                    /* +0x08 */
    ZglModel *(__cdecl *loadFile)(const char *path, int flags);
    int      (__cdecl *save)(void *model, const char *path);                           /* +0x10 not coded */
    ZglModel *(__cdecl *loadIff)(void *iff, int flags);
    ZglModel *(__cdecl *createInstance)(ZglModel *model);                              /* +0x18 */
    int      (__cdecl *release)(ZglModel *model);
    int      (__cdecl *update)(ZglModel *model);                                       /* +0x20 */
    int      (__cdecl *setPosition)(ZglModel *model, const RwV3d *pos);
    int      (__cdecl *setRotation)(ZglModel *model, const float *quat);              /* +0x28 */
    int      (__cdecl *setScale)(ZglModel *model, const RwV3d *scale);
    int32_t  (__cdecl *getNumParts)(ZglModel *model);                                  /* +0x30 */
    ZglModelPart *(__cdecl *findPart)(ZglModel *model, uint32_t id);
    void     (__cdecl *partToWorld)(ZglModel *model, ZglModelPart *part, const float *local, float *out,
                                    int flags);                                        /* +0x38 */
    void     (__cdecl *partBindOffset)(ZglModel *model, ZglModelPart *part, float *out);
    void     (__cdecl *getPartMatrix)(ZglModel *model, ZglModelPart *part, float *out, int flags); /* +0x40 */
    ZglModelPart *(__cdecl *getPartParent)(ZglModel *model, ZglModelPart *part);
    uint32_t (__cdecl *getNameHash)(ZglModel *model, const uint32_t *key);            /* +0x48 */
    void    *(__cdecl *getDefEntry)(ZglModel *model, const uint32_t *key);
    void     (__cdecl *getPartIds)(ZglModel *model, uint32_t *out);                    /* +0x50 */
    void     (__cdecl *bindParts)(ZglModel *model, void *nodeTable);
    void     (__cdecl *render)(ZglModel *model);                                       /* +0x58 */
    void     (__cdecl *renderSkeleton)(ZglModel *model);
    int      (__cdecl *setFlags)(ZglModel *model, uint32_t clear, uint32_t set);      /* +0x60 */
    void     (__cdecl *setUser)(ZglModel *model, void *user);
    void    *(__cdecl *getUser)(ZglModel *model);                                      /* +0x68 */
    void    *(__cdecl *getClump)(ZglModel *model);
    ZglMaterial *(__cdecl *findMaterial)(ZglModel *model, uint32_t textureHash);      /* +0x70 */
    int32_t  (__cdecl *getNumMaterials)(ZglModel *model);
    ZglMaterial *(__cdecl *getMaterial)(ZglModel *model, uint32_t index);             /* +0x78 */
    void     (__cdecl *setColour)(ZglModel *model, const RwRGBA *colour);
    void     (__cdecl *setUpdateCallback)(ZglModel *model, void (__cdecl *fn)(ZglModel *)); /* +0x80 */
    int      (__cdecl *notCoded)(void);
    int      (__cdecl *vertexNumber)(int index);                                       /* +0x88 */
    int      (__cdecl *lockGeometry)(ZglModel *model, uint32_t flags);
    int      (__cdecl *unlockGeometry)(ZglModel *model);                               /* +0x90 */
    int      (__cdecl *getVertex)(ZglModel *model, int n, float *out);
    int      (__cdecl *setVertex)(ZglModel *model, int n, const float *in);            /* +0x98 */
    void     (__cdecl *printInfo)(ZglModel *model);
    void     (__cdecl *printStats)(void);                                              /* +0xA0 */
} ZglModelApi;                                                                         /* 0xA4 */

/* ================================================================== zrwLight.c */
typedef struct ZglLight {
    uint32_t type;            /* +0x00 low byte ZGL_TYPE_LIGHT */
    uint32_t unk04;
    void    *rpLight;         /* +0x08 RpLight * */
} ZglLight;                   /* 0x0C */

typedef struct ZglLightApi {  /* 0x00563420 */
    int       (__cdecl *init)(void);
    int       (__cdecl *shutdown)(void);
    ZglLight *(__cdecl *create)(int type, const uint8_t *rgba);
    int       (__cdecl *release)(ZglLight *l);
    int       (__cdecl *setColour)(ZglLight *l, const uint8_t *rgba);
    int       (__cdecl *setFlags)(ZglLight *l, uint32_t flags);
    int       (__cdecl *setPosition)(ZglLight *l, const float *pos);
    int       (__cdecl *setDirection)(ZglLight *l, const float *dir);
    int       (__cdecl *setConeAngle)(ZglLight *l, int16_t angle);
    int       (__cdecl *setRadius)(ZglLight *l, float radius);
} ZglLightApi;                /* 0x28 */

/* ================================================================== zrwCam.c */
typedef struct ZglCamera {
    uint32_t type;            /* +0x00 low byte ZGL_TYPE_CAMERA */
    uint32_t unk04;
    uint32_t flags;           /* +0x08 1 window set, 2 fog, 4 own frame-buffer sub-rasters */
    void    *rwCamera;        /* +0x0C RwCamera * */
    void    *user;            /* +0x10 the zobj camera */
    RwMatrix rwMatrix;        /* +0x14 the RW frame matrix (zrwCamSetMatrix) */
    RwMatrix zgeMatrix;       /* +0x54 the ZGE matrix as given; its pos (+0x84) = the ZGE position */
    float    window[4];       /* +0x94 x0, y0, x1, y1 pixels */
    int32_t  width;           /* +0xA4 */
    int32_t  height;          /* +0xA8 */
    float    nearClip;        /* +0xAC 0.5 */
    float    farClip;         /* +0xB0 75 */
    float    centreX;         /* +0xB4 raster offset */
    float    centreY;         /* +0xB8 */
    int16_t  fov;             /* +0xBC 16-bit angle, 0x271C */
    int16_t  padBE;
    RwRGBA   fogColour;       /* +0xC0 */
    float    fogDistance;     /* +0xC4 fog starts this far before the far plane */
} ZglCamera;                  /* 0xC8 */

typedef struct ZglCameraApi { /* 0x00561D88 */
    int        (__cdecl *init)(void);
    int        (__cdecl *shutdown)(void);
    ZglCamera *(__cdecl *create)(uint32_t flags, void *user);
    int        (__cdecl *release)(ZglCamera *cam);
    void       (__cdecl *setFov)(ZglCamera *cam, int fov);
    int16_t    (__cdecl *getFov)(ZglCamera *cam);
    void       (__cdecl *setMatrix)(ZglCamera *cam, const RwMatrix *m, const float *pos);
    RwMatrix  *(__cdecl *getMatrix)(ZglCamera *cam);
    int        (__cdecl *project)(ZglCamera *cam, const float *p, float *out);
    void       (__cdecl *resize)(ZglCamera *cam);
    ZglCamera *(__cdecl *getCurrent)(void);
    void      *(__cdecl *getUser)(ZglCamera *cam);
    void       (__cdecl *setWindow)(ZglCamera *cam, uint32_t flags, const float *rect);
    int        (__cdecl *setResolution)(int32_t width, int32_t height);
    void       (__cdecl *setViewOffset)(ZglCamera *cam, float x, float y);
} ZglCameraApi;               /* 0x3C */

/* ================================================================== zrwCol.c, zrwPrim.c, zrwZgl.c */
typedef struct ZglColApi {    /* 0x00562060 */
    int (__cdecl *init)(void);
    int (__cdecl *shutdown)(void);
    int (__cdecl *lineTest)(const RwV3d *a, const RwV3d *b, uint32_t mode, void **exclude, void *hit);
    int (__cdecl *sphereTest)(const float *centre, float radius, uint32_t mode, void **exclude, void *list);
} ZglColApi;                  /* 0x10 */

typedef struct ZglPrimApi {   /* 0x00561720 */
    int  (__cdecl *init)(void);
    int  (__cdecl *shutdown)(void);
    void (__cdecl *drawLines)(uint32_t flags, const void *lines, int count, const RwMatrix *m);
    void (__cdecl *drawTris)(uint32_t flags, const void *tris, int count, const RwMatrix *m);
    void (__cdecl *drawStrips)(uint32_t flags, const void *polys, int count, const RwMatrix *m);
    void (__cdecl *drawPolys)(uint32_t flags, const void *polys, int count, const RwMatrix *m);
    void (__cdecl *drawQuads)(uint32_t flags, const void *quads, int count, const RwMatrix *m);
} ZglPrimApi;                 /* 0x1C */

typedef struct ZglSystemApi { /* 0x00561468 */
    int      (__cdecl *init)(void);
    int      (__cdecl *shutdown)(void);
    void    *(__cdecl *addCallback)(int list, void *fn, void *arg, int tail);
    int      (__cdecl *removeCallback)(void *node);
    void     (__cdecl *runCallbacks)(int list);
    void     (__cdecl *assertHandler)(const char *why, const char *file, int line, const char *func);
    uint32_t (__cdecl *getTime)(void);
    int      (__cdecl *setValue)(int what, void *value, void *arg);
    int      (__cdecl *getValue)(int what, void *out, void *arg);
    void     (__cdecl *resetRenderState)(void);
    int      (__cdecl *setRenderState)(uint32_t state, void *raster);
    int      (__cdecl *pushRenderState)(void);
    int      (__cdecl *popRenderState)(void);
} ZglSystemApi;               /* 0x34 */

/* RpCollisionTriangle (RW 3.x, 0x28) and the line / sphere query (RwLine / RwSphere + type) */
typedef struct RwCollTri {
    RwV3d    normal;          /* +0x00 */
    RwV3d    point;           /* +0x0C */
    int32_t  index;           /* +0x18 triangle / polygon index */
    RwV3d   *vertices[3];     /* +0x1C */
} RwCollTri;                  /* 0x28 */

typedef struct RwIntersection {
    float    v[6];            /* line: start, end; sphere: centre, radius */
    int32_t  type;            /* +0x18 1 line, 3 sphere */
} RwIntersection;             /* 0x1C */

/* zrwPrim: a polygon (drawPolys / drawStrips) */
typedef struct ZglPoly {
    uint32_t    material;     /* +0x00 ZglMaterial *, 0 = untextured */
    ZglVertex **v;            /* +0x04 */
    int32_t     count;        /* +0x08 */
} ZglPoly;                    /* 0x0C */

/* zrwDbg: one queued debug shape (512 per frame at 0x00650A9C) */
typedef struct ZrwDbgShape {
    uint32_t type;            /* +0x00 0 axes, 1 line, 2 triangle, 3 sphere, 4 box */
    float    data[25];        /* +0x04 per type: points / matrix / radius, colour, flags */
    uint16_t flags[2];        /* +0x68 box: render flags */
} ZrwDbgShape;                /* 0x6C */

/* zrwShadow: a shadow request [V: field use] */
typedef struct ZrwShadow {
    uint32_t unk00[2];
    uint8_t *owner;           /* +0x08 the owner zobj link (its +0x14 = the object) */
    RwV3d    dir;             /* +0x0C owner -> "light" */
} ZrwShadow;                  /* 0x18 */

/* GHIDRA-SKIP-BEGIN */
typedef char zglrw_h_size_check_other[(sizeof(ZglModelApi) == 0xA4 && sizeof(ZglLight) == 0x0C &&
                                       sizeof(ZglLightApi) == 0x28 && sizeof(ZglCamera) == 0xC8 &&
                                       sizeof(ZglCameraApi) == 0x3C && sizeof(ZglColApi) == 0x10 &&
                                       sizeof(RwCollTri) == 0x28 && sizeof(RwIntersection) == 0x1C &&
                                       sizeof(ZglPoly) == 0x0C && sizeof(ZrwDbgShape) == 0x6C &&
                                       sizeof(ZrwShadow) == 0x18 &&
                                       sizeof(ZglPrimApi) == 0x1C && sizeof(ZglSystemApi) == 0x34) ? 1 : -1];
typedef char zglrw_h_size_check_model[(sizeof(ZglModelDefEntry) == 0x14 && sizeof(ZglModelDef) == 0xBC &&
                                       sizeof(ZglModelPart) == 0x28 && sizeof(ZglModelLod) == 0x0C &&
                                       sizeof(ZglModel) == 0x8C) ? 1 : -1];
typedef char zglrw_h_size_check_tex[(sizeof(ZglTexture) == 0x28 && sizeof(ZrwTxrGlobals) == 0x430 &&
                                     sizeof(ZglTextureApi) == 0x3C) ? 1 : -1];
typedef char zglrw_h_size_check_mat[(sizeof(ZrwMadf) == 0x88 && sizeof(ZglMatPass) == 0x0C &&
                                     sizeof(ZglMaterial) == 0x74 && sizeof(ZrwMatGlobals) == 0x24 &&
                                     sizeof(ZglMaterialApi) == 0x3C) ? 1 : -1];

/* ---- zrwCam.c */
int zrwCamBeginUpdateRaw(ZglCamera *cam);
void zrwCamEndUpdateRaw(void);
void *zrwCamCreateSubRaster(void *parent, const float *rect, int32_t type);
void zrwCamSetRasters(ZglCamera *cam, void *raster, void *zRaster);
int zrwCamDestroyFrameBuffers(void);
int zrwCamCreateFrameBuffers(void);
int zrwCamSetResolution(int32_t width, int32_t height);
int zrwCamInit(void);
int zrwCamShutdown(void);
ZglCamera *zrwCamCreate(uint32_t flags, void *user);
int zrwCamRelease(ZglCamera *cam);
void zrwCamSetClipPlanes(ZglCamera *cam, float nearClip, float farClip);
void zrwCamSetFov(ZglCamera *cam, int fov);
int16_t zrwCamGetFov(ZglCamera *cam);
void zrwCamSetMatrix(ZglCamera *cam, const RwMatrix *m, const float *pos);
RwMatrix *zrwCamGetMatrix(ZglCamera *cam);
int zrwCamProject(ZglCamera *cam, const float *p, float *out);
void zrwCamResize(ZglCamera *cam);
void zrwCamSetWindow(ZglCamera *cam, uint32_t flags, const float *rect);
ZglCamera *zrwCamGetFirst(void);
ZglCamera *zrwCamGetNext(ZglCamera *cam);
int zrwCamBeginUpdate(ZglCamera *cam);
int zrwCamEndUpdate(void);
void *zrwCamGetRwCamera(ZglCamera *cam);
ZglCamera *zrwCamGetCurrent(void);
RwMatrix *zrwCamGetRwMatrix(ZglCamera *cam);
void *zrwCamGetUser(ZglCamera *cam);
void zrwCamSetClearMode(ZglCamera *cam, int32_t mode);
void zrwCamSetClearColour(ZglCamera *cam, const RwRGBA *c);
void zrwCamSetFog(ZglCamera *cam, uint32_t on);
void zrwCamSetFogColour(ZglCamera *cam, const RwRGBA *c);
void zrwCamSetFogDistance(ZglCamera *cam, float d);
void zrwCamSetViewOffset(ZglCamera *cam, float x, float y);
/* ---- zrwCol.c */
void *zrwColAtomicTriMaterial(void *atomic, const RwCollTri *tri);
void *zrwColSectorTriMaterial(void *sector, const RwCollTri *tri);
int zrwColInit(void);
int zrwColShutdown(void);
int zrwColLineTest(const RwV3d *a, const RwV3d *b, uint32_t mode, void **exclude, ZrwColHit *out);
void *zrwColWorldLineCB(void *isect, void *sector, RwCollTri *tri, float t, void *data);
void *zrwColAtomicLineCB(void *isect, void *sector, void *atomic, float dist, void **exclude);
void *zrwColAtomicTriLineCB(void *isect, RwCollTri *tri, float t, void *atomic);
int zrwColSphereTest(const float *centre, float radius, uint32_t mode, void **exclude, ZrwColTriList *out);
void *zrwColSphereTriCB(void *isect, void *sector, RwCollTri *tri, float dist, void *atomic);
void *zrwColAtomicSphereCB(void *isect, void *sector, void *atomic, float dist, void **exclude);
void *zrwColAtomicTriSphereCB(void *isect, RwCollTri *tri, float dist, void *atomic);
const RwRGBA *zrwColGetHitColour(ZrwColHit *hit);
int zrwColTagsInit(void);
int zrwColTagsShutdown(void);
int zrwColLine(const RwV3d *a, const RwV3d *b, uint32_t mode, void **exclude, ZrwColHit *out);
int zrwColSphere(const float *centre, float radius, uint32_t mode, void **exclude, ZrwColTriList *out);
int zrwColPointInTri(const float *v0, const float *v1, const float *v2, const float *n, const float *p);
int zrwColPointInTriByNormals(const RwV3d *a, const RwV3d *b, const RwV3d *c, const float *n, const RwV3d *p);
void zrwColClosestPointOnSegment(float *out, const RwV3d *a, const RwV3d *b, const RwV3d *p);
RwV3d *zrwColSphereTriContact(RwV3d *out, const RwV3d *c, float r, const RwV3d *v0, const RwV3d *v1, const RwV3d *v2, const RwV3d *n);
/* ---- zrwDbg.c */
int zrwDbgPrintStats(void);
void zrwDbgDrawLine(ZrwDbgShape *sh);
void zrwDbgDrawAxes(ZrwDbgShape *sh);
void zrwDbgDrawTri(ZrwDbgShape *sh);
void zrwDbgDrawSphere(ZrwDbgShape *sh);
void zrwDbgDrawBox(ZrwDbgShape *sh);
int zrwDbgDrawText(const char *text, int x, int y);
int zrwDbgRender(void);
void zrwDbgCreateCharset(void);
void zrwDbgDestroyCharset(void);
int zrwDbgInit(void);
int zrwDbgShutdown(void);
int zdebugPrint(int16_t x, int16_t y, const char *fmt, ...);
void zrwDbgPrintFloat(int x, int y, const char *label, float f);
void zrwDbgPrintMatrix(int x, int y, const char *label, const float *m);
void zrwDbgPrintVec4(int x, int y, const char *label, const float *v);
void zrwDbgPrintVec3(int x, int y, const char *label, const float *v);
void zrwDbgPrintAngle(int x, int y, const char *label, int16_t a);
void zrwDbgPrintEuler(int x, int y, const char *label, const int16_t *a);
void zrwDbgPrintMatrixAt(int x, int y, const char *label, const float *m);
void zrwDbgPrintVec3b(int x, int y, const char *label, const float *v);
void zrwDbgShowStats(int8_t mode);
void zrwDbgAddLine(const RwV3d *a, const RwV3d *b, const RwRGBA *colour, uint16_t flags);
void zrwDbgAddAxes(const RwMatrix *m, uint16_t flags0, uint16_t flags1);
void zrwDbgAddTri(const RwV3d *a, const RwV3d *b, const RwV3d *c, const RwRGBA *colour, uint16_t flags);
void zrwDbgAddSphere(const RwV3d *c, float radius, const RwRGBA *colour, uint16_t flags);
void zrwDbgAddBox(const float *box, const RwMatrix *m, const RwRGBA *colour, uint16_t flags);
void *zrwDbgAddTextCallback(int unused, void *fn, void *arg);
void zrwDbgRemoveTextCallback(void *node);
int zrwDbgPrintf(const char *fmt, ...);
void zrwDbgSetIndent(int n);
void zrwDbgIndent(int delta);
/* ---- zrwLight.c */
int zrwLightInit(void);
int zrwLightShutdown(void);
ZglLight *zrwLightCreate(int type, const uint8_t *rgba);
int zrwLightRelease(ZglLight *l);
int zrwLightSetColour(ZglLight *l, const uint8_t *rgba);
int zrwLightSetFlags(ZglLight *l, uint32_t flags);
int zrwLightSetPosition(ZglLight *l, const float *pos);
int zrwLightSetDirection(ZglLight *l, const float *dir);
int zrwLightSetConeAngle(ZglLight *l, int16_t angle);
int zrwLightSetRadius(ZglLight *l, float radius);
/* ---- zrwMat.c */
int zrwMat_ChunkMATI(void *iff, void *ctx);
int zrwMat_ChunkMADF(void *iff);
int zrwMat_Init(void);
int zrwMatShutdown(void);
ZglMaterial *zrwMatCreate(void);
void zrwMatRelease(ZglMaterial *mat);
void zrwMatSetTexture(ZglMaterial *mat, ZglTexture *tex, uint32_t pass);
void zrwMatAddTexture(ZglMaterial *mat, ZglTexture *tex);
void zrwMatSetColour(ZglMaterial *mat, int pass, const RwRGBA *colour);
void *zrwMatSetCreateCallback(void *fn);
ZglMaterial *zrwMatCreateFromImage(void *buf, int len, const char *name, int type);
ZglMaterial *zrwMatCreateProcedural(int width, int height, int depth, const char *name);
ZglMaterial *zrwMatCreatePaletteFromMemory(const uint8_t *pixels, int width, int height, const RwRGBA *palette, const char *name);
int zrwMatFindByTextureHashCB(ZglMaterial *mat, void *args);
ZglMaterial *zrwMatFindByTextureHash(uint32_t hash, int index);
ZglTexture *zrwMatGetTexture(ZglMaterial *mat, uint32_t pass);
ZglMaterial *zrwMatCopy(ZglMaterial *mat);
ZglMaterial *zrwMatAddRef(ZglMaterial *mat);
uint32_t *zrwMatGetSurface(ZglMaterial *mat);
void *zrwMatGetRaster(ZglMaterial *mat, uint32_t pass);
void zrwMatSetRpMaterial(ZglMaterial *mat, void *rpMaterial);
void *zrwMatGetRpMaterial(ZglMaterial *mat);
int zrwMat_CurrentMadfFlag0(void);
ZglMaterial *zrwMat_CreateFromTexture(ZglTexture *tex);
void zrwMatPrintStats(void);
/* ---- zrwModel.c */
ZglModelDefEntry *zrwModelFindDefEntry(ZglModel *model, uint32_t id);
void zrwModelOpen(void);
void zrwModelClose(void);
void *zrwModelAtomicFlagsCB(void *atomic, uint32_t *mask);
void zrwModelSetAtomicFlags(void *clump, uint32_t clear, uint32_t set);
void *zrwModelGeometryFlagsCB(void *atomic, uint32_t *mask);
void zrwModelSetGeometryFlags(void *clump, uint32_t clear, uint32_t set);
void zrwModelAddToWorld(ZglModel *model, int on);
void zrwModelSetCollision(ZglModel *model, int on);
void *zrwModelClumpPluginCtor(void *clump, int offset);
void *zrwModelClumpPluginDtor(void *clump);
int zrwModelSetClumpOwner(void *clump, ZglModel *model);
ZglModel *zrwModelGetClumpOwner(void *clump);
void *zrwModelPlugin2Ctor(void *obj, int offset);
void *zrwModelPlugin2Dtor(void *obj);
int zrwModelPlugin2Set(void *obj, void *value);
void *zrwModelPlugin2Get(void *obj);
void zrwModelQuatToRwMatrix(const float *quat, RwMatrix *m);
void zrwModelQuatToRwMatrixMirror(const float *quat, RwMatrix *m);
void zrwModelUpdateParts(ZglModel *model);
int zrwModelInit(void);
int zrwModelShutdown(void);
void zrwModelBindParts(ZglModel *model, ZanimNodeTable *table);
int32_t zrwModelGetNumParts(ZglModel *model);
void zrwModelGetPartIds(ZglModel *model, uint32_t *out);
ZglModelPart *zrwModelFindPart(ZglModel *model, uint32_t id);
void zrwModelPartToWorld(ZglModel *model, ZglModelPart *part, const float *local, float *out, int flags);
void zrwModelPartBindOffset(ZglModel *model, ZglModelPart *part, float *out);
void zrwModelGetPartMatrix(ZglModel *model, ZglModelPart *part, float *out, int flags);
ZglModelPart *zrwModelGetPartParent(ZglModel *model, ZglModelPart *part);
uint32_t zrwModelGetNameHash(ZglModel *model, const uint32_t *key);
void *zrwModelGetDefEntry(ZglModel *model, const uint32_t *key);
int zrwModelSetPosition(ZglModel *model, const RwV3d *pos);
int zrwModelSetRotation(ZglModel *model, const float *quat);
int zrwModelSetScale(ZglModel *model, const RwV3d *scale);
int zrwModelUpdate(ZglModel *model);
int zrwModelRegisterPlugin(void);
int zrwModelSetFlags(ZglModel *model, uint32_t clear, uint32_t set);
ZglModel *zrwModelFromAtomic(void *atomic);
void zrwModelSetUser(ZglModel *model, void *user);
void *zrwModelGetUser(ZglModel *model);
void *zrwModelGetClump(ZglModel *model);
ZglMaterial *zrwModelFindMaterial(ZglModel *model, uint32_t textureHash);
int32_t zrwModelGetNumMaterials(ZglModel *model);
ZglMaterial *zrwModelGetMaterial(ZglModel *model, uint32_t index);
void zrwModelSetColour(ZglModel *model, const RwRGBA *colour);
void zrwModelApplyColour(ZglModel *model, const RwRGBA *colour);
/* ---- zrwModelCreate.c */
void zrwModelCreateRunLinks(void *obj);
void zrwModelAddLink(void *obj, int32_t id, void (__cdecl *fn)(int32_t *item));
void zrwModelRemoveLink(void *obj, int32_t id);
void *zrwModelStashFrameIdCB(void *frame, void *data);
void zrwModelStashFrameIds(void *clump);
void *zrwModelRestoreFrameIdCB(void *frame, void *data);
void zrwModelRestoreFrameIds(void *clump);
void *zrwModelFrameSetIdentity(void *frame);
void zrwModelClumpSetIdentity(void *clump);
void zrwModelSetRwInfo(ZglModel *model);
void *zrwModelPrintAtomicCB(void *atomic, int32_t *totals);
void zrwModelPrintClump(void *clump);
void *zrwModelPrintFrameCB(void *frame, ZglModel *model);
void zrwModelPrintHierarchy(ZglModel *model);
void zrwModelPrintMaterials(ZglModel *model);
void *zrwModelFirstAtomicCB(void *atomic, void **out);
void *zrwModelGetFirstAtomic(void *clump);
void *zrwModelCountAtomicCB(void *atomic, int32_t *count);
int32_t zrwModelCountAtomics(void *clump);
void *zrwModelAddPartCB(void *frame, ZglModelPart *parent);
ZglModelPart *zrwModelBuildParts(ZglModel *model, int lod);
ZglModel *zrwModelAlloc(void);
void zrwModelFree(ZglModel *model);
ZglModel *zrwModelCreateInstance(ZglModel *src);
int zrwModelCreateNop(void);
int zrwModelLoadClump(ZglModel *model, void **buf, const char *imagePath, int isCol);
int zrwModelChunkMLOD(ZiffFile *iff, ZglModel *model);
int zrwModelReadLods(ZiffFile *iff, void *model);
int zrwModelChunkMCOL(ZiffFile *iff, ZglModel *model);
int zrwModelReadMcol(ZiffFile *iff, ZglModel *model);
int zrwModelReadMDEF(ZiffFile *iff, ZglModel *model);
int zrwModelAddMaterialCB(ZglMaterial *mat, void *model);
int zrwModelReadMaterials(ZiffFile *iff, void *model);
int zrwModelChunkMODLBody(ZiffFile *iff, ZglModel *model);
int zrwModelChunkMODL(ZiffFile *iff);
int zrwModelCreateInit(void);
int zrwModelCreateShutdown(void);
ZglModel *zrwModelLoadIff(ZiffFile *iff, int flags);
ZglModel *zrwModelLoadMemory(uint32_t *buf);
ZglModel *zrwModelLoadFile(const char *path, int flags);
int zrwModelSave(void);
int zrwModelRelease(ZglModel *model);
void zrwModelPrintStats(void);
void zrwModelPrintInfo(ZglModel *model);
/* ---- zrwModelGeom.c */
void zrwModelGeomInit(void);
int zrwModelGeomShutdown(void);
void zrwModelSetUpdateCallback(ZglModel *model, void (__cdecl *fn)(ZglModel *));
int zrwModelGeomNotCoded(void);
int zrwModelGeomVertexNumber(int index);
int zrwModelGeomLock(ZglModel *model, uint32_t flags);
int zrwModelGeomUnlock(ZglModel *model);
int zrwModelGeomGetVertex(ZglModel *model, int n, float *out);
int zrwModelGeomSetVertex(ZglModel *model, int n, const float *in);
/* ---- zrwModelRender.c */
void *zrwModelRenderNop(void *atomic);
void *zrwModelRenderFlatCB(void *atomic);
void *zrwModelRenderFacingCB(void *atomic);
void *zrwModelRenderColouredCB(void *atomic);
void *zrwModelRenderImmediateCB(void *atomic, void *unused);
void *zrwModelRenderProjectedCB(void *atomic);
void *zrwModelDrawBoneCB(void *frame, RwV3d *parentPos);
int zrwModelRenderFlat(ZglModel *model);
void zrwModelRenderSkeleton(ZglModel *model);
int zrwModelRenderShadowPass(ZglModel *model);
int zrwModelRenderInit(void);
int zrwModelRenderShutdown(void);
void zrwModelRender(ZglModel *model);
int32_t zrwModelSetRenderMode(int32_t mode);
void *zrwModelSetAtomicRenderCB(void *atomic, void *fn);
void zrwModelForEachMaterial(void *model, void *fn);
void zrwModelApplyRenderMode(ZglModel *model);
void zrwModelAddShadowCallback(ZglModel *model);
void zrwModelRemoveShadowCallback(void);
/* ---- zrwPrim.c */
void zrwPrimReserve2D(uint32_t n);
void zrwPrimReserve3D(uint32_t n);
void zrwPrimClipX(float x, const ZglVertex *a, const ZglVertex *b, ZglVertex *out);
void zrwPrimClipY(float y, const ZglVertex *a, const ZglVertex *b, ZglVertex *out);
uint32_t zrwPrimDrawClipped(uint32_t flags, uint32_t material, ZglVertex **verts, int n, const RwMatrix *m, const float *rect);
int zrwPrimInit(void);
int zrwPrimShutdown(void);
void zrwPrimDrawLines(uint32_t flags, const ZglLine *lines, int count, const RwMatrix *m);
void zrwPrimDrawTris(uint32_t flags, const ZglTri *tris, int count, const RwMatrix *m);
void zrwPrimDrawPolys(uint32_t flags, const ZglPoly *polys, int count, const RwMatrix *m);
void zrwPrimDrawStrips(uint32_t flags, const ZglPoly *polys, int count, const RwMatrix *m);
void zrwPrimDrawQuads(uint32_t flags, const ZglQuad *quads, int count, const RwMatrix *m);
/* ---- zrwShadow.c */
int zrwShadowInit(void);
int zrwShadowDraw(void);
int zrwShadowShutdown(void);
void zrwShadowNop(void);
void zrwShadowUpdate(ZrwShadow *sh);
/* ---- zrwTxr.c */
void zrwTxrPrintRasterInfo(void *raster);
void zrwTxr_SetColorKey(const RwRGBA *key);
void zrwTxr_ApplyColorKey(RwImage *img);
RwImage *zrwTxr_ReadBmp(const char *name);
RwImage *zrwTxr_ReadPng(const char *name);
RwImage *zrwTxr_ReadDds(const char *name);
void *zrwTxrPluginConstructor(void *rwTexture, int offset);
void *zrwTxrPluginDestructor(void *rwTexture);
int zrwTxrSetOwnerOfRwTexture(void *rwTexture, void *owner);
void *zrwTxrGetOwnerOfRwTexture(void *rwTexture);
ZglTexture *zrwTxr_CreateTextureFromMemory(void *buf, int len, const char *name, int type, int unused);
ZglTexture *zrwTxrLoadPalettedImageFromMemory(const uint8_t *pixels, int width, int height, const RwRGBA *palette, const char *name);
ZglTexture *zrwTxrCreateProceduralTexture(int width, int height, int depth, const char *name);
void zrwTxrLock(ZglTexture *tex);
void zrwTxrUnlock(ZglTexture *tex);
int zrwTxrWrapRwTexture(void *rwTexture, ZglTexture *tex, int width, int height, int depth);
void *zrwTxrReadCallBack(const char *name, const char *mask);
int zrwTxr_ChunkTBMP(void *iff);
int zrwTxr_ChunkTPNG(void *iff);
int zrwTxr_ChunkTDDS(void *iff);
void zrwTxrPixelToRGBA(const void *pixels, uint32_t format, int index, RwRGBA *out);
void zrwTxrRGBAToPixel(void *pixels, uint32_t format, int index, const RwRGBA *in);
int zrwTxrPrintCB(void *obj, void *unused);
void zrwTxrOpenDictionary(void);
void zrwTxrCloseDictionary(void);
int zrwTxr_Init(void);
int zrwTxrShutdown(void);
ZglTexture *zrwTxrCreate(void);
int zrwTxrRelease(ZglTexture *tex);
int32_t zrwTxrGetWidth(ZglTexture *tex);
int32_t zrwTxrGetHeight(ZglTexture *tex);
int32_t zrwTxrGetDepth(ZglTexture *tex);
const char *zrwTxrGetName(ZglTexture *tex);
uint32_t zrwTxrGetNameHash(ZglTexture *tex);
RwRGBA *zrwTxrGetPalette(ZglTexture *tex);
int zrwTxrSetPalette(ZglTexture *tex);
void *zrwTxrGetRwTexture(ZglTexture *tex);
void *zrwTxrGetRaster(ZglTexture *tex);
void zrwTxrAddRef(ZglTexture *tex);
int zrwTxrRegisterPlugin(void);
void zrwTxr_SetPendingName(const char *name);
void zrwTxrSetOwner(ZglTexture *tex, void *owner);
void zrwTxrSetMipmapping(int on);
void zrwTxrFlush(void);
void zrwTxrSetFilterMode(ZglTexture *tex, int mode);
int zrwTxrPrintInfo(ZglTexture *tex);
void zrwTxrPrintAll(void);
void zrwTxr_SetLoadBuffer(void *buf, uint32_t size);
void zrwTxrClearLoadBuffer(void);
/* ---- zrwZgl.c */
void zrwZglErrorBox(const char *msg);
void zrwZglDebugString(const char *msg);
void zAssertMessageBox(const char *why, const char *file, int line, const char *func);
int zrwZglSelectDevice(void);
int zrwZglReselectDevice(void);
void zrwZglDeviceDlgInit(HWND dlg);
void zrwZglDeviceDlgChangeSubSystem(HWND dlg);
int __stdcall zrwZglDeviceDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, char *lpCmdLine, int nShowCmd);
LRESULT __stdcall MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
int zfileGetPlatformDevices(zfileDeviceList *list);
void zfileReleasePlatformDevices(void);
void *zrwZglGetMemoryFunctions(void);
int zrwZglConfirmQuit(void);
int app_CheckGameCD(void);
int zrwZglSetValue(int what, void *value, void *arg);
int zrwZglGetValue(int what, void *out, void *arg);
void zrwZglHookMemory(void);
void zrwZglNop(void);
int zrwZglHookMemoryRet(void);
int zrwZglInit(void);
int zrwZglShutdown(void);
int App_Open(HWND hWnd);
int App_CloseEngine(void);
int App_Reopen(void);
int App_Start(char *cmdLine);
int App_Stop(void);
int App_Frame(void);
int zrwZglRender(void);
void zrwZglShowFrame(int vsync);
void zrwZglRequestQuit(void);
void *zrwZglAddCallbackToList(zList *list, void *fn, void *arg, int tail);
int zrwZglRunCallbackList(zList *list);
void *zrwZglAddCallback(int list, void *fn, void *arg, int tail);
int zrwZglRemoveCallback(void *node);
void zrwZglRunCallbacks(int list);
void zrwZglResetRenderState(void);
int zrwZglSetRenderState(uint32_t state, void *raster);
int zrwZglPushRenderState(void);
int zrwZglPopRenderState(void);
void zrwZglMatrixFromRw(float *out, const RwMatrix *m);
int zrwZglNotTested(void);
int zrwZglPlayMovie(const char *path);

/* ---- driver tables and globals */
extern ZrwTxrGlobals        g_zrwTxr;             /* 0x00674290 */
extern int32_t              g_zrwTxrPluginOffset; /* 0x005623E8 RwTexture plugin 0xFF01 (owner pointer), -1 until registered */
extern const ZglTextureApi  g_texDriverVtbl;      /* 0x005623F0 */
extern ZrwMatGlobals        g_zrwMat;             /* 0x006723A8 */
extern const ZglMaterialApi g_zglMaterialApi;     /* 0x00561018 */
extern const ZglModelApi    g_zglModelApi;        /* 0x00560E78 */
extern const ZglLightApi    g_zglLightApi;        /* 0x00563420 */
extern const ZglColApi      g_zglColApi;          /* 0x00562060 */
extern const ZglPrimApi     g_zglPrimApi;         /* 0x00561720 */
extern const ZglCameraApi   g_zglCameraApi;       /* 0x00561D88 */
extern const ZglSystemApi   g_zglSystemApi;       /* 0x00561468 */
extern void *const          g_zrwDbgDriver[20];   /* 0x00560030 (zengine.h: g_pfnDebug*) */
extern int32_t              g_zrwModelPluginOffset; /* 0x00560E70 RpClump plugin 0xFF02 */
int zdebugPrint(int16_t x, int16_t y, const char *fmt, ...);

/* ---- RenderWare 3.1 (library code: names [I] from the RW 3.x SDK and their use here) */
#define RpClumpGetFrame(clump) (*(void **)((uint8_t *)(clump) + 4))   /* RW macro */
int32_t RpClumpGetNumAtomics(void *clump);                                                   /* 0x004B1440 */
void *RpClumpRender(void *clump);                                                            /* 0x004B1940 */
void *RpClumpForAllAtomics(void *clump, void *cb, void *data);                               /* 0x004B1990 */
void *RpClumpClone(void *clump);                                                             /* 0x004B1EE0 */
int RpClumpDestroy(void *clump);                                                             /* 0x004B21E0 */
void *RpClumpStreamRead(void *stream);                                                       /* 0x004B2790 */
int32_t RpClumpRegisterPlugin(int32_t size, uint32_t id, void *ctor, void *dtor, void *copy); /* 0x004B31E0 */
void *RpGeometryGetMorphTarget(void *geom, int32_t index);                                   /* 0x004B43E0 */
void *RpGeometryGetVertexTexCoords(void *geom);                                              /* 0x004B44E0 */
int32_t RpGeometryGetNumVertices(void *geom);                                                /* 0x004B4550 */
RwV3d *RpMorphTargetGetVertices(void *morphTarget);                                          /* 0x004B45D0 */
void *RpGeometryGetTriangles(void *geom);                                                    /* 0x004B46B0 */
int32_t RpGeometryGetNumTriangles(void *geom);                                               /* 0x004B4720 */
void *RpGeometryGetMaterial(void *geom, int32_t index);                                      /* 0x004B47A0 */
int32_t RpGeometryGetNumMaterials(void *geom);                                               /* 0x004B4CE0 */
void *RpGeometryLock(void *geom, int32_t lockMode);                                          /* 0x004B4F20 */
void *RpGeometryUnlock(void *geom);                                                          /* 0x004B4FD0 */
uint32_t RpGeometryGetFlags(void *geom);                                                     /* 0x004B6140 */
void *RpGeometrySetFlags(void *geom, uint32_t flags);                                        /* 0x004B61B0 */
void *RpWorldAddCamera(void *world, void *camera);                                           /* 0x004B81F0 */
void *RpWorldRemoveCamera(void *world, void *camera);                                        /* 0x004B8240 */
void *RwCameraGetWorld(void *camera);                                                        /* 0x004B82A0 */
void *RpWorldAddClump(void *world, void *clump);                                             /* 0x004B86A0 */
void *RpWorldRemoveClump(void *world, void *clump);                                          /* 0x004B8770 */
void *RpClumpGetWorld(void *clump);                                                          /* 0x004B8870 */
void *RpWorldAddLight(void *world, void *light);                                             /* 0x004B8AF0 */
void *RpWorldRemoveLight(void *world, void *light);                                          /* 0x004B8B80 */
void *RpMaterialSetColor(void *mat, const RwRGBA *colour);                                   /* 0x004B9310 */
void *RpMaterialSetTexture(void *mat, void *tex);                                            /* 0x004B9380 */
void *RpMaterialGetTexture(const void *mat);                                                 /* 0x004B93D0 */
void *RpWorldGetMaterial(void *world, int32_t index);                                        /* 0x004BA140 */
int RpWorldPluginAttach(void);                                                               /* 0x004BC1C0 */
void *RpCollisionWorldForAllIntersections(void *world, void *isect, void *cb, void *data);   /* 0x004C1B10 */
void *RpWorldForAllAtomicIntersections(void *world, void *isect, void *cb, void *data);      /* 0x004C1C10 */
void *RpAtomicForAllIntersections(void *atomic, void *isect, void *cb, void *data);          /* 0x004C1F30 */
void *RpLightCreate(int32_t type);                                                           /* 0x004C2400 */
int RpLightDestroy(void *light);                                                             /* 0x004C24C0 */
void *RpLightSetRadius(void *light, float radius);                                           /* 0x004C2530 */
void *RpLightSetColor(void *light, const float *rgbaReal);                                   /* 0x004C2570 */
void *RpLightSetConeAngle(void *light, float angle);                                         /* 0x004C25E0 */
void *RpLightSetFrame(void *light, void *frame);                                             /* 0x004C2BB0 */
void *RpLightGetFrame(void *light);                                                          /* 0x004C2BE0 */
void *RpLightSetFlags(void *light, uint32_t flags);                                          /* 0x004C2C00 */
void *RtCharsetGetDesc(void *charset, int32_t *desc);                                        /* 0x004CB3F0 */
int RtCharsetPrint(void *charset, const char *text, int32_t x, int32_t y);                   /* 0x004CBEF0 */
void RtCharsetDestroy(void *charset);                                                        /* 0x004CBF60 */
void *RtCharsetCreate(const RwRGBA *fg, const RwRGBA *bg);                                   /* 0x004CBFB0 */
void *RpPVSHook(void *world);                                                                /* 0x004CC270 */
void *RpPVSUnhook(void *world);                                                              /* 0x004CC2C0 */
void *RpPVSSetViewPosition(void *world, const RwV3d *pos);                                   /* 0x004CC510 */
void *RpPVSStatisticsGet(void *world, int32_t *visible, int32_t *total);                     /* 0x004CC670 */
int RpPVSPluginAttach(void);                                                                 /* 0x004CCAC0 */
int RwEngineTerm(void);                                                                      /* 0x004CF840 */
int32_t RwEngineGetNumSubSystems(void);                                                      /* 0x004CF870 */
void *RwEngineGetSubSystemInfo(void *info, int32_t index);                                   /* 0x004CF8B0 */
int32_t RwEngineGetCurrentSubSystem(void);                                                   /* 0x004CF940 */
int RwEngineSetSubSystem(int32_t index);                                                     /* 0x004CF980 */
int32_t RwEngineGetNumVideoModes(void);                                                      /* 0x004CF9F0 */
void *RwEngineGetVideoModeInfo(void *mode, int32_t index);                                   /* 0x004CFA60 */
int32_t RwEngineGetCurrentVideoMode(void);                                                   /* 0x004CFAD0 */
int RwEngineSetVideoMode(int32_t index);                                                     /* 0x004CFB40 */
int RwEngineStart(void);                                                                     /* 0x004CFD50 */
int RwEngineOpen(void *params);                                                              /* 0x004D0020 */
int RwEngineClose(void);                                                                     /* 0x004D0070 */
void *RwEngineGetMetrics(void);                                                              /* 0x004D0120 */
int RwEngineStop(void);                                                                      /* 0x004D0140 */
int RwEngineInit(void *memFuncs, uint32_t flags);                                            /* 0x004D01F0 */
RwMatrix *RwMatrixUpdate(RwMatrix *m);                                                       /* 0x004D04D0 */
RwMatrix *RwMatrixMultiply(RwMatrix *out, const RwMatrix *a, const RwMatrix *b);             /* 0x004D04F0 */
RwMatrix *RwMatrixRotate(RwMatrix *m, const RwV3d *axis, float angle, int32_t combine);      /* 0x004D1040 */
RwMatrix *RwMatrixScale(RwMatrix *m, const RwV3d *s, int32_t combine);                       /* 0x004D1400 */
RwMatrix *RwMatrixTranslate(RwMatrix *m, const RwV3d *t, int32_t combine);                   /* 0x004D15A0 */
int RwMatrixDestroy(RwMatrix *m);                                                            /* 0x004D1FB0 */
RwMatrix *RwMatrixCreate(void);                                                              /* 0x004D1FE0 */
void *RwFrameCreate(void);                                                                   /* 0x004D2870 */
int RwFrameDestroy(void *frame);                                                             /* 0x004D2920 */
void *RwFrameUpdateObjects(void *frame);                                                     /* 0x004D2A80 */
RwMatrix *RwFrameGetMatrix(void *frame);                                                     /* 0x004D2B00 */
RwMatrix *RwFrameGetLTM(void *frame);                                                        /* 0x004D2B10 */
void *RwFrameForAllChildren(void *frame, void *cb, void *data);                              /* 0x004D2EB0 */
int32_t RwFrameCount(void *frame);                                                           /* 0x004D2F00 */
void *RwFrameTranslate(void *frame, const RwV3d *v, int32_t combine);                        /* 0x004D2F40 */
void *RwFrameSetIdentity(void *frame);                                                       /* 0x004D3200 */
RwV3d *RwV3dTransformPoints(RwV3d *out, const RwV3d *in, int32_t n, const RwMatrix *m);      /* 0x004D3A90 */
RwV3d *RwV3dTransformVectors(RwV3d *out, const RwV3d *in, int32_t n, const RwMatrix *m);     /* 0x004D3AE0 */
float RwIm2DGetNearScreenZ(void);                                                            /* 0x004DB800 */
int RwRenderStateSet(int32_t state, uint32_t value);                                         /* 0x004DB820 */
int RwIm2DRenderPrimitive(int32_t type, void *verts, int32_t count);                         /* 0x004DB920 */
void **RwOsGetMemoryInterface(void);                                                         /* 0x004DBA80 */
void RwEngineSetSomething(uint32_t value);                                                   /* 0x004DC6E0 */
void *RwRasterCreate(int32_t width, int32_t height, int32_t depth, int32_t flags);           /* 0x004DCB10 */
int RwRasterDestroy(void *raster);                                                           /* 0x004DCBD0 */
int32_t RwRasterGetWidth(void *raster);                                                      /* 0x004DCC30 */
int32_t RwRasterGetHeight(void *raster);                                                     /* 0x004DCC40 */
int32_t RwRasterGetStride(void *raster);                                                     /* 0x004DCC50 */
int32_t RwRasterGetDepth(void *raster);                                                      /* 0x004DCC60 */
uint32_t RwRasterGetFormat(void *raster);                                                    /* 0x004DCC70 */
void *RwRasterSubRaster(void *sub, void *parent, const int32_t *rect);                       /* 0x004DCC80 */
void *RwRasterGetParent(void *raster);                                                       /* 0x004DCD70 */
void *RwRasterPushContext(void *raster);                                                     /* 0x004DCD80 */
void *RwRasterPopContext(void);                                                              /* 0x004DCE20 */
void *RwRasterShowRaster(void *raster, void *hwnd, uint32_t flags);                          /* 0x004DCFD0 */
void *RwRasterLock(void *raster, uint8_t level, int32_t lockMode);                           /* 0x004DD010 */
void *RwRasterUnlock(void *raster);                                                          /* 0x004DD100 */
void *RwRasterLockPalette(void *raster, int32_t lockMode);                                   /* 0x004DD160 */
void *RwRasterUnlockPalette(void *raster);                                                   /* 0x004DD1D0 */
void *RwCameraSetViewOffset(void *camera, const float *offset);                              /* 0x004DE540 */
void *RwCameraSetRaster(void *camera, void *raster);                                         /* 0x004DE580 */
void *RwCameraGetRaster(void *camera);                                                       /* 0x004DE5A0 */
void *RwCameraSetZRaster(void *camera, void *raster);                                        /* 0x004DE5B0 */
void *RwCameraGetZRaster(void *camera);                                                      /* 0x004DE5D0 */
void *RwCameraSetNearClipPlane(void *camera, float nearClip);                                /* 0x004DE5E0 */
float RwCameraGetNearClipPlane(void *camera);                                                /* 0x004DE610 */
void *RwCameraSetFarClipPlane(void *camera, float farClip);                                  /* 0x004DE620 */
float RwCameraGetFarClipPlane(void *camera);                                                 /* 0x004DE650 */
void *RwCameraSetFogDistance(void *camera, float distance);                                  /* 0x004DE660 */
void *RwCameraBeginUpdate(void *camera);                                                     /* 0x004DE790 */
void *RwCameraEndUpdate(void *camera);                                                       /* 0x004DE7F0 */
void *RwCameraClear(void *camera, const RwRGBA *colour, int32_t mode);                       /* 0x004DE8A0 */
void *RwCameraSetProjection(void *camera, int32_t projection);                               /* 0x004DE920 */
void *RwCameraSetViewWindow(void *camera, const float *window);                              /* 0x004DE9B0 */
const float *RwCameraGetViewWindow(void *camera);                                            /* 0x004DEA10 */
void *RwCameraSetFrame(void *camera, void *frame);                                           /* 0x004DEA60 */
void *RwCameraGetFrame(void *camera);                                                        /* 0x004DEA80 */
int RwCameraDestroy(void *camera);                                                           /* 0x004DEA90 */
void *RwCameraCreate(void);                                                                  /* 0x004DEAE0 */
void *RwErrorGet(void *code);                                                                /* 0x004DED90 */
int RwIm3DRenderPrimitive(int32_t type);                                                     /* 0x004DEF70 */
int RwIm3DRenderLine(int32_t v0, int32_t v1);                                                /* 0x004DEFC0 */
int RwIm3DRenderIndexedPrimitive(int32_t type, const uint16_t *indices, int32_t count);      /* 0x004DF020 */
int RwIm3DEnd(void);                                                                         /* 0x004DF090 */
void *RwIm3DTransform(void *verts, int32_t count, const RwMatrix *ltm, uint32_t flags);      /* 0x004DF0B0 */
int RwTextureSetReadCallBack(void *(__cdecl *fn)(const char *name, const char *mask));       /* 0x004DF520 */
void *RwTextureGetReadCallBack(void);                                                        /* 0x004DF540 */
int RwTextureSetAutoMipmapping(int enable);                                                  /* 0x004DF550 */
int RwTextureSetMipmapping(int enable);                                                      /* 0x004DF580 */
void *RwTextureSetRaster(void *tex, void *raster);                                           /* 0x004DF5F0 */
void *RwTextureGetRaster(const void *tex);                                                   /* 0x004DF650 */
void *RwTexDictionaryCreate(void);                                                           /* 0x004DF670 */
int RwTextureDestroy(void *tex);                                                             /* 0x004DF760 */
int RwTexDictionaryDestroy(void *dict);                                                      /* 0x004DF810 */
void *RwTextureCreate(void *raster);                                                         /* 0x004DF910 */
void *RwTextureAddRef(void *tex);                                                            /* 0x004DF980 */
char *RwTextureGetName(void *tex);                                                           /* 0x004DF990 */
char *RwTextureSetName(void *tex, const char *name);                                         /* 0x004DF9B0 */
void *RwTexDictionaryAddTexture(void *dict, void *tex);                                      /* 0x004DFB20 */
void *RwTexDictionarySetCurrent(void *dict);                                                 /* 0x004DFC80 */
void *RwTextureRead(const char *name, const char *mask);                                     /* 0x004DFCB0 */
void *RwTextureSetFilterMode(void *tex, int32_t mode);                                       /* 0x004DFF70 */
void *RwTextureSetAddressing(void *tex, int32_t mode);                                       /* 0x004DFFA0 */
int32_t RwTextureRegisterPlugin(int32_t size, uint32_t id, void *ctor, void *dtor, void *copy); /* 0x004DFFD0 */
void *RwStreamOpen(int32_t type, int32_t access, const void *data);                          /* 0x004E23E0 */
int RwStreamClose(void *stream, void *data);                                                 /* 0x004E2660 */
int RwStreamFindChunk(void *stream, uint32_t type, uint32_t *len, uint32_t *ver);            /* 0x004E2EC0 */
int RwImageDestroy(RwImage *img);                                                            /* 0x004E3960 */
int32_t RwImageGetWidth(const RwImage *img);                                                 /* 0x004E3A60 */
int32_t RwImageGetHeight(const RwImage *img);                                                /* 0x004E3A70 */
int32_t RwImageGetDepth(const RwImage *img);                                                 /* 0x004E3A80 */
uint8_t *RwImageGetPixels(const RwImage *img);                                               /* 0x004E3A90 */
RwRGBA *RwImageGetPalette(const RwImage *img);                                               /* 0x004E3AA0 */
int RwImageSetPath(const char *path);                                                        /* 0x004E4420 */
int RwImageRegisterImageFormat(const char *ext, RwImage *(__cdecl *read)(const char *), void *write); /* 0x004E4A30 */
RwRGBA *RwRGBASetFromPixel(RwRGBA *out, uint32_t pixel, int32_t format);                     /* 0x004E5330 */
RwImage *RtBMPImageRead(const char *name);                                                   /* 0x004E6D90 */
void *RwRasterSetFromImage(void *raster, RwImage *img);                                      /* 0x004E7F70 */
uint32_t RpAnimFrameGetID(void *frame);                                                      /* 0x00500220 */
void RpAnimFrameSetID(void *frame, uint32_t id);                                             /* 0x00500230 */
int RpAnimPluginAttach(void);                                                                /* 0x00500250 */
void RpBoneClumpUpdate(void *clump);                                                         /* 0x00504490 */
int RpBonePluginAttach(void);                                                                /* 0x00505870 */
void RpBoneClumpInitialize(void *clump);                                                     /* 0x00505950 */
int RpLODAtomicPluginAttach(void);                                                           /* 0x005062B0 */
void RpLODAtomicSetCamera(void *camera);                                                     /* 0x005065D0 */
RwImage *RtPNGImageRead(const char *name);                                                   /* 0x005068A0 */
/* GHIDRA-SKIP-END */

#endif /* ZGLRW_H */

/*
 * zobj.h - the ZGE object model: NgEngine/Zge/zobj.c and the ZgeMods object types
 * zobjModel.c, zobjLight.c, zobjCamera.c, zobjPEmit.c, plus the engine start-up (zge.c) and the
 * input front end (zinput.c).
 *
 * A zobj is a 0x3C-byte node from one free list (g_zobjPool, 0x100 per block): a position, a
 * rotation quaternion, an optional hierarchy link (attach to a parent object or to one of a model's
 * frames) and a 9-slot vtable that forwards to the renderer object in `handle` (a zrwModel, a
 * zrwLight, a zrwCam or a particle emitter). Every frame the ZGL callback zobjUpdateAll (priority 8)
 * pushes the transforms of all active objects (and, through the hierarchy, of their children) into
 * the renderer.
 *
 * Offsets [V] (field use in these files and their callers); names [I]. Description: docs/re/zge.md.
 */
#ifndef ZOBJ_H
#define ZOBJ_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "zmath.h"

/* zobj.flags */
#define ZOBJ_TYPE_MASK   0x07u   /* zobjConstruct(type) */
#define ZOBJ_TYPE_MODEL  0       /* zobjModelCreate (zobjModel.c) */
#define ZOBJ_TYPE_LIGHT  1       /* zobjLightCreate */
#define ZOBJ_TYPE_PEMIT  2       /* zobjPEmitCreate (zobjPEmit.c) */
#define ZOBJ_TYPE_CAMERA 3       /* zobjCameraCreate */
#define ZOBJF_ACTIVE     0x08u   /* set by zobjConstruct; zobjSetActive; only active objects update */
#define ZOBJF_FLAG10     0x10u   /* zobjSetFlag10; never read in the exe [V, static scan] */
#define ZOBJF_ATTACHED   0x20u   /* zobjAttach; the parent's update moves the object */

typedef struct ZObj ZObj;

/* The hierarchy link (0x10, from g_zobjLinkPool), allocated on demand. */
typedef struct ZobjLink {
    ZObj    *parent;      /* +0x00 object this one is attached to, NULL = root */
    void    *frame;       /* +0x04 a frame of the parent's model, NULL = the parent itself */
    ZObj    *next;        /* +0x08 next sibling under the same parent */
    ZObj    *child;       /* +0x0C first child */
} ZobjLink;               /* 0x10 */

/* The per-type methods. NULL slots are skipped by the zobj.c wrappers. */
typedef int      (__cdecl *ZobjDestroyFn)(ZObj *obj);
typedef int      (__cdecl *ZobjUpdateFn)(ZObj *obj);
typedef int      (__cdecl *ZobjSetVecFn)(ZObj *obj, const RwV3d *v);
typedef int      (__cdecl *ZobjSetQuatFn)(ZObj *obj, const Zquat *q);
typedef int      (__cdecl *ZobjSetIntFn)(ZObj *obj, int value);
typedef uint32_t (__cdecl *ZobjGetIdFn)(ZObj *obj);
typedef void    *(__cdecl *ZobjGetPtrFn)(ZObj *obj);

typedef struct ZobjVtbl {
    ZobjDestroyFn destroy;      /* +0x00 zobjDestroy, before the node is freed */
    ZobjUpdateFn  update;       /* +0x04 zobjUpdateAll: push the transform to the renderer */
    ZobjSetVecFn  setPosition;  /* +0x08 zobjSetPosition (world position when attached) */
    ZobjSetQuatFn setRotation;  /* +0x0C zobjSetRotation / FromEuler / FromMatrix */
    ZobjSetVecFn  setScale;     /* +0x10 zobjSetScale */
    ZobjSetIntFn  setActive;    /* +0x14 zobjSetActive */
    ZobjGetIdFn   getId;        /* +0x18 zobjFind compares it (a model's name hash) */
    ZobjGetPtrFn  getRwObject;  /* +0x1C zobjGetRwObject (a model's RpClump / RpAtomic) */
    ZobjGetPtrFn  getBounds;    /* +0x20 a model's bounding data (zrwModel def + 0x14) */
} ZobjVtbl;                     /* 0x24 */

struct ZObj {
    uint32_t        flags;      /* +0x00 ZOBJ_TYPE_* | ZOBJF_* */
    RwV3d           pos;        /* +0x04 local position (relative to the parent when attached) */
    uint32_t        unk10;      /* +0x10 never accessed (pads pos to a 4-vector) */
    Zquat           rot;        /* +0x14 local rotation */
    ZobjLink       *link;       /* +0x24 NULL until attached / given children */
    const ZobjVtbl *vtbl;       /* +0x28 */
    void           *handle;     /* +0x2C the renderer object (zrwModel, zrwLight, zrwCam, emitter) */
    void           *data;       /* +0x30 per type: ZobjCameraData, a model's ZanimNodeTable, ... */
    void           *user;       /* +0x34 zobjSetUserData / zobjGetUserData */
    uint32_t        unk38;      /* +0x38 cleared by zobjConstruct, otherwise unused here */
};                              /* 0x3C */

/* A camera zobj's data (0x104 from g_zobjCameraPool): four derived matrices, rebuilt on demand. */
#define ZOBJCAM_VIEW     0      /* world -> camera (rotation^T, -R^T pos) */
#define ZOBJCAM_WORLD    1      /* camera -> world (the inverse) */
#define ZOBJCAM_ROTATION 2      /* the view matrix without translation */
#define ZOBJCAM_YAW      3      /* a pure yaw by the camera heading */
typedef struct ZobjCameraData {
    RwMatrix matrix[4];         /* +0x000 ZOBJCAM_* */
    uint32_t valid;             /* +0x100 bit n: matrix[n] is current; position/rotation clear it */
} ZobjCameraData;               /* 0x104 */

/* A particle emitter zobj's data (0x3C, zmemAlloc by zobjPEmitCreate 0x00495C10). */
typedef struct ZobjPEmitData {
    void    *systems;           /* +0x00 zHeapList of ZfxSystem *, one per zfx type (zpemitSpawn) */
    void    *drawCallback;      /* +0x04 ZGL callback node (list 2, zobjPEmitDraw 0x00495BD0) */
    RwV3d    pos;               /* +0x08 zobjPEmitSetPosition */
    uint32_t unk14;             /* +0x14 */
    Zquat    rot;               /* +0x18 zobjPEmitSetRotation (identity at creation [V]) */
    RwV3d    scale;             /* +0x28 zobjPEmitSetScale (1, 1, 1) */
    uint32_t unk34;             /* +0x34 */
    int      active;            /* +0x38 zobjPEmitSetActive (1) */
} ZobjPEmitData;                /* 0x3C */

/* GHIDRA-SKIP-BEGIN */
typedef char zobj_h_size_check[(sizeof(ZobjLink) == 0x10 && sizeof(ZobjVtbl) == 0x24 &&
                                sizeof(ZObj) == 0x3C && sizeof(ZobjCameraData) == 0x104 &&
                                sizeof(ZobjPEmitData) == 0x3C) ? 1 : -1];

/* ------------------------------------------------------------------ zobj.c */
int       zobjUpdateChild(ZObj *obj, void *arg);                    /* 0x00486EE0 */
int       zobjUpdateAll(void *arg);                                 /* 0x00487020 ZGL frame callback */
int       zobjPrintStats(void *arg);                                /* 0x004871E0 debug-draw callback */
int       zobjInit(int maxObjects);                                 /* 0x00487210 */
int       zobjShutdown(void);                                       /* 0x004872F0 */
ZObj     *zobjAlloc(void);                                          /* 0x00487370 */
ZObj     *zobjConstruct(ZObj *obj, uint32_t type);                  /* 0x004873A0 */
int       zobjDestroy(ZObj *obj);                                   /* 0x00487400 */
void     *zobjGetHandle(ZObj *obj);                                 /* 0x00487440 */
void     *zobjGetRwObject(ZObj *obj);                               /* 0x00487450 */
void      zobjSetPosition(ZObj *obj, const RwV3d *pos);             /* 0x00487470 */
RwV3d    *zobjGetPosition(ZObj *obj);                               /* 0x004874A0 */
void      zobjSetScale(ZObj *obj, const RwV3d *scale);              /* 0x004874B0 */
void      zobjSetRotation(ZObj *obj, const Zquat *rot);             /* 0x004874D0 */
Zquat    *zobjGetRotation(ZObj *obj);                               /* 0x00487510 */
void      zobjSetRotationEuler(ZObj *obj, const int16_t *angles);   /* 0x00487520 */
void      zobjSetRotationMatrix(ZObj *obj, const RwMatrix *m);      /* 0x00487580 */
void      zobjSetUserData(ZObj *obj, void *user);                   /* 0x004875E0 */
void     *zobjGetUserData(ZObj *obj);                               /* 0x004875F0 */
void      zobjSetActive(ZObj *obj, int active);                     /* 0x00487600 */
void      zobjSetFlag10(ZObj *obj, int on);                         /* 0x00487630 */
int       zobjForEachOfType(int (__cdecl *fn)(ZObj *obj, void *arg), void *arg, uint32_t type); /* 0x00487650 */
ZObj     *zobjFind(uint32_t id, void *unused, ZObj *after);         /* 0x004876B0 */
void      zobjAllocLink(ZObj *obj);                                 /* 0x00487730 */
void      zobjFreeLink(ZObj *obj);                                  /* 0x00487760 */
void      zobjAttach(ZObj *obj, ZObj *parent, void *frame);         /* 0x00487790 */
void      zobjDetach(ZObj *obj);                                    /* 0x00487830 */
ZObj     *zobjGetNextSibling(ZObj *obj);                            /* 0x00487960 */
ZObj     *zobjGetFirstChild(ZObj *obj);                             /* 0x00487980 */
ZObj     *zobjGetParent(ZObj *obj);                                 /* 0x004879A0 */
int       zobjForEachChild(ZObj *obj, int (__cdecl *fn)(ZObj *child, void *arg), void *arg); /* 0x004879C0 */
int       zobjPrintCB(ZObj *obj, void *unused);                     /* 0x00487A00 zobjDumpAll's pool callback */
void      zobjDumpAll(void);                                        /* 0x00487A50 */

/* ------------------------------------------------------------------ zobjCamera.c */
int       zobjCameraDestroy(ZObj *cam);                             /* 0x00482090 vtbl[0] */
int       zobjCameraUpdate(ZObj *cam);                              /* 0x004820C0 vtbl[1] */
int       zobjCameraSetPosition(ZObj *cam, const RwV3d *pos);       /* 0x004820F0 vtbl[2] */
int       zobjCameraSetRotation(ZObj *cam, const Zquat *rot);       /* 0x00482110 vtbl[3] */
int       zobjCameraInit(void);                                     /* 0x00482130 */
int       zobjCameraShutdown(void);                                 /* 0x00482180 */
ZObj     *zobjCameraCreate(void);                                   /* 0x004821C0 */
void      zobjCameraSetViewport(ZObj *cam, const float *rect);      /* 0x00482280 */
void      zobjCameraSetFov(ZObj *cam, int angle);                   /* 0x004822A0 */
void      zobjCameraLookAt(ZObj *cam, const RwV3d *eye, const RwV3d *at, const RwV3d *up); /* 0x004822C0 */
RwMatrix *zobjCameraGetMatrix(ZObj *cam, int which);                /* 0x00482310 ZOBJCAM_* */
extern const ZobjVtbl g_zobjCameraVtbl;                             /* 0x00560518 */

/* ------------------------------------------------------------------ zobjModel.c */
int       zobjModelUpdate(ZObj *obj);                               /* 0x00488D60 vtbl[1] (animated) */
int       zobjModelSetPosition(ZObj *obj, const RwV3d *pos);        /* 0x00488D80 vtbl[2] */
int       zobjModelSetRotation(ZObj *obj, const Zquat *rot);        /* 0x00488DA0 vtbl[3] */
int       zobjModelSetScale(ZObj *obj, const RwV3d *scale);         /* 0x00488DC0 vtbl[4] */
int       zobjModelSetActive(ZObj *obj, int active);                /* 0x00488DE0 vtbl[5] */
uint32_t  zobjModelGetId(ZObj *obj);                                /* 0x00488E20 vtbl[6] */
void     *zobjModelGetRwObject(ZObj *obj);                          /* 0x00488E40 vtbl[7] */
void     *zobjModelGetBounds(ZObj *obj);                            /* 0x00488E60 vtbl[8] */
int       zobjModelUpdateStatic(ZObj *obj);                         /* 0x00488E80 vtbl[1] (static) */
int       zobjModelDestroy(ZObj *obj);                              /* 0x00488EA0 vtbl[0] (static) */
int       zobjModelRender(ZObj *obj, void *arg);                    /* 0x00488EC0 */
int       zobjModelRenderAll(void);                                 /* 0x00488EE0 */
int       zobjModelHide(ZObj *obj);                                 /* 0x00488F00 */
int       zobjModelShow(ZObj *obj);                                 /* 0x00488F20 */
int       zobjModelInit(void);                                      /* 0x00488F40 */
int       zobjModelShutdown(void);                                  /* 0x00488F80 */
extern const ZobjVtbl g_zobjModelAnimVtbl;                          /* 0x00560FA8 (slot 0 = 0x00488D30) */
extern const ZobjVtbl g_zobjModelVtbl;                              /* 0x00560FD0 */

/* ------------------------------------------------------------------ zobjLight.c */
#define ZOBJLIGHT_DIRECTIONAL 0  /* the RW light type table 0x00563408 [I order] */
#define ZOBJLIGHT_AMBIENT     1
#define ZOBJLIGHT_POINT       2
#define ZOBJLIGHT_SPOT        3
int       zobjLightDestroy(ZObj *light);                            /* 0x0048FF50 vtbl[0] */
int       zobjLightSetPosition(ZObj *light, const RwV3d *pos);      /* 0x0048FF70 vtbl[2] */
int       zobjLightSetRotation(ZObj *light, const Zquat *rot);      /* 0x0048FF90 vtbl[3] */
int       zobjLightInit(void);                                      /* 0x0048FFC0 */
int       zobjLightShutdown(void);                                  /* 0x0048FFF0 */
ZObj     *zobjLightCreate(int type, const RwRGBA *colour);          /* 0x00490010 */
void      zobjLightSetColour(ZObj *light, const RwRGBA *colour);    /* 0x004900D0 */
void      zobjLightSetFlags(ZObj *light, uint32_t flags);           /* 0x004900F0 */
void      zobjLightSetDirection(ZObj *light, const RwV3d *dir);     /* 0x00490120 */
void      zobjLightSetConeAngle(ZObj *light, int angle);            /* 0x00490140 */
void      zobjLightSetRadius(ZObj *light, float radius);            /* 0x00490160 */
extern const ZobjVtbl g_zobjLightVtbl;                              /* 0x00561858 */

/* ------------------------------------------------------------------ zobjPEmit.c */
int       zobjPEmitSetPosition(ZObj *obj, const RwV3d *pos);        /* 0x00495AD0 vtbl[2] */
int       zobjPEmitSetRotation(ZObj *obj, const Zquat *rot);        /* 0x00495B00 vtbl[3] */
int       zobjPEmitSetScale(ZObj *obj, const RwV3d *scale);         /* 0x00495B30 vtbl[4] */
int       zobjPEmitSetActive(ZObj *obj, int active);                /* 0x00495B60 vtbl[5] */
int       zobjPEmitInit(void);                                      /* 0x00495B80 */
int       zobjPEmitShutdown(void);                                  /* 0x00495BB0 */
int       zobjPEmitUpdate(ZObj *obj);                               /* 0x00495A10 vtbl[1] */
int       zobjPEmitDestroy(ZObj *obj);                              /* 0x00495A60 vtbl[0] */
int       zobjPEmitDraw(ZObj *obj);                                 /* 0x00495BD0 ZGL callback (list 2) */
extern const ZobjVtbl g_zobjPEmitVtbl;                              /* 0x00562098 */

/* zinput.c: the prototypes are in contrler.h (with ZInputDevice) */

/* ------------------------------------------------------------------ zge.c */
int       zgeInit(const char *sceneFile);                           /* 0x0049F900 */
int       zgeShutdown(void);                                        /* 0x0049F9C0 */
float     zgeScalarFromRw(float v);                                 /* 0x0049FAF0 zglGet(0x13) */
RwV3d    *zgeVecFromRw(RwV3d *out, const RwV3d *v);                 /* 0x0049FB10 zglGet(0x11) */

/* ------------------------------------------------------------------ the renderer's function tables
 * (zrwModel.c 0x00560E94.., zrwLight.c 0x00563428.., zrwCam.c 0x00561D88..; zglrw.h has the whole tables
 * as structs: g_zglModelApi, g_zglLightApi, g_zglCameraApi, g_zglSystemApi; these names are
 * the single slots the zobj types call through; names [I] from the slot targets. */
extern int   (__cdecl *g_zrwModelDestroy)(void *model);                          /* 0x00560E94 g_zglModelApi.release = zrwModelRelease */
extern int   (__cdecl *g_zrwModelSync)(void *model);                             /* 0x00560E98 .update = zrwModelUpdate */
extern int   (__cdecl *g_zrwModelSetPosition)(void *model, const RwV3d *pos);    /* 0x00560E9C .setPosition = zrwModelSetPosition */
extern int   (__cdecl *g_zrwModelSetRotation)(void *model, const Zquat *rot);    /* 0x00560EA0 .setRotation = zrwModelSetRotation */
extern int   (__cdecl *g_zrwModelSetScale)(void *model, const RwV3d *scale);     /* 0x00560EA4 .setScale = zrwModelSetScale */
extern uint32_t (__cdecl *g_zrwModelGetId)(void *model, const uint32_t *frame);  /* 0x00560EC0 .getNameHash = zrwModelGetNameHash */
extern void *(__cdecl *g_zrwModelGetBounds)(void *model, const uint32_t *frame); /* 0x00560EC4 .getDefEntry = zrwModelGetDefEntry (NULL: the name) */
extern int   (__cdecl *g_zrwModelRender)(void *model);                           /* 0x00560ED4 .renderSkeleton = zrwModelRenderSkeleton */
extern void *(__cdecl *g_zrwModelGetRwObject)(void *model);                      /* 0x00560EE4 .getClump = zrwModelGetClump */
extern void *(__cdecl *g_zrwLightCreate)(int type, const RwRGBA *colour);        /* 0x00563428 g_zglLightApi.create = zrwLightCreate */
extern int   (__cdecl *g_zrwLightDestroy)(void *light);                          /* 0x0056342C .release = zrwLightRelease */
extern int   (__cdecl *g_zrwLightSetColour)(void *light, const RwRGBA *colour);  /* 0x00563430 .setColour = zrwLightSetColour */
extern int   (__cdecl *g_zrwLightSetFlags)(void *light, uint8_t rwFlags);        /* 0x00563434 .setFlags = zrwLightSetFlags */
extern int   (__cdecl *g_zrwLightSetPosition)(void *light, const RwV3d *pos);    /* 0x00563438 .setPosition = zrwLightSetPosition */
extern int   (__cdecl *g_zrwLightSetDirection)(void *light, const RwV3d *dir);   /* 0x0056343C .setDirection = zrwLightSetDirection */
extern void *(__cdecl *g_zrwLightSetConeAngle)(void *light, int16_t angle);      /* 0x00563440 .setConeAngle = zrwLightSetConeAngle */
extern void *(__cdecl *g_zrwLightSetRadius)(void *light, float radius);          /* 0x00563444 .setRadius = zrwLightSetRadius */
extern int   (__cdecl *g_zrwCamInit)(void);                                      /* 0x00561D88 g_zglCameraApi.init = zrwCamInit */
extern int   (__cdecl *g_zrwCamShutdown)(void);                                  /* 0x00561D8C .shutdown = zrwCamShutdown */
extern void *(__cdecl *g_zrwCamCreate)(int type, ZObj *owner);                   /* 0x00561D90 .create = zrwCamCreate */
extern int   (__cdecl *g_zrwCamDestroy)(void *cam);                              /* 0x00561D94 .release = zrwCamRelease */
extern void  (__cdecl *g_zrwCamSetFov)(void *cam, int angle);                    /* 0x00561D98 .setFov = zrwCamSetFov */
extern void  (__cdecl *g_zrwCamSetView)(void *cam, const RwMatrix *view, const RwV3d *pos); /* 0x00561DA0 .setMatrix = zrwCamSetMatrix */
extern void  (__cdecl *g_zrwCamSetViewport)(void *cam, const float *rect);       /* 0x00561DAC .resize = zrwCamResize */
extern void  (__cdecl *g_zrwDbgRemoveCallback)(int handle);                      /* 0x0056005C */
extern int   (__cdecl *g_zrwDbgAddCallback)(int flags, int (__cdecl *fn)(void *arg), void *arg); /* 0x00560058 g_pfnDebugAddTextCallback = zrwDbgAddTextCallback */
extern int   (__cdecl *g_zrwZglInit)(void);                                      /* 0x00561468 g_zglSystemApi.init = zrwZglInit */
extern int   (__cdecl *g_zrwZglShutdown)(void);                                  /* 0x0056146C */
/* GHIDRA-SKIP-END */

#endif /* ZOBJ_H */

/*
 * zscene.h - NgEngine/Zge/zScene.c: the scene database (Scene.Txt) and the scene object registry
 * (replaces the earlier ZsceneObj placeholder).
 *
 * Scene.Txt is parsed by zsceneLoadDatabase 0x004904E0 into a tree of ZsceneDef blocks:
 *   SCENE_COMMON { MODEL { NAME .. FILE .. } ANIM { .. } }   -> g_zscene.common
 *   SCENE { NAME .. FILE <name>.ZSD  MODEL {..} ANIM {..} } -> g_zscene.scenes (+ children)
 *   END
 * zsceneOpen 0x004911C0 picks a SCENE by name, loads its ZSD (docs/formats/LEVEL.md) and every
 * level component. Everything created while a scene is open (models, anims, lights, cameras,
 * particle emitters) is registered as a ZsceneEntry and released by zsceneClose.
 * Offsets [V] (field use in the code), names [I].
 */
#ifndef ZSCENE_H
#define ZSCENE_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "zfile.h"
#include "ziff.h"

/* ZsceneDef.kind: the block keyword */
#define ZSCENE_DEF_SCENE 0     /* SCENE / SCENE_COMMON */
#define ZSCENE_DEF_MODEL 1     /* MODEL { } */
#define ZSCENE_DEF_ANIM  2     /* ANIM { } */

typedef struct ZsceneDef {
    char              name[0x20];  /* +0x00 NAME; "Object name is too long." above 31 chars */
    char              file[0x40];  /* +0x20 FILE; "File name is too long." above 63 chars */
    int32_t           kind;        /* +0x60 ZSCENE_DEF_* */
    uint32_t          id;          /* +0x64 zHashName(name) */
    int32_t           type;        /* +0x68 TYPE (number) */
    int32_t           subtype;     /* +0x6C SUBTYPE (number) */
    struct ZsceneDef *next;        /* +0x70 next sibling */
    struct ZsceneDef *child;       /* +0x74 first child block (newest first) */
} ZsceneDef;                       /* 0x78, zmem pool of 256 */

/* ZsceneEntry.type ("EzsceneObjType" in the assert text) */
#define ZSCENE_OBJ_MODEL   1
#define ZSCENE_OBJ_ANIM    2
#define ZSCENE_OBJ_LIGHT   3
#define ZSCENE_OBJ_CAMERA  4
#define ZSCENE_OBJ_EMITTER 5

typedef struct ZsceneEntry {
    ZsceneDef *def;          /* +0x00 definition the object came from (NULL for lights/cameras/emitters) */
    uint32_t   id;           /* +0x04 zHashName of the model/anim name */
    int8_t     type;         /* +0x08 ZSCENE_OBJ_* */
    uint8_t    isTemplate;   /* +0x09 1: obj is shared model data (prop def, panorama), 0: an instance */
    uint8_t    sceneOwned;   /* +0x0A 1: released by zsceneClose; 0: lives until zsceneShutdown */
    uint8_t    pad0B;        /* +0x0B */
    void      *obj;          /* +0x0C zobj, model, or ZsceneAnim list */
} ZsceneEntry;               /* 0x10, zmem pool of 256 */

/* one motion of an animation library (list built by the ANIM loaders) */
typedef struct ZsceneAnim {
    uint32_t           motionId; /* +0x00 first u32 of the motion (zanimGetId 0x0049A530) */
    void              *table;    /* +0x04 ZanimContents * from zanimLoadIff */
    struct ZsceneAnim *next;     /* +0x08 */
} ZsceneAnim;                    /* 0x0C, zmemAlloc */

/* request passed to a MODEL/ANIM file loader (ZsceneLoader.load) */
typedef struct ZsceneLoadReq {
    const char *path;   /* +0x00 ZsceneDef.file */
    void       *aux;    /* +0x04 model loaders: flags (0) in; anim loaders: ZsceneAnim * out */
    void       *model;  /* +0x08 model loaders: the model out */
} ZsceneLoadReq;        /* 0x0C */

typedef int (__cdecl *ZsceneLoadFn)(ZsceneLoadReq *req);

typedef struct ZsceneLoader {
    const char  *ext;      /* +0x00 upper-case extension: ZMD, NAD, IFF, ZAL */
    ZsceneLoadFn load;     /* +0x04 */
} ZsceneLoader;                                        /* 0x08, NULL-terminated tables 0x005618A0/0x005618B0 */

typedef struct ZsceneKindInfo {
    const char   *name;      /* +0x00 "SCENE", "MODEL", "ANIM" */
    ZsceneLoader *loaders;   /* +0x04 per-extension loaders (NULL for SCENE) */
} ZsceneKindInfo;            /* 0x08, table 0x005618D0 indexed by ZSCENE_DEF_* */

/* search keys for the recursive def search and the entry-pool search */
typedef struct ZsceneDefKey {
    int32_t    kind;     /* +0x00 */
    uint32_t   id;       /* +0x04 0 = any (set to 0xFFFFFFFF once matched) */
    uint32_t   unused;   /* +0x08 */
    ZsceneDef *found;    /* +0x0C */
} ZsceneDefKey;          /* 0x10 */

typedef struct ZsceneEntryKey {
    int32_t      type;   /* +0x00 */
    uint32_t     id;     /* +0x04 */
    ZsceneEntry *found;  /* +0x08 */
} ZsceneEntryKey;        /* 0x0C */

typedef struct ZsceneGlobals {
    int        initialized;  /* +0x00 0x00673F70 */
    void      *entryPool;    /* +0x04 0x00673F74 pool of ZsceneEntry */
    void      *defPool;      /* +0x08 0x00673F78 pool of ZsceneDef */
    ZsceneDef *scenes;       /* +0x0C 0x00673F7C the SCENE blocks (first one = list head) */
    ZsceneDef *current;      /* +0x10 0x00673F80 the open scene, NULL if none */
    ZsceneDef  common;       /* +0x14 0x00673F84 SCENE_COMMON */
} ZsceneGlobals;             /* 0x8C, cleared by zsceneInit */

/* chunk handler table passed to zsceneLoadComponent (not the 0x10-byte ZiffHandler) */
typedef int (__cdecl *ZsceneChunkFn)(ZiffFile *iff);

typedef struct ZsceneChunkHandler {
    uint32_t      id;   /* +0x00 4CC of a top-level chunk: TAGB, SCPT, SDPA, PDBL, PRBL */
    ZsceneChunkFn fn;   /* +0x04 */
} ZsceneChunkHandler;   /* 0x08 */

/* GHIDRA-SKIP-BEGIN */
typedef char zscene_h_size_check[(sizeof(ZsceneDef) == 0x78 && sizeof(ZsceneEntry) == 0x10 &&
                                  sizeof(ZsceneAnim) == 0x0C && sizeof(ZsceneLoadReq) == 0x0C &&
                                  sizeof(ZsceneLoader) == 0x08 && sizeof(ZsceneKindInfo) == 0x08 &&
                                  sizeof(ZsceneDefKey) == 0x10 && sizeof(ZsceneEntryKey) == 0x0C &&
                                  sizeof(ZsceneGlobals) == 0x8C && sizeof(ZsceneChunkHandler) == 0x08) ? 1 : -1];

ZsceneDef   *zsceneAllocDef(int kind);
int          zsceneFreeDef(ZsceneDef *def);
void         zsceneDefAddSibling(ZsceneDef *def, ZsceneDef *sibling);
void         zsceneDefAddChild(ZsceneDef *def, ZsceneDef *child);
ZsceneDef   *zsceneDefGetNext(ZsceneDef *def);
ZsceneDef   *zsceneDefGetChild(ZsceneDef *def);
int          zsceneParseObject(ZsceneDef *def);
int          zsceneLoadDatabase(char *fileName);
ZsceneDef   *zsceneFindDefRecurse(ZsceneDef *def, ZsceneDefKey *key);
int          zsceneMatchEntry(ZsceneEntry *entry, ZsceneEntryKey *key);
ZsceneEntry *zsceneFindEntry(int type, uint32_t id);
ZsceneEntry *zsceneAddObject(ZsceneDef *def, uint32_t id, int type, int isTemplate, void *obj);
int          zsceneLoadComponent(void *zsd, int zsdSize, const char *dir, int type, int nHandlers,
                                 const ZsceneChunkHandler *handlers);
int          zsceneLoadComponents(void *zsd, int zsdSize, const char *dir);
void         zsceneFreeObjects(int all);
int          zsceneRemoveObject(int type, int isTemplate, void *obj, ZsceneEntry *entry);
ZsceneLoadFn zsceneFindLoader(int kind, const char *ext);
int          zsceneLoadZmd(ZsceneLoadReq *req);
ZsceneAnim  *zsceneReadAnimIff(zfileHandle *file, ZsceneAnim *list);
int          zsceneLoadAnimFile(ZsceneLoadReq *req);
int          zsceneLoadAnimZal(ZsceneLoadReq *req);
int          zsceneInit(char *database);
int          zsceneShutdown(void);
ZsceneDef   *zsceneGetFirstScene(uint32_t unused);
int          zsceneOpen(char *sceneName);
int          zsceneClose(void);
void        *zsceneCreateModel(int flags, const char *name);
int          zsceneDestroyModel(void *obj);
ZsceneEntry *zsceneLoadAnim(int flags, const char *name);
int          zsceneFreeAnim(ZsceneEntry *lib);
void        *zsceneAnimFind(ZsceneEntry *lib, uint32_t motionId);
int          zsceneAnimGetTable(ZsceneEntry *lib, void **table, int tableSize);
void        *zsceneCreateCamera(int flags);
int          zsceneDestroyCamera(void *obj);
void        *zsceneCreateLight(int flags, int lightType, const RwRGBA *colour);
int          zsceneDestroyLight(void *obj);
void        *zsceneCreateEmitter(int flags);
int          zsceneDestroyEmitter(void *obj);
void         zsceneParseMotionName(const char *name, uint32_t *id, uint32_t *number);

extern ZsceneGlobals            g_zscene;              /* 0x00673F70 */
extern const ZsceneChunkHandler zsceneTagHandlers[1];     /* 0x00561900 TAGB */
extern const ZsceneChunkHandler zsceneSceneHandlers[2];   /* 0x00561908 SCPT, SDPA */
extern const ZsceneChunkHandler zscenePropDefHandlers[1]; /* 0x00561918 PDBL */
extern const ZsceneChunkHandler zscenePropHandlers[1];    /* 0x00561920 PRBL */
/* GHIDRA-SKIP-END */

#endif /* ZSCENE_H */

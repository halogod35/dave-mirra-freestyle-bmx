/*
 * zrwworld.h - NgEngine/Zge/Com/ZglRw/zrwWorld.c: the RenderWare world of a level (BSP._sd), its
 * materials (Mat._sd) and the ZSD index. File formats: docs/formats/LEVEL.md 3, 4.
 *
 * The engine core calls the world module through the table g_zrwWorldApi 0x005620E0 (zScene.c uses
 * slots open/close/findScfi). The world itself (RpWorld *) and its origin live in zrwZgl.c globals.
 * Offsets [V], names [I].
 */
#ifndef ZRWWORLD_H
#define ZRWWORLD_H
#include "dmtypes.h"
#include "zlist.h"

/* SCFI payload of a .ZSD (zrwWorldFindScfi asserts "Chunk out of sync" unless the chunk is 0x28) */
typedef struct ZsdScfiRecord {
    uint8_t  type;            /* +0x00 1 Mat, 2 PropDef, 3 Tag, 4 BSP, 5 Prop, 6 Scene */
    uint8_t  junk[3];         /* +0x01 not read */
    uint32_t size;            /* +0x04 component file size */
    char     name[32];        /* +0x08 file name relative to the ZSD folder */
} ZsdScfiRecord;              /* 0x28 */

#define ZSD_TYPE_MAT     1
#define ZSD_TYPE_PROPDEF 2
#define ZSD_TYPE_TAG     3
#define ZSD_TYPE_BSP     4
#define ZSD_TYPE_PROP    5
#define ZSD_TYPE_SCENE   6

typedef struct ZrwWorldApi {
    int   (__cdecl *open)(void *zsd, const char *dir);                 /* +0x00 zrwWorldOpen */
    int   (__cdecl *close)(void);                                      /* +0x04 zrwWorldClose */
    ZsdScfiRecord *(__cdecl *findScfi)(void *zsd, int type, uint32_t *size); /* +0x08 */
    void *(__cdecl *findMaterial)(uint32_t textureHash);               /* +0x0C */
    int   (__cdecl *getNumMaterials)(void);                            /* +0x10 */
    void *(__cdecl *getMaterial)(int index);                           /* +0x14 */
    void *(__cdecl *printInfo)(void);                                  /* +0x18 zrwWorldPrintInfo */
    int   (__cdecl *hasPVS)(void);                                     /* +0x1C */
    void  (__cdecl *createPVS)(void);                                  /* +0x20 */
    int   (__cdecl *enablePVS)(int on);                                /* +0x24 */
    int   (__cdecl *savePVS)(void);                                    /* +0x28 */
} ZrwWorldApi;                                                         /* 0x2C */

typedef struct ZrwWorldStats {
    int32_t sectorPolys;      /* +0x00 0x006741EC */
    int32_t sectorVerts;      /* +0x04 */
    int32_t clumpPolys;       /* +0x08 */
    int32_t clumpVerts;       /* +0x0C */
    int32_t lightIndex;       /* +0x10 0x006741FC */
    int32_t unk14;            /* +0x14 */
    int32_t pvsOldCallback;   /* +0x18 0x00674204 */
} ZrwWorldStats;              /* 0x1C, cleared before the counters run */

typedef struct ZrwWorldGlobals {
    int           initialized;    /* +0x00 0x006741E0 */
    int           isOpen;         /* +0x04 0x006741E4 */
    zHeapList    *materials;      /* +0x08 0x006741E8 the zrwMat materials of Mat._sd */
    ZrwWorldStats stats;          /* +0x0C 0x006741EC */
    char          path[0x80];     /* +0x28 0x00674208 BSP file name (PVS file is derived from it) */
    uint32_t      unkA8;          /* +0xA8 */
} ZrwWorldGlobals;                /* 0xAC, cleared by zrwWorldInit */

/* GHIDRA-SKIP-BEGIN */
typedef char zrwworld_h_size_check[(sizeof(ZsdScfiRecord) == 0x28 && sizeof(ZrwWorldApi) == 0x2C &&
                                    sizeof(ZrwWorldStats) == 0x1C && sizeof(ZrwWorldGlobals) == 0xAC) ? 1 : -1];

void          *zrwWorldCountSectorCB(void *sector, void *data);
void          *zrwWorldCountAtomicCB(void *atomic, void *data);
void          *zrwWorldCountClumpCB(void *clump, void *data);
void           zrwWorldPrintGeometry(void);
ZrwWorldStats *zrwWorldPrintInfo(void);
void          *zrwWorldPrintLightCB(void *light, void *data);
void           zrwWorldSetZglBounds(void);
ZsdScfiRecord *zrwWorldFindScfi(void *zsd, int type, uint32_t *size);
int            zrwWorldAddMaterialCB(void *material);
int            zrwWorldLoadMaterials(void *zsd, const char *dir);
void           zrwWorldHookRpMaterials(void);
int            zrwWorldFreeMaterials(void);
int            zrwWorldPVSProgressCB(int message, float percent);
void           zrwWorldDestroyPVS(void);
int            zrwWorldLoad(void *zsd, const char *dir);
int            zrwWorldCreateEmpty(void);
int            zrwWorldInit(void);
int            zrwWorldShutdown(void);
int            zrwWorldOpen(void *zsd, const char *dir);
int            zrwWorldClose(void);
void          *zrwWorldFindMaterial(uint32_t textureHash);
int            zrwWorldGetNumMaterials(void);
void          *zrwWorldGetMaterial(int index);
int            zrwWorldHasPVS(void);
void           zrwWorldCreatePVS(void);
void           zrwWorldCreatePVSBody(void);
int            zrwWorldEnablePVS(int on);
int            zrwWorldSavePVS(void);
int            zrwWorldRender(void);

extern ZrwWorldGlobals   g_zrwWorldState;   /* 0x006741E0 */
extern ZsdScfiRecord     g_zrwWorldScfi;    /* 0x006741B8 FindScfi returns a pointer to this copy */
extern const ZrwWorldApi g_zrwWorldApi;     /* 0x005620E0 */
/* GHIDRA-SKIP-END */

#endif /* ZRWWORLD_H */

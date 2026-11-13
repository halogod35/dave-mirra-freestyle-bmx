/*
 * zprop.h - NgEngine/Zge/ZgeMods/zprop.c: prop definitions (PropDef._sd) and placed props
 * (Prop._sd). File formats: docs/formats/LEVEL.md section 5.
 *
 * Both lists are intrusive doubly linked lists (zList, zlist.h; appended by zListAddTail 0x0048C150). The
 * game code gets &node->info (zpropFindDef returns ZpropDefInfo *, NgIFzbsp walks
 * zpropGetDefList and passes node + 8), so the payload is its own struct here.
 * Offsets [V] (field use in zprop.c, zbspInitPropDefShape and zpropListsFree), names [I].

 */
#ifndef ZPROP_H
#define ZPROP_H
#include "dmtypes.h"
#include "zlist.h"

/* PDIN record of PropDef._sd (read as one block, asserted size 0x38) */
typedef struct ZpropPdinRecord {
    uint32_t size;            /* +0x00 0x38 */
    uint32_t id;              /* +0x04 zHashName(name) */
    char     name[20];        /* +0x08 */
    float    extents[3];      /* +0x1C collision box size (x width, y height, z depth), RW units */
    float    extra[4];        /* +0x28 copied to ZpropDefInfo.extra; [1] and [3] are read as height offsets */
} ZpropPdinRecord;            /* 0x38 */

/* inner PRBL record of Prop._sd (asserted size 0x48) */
typedef struct ZpropPrblRecord {
    uint32_t size;            /* +0x00 0x48 */
    uint32_t defId;           /* +0x04 PDIN id */
    char     name[16];        /* +0x08 */
    float    matrix[9];       /* +0x18 rows right, up, at (ZGE axes) */
    float    pos[3];          /* +0x3C ZGE / 100 */
} ZpropPrblRecord;            /* 0x48 */

/* OBJI chunk header (all level files) */
typedef struct ZobjiHeader {
    uint32_t hdrSize;         /* +0x00 8 */
    uint32_t size;            /* +0x04 data bytes that follow */
} ZobjiHeader;                /* 0x08 */

typedef struct ZpropDefInfo {
    uint32_t id;              /* +0x00 node+0x08 */
    char     name[20];        /* +0x04 */
    void    *model;           /* +0x18 zobjModelLoadIff(MODL) */
    uint32_t unk1C;           /* +0x1C 0 (memset) */
    uint8_t *obji;            /* +0x20 OBJI data or NULL; byte 22 = collision class (zbspInitPropDefShape) */
    void   (__cdecl *objiFree)(void *p);      /* +0x24 0; zpropListsFree calls it instead of zmemFree */
    void    *collision;       /* +0x28 0; zbspInitPropDefShape allocates a 0x38-byte collision record lazily */
    void   (__cdecl *collisionFree)(void *p); /* +0x2C 0; same for collision */
    float    extents[3];      /* +0x30 PDIN +0x1C */
    float    extra[4];        /* +0x3C PDIN +0x28 */
} ZpropDefInfo;               /* 0x4C */

typedef struct ZpropDef {
    zListNode    link;        /* +0x00 */
    ZpropDefInfo info;        /* +0x08 */
} ZpropDef;                   /* 0x54, zmemAlloc */

typedef struct ZpropInfo {
    uint32_t      defId;      /* +0x00 node+0x08 */
    char          name[16];   /* +0x04 */
    void         *obj;        /* +0x14 zobj instance from zsceneCreateModel */
    ZpropDefInfo *def;        /* +0x18 */
    uint8_t      *obji;       /* +0x1C OBJI data or NULL (prop.c reads the u16 at +8) */
    void   (__cdecl *objiFree)(void *p);      /* +0x20 0; zpropListsFree calls it instead of zmemFree */
    void         *user;       /* +0x24 0; game data (prop.c) */
    void   (__cdecl *userFree)(void *p);      /* +0x28 0 */
} ZpropInfo;                  /* 0x2C */

typedef struct Zprop {
    zListNode link;           /* +0x00 */
    ZpropInfo info;           /* +0x08 */
} Zprop;                      /* 0x34, zmemAlloc */

/* GHIDRA-SKIP-BEGIN */
typedef char zprop_h_size_check[(sizeof(ZpropPdinRecord) == 0x38 && sizeof(ZpropPrblRecord) == 0x48 &&
                                 sizeof(ZobjiHeader) == 0x08 && sizeof(ZpropDefInfo) == 0x4C &&
                                 sizeof(ZpropDef) == 0x54 && sizeof(ZpropInfo) == 0x2C &&
                                 sizeof(Zprop) == 0x34) ? 1 : -1];

int           zpropListsInit(void);
int           zpropShutdown(void);
void          zpropListsFree(void);
int           zpropLoadDef(ZiffFile *iff);
int           zpropLoad(ZiffFile *iff);
ZpropDefInfo *zpropFindDef(uint32_t id);
zList    *zpropGetDefList(void);
zList    *zpropGetList(void);

extern zList g_zpropDefs;   /* 0x006722B8 */
extern zList g_zprops;      /* 0x006722C4 */
/* GHIDRA-SKIP-END */

#endif /* ZPROP_H */

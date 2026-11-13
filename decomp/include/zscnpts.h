/*
 * zscnpts.h - NgEngine/Zge/ZgeMods/zscnpts.c: scene points (Scene._sd SCPT forms) (DM-043).
 * File format: docs/formats/LEVEL.md section 5.3.
 *
 * Every SCPT becomes a ZscnPoint on a singly linked list (newest first). Game code looks points
 * up by (zHashName(name), type): "Player"/"PlayerT" (type = player number) for the start position
 * and heading (zrollerGetPlayerStartPos 0x0046CF30), "SpotLight"/"SpotLightT" (type 1..3 = light
 * slot) for the three directional lights (ngappCreateLevelLights), "Globals" (type 1) for the camera clear
 * colour (ngappFrame). Offsets [V], names [I].
 */
#ifndef ZSCNPTS_H
#define ZSCNPTS_H
#include "dmtypes.h"
#include "rwtypes.h"

/* SDPT record (asserted size 0x4C) */
typedef struct ZscnSdptRecord {
    uint32_t size;            /* +0x00 0x4C */
    uint32_t id;              /* +0x04 zHashName(name) */
    uint32_t type;            /* +0x08 */
    char     name[16];        /* +0x0C */
    float    matrix[9];       /* +0x1C rows right, up, at */
    float    pos[3];          /* +0x40 ZGE / 100 */
} ZscnSdptRecord;             /* 0x4C */

typedef struct ZscnPoint {
    uint32_t          id;     /* +0x00 */
    uint32_t          type;   /* +0x04 */
    RwMatrix          matrix; /* +0x08 right = -row0, up = row1, at = -row2, pos = pos * world scale */
    uint8_t          *obji;   /* +0x48 OBJI data (zmemAlloc) or NULL */
    struct ZscnPoint *next;   /* +0x4C */
} ZscnPoint;                  /* 0x50, zmem pool (grows; free callback zscnptsFreePoint) */

typedef struct ZscnptsGlobals {
    void      *debugTask;     /* +0x00 0x00674000 zrwZgl render callback handle */
    uint32_t   unk04;         /* +0x04 0x00674004 */
    uint32_t   unk08;         /* +0x08 0x00674008 cleared by Init/Reset/Unload, never read here */
    int        debugDraw;     /* +0x0C 0x0067400C */
    void      *pool;          /* +0x10 0x00674010 */
    ZscnPoint *points;        /* +0x14 0x00674014 */
} ZscnptsGlobals;             /* 0x18 */

/* GHIDRA-SKIP-BEGIN */
typedef char zscnpts_h_size_check[(sizeof(ZscnSdptRecord) == 0x4C && sizeof(ZscnPoint) == 0x50 &&
                                   sizeof(ZscnptsGlobals) == 0x18) ? 1 : -1];

typedef struct ZscnPointRef {         /* list element of zscnptsCollect (zmemAlloc(0xC)) */
    void      *link[2];               /* +0x00 zListNode filled by zListAddTail */
    ZscnPoint *point;                 /* +0x08 */
} ZscnPointRef;

int        zscnptsReset(void);
int        zscnptsFreePoint(ZscnPoint *p);
int        zscnptsInit(void);
void       zscnptsSetDebugDraw(int on);
int        zscnptsShutdown(void);
void       zscnptsUnload(void);
int        zscnptsFreeOne(ZscnPoint *p);
int        zscnptsLoad(ZiffFile *iff);
int        zscnptsDebugRender(void);
ZscnPoint *zscnptsFind(uint32_t id, uint32_t type);
int16_t    zscnptsCollect(void *list, uint32_t id);

extern ZscnptsGlobals g_zscnpts;    /* 0x00674000 */
/* GHIDRA-SKIP-END */

#endif /* ZSCNPTS_H */

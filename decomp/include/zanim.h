/*
 * zanim.h - NgEngine keyframe animation (NgEngine/Zge/zanim.c): the node table, the
 * loaded contents and the whole module (VA 0x00498D80-0x0049A6CB): the "NAD" key
 * channels loaded from ANIM/DEFI/AROT/ATRA IFF chunks, the per-node track lists and the blender.
 *
 * Offsets [V] (field use in the code), names [I]. Only the node table and the loader are used by
 * the shipped game (zsceneLoadAnim*, NgIFzactor_ApplyMotionPose, zrwModelUpdate); the track
 * playback/blending code (zanimPlay ... zanimApplyKeys) has no callers in NgBMX.exe.
 */
#ifndef ZANIM_H
#define ZANIM_H
#include "dmtypes.h"

/* node flags: which of quat / trans / scale the animation wrote this frame */
#define ZANIM_NODE_QUAT   1
#define ZANIM_NODE_TRANS  2
#define ZANIM_NODE_SCALE  4

typedef struct ZanimKey {          /* AROT keys use all four values; ATRA keys only v[0..2] */
    float    time;                 /* +0x00 ms from the start of the animation */
    float    v[4];                 /* +0x04 quaternion (x, y, z, w) or vector (x, y, z, unused) */
} ZanimKey;                        /* 0x14 */

typedef struct ZanimChannel {      /* lists at ZanimContents +0x08 (rot) / +0x0C (trans) / +0x10 (scale) */
    uint32_t count;                /* +0x00 number of keys */
    uint32_t hash;                 /* +0x04 node name hash (from the DEFI entry) */
    ZanimKey *keys;                /* +0x08 */
    struct ZanimChannel *next;     /* +0x0C */
} ZanimChannel;                    /* 0x10 */

typedef struct ZanimDefName {      /* one DEFI entry */
    char     name[32];             /* +0x00 node name */
    uint32_t hash;                 /* +0x20 zHashName(name) */
} ZanimDefName;                    /* 0x24 */

typedef struct ZanimNameList {     /* list at ZanimContents +0x04, one per DEFI chunk */
    uint32_t count;                /* +0x00 */
    ZanimDefName *names;           /* +0x04 */
    struct ZanimNameList *next;    /* +0x08 */
} ZanimNameList;                   /* 0x0C */

typedef struct ZanimContents {
    uint32_t       id;             /* +0x00 first long of the DEFI chunk (zanimGetId) */
    ZanimNameList *names;          /* +0x04 */
    ZanimChannel  *rot;            /* +0x08 AROT channels */
    ZanimChannel  *trans;          /* +0x0C ATRA channels */
    ZanimChannel  *scale;          /* +0x10 no loader in this build; freed by zanimFreeContents */
} ZanimContents;                   /* 0x14, filled by the chunk handlers while zanimLoadIff parses */

typedef struct ZanimTrack {        /* one playing animation on one node; pooled at 0x006746C4 */
    uint32_t fading;               /* +0x00 1 once zanimTrackFadeOut / zanimCrossFade started its fade-out */
    float    start;                /* +0x04 time the animation started */
    float    end;                  /* +0x08 start + time of the last key */
    float    expire;               /* +0x0C zanimEvaluate frees the track after this time */
    float    fadeInEnd;            /* +0x10 weight ramps 0 -> 1 over [start, fadeInEnd] */
    float    fadeOutStart;         /* +0x14 and 1 -> 0 over [fadeOutStart, fadeOutEnd] */
    float    fadeOutEnd;           /* +0x18 */
    float    weight;               /* +0x1C */
    uint32_t rotKey;               /* +0x20 key cursors (index of the current segment) */
    uint32_t transKey;             /* +0x24 */
    uint32_t scaleKey;             /* +0x28 */
    ZanimChannel *rot;             /* +0x2C */
    ZanimChannel *trans;           /* +0x30 */
    ZanimChannel *scale;           /* +0x34 */
    struct ZanimTrack *next;       /* +0x38 node list / pool link */
} ZanimTrack;                      /* 0x3C */

typedef struct ZanimNode {
    uint32_t flags;                /* +0x00 ZANIM_NODE_* */
    uint32_t hash;                 /* +0x04 frame name hash; the table is sorted by it */
    float    trans[3];             /* +0x08 translation (zrwModelUpdate adds it to the bind position) */
    uint32_t unk14;                /* +0x14 */
    float    quat[4];              /* +0x18 x, y, z, w (0, 0, 0, 1 initially); zrwModelUpdate turns it
                                    *       into the frame rotation with zrwModelQuatToRwMatrix */
    float    scale[3];             /* +0x28 1, 1, 1 initially */
    uint32_t unk34;                /* +0x34 */
    uint32_t unk38;                /* +0x38 */
    ZanimTrack *tracks;            /* +0x3C playing tracks, newest first */
    uint32_t unk40;                /* +0x40 */
} ZanimNode;                       /* 0x44 */

typedef struct ZanimNodeTable {
    int        count;              /* +0x00 */
    uint32_t   unk04;              /* +0x04 */
    ZanimNode *nodes;              /* +0x08 points at the inline array that follows */
} ZanimNodeTable;                  /* 0x0C header + count * ZanimNode */

typedef struct ZanimObj {          /* the first 0x34 bytes of a model zobj (zobjModelCreate 0x00488FB0) */
    uint32_t        unk00[11];     /* +0x00 */
    void           *model;         /* +0x2C the zrwModel */
    ZanimNodeTable *nodes;         /* +0x30 zanimCreateNodeTable (attached by 0x00489210) */
} ZanimObj;                        /* 0x34 (partial) */

typedef struct ZanimWeight {       /* zanimMix: per-node weight, sorted by hash */
    uint32_t hash;                 /* +0x00 */
    float    weight;               /* +0x04 */
} ZanimWeight;                     /* 0x08 */

typedef struct ZanimWeights {
    uint32_t     count;            /* +0x00 */
    ZanimWeight *entries;          /* +0x04 */
} ZanimWeights;                    /* 0x08 */

/* GHIDRA-SKIP-BEGIN */
typedef char zanim_h_size_check[(sizeof(ZanimKey) == 0x14 && sizeof(ZanimChannel) == 0x10 &&
                                 sizeof(ZanimDefName) == 0x24 && sizeof(ZanimNameList) == 0x0C &&
                                 sizeof(ZanimContents) == 0x14 && sizeof(ZanimTrack) == 0x3C &&
                                 sizeof(ZanimNode) == 0x44 && sizeof(ZanimNodeTable) == 0x0C &&
                                 sizeof(ZanimObj) == 0x34 && sizeof(ZanimWeight) == 0x08 &&
                                 sizeof(ZanimWeights) == 0x08) ? 1 : -1];
/* GHIDRA-SKIP-END */

#endif /* ZANIM_H */

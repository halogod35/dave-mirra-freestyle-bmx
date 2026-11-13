/*
 * zactor.h - Bmx/Src/com/zge/zactor.c: actors (the rider and bike objects of the game code) and
 * the motion.zal clip decoder (DM-053; DM-016 started it with a placeholder ZActor).
 *
 * Offsets [V] (field use in zactor.c, NgIFzactor.c, rider.c and physics.c), names [I].
 *
 * An actor owns a ZActorAnim (only for ZACTOR_KIND_PERSON, the rider and the bike): nine
 * ZMotionState blocks. Block 0 is the output pose that NgIFzactor_ApplyMotionPose turns into the
 * model's node quaternions; blocks 1..4 decode the rider section of a clip (decoder slot 0..3) and
 * blocks 5..8 hold the per-channel angles of the clip's other section (the bike) for the same slot.
 * Clip format: docs/formats/ANIM.md section 2.
 */
#ifndef ZACTOR_H
#define ZACTOR_H
#include "dmtypes.h"
#include "rwtypes.h"
#include <stddef.h>   /* offsetof (size checks) */

#define ZACTOR_MAX_CHANNELS   19        /* rows of the bone tables 0x0055F390 / 0x0055F4C0 */
#define ZACTOR_ANIM_BLOCKS    9

/* ZActor.kind (zactorSetup) */
#define ZACTOR_KIND_PERSON    1         /* animated: gets a ZActorAnim (rider, bike) */
#define ZACTOR_KIND_3         3         /* flags = 0x51, radius 80 */
#define ZACTOR_KIND_4         4         /* radius 50 */
#define ZACTOR_KIND_8         8         /* zactorSetScale accepts it */

/* ZActor.flags */
#define ZACTOR_VISIBLE        0x0001u   /* zactorShow / zactorHide (also the model's zobj flag) */
#define ZACTOR_FLAG_2         0x0002u   /* physics sets it on the rider and clears it on the bike */
#define ZACTOR_ANIMATED       0x0004u   /* has a ZActorAnim */
#define ZACTOR_SCALED         0x0008u   /* scale != (1, 1, 1): decoder output is scaled */
#define ZACTOR_HEAD_OVERRIDE  0x2000u   /* next decode copies ZMotionState.headYaw into rot[2][1] */
#define ZACTOR_MOTION_POSED   0x4000u   /* a clip was decoded into this actor */

/* ZMotionState.decodeFlags */
#define ZMOTION_WIDE          0x0010u   /* clip flags bit 15: 16-bit angles (all shipped clips) */

/* motion play flags (the BmxRider motion slot flags, DM-047) the decoder looks at */
#define ZMOTION_RESTART       0x0002u   /* decode the init records (frame 0) */
#define ZMOTION_MIRROR        0x0008u   /* swap left/right channels, negate y/z angles and x translation */
#define ZMOTION_CUT           0x0080u   /* restart without the crossfade from the previous clip */

/* A motion clip as stored in motion.zal (docs/formats/ANIM.md section 2). */
typedef struct ZMotionSection {
    uint8_t  channels;           /* +0x00 */
    uint8_t  flags;              /* +0x01 bit 1: no init record and never stepped; 0x80 (all clips) unread */
    uint16_t kind;               /* +0x02 0 = rider section, else the bike section */
} ZMotionSection;                /* 0x04 */

typedef struct ZMotionHeader {
    uint16_t frames;             /* +0x00 bits 0-14 frame count; bit 15: a 100-byte block follows the sections */
    uint16_t flags;              /* +0x02 bits 4-7 section count, bit 15 16-bit angles; the rest unread */
    uint32_t frameMs;            /* +0x04 34 */
    uint32_t extra;              /* +0x08 byte offset from the end of the section table (0) */
    ZMotionSection section[1];   /* +0x0C [section count] */
} ZMotionHeader;

/* The decoder's state for one section group of one clip (0x17C). In a ZActorAnim the rider-section
 * block (1..4) keeps all the stream fields; the matching bike block (5..8) only uses rotDelta / rot. */
typedef struct ZMotionState {
    uint32_t time;               /* +0x000 ms since the clip started (+= 34 x rate per call) */
    uint32_t frameMs;            /* +0x004 header frameMs */
    uint32_t duration;           /* +0x008 frames x frameMs */
    uint32_t nextFrame;          /* +0x00C time of the next frame step */
    const uint32_t *rec;         /* +0x010 next stream record (u32 mask 0 = terminator); rider.c reads it */
    uint32_t unk_014[2];
    float    value;              /* +0x01C the f32 of the last record with mask bit 0 (never read) */
    const uint8_t *extra;        /* +0x020 section table end + header extra (never read) */
    const ZMotionSection *sections; /* +0x024 */
    const uint8_t *block;        /* +0x028 the 100-byte block (frames bit 15), else stale */
    float    transDelta[3];      /* +0x02C rider translation delta per frame (x, z root motion; y height) */
    uint32_t unk_038;
    float    bikeTransDelta[3];  /* +0x03C */
    uint32_t unk_048;
    int16_t  rotDelta[19][3];      /* +0x04C per-channel angle delta; persists until replaced */
    uint16_t pad_0be;
    const ZMotionHeader *motion; /* +0x0C0 the clip */
    uint32_t playFlags;          /* +0x0C4 flags of the last decode call */
    uint32_t unk_0c8;
    uint32_t decodeFlags;        /* +0x0CC ZMOTION_WIDE; bits 0 and 5 survive a restart */
    float    lastDelta[3];       /* +0x0D0 transDelta of the last frame: the root delta of a restart */
    uint32_t unk_0dc;
    uint8_t  numSections;        /* +0x0E0 */
    uint8_t  numChannels;        /* +0x0E1 channels of section 0 */
    uint16_t pad_0e2;
    float    bikeTrans[3];       /* +0x0E4 bike section translation (bike relative to the rider) */
    uint32_t unk_0f0;
    int16_t  bikeRoot[3];        /* +0x0F4 the bike section's channel 0 (Hipsdeck) */
    int16_t  bikeRoot2[3];       /* +0x0FA cleared with bikeRoot, never set */
    float    height;             /* +0x100 rider translation y (accumulated) */
    int16_t  rot[19][3];      /* +0x104 per-channel Euler angles (s16 binary angles) */
    uint16_t rotBytes;           /* +0x176 0x72 = sizeof rot (zactorSetup, block 0) */
    int16_t  headYaw;            /* +0x178 copied into rot[2][1] on ZACTOR_HEAD_OVERRIDE */
    uint8_t  frameCount;         /* +0x17A rider frames stepped since the restart */
    uint8_t  pad_17b;
} ZMotionState;                  /* 0x17C */

typedef struct ZActorAnim {
    ZMotionState block[9];                   /* +0x000 [0] output pose, [1+slot] rider, [5+slot] bike */
    uint8_t  unk_d5c;
    uint8_t  fade;               /* +0xD5D crossfade frames left (3, 2, 1, 0) */
    uint8_t  pad_d5e[2];
} ZActorAnim;                    /* 0xD60, zmemTempAlloc'd by zactorSetup */

/* What one decode call returns (static buffers; rider.c keeps the pointer in BmxRider.motion[].frame). */
typedef struct ZactorFrame {
    float    delta[3];           /* +0x00 root delta x, (0), z this call, rotated by the global heading */
    uint32_t unk_0c;
    int16_t  yaw;                /* +0x10 Hips y angle relative to the global heading (never read) */
    uint16_t pad_12;
    float    value;              /* +0x14 ZMotionState.value (never read) */
} ZactorFrame;                   /* 0x18 */

/* zactorGetBikeOffset's result */
typedef struct ZactorBikeOffset {
    RwV3d    pos;                /* +0x00 bike translation rotated by the global heading (and scaled) */
    uint32_t unk_0c;
    int16_t  rot[3];             /* +0x10 the bike section's channel 0 angles (ZMotionState.bikeRoot) */
    int16_t  zero;               /* +0x16 0 */
} ZactorBikeOffset;              /* 0x18 */

typedef struct ZActor {
    int32_t  kind;               /* +0x000 ZACTOR_KIND_*; 0 = free pool slot */
    uint32_t flags;              /* +0x004 ZACTOR_* */
    int32_t  skeleton;           /* +0x008 row of the bone tables (zactorSetSkeleton; 19 bones per row) */
    RwV3d    pos;                /* +0x00C */
    uint32_t unk_018;
    int16_t  heading;            /* +0x01C atan2(-m.right.z, m.right.x) of the last orientation */
    int16_t  rot[3];             /* +0x01E x, y, z Euler angles (zactorSetRotFromMatrix) */
    uint32_t unk_024;
    RwV3d    scale;              /* +0x028 (1, 1, 1) */
    uint32_t unk_034;
    RwV3d    extent;             /* +0x038 zactorSetExtent; radius = its largest component (DM-064) */
    uint32_t unk_044;
    ZActorAnim *anim;            /* +0x048 */
    uint32_t unk_04c;
    uint32_t colour;             /* +0x050 0x808080 */
    uint32_t unk_054[2];
    uint32_t unk_05c;
    uint32_t unk_060;
    uint16_t unk_064;
    uint8_t  unk_066[0x10A];
    uint16_t modelId;            /* +0x170 LOD 0 model id (zactorSetModel / zactorSetLodModel 0) (DM-064) */
    uint16_t lod;                /* +0x172 zactorSetLod [I] */
    void    *user;               /* +0x174 zactorSetUser / zactorGetUser */
    uint32_t targetId;           /* +0x178 zactorSetTarget [I] */
    RwV3d    target;             /* +0x17C */
    uint8_t  unk_188[6];
    uint16_t unk_18e;            /* +0x18E 2 */
    uint32_t unk_190;
    uint8_t  lodData[0x3C];      /* +0x194 4 LOD records of 0x10 (u16 model at +0); the last one
                                    overlaps radius, only its first word is written (ZACTOR_LOD_MODEL) */
    float    radius;             /* +0x1D0 220 (person), 80 (kind 3), 50 (kind 4) */
    void    *model;              /* +0x1D4 zobj of the model (zrwModel) */
    void    *model2;             /* +0x1D8 second zobj posed with the same channels */
    uint32_t channelHash[19];                    /* +0x1DC zHashName of each channel's frame name */
    uint32_t channelMirror[19];                  /* +0x228 left/right partner of each channel */
    uint32_t numChannels;        /* +0x274 15 rider, 10 bike (NgIFzactor_SetRiderChannels/SetBikeChannels) */
} ZActor;                        /* 0x278 */

/* The in-memory motion bank (headerless ZAL, docs/formats/ZAL.md 2.6) as zactorOpenMemBank keeps it. */
typedef struct ZactorBank {
    const uint8_t  *base;        /* +0x00 offsets in the group table count from here */
    uint32_t        size;        /* +0x04 */
    const uint16_t *table;       /* +0x08 u16 assetsPerGroup, u16 groups, then the groups */
    const uint32_t *cursor;      /* +0x0C next group of zactorNextMotion */
    uint32_t        left;        /* +0x10 groups not yet returned */
} ZactorBank;                    /* 0x14 */

typedef struct ZactorBankEntry {
    uint32_t        id;          /* +0x00 motion id (1..238) */
    const uint8_t  *data;        /* +0x04 the clip */
} ZactorBankEntry;               /* 0x08 */

/* DM-064: the model id of LOD i (0..3), a 0x10-stride table from +0x194 */
#define ZACTOR_LOD_MODEL(a, i) (*(uint16_t *)((uint8_t *)(a) + 0x194 + (i) * 0x10))

/* zactorGetMotionTags' output record (DM-064; no clip ships tags: header extra is 0) */
typedef struct ZactorTag {
    RwV3d    pos;                /* +0x00 s16 / 8, rotated by the global heading */
    float    time;               /* +0x0C s16 / 1024 */
    int16_t  id;                 /* +0x10 0 unless the clip's tag records carry one */
    uint16_t pad_12;
} ZactorTag;                     /* 0x14 */

/* GHIDRA-SKIP-BEGIN */
typedef char zactor_h_size_check[(sizeof(ZMotionSection) == 0x04 && sizeof(ZMotionState) == 0x17C &&
                                  sizeof(ZActorAnim) == 0xD60 && sizeof(ZactorFrame) == 0x18 &&
                                  sizeof(ZactorBikeOffset) == 0x18 && sizeof(ZActor) == 0x278 &&
                                  sizeof(ZactorBank) == 0x14 && sizeof(ZactorBankEntry) == 0x08 &&
                                  sizeof(ZactorTag) == 0x14 && offsetof(ZActor, modelId) == 0x170 &&
                                  offsetof(ZActor, target) == 0x17C && offsetof(ZActor, lodData) == 0x194 &&
                                  offsetof(ZActor, radius) == 0x1D0) ? 1 : -1];

/* zactor.c (DM-053) */
void zactorStub1(void);                                                                              /* 0x004726C0 (DM-064) */
void zactorCopy(ZActor *dst, const ZActor *src);                                                     /* 0x004726E0 (DM-064) */
void zactorFree(ZActor *a);                                                                          /* 0x004728C0 (DM-064) */
void zactorClear(ZActor *a);                                                                         /* 0x004728E0 (DM-064) */
void zactorSetYaw(ZActor *a, int yaw);                                                               /* 0x00472960 (DM-064) */
int zactorGetYaw(ZActor *a);                                                                         /* 0x00472990 (DM-064) */
int zactorToWorldAngle(int angle);                                                                   /* 0x004729C0 (DM-064) */
int zactorToLocalAngle(int angle);                                                                   /* 0x00472A00 (DM-064) */
RwV3d *zactorGetScale(ZActor *a);                                                                    /* 0x00472AF0 (DM-064) */
void zactorSampleMotion(ZActor *a, int16_t (*rot)[3], const ZMotionHeader *m, int frames, uint8_t flags); /* 0x004739C0 (DM-064) */
void zactorLoadMotionBankTemp(uint32_t bankId);                                                      /* 0x00475140 (DM-064) */
void zactorStub2(void);                                                                              /* 0x004752C0 (DM-064) */
ZactorBank *zactorGetBank(int index);                                                                /* 0x004752E0 (DM-064) */
void zactorSetModel(ZActor *a, uint16_t model);                                                      /* 0x00475430 (DM-064) */
void zactorSetLodModel(ZActor *a, uint16_t lod, uint16_t model);                                     /* 0x00475460 (DM-064) */
uint16_t zactorGetModel(ZActor *a);                                                                  /* 0x004754D0 (DM-064) */
uint8_t zactorGetMotionTagCount(const ZMotionHeader *m);                                             /* 0x004754F0 (DM-064) */
uint8_t zactorGetMotionTags(const ZMotionHeader *m, ZactorTag *out);                                 /* 0x00475520 (DM-064) */
uint8_t zactorGetFirstMotionTag(const ZMotionHeader *m, ZactorTag *out);                             /* 0x00475730 (DM-064) */
void zactorSetLod(ZActor *a, uint8_t lod);                                                           /* 0x004758F0 (DM-064) */
void zactorSetRadius(ZActor *a, float radius);                                                       /* 0x00475920 (DM-064) */
void zactorSetExtent(ZActor *a, const RwV3d *extent);                                                /* 0x00475940 (DM-064) */
int zactorAddAngle(int angle, int delta);                                                            /* 0x00475A00 (DM-064) */
void zactorSetTarget(ZActor *a, const RwV3d *pos, uint32_t id);                                      /* 0x00475AD0 (DM-064) */
void zactorEnableTag(ZActor *a, int tag);                                                            /* 0x00475B10 (DM-064) */
void zactorDisableTag(ZActor *a, int tag);                                                           /* 0x00475B50 (DM-064) */
void zactorSetFlag20(ZActor *a);                                                                     /* 0x00475B90 (DM-064) */
void zactorClearFlag20(ZActor *a);                                                                   /* 0x00475BB0 (DM-064) */
void zactorStub3(void);                                                                              /* 0x00475BD0 (DM-064) */
void zactorSetFrameRate(int16_t rate);                                                               /* 0x00475BF0 (DM-064) */
void zactorStub4(void);                                                                              /* 0x00475C20 (DM-064) */
void zactorSetUser(ZActor *a, void *user);                                                           /* 0x00475C40 (DM-064) */
void *zactorGetUser(ZActor *a);                                                                      /* 0x00475C60 (DM-064) */
void zactorGetRootPos(ZActor *a, RwV3d *out);                                                        /* 0x00475C80 (DM-064) */
void zactorTurnHead(ZActor *a, int16_t delta);                                                       /* 0x004760B0 (DM-064) */
void zactorGetChannel(ZActor *a, uint16_t channel, int16_t *euler);                                  /* 0x00476190 (DM-064) */
int zactorGetSkeleton(ZActor *a);                                                                    /* 0x00476200 (DM-064) */
uint8_t zactorGetFrameCount(ZActor *a);                                                              /* 0x00476250 (DM-064) */
void zactorGetRotation(ZActor *a, int16_t *euler);                                                   /* 0x004762A0 (DM-064) */
void zactorSetRotation(ZActor *a, int16_t *euler);                                                   /* 0x00476340 (DM-064) */
void              zactorInit(void);                                                   /* 0x00472630 */
ZActor           *zactorAlloc(void);                                                  /* 0x00472730 */
ZActor           *zactorCreate(int kind);                                             /* 0x00472780 */
ZActor           *zactorSetup(int kind, ZActor *a);                                   /* 0x004727B0 */
void              zactorSetPos(ZActor *a, const RwV3d *pos);                          /* 0x00472910 */
RwV3d            *zactorGetPos(ZActor *a);                                            /* 0x00472940 */
void              zactorSetScale(ZActor *a, const RwV3d *scale);                      /* 0x00472A30 */
ZactorFrame      *zactorDecodeMotion(ZActor *a, float rate, uint32_t flags, ZactorFrame *out, uint32_t slot); /* 0x00472B10 */
int               zactorVecIsZero(const float *v);                                    /* 0x00474070 */
ZactorFrame      *zactorPlayMotion(ZActor *a, const ZMotionHeader *motion, float rate, uint32_t flags); /* 0x004740C0 */
void              zactorCopyBikePose(ZActor *rider, ZActor *bike);                    /* 0x00474960 */
void              zactorAngleMix50(const int16_t *a, const int16_t *b, int16_t *out, int channels); /* 0x004749A0 */
void              zactorAngleMix25(const int16_t *a, const int16_t *b, int16_t *out, int channels); /* 0x00474B10 */
void              zactorVecMix50(const float *a, const float *b, float *out);         /* 0x00474D20 */
void              zactorVecMix25(const float *a, const float *b, float *out);         /* 0x00474D70 */
ZactorBikeOffset *zactorGetBikeOffset(ZActor *a);                                     /* 0x00474DE0 */
int               zactorOpenMotionLibrary(const char *path);                          /* 0x00474F10 */
ZactorBank       *zactorLoadMotionBank(uint32_t bankId);                              /* 0x00475050 */
ZactorBank       *zactorOpenMemBank(const uint8_t *data, uint32_t size, int reuse);   /* 0x00475200 */
const uint8_t    *zactorFindMotion(uint32_t id);                                      /* 0x00475300 */
ZactorBankEntry  *zactorNextMotion(ZactorBank *bank);                                 /* 0x004753C0 */
void              zactorSetHeading(int16_t heading, int negate);                      /* 0x004759D0 */
void              zactorShow(ZActor *a);                                              /* 0x00475A30 */
void              zactorHide(ZActor *a);                                              /* 0x00475A60 */
void              zactorSetFlag2(ZActor *a);                                          /* 0x00475A90 */
void              zactorClearFlag2(ZActor *a);                                        /* 0x00475AB0 */
void              zactorGetSpinePoint(ZActor *a, RwV3d *out);                         /* 0x00475CC0 */
void              zactorGetBonePos(ZActor *a, int bone, const RwV3d *local, RwV3d *out, uint8_t flags); /* 0x00475E30 */
void              zactorSetChannel(ZActor *a, int channel, const int16_t *euler);     /* 0x00476140 */
void              zactorSetSkeleton(ZActor *a, int skeleton);                         /* 0x004761E0 */
void              zactorSetHeight(ZActor *a, float height);                           /* 0x00476220 */
float             zactorGetHeight(ZActor *a);                                         /* 0x00476280 */
void              zactorGetRootMatrix(ZActor *a, RwMatrix *m);                        /* 0x004762F0 */
void              zactorSetMatrix(ZActor *a, const RwMatrix *m);                      /* 0x004763B0 */
void              zactorSetOrientation(ZActor *a, const int16_t *euler, const RwV3d *up, const RwV3d *right,
                                       RwMatrix *out);                                /* 0x00476420 */
void              zactorSetRotFromMatrix(ZActor *a, const RwMatrix *m);               /* 0x00476770 */
void              zactorCopyRootAngles(ZActor *bike, ZActor *rider);                  /* 0x00476880 */
void              zactorCopyRotation(ZActor *dst, const ZActor *src);                 /* 0x004768F0 */
void              zactorLerpAngles(const int16_t *a, const int16_t *b, int16_t *out, float w, uint8_t channels); /* 0x00476930 */
void              zactorResetWeights(int16_t *angles, const int16_t *other, float *weights, uint8_t channels); /* 0x00476A10 */
ZactorFrame     **zactorPlayMotionBlend(ZActor *a, const ZMotionHeader *motionA, const ZMotionHeader *motionB,
                                        const uint16_t *flags, float w);              /* 0x00476AE0 */
ZactorFrame      *zactorPlaySecondMotion(ZActor *a, const ZMotionHeader *motion, uint32_t flags, float *weights); /* 0x00477210 */
void              zactorSelectAngles(const int16_t *a, const int16_t *b, int16_t *out, const uint8_t *mask,
                                     uint32_t channels);                              /* 0x004772A0 */
ZactorFrame     **zactorPlayMotionMasked(ZActor *a, const ZMotionHeader *motionA, const ZMotionHeader *motionB,
                                         const uint16_t *flags, const uint8_t *mask);  /* 0x00477330 */
/* GHIDRA-SKIP-END */

#endif /* ZACTOR_H */

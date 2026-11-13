/*
 * physics.h - BMX rider physics (Bmx/Src/com/action/physics.c) and the collision records it gets
 * from the NgIF collision layer (NgIFzcollide.c / NgIFzbsp.c) (DM-048).
 *
 * Offsets of the records are [V] (read/written at those offsets by physics.c, NgIFzcollide.c and
 * NgIFzbsp.c); names and meanings [I] unless noted. Units: ZGE world units (the level is in
 * centimetres, RW units x 100), y points DOWN (the "up" vector is {0, -1, 0}, g_bmxphysUp),
 * velocities are per frame, angles are 16-bit binary angles (0x10000 = 360 degrees).
 * Full description: docs/re/physics.md.
 */
#ifndef PHYSICS_H
#define PHYSICS_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "rider.h"

/* Physics states (rider->physState). [V] dispatch tables 0x00557898-0x0055793F, 7 entries each. */
enum {
    BMXPHYS_ROLL   = 0,     /* on the ground, two-wheel suspension model */
    BMXPHYS_AIR    = 1,     /* ballistic flight */
    BMXPHYS_CRASH  = 2,     /* bail: the rider and the bike are ragdoll/animation driven */
    BMXPHYS_GRIND_LOCK = 3, /* flying to a grind target (physTimer frames, physLockStep per frame) */
    BMXPHYS_GRIND  = 4,     /* grinding along an edge (surface 9), or wall-riding (11) / prop (12) */
    BMXPHYS_STALL_LOCK = 5, /* flying to a stall target */
    BMXPHYS_STALL  = 6,     /* stall / fast plant / wall tap: held at physLockPos */
    BMXPHYS_STATES = 7,
    BMXPHYS_NONE   = 0xFF   /* before the first bmxphysSetState */
};

/* Surface types (rider->surface, BmxContact.type). The record behind rider->surfaceObj is a
 * NgColFace (0x88 bytes), a NgColEdge (0x38) or a prop group (500 bytes) - see
 * bmxphysCopySurface. [V] values from the code; meanings [I]. 1..8 are the face-tag classes of
 * Tag._sd (TAGI face record code >> 12), docs/re/physics.md 2. */
enum {
    BMXSURF_GROUND     = 0,  /* untagged world face (or a tagged face whose edge is missing) */
    BMXSURF_QUARTER    = 1,  /* quarterpipe (id = coping edge code); never seen with the shipped data: the
                                collision layer strips class 1 when the record's extra == 0 (always) */
    BMXSURF_SPINE      = 2,  /* spine face: air from it hops over to the other side */
    BMXSURF_VERT       = 3,  /* vert wall: like 1 but the take-off keeps no horizontal speed */
    BMXSURF_KICKER_S   = 4,  /* launch ramps: classes 4..8 cap the take-off speed at 11, 15, 21, 23, 35 */
    BMXSURF_KICKER_M   = 5,
    BMXSURF_KICKER_L   = 6,
    BMXSURF_KICKER_XL  = 7,
    BMXSURF_KICKER_XXL = 8,
    BMXSURF_EDGE       = 9,  /* a grind edge (NgColEdge): tagged world edge or prop rail */
    BMXSURF_FACE10     = 10, /* stall target face (only from bmxphysStateInitLockOn's search) */
    BMXSURF_WALL       = 11, /* wall for a fast plant / wall tap (tricks 0x2A/0x2B) */
    BMXSURF_PROP       = 12, /* prop / prop group (500-byte record) */
    BMXSURF_HAZARD_D   = 13, /* fell on a hazard material (material flags bits 19-20 = 0 or 3) */
    BMXSURF_HAZARD_E   = 14  /* hazard material with bits 19-20 = 1 */
};

/* A face hit by the collision layer: what NgColHit.face points to (0x88 bytes; copied whole by
 * bmxphysCopySurface). Built by zcolSegmentTest from the RW intersection and the Tag._sd
 * face record of the nearest tagged face. */
typedef struct NgColFace {
    float    normal[3];        /* +0x00 unit normal (ZGE axes) */
    float    d;                /* +0x0C plane: dot(normal, p) + d = 0 */
    uint32_t unk_10;           /* +0x10 0 */
    uint8_t  kind;             /* +0x14 0x88 for world faces */
    uint8_t  flags;            /* +0x15 tag flags remapped: TAGI bit0 -> 0x10, bit1 -> 0x08, bit2 -> 0x20, bit3 -> 0x40 */
    uint16_t code;             /* +0x16 tag code: class << 12 | id (class 1 with extra == 0 keeps only the id) */
    uint8_t  unk_18[4];        /* +0x18 0 */
    int16_t *vtx[3];           /* +0x1C integer corners (NgIFzbsp faces: ivtx; zbspFacePointInside) */
    const void *tagFace;       /* +0x28 the ZtagFace (zbspMakeTagFace); a 4th corner when flags bit 0 (DM-057) */
    void    *prop;             /* +0x2C NgPropCol * of a prop switch face (zbspFindTaggedFaces) */
    float    v0[3];            /* +0x30 triangle corners */
    uint8_t  unk_3c[4];
    float    v1[3];            /* +0x40 */
    uint8_t  unk_4c[4];
    float    v2[3];            /* +0x50 */
    uint8_t  unk_5c[4];
    int16_t  ivtx[3][6];       /* +0x60 integer copies of the corners (x, y, z, 3 unused) */
    const void *rwTri;         /* +0x84 the RW collision triangle (zbspColSphere faces) */
} NgColFace;                   /* 0x88 */

/* A grind edge (0x38 bytes): a tagged world edge (zbspFindEdgesNear / 00467410 from Tag._sd edge
 * records) or a prop rail (zcolMakeRail). */
typedef struct NgColEdge {
    uint8_t  flags;            /* +0x00 bit 0 / bit 1: open end on the low / high side (prop rails) */
    uint8_t  pad_01[3];
    uint8_t  kind;             /* +0x04 edge-tag kind 0..3 (TAGI edge record byte 2) or prop class: sound 1 -> metal (7), 2 -> wood (2) */
    uint8_t  pad_05[3];
    int16_t  v0[3];            /* +0x08 end points (integer ZGE units) */
    int16_t  v1[3];            /* +0x0E */
    int16_t  length;           /* +0x14 |v1 - v0| */
    uint16_t code;             /* +0x16 edge-tag code (the id that class 1..3 faces name) */
    float    dir[3];           /* +0x18 unit v0 -> v1 */
    float    normal[3];        /* +0x24 horizontal normal: cross(up, dir) */
    float    d;                /* +0x30 -dot(normal, v0) */
    void    *owner;            /* +0x34 prop record, or 0 for world edges (prop: +0x1EC = game prop) */
} NgColEdge;                   /* 0x38 */

/* Result of a segment test against the world and the props (zcolSegmentTest, 0xD8 bytes;
 * physics keeps one at 0x0056C128). [V] layout from the writer. */
typedef struct NgColHit {
    uint32_t type;             /* +0x00 1 = world face, 2 = prop */
    uint32_t unk_04;           /* +0x04 0 */
    float    pos[3];           /* +0x08 hit point */
    uint8_t  unk_14[4];
    float    t;                /* +0x18 fraction along the segment, 0..1 ("Degenerate collision" assert) */
    float    normal[3];        /* +0x1C */
    float    d;                /* +0x28 -dot(normal, v0) */
    uint32_t hasFace;          /* +0x2C 1 when face is valid */
    NgColFace *face;           /* +0x30 -> faceData (or 0) */
    void    *prop;             /* +0x34 prop collision record (type 2) */
    uint32_t *material;        /* +0x38 -> materialFlags */
    uint16_t propId;           /* +0x3C */
    uint16_t pad_3e;
    void    *propObj;          /* +0x40 prop object */
    uint8_t  unk_44[4];
    NgColFace faceData;        /* +0x48 */
    uint32_t materialFlags;    /* +0xD0 bits 0-3 surface type, 19-20 hazard kind, 21-29 kept */
    uint32_t unk_d4;           /* +0xD4 0 */
} NgColHit;                    /* 0xD8 */

/* Vertical probe result (bmxphysProbeGround, 0x30 bytes, caller's stack). */
typedef struct BmxProbe {
    uint32_t flags;            /* +0x00 bit 0: hit a prop */
    float    pos[3];           /* +0x04 hit point */
    uint8_t  unk_10[4];
    float    normal[3];        /* +0x14 */
    float    d;                /* +0x20 */
    NgColFace *face;           /* +0x24 world face (or 0) */
    uint32_t *material;        /* +0x28 */
    void    *prop;             /* +0x2C prop (flags bit 0) */
} BmxProbe;                    /* 0x30 */

/* GHIDRA-SKIP-BEGIN */
typedef int  (__cdecl *BmxPhysTransFn)(BmxRider *r, int from, int to);   /* exit / enter */
typedef void (__cdecl *BmxPhysStepFn)(BmxRider *r);

typedef char physics_h_size_check[(sizeof(NgColFace) == 0x88 && sizeof(NgColEdge) == 0x38 &&
                                   sizeof(NgColHit) == 0xD8 && sizeof(BmxProbe) == 0x30) ? 1 : -1];

/* physics.c, VA 0x0041FA20-0x0042DE23 (DM-048) */
void      bmxphysInit(void);                                                      /* 0x0041FA20 */
void      bmxphysShutdown(void);                                                  /* 0x0041FA40 */
void      bmxphysSetLimbTargets(BmxRider *r);                                     /* 0x0041FA60 */
void      bmxphysUpdate(void);                                                    /* 0x0041FC40 */
void      bmxphysResetRider(BmxRider *r);                                         /* 0x0041FE10 */
NgColFace *bmxphysFindRespawnFace(const RwV3d *pos, uint32_t flags);              /* 0x004200B0 */
int       bmxphysDebugRender(void);                                               /* 0x004203A0 */
int       bmxphysGetState(const BmxRider *r);                                     /* 0x00420C90 */
int       bmxphysSetState(BmxRider *r, int state);                                /* 0x00420CB0 */
void      bmxphysAddVelocity(BmxRider *r, const RwV3d *dv);                       /* 0x00420D30 */
int       bmxphysProbeGround(const RwV3d *pos, BmxProbe *out);                    /* 0x00420D70 */
int       bmxphysStateExitGrind(BmxRider *r, int from, int to);                   /* 0x00420F00 */
int       bmxphysStateExitAir(BmxRider *r, int from, int to);                     /* 0x00420F30 */
int       bmxphysStateExitCrash(BmxRider *r, int from, int to);                   /* 0x004210A0 */
int       bmxphysStateInitRoll(BmxRider *r, int from, int to);                    /* 0x004210C0 */
int       bmxphysStateInitLockOn(BmxRider *r, int from, int to);                  /* 0x00421190 */
int       bmxphysStateInitGrind(BmxRider *r, int from, int to);                   /* 0x00422830 */
int       bmxphysStateInitAir(BmxRider *r, int from, int to);                     /* 0x00422E50 */
int       bmxphysStateInitStall(BmxRider *r, int from, int to);                   /* 0x004243B0 */
int       bmxphysStateInitCrash(BmxRider *r, int from, int to);                   /* 0x00424740 */
void      bmxphysUpdateOrientationRoll(BmxRider *r);                              /* 0x00424800 */
void      bmxphysUpdateOrientationAir(BmxRider *r);                               /* 0x00424A10 */
void      bmxphysUpdateOrientationGrind(BmxRider *r);                             /* 0x00424EA0 */
void      bmxphysUpdateOrientationLockOn(BmxRider *r);                            /* 0x00425610 */
void      bmxphysUpdateForcesRoll(BmxRider *r);                                   /* 0x00425740 */
void      bmxphysUpdateForcesAir(BmxRider *r);                                    /* 0x00425760 */
void      bmxphysUpdateForcesGrind(BmxRider *r);                                  /* 0x004257E0 */
void      bmxphysUpdateForcesLockOn(BmxRider *r);                                 /* 0x00425910 */
void      bmxphysUpdatePositionRoll(BmxRider *r);                                 /* 0x00425940 */
void      bmxphysUpdatePositionAir(BmxRider *r);                                  /* 0x00428DF0 */
void      bmxphysUpdatePositionLockOn(BmxRider *r);                               /* 0x0042AFD0 */
void      bmxphysUpdatePositionGrind(BmxRider *r);                                /* 0x0042B0A0 */
NgColEdge *bmxphysFindNearestEdge(BmxRider *r, const RwV3d *pos, float radius, RwV3d *closest); /* 0x0042B3F0 */
int       bmxphysEdgeEquals(const NgColEdge *a, const NgColEdge *b);              /* 0x0042B570 */
void      bmxphysUpdatePositionCrash(BmxRider *r);                                /* 0x0042B5E0 */
void      bmxphysPostRoll(BmxRider *r);                                           /* 0x0042B830 */
void      bmxphysPostAir(BmxRider *r);                                            /* 0x0042BA50 */
void      bmxphysPostGrind(BmxRider *r);                                          /* 0x0042C7E0 */
void      bmxphysSnapToEdge(BmxRider *r, NgColEdge *edge);                        /* 0x0042CA90 */
void      bmxphysClosestPointOnEdge(const RwV3d *p, const NgColEdge *e, RwV3d *out); /* 0x0042CCA0 */
void      bmxphysPlaceActorsRoll(BmxRider *r);                                    /* 0x0042CCE0 */
void      bmxphysPlaceActors(BmxRider *r);                                        /* 0x0042D050 */
void      bmxphysMatrixDeltaAngles(const RwMatrix *from, const RwMatrix *to, int steps, int16_t *euler); /* 0x0042D250 */
void      bmxphysAlignToRamp(BmxRider *r, NgColEdge *edge, int always);           /* 0x0042D3C0 */
float     bmxphysEdgePlaneDistance(const RwV3d *p, const float *plane);           /* 0x0042D670 */
void      bmxphysClosestPointOnSegment(const int16_t *a, const int16_t *b, const int16_t *p, float *out); /* 0x0042D6E0 */
NgColEdge *bmxphysFindTaggedEdge(uint32_t ctx, int id, const RwV3d *pos);         /* 0x0042D950 */
int       bmxphysSetSurfaceFromHit(BmxRider *r, NgColHit *hit);                   /* 0x0042DAD0 */
void      bmxphysCopyContactSurface(BmxContact *c);                               /* 0x0042DCE0 */
void      bmxphysCopySurface(BmxRider *r);                                        /* 0x0042DDA0 */

extern const BmxPhysTransFn g_bmxphysExit[BMXPHYS_STATES];          /* 0x00557898 */
extern const BmxPhysTransFn g_bmxphysEnter[BMXPHYS_STATES];         /* 0x005578B4 */
extern const BmxPhysStepFn  g_bmxphysOrientation[BMXPHYS_STATES];   /* 0x005578D0 */
extern const BmxPhysStepFn  g_bmxphysForces[BMXPHYS_STATES];        /* 0x005578EC */
extern const BmxPhysStepFn  g_bmxphysPosition[BMXPHYS_STATES];      /* 0x00557908 */
extern const BmxPhysStepFn  g_bmxphysPost[BMXPHYS_STATES];          /* 0x00557924 */
/* GHIDRA-SKIP-END */

#endif /* PHYSICS_H */

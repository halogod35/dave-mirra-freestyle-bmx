/*
 * dyn.h - the crash rag doll ("dyn*") and the limb IK of Bmx/src/com/zge/bmx/dyn.c.
 *
 * A DynBody is a small particle system (Verlet-style: position, previous position, velocity) with
 * three kinds of links:
 *   - springs (DynSpring): relaxed together by weighted averaging, two passes per step;
 *   - limits (DynLimit): one-sided plane constraints that keep a knee / elbow on its side of the
 *     body (applied once per step, before the springs);
 *   - the world: every moving particle is swept against the level and the props (zbspColSphere) as a
 *     sphere of radius 20, with bounce and friction.
 * The rider body has 14 particles (joints of the rider skeleton, dynCreateRider) and 21 springs; the
 * bike body 3 particles and 3 springs. Layouts [V] from the code that reads / writes them (dynCreate
 * allocates 0xE88 bytes, particles are 0xB4 apart, springs 0x20, limits 0x28); names [I].
 * docs/re/physics.md section 7.
 */
#ifndef DYN_H
#define DYN_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "physics.h"
#include "zactor.h"

#define DYN_MAX_PARTICLES   15
#define DYN_MAX_SPRINGS     22
#define DYN_MAX_LIMITS      7
#define DYN_RADIUS          20.0f   /* particle collision radius (engine units) */

/* DynParticle.type: 0 does not move (keeps its previous position) */
#define DYN_FIXED           0       /* stuck to the ground (Sticky Crash) or a hand holding an edge */
#define DYN_RIDER           1
#define DYN_BIKE            2

/* DynParticle.flags */
#define DYNF_CONTACT        0x0001u /* touched the world this step */
#define DYNF_REST           0x0002u /* touched it nearly at rest: velocity zeroed */

/* DynSpring.type */
#define DYN_SPRING          0       /* keep the rest length */
#define DYN_SPRING_BEND     1       /* + weight that grows when c's up axis turns against d's right axis (unused) */
#define DYN_SPRING_ANGLE    2       /* angle spring: recorded, never solved (dynSolveAngleSpring is empty; unused) */

/* DynLimit.type */
#define DYN_LIMIT           0       /* push a onto plane (pivot, frameB axisB) and b onto plane (pivot, frameA axisA) */
#define DYN_LIMIT_B         7       /* push b only (clamped to 4 units per step) */
#define DYN_LIMIT_AXIS      9       /* pull b towards a - b.up x length (unused) */

/* DynLimit.side: when the limit acts (d = signed distance of the particle from the plane) */
#define DYN_SIDE_BOTH       0
#define DYN_SIDE_BEHIND     1       /* acts while d >= 0: keeps the particle behind the plane */
#define DYN_SIDE_FRONT      2       /* acts while d <= 0: keeps it in front */

/* DynLimit.axisA / axisB: an axis of a particle's frame */
#define DYN_AXIS_RIGHT      1
#define DYN_AXIS_UP         2
#define DYN_AXIS_AT         3
#define DYN_AXIS_NRIGHT     4
#define DYN_AXIS_NUP        5       /* also any other value */
#define DYN_AXIS_NAT        6

/* What dynAddParticle takes (0x20, on the caller's stack). */
typedef struct DynParticleDesc {
    uint16_t type;             /* +0x00 DYN_RIDER / DYN_BIKE (>= 3 is refused) */
    uint16_t pad_02;
    RwV3d    pos;              /* +0x04 start position (the rider's are set from the skeleton afterwards) */
    float    unk_10;           /* +0x10 not read */
    float    mass;             /* +0x14 */
    float    bounce;           /* +0x18 restitution: the normal speed kept is impact x bounce x 0.95 */
    float    friction;         /* +0x1C the tangential speed is scaled by 1 - friction x 0.35 per impact */
} DynParticleDesc;             /* 0x20 */

typedef struct DynParticle {
    uint16_t type;             /* +0x00 DYN_FIXED / DYN_RIDER / DYN_BIKE */
    uint16_t flags;            /* +0x02 DYNF_* */
    RwMatrix frame;            /* +0x04 orientation of the body segment at this joint (dynStepRider,
                                  dynStepBike); the limits read their axes */
    float    mass;             /* +0x44 */
    float    weight;           /* +0x48 sum of the spring weights pulling it (relaxation pass) */
    float    bounce;           /* +0x4C */
    float    friction;         /* +0x50 */
    RwV3d    pos;              /* +0x54 */
    uint32_t pad_60;
    RwV3d    prevPos;          /* +0x64 */
    uint32_t pad_70;
    RwV3d    sum;              /* +0x74 weighted sum of the spring targets (relaxation pass) */
    uint32_t pad_80;
    RwV3d    vel;              /* +0x84 per step */
    uint8_t  unk_90[0x24];     /* +0x90 not used */
} DynParticle;                 /* 0xB4 */

/* A spring between particles a and b (0x20). */
typedef struct DynSpring {
    uint32_t type;             /* +0x00 DYN_SPRING_* */
    float    weight;           /* +0x04 relaxation weight ("stiffness"); 0 = the joint is free */
    DynParticle *a;            /* +0x08 */
    DynParticle *b;            /* +0x0C */
    float    rest;             /* +0x10 rest length (DYN_SPRING_ANGLE: int16 rest angle, u8 i0, u8 i1) */
    DynParticle *c;            /* +0x14 DYN_SPRING_BEND (DYN_SPRING_ANGLE: low byte = u8 i2) */
    DynParticle *d;            /* +0x18 DYN_SPRING_BEND */
    float    bend;             /* +0x1C DYN_SPRING_BEND: extra weight per unit of -dot(c.up, d.right) */
} DynSpring;                   /* 0x20 */

/* What dynAddLimit / dynAddLimitB take (0x24, on the caller's stack). */
typedef struct DynLimitDesc {
    uint32_t axisA;            /* +0x00 */
    uint32_t axisB;            /* +0x04 */
    DynParticle *a;            /* +0x08 */
    DynParticle *b;            /* +0x0C */
    DynParticle *pivot;        /* +0x10 */
    DynParticle *frameA;       /* +0x14 */
    DynParticle *frameB;       /* +0x18 */
    uint32_t side;             /* +0x1C DYN_SIDE_* */
    float    strength;         /* +0x20 */
} DynLimitDesc;                /* 0x24 */

/* A one-sided plane limit (0x28). */
typedef struct DynLimit {
    uint32_t type;             /* +0x00 DYN_LIMIT* */
    DynParticle *a;            /* +0x04 */
    DynParticle *b;            /* +0x08 */
    DynParticle *pivot;        /* +0x0C point on both planes */
    DynParticle *frameA;       /* +0x10 owner of axisA */
    DynParticle *frameB;       /* +0x14 owner of axisB */
    uint32_t side;             /* +0x18 DYN_SIDE_* */
    uint32_t axisA;            /* +0x1C DYN_AXIS_* */
    uint32_t axisB;            /* +0x20 DYN_AXIS_* */
    float    strength;         /* +0x24 fraction of the violation corrected per step (0 = off);
                                  DYN_LIMIT_AXIS: the length */
} DynLimit;                    /* 0x28 */

/* impact callback: particle index, the speed into the surface and the contact (rider.c plays the crash
 * sounds and sparks from it) */
typedef void (__cdecl *DynImpactFn)(void *body, int particle, float force, NgColHit *hit);

typedef struct DynBody {
    uint32_t    numParticles;                  /* +0x000 */
    DynParticle particle[15];                  /* +0x004 */
    uint32_t    numSprings;                    /* +0xA90 */
    DynSpring   spring[22];                    /* +0xA94 */
    uint32_t    numLimits;                     /* +0xD54 dynSetNormalCrash: 6; dynClearLimits: 0 */
    DynLimit    limit[7];                      /* +0xD58 */
    RwV3d       centre;                        /* +0xE70 mass centre; the step works relative to it.
                                                  physics.c copies it to the rider / bike position */
    uint32_t    pad_e7c;
    DynImpactFn onImpact;                      /* +0xE80 dynSetImpactCallback */
    uint32_t    starting;                      /* +0xE84 1 from dynStartRider to the first dynStepRider:
                                                  no limits and no stick input in that step */
} DynBody;                                     /* 0xE88, zmemTempAlloc'd by dynCreate */

/* dynCollideParticle's scratch: the swept particle projected on a face normal (a global, 0x0068FFC0). */
typedef struct DynSlab {
    float    centre;           /* +0x00 mean of the start and end distances along the normal */
    float    half;             /* +0x04 half the swept distance + DYN_RADIUS */
    float    dist;             /* +0x08 the plane's distance (-face.d) */
    uint32_t zero;             /* +0x0C 0 */
    RwV3d    normal;           /* +0x10 */
} DynSlab;                     /* 0x1C */

/* GHIDRA-SKIP-BEGIN */
typedef char dyn_h_size_check[(sizeof(DynParticleDesc) == 0x20 && sizeof(DynParticle) == 0xB4 &&
                               sizeof(DynSpring) == 0x20 && sizeof(DynLimitDesc) == 0x24 &&
                               sizeof(DynLimit) == 0x28 && sizeof(DynBody) == 0xE88 &&
                               sizeof(DynSlab) == 0x1C) ? 1 : -1];

/* rider particles (dynCreateRider order) and the rider skeleton bone each one sits on */
enum {
    DYNP_PELVIS = 0,           /* Chest's origin (bone 1) */
    DYNP_RHIP, DYNP_LHIP,      /* RightUpLeg 12, LeftUpLeg 9 */
    DYNP_RKNEE, DYNP_LKNEE,    /* RightLowLeg 13, LeftLowLeg 10 */
    DYNP_RANKLE, DYNP_LANKLE,  /* RightFoot 14, LeftFoot 11 */
    DYNP_RSHOULDER, DYNP_LSHOULDER, /* RightUpArm 6, LeftUpArm 3 */
    DYNP_RELBOW, DYNP_LELBOW,  /* RightLowArm 7, LeftLowArm 4 */
    DYNP_RWRIST, DYNP_LWRIST,  /* RightHand 8, LeftHand 5 */
    DYNP_HEAD,                 /* 25 units up the Head bone (2) */
    DYNP_RIDER_COUNT
};

/* ------------------------------------------------------------------ dyn.c */
DynBody *dynCreate(void);                                                          /* 0x00450060 */
int      dynAddParticle(DynBody *b, const DynParticleDesc *desc);                  /* 0x004500A0 */
int      dynAddSpring(DynBody *b, int i, int j, float weight);                     /* 0x00450140 */
int      dynAddBendSpring(DynBody *b, int i, int j, int c, int d, float weight, float bend); /* 0x00450210 */
int      dynAddAngleSpring(DynBody *b, int i, int j, int i0, int i1, int i2, float weight); /* 0x00450300 */
int      dynAddLimit(DynBody *b, const DynLimitDesc *desc);                        /* 0x00450430 */
int      dynAddLimitB(DynBody *b, const DynLimitDesc *desc);                       /* 0x004504C0 */
int      dynAddAxisLimit(DynBody *b, const DynSpring *s);                          /* 0x00450550 */
void     dynStep(DynBody *b);                                                      /* 0x00450E80 */
void     dynSetImpactCallback(void *body, DynImpactFn fn);                         /* 0x00451280 */
void     dynFrameFromParticles(DynBody *b, int i, int j, int k, RwMatrix *out);    /* 0x004512A0 */
void    *dynCreateBike(void);                                                      /* 0x004526A0 */
void     dynStartBike(void *body, ZActor *actor, const RwMatrix *m, const float *vel, const int16_t *spin); /* 0x004527C0 */
void     dynStepBike(void *body, ZActor *actor);                                   /* 0x00452A40 */
void    *dynCreateRider(void *actor, int superCrash);                              /* 0x00452E80 */
void     dynStartRider(void *body, ZActor *actor, const RwMatrix *m, const float *vel, const int16_t *spin); /* 0x004534A0 */
void     dynGrab(void *body, void *actor);                                         /* 0x004535D0 */
int      dynIsHanging(void);                                                       /* 0x00453870 */
void     dynRelease(void *body, void *actor);                                      /* 0x00453890 */
void     dynBrace(void *body, void *actor);                                        /* 0x00453AB0 */
void     dynUnbrace(void *body, void *actor);                                      /* 0x00453B70 */
void     dynTuck(void *body, void *actor);                                         /* 0x00453F80 */
void     dynUntuck(void *body, void *actor);                                       /* 0x00454140 */
void     dynSetLimitsOff(void *body, void *actor);                                 /* 0x004542F0 */
void     dynSetLimitsOn(void *body, void *actor);                                  /* 0x00454320 */
void     dynSetStickyCrash(void *body, void *actor);                               /* 0x00454400 */
void     dynSetNormalCrash(void *body, void *actor);                               /* 0x00454430 */
void     dynSetPitchInput(void *body, void *actor, int16_t angle);                 /* 0x00454660 */
void     dynClearPitchInput(void *body, void *actor);                              /* 0x004546A0 */
void     dynSetRollInput(void *body, void *actor, int16_t angle);                  /* 0x00454750 */
void     dynClearRollInput(void *body, void *actor);                               /* 0x00454790 */
void     dynStepRider(void *body, ZActor *actor);                                  /* 0x004547C0 */
/* the limb IK (the BmxRider.ik block, rider.h) */
void     dynIkSetGains(BmxIk *ik, float gain, float gain2);                        /* 0x00455E60 */
void     dynIkSetLooseGains(BmxIk *ik);                                            /* 0x00455E90 */
void     dynIkSetDefaultGains(BmxIk *ik);                                          /* 0x00455EC0 */
void     dynIkReset(void *actor, BmxIk *ik);                                       /* 0x00455EF0 */
void     dynIkMeasure(void *actor, BmxIk *ik);                                     /* 0x00455F10 */
void     dynIkSolve(ZActor *actor, BmxIk *ik);                                     /* 0x00456120 */
void     dynIkSetTarget(BmxIk *ik, const RwV3d *target, const RwV3d *dir, uint32_t limbs); /* 0x00457790 */
/* GHIDRA-SKIP-END */

#endif /* DYN_H */

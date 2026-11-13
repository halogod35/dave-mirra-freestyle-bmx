/*
 * zaxis/Bmx/Src/com/action/physics.c
 * Original: C:\zaxis\Bmx\Src\com\action\physics.c
 * Evidence: [V] path string 0x005579A4 (assert "couldn't find respawn face", line 0x1BF, in
 *           bmxphysResetRider; "orientation are zero", lines 0xD65-0xF9E, in bmxphysUpdatePosition
 *           Roll/Air); [V] function names from its own messages: bmxphysStateInitAir,
 *           bmxphysUpdatePositionRoll, bmxphysUpdatePositionAir. The rest [I]: link order (between
 *           overlay.c and prop.c), the state tables 0x00557898-0x0055793F, the globals
 *           0x0056C128-0x0056C44F.
 * 50 functions, 45590 bytes, VA 0x0041fa20-0x0042de22.
 * Draft. Non-matching C (AD-8). Types: physics.h, rider.h. Description: docs/re/physics.md.
 *
 * The physics.c pass found 15 functions Ghidra had not created (reached only through the state tables or a
 * thunk; decomp/symbols/symbols.csv rows create them), moved bmxphysInit/Shutdown in from the overlay/physics
 * gap, and renamed the skeleton's seed "bmxphysUpdatePositionAir" 0x0042DAD0 (it only shares that
 * function's message) to bmxphysSetSurfaceFromHit; the real bmxphysUpdatePositionAir is 0x00428DF0.
 *
 * The rider is a 7-state machine (physics.h BMXPHYS_*). bmxphysUpdate runs, per active rider and
 * frame: orientation[state], forces[state], wheel spin, position[state] (which detects contacts
 * and changes state through bmxphysSetState = exit[old] + enter[new]) and post[state] (sounds,
 * particles, actor placement). Collision is a segment test against the RenderWare world (the
 * per-sector collision trees of BSP._sd) and the props (zcolSegmentTest); Tag._sd only
 * supplies the tag of the face that was hit and the grind edges.
 *
 * Conventions: ZGE units, y DOWN (g_bmxphysUp = {0, -1, 0}); per-frame velocities; 16-bit binary
 * angles. r->pos is the rider (hips) position, r->spawnPos the bike's (the game.c pass name), r->matrix the
 * rider orientation and r->bikeMatrix the bike's; row 0 ("right") of both is the forward
 * direction, row 1 ("up") the up direction.
 */

#include <math.h>

#include <stddef.h>

#include <string.h>

#include "decomp.h"

#include "zmath.h"

#include "physics.h"

#define PHYSICS_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\physics.c"
#define MAT(p)        ((RwMatrix *)(p))
#define V3(p)         ((RwV3d *)(p))
#define RMAT(r)       MAT((r)->matrix)                 /* rider orientation */
#define BMAT(r)       MAT((r)->bikeMatrix)         /* bike orientation */
#define WFRAME(w)     MAT((w)->frame)                  /* wheel contact frame */
/* zactor pose block (actor + 0x48): wheel / handlebar angles written every frame */
#define ACTOR_POSE(a)         (*(uint8_t **)((uint8_t *)(a) + 0x48))
/* rider stateFlags bits physics uses (rider.c RIDER_FLAG_* names); motion[0].flags bit 3 = MOTION_MIRROR */
#define RIDER_FLAG_ACTIVE       0x00000001u   /* rider slot in play */
#define RIDER_FLAG_IDLE         0x00000004u   /* standing still (rider.c); bmxphysDebugRender draws a marker over the head */
#define RIDER_FLAG_FLIP         0x00000800u   /* air trick flips: flipVel applied in trickMatrix's frame */
#define RIDER_FLAG_CARRY_SPIN   0x00001000u   /* physAirRot is a plain rotation (spin carried from the rail / lip, QP 180) */
#define RIDER_FLAG_FAKIE        0x00002000u   /* air off a quarterpipe face: land back on it */
#define RIDER_FLAG_FORWARD_HOP  0x00004000u   /* hop out of a quarterpipe (rider.c: stick up); physics keeps the rider
                                                   inside unless there is room behind the coping, and clears it */
#define RIDER_FLAG_AIR_8000     0x00008000u   /* cleared by bmxphysStateExitAir [I] */
#define RIDER_FLAG_GRIND_BUTTON 0x00020000u   /* grind/stall button: lock onto an edge when possible */
#define RIDER_FLAG_TURNAROUND   0x00040000u   /* moving backwards relative to the bike (rider.c turns around) */
#define RIDER_FLAG_MIRROR       0x00080000u   /* grind/stall/wall hit from the edge's other side (clips mirrored) */
#define RIDER_FLAG_LAND_ALT     0x00010000u   /* a wheel landed with impact > 20 (rider.c: landing clip 22) */
#define RIDER_FLAG_MANUAL       0x00100000u   /* \ balancing on one wheel (manual / nose manual): */
#define RIDER_FLAG_NOSE_MANUAL  0x00200000u   /* /  the raised wheel is steered, grindBalance = meter */
#define RIDER_FLAG_CAN_MANUAL   0x00400000u   /* one wheel down on a gentle (< 50 deg) slope; cleared every frame */
#define RIDER_FLAG_HIT_WALL     0x00800000u   /* a wheel hit a wall harder than 10; cleared every frame */
#define RIDER_FLAG_TAKEOFF_BACK 0x10000000u   /* InitAir: took off moving backwards */

/* ------------------------------------------------------------------ other modules (current names) */
typedef struct NgFaceHit {             /* entry of zbspColSphere's list (0x1C bytes) */
    NgColFace *face;                   /* +0x00 */
    float      dist;                   /* +0x04 */
    float      point[3];               /* +0x08 closest point */
    uint32_t   onFace;                 /* +0x14 0: the point is on the face */
    uint32_t   unk_18;
} NgFaceHit;

void      zactorGetAbsolutePartPosition(void *actor, int bone, const RwV3d *local, RwV3d *out);  /* bone position */
/* dyn.c: the limb IK on BmxRider.ik and the crash rag doll (bodies are void * here) */
void      dynIkSetTarget(void *ik, const RwV3d *target, const RwV3d *dir, int limbs);   /* 0x00457790 */
void      dynIkReset(void *actor, void *ik);                                        /* 0x00455EF0 */
void      dynIkSolve(void *actor, void *ik);                                        /* 0x00456120 */
void      dynIkSetDefaultGains(void *ik);                                           /* 0x00455EC0 */
void      dynStartRider(void *body, void *actor, RwMatrix *m, RwV3d *vel, int16_t *rot); /* 0x004534A0 */
void      dynStartBike(void *body, void *actor, RwMatrix *m, RwV3d *vel, int16_t *rot);  /* 0x004527C0 */
void      dynStepRider(void *body, void *actor);                                    /* 0x004547C0 */
void      dynStepBike(void *body, void *actor);                                     /* 0x00452A40 */
void      contrlerRumble(uint32_t slot, int motor, int strength);                       /* contrler.c, empty on PC */
uint32_t  bmxsoundEvent(uint32_t cmd, const void *pos, uint32_t id, int volume, int pitch);
uint32_t  riderGetState(BmxRider *r, int level);                                       /* rider state at level 0..2 */
void      riderAddCrashDamage(BmxRider *r, int kind, float impact);                          /* bail */
uint32_t  riderGrindTrick(BmxRider *r);                                                  /* current grind trick */
uint32_t  riderStallTrick(BmxRider *r);                                                  /* current stall trick */
uint32_t  riderPlantTrick(BmxRider *r);                                                  /* current stall/plant trick */
void      psxDrawLine3D(int kind, const float *a, const float *b);                   /* debug line (xyz + rgb) */
void      psxDrawPoly3D(int kind, int count, const float *verts, int arg);           /* debug polygon */
float    *zrollerGetWorldBounds(void);                                                   /* level box min[4], max[4] */
void      zrollerAddDrawCallback(int (*fn)(void));                                        /* add debug render callback */
void      zbspFaceCentroid(const NgColFace *face, RwV3d *centroid);
void      zbspPropSetMode(void *prop, int arg);                                       /* prop collision mode (bmxprop.h NGPROPCOL_SET_*; 0 = off) */
void     *zbspGetPropShape(uint16_t propId);                                           /* prop info (+2 class) */
NgColEdge **zbspFindEdgesNear(const RwV3d *pos, float radius);                          /* edges near pos, 0-terminated */
NgColEdge *zbspGetTaggedEdge(uint32_t ctx, int code, int index, int unused);            /* index-th edge with that code */
int       zbspEdgeOpenEnd(const NgColEdge *e);                                        /* open end: 1 low, -1 high, 0 none */
uint8_t  *zbspColSphere(const RwV3d *pos, float radius, float minCos);              /* faces near pos (+0x3C list) */
void      zbspEdgeClosestPoint(const int16_t *v0, const int16_t *v1, const float *dir, const RwV3d *p, RwV3d *out);
void    **zbspFindPropsByGroup(int classLo, int classHi);                                  /* prop groups of those classes */
NgColFace **zbspFindTaggedFaces(int idLo, int idHi);                                      /* faces with tag id in range */
uint8_t  *zcolGetPropShape(uint16_t propId);                                       /* prop collision kind */
int       zcolSegmentTest(const RwV3d *from, const RwV3d *to, NgColHit *hit);
extern void      *g_motionTable[239];                  /* 0x006962FC */
extern RwV3d      g_zmathOrigin;                       /* 0x006702D8 (0, 0, 0) */
extern uint16_t   g_debugColour;                       /* 0x006951E8 debug polygon argument */

/* ------------------------------------------------------------------ physics constants [V] .data */
static const float g_bmxphysRollDrag[5] = {         /* 0x005577EC wheel speed loss per frame, by surface group */
    0.0015f, 0.0085f, 0.0015f, 0.019f, 0.0018f };

static const float g_bmxphysAirDrag     = 0.001f;   /* 0x00557800 velocity loss per frame in the air */
static const float g_bmxphysSpinDamp    = 0.35f;    /* 0x00557804 roll: yaw rate decay */
static const float g_bmxphysGravity     = 1.58f;    /* 0x00557810 per frame^2 (+y = down) */
static const float g_bmxphysHalf        = 0.5f;     /* 0x00557814 */
static const float g_bmxphysMaxRollSpeed = 60.0f;   /* 0x00557818 roll: no steering above this */
static const float g_bmxphysCrashImpact = 60.0f;    /* 0x0055781C wheel impact speed (x rider stat 4) that bails */
static const float g_bmxphysSteerSpeed  = 12.0f;    /* 0x00557828 full steering below this speed */
static const float g_bmxphysAirSpeedCap = 16.0f;    /* 0x0055782C take-off speed cap off plain ground */
static const float g_bmxphysSpineHop    = 0.85f;    /* 0x00557834 spine hop height factor */
static const float g_bmxphysGrindMinSpeed = 6.0f;   /* 0x00557838 slower along the edge than this = stall */
static const float g_bmxphysWheelProbe  = 40.0f;    /* 0x00557844 side offset of the wall probes */
static const int16_t g_bmxphysWallAngle  = 0x3777;  /* 0x00557848 78 deg: steeper than this vs the bike = wall */
static const int16_t g_bmxphysRampAngle  = 0x238E;  /* 0x0055784A 50 deg: wheel may follow a surface this far */
static const int16_t g_bmxphysLipAngle   = 0x3777;  /* 0x0055784C 78 deg: landing tolerance / air ground test */
static const int16_t g_bmxphysLandAngle  = 0x2000;  /* 0x0055784E 45 deg: grind lean that throws you off */
static const int16_t g_bmxphysAngle90    = 0x4000;  /* 0x00557850 (unused here) */
static const int16_t g_bmxphysQpAngle    = 0x2EEE;  /* 0x00557852 66 deg (unused here) */
static const int16_t g_bmxphysAngle60    = 0x2AAA;  /* 0x00557854 (unused here) */
static const int16_t g_bmxphysQpLimit    = 0x238E;  /* 0x00557856 50 deg: quarterpipe alignment window */
static const int16_t g_bmxphysVertAngle  = 0x271C;  /* 0x00557858 55 deg */
static const int32_t g_bmxphysLockFramesGrind = 12; /* 0x0055785C lock-on look-ahead (grind) */
static const int32_t g_bmxphysLockFramesStall = 24; /* 0x00557860 lock-on look-ahead (stall) */
static const int32_t g_bmxphysAutoRotFrames = 15;   /* 0x00557864 air auto-rotation frames */
static const float g_bmxphysLockRadius  = 150.0f;   /* 0x00557868 lock-on: nearest-edge radius */
static const float g_bmxphysEdgeRadius  = 30.0f;    /* 0x00557870 edge search radius in the air */
static const float g_bmxphysSuspGravity = 30.0f;    /* 0x00557878 */
static const float g_bmxphysYawDamp     = 0.3f;     /* 0x0055787C */
static const float g_bmxphysOrientBlend = 0.15f;    /* 0x00557880 per-frame blend towards a target orientation */
static const float g_bmxphysMaxLeanDeg  = 45.0f;    /* 0x00557884 */
static const RwV3d g_bmxphysUp = { 0.0f, -1.0f, 0.0f }; /* 0x00557888 */
/* state tables [V] 0x00557898.. (7 entries each; 0 = nothing to do) */
const BmxPhysTransFn g_bmxphysExit[BMXPHYS_STATES] = {          /* 0x00557898 */
    0, bmxphysStateExitAir, bmxphysStateExitCrash, bmxphysStateExitAir, bmxphysStateExitGrind,
    bmxphysStateExitAir, 0 };

const BmxPhysTransFn g_bmxphysEnter[BMXPHYS_STATES] = {         /* 0x005578B4 */
    bmxphysStateInitRoll, bmxphysStateInitAir, bmxphysStateInitCrash, bmxphysStateInitLockOn,
    bmxphysStateInitGrind, bmxphysStateInitLockOn, bmxphysStateInitStall };

const BmxPhysStepFn g_bmxphysOrientation[BMXPHYS_STATES] = {    /* 0x005578D0 */
    bmxphysUpdateOrientationRoll, bmxphysUpdateOrientationAir, 0, bmxphysUpdateOrientationLockOn,
    bmxphysUpdateOrientationGrind, bmxphysUpdateOrientationLockOn, bmxphysUpdateOrientationGrind };

const BmxPhysStepFn g_bmxphysForces[BMXPHYS_STATES] = {         /* 0x005578EC */
    bmxphysUpdateForcesRoll, bmxphysUpdateForcesAir, 0, bmxphysUpdateForcesLockOn,
    bmxphysUpdateForcesGrind, bmxphysUpdateForcesLockOn, 0 };

const BmxPhysStepFn g_bmxphysPosition[BMXPHYS_STATES] = {       /* 0x00557908 */
    bmxphysUpdatePositionRoll, bmxphysUpdatePositionAir, bmxphysUpdatePositionCrash,
    bmxphysUpdatePositionLockOn, bmxphysUpdatePositionGrind, bmxphysUpdatePositionLockOn, 0 };

const BmxPhysStepFn g_bmxphysPost[BMXPHYS_STATES] = {           /* 0x00557924 */
    bmxphysPostRoll, bmxphysPostAir, 0, bmxphysPostAir, bmxphysPostGrind, bmxphysPostAir,
    bmxphysPostGrind };

/* ------------------------------------------------------------------ globals 0x0056C128.. */
static NgColHit   g_bmxphysHit;            /* 0x0056C128 last world probe */
static NgColEdge  g_bmxphysTaggedEdge;     /* 0x0056C200 bmxphysFindTaggedEdge result */
static NgColEdge  g_bmxphysLockAligned;    /* 0x0056C238 lock-on: best-aligned edge */
static NgColEdge  g_bmxphysNearestEdge;    /* 0x0056C2C0 bmxphysFindNearestEdge result */
static NgColEdge  g_bmxphysEdgeScratch;    /* 0x0056C2F8 */
static NgColEdge  g_bmxphysLockNearest;    /* 0x0056C330 lock-on: nearest edge */
static NgColFace  g_bmxphysLockFace;       /* 0x0056C3B8 lock-on: wall / stall face */
static int        g_bmxphysDebugDraw;      /* 0x0056C444 draw wheel probes and contacts */
static void     **g_bmxphysDebugEdges;     /* 0x0056C448 lock-on candidates (debug draw), 0-terminated */
static uint32_t   g_bmxphysDebugFrame;     /* 0x0056C44C */

/* ------------------------------------------------------------------ helpers (inlined in the exe) */
static void vecSet(float *v, float x, float y, float z) { v[0] = x; v[1] = y; v[2] = z; }

static void vecCopy(float *d, const float *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

static int  iabs(int x) { return x < 0 ? -x : x; }

static int  clampSoundVolume(int v) { return v < 0 ? 0 : (v > 0x7F ? 0x7F : v); }

/* squared distance of two points after truncating each axis difference to int (the exe's __ftol) */
static uint32_t idist2(const float *a, const float *b)
{
    int dx = (int)(a[0] - b[0]), dy = (int)(a[1] - b[1]), dz = (int)(a[2] - b[2]);
    return (uint32_t)(dx * dx + dy * dy + dz * dz);
}

/* rider matrix rows are blended towards target rows (right, up) by g_bmxphysOrientBlend */
static void blendRow(RwV3d *row, const RwV3d *target)
{
    row->x += (target->x - row->x) * g_bmxphysOrientBlend;
    row->y += (target->y - row->y) * g_bmxphysOrientBlend;
    row->z += (target->z - row->z) * g_bmxphysOrientBlend;
}

static int16_t ftos(double x) { return (int16_t)(int32_t)x; }

static uint32_t surfaceRecordSize(int type)
{
    switch (type) {
    case 0: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 13: case 14:
        return sizeof(NgColFace);      /* 0x88 */
    case 1: case 2: case 3: case 9:
        return sizeof(NgColEdge);      /* 0x38 */
    case 12:
        return 500;                    /* prop group */
    }
    return 0;
}

/* prop group record (500 bytes; zbspFindPropsByGroup list, BMXSURF_PROP surfaces) [V] offsets */
#define PG_ROT(g)    ((int16_t *)((uint8_t *)(g) + 0x04))     /* euler x, y, z */
#define PG_POS(g)    ((int16_t *)((uint8_t *)(g) + 0x0A))     /* position (integer ZGE units) */
#define PG_PROP(g)   (*(uint8_t **)((uint8_t *)(g) + 0x1EC))  /* game prop (+0x3C facing normal) */
#define PG_MODEL(g)  (*(uint8_t **)((uint8_t *)(g) + 0x1F0))  /* model (+0x1C -> +0x14 quaternion) */

static void soundForMaterial(const NgColHit *hit, uint32_t *sound)
{
    /* collision sound kind from the material (world) or the prop class: 1/3 metal (7), 2 wood (2) */
    uint8_t *info;
    uint8_t cls;

    if (hit->type == 1) {
        if (hit->material) {
            switch (*hit->material & 0xF) {
            case 0:         *sound = 0; break;
            case 1: case 3: *sound = 7; break;
            case 2:         *sound = 2; break;
            }
        }
    } else if (hit->type == 2) {
        info = (uint8_t *)zbspGetPropShape(hit->propId);
        if (zcolGetPropShape(hit->propId)) {
            cls = info[2];
            if (cls == 0) *sound = 0;
            else if (cls == 1) *sound = 7;
            else if (cls == 2) *sound = 2;
        }
    }
}

/* is n a floor the wheel may roll onto (not a side wall, not too steep against the bike), checked
 * the way the roll integrator does after the plain ground probe missed */

static int rollOntoSurface(BmxRider *r, const RwV3d *n, const float *wheelUp, int rear)
{
    if (zmathVecDot(V3(r->vel), n) < 0.0f)
        return 1;
    if ((zmathVecDot(V3(r->vel), &BMAT(r)->right) >= 0.0f) == rear)
        return zmathAcos(zmathVecDot(V3(wheelUp), n)) < g_bmxphysRampAngle;
    return zmathAcos(-n->y) < g_bmxphysWallAngle && zmathAcos(zmathVecDot(V3(wheelUp), n)) < g_bmxphysRampAngle;
}

extern uint8_t g_trickFlags[28];              /* 0x00558FB4 (game.c) bits 0-2: landing allowed mid-trick */
extern uint32_t g_modLimbMask[38];          /* 0x0055907C per trick modifier: limb / bar actions (bits below) */
static int g_bmxphysHipsFollow;              /* 0x0056C440 debug: keep the hip offset in the air */

/* point the IK limb at target, with the rider forearm bone's x axis as the reach direction */
static void ikReach(BmxRider *r, const RwV3d *target, int forearmBone, int limb)
{
    RwV3d local, a, b, dir;

    zmathVecZero(&local);
    local.x = 1.0f;
    zactorGetAbsolutePartPosition(r->riderActor, forearmBone, &local, &a);
    local.x = 0.0f;
    zactorGetAbsolutePartPosition(r->riderActor, forearmBone, &local, &b);
    zmathVecSub(&dir, &a, &b);
    dynIkSetTarget(&r->ik, target, &dir, limb);
}

/* handlebar bone angles: yaw, then the 15 deg fork rake */
static void setBars(BmxRider *r, int yaw)
{
    RwMatrix to, from;
    int16_t euler[4];

    zmathMatrixIdentity(&to);
    zmathMatrixRotateY(&to, &to, yaw);
    zmathMatrixRotateX(&to, &to, -0xAAA);
    zmathMatrixIdentity(&from);
    bmxphysMatrixDeltaAngles(&from, &to, 1, euler);
    zactorSetChannel(r->bikeActor, 1, euler);
}

/* NgIFpsx debug primitive vertex (0x14 bytes) */
typedef struct DbgVert {
    float   p[3];
    float   unk_0c;
    uint8_t rgb[4];
} DbgVert;

static void dbgLine(const float *a, const float *b, uint8_t cr, uint8_t cg, uint8_t cb)
{
    DbgVert v[2];

    vecCopy(v[0].p, a);
    vecCopy(v[1].p, b);
    v[0].rgb[0] = v[1].rgb[0] = cr;
    v[0].rgb[1] = v[1].rgb[1] = cg;
    v[0].rgb[2] = v[1].rgb[2] = cb;
    psxDrawLine3D(0xC, (const float *)&v[0], (const float *)&v[1]);
}

/* ================================================================== functions (VA order) */

/* VA 0x0041fa20, 12 bytes, [I] row (was gap_overlay__physics.c), status draft */
void bmxphysInit(void)
{
    zrollerAddDrawCallback(bmxphysDebugRender);    /* called from action.c's game start */
}

/* VA 0x0041fa40, 1 bytes, [I] row (was gap_overlay__physics.c), status draft */
void bmxphysShutdown(void)
{
}

/* VA 0x0041fa60, 383 bytes, [I] row, status draft */
void bmxphysSetLimbTargets(BmxRider *r)
{
    /* the rider's hands follow the handlebar grips (bike bones 7 and 8), oriented like the rider's
     * forearms (bones 3 and 6); the IK block is r->ik (dyn.c dynIk*) */
    RwV3d local, grip, a, b, dir;

    zmathVecZero(&local);
    local.z = 0.0f;
    local.y = 22.0f;
    zactorGetAbsolutePartPosition(r->bikeActor, 7, &local, &grip);
    zmathVecZero(&local);
    local.x = 1.0f;
    zactorGetAbsolutePartPosition(r->riderActor, 3, &local, &a);
    local.x = 0.0f;
    zactorGetAbsolutePartPosition(r->riderActor, 3, &local, &b);
    zmathVecSub(&dir, &a, &b);
    dynIkSetTarget(&r->ik, &grip, &dir, 1);

    zmathVecZero(&local);
    local.z = 0.0f;
    local.y = 22.0f;
    zactorGetAbsolutePartPosition(r->bikeActor, 8, &local, &grip);
    zmathVecZero(&local);
    local.x = 1.0f;
    zactorGetAbsolutePartPosition(r->riderActor, 6, &local, &a);
    local.x = 0.0f;
    zactorGetAbsolutePartPosition(r->riderActor, 6, &local, &b);
    zmathVecSub(&dir, &a, &b);
    dynIkSetTarget(&r->ik, &grip, &dir, 2);
}

/* VA 0x0041fc40, 358 bytes, [I] row, status draft */
void bmxphysUpdate(void)
{
    uint32_t p, players;
    BmxRider *r;
    int before, state, i;

    g_bmxphysDebugEdges = 0;
    for (p = 0, r = g_riders;; p++, r++) {
        players = g_numPlayers ? g_numPlayers : 1;
        if (p >= players)
            break;
        if (!(r->stateFlags & RIDER_FLAG_ACTIVE))
            continue;
        zactorSetFlag2(r->riderActor);
        zactorClearFlag2(r->bikeActor);
        if (g_bmxphysOrientation[bmxphysGetState(r)])
            g_bmxphysOrientation[bmxphysGetState(r)](r);
        if (g_bmxphysForces[bmxphysGetState(r)])
            g_bmxphysForces[bmxphysGetState(r)](r);
        for (i = 0; i < 2; i++)
            r->wheel[i].angle += (int16_t)r->wheel[i].spin;
        r->stateFlags &= ~(RIDER_FLAG_CAN_MANUAL | RIDER_FLAG_HIT_WALL);

        before = bmxphysGetState(r);
        state = bmxphysGetState(r);
        if (g_bmxphysPosition[state])
            g_bmxphysPosition[state](r);
        if (bmxphysGetState(r) == BMXPHYS_CRASH && before != BMXPHYS_CRASH)
            g_bmxphysPosition[BMXPHYS_CRASH](r);     /* the crash starts moving this frame */
        if (g_bmxphysPost[bmxphysGetState(r)])
            g_bmxphysPost[bmxphysGetState(r)](r);

        /* wheel angles into the bike pose (and the rider's, with cheat bit 3 in multiplayer) */
        *(int16_t *)(ACTOR_POSE(r->bikeActor) + 0x128) = r->wheel[0].angle;
        *(int16_t *)(ACTOR_POSE(r->bikeActor) + 0x122) = r->wheel[1].angle;
        if ((g_saveBlockA.cheats_active & 8) && g_numPlayers)
            *(int16_t *)(ACTOR_POSE(r->riderActor) + 0x112) = r->wheel[0].angle;
    }
}

/* VA 0x0041fe10, 520 bytes, [V] direct, status draft */
void bmxphysResetRider(BmxRider *r)
{
    /* put the rider on the nearest respawn face, standing, with no motion */
    NgColFace *face;
    RwMatrix *m = RMAT(r);
    RwV3d up;
    float h;
    int i;

    zmathVecZero(V3(r->physHipOffset));
    zmathVecZero(V3(r->rootMotion));
    zmathVecZero(V3(r->vel));
    zmathVecZero(V3(r->physSlopeAccel));
    r->frameDist = 0.0f;
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    r->airLean = 0;
    r->crash = 0;
    r->pedalForce = 0.0f;
    r->wheelDrag = 0.0f;
    for (i = 0; i < 2; i++) {
        zmathVecZero(V3(r->wheel[i].vel));
        r->wheel[i].speed = 0.0f;
        r->wheel[i].contact = 0;
        r->wheel[i].angle = 0;
        r->wheel[i].spin = 0;
    }
    face = bmxphysFindRespawnFace(V3(r->pos), 0);
    if (!face)
        zAssertFail("couldn't find respawn face", PHYSICS_SRC, 0x1bf, 0);
    r->surface = BMXSURF_GROUND;
    r->surfaceObj = face;
    bmxphysCopySurface(r);
    zbspFaceCentroid(face, V3(r->pos));               /* stand on the face's centroid */

    up.x = face->normal[0];
    up.y = face->normal[1];
    up.z = face->normal[2];
    if (fabs(zmathVecDot(&m->right, &up)) > 0.99f)
        m->right = m->up;                              /* forward would be degenerate: use the old up */
    m->up = up;
    zmathTriangleNormal(&m->at, &m->right, &g_zmathOrigin, &m->up);
    zmathVecNormalize(&m->at, &m->at);
    zmathTriangleNormal(&m->right, &m->up, &g_zmathOrigin, &m->at);
    zmathVecZero(&m->pos);
    zactorSetMatrix(r->riderActor, m);

    h = (r->wheel[1].radius + r->wheel[0].radius) * 0.5f;   /* bike sits a wheel radius above */
    r->spawnPos[0] = h * m->up.x + r->pos[0];
    r->spawnPos[1] = h * m->up.y + r->pos[1];
    r->spawnPos[2] = h * m->up.z + r->pos[2];
    zmathMatrixCopy(BMAT(r), m);
    r->physState = BMXPHYS_ROLL;
}

/* VA 0x004200b0, 577 bytes, [I] row, status draft */
NgColFace *bmxphysFindRespawnFace(const RwV3d *pos, uint32_t flags)
{
    /* flags bit 0: skip the search around pos; bit 1: only faces within -200..+400 of pos.y.
     * 1) spiral out from pos (24 probes, 60 degrees apart, radius 0, 100, 110, 121, ...) for a
     *    walkable face: not a prop, slope < 30 deg, tag flag 0x10 clear, class 0 or 4..8 (no
     *    quarterpipe/spine/vert), not material 6 (hazard), and zbspColSphere finds no
     *    tagged face within 50 of the probe point (facing within 60 deg);
     * 2) else the tagged respawn face (id 0xFA1..0xFA4, facing up) nearest in x/z. */
    NgColFace *found = 0, **list, *f;
    BmxProbe probe;
    RwV3d p, c;
    float radius;
    int angle, tries;
    uint32_t best, d;
    uint8_t *near;
    uint16_t cls;

    if (!(flags & 1)) {
        radius = 0.0f;
        angle = 0;
        tries = 25;
        do {
            if (--tries == 0) {
                g_pfnDebugPrintf("warning: no respawn faces found by distance check\n");
                break;
            }
            vecSet(&p.x, radius, 0.0f, 0.0f);
            zmathVecRotateY(&p, &p, angle);
            zmathVecAdd(&p, &p, pos);
            if (bmxphysProbeGround(&p, &probe) && !(probe.flags & 1) &&
                (!(flags & 2) || (probe.pos[1] - pos->y < 400.0f && probe.pos[1] - pos->y >= -200.0f)) &&
                zmathAcos(-probe.normal[1]) < 0x1556 && !(probe.face->flags & 0x10)) {
                cls = probe.face->code >> 12;
                if ((cls == 0 || (cls > 3 && cls < 9)) && (*probe.material & 0xF) != 6) {
                    near = zbspColSphere(&p, 50.0f, zmathCos(0x2AAA));
                    if (!near || !*(void ***)(near + 0x3C) || !**(void ***)(near + 0x3C))
                        found = probe.face;
                }
            }
            angle += 0x2AAA;
            radius = (radius == 0.0f) ? radius + 100.0f : radius * 1.1f;
        } while (!found);
        if (found)
            return found;
    }

    best = 0xFFFFFFFFu;
    list = zbspFindTaggedFaces(0xFA1, 0xFA4);
    if (!list) {
        g_pfnDebugPrintf("warning: no tagged respawn faces found in level\n");
        return 0;
    }
    if (!*list)
        return 0;
    for (; (f = *list) != 0; list++) {
        int dx, dz;
        zbspFaceCentroid(f, &c);
        dx = (int)(c.x - pos->x);
        dz = (int)(c.z - pos->z);
        d = (uint32_t)(dz * dz + dx * dx);
        if (d < best && f->normal[1] < 0.0f) {      /* facing up */
            best = d;
            found = f;
        }
    }
    return found;
}

/* VA 0x004203a0, 1789 bytes, [I] row (new function; bmxphysInit registers it), status draft */
int bmxphysDebugRender(void)
{
    /* with g_bmxphysDebugDraw: each wheel's contact frame (or a radius line when in the air), the
     * current surface (edge in blue, face outline) and the lock-on candidates; with stateFlags 4
     * seven rising, spinning squares above the rider's head (a marker) */
    uint32_t p, players, i, j, k, ph, side;
    BmxRider *r;
    BmxWheel *w;
    RwV3d hub, t, radial, corner;
    float a[3], b[3], size, cx, cy, cz;
    DbgVert v[4];
    NgColEdge **e;

    for (p = 0, r = g_riders;; p++, r++) {
        players = g_numPlayers ? g_numPlayers : 1;
        if (p >= players)
            break;
        if (!(r->stateFlags & RIDER_FLAG_ACTIVE))
            continue;
        if (g_bmxphysDebugDraw) {
            for (i = 0; i < 2; i++) {
                w = &r->wheel[i];
                zmathTransformPoint(&t, BMAT(r), V3(w->pos));
                zmathVecAdd(&hub, &t, V3(r->spawnPos));
                if (w->contact == 2) {
                    vecSet(b, hub.x - BMAT(r)->up.x * w->radius, hub.y - BMAT(r)->up.y * w->radius,
                           hub.z - BMAT(r)->up.z * w->radius);
                    dbgLine(&hub.x, b, 0xB0, 0x50, 0x10);
                } else {
                    for (k = 0; k < 3; k++) {
                        const float *row = &w->frame[k * 4];
                        vecSet(b, row[0] * 30.0f + hub.x, row[1] * 30.0f + hub.y, row[2] * 30.0f + hub.z);
                        dbgLine(&hub.x, b, 0xB0, 0x50, 0x10);
                    }
                }
            }
            switch (r->surface) {
            case 1: case 2: case 3: case 9: {
                NgColEdge *edge = (NgColEdge *)r->surfaceObj;
                vecSet(a, edge->v0[0], edge->v0[1], edge->v0[2]);
                vecSet(b, edge->v1[0], edge->v1[1], edge->v1[2]);
                dbgLine(a, b, 0x20, 0x40, 0xC0);
                break;
            }
            case 10: case 11: {
                uint8_t *face = (uint8_t *)r->surfaceObj;
                uint32_t n = (face[0x15] & 1) ? 4 : 3;              /* quad flag */
                int16_t **vp = (int16_t **)(face + 0x1C);           /* vertex pointers (lock-on faces) */
                for (k = 0; k < n; k++) {
                    vecSet(v[k].p, vp[k][0], vp[k][1], vp[k][2]);
                    v[k].rgb[0] = 0x20; v[k].rgb[1] = 0x80; v[k].rgb[2] = 0xC0;
                }
                psxDrawPoly3D(10, (int)n, (const float *)v, 0);
                break;
            }
            }
            for (e = (NgColEdge **)g_bmxphysDebugEdges; e && *e; e++) {
                vecSet(a, (*e)->v0[0], (*e)->v0[1], (*e)->v0[2]);
                vecSet(b, (*e)->v1[0], (*e)->v1[1], (*e)->v1[2]);
                dbgLine(a, b, 0x20, 0x40, 0xC0);
            }
        }
        if (r->stateFlags & RIDER_FLAG_IDLE) {
            for (j = 0; j < 7; j++) {
                ph = (j * 0x80 / 7 + g_bmxphysDebugFrame) & 0x7F;              /* 0..127, rising */
                cx = RMAT(r)->up.x * 75.0f + r->physHipOffset[0] + r->pos[0];
                cy = ((RMAT(r)->up.y * 75.0f - (float)ph * 0.703125f) + r->physHipOffset[1] + r->pos[1]) - 5.0f;
                cz = RMAT(r)->up.z * 75.0f + r->physHipOffset[2] + r->pos[2];
                vecSet(&radial.x, (float)(int16_t)(int32_t)(zmathSin(ph << 11) * 0.15625f * ((ph >> 1) + 0x40)), 0.0f, 0.0f);
                zmathVecRotateY(&radial, &radial, (int)(j * 0x2492));
                size = (float)ph * 0.0703125f + 1.0f;
                for (side = 0; side < 2; side++) {
                    for (k = 0; k < 4; k++) {
                        corner.x = ((k + 1) & 2) ? size : -size;
                        if (side)
                            corner.x = -corner.x;
                        corner.y = (k & 2) ? size : -size;
                        corner.z = 0.0f;
                        zmathVecRotateY(&corner, &corner, (int)(ph * -0x200));
                        vecSet(v[k].p, corner.x + radial.x + cx, corner.y + radial.y + cy, corner.z + radial.z + cz);
                        v[k].rgb[0] = v[k].rgb[1] = v[k].rgb[2] = (uint8_t)((0x80 - ph) >> 1);
                    }
                    psxDrawPoly3D(0x1A, 4, (const float *)v, g_debugColour);
                }
            }
        }
    }
    g_bmxphysDebugFrame++;
    return 0;
}

/* VA 0x00420c90, 13 bytes, [I] row, status draft */
int bmxphysGetState(const BmxRider *r)
{
    return r->physState;
}

/* VA 0x00420cb0, 95 bytes, [I] row, status draft */
int bmxphysSetState(BmxRider *r, int state)
{
    int from = bmxphysGetState(r);

    if (from != state) {
        if (from == BMXPHYS_NONE)
            g_bmxphysExit[BMXPHYS_CRASH](r, BMXPHYS_NONE, state);   /* [V] exactly so: a no-op exit */
        else if (g_bmxphysExit[from])
            g_bmxphysExit[from](r, from, state);
        /* the enter function may redirect (e.g. a lock-on that finds nothing keeps the old state) */
        r->physState = (uint8_t)g_bmxphysEnter[state](r, from, state);
    }
    return r->physState;
}

/* VA 0x00420d30, 42 bytes, [I] row, status draft */
void bmxphysAddVelocity(BmxRider *r, const RwV3d *dv)
{
    zmathVecAdd(V3(r->vel), V3(r->vel), dv);
    r->frameDist = zmathVecLength(V3(r->vel));
}

/* VA 0x00420d70, 312 bytes, [I] row, status draft */
int bmxphysProbeGround(const RwV3d *pos, BmxProbe *out)
{
    /* vertical segment through the whole level at (x, z); succeeds on an upward-facing hit */
    RwV3d top, bottom;
    float *box;

    out->flags = 0;
    bottom = *pos;
    top = *pos;
    box = zrollerGetWorldBounds();
    top.y = box[1] + 1.0f;            /* level minimum y (= the highest point, y is down) */
    bottom.y = box[5] - 1.0f;         /* level maximum y */
    out->face = 0;
    out->prop = 0;
    out->material = 0;
    if (zcolSegmentTest(&bottom, &top, &g_bmxphysHit) && g_bmxphysHit.normal[1] < 0.0f) {
        vecCopy(out->pos, g_bmxphysHit.pos);
        vecCopy(out->normal, g_bmxphysHit.normal);
        out->d = g_bmxphysHit.d;
        if ((g_bmxphysHit.type & 1) && g_bmxphysHit.face) {
            out->face = g_bmxphysHit.face;
            out->material = g_bmxphysHit.material;
            return 1;
        }
        if ((g_bmxphysHit.type & 2) && g_bmxphysHit.propObj) {
            out->flags |= 1;
            out->prop = g_bmxphysHit.propObj;
            return 1;
        }
    }
    return 0;
}

/* VA 0x00420f00, 32 bytes, [I] row (new function, exit table), status draft */
int bmxphysStateExitGrind(BmxRider *r, int from, int to)
{
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    return from;
}

/* VA 0x00420f30, 281 bytes, [I] row, status draft */
int bmxphysStateExitAir(BmxRider *r, int from, int to)
{
    /* leaving air / lock-on: re-attach the rider to the bike, remember the landing contact */
    RwV3d t;
    float *root;

    r->stateFlags &= ~RIDER_FLAG_AIR_8000;
    if (to == BMXPHYS_ROLL) {
        root = &zactorGetBikeOffset(r->riderActor)->pos.x;
        vecCopy(r->physHipOffset, root);
        zmathTransformPoint(&t, BMAT(r), V3(r->physHipOffset));
        vecCopy(r->physHipOffset, &t.x);
        zmathVecNegate(V3(r->physHipOffset), V3(r->physHipOffset));
        vecCopy(r->pos, r->spawnPos);
    } else if (to == BMXPHYS_CRASH) {
        zactorSetHeight(r->riderActor, 0.0f);
        zmathVecZero(V3(r->physHipOffset));
    }
    vecCopy(r->contact[1].pos, r->pos);
    r->contact[1].obj = r->surfaceObj;
    r->contact[1].type = r->surface;
    bmxphysCopyContactSurface(&r->contact[1]);
    return from;
}

/* VA 0x004210a0, 5 bytes, [I] row, status draft */
int bmxphysStateExitCrash(BmxRider *r, int from, int to)
{
    return from;
}

/* VA 0x004210c0, 145 bytes, [I] row (new function, enter table), status draft */
int bmxphysStateInitRoll(BmxRider *r, int from, int to)
{
    if (from == BMXPHYS_GRIND) {
        zmathMatrixCopy(BMAT(r), RMAT(r));
        if (zmathVecDot(&RMAT(r)->right, V3(r->vel)) >= 0.0f)
            r->stateFlags &= ~RIDER_FLAG_TURNAROUND;
        else
            r->stateFlags |= RIDER_FLAG_TURNAROUND;
    }
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    r->wallHeight = 0.0f;
    dynIkReset(r->riderActor, &r->ik);
    dynIkSetDefaultGains(&r->ik);
    return to;
}

/* VA 0x00421190, 4611 bytes, [I] row (new function, enter table [3] and [5]), status draft */
int bmxphysStateInitLockOn(BmxRider *r, int from, int to)
{
    /* Grind / stall button in the air: fly the predicted parabola (velocity + gravity per frame)
     * for up to 12 (grind) or 24 (stall) frames, widening the search radius each frame, and pick
     * a target: an edge (BMXSURF_EDGE; nearest, or the best-aligned one when they are within 50
     * units), a steep wall face for a fast plant / wall tap (BMXSURF_WALL), or a prop of class
     * 10..13 (BMXSURF_PROP). Then aim: physTimer frames of physLockStep moves to physLockPos.
     * No target: stay in `from`. */
    uint32_t trick, frames, maxFrames, i, d2, nearestD2, r2;
    int kind, open, a, b, flipped, k;
    float step, radius, bestY, horiz, disc, s;
    RwV3d probe, stepVel, c, rel, endPt;
    RwV3d nearestPt, alignedPt, *targetUp = 0;
    NgColEdge **edges, *e, *use, *nearest = 0, *aligned = 0;
    void **groups = 0, *propTarget = 0;
    NgColFace *faceTarget = 0;
    uint8_t *faces;
    NgFaceHit *h;
    int16_t bestAngle = 0x7FFF, ang, euler[3];
    RwMatrix m, rot;
    Zquat q;
    float propNormal[3];
    int dx, dy, dz;

    if (to == BMXPHYS_GRIND_LOCK) {
        trick = riderGrindTrick(r);
        kind = BMXSURF_EDGE;
    } else {
        trick = riderPlantTrick(r);
        kind = (trick == 0x2A || trick == 0x2B) ? BMXSURF_WALL : BMXSURF_EDGE;   /* fast plant / wall tap */
    }
    k = (int)g_bmxphysLockRadius;
    nearestD2 = (uint32_t)(k * k);
    probe = *V3(r->spawnPos);
    stepVel = *V3(r->vel);
    bestY = 32767.0f;
    step = (kind == BMXSURF_WALL) ? 30.0f : 10.0f;
    radius = (bmxphysGetState(r) == BMXPHYS_AIR) ? g_bmxphysEdgeRadius : 50.0f;
    maxFrames = (to == BMXPHYS_GRIND_LOCK) ? (uint32_t)g_bmxphysLockFramesGrind : (uint32_t)g_bmxphysLockFramesStall;
    if (kind == BMXSURF_EDGE)
        edges = zbspFindEdgesNear(V3(r->spawnPos), 400.0f);
    else
        edges = (NgColEdge **)(groups = zbspFindPropsByGroup(10, 13));
    g_bmxphysDebugEdges = (void **)edges;

    for (i = 0; i < maxFrames && !propTarget; i++) {
        k = (int)radius;
        r2 = (uint32_t)(k * k);
        if (kind == BMXSURF_EDGE) {
            for (; edges && (e = *edges) != 0; edges++) {
                bmxphysClosestPointOnEdge(&probe, e, &c);
                rel.x = c.x - r->spawnPos[0];
                rel.y = c.y - r->spawnPos[1];
                rel.z = c.z - r->spawnPos[2];
                if (zmathVecDot(&rel, V3(r->vel)) < 0.0f)
                    continue;                                   /* behind */
                d2 = idist2(&c.x, &probe.x);
                if (d2 > r2)
                    continue;
                use = e;
                if ((int)d2 < (int)nearestD2) {
                    use = &g_bmxphysLockNearest;
                    zstringMemCopy(&g_bmxphysLockNearest, e, sizeof(NgColEdge));
                    nearest = &g_bmxphysLockNearest;
                    nearestPt = c;
                    nearestD2 = d2;
                }
                ang = zmathAcos((float)fabs(zmathVecDot(V3(use->dir), &RMAT(r)->right)));
                if (ang < bestAngle) {
                    zstringMemCopy(&g_bmxphysLockAligned, use, sizeof(NgColEdge));
                    aligned = &g_bmxphysLockAligned;
                    alignedPt = c;
                    bestAngle = ang;
                }
            }
            edges = (NgColEdge **)g_bmxphysDebugEdges;
        } else if (kind > BMXSURF_EDGE && kind < BMXSURF_PROP && (i & 3) == 0) {
            void **g;
            for (g = groups; g && *g; g++) {
                int16_t *gp = PG_POS(*g);
                dx = gp[0] - (int)probe.x;
                dy = gp[1] - (int)probe.y;
                dz = gp[2] - (int)probe.z;
                d2 = (uint32_t)(dz * dz + dy * dy + dx * dx);
                if ((int)d2 > (int)r2)
                    continue;
                rel.x = gp[0] - probe.x;
                rel.y = gp[1] - probe.y;
                rel.z = gp[2] - probe.z;
                if (!PG_PROP(*g) || zmathVecDot(V3(PG_PROP(*g) + 0x3C), &rel) > 0.0f)
                    continue;
                rel.x = (float)(int16_t)dx;                       /* horizontal, must be ahead */
                rel.z = (float)(int16_t)dz;
                rel.y = 0.0f;
                if (zmathVecDot(&rel, &stepVel) < 0.0f)
                    continue;
                /* a prop facing us: its horizontal "at" is the target normal */
                vecSet(&nearestPt.x, gp[0], gp[1], gp[2]);
                nearestD2 = d2;
                bestY = nearestPt.y;
                propTarget = *g;
                zmathMatrixIdentity(&rot);
                euler[0] = PG_ROT(*g)[0];
                euler[1] = (int16_t)-PG_ROT(*g)[1];
                euler[2] = PG_ROT(*g)[2];
                q.x = *(float *)(*(uint8_t **)(PG_MODEL(*g) + 0x1C) + 0x14);
                q.y = -*(float *)(*(uint8_t **)(PG_MODEL(*g) + 0x1C) + 0x18);
                q.z = *(float *)(*(uint8_t **)(PG_MODEL(*g) + 0x1C) + 0x1C);
                q.w = *(float *)(*(uint8_t **)(PG_MODEL(*g) + 0x1C) + 0x20);
                zquatToMatrix(&rot, &q);
                zmathMatrixRotateXYZ(&rot, &rot, euler);
                zmathMatrixRotateXYZ(&rot, &rot, PG_ROT(*g));
                rot.at.y = 0.0f;
                zmathVecNormalize(&rot.at, &rot.at);
                vecSet(propNormal, -rot.at.x, -rot.at.y, -rot.at.z);
                targetUp = V3(propNormal);
                kind = BMXSURF_PROP;
                break;
            }
            if (propTarget)
                goto next_step;
            faces = zbspColSphere(&probe, radius, 1.0f);
            if (faces) {
                for (h = *(NgFaceHit **)(faces + 0x3C); h->face; h++) {
                    NgColFace *f = h->face;
                    if (kind == BMXSURF_FACE10) {
                        if (zmathAcos((float)fabs(f->normal[1])) >= 0x18E3)
                            continue;
                    } else if (kind == BMXSURF_WALL) {
                        /* steep (> 75 deg), facing us, ahead, not tag-flagged 0x10 / 0x20 */
                        if (h->onFace != 0 || (f->flags & 0x10) || zmathAcos((float)fabs(f->normal[1])) <= 0x3555 ||
                            zmathVecDot(V3(f->normal), &stepVel) > 0.0f || h->dist <= 0.0f || (f->flags & 0x20))
                            continue;
                    } else {
                        continue;
                    }
                    d2 = idist2(h->point, &probe.x);
                    if ((int)d2 < (int)nearestD2 && h->point[1] < bestY) {
                        zstringMemCopy(&g_bmxphysLockFace, f, sizeof(NgColFace));
                        faceTarget = &g_bmxphysLockFace;
                        targetUp = V3(g_bmxphysLockFace.normal);
                        vecCopy(&nearestPt.x, h->point);
                        nearestD2 = d2;
                        bestY = nearestPt.y;
                    }
                }
            }
        }
next_step:
        stepVel.y += g_bmxphysGravity;
        zmathVecAdd(&probe, &probe, &stepVel);
        radius += step;
    }

    /* choose */
    if (kind == BMXSURF_EDGE && aligned) {
        if (nearest) {
            /* prefer the best-aligned edge when it is about as close as the nearest one */
            if (iabs((int)idist2(&nearestPt.x, r->spawnPos) - (int)idist2(&alignedPt.x, r->spawnPos)) < 2500) {
                nearest = aligned;
                nearestPt = alignedPt;
            }
        } else if (!faceTarget) {
            return from;
        }
    } else if (!nearest && !faceTarget && !propTarget) {
        return from;
    }

    /* frames to reach the target: horizontally at the current speed, or along the parabola */
    dx = (int)(nearestPt.x - r->spawnPos[0]);
    dy = (int)(nearestPt.y - r->spawnPos[1]);
    dz = (int)(nearestPt.z - r->spawnPos[2]);
    rel.x = r->vel[0];
    rel.y = 0.0f;
    rel.z = r->vel[2];
    horiz = zmathVecLength(&rel);
    if (fabs(r->vel[1]) <= horiz) {
        frames = horiz != 0.0f ? (uint32_t)(int)((int16_t)(int)sqrt((double)(dz * dz + dx * dx)) / horiz) : maxFrames;
    } else {
        disc = r->vel[1] * r->vel[1] + 2.0f * (nearestPt.y - r->spawnPos[1]) * g_bmxphysGravity;
        if (disc < 0.0f) {
            frames = maxFrames;
        } else {
            s = (float)sqrt(disc);
            frames = (-r->vel[1] < s) ? (uint32_t)(int)((s - r->vel[1]) / g_bmxphysGravity)
                                      : (uint32_t)(int)((-r->vel[1] - s) / g_bmxphysGravity);
        }
    }
    if (frames < 6)
        frames = 6;
    else if (frames >= maxFrames)
        frames = maxFrames;

    if (kind > BMXSURF_EDGE && kind < 13) {
        /* wall / face / prop: up = its normal, tilted 45 deg into it (not for 10) */
        if (zmathAcos((float)fabs(zmathVecDot(&RMAT(r)->right, &g_bmxphysUp))) < 0x2000)
            vecSet(&m.right.x, RMAT(r)->at.x, 0.0f, RMAT(r)->at.z);
        else
            vecSet(&m.right.x, RMAT(r)->right.x, 0.0f, RMAT(r)->right.z);
        m.up = *targetUp;
        zmathTriangleNormal(&m.right, &m.up, &g_zmathOrigin, &g_bmxphysUp);
        zmathVecNormalize(&m.right, &m.right);
        if (zmathVecDot(&m.right, &RMAT(r)->right) < 0.0f)
            zmathVecNegate(&m.right, &m.right);
        zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
        zmathVecNormalize(&m.at, &m.at);
        zmathVecZero(&m.pos);
        if (kind != BMXSURF_FACE10) {
            zmathMatrixIdentity(&rot);
            zmathMatrixRotateX(&rot, &rot, m.at.y <= 0.0f ? 0x2000 : -0x2000);
            zmathMatrixMultiply(&m, &m, &rot);
        }
        r->surface = (uint8_t)kind;
        r->surfaceObj = (kind == BMXSURF_PROP) ? propTarget : (void *)faceTarget;
        bmxphysCopySurface(r);
        vecCopy(r->physLockPos, &nearestPt.x);
        zmathMatrixCopy(MAT(r->physLockMatrix), &m);
        if (m.at.y >= 0.0f)
            r->stateFlags &= ~RIDER_FLAG_MIRROR;
        else
            r->stateFlags |= RIDER_FLAG_MIRROR;
        if (kind != BMXSURF_PROP && trick == 0x2B) {             /* wall tap marker */
            endPt.x = m.right.x * 30.0f + nearestPt.x;
            endPt.y = m.right.y * 30.0f + nearestPt.y;
            endPt.z = m.right.z * 30.0f + nearestPt.z;
            fxSkid(&nearestPt, &endPt, 0, (const RwV3d *)faceTarget);   /* its normal is at +0 */
        }
    } else {
        /* edge: forward along it (towards the rider's forward); too slow along it = stall */
        e = nearest;
        if (fabs(zmathVecDot(V3(e->dir), V3(r->vel))) < g_bmxphysGrindMinSpeed) {
            to = BMXPHYS_STALL_LOCK;
            trick = riderStallTrick(r);
        }
        if (trick < 0x27 || trick > 0x29) {
            vecCopy(&m.right.x, e->dir);
            flipped = zmathVecDot(&RMAT(r)->right, &m.right) < 0.0f;
            if (flipped)
                zmathVecNegate(&m.right, &m.right);
            zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &g_bmxphysUp);
            zmathVecNormalize(&m.at, &m.at);
            zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
            zmathVecZero(&m.pos);
            vecSet(&rel.x, (float)(int16_t)dx, (float)(int16_t)dy, (float)(int16_t)dz);
            s = zmathVecDot(&m.at, &rel);
            if (to == BMXPHYS_GRIND_LOCK && from == BMXPHYS_ROLL) {
                zmathMatrixIdentity(&rot);              /* lean 30 deg towards the edge */
                zmathMatrixRotateX(&rot, &rot, s < 0.0f ? 0x1555 : -0x1555);
                zmathMatrixMultiply(&m, &m, &rot);
            }
            open = zbspEdgeOpenEnd(e);
            if (open == 0) {
                a = s >= 0.0f;
                b = (r->motion[0].flags >> 3) & 1;
            } else {
                /* an open-ended rail: which side to face follows the open end */
                RwV3d side = m.at;
                if ((open < 0) == flipped)
                    zmathVecNegate(&side, &side);
                a = zmathVecDot(&side, &rel) >= 0.0f;
                b = ((r->motion[0].flags >> 3) & 1) ^ (s >= 0.0f);
            }
            if (b == a)
                r->stateFlags &= ~RIDER_FLAG_MIRROR;
            else
                r->stateFlags |= RIDER_FLAG_MIRROR;
        } else {
            /* sideways stalls (tricks 0x27-0x29): face along the edge */
            vecCopy(&m.at.x, e->dir);
            if (zmathVecDot(&RMAT(r)->at, &m.at) < 0.0f)
                zmathVecNegate(&m.at, &m.at);
            zmathTriangleNormal(&m.right, &g_bmxphysUp, &g_zmathOrigin, &m.at);
            zmathVecNormalize(&m.right, &m.right);
            zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
            zmathVecZero(&m.pos);
            r->stateFlags &= ~RIDER_FLAG_MIRROR;
        }
        r->surface = BMXSURF_EDGE;
        r->surfaceObj = e;
        bmxphysCopySurface(r);
        vecCopy(r->physLockPos, &nearestPt.x);
        zmathMatrixCopy(MAT(r->physLockMatrix), &m);
    }

    if (to != from) {
        r->physTimer = (int16_t)frames;
        zmathVecAdd(V3(r->pos), V3(r->pos), V3(r->physHipOffset));
        zmathVecZero(V3(r->physHipOffset));
        r->physLockStep[0] = (nearestPt.x - r->pos[0]) / (float)(int)frames;
        r->physLockStep[1] = (nearestPt.y - r->pos[1]) / (float)(int)frames -
                             g_bmxphysGravity * 0.5f * (float)(int)frames;
        r->physLockStep[2] = (nearestPt.z - r->pos[2]) / (float)(int)frames;
    }
    return to;
}

/* VA 0x00422830, 1235 bytes, [I] row (new function, enter table [4]), status draft */
int bmxphysStateInitGrind(BmxRider *r, int from, int to)
{
    /* Start a grind. From the ground on a quarterpipe face (surface 1..3) the coping edge itself is
     * ground on if it is close (bike within 100, or within 500 and below it) and its kind is not 0. */
    NgColEdge *e;
    RwV3d c, n;
    RwMatrix m, rot;
    float plane[4];
    uint32_t trick, sound;
    int dx, dy, dz, d2;
    uint8_t *prop;

    if (from == BMXPHYS_ROLL && r->surface != BMXSURF_GROUND && r->surface < 4) {
        e = (NgColEdge *)r->surfaceObj;
        bmxphysClosestPointOnEdge(V3(r->pos), e, &c);
        dx = (int)(r->spawnPos[0] - c.x);
        dy = (int)(r->spawnPos[1] - c.y);
        dz = (int)(r->spawnPos[2] - c.z);
        d2 = dz * dz + dy * dy + dx * dx;
        if (((dy >= 0 || d2 >= 250000) && d2 >= 10000) || e->kind == 0)
            return BMXPHYS_ROLL;
        vecCopy(r->physLockPos, &c.x);
        vecCopy(&n.x, e->normal);
        vecCopy(plane, e->normal);
        plane[3] = e->d;
        if (bmxphysEdgePlaneDistance(V3(r->pos), plane) < 0.0f)
            zmathVecNegate(&n, &n);
        if (fabs(zmathVecDot(V3(e->dir), V3(r->vel))) < g_bmxphysGrindMinSpeed) {
            to = BMXPHYS_STALL;
            trick = riderStallTrick(r);
        } else {
            trick = riderGrindTrick(r);
        }
        if (trick < 0x27 || trick > 0x29) {
            vecCopy(&m.right.x, e->dir);
            m.at = n;
            zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
            if (m.up.y > 0.0f) {
                zmathVecNegate(&m.right, &m.right);
                zmathVecNegate(&m.up, &m.up);
            }
            if (zmathVecDot(&RMAT(r)->right, &m.right) < 0.0f) {
                zmathVecNegate(&m.right, &m.right);
                zmathVecNegate(&m.at, &m.at);
            }
            if (r->surface != BMXSURF_SPINE) {
                zmathMatrixIdentity(&rot);       /* lean 20 deg over the coping */
                zmathMatrixRotateX(&rot, &rot, zmathVecDot(&n, &m.at) >= 0.0f ? 0xE38 : -0xE38);
                zmathMatrixMultiply(&m, &m, &rot);
            }
        } else {
            vecCopy(&m.at.x, e->dir);
            if (zmathVecDot(&RMAT(r)->at, &m.at) < 0.0f)
                zmathVecNegate(&m.at, &m.at);
            zmathTriangleNormal(&m.right, &g_bmxphysUp, &g_zmathOrigin, &m.at);
            zmathVecNormalize(&m.right, &m.right);
            zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
        }
        zmathVecZero(&m.pos);
        zmathMatrixCopy(RMAT(r), &m);
        if (zmathVecDot(&m.at, &n) >= 0.0f)
            r->stateFlags &= ~RIDER_FLAG_MIRROR;
        else
            r->stateFlags |= RIDER_FLAG_MIRROR;
        r->surfaceObj = e;
        r->surface = BMXSURF_EDGE;
        bmxphysCopySurface(r);
        zmathMatrixCopy(MAT(r->physLockMatrix), &m);
    }
    e = (NgColEdge *)r->surfaceObj;
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    if (to == BMXPHYS_GRIND) {
        bmxphysSnapToEdge(r, e);
    } else {
        zmathVecZero(V3(r->vel));
        zmathVecZero(V3(r->physSlopeAccel));
        r->frameDist = 0.0f;
        vecCopy(r->pos, r->physLockPos);
        vecCopy(r->spawnPos, r->physLockPos);
    }
    switch (((NgColEdge *)r->surfaceObj)->kind) {
    case 1:  sound = 7; break;
    case 2:  sound = 2; break;
    default: sound = 0; break;
    }
    if (e->owner && (prop = *(uint8_t **)((uint8_t *)e->owner + 0x1EC)) != 0)
        bmxpropTouched(r->slot, (BmxProp *)prop);
    r->wallHeight = 0.0f;
    bmxsoundEvent(0xBBA, r->pos, (uint32_t)r->slot << 28 | sound | 0x200, 0x7F, 0);
    return to;
}

/* VA 0x00422e50, 4330 bytes, [I] row (new function, enter table [1]; name [V] from its message), status draft */
int bmxphysStateInitAir(BmxRider *r, int from, int to)
{
    /* Take-off. Records the take-off contact, measures the height above ground (wallHeight), caps
     * the take-off speed by the launch-ramp class, and sets up the air: auto-rotation to land
     * upright (physAirRot over physTimer frames), quarterpipe air (straight up and back: FAKIE,
     * AlignToRamp, the LIP guard), the spine hop, or the kick off a wall / stall. */
    RwMatrix *m = RMAT(r), tilt, rot;
    RwV3d p, q, n, ep, t, *pos = V3(r->pos);
    NgColEdge *edge;
    void *found = 0;
    NgColHit hit;
    float cap, horiz, dist, plane[4], v, k;
    int cls = -1;                /* tag class of the face under the hips, -1 none / prop */
    int noRotate = 0, a, frames, s;
    int16_t ang;

    vecCopy(r->contact[0].pos, r->pos);                 /* take-off */
    r->contact[0].type = r->surface;
    r->contact[0].obj = r->surfaceObj;
    bmxphysCopyContactSurface(&r->contact[0]);
    vecCopy(r->contact[1].pos, r->pos);
    r->contact[1].obj = r->surfaceObj;
    r->contact[1].type = r->surface;
    bmxphysCopyContactSurface(&r->contact[1]);

    p.x = m->up.x * 100.0f + r->pos[0];
    p.y = m->up.y * 100.0f + r->pos[1];
    p.z = m->up.z * 100.0f + r->pos[2];
    r->stateFlags &= ~RIDER_FLAG_FAKIE;
    q = p;
    q.y = p.y + 10000.0f;
    if (!zcolSegmentTest(&p, &q, &hit)) {
        g_pfnDebugPrintf("bmxphysStateInitAir: no valid terrain found under rider's hips (pos X: %d, Z: %d)\n",
                         (int)p.x, (int)p.z);
    } else {
        r->wallHeight = hit.pos[1] - p.y;                  /* height of the take-off point */
        if ((hit.type & 1) && hit.face) {
            cls = hit.face->code >> 12;
            switch (cls) {
            case 1: case 2: case 3:
                found = bmxphysFindTaggedEdge(hit.unk_04, hit.face->code & 0xFFF, pos);
                cls = hit.face->code >> 12;
                if (!found) {
                    g_pfnDebugPrintf("bmxphysStateInitAir: quarterpipe face with no assigned edge, ID=%d\n",
                                     hit.face->code & 0xFFF);
                    cls = 0;
                    found = hit.face;
                }
                break;
            default:
                cls = 0;
                /* fall through */
            case 4: case 5: case 6: case 7: case 8:
                found = hit.face;
                break;
            }
        } else if ((hit.type & 2) && hit.propObj) {
            r->surface = BMXSURF_PROP;
            r->surfaceObj = hit.propObj;
            bmxphysCopySurface(r);
            found = hit.propObj;
        }
    }
    zmathVecAdd(pos, pos, V3(r->physHipOffset));
    zmathVecZero(V3(r->physHipOffset));
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    r->airLean = 0;
    r->physAirRot[0] = r->physAirRot[1] = r->physAirRot[2] = 0;
    r->physTimer = 0;
    tilt.up = g_bmxphysUp;
    r->stateFlags &= ~RIDER_FLAG_CARRY_SPIN;
    if (from != BMXPHYS_ROLL || zmathVecDot(V3(r->vel), &m->right) >= 0.0f)
        r->stateFlags &= ~RIDER_FLAG_TAKEOFF_BACK;
    else
        r->stateFlags |= RIDER_FLAG_TAKEOFF_BACK;

    if (from == BMXPHYS_ROLL) {
        if (r->surface == BMXSURF_GROUND || r->surface > BMXSURF_VERT) {
            /* off the ground / a kicker / a prop */
            if (cls <= 0 || cls > 3) {
                noRotate = 1;
            } else if (r->stateFlags & RIDER_FLAG_GRIND_BUTTON) {
                /* a quarterpipe face under the hips: grind its coping right away */
                r->surfaceObj = found;
                r->surface = (uint8_t)cls;
                bmxphysCopySurface(r);
                s = bmxphysSetState(r, BMXPHYS_GRIND);
                if (s == BMXPHYS_GRIND || s == BMXPHYS_STALL)
                    return bmxphysGetState(r);
            }
            switch (r->surface) {                         /* take-off speed cap */
            case 4:  cap = 11.0f; break;
            case 5:  cap = 15.0f; break;
            case 6:  cap = 21.0f; break;
            case 7:  cap = 23.0f; break;
            case 8:  cap = 35.0f; break;
            default: cap = g_bmxphysAirSpeedCap; break;
            }
            a = iabs(zmathAcos(-m->up.y) - 0x2000);       /* steepness away from 45 deg */
            if ((int16_t)a < 0x1555) {
                t.x = r->vel[0];
                t.z = r->vel[2];
                t.y = 0.0f;
                cap = (zmathSin(a) + 1.0f) * cap;
                horiz = zmathVecLength(&t);
                if (cap < horiz) {
                    cap /= horiz;
                    r->vel[0] = cap * t.x;
                    r->vel[2] = cap * t.z;
                }
            }
            if (!noRotate)
                goto sound;
        } else {
            /* off a quarterpipe / spine / vert face: the surface is its coping edge */
            edge = (NgColEdge *)r->surfaceObj;
            vecCopy(&n.x, edge->normal);
            vecCopy(plane, edge->normal);
            plane[3] = edge->d;
            dist = bmxphysEdgePlaneDistance(pos, plane);
            if (dist < 0.0f) {
                zmathVecNegate(&n, &n);
                dist = -dist;
            }
            vecSet(&ep.x, edge->v0[0], edge->v0[1], edge->v0[2]);
            if (r->vel[1] < 0.0f && zmathAsin((float)fabs(m->at.y)) < g_bmxphysVertAngle &&
                (((r->surface == BMXSURF_QUARTER || r->surface == BMXSURF_VERT) &&
                  zmathAcos(zmathVecDot(&m->up, &n)) < g_bmxphysVertAngle) ||
                 (r->surface == BMXSURF_SPINE && r->pos[1] - (ep.y - 50.0f) < 250.0f && dist < 300.0f))) {
                /* going up the ramp face */
                if ((r->stateFlags & RIDER_FLAG_GRIND_BUTTON) &&
                    ((s = bmxphysSetState(r, BMXPHYS_GRIND)) == BMXPHYS_GRIND || s == BMXPHYS_STALL))
                    return bmxphysGetState(r);
                if (r->surface == BMXSURF_SPINE) {
                    /* hop over the spine: land (dist + 150) further on the far side */
                    ep.y += 50.0f;
                    if (r->pos[1] <= ep.y)
                        ep.y = r->pos[1];
                    r->pos[1] = ep.y;
                    tilt.up.y = -3.0f;
                    r->pos[0] += (120.0f - dist) * n.x;
                    r->pos[2] += (120.0f - dist) * n.z;
                    r->spawnPos[2] = r->pos[2];
                    tilt.up.x = -n.x;
                    r->spawnPos[0] = r->pos[0];
                    tilt.up.z = -n.z;
                    r->spawnPos[1] = r->pos[1];
                    zmathVecNormalize(&tilt.up, &tilt.up);
                    frames = (int)(g_bmxphysSpineHop * r->vel[1] * -2.0f / g_bmxphysGravity);
                    if ((uint32_t)frames < 15)
                        frames = 15;
                    r->vel[1] = -((float)(uint32_t)frames * g_bmxphysGravity * 0.5f);
                    k = (dist + 150.0f) / (float)(uint32_t)frames;
                    r->vel[0] = -(n.x * k);
                    r->vel[2] = -(n.z * k);
                    r->frameDist = zmathVecLength(V3(r->vel));
                    t.x = m->right.x;
                    t.z = m->right.z;
                    r->physAirRot[1] = 0;
                    r->physAirRot[0] = 0;
                    t.y = -m->right.y;
                    zmathMatrixCopy(MAT(r->physAirMatrix), m);
                    r->physAirRot[2] = (int16_t)(zmathAcos(zmathVecDot(&t, &m->right)) / (frames - 3));
                    if (zmathVecDot(V3(r->vel), &m->right) >= 0.0f)
                        r->physAirRot[2] = (int16_t)-r->physAirRot[2];
                    r->physTimer = (int16_t)(frames - 3);
                    goto sound;
                }
                if (r->stateFlags & RIDER_FLAG_FORWARD_HOP) {
                    /* lip guard: stay inside the ramp unless there is room behind the coping */
                    p.x = n.x * 50.0f + edge->v0[0];
                    p.y = edge->v0[1] - 50.0f;
                    p.z = n.z * 50.0f + edge->v0[2];
                    q.x = n.x * -100.0f + p.x;
                    q.z = n.z * -100.0f + p.z;
                    q.y = p.y;
                    if (zcolSegmentTest(&p, &q, &hit) ||
                        (bmxphysClosestPointOnEdge(pos, edge, &p), (r->pos[1] + r->vel[1]) - p.y >= 150.0f))
                        r->stateFlags &= ~RIDER_FLAG_FORWARD_HOP;
                    if (r->stateFlags & RIDER_FLAG_FORWARD_HOP) {
                        v = zmathVecDot(V3(r->vel), &n);
                        if (-v < 10.0f) {                  /* at least 10 per frame towards the ramp */
                            v = 10.0f + v;
                            r->vel[0] -= v * n.x;
                            r->vel[1] -= v * n.y;
                            r->vel[2] -= v * n.z;
                        }
                        if (r->vel[1] >= -25.0f)
                            r->vel[1] = -25.0f;
                        r->frameDist = zmathVecLength(V3(r->vel));
                        bmxphysClosestPointOnEdge(pos, edge, &p);
                        p.x += n.x * 120.0f;
                        p.y += n.y * 120.0f;
                        p.z += n.z * 120.0f;
                        if (p.y < r->pos[1]) {
                            vecCopy(r->pos, &p.x);
                            vecCopy(r->spawnPos, &p.x);
                        }
                        goto rotate_upright;
                    }
                }
                bmxphysAlignToRamp(r, (NgColEdge *)r->surfaceObj, 1);
                if (r->surface == BMXSURF_VERT) {
                    r->vel[0] = 0.0f;
                    r->vel[2] = 0.0f;
                }
                if (zmathAcos(zmathVecDot(&m->up, &n)) < g_bmxphysQpLimit &&
                    zmathVecDot(V3(r->vel), &m->right) >= 0.0f) {
                    /* straight up the quarterpipe: turn around (180 about the vertical) for the way down */
                    frames = (int)(r->vel[1] * -2.0f / g_bmxphysGravity);
                    if ((uint32_t)frames > 15) {
                        frames -= 7;
                        zmathMatrixCopy(MAT(r->physAirMatrix), m);
                        r->physAirRot[0] = 0;
                        r->physAirRot[2] = 0;
                        zmathMatrixCopy(&rot, m);
                        zmathTriangleNormal(&rot.right, &g_bmxphysUp, &g_zmathOrigin, &rot.up);
                        zmathVecNormalize(&rot.right, &rot.right);
                        zmathTriangleNormal(&rot.at, &rot.right, &g_zmathOrigin, &rot.up);
                        v = zmathVecDot(&m->right, &rot.right);
                        k = zmathVecDot(&m->right, &rot.at);
                        t.x = rot.right.x * v + -(rot.at.x * k);
                        t.y = rot.right.y * v + -(rot.at.y * k);
                        t.z = rot.right.z * v + -(rot.at.z * k);
                        zmathVecNormalize(&t, &t);
                        ang = (int16_t)(zmathAcos(zmathVecDot(&t, &m->right)) / frames);
                        r->physAirRot[1] = ang;
                        if (m->at.y > 0.0f)
                            r->physAirRot[1] = (int16_t)-ang;
                        r->physTimer = (int16_t)frames;
                        r->stateFlags |= RIDER_FLAG_CARRY_SPIN;
                    }
                }
                r->stateFlags |= RIDER_FLAG_FAKIE;
                goto sound;
            }
            r->stateFlags |= RIDER_FLAG_FORWARD_HOP;
        }
rotate_upright:
        /* turn to land upright (up = tilt.up) over 15 frames */
        zmathTriangleNormal(&tilt.at, &m->right, &g_zmathOrigin, &tilt.up);
        zmathVecNormalize(&tilt.at, &tilt.at);
        zmathTriangleNormal(&tilt.right, &tilt.up, &g_zmathOrigin, &tilt.at);
        zmathMatrixCopy(MAT(r->physAirMatrix), m);
        bmxphysMatrixDeltaAngles(m, &tilt, g_bmxphysAutoRotFrames, r->physAirRot);
    } else if (from == BMXPHYS_GRIND) {
        t.x = m->up.x * r->wheel[0].radius;
        t.y = m->up.y * r->wheel[0].radius;
        t.z = m->up.z * r->wheel[0].radius;
        zmathVecAdd(pos, pos, &t);
        zmathVecAdd(V3(r->spawnPos), V3(r->spawnPos), &t);
        if (cls <= 0 || cls > 3)
            goto sound;
        zmathMatrixCopy(MAT(r->physAirMatrix), m);
        ang = zmathAcos(-m->up.y);
        goto turn_six;
    } else if (from == BMXPHYS_STALL) {
        t.x = m->up.x * r->wheel[0].radius;
        t.y = m->up.y * r->wheel[0].radius;
        t.z = m->up.z * r->wheel[0].radius;
        zmathVecAdd(pos, pos, &t);
        zmathVecAdd(V3(r->spawnPos), V3(r->spawnPos), &t);
        if (r->surface != BMXSURF_WALL && r->surface != BMXSURF_PROP) {
            r->vel[0] += m->right.x * 5.0f;
            r->vel[1] += m->right.y * 5.0f;
            r->vel[2] += m->right.z * 5.0f;
            if (cls <= 0 || cls > 3 || (r->trickId > 0x26 && r->trickId < 0x2A))
                goto sound;
            zmathMatrixCopy(MAT(r->physAirMatrix), m);
            ang = zmathAcos(-m->up.y);
turn_six:
            r->physAirRot[1] = (int16_t)(ang / 6);
            if (m->at.y > 0.0f)
                r->physAirRot[1] = (int16_t)-(ang / 6);
            r->physTimer = 6;
            goto sound;
        }
        /* kick off the wall / prop: up and out, turning 60 deg and rolling 50 deg */
        r->vel[0] = m->up.x * 15.0f + m->right.x * 5.0f;
        r->vel[1] = m->up.y * 15.0f + m->right.y * 5.0f;
        r->vel[2] = m->up.z * 15.0f + m->right.z * 5.0f;
        zmathTriangleNormal(&tilt.at, &m->right, &g_zmathOrigin, &tilt.up);
        zmathVecNormalize(&tilt.at, &tilt.at);
        zmathTriangleNormal(&tilt.right, &tilt.up, &g_zmathOrigin, &tilt.at);
        zmathMatrixIdentity(&rot);
        zmathMatrixRotateY(&rot, &rot, m->at.y >= 0.0f ? -0x2AAA : 0x2AAA);
        zmathMatrixMultiply(&tilt, &tilt, &rot);
        zmathMatrixIdentity(&rot);
        zmathMatrixRotateZ(&rot, &rot, -0x238E);
        zmathMatrixMultiply(&tilt, &tilt, &rot);
        zmathMatrixCopy(MAT(r->physAirMatrix), m);
        bmxphysMatrixDeltaAngles(m, &tilt, g_bmxphysAutoRotFrames, r->physAirRot);
    } else {
        goto sound;
    }
    r->physTimer = (int16_t)g_bmxphysAutoRotFrames;

sound:
    if (from == BMXPHYS_ROLL) {
        /* launched off a quarterpipe face nearly vertically: the "lip" sound */
        if (r->surface == BMXSURF_GROUND || r->surface > BMXSURF_VERT ||
            zmathAcos((float)fabs(m->up.y)) < 0x2AAB || zmathAcos((float)fabs(m->right.y)) > 0x1FFF)
            return to;
        bmxsoundEvent(0xBB9, pos, (uint32_t)r->slot << 28 | 0x102, 0x7F, 0);
    } else if ((from == BMXPHYS_GRIND || from == BMXPHYS_STALL) && r->surface == BMXSURF_EDGE &&
               ((NgColEdge *)r->surfaceObj)->kind == 1) {
        bmxsoundEvent(0xBB9, pos, (uint32_t)r->slot << 28 | 0x207, 0x7F, 0);   /* off a metal rail */
    }
    return to;
}

/* VA 0x004243b0, 667 bytes, [I] row (new function, enter table [6]), status draft */
int bmxphysStateInitStall(BmxRider *r, int from, int to)
{
    /* arrive on the stall target: orientation and position from the lock-on, motion stops */
    NgColEdge *e;
    NgColHit hit;
    RwV3d p, q;
    uint8_t *prop;
    uint32_t sound = 0, k;

    switch (r->surface) {
    case BMXSURF_EDGE:
        e = (NgColEdge *)r->surfaceObj;
        if (e->owner && (prop = *(uint8_t **)((uint8_t *)e->owner + 0x1EC)) != 0)
            bmxpropTouched(r->slot, (BmxProp *)prop);
        break;
    case BMXSURF_PROP:
        if ((prop = PG_PROP(r->surfaceObj)) != 0)
            bmxpropTouched(r->slot, (BmxProp *)prop);
        break;
    }
    zmathMatrixCopy(RMAT(r), MAT(r->physLockMatrix));
    r->rotVel[0] = r->rotVel[1] = r->rotVel[2] = 0;
    if (r->surface < BMXSURF_WALL || r->surface > BMXSURF_PROP) {
        r->wallHeight = 0.0f;
    } else {
        /* wall tap / fast plant height: distance from the target down to the ground */
        p.x = MAT(r->physLockMatrix)->up.x * 100.0f + r->physLockPos[0];
        p.y = MAT(r->physLockMatrix)->up.y * 100.0f + r->physLockPos[1];
        p.z = MAT(r->physLockMatrix)->up.z * 100.0f + r->physLockPos[2];
        q = p;
        q.y = p.y + 10000.0f;
        if (zcolSegmentTest(&p, &q, &hit))
            r->wallHeight = hit.pos[1] - p.y;
    }
    vecCopy(r->pos, r->physLockPos);
    vecCopy(r->spawnPos, r->physLockPos);
    if (r->surface == BMXSURF_EDGE) {
        switch (((NgColEdge *)r->surfaceObj)->kind) {
        case 1:  sound = 7; break;
        case 2:  sound = 2; break;
        default: sound = 0; break;
        }
    }
    switch (r->trickId) {
    case 0x25: case 0x26:             k = 3; break;
    case 0x27: case 0x28: case 0x2B:  k = 5; break;
    case 0x2A:                        k = 4; break;
    default:                          k = 2; break;
    }
    bmxsoundEvent(0xBBA, r->pos, (((uint32_t)r->slot << 20 | k) << 8) | sound, 0x7F, 0);
    zmathVecZero(V3(r->vel));
    zmathVecZero(V3(r->physSlopeAccel));
    r->frameDist = 0.0f;
    return to;
}

/* VA 0x00424740, 140 bytes, [I] row, status draft */
int bmxphysStateInitCrash(BmxRider *r, int from, int to)
{
    int16_t noSpin[3];

    r->vel[1] -= 5.0f;                                   /* a little hop up */
    dynStartRider(r->animCtl, r->riderActor, RMAT(r), V3(r->vel), r->rotVel);
    noSpin[0] = noSpin[1] = noSpin[2] = 0;
    dynStartBike(r->bikeCtl, r->bikeActor, BMAT(r), V3(r->vel), noSpin);
    r->wallHeight = 0.0f;
    r->crash = 0;
    return to;
}

/* VA 0x00424800, 404 bytes, [I] row, status draft */
void bmxphysUpdateOrientationRoll(BmxRider *r)
{
    RwMatrix m;
    int16_t a;

    zmathMatrixIdentity(&m);
    /* on a slope (> 30 deg) whose fall line is within 60 deg of sideways, slow riding turns the
     * bike downhill: yaw += (50 - speed) * 0.3 * at.y */
    a = zmathAcos((float)fabs(RMAT(r)->up.y));
    if (fabs((float)a) > 5461.0f) {
        a = zmathAcos((float)fabs(RMAT(r)->at.y));
        if (fabs((float)a) < 10922.0f && r->frameDist < 50.0f && r->frameDist > 0.1f &&
            r->physSlopeAccel[2] * r->physSlopeAccel[2] + r->physSlopeAccel[1] * r->physSlopeAccel[1] +
            r->physSlopeAccel[0] * r->physSlopeAccel[0] > 0.1f)
            r->rotVel[1] += ftos((50.0f - r->frameDist) * g_bmxphysYawDamp * RMAT(r)->at.y * -182.04445f);
    }
    if (r->motion[0].id == 2) {
        zmathMatrixRotateXYZ(&m, &m, r->rotVel);
        zmathMatrixMultiply(RMAT(r), RMAT(r), &m);
        zmathMatrixMultiply(BMAT(r), BMAT(r), &m);
        r->rotVel[1] = ftos(r->rotVel[1] - g_bmxphysSpinDamp * r->rotVel[1]);
        if (fabs((float)r->rotVel[1]) < 91.0f)   /* < 0.5 deg per frame */
            r->rotVel[1] = 0;
    }
}

/* VA 0x00424a10, 913 bytes, [I] row (new function, orientation table), status draft */
void bmxphysUpdateOrientationAir(BmxRider *r)
{
    /* spin by rotVel; while physTimer runs add the take-off auto-rotation physAirRot (in the
     * frame of physAirMatrix unless RIDER_FLAG_CARRY_SPIN), and with RIDER_FLAG_FLIP the extra
     * rotation flipVel in trickMatrix's frame; re-orthonormalise; the bike follows */
    RwMatrix *m = RMAT(r);
    RwMatrix rot, ref;
    RwV3d t;

    zmathMatrixIdentity(&rot);
    zmathMatrixRotateXYZ(&rot, &rot, r->rotVel);
    zmathMatrixMultiply(m, m, &rot);
    if (r->physTimer != 0) {
        if (!(r->stateFlags & RIDER_FLAG_CARRY_SPIN)) {
            /* rotate the rows into the take-off frame, rotate there, and back */
            zmathMatrixCopy(&ref, MAT(r->physAirMatrix));
            zmathMatrixTranspose(&ref, &ref);
            zmathTransformPoint(&t, &ref, &m->right); m->right = t;
            zmathTransformPoint(&t, &ref, &m->up);    m->up = t;
            zmathTransformPoint(&t, &ref, &m->at);    m->at = t;
            zmathMatrixRotateXYZ(m, m, r->physAirRot);
            zmathMatrixTranspose(&ref, &ref);
            zmathTransformPoint(&t, &ref, &m->right); m->right = t;
            zmathTransformPoint(&t, &ref, &m->up);    m->up = t;
            zmathTransformPoint(&t, &ref, &m->at);    m->at = t;
        } else {
            zmathMatrixIdentity(&rot);
            zmathMatrixRotateXYZ(&rot, &rot, r->physAirRot);
            zmathMatrixMultiply(m, m, &rot);
        }
        r->physTimer--;
    }
    if (r->stateFlags & RIDER_FLAG_FLIP) {
        zmathMatrixCopy(&ref, MAT(r->trickMatrix));
        zmathMatrixTranspose(&ref, &ref);
        zmathTransformPoint(&t, &ref, &m->right); m->right = t;
        zmathTransformPoint(&t, &ref, &m->up);    m->up = t;
        zmathTransformPoint(&t, &ref, &m->at);    m->at = t;
        zmathMatrixRotateXYZ(m, m, r->flipVel);
        zmathMatrixTranspose(&ref, &ref);
        zmathTransformPoint(&t, &ref, &m->right); m->right = t;
        zmathTransformPoint(&t, &ref, &m->up);    m->up = t;
        zmathTransformPoint(&t, &ref, &m->at);    m->at = t;
    }
    zmathVecNormalize(&m->up, &m->up);
    zmathTriangleNormal(&m->at, &m->right, &g_zmathOrigin, &m->up);
    zmathVecNormalize(&m->at, &m->at);
    zmathTriangleNormal(&m->right, &m->up, &g_zmathOrigin, &m->at);
    zmathMatrixCopy(BMAT(r), m);
}

/* VA 0x00424ea0, 1500 bytes, [I] row, status draft */
void bmxphysUpdateOrientationGrind(BmxRider *r)
{
    /* balance on the edge / wall: rotVel[0] is the lean rate. Gravity tips the rider by
     * (40 - speed) * (1 + 10 * (1 - stat 5)) * 0.1 * lean; leaning more than 45 deg from the
     * lock-on frame ends the grind with a hop off (state AIR). Tricks 0x27-0x29 (the stall-like
     * grinds) lean about z instead of x. */
    RwMatrix rot, *m = RMAT(r);
    RwMatrix *lock = MAT(r->physLockMatrix);
    RwV3d up;
    float lean, d;
    int sideways = r->trickId >= 0x27 && r->trickId <= 0x29;

    zmathMatrixIdentity(&rot);
    switch (r->surface) {
    case BMXSURF_EDGE:
        if (r->frameDist < 40.0f) {
            lean = sideways ? -m->right.y : m->at.y;
            if (fabs(lean) < 0.1f)
                lean = lean < 0.0f ? -0.1f : 0.1f;
            r->rotVel[0] += ftos((40.0f - r->frameDist) * ((1.0f - r->stats[5]) * 10.0f + 1.0f) *
                                     0.1f * lean * 182.04445f);
        }
        if (!sideways)
            zmathMatrixRotateX(&rot, &rot, r->rotVel[0]);
        else
            zmathMatrixRotateZ(&rot, &rot, r->rotVel[0]);
        zmathMatrixMultiply(m, m, &rot);
        zmathMatrixMultiply(BMAT(r), BMAT(r), &rot);
        r->rotVel[0] = ftos(r->rotVel[0] - r->rotVel[0] * 0.35f);

        /* lean = angle between up and the lock frame's up, measured in the balance plane */
        up = m->up;
        d = zmathVecDot(&up, sideways ? &lock->at : &lock->right);
        if (!sideways) {
            up.x -= d * lock->right.x; up.y -= d * lock->right.y; up.z -= d * lock->right.z;
        } else {
            up.x -= d * lock->at.x; up.y -= d * lock->at.y; up.z -= d * lock->at.z;
        }
        zmathVecNormalize(&up, &up);
        if (fabs((float)zmathAcos(zmathVecDot(&up, &lock->up))) <= (float)g_bmxphysLandAngle)
            break;
        /* fell off: push away from the edge and fly */
        zmathMatrixCopy(&rot, lock);
        if (!sideways) {
            rot.at.y = 0.0f;
            zmathVecNormalize(&rot.at, &rot.at);
            zmathTriangleNormal(&rot.up, &rot.at, &g_zmathOrigin, &rot.right);
            if (zmathVecDot(&rot.at, &m->up) < 0.0f)
                zmathVecNegate(&rot.at, &rot.at);
            r->vel[0] += rot.at.x * 10.0f + rot.up.x * 7.5f;
            r->vel[1] += rot.at.y * 10.0f + rot.up.y * 7.5f;
            r->vel[2] += rot.at.z * 10.0f + rot.up.z * 7.5f;
        } else {
            rot.right.y = 0.0f;
            zmathVecNormalize(&rot.right, &rot.right);
            zmathTriangleNormal(&rot.up, &rot.at, &g_zmathOrigin, &rot.right);
            if (zmathVecDot(&rot.right, &m->up) < 0.0f)
                zmathVecNegate(&rot.right, &rot.right);
            if (zmathVecDot(&m->right, &rot.up) < 0.0f) {
                r->vel[0] += rot.up.x * 15.0f;
                r->vel[1] += rot.up.y * 15.0f;
                r->vel[2] += rot.up.z * 15.0f;
            } else {
                r->vel[0] = rot.up.x * 17.0f + rot.right.x * 9.5f;
                r->vel[1] = rot.up.y * 17.0f + rot.right.y * 9.5f;
                r->vel[2] = rot.up.z * 17.0f + rot.right.z * 9.5f;
            }
        }
        bmxphysSetState(r, BMXPHYS_AIR);
        return;
    case BMXSURF_FACE10:
        zmathMatrixRotateY(&rot, &rot, -r->rotVel[0]);
        r->rotVel[0] = ftos(r->rotVel[0] - r->rotVel[0] * 0.35f);
        zmathMatrixMultiply(m, m, &rot);
        zmathMatrixMultiply(BMAT(r), BMAT(r), &rot);
        return;
    case BMXSURF_WALL:
    case BMXSURF_PROP:
        r->rotVel[0] += ftos((30.0f - r->frameDist) * ((1.0f - r->stats[5]) * 10.0f + 1.0f) * 0.1f *
                                 m->at.y * 182.04445f);
        zmathMatrixRotateX(&rot, &rot, r->rotVel[0]);
        zmathMatrixMultiply(m, m, &rot);
        zmathMatrixMultiply(BMAT(r), BMAT(r), &rot);
        r->rotVel[0] = ftos(r->rotVel[0] - r->rotVel[0] * 0.35f);
        if (zmathAsin(-m->up.y) < 0xAAA)                 /* tipped below 15 deg: fall off */
            bmxphysSetState(r, BMXPHYS_AIR);
        break;
    }
}

/* VA 0x00425610, 227 bytes, [I] row (new function, orientation table), status draft */
void bmxphysUpdateOrientationLockOn(BmxRider *r)
{
    /* turn 15 % per frame towards the orientation on the target */
    RwMatrix *m = RMAT(r);

    blendRow(&m->right, &MAT(r->physLockMatrix)->right);
    blendRow(&m->up, &MAT(r->physLockMatrix)->up);
    zmathTriangleNormal(&m->at, &m->right, &g_zmathOrigin, &m->up);
    zmathVecNormalize(&m->at, &m->at);
    zmathTriangleNormal(&m->right, &m->up, &g_zmathOrigin, &m->at);
}

/* VA 0x00425740, 1 bytes, [I] row (new function, forces table), status draft */
void bmxphysUpdateForcesRoll(BmxRider *r)
{
    /* nothing: the roll forces are applied per wheel in bmxphysUpdatePositionRoll */
}

/* VA 0x00425760, 96 bytes, [I] row (new function, forces table), status draft */
void bmxphysUpdateForcesAir(BmxRider *r)
{
    vecSet(r->physSlopeAccel, 0.0f, g_bmxphysGravity, 0.0f);
    r->vel[1] += g_bmxphysGravity;
    r->frameDist = zmathVecLength(V3(r->vel));
    zmathVecScale(V3(r->vel), V3(r->vel), 1.0f - g_bmxphysAirDrag);
}

/* VA 0x004257e0, 238 bytes, [I] row, status draft */
void bmxphysUpdateForcesGrind(BmxRider *r)
{
    /* gravity along the edge, then friction 0.85 % per frame, x (1 + 10 * (1 - stat 5)) */
    NgColEdge *e = (NgColEdge *)r->surfaceObj;
    RwV3d a;
    float g;

    vecCopy(&a.x, e->dir);
    if (zmathVecDot(&RMAT(r)->right, V3(e->dir)) < 0.0f)
        zmathVecNegate(&a, &a);
    g = g_bmxphysGravity * a.y;
    a.x *= g;
    a.y *= g;
    a.z *= g;
    vecCopy(r->physSlopeAccel, &a.x);
    zmathVecAdd(V3(r->vel), V3(r->vel), &a);
    zmathVecScale(V3(r->vel), V3(r->vel), 1.0f - ((1.0f - r->stats[5]) * 10.0f + 1.0f) * g_bmxphysRollDrag[1]);
    r->frameDist = zmathVecLength(V3(r->vel));
}

/* VA 0x00425910, 23 bytes, [I] row (new function, forces table), status draft */
void bmxphysUpdateForcesLockOn(BmxRider *r)
{
    r->physLockStep[1] += g_bmxphysGravity;     /* the lock-on flight is a parabola */
}

/* VA 0x00425940, 10654 bytes, [V] direct (new function, position table [0]), status draft */
void bmxphysUpdatePositionRoll(BmxRider *r)
{
    /* Two-wheel ground model. For each wheel: wall probes ahead (left and right of the hub), the
     * suspension (spring travel clamped to suspension[0..1]), a ground probe along -up, sliding
     * on the surface, weight transfer on slopes, the surface type (tag class / material) and
     * rolling with gravity along the slope and per-material drag. Then the bike frame is rebuilt
     * from the two hubs (forward = front hub - rear hub), the bike leans into turns, the wheel
     * velocities are re-derived from the moved hubs, and with both wheels in the air the rider
     * goes to AIR. */
    RwMatrix *bm = BMAT(r), fr, other, m, rot;
    BmxWheel *w, *ow;
    NgColHit hit;
    RwV3d hub[2], next[2], *h, q, n, d, c0, c1, centre, t, old;
    float reach, travel, dt, impact, s, f, damp, x, height, base;
    uint32_t i, side, airborne = 0, sound, k, cnt, hz, grp;
    int rear, solid, cls, sgn, ang, base16;
    void *obj;
    NgColEdge *e;
    uint8_t *info;

    fxSetRider(r);                                   /* dust / skid marks are spawned for this rider */
    for (i = 0; i < 2; i++) {
        w = &r->wheel[i];
        ow = &r->wheel[i == 0];
        rear = (int)(i & 1);
        h = &hub[i];
        w->hit = 0;
        zmathTransformPoint(h, bm, V3(w->pos));
        zmathVecAdd(h, h, V3(r->spawnPos));

        /* -- wall probes, only for the leading wheel of the direction of travel */
        if ((zmathVecDot(V3(w->vel), &bm->right) >= 0.0f) != rear) {
            for (side = 0; side < 2; side++) {
                reach = rear ? -w->radius : w->radius;
                if (w->contact == 2) {
                    t.x = ow->frame[0]; t.y = ow->frame[1]; t.z = ow->frame[2];
                } else {
                    t = bm->right;
                }
                s = (side & 1) ? g_bmxphysWheelProbe : -g_bmxphysWheelProbe;
                q.x = s * bm->at.x + t.x * reach + h->x + w->vel[0];
                q.y = s * bm->at.y + t.y * reach + w->vel[1] + h->y;
                q.z = s * bm->at.z + reach * t.z + h->z + w->vel[2];
                if (!zcolSegmentTest(h, &q, &hit))
                    continue;
                vecCopy(&n.x, hit.normal);
                if ((n.y != 0.0f || (hit.face && (hit.face->code >> 8 & 0xF0))) &&
                    zmathAcos((float)fabs(zmathVecDot(&bm->at, &n))) > 0x1555 &&
                    zmathAcos(zmathVecDot(&bm->up, &n)) < g_bmxphysWallAngle)
                    continue;                          /* floor-like: the ground probe handles it */
                impact = -zmathVecDot(V3(w->vel), &n);
                if (impact <= 0.0f)
                    continue;
                solid = 1;
                if (hit.face && (hit.face->code & 0xF000) == 0x9000 && impact > 0.0f) {
                    bmxpropHitTrigger(r->slot, hit.face);  /* class 9: a trigger face, not solid */
                    solid = 0;
                }
                if ((!(hit.type & 2) || !*(void **)((uint8_t *)hit.propObj + 0x1EC) ||
                     bmxpropTouched(r->slot, *(void **)((uint8_t *)hit.propObj + 0x1EC)) != 0) && solid) {
                    w->hit |= 1;
                    if (impact > 10.0f) {
                        r->stateFlags |= RIDER_FLAG_HIT_WALL;
                        if ((~r->motion[0].flags >> 3 & 1) == (uint32_t)(zmathVecDot(&n, &RMAT(r)->at) >= 0.0f))
                            r->stateFlags &= ~RIDER_FLAG_MIRROR;
                        else
                            r->stateFlags |= RIDER_FLAG_MIRROR;
                    }
                    sound = 0;
                    soundForMaterial(&hit, &sound);
                    if (impact > 10.0f) {
                        k = (uint32_t)(int)(impact + impact);
                        bmxsoundEvent(0xBBC, r->pos, (uint32_t)r->slot << 28 | sound | 0x100,
                                          (int)k > 0x7E ? 0x7F : (int)k, 0);
                    }
                    if (impact >= g_bmxphysCrashImpact * r->stats[4]) {
                        bmxphysSetState(r, BMXPHYS_CRASH);   /* hit a wall too hard */
                        r->crashKind = 0;
                        riderAddCrashDamage(r, 0, impact);
                        if (hit.type == 2)
                            zbspPropSetMode(hit.propObj, 0);
                        return;
                    }
                    ang = zmathAcos((float)fabs(zmathVecDot(&bm->right, &n)));
                    if (ang < 0xAAA) {
                        /* head-on (within 15 deg): both wheels bounce back at a quarter speed */
                        zmathVecScale(V3(r->wheel[0].vel), V3(r->wheel[0].vel), -0.25f);
                        r->wheel[0].speed *= 0.25f;
                        zmathVecScale(V3(r->wheel[1].vel), V3(r->wheel[1].vel), -0.25f);
                        r->wheel[1].speed *= 0.25f;
                        r->stateFlags ^= RIDER_FLAG_TURNAROUND;
                    } else {
                        /* glancing: deflect along the wall and lose 0..67 % of the speed */
                        impact *= 1.25f;
                        w->vel[0] += impact * n.x;
                        w->vel[1] += impact * n.y;
                        w->vel[2] += impact * n.z;
                        vecCopy(w->frame, w->vel);
                        zmathVecNormalize(&WFRAME(w)->right, &WFRAME(w)->right);
                        zmathTriangleNormal(&WFRAME(w)->at, &WFRAME(w)->right, &g_zmathOrigin, &WFRAME(w)->up);
                        s = 1.0f - zmathCos(ang) * 0.67f;
                        zmathVecScale(V3(w->vel), V3(w->vel), s);
                        w->speed = zmathVecLength(V3(w->vel));
                        zmathVecScale(V3(ow->vel), V3(ow->vel), s);
                        w->speed = zmathVecLength(V3(ow->vel));     /* [V] as compiled: this wheel's speed */
                    }
                    break;
                }
            }
        }

        /* -- suspension */
        travel = w->travel + w->travelVel;
        if (travel < r->suspension[0]) {
            dt = r->suspension[0] - w->travel;
            travel = r->suspension[0];
            w->travelVel = 0.0f;
        } else if (travel > r->suspension[1]) {
            dt = r->suspension[1] - w->travel;
            travel = r->suspension[1];
            w->travelVel = 0.0f;
        } else {
            dt = w->travelVel;
        }
        w->travel = travel;
        if (w->contact < 2) {
            h->x += dt * bm->up.x;
            h->y += dt * bm->up.y;
            h->z += dt * bm->up.z;
        }

        /* -- ground under the wheel */
        reach = w->travelVel + w->radius + travel;
        q.x = (h->x + w->vel[0]) - reach * bm->up.x;
        q.y = (w->vel[1] + h->y) - reach * bm->up.y;
        q.z = (h->z + w->vel[2]) - reach * bm->up.z;
        if (g_bmxphysDebugDraw)
            dbgLine(&h->x, &q.x, 0xC0, 0xA0, 0x20);
        if (zcolSegmentTest(h, &q, &hit)) {
            vecCopy(&n.x, hit.normal);
            if (zmathAcos((float)fabs(zmathVecDot(&bm->at, &n))) >= 0x1556 &&
                zmathAcos(zmathVecDot(&bm->up, &n)) < g_bmxphysWallAngle) {
                /* hit the ground: rest on it, slide along it */
                t.x = hit.pos[0] - h->x;
                t.y = hit.pos[1] - h->y;
                t.z = hit.pos[2] - h->z;
                travel = -zmathVecDot(&t, &bm->up) - w->radius;
                if (travel > r->suspension[1])
                    travel = r->suspension[1];
                else if (travel < r->suspension[0])
                    travel = r->suspension[0];
                old.x = h->x + w->vel[0];
                old.y = w->vel[1] + h->y;
                old.z = h->z + w->vel[2];
                s = travel + w->radius;
                h->x = s * bm->up.x + hit.pos[0];
                h->y = s * bm->up.y + hit.pos[1];
                h->z = s * bm->up.z + hit.pos[2];
                d.x = old.x - h->x;
                d.y = old.y - h->y;
                d.z = old.z - h->z;
                s = zmathVecDot(&d, &n);
                h->x += d.x - s * n.x;
                h->y += d.y - s * n.y;
                h->z += d.z - s * n.z;
                goto grounded;
            }
        }

        /* no ground right under the wheel */
        h->x += w->vel[0];
        h->y += w->vel[1];
        h->z += w->vel[2];
        if ((r->stateFlags & (RIDER_FLAG_MANUAL | RIDER_FLAG_NOSE_MANUAL)) && w->contact != 2 && ow->contact == 2) {
            /* balancing on this wheel: look further down along the wheel's own normal */
            s = w->travel + w->travelVel;
            f = -(g_bmxphysGravity * bm->up.y) * g_bmxphysSuspGravity + w->radius;
            q.x = (h->x - s * bm->up.x) - f * w->frame[4];
            q.y = (h->y - s * bm->up.y) - f * w->frame[5];
            q.z = (h->z - s * bm->up.z) - f * w->frame[6];
            if (zcolSegmentTest(h, &q, &hit)) {
                vecCopy(&n.x, hit.normal);
                if (zmathAcos((float)fabs(zmathVecDot(&bm->at, &n))) >= 0x1556 &&
                    rollOntoSurface(r, &n, &w->frame[4], rear)) {
                    t.x = (w->frame[4] * w->radius + hit.pos[0]) - h->x;
                    t.y = (w->frame[5] * w->radius + hit.pos[1]) - h->y;
                    t.z = (w->radius * w->frame[6] + hit.pos[2]) - h->z;
                    s = -zmathVecDot(&t, &bm->up);
                    travel = s > r->suspension[1] ? r->suspension[1] : (s < r->suspension[0] ? r->suspension[0] : s);
                    dt = s - travel;
                    goto settle;
                }
            }
        }
        reach = w->travel + w->travelVel + w->radius;
        if (w->contact != 2)
            reach += -(g_bmxphysGravity * bm->up.y) * g_bmxphysSuspGravity;
        q.x = h->x - reach * bm->up.x;
        q.y = h->y - reach * bm->up.y;
        q.z = h->z - reach * bm->up.z;
        if (zcolSegmentTest(h, &q, &hit)) {
            vecCopy(&n.x, hit.normal);
            if (zmathAcos((float)fabs(zmathVecDot(&bm->at, &n))) > 0x1555 &&
                rollOntoSurface(r, &n, &bm->up.x, rear)) {
                /* the ground fell away less than the suspension can follow: keep contact */
                t.x = hit.pos[0] - h->x;
                t.y = hit.pos[1] - h->y;
                t.z = hit.pos[2] - h->z;
                s = -zmathVecDot(&t, &bm->up);
                travel = s - w->radius;
                if (travel > r->suspension[1])
                    travel = r->suspension[1];
                else if (travel < r->suspension[0])
                    travel = r->suspension[0];
                dt = s - (travel + w->radius);
settle:
                h->x -= dt * bm->up.x;
                h->y -= dt * bm->up.y;
                h->z -= dt * bm->up.z;
                goto grounded;
            }
        }

        /* -- the wheel is in the air */
        if (!(r->stateFlags & (RIDER_FLAG_MANUAL | RIDER_FLAG_NOSE_MANUAL))) {
            s = g_bmxphysGravity * bm->up.y;
            w->vel[0] += s * bm->up.x;
            w->vel[1] += s * bm->up.y;
            w->vel[2] += s * bm->up.z;
        } else if (ow->contact != 2) {
            /* manual / nose manual: steer the raised wheel around the balance point (40 deg rear,
             * 25 deg front); r->grindBalance is the HUD balance meter (-1..1) */
            zmathMatrixCopy(&other, MAT(ow->frame));
            zmathTriangleNormal(&other.at, &bm->right, &g_zmathOrigin, &other.up);
            zmathVecNormalize(&other.at, &other.at);
            zmathTriangleNormal(&other.right, &other.up, &g_zmathOrigin, &other.at);
            ang = zmathAcos(zmathVecDot(&other.right, &bm->right));
            s = zmathVecDot(V3(w->vel), &other.up);
            w->vel[0] -= s * other.up.x;
            w->vel[1] -= s * other.up.y;
            w->vel[2] -= s * other.up.z;
            rot.right = other.right;
            zmathTriangleNormal(&rot.at, &rot.right, &g_zmathOrigin, &g_bmxphysUp);
            zmathVecNormalize(&rot.at, &rot.at);
            zmathTriangleNormal(&rot.up, &rot.at, &g_zmathOrigin, &rot.right);
            base16 = (rear ? 0xAAA : 0) + 0x11C7;
            if (zmathAcos(-rot.up.y) + base16 < 0x2AAB)
                base16 += zmathAcos(-rot.up.y);
            else
                base16 = 0x2AAA;
            if ((~i & 1) != (uint32_t)(other.right.y < 0.0f))
                base16 = (int)(-((float)(int16_t)base16 * other.up.y));
            base16 -= ang;
            f = (float)(int16_t)base16 * 0.005493164f * -0.022222223f;      /* degrees / 45 */
            r->grindBalance = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
            base = r->wheel[0].pos[0] - r->wheel[1].pos[0];                 /* wheelbase */
            s = zmathSin(base16) * base * -0.075f;
            w->vel[0] += s * other.up.x;
            w->vel[1] += s * other.up.y;
            w->vel[2] += s * other.up.z;
            s = zmathSin(rear ? -r->rotVel[2] : r->rotVel[2]) * base;
            w->vel[0] += s * other.up.x;
            w->vel[1] += s * other.up.y;
            w->vel[2] += s * other.up.z;
            /* yaw the raised wheel's velocity by 0.15 x the yaw rate (at most +-910 = 5 deg) */
            zmathMatrixCopy(&rot, bm);
            vecCopy(&t.x, w->vel);
            zmathMatrixTranspose(&rot, &rot);
            zmathTransformPoint(&d, &rot, &t);
            sgn = rear ? r->rotVel[1] : -r->rotVel[1];
            s = (float)sgn * 0.15f;
            s = s > 910.0f ? 910.0f : (s < -910.0f ? -910.0f : s);
            zmathVecRotateY(&d, &d, (int)s);
            zmathMatrixTranspose(&rot, &rot);
            zmathTransformPoint(&t, &rot, &d);
            s = other.at.y * g_bmxphysGravity;
            w->vel[0] = s * other.at.x + t.x;
            w->vel[1] = s * other.at.y + t.y;
            w->vel[2] = s * other.at.z + t.z;
        }
        f = ((r->suspension[1] - w->travel) * r->suspension[2]) / r->tuneC70 + w->travelVel;
        damp = f >= 0.0f ? r->suspension[4] : r->suspension[3];
        w->contact = 2;
        w->travelVel = (1.0f - damp) * f;
        w->spin = (int16_t)(int32_t)(w->spin * 0.99f * 65536.0f);              /* [V] as compiled */
        airborne++;
        continue;

grounded:
        /* -- weight transfer: slope acceleration along the bike shifts load between the wheels */
        s = zmathVecDot(V3(r->physSlopeAccel), &RMAT(r)->right);
        height = r->tuneC68 + w->travel + w->radius;
        base = r->wheel[0].pos[0] - r->wheel[1].pos[0];
        x = (rear ? s : -s) * height / base;
        x = x > 0.5f ? 0.5f : (x < -0.5f ? -0.5f : x);
        f = ((x + 0.5f) * r->tuneC6C * bm->up.y * g_bmxphysGravity + (r->suspension[1] - travel) * r->suspension[2]) /
            (r->tuneC6C * 0.5f) + w->travelVel;
        damp = f >= 0.0f ? r->suspension[4] : r->suspension[3];
        w->travel = travel;
        vecCopy(&w->frame[4], &n.x);
        w->travelVel = (1.0f - damp) * f;
        vecCopy(&w->frame[0], &bm->right.x);
        zmathTriangleNormal(&WFRAME(w)->at, &WFRAME(w)->right, &g_zmathOrigin, &WFRAME(w)->up);
        zmathVecNormalize(&WFRAME(w)->at, &WFRAME(w)->at);
        zmathTriangleNormal(&WFRAME(w)->right, &WFRAME(w)->up, &g_zmathOrigin, &WFRAME(w)->at);
        if (!rear) {
            zmathMatrixIdentity(&rot);                 /* the front wheel is steered */
            zmathMatrixRotateY(&rot, &rot, r->rotVel[1]);
            zmathMatrixMultiply(WFRAME(w), WFRAME(w), &rot);
        }
        w->material = 0;
        if (hit.type & 1) {
            if (hit.material)
                w->material = *hit.material & 0xF;
        } else if (hit.type & 2) {
            if (*(void **)((uint8_t *)hit.propObj + 0x1EC))
                bmxpropTouched(r->slot, *(void **)((uint8_t *)hit.propObj + 0x1EC));
            info = (uint8_t *)zbspGetPropShape(hit.propId);
            if (zcolGetPropShape(hit.propId))
                w->material = info[2];
        }
        switch (w->material) {
        case 5: case 9: case 10:                       /* dirt / grass: the rear wheel throws dirt */
            if (!rear)
                goto classify;
            cnt = (uint32_t)(int)(w->speed * 0.15f);
            for (k = 0; k < cnt; k++, cnt = (uint32_t)(int)(w->speed * 0.15f)) {
                uint32_t rnd = zmathRandom();
                if ((rnd & 0xF) == 0) {
                    vecSet(&t.x, (float)(rnd & 0x1F), 0.0f, 0.0f);
                    zmathVecRotateY(&t, &t, zmathRandom());
                    t.x += hit.pos[0];
                    t.y += hit.pos[1];
                    t.z += hit.pos[2];
                    vecSet(&d.x, w->vel[0] * 0.75f, w->vel[1] * 0.75f, w->vel[2] * 0.75f);
                    fxDust(&t, &d);
                }
            }
            break;
        case 6:                                        /* hazard material (water, ...) */
            zactorClearFlag2(r->riderActor);
            zactorClearFlag2(r->bikeActor);
            hz = *hit.material >> 19 & 3;
            if (hz == 1) {
                bmxphysSetState(r, BMXPHYS_CRASH);
                r->crashKind = 9;
                r->surface = BMXSURF_HAZARD_E;
                return;
            }
            if (hz != 2) {
                bmxphysSetState(r, BMXPHYS_CRASH);
                r->crashKind = 9;
                r->surface = BMXSURF_HAZARD_D;
                return;
            }
            break;
        case 0xB:                                      /* rumble strip */
            k = (uint32_t)(int)(r->frameDist * 1.7f);
            contrlerRumble(r->slot, 1, (int)k > 0x3F ? 0x40 : (int)k);
            break;
        }
        if (rear && r->wheel[0].contact != 2)
            goto roll;                                 /* the front wheel decides the surface */
classify:
        /* -- what the bike stands on (the front wheel, or the rear with the front in the air) */
        if (fabs(n.y) > 0.1f) {
            if (!(hit.type & 1) || !hit.face) {
                if ((hit.type & 2) && hit.propObj) {
                    cls = BMXSURF_PROP;
                    obj = hit.propObj;
                    goto set_surface;
                }
                g_pfnDebugPrintf("bmxphysUpdatePositionRoll: no valid terrain below front wheel (pos X: %d, Z: %d)\n",
                                 (int)hub[0].x, (int)hub[0].z);
                r->stateFlags &= ~RIDER_FLAG_FORWARD_HOP;
                goto roll;
            }
            cls = hit.face->code >> 12;
            obj = hit.face;
            switch (cls) {
            case 1: case 2: case 3:
                e = bmxphysFindTaggedEdge(hit.unk_04, hit.face->code & 0xFFF, V3(r->pos));
                cls = hit.face->code >> 12;
                if (!e) {
                    g_pfnDebugPrintf("bmxphysUpdatePositionRoll: quarterpipe face with no assigned edge, ID=%d\n",
                                     hit.face->code & 0xFFF);
                    cls = 0;
                    break;
                }
                vecCopy(&d.x, e->normal);
                if (r->surface == BMXSURF_GROUND && cls != 0) {
                    /* rolled from flat ground onto a ramp face */
                    if (r->stateFlags & RIDER_FLAG_GRIND_BUTTON) {
                        r->surface = (uint8_t)cls;
                        r->surfaceObj = e;
                        bmxphysCopySurface(r);
                        k = (uint32_t)bmxphysSetState(r, BMXPHYS_GRIND);
                        if (k == BMXPHYS_GRIND || k == BMXPHYS_STALL)
                            return;
                    }
                    if (zmathAcos((float)fabs(zmathVecDot(&RMAT(r)->right, &d))) < 0x1555 &&
                        zmathAcos(-RMAT(r)->up.y) < 0xAAA && r->frameDist > 15.0f)
                        bmxsoundEvent(0xBBB, hit.pos, (uint32_t)r->slot << 28 | 0x102, 0x7F, 0);
                }
                obj = e;
                break;
            case 4: case 5: case 6: case 7: case 8:
                break;
            case 10:
                /* launch pad: the face id is the launch speed along its normal */
                s = zmathVecDot(V3(r->vel), &n);
                r->vel[0] -= s * n.x;
                r->vel[1] -= s * n.y;
                r->vel[2] -= s * n.z;
                s = (float)(int16_t)(hit.face->code & 0xFFF);
                r->vel[0] += s * n.x;
                r->vel[1] += s * n.y;
                r->vel[2] += s * n.z;
                r->frameDist = zmathVecLength(V3(r->vel));
                bmxphysSetState(r, BMXPHYS_AIR);
                bmxsoundEvent(0xBB9, r->pos, (uint32_t)r->slot << 28 | 0x109, 0x7F, 0);
                r->surface = BMXSURF_GROUND;
                r->surfaceObj = hit.face;
                bmxphysCopySurface(r);
                return;
            default:
                if (r->surface == BMXSURF_QUARTER && (r->stateFlags & RIDER_FLAG_GRIND_BUTTON)) {
                    k = (uint32_t)bmxphysSetState(r, BMXPHYS_GRIND);
                    if (k == BMXPHYS_GRIND || k == BMXPHYS_STALL)
                        return;
                }
                cls = 0;
                r->stateFlags &= ~RIDER_FLAG_FORWARD_HOP;
                break;
            }
set_surface:
            r->surface = (uint8_t)cls;
            r->surfaceObj = obj;
            bmxphysCopySurface(r);
        }
roll:
        /* -- roll: gravity along the slope, boost / drag, per-surface rolling resistance */
        zmathMatrixCopy(&fr, WFRAME(w));
        if (zmathVecDot(V3(w->vel), &fr.right) < 0.0f)
            zmathVecNegate(&fr.right, &fr.right);
        if (rear)
            w->speed += r->pedalForce;
        w->speed = (fr.right.y * g_bmxphysGravity + w->speed) * (1.0f - r->wheelDrag);
        w->vel[0] = fr.right.x * w->speed;
        w->vel[1] = fr.right.y * w->speed;
        w->vel[2] = fr.right.z * w->speed;
        if (!rear || riderGetState(r, 0) != 0 || riderGetState(r, 1) != 2) {
            w->contact = 0;
            switch (w->material) {
            case 2:                           grp = 2; break;   /* wood */
            case 5: case 6: case 10: case 11: grp = 4; break;   /* dirt / grass */
            default:                          grp = 0; break;
            }
            zmathVecScale(V3(w->vel), V3(w->vel), 1.0f - g_bmxphysRollDrag[grp]);
        } else {
            /* rear wheel locked (rider state 0/2: skid): marks or dirt, skid length for the records */
            w->spin = 0;
            if (w->speed > 5.0f) {
                switch (w->material) {
                case 0: case 1: case 2: case 4:
                    t.x = hit.pos[0] + w->vel[0];
                    t.y = hit.pos[1] + w->vel[1];
                    t.z = hit.pos[2] + w->vel[2];
                    fxSkid(V3(hit.pos), &t, 0, V3(hit.normal));
                    break;
                case 5: case 9: case 10:
                    vecSet(&t.x, (float)(zmathRandom() & 0x1F), 0.0f, 0.0f);
                    zmathVecRotateY(&t, &t, zmathRandom());
                    t.x += hit.pos[0];
                    t.y += hit.pos[1];
                    t.z += hit.pos[2];
                    vecSet(&d.x, w->vel[0] * 0.5f, w->vel[1] * 0.5f, w->vel[2] * 0.5f);
                    fxDust(&t, &d);
                    break;
                }
                r->skidLength += w->speed;
            }
            w->contact = 1;
        }
    }

    /* -- anything between the hubs (the frame) ends the ride */
    for (i = 0; i < 2; i++) {
        next[i].x = bm->up.x * 20.0f + hub[i].x;
        next[i].y = bm->up.y * 20.0f + hub[i].y;
        next[i].z = bm->up.z * 20.0f + hub[i].z;
    }
    if (zcolSegmentTest(&next[0], &next[1], &hit)) {
        bmxphysSetState(r, BMXPHYS_CRASH);
        r->crashKind = 5;
        return;
    }

    /* -- new bike frame: forward = front hub - rear hub, up = the ground normals */
    zmathVecZero(&m.pos);
    zmathVecSub(&m.right, &hub[0], &hub[1]);
    if (m.right.x == 0.0f && m.right.y == 0.0f && m.right.z == 0.0f)
        zAssertFail("all components of X orientation are zero", PHYSICS_SRC, 0xd65, 0);
    zmathVecNormalize(&m.right, &m.right);
    if (airborne < 2) {
        if (airborne == 0) {
            m.up.x = (r->wheel[1].frame[4] - r->wheel[0].frame[4]) * 0.5f + r->wheel[0].frame[4];
            m.up.y = (r->wheel[1].frame[5] - r->wheel[0].frame[5]) * 0.5f + r->wheel[0].frame[5];
            m.up.z = (r->wheel[1].frame[6] - r->wheel[0].frame[6]) * 0.5f + r->wheel[0].frame[6];
        } else {
            for (i = 0; i < 2; i++) {
                if (r->wheel[i].contact != 2) {
                    vecCopy(&m.up.x, &r->wheel[i].frame[4]);
                    if (zmathAcos(-r->wheel[i].frame[5]) < 0x238E)
                        r->stateFlags |= RIDER_FLAG_CAN_MANUAL;
                    else
                        r->stateFlags &= ~RIDER_FLAG_CAN_MANUAL;
                    break;
                }
            }
        }
        if (r->rotVel[1] != 0 && r->frameDist > g_bmxphysSteerSpeed) {
            /* lean into the turn: up to 45 deg, more with speed and yaw rate */
            zmathMatrixIdentity(&rot);
            f = ((r->frameDist - g_bmxphysSteerSpeed) / (g_bmxphysMaxRollSpeed - g_bmxphysSteerSpeed)) *
                ((float)r->rotVel[1] * 4.5f / (float)(int16_t)(int32_t)(75.0f * 182.04445f));
            if (f > 1.0f)
                f = 1.0f;
            zmathMatrixRotateX(&rot, &rot, -(int)(f * g_bmxphysMaxLeanDeg * 182.04445f));
            zmathMatrixMultiply(&m, &m, &rot);
        }
        /* the bike's up moves 15 % of the way to the new ground normal */
        m.up.x = (m.up.x - bm->up.x) * g_bmxphysOrientBlend + bm->up.x;
        m.up.y = (m.up.y - bm->up.y) * g_bmxphysOrientBlend + bm->up.y;
        m.up.z = (m.up.z - bm->up.z) * g_bmxphysOrientBlend + bm->up.z;
    } else {
        m.up = bm->up;
    }
    zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
    if (m.at.x == 0.0f && m.at.y == 0.0f && m.at.z == 0.0f)
        zAssertFail("all components of Z orientation are zero", PHYSICS_SRC, 0xd8d, 0);
    zmathVecNormalize(&m.at, &m.at);
    zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
    if (m.up.x == 0.0f && m.up.y == 0.0f && m.up.z == 0.0f)
        zAssertFail("all components of Y orientation are zero", PHYSICS_SRC, 0xd91, 0);
    zmathVecNormalize(&m.up, &m.up);
    zmathMatrixCopy(bm, &m);
    zmathMatrixCopy(RMAT(r), bm);

    /* bike position: midpoint of where each hub puts it */
    zmathTransformPoint(&t, &m, V3(r->wheel[0].pos));
    zmathVecSub(&c0, &hub[0], &t);
    zmathTransformPoint(&t, &m, V3(r->wheel[1].pos));
    zmathVecSub(&c1, &hub[1], &t);
    r->spawnPos[0] = (c1.x - c0.x) * 0.5f + c0.x;
    r->spawnPos[1] = (c1.y - c0.y) * 0.5f + c0.y;
    r->spawnPos[2] = (c1.z - c0.z) * 0.5f + c0.z;
    vecCopy(r->pos, r->spawnPos);

    /* -- wheel velocities from where the hubs go next frame (rigid wheelbase) */
    for (i = 0; i < 2; i++)
        zmathVecAdd(&next[i], &hub[i], V3(r->wheel[i].vel));
    zmathVecSub(&m.right, &next[0], &next[1]);
    zmathVecNormalize(&m.right, &m.right);
    zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
    zmathVecNormalize(&m.at, &m.at);
    zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
    zmathTransformPoint(&t, &m, V3(r->wheel[0].pos));
    zmathVecSub(&c0, &next[0], &t);
    zmathTransformPoint(&t, &m, V3(r->wheel[1].pos));
    zmathVecSub(&c1, &next[1], &t);
    vecCopy(r->physSlopeAccel, r->vel);              /* old velocity, for the acceleration below */
    centre.x = (c1.x - c0.x) * 0.5f + c0.x;
    centre.y = (c1.y - c0.y) * 0.5f + c0.y;
    centre.z = (c1.z - c0.z) * 0.5f + c0.z;
    zmathVecZero(V3(r->vel));
    for (i = 0; i < 2; i++) {
        w = &r->wheel[i];
        zmathTransformPoint(&t, &m, V3(w->pos));
        zmathVecAdd(&t, &t, &centre);
        zmathVecSub(V3(w->vel), &t, &hub[i]);
        w->speed = zmathVecLength(V3(w->vel));
        if (w->speed < r->wheelDrag * 30.0f + g_bmxphysHalf) {
            zmathVecZero(V3(w->vel));
            w->speed = 0.0f;
        }
        if (w->contact == 0) {
            /* rolling: spin = distance / circumference (2 pi 25) in binary angle */
            w->spin = (int16_t)(int32_t)(w->speed * 0.006366203f * 65536.0f);
            if (zmathVecDot(&WFRAME(w)->right, V3(w->vel)) < 0.0f)
                w->spin = -w->spin;
        }
        zmathVecAdd(V3(r->vel), V3(r->vel), V3(w->vel));
    }
    zmathVecScale(V3(r->vel), V3(r->vel), 0.5f);
    r->frameDist = zmathVecLength(V3(r->vel));
    if (r->frameDist < r->wheelDrag * 30.0f + g_bmxphysHalf) {
        zmathVecZero(V3(r->vel));                    /* stopped */
        r->frameDist = 0.0f;
        for (i = 0; i < 2; i++) {
            if (r->wheel[i].contact != 2) {
                zmathVecZero(V3(r->wheel[i].vel));
                r->wheel[i].speed = 0.0f;
            }
        }
    }
    r->physSlopeAccel[0] = r->vel[0] - r->physSlopeAccel[0];
    r->physSlopeAccel[1] = r->vel[1] - r->physSlopeAccel[1];
    r->physSlopeAccel[2] = r->vel[2] - r->physSlopeAccel[2];
    if (zmathVecDot(V3(r->vel), &bm->right) >= 0.0f)
        r->stateFlags &= ~RIDER_FLAG_TURNAROUND;
    else
        r->stateFlags |= RIDER_FLAG_TURNAROUND;
    if (airborne == 2)
        bmxphysSetState(r, BMXPHYS_AIR);
}

/* VA 0x00428df0, 6892 bytes, [V] direct (new function, position table [1]), status draft */
void bmxphysUpdatePositionAir(BmxRider *r)
{
    /* Flight. Each wheel probes along -up for a landing: a hit within 78 deg of the bike's up
     * (and not a vertical face) compresses the suspension, may crash the rider (impact >= 85 x
     * stat 4, or sideways speed >= 35), and sets the landing surface (front wheel). With both
     * wheels still in the air, eight corners of a 100 x 80 x 80 box around the rider probe for
     * walls (bounce + turn away, or crash), then the rider moves and the ground under the hips
     * gives wallHeight. Once a wheel is down: a trick not finished = bail, else the landing frame
     * is built like the roll frame and the state becomes ROLL (or CRASH when more than 78 deg off). */
    RwMatrix lean, m, rot, tgt;
    BmxWheel *w;
    NgColHit hit;
    RwV3d hub[2], next[2], q, n, t, d, old, ctr, cv, c0, c1, centre;
    float travel, dt, f, damp, impact, s, k, push;
    uint32_t i, airborne = 0, corner, mat, sound, st;
    int cls;
    NgColEdge *e;
    void *obj;

    zmathMatrixCopy(&lean, RMAT(r));
    zmathMatrixIdentity(&rot);
    zmathMatrixRotateZ(&rot, &rot, r->airLean);
    zmathMatrixMultiply(&lean, &lean, &rot);

    for (i = 0; i < 2; i++) {
        w = &r->wheel[i];
        vecCopy(w->vel, r->vel);
        zmathTransformPoint(&hub[i], &lean, V3(w->pos));
        zmathVecAdd(&hub[i], &hub[i], V3(r->spawnPos));
        travel = w->travel + w->travelVel;
        if (travel < r->suspension[0]) {
            dt = r->suspension[0] - w->travel;
            travel = r->suspension[0];
            w->travelVel = 0.0f;
        } else if (travel > r->suspension[1]) {
            dt = r->suspension[1] - w->travel;
            travel = r->suspension[1];
            w->travelVel = 0.0f;
        } else {
            dt = w->travelVel;
        }
        w->travel = travel;
        s = travel + w->radius;
        q.x = (hub[i].x + w->vel[0]) - lean.up.x * s;
        q.y = (hub[i].y + w->vel[1]) - lean.up.y * s;
        q.z = (hub[i].z + w->vel[2]) - lean.up.z * s;
        if (!zcolSegmentTest(&hub[i], &q, &hit) || hit.normal[1] == 0.0f ||
            zmathAcos(zmathVecDot(&lean.up, V3(hit.normal))) >= g_bmxphysLipAngle)
            goto in_air;
        vecCopy(&n.x, hit.normal);
        vecCopy(&w->frame[4], &n.x);
        t.x = -(lean.up.x * w->travelVel) + w->vel[0];
        t.y = -(lean.up.y * w->travelVel) + w->vel[1];
        t.z = -(lean.up.z * w->travelVel) + w->vel[2];
        impact = -zmathVecDot(&t, &n);
        if (impact <= 0.0f)
            continue;                                   /* moving away from it */
        if (!(hit.type & 1)) {
            if ((hit.type & 2) && *(void **)((uint8_t *)hit.propObj + 0x1EC) &&
                bmxpropTouched(r->slot, *(void **)((uint8_t *)hit.propObj + 0x1EC)) == 0)
                goto in_air;                            /* the prop gave way */
        } else {
            cls = hit.face->code >> 12;
            if (cls == 9) {                             /* trigger face: not solid */
                bmxpropHitTrigger(r->slot, hit.face);
                goto in_air;
            }
            if (cls == 10) {                            /* launch pad: id = speed along the normal */
                s = zmathVecDot(V3(r->vel), &n);
                r->vel[0] -= s * n.x;
                r->vel[1] -= s * n.y;
                r->vel[2] -= s * n.z;
                s = (float)(int16_t)(hit.face->code & 0xFFF);
                r->vel[0] += s * n.x;
                r->vel[1] += s * n.y;
                r->vel[2] += s * n.z;
                r->frameDist = zmathVecLength(V3(r->vel));
                bmxsoundEvent(0xBB9, r->pos, (uint32_t)r->slot << 28 | 0x109, 0x7F, 0);
                return;
            }
        }
        if (i == 0)
            bmxphysSetSurfaceFromHit(r, &hit);          /* the front wheel decides the landing surface */
        mat = 0;
        if (hit.type & 1) {
            if (hit.material)
                mat = *hit.material & 0xF;
        } else if (hit.type & 2) {
            uint8_t *info = (uint8_t *)zbspGetPropShape(hit.propId);
            if (zcolGetPropShape(hit.propId))
                mat = info[2];
        }
        switch (mat) {
        case 1: case 3: sound = 7; break;
        case 2:         sound = 2; break;
        case 4:         sound = 5; break;
        case 5: case 9: sound = 4; break;
        case 6:         goto hazard;
        case 8:         sound = 0; break;
        default:        sound = 0; break;
        }
        if (impact > 10.0f) {
            st = (uint32_t)(int)(impact + impact);
            bmxsoundEvent(0xBBA, hit.pos, (uint32_t)r->slot << 28 | sound | 0x100, (int)st > 0x7E ? 0x7F : (int)st, 0);
        }
        if (impact >= 85.0f * r->stats[4]) {            /* 0x00557820: landed too hard */
            bmxphysSetState(r, BMXPHYS_CRASH);
            r->crashKind = 2;
            riderAddCrashDamage(r, 0, impact);
            goto knock_prop;
        }
        if (impact > 20.0f)
            r->stateFlags |= RIDER_FLAG_LAND_ALT;
        else
            r->stateFlags &= ~RIDER_FLAG_LAND_ALT;
        t.x = hit.pos[0] - hub[i].x;
        t.y = hit.pos[1] - hub[i].y;
        t.z = hit.pos[2] - hub[i].z;
        travel = -zmathVecDot(&t, &lean.up) - w->radius;
        dt = w->travelVel;
        if (travel > r->suspension[1])
            travel = r->suspension[1];
        else if (travel < r->suspension[0])
            travel = r->suspension[0];
        k = r->tuneC6C * 0.5f;
        f = ((r->suspension[1] - travel) * r->suspension[2] + zmathVecDot(&n, &lean.up) * impact * k) / k + dt;
        damp = f >= 0.0f ? r->suspension[4] : r->suspension[3];
        w->travelVel = (1.0f - damp) * f;
        w->travel = travel;
        old.x = hub[i].x + w->vel[0];
        old.y = hub[i].y + w->vel[1];
        old.z = hub[i].z + w->vel[2];
        s = travel + w->radius;
        hub[i].x = s * lean.up.x + hit.pos[0];
        hub[i].y = s * lean.up.y + hit.pos[1];
        hub[i].z = s * lean.up.z + hit.pos[2];
        zmathVecSub(&d, &old, &hub[i]);
        s = zmathVecDot(&d, &n);
        vecCopy(&w->frame[4], &n.x);
        vecCopy(&w->frame[0], &lean.right.x);
        hub[i].x += d.x - s * n.x;
        hub[i].y += d.y - s * n.y;
        hub[i].z += d.z - s * n.z;
        zmathTriangleNormal(&WFRAME(w)->at, &WFRAME(w)->right, &g_zmathOrigin, &WFRAME(w)->up);
        zmathVecNormalize(&WFRAME(w)->at, &WFRAME(w)->at);
        zmathTriangleNormal(&WFRAME(w)->right, &WFRAME(w)->up, &g_zmathOrigin, &WFRAME(w)->at);
        if (fabs(zmathVecDot(&WFRAME(w)->at, V3(w->vel))) >= 35.0f) {   /* 0x00557824: sideways */
            bmxphysSetState(r, BMXPHYS_CRASH);
            r->crashKind = 4;
            return;
        }
        w->contact = 0;
        s = zmathVecDot(&WFRAME(w)->right, V3(w->vel));
        w->vel[0] = s * w->frame[0];
        w->vel[1] = s * w->frame[1];
        w->vel[2] = s * w->frame[2];
        w->speed = zmathVecLength(V3(w->vel));
        w->spin = (int16_t)(int32_t)(w->speed * 0.006366203f * 65536.0f);
        if (zmathVecDot(V3(w->vel), &WFRAME(w)->right) < 0.0f)
            w->spin = -w->spin;
        if (!(hit.type & 1) || !hit.face) {
            if ((hit.type & 2) && hit.propObj) {
                r->surface = BMXSURF_PROP;
                r->surfaceObj = hit.propObj;
                bmxphysCopySurface(r);
            }
        } else if ((cls = hit.face->code >> 12) != 0 && cls < 4) {
            e = bmxphysFindTaggedEdge(hit.unk_04, hit.face->code & 0xFFF, V3(r->pos));
            obj = e;
            if (!e) {
                g_pfnDebugPrintf("bmxphysUpdatePositionAir: quarterpipe face with no assigned edge, ID=%d\n",
                                 hit.face->code & 0xFFF);
                cls = 0;
                obj = hit.face;
            }
            r->surface = (uint8_t)cls;
            r->surfaceObj = obj;
            bmxphysCopySurface(r);
        }
        continue;
in_air:
        hub[i].x += w->vel[0];
        hub[i].y += w->vel[1];
        hub[i].z += w->vel[2];
        f = ((r->suspension[1] - travel) * r->suspension[2]) / r->tuneC70 + dt;
        damp = f >= 0.0f ? r->suspension[4] : r->suspension[3];
        w->contact = 2;
        w->travelVel = (1.0f - damp) * f;
        w->spin = (int16_t)(int32_t)(w->spin * 0.99f * 65536.0f);              /* [V] as compiled */
        airborne++;
    }

    if (airborne > 1) {
        /* still flying: probe the leading corners of the body box against walls */
        t.x = r->spawnPos[0] - (r->physHipOffset[0] + r->pos[0]);
        t.y = r->spawnPos[1] - (r->physHipOffset[1] + r->pos[1]);
        t.z = r->spawnPos[2] - (r->pos[2] + r->physHipOffset[2]);
        s = zmathVecDot(&t, &lean.right);
        ctr.x = lean.right.x * s + r->physHipOffset[0] + r->pos[0];
        ctr.y = lean.right.y * s + r->physHipOffset[1] + r->pos[1];
        ctr.z = lean.right.z * s + r->pos[2] + r->physHipOffset[2];
        for (corner = 0; corner < 8; corner++) {
            t.x = (corner & 2) ? -50.0f : 50.0f;            /* 0x0055783C */
            t.y = (corner & 4) ? -40.0f : 40.0f;            /* 0x00557840 */
            t.z = (corner & 1) ? -g_bmxphysWheelProbe : g_bmxphysWheelProbe;
            zmathTransformPoint(&cv, &lean, &t);
            if (zmathVecDot(&cv, V3(r->vel)) < 0.0f)
                continue;                                   /* trailing corner */
            zmathVecAdd(&q, &cv, &ctr);
            zmathVecAdd(&q, &q, V3(r->vel));
            if (!zcolSegmentTest(&ctr, &q, &hit))
                continue;
            vecCopy(&n.x, hit.normal);
            impact = -zmathVecDot(V3(r->vel), &n);
            if (impact <= 0.0f)
                continue;
            if (!(hit.type & 1) || !hit.face) {
                if ((hit.type & 2) && *(void **)((uint8_t *)hit.propObj + 0x1EC) &&
                    bmxpropTouched(r->slot, *(void **)((uint8_t *)hit.propObj + 0x1EC)) == 0)
                    continue;
                /* [V] the exe's prop-class hazard test sits in an unreachable branch here */
            } else if ((hit.face->code & 0xF000) == 0x9000) {
                bmxpropHitTrigger(r->slot, hit.face);
                continue;
            } else if (hit.material && (*hit.material & 0xF) == 6) {
                goto hazard;
            }
            if (impact >= g_bmxphysCrashImpact * r->stats[4]) {
                bmxphysSetState(r, BMXPHYS_CRASH);
                r->crashKind = (uint8_t)((zmathAcos(-n.y) < 0x2AAA) + 1);   /* 2 = onto a floor, 1 = a wall */
                riderAddCrashDamage(r, 0, impact);
                goto knock_prop;
            }
            if (zmathAcos(-n.y) < 0x2AAB)
                goto crash_body;                              /* landed on the body */
            /* a wall: stay 10 off it, bounce, and turn along it over 15 frames */
            t.x = hit.pos[0] - ctr.x;
            t.y = hit.pos[1] - ctr.y;
            t.z = hit.pos[2] - ctr.z;
            push = zmathVecDot(&t, &n) + (10.0f - zmathVecDot(&cv, &n));
            r->pos[0] += n.x * push;
            r->pos[1] += n.y * push;
            r->pos[2] += n.z * push;
            r->spawnPos[0] += n.x * push;
            r->spawnPos[1] += n.y * push;
            r->spawnPos[2] += n.z * push;
            k = impact * 1.25f;
            vecSet(r->physSlopeAccel, n.x * k, n.y * k, n.z * k);
            zmathVecAdd(V3(r->vel), V3(r->vel), V3(r->physSlopeAccel));
            r->frameDist = zmathVecLength(V3(r->vel));
            s = zmathVecDot(&RMAT(r)->right, &n);
            if (zmathAcos((float)fabs(s)) > 0x1554) {
                s *= 1.25f;
                tgt.up = RMAT(r)->up;
                tgt.right.x = RMAT(r)->right.x - n.x * s;
                tgt.right.y = RMAT(r)->right.y - n.y * s;
                tgt.right.z = RMAT(r)->right.z - n.z * s;
                if (r->physTimer != 0 && !(r->stateFlags & RIDER_FLAG_CARRY_SPIN))
                    tgt.up = g_bmxphysUp;
                zmathTriangleNormal(&tgt.at, &tgt.right, &g_zmathOrigin, &tgt.up);
                zmathVecNormalize(&tgt.at, &tgt.at);
                zmathTriangleNormal(&tgt.right, &tgt.up, &g_zmathOrigin, &tgt.at);
                bmxphysMatrixDeltaAngles(RMAT(r), &tgt, g_bmxphysAutoRotFrames, r->physAirRot);
                zmathMatrixCopy(MAT(r->physAirMatrix), RMAT(r));
                r->physTimer = (int16_t)g_bmxphysAutoRotFrames;
                r->stateFlags &= ~RIDER_FLAG_CARRY_SPIN;
            }
            break;
        }
        zmathVecAdd(V3(r->pos), V3(r->pos), V3(r->vel));
        /* the ground under the hips: surface and height */
        old.x = r->physHipOffset[0] + r->pos[0];
        old.y = r->physHipOffset[1] + r->pos[1];
        old.z = r->pos[2] + r->physHipOffset[2];
        vecSet(&q.x, old.x, 15535.0f, old.z);
        if (!zcolSegmentTest(&old, &q, &hit) || !bmxphysSetSurfaceFromHit(r, &hit)) {
            g_pfnDebugPrintf("bmxphysUpdatePositionAir: no valid terrain found under rider's hips (pos X: %d, Z: %d)\n",
                             (int)q.x, (int)q.z);
            r->stateFlags &= ~RIDER_FLAG_FORWARD_HOP;
            return;
        }
        r->wallHeight = hit.pos[1] - old.y;
        return;
    }

    /* a wheel is down: not out of the trick yet = bail */
    if (riderGetState(r, 0) == 1) {
        st = riderGetState(r, 1);
        if ((st == 1 && riderGetState(r, 2) < 3 && (g_trickFlags[r->trickId] & 7) == 0) ||
            (st == 2 && riderGetState(r, 2) < 3)) {
            bmxphysSetState(r, BMXPHYS_CRASH);
            if (r->trickCount < 2 && (riderGetState(r, 2) != 2 || !(r->stateFlags & 0x100)))
                r->crashKind = 7;
            else
                r->crashKind = 8;
            return;
        }
    }

    /* landing frame */
    zmathVecZero(&m.pos);
    zmathVecSub(&m.right, &hub[0], &hub[1]);
    if (m.right.x == 0.0f && m.right.y == 0.0f && m.right.z == 0.0f)
        zAssertFail("all components of X orientation are zero", PHYSICS_SRC, 0xf82, 0);
    zmathVecNormalize(&m.right, &m.right);
    if (airborne == 0) {
        m.up.x = (r->wheel[1].frame[4] - r->wheel[0].frame[4]) * 0.5f + r->wheel[0].frame[4];
        m.up.y = (r->wheel[1].frame[5] - r->wheel[0].frame[5]) * 0.5f + r->wheel[0].frame[5];
        m.up.z = (r->wheel[1].frame[6] - r->wheel[0].frame[6]) * 0.5f + r->wheel[0].frame[6];
    } else {
        for (i = 0; i < 2; i++) {
            if (r->wheel[i].contact != 2) {
                vecCopy(&m.up.x, &r->wheel[i].frame[4]);
                if (zmathAcos(-r->wheel[i].frame[5]) < 0x238E)
                    r->stateFlags |= RIDER_FLAG_CAN_MANUAL;
                else
                    r->stateFlags &= ~RIDER_FLAG_CAN_MANUAL;
            }
        }
    }
    m.up.x = (m.up.x - lean.up.x) * g_bmxphysOrientBlend + lean.up.x;
    m.up.y = (m.up.y - lean.up.y) * g_bmxphysOrientBlend + lean.up.y;
    m.up.z = (m.up.z - lean.up.z) * g_bmxphysOrientBlend + lean.up.z;
    zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
    if (m.at.x == 0.0f && m.at.y == 0.0f && m.at.z == 0.0f)
        zAssertFail("all components of Z orientation are zero", PHYSICS_SRC, 0xf9a, 0);
    zmathVecNormalize(&m.at, &m.at);
    zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
    if (m.up.x == 0.0f && m.up.y == 0.0f && m.up.z == 0.0f)
        zAssertFail("all components of Y orientation are zero", PHYSICS_SRC, 0xf9e, 0);
    zmathVecNormalize(&m.up, &m.up);
    if (fabs((float)zmathAcos(zmathVecDot(&m.up, &lean.up))) >= (float)g_bmxphysLipAngle)
        goto crash_body;                                  /* more than 78 deg off: crash */

    zmathMatrixCopy(BMAT(r), &m);
    zmathMatrixCopy(RMAT(r), &m);
    zmathTransformPoint(&t, &m, V3(r->wheel[0].pos));
    zmathVecSub(&c0, &hub[0], &t);
    zmathTransformPoint(&t, &m, V3(r->wheel[1].pos));
    zmathVecSub(&c1, &hub[1], &t);
    r->spawnPos[0] = (c1.x - c0.x) * 0.5f + c0.x;
    r->spawnPos[1] = (c1.y - c0.y) * 0.5f + c0.y;
    r->spawnPos[2] = (c1.z - c0.z) * 0.5f + c0.z;
    vecCopy(r->pos, r->spawnPos);
    for (i = 0; i < 2; i++)
        zmathVecAdd(&next[i], &hub[i], V3(r->wheel[i].vel));
    zmathVecSub(&m.right, &next[0], &next[1]);
    zmathVecNormalize(&m.right, &m.right);
    zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
    zmathVecNormalize(&m.at, &m.at);
    zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
    zmathTransformPoint(&t, &m, V3(r->wheel[0].pos));
    zmathVecSub(&c0, &next[0], &t);
    zmathTransformPoint(&t, &m, V3(r->wheel[1].pos));
    zmathVecSub(&c1, &next[1], &t);
    vecCopy(r->physSlopeAccel, r->vel);
    centre.x = (c1.x - c0.x) * 0.5f + c0.x;
    centre.y = (c1.y - c0.y) * 0.5f + c0.y;
    centre.z = (c1.z - c0.z) * 0.5f + c0.z;
    zmathVecZero(V3(r->vel));
    for (i = 0; i < 2; i++) {
        w = &r->wheel[i];
        zmathTransformPoint(&t, &m, V3(w->pos));
        zmathVecAdd(&t, &t, &centre);
        zmathVecSub(V3(w->vel), &t, &hub[i]);
        if (w->contact != 2) {                            /* keep it on the ground plane */
            s = zmathVecDot(V3(w->vel), &WFRAME(w)->up);
            w->vel[0] -= s * w->frame[4];
            w->vel[1] -= s * w->frame[5];
            w->vel[2] -= s * w->frame[6];
        }
        zmathVecAdd(V3(r->vel), V3(r->vel), V3(w->vel));
    }
    zmathVecScale(V3(r->vel), V3(r->vel), 0.5f);
    r->frameDist = zmathVecLength(V3(r->vel));
    r->physSlopeAccel[0] = r->vel[0] - r->physSlopeAccel[0];
    r->physSlopeAccel[1] = r->vel[1] - r->physSlopeAccel[1];
    r->physSlopeAccel[2] = r->vel[2] - r->physSlopeAccel[2];
    if (zmathVecDot(V3(r->vel), &BMAT(r)->right) < 0.0f)
        r->stateFlags |= RIDER_FLAG_TURNAROUND;
    else
        r->stateFlags &= ~RIDER_FLAG_TURNAROUND;
    bmxphysSetState(r, BMXPHYS_ROLL);
    return;

hazard:
    bmxphysSetState(r, BMXPHYS_CRASH);
    r->crashKind = 10;
    r->surface = BMXSURF_HAZARD_D;
    contrlerRumble(r->slot, 1, 0xFF);
    return;
knock_prop:
    if (hit.type & 2)
        zbspPropSetMode(hit.propObj, 0);
    return;
crash_body:
    bmxphysSetState(r, BMXPHYS_CRASH);
    r->crashKind = 5;
}

/* VA 0x0042afd0, 158 bytes, [I] row, status draft */
void bmxphysUpdatePositionLockOn(BmxRider *r)
{
    int i;

    for (i = 0; i < 2; i++)     /* [V] as compiled: keeps the low 16 bits of spin * 0.99 * 65536 */
        r->wheel[i].spin = (int16_t)(int32_t)(r->wheel[i].spin * 0.99f * 65536.0f);
    if (--r->physTimer == 0) {
        if (bmxphysGetState(r) == BMXPHYS_GRIND_LOCK)
            bmxphysSetState(r, BMXPHYS_GRIND);
        else if (bmxphysGetState(r) == BMXPHYS_STALL_LOCK)
            bmxphysSetState(r, BMXPHYS_STALL);
    } else {
        zmathVecAdd(V3(r->spawnPos), V3(r->spawnPos), V3(r->physLockStep));
        zmathVecAdd(V3(r->pos), V3(r->pos), V3(r->physLockStep));
    }
}

/* VA 0x0042b0a0, 668 bytes, [I] row, status draft */
void bmxphysUpdatePositionGrind(BmxRider *r)
{
    NgColEdge *e = (NgColEdge *)r->surfaceObj, *next;
    RwMatrix *m = RMAT(r);
    RwV3d d, closest;
    int a0[3], a1[3], k;

    r->wheel[0].spin = (int16_t)(int32_t)(r->wheel[0].spin * 0.99f * 65536.0f);   /* [V] as compiled */
    r->wheel[1].spin = 0;
    for (k = 0; k < 3; k++) {
        a0[k] = (int)(e->v0[k] - r->pos[k]);
        a1[k] = (int)(e->v1[k] - r->pos[k]);
    }
    if (a1[2] * a0[2] + a1[1] * a0[1] + a1[0] * a0[0] < 1) {
        /* still between the two ends: turn 15 % per frame towards the edge direction */
        vecCopy(&d.x, e->dir);
        if (zmathVecDot(&m->right, &d) < 0.0f)
            zmathVecNegate(&d, &d);
        blendRow(&m->right, &d);
        zmathTriangleNormal(&m->at, &m->right, &g_zmathOrigin, &m->up);
        zmathVecNormalize(&m->at, &m->at);
        zmathTriangleNormal(&m->up, &m->at, &g_zmathOrigin, &m->right);
    } else {
        /* past an end: continue on a connected edge (within 30, less than 75 deg off), else hop off */
        next = bmxphysFindNearestEdge(r, V3(r->pos), g_bmxphysEdgeRadius, &closest);
        if (next && zmathAcos((float)fabs(zmathVecDot(V3(next->dir), V3(((NgColEdge *)r->surfaceObj)->dir)))) < 0x3555) {
            bmxphysSnapToEdge(r, next);
        } else {
            d = m->up;
            zmathVecScale(&d, &d, 7.5f);
            zmathVecAdd(V3(r->vel), V3(r->vel), &d);
            bmxphysSetState(r, BMXPHYS_AIR);
        }
    }
    zmathVecAdd(V3(r->pos), V3(r->pos), V3(r->vel));
    zmathVecAdd(V3(r->spawnPos), V3(r->spawnPos), V3(r->vel));
}

/* VA 0x0042b3f0, 293 bytes, [I] row, status draft */
NgColEdge *bmxphysFindNearestEdge(BmxRider *r, const RwV3d *pos, float radius, RwV3d *closest)
{
    /* nearest edge within radius, except the one being ground on */
    NgColEdge **list, *e, *best = 0;
    RwV3d c;
    uint32_t bestD = 0xFFFFFFFFu, d;

    list = zbspFindEdgesNear(pos, radius);
    if (!list || !*list)
        return 0;
    for (; (e = *list) != 0; list++) {
        if (bmxphysGetState(r) == BMXPHYS_GRIND && r->surface == BMXSURF_EDGE &&
            bmxphysEdgeEquals(e, (NgColEdge *)r->surfaceObj))
            continue;
        bmxphysClosestPointOnEdge(pos, e, &c);
        d = idist2(&c.x, &pos->x);
        if (d < bestD) {
            *closest = c;
            bestD = d;
            best = e;
        }
    }
    if (!best)
        return 0;
    zstringMemCopy(&g_bmxphysNearestEdge, best, sizeof(NgColEdge));
    return &g_bmxphysNearestEdge;
}

/* VA 0x0042b570, 77 bytes, [I] row, status draft */
int bmxphysEdgeEquals(const NgColEdge *a, const NgColEdge *b)
{
    return a->v0[0] == b->v0[0] && a->v0[1] == b->v0[1] && a->v0[2] == b->v0[2] &&
           a->v1[0] == b->v1[0] && a->v1[1] == b->v1[1] && a->v1[2] == b->v1[2];
}

/* VA 0x0042b5e0, 457 bytes, [I] row, status draft */
void bmxphysUpdatePositionCrash(BmxRider *r)
{
    /* the ragdoll controllers move rider and bike; physics copies their state back */
    uint8_t *ctl;               /* DynBody (dyn.h): +8 particle[0].frame, +0x88 particle[0].vel, +0xE70 centre */
    uint32_t *dst, *src;
    int i;

    for (i = 0; i < 2; i++)     /* [V] as compiled */
        r->wheel[i].spin = (int16_t)(int32_t)(r->wheel[i].spin * 0.99f * 65536.0f);
    if (r->surface != BMXSURF_HAZARD_D) {
        dynStepRider(r->animCtl, r->riderActor);
        r->motion[1].frame = zactorPlaySecondMotion(r->riderActor, g_motionTable[r->motion[1].id],
                                              (int16_t)r->motion[1].flags, (float *)r->cleared_bc0);
        dst = (uint32_t *)(ACTOR_POSE(r->riderActor) + 0x104);    /* pose <- rag-doll pose (block 1 -> block 0) */
        src = (uint32_t *)(ACTOR_POSE(r->riderActor) + 0x280);
        for (i = 0; i < 30; i++)
            dst[i] = src[i];
        ctl = (uint8_t *)r->animCtl;
        zmathMatrixCopy(RMAT(r), MAT(ctl + 8));
        vecCopy(r->pos, (float *)(ctl + 0xE70));
        zmathVecZero(V3(r->physHipOffset));
        vecCopy(r->vel, (float *)(ctl + 0x88));
        r->frameDist = zmathVecLength(V3(r->vel));
    }
    dynStepBike(r->bikeCtl, r->bikeActor);
    ctl = (uint8_t *)r->bikeCtl;
    vecCopy(r->spawnPos, (float *)(ctl + 0xE70));
    zmathMatrixCopy(BMAT(r), MAT(ctl + 8));
    bmxsoundEvent(4000, r->spawnPos, (uint32_t)r->slot << 28 | 0x103, 0,
                      clampSoundVolume((int)(r->wheel[1].spin * 423.3333f)));
}

/* VA 0x0042b830, 379 bytes, [I] row, status draft */
void bmxphysPostRoll(BmxRider *r)
{
    /* rolling sound: the surface of the first wheel on the ground selects the sample */
    uint32_t material = 0, sound = 0, manual = 0, wheelie = 0;
    int i, pitch, volume;

    bmxphysPlaceActorsRoll(r);
    for (i = 0; i < 2; i++) {
        if (r->wheel[i].contact != 2) {
            material = r->wheel[i].material;
            break;
        }
    }
    if (material <= 10) {
        switch (material) {
        case 1: case 3: sound = 7; break;      /* metal */
        case 2:         sound = 2; break;      /* wood */
        case 4:         sound = 5; break;
        case 5:         sound = 4; break;      /* dirt */
        case 6:         sound = 6; break;
        case 7: case 8: case 9: break;         /* keep 0 */
        case 10:        sound = 1; break;
        default:        sound = 0; break;
        }
    }
    pitch = clampSoundVolume((int)(r->wheel[1].spin * 423.3333f));
    if (riderGetState(r, 0) == 0 && riderGetState(r, 1) == 2)
        manual = 1;
    else if (riderGetState(r, 0) == 0 && riderGetState(r, 1) == 1)
        wheelie = 1;
    else if (riderGetState(r, 0) == 0 && riderGetState(r, 1) == 4)
        pitch = 0x6E;
    volume = (int)(r->frameDist * 3.0f);
    if (volume > 0x7E)
        volume = 0x7F;
    bmxsoundEvent(4000, r->spawnPos,
                      ((((uint32_t)r->slot << 4 | manual) << 8 | wheelie) << 16) | sound | 0x100, volume, pitch);
}

/* VA 0x0042ba50, 2684 bytes, [I] row (new function, post table [1], [3], [5]), status draft */
void bmxphysPostAir(BmxRider *r)
{
    /* place the actors (rider leaned by airLean) and pose the trick modifier: for each bit of
     * g_modLimbMask[modId] move a hand / foot (IK limb 1, 2, 4, 8) to an offset or a bike bone,
     * or spin / wiggle the handlebars; RIDER_TRICK_BLEND fades the pose in */
    RwMatrix lean, rot;
    RwV3d t, target, off;
    uint32_t bits, bit, b, used = 0;
    float blend = r->modBlend, k;
    int bone, forearm, limb, pitch, volume;

    zmathMatrixCopy(&lean, RMAT(r));
    zmathMatrixIdentity(&rot);
    zmathMatrixRotateZ(&rot, &rot, r->airLean);
    zmathMatrixMultiply(&lean, &lean, &rot);
    zactorSetMatrix(r->riderActor, &lean);
    if (bmxphysGetState(r) == BMXPHYS_GRIND_LOCK || bmxphysGetState(r) == BMXPHYS_STALL_LOCK || g_bmxphysHipsFollow) {
        vecCopy(r->physHipOffset, r->rootMotion);
        zmathTransformPoint(&t, RMAT(r), V3(r->physHipOffset));
        vecCopy(r->physHipOffset, &t.x);
    } else {
        zmathVecZero(V3(r->physHipOffset));
    }
    bmxphysPlaceActors(r);

    if ((r->stateFlags & 0x400) && (bits = g_modLimbMask[r->modId]) != 0) {
        for (b = 0; b < 31; b++) {
            bit = (1u << b) & bits;
            switch (bit) {
            case 0x1: case 0x2: case 0x4: case 0x8:
                /* hand / foot off the bike, in rider space */
                if (bit == 0x2) {
                    vecSet(&off.x, blend * -60.0f, blend * 30.0f, blend * -60.0f);
                    bone = 8; forearm = 6; limb = 2;
                } else if (bit == 0x4) {
                    vecSet(&off.x, blend * -20.0f, 0.0f, blend * 40.0f);
                    bone = 0xB; forearm = 9; limb = 4;
                } else if (bit == 0x8) {
                    vecSet(&off.x, blend * -20.0f, 0.0f, blend * -40.0f);
                    bone = 0xE; forearm = 0xC; limb = 8;
                } else {
                    vecSet(&off.x, blend * -60.0f, blend * 30.0f, blend * 60.0f);
                    bone = 5; forearm = 3; limb = 1;
                }
                zmathTransformPoint(&t, &lean, &off);
                zmathVecZero(&off);
                zactorGetAbsolutePartPosition(r->riderActor, bone, &off, &target);
                zmathVecAdd(&target, &target, &t);
                ikReach(r, &target, forearm, limb);
                used = 1;
                break;
            case 0x40: case 0x80: case 0x100:
                /* bar spin: +-120 deg (0x80 the other way); 0x100 keeps the left hand off */
                setBars(r, (int)((float)((bit != 0x80 ? 0xAAAA : 0) - 0x5555) * blend));
                if (bit != 0x100) {
                    zmathVecZero(&off);
                    zactorGetAbsolutePartPosition(r->bikeActor, 7, &off, &target);
                    ikReach(r, &target, 3, 1);
                }
                zmathVecZero(&off);
                zactorGetAbsolutePartPosition(r->bikeActor, 8, &off, &target);
                ikReach(r, &target, 6, 2);
                used = 1;
                break;
            case 0x200: case 0x400:
                /* bar wiggle: driven by the rider byte at +0x44 */
                if (blend > 0.0f) {
                    k = bit == 0x200 ? 0.065f : -0.065f;
                    setBars(r, (int)((float)*((uint8_t *)r + 0x44) * k * 65536.0f));
                }
                break;
            case 0x800: case 0x1000:
                /* hand towards the seat (bike bone 9) */
                zmathVecZero(&off);
                zactorGetAbsolutePartPosition(r->bikeActor, 9, &off, &target);
                zmathVecZero(&off);
                zactorGetAbsolutePartPosition(r->riderActor, bit == 0x800 ? 5 : 8, &off, &t);
                target.x = (target.x - t.x) * blend + t.x;
                target.y = (target.y - t.y) * blend + t.y;
                target.z = (target.z - t.z) * blend + t.z;
                ikReach(r, &target, bit == 0x800 ? 3 : 6, bit == 0x800 ? 1 : 2);
                used = 1;
                break;
            case 0x2000: case 0x4000: case 0x8000: case 0x10000: case 0x20000: case 0x40000:
                /* hand / foot onto a bike bone (bars, pegs, frame) */
                zmathVecZero(&off);
                switch (bit) {
                case 0x4000:  bone = 7; forearm = 6;   limb = 2; break;
                case 0x8000:  bone = 6; off.x = -10.0f; forearm = 3; limb = 1; break;
                case 0x10000: bone = 6; off.x = 10.0f;  forearm = 6; limb = 2; break;
                case 0x20000: bone = 5; off.x = 15.0f;  forearm = 9; limb = 4; break;
                case 0x40000: bone = 5; off.x = -15.0f; forearm = 0xC; limb = 8; break;
                default:      bone = 8; forearm = 3;   limb = 1; break;          /* 0x2000 */
                }
                zactorGetAbsolutePartPosition(r->bikeActor, bone, &off, &target);
                ikReach(r, &target, forearm, limb);
                used = 1;
                break;
            }
        }
        if (used)
            dynIkSolve(r->riderActor, &r->ik);
    }
    pitch = clampSoundVolume((int)(r->wheel[1].spin * 423.3333f));
    volume = (int)(r->frameDist * 3.0f);
    if (volume > 0x7E)
        volume = 0x7F;
    bmxsoundEvent(4000, r->spawnPos, (uint32_t)r->slot << 28 | 0x103, volume, pitch);
}

/* VA 0x0042c7e0, 530 bytes, [I] row, status draft */
void bmxphysPostGrind(BmxRider *r)
{
    /* grind / stall: place the actors, rumble, sparks on metal (edge kind 1) and kind 0, sound */
    RwV3d t, p;
    uint32_t sound = 0, i, n;
    int pitch, volume;
    uint8_t kind;

    r->wheel[1].spin = 0;
    if (r->trickId == 0x2B)                         /* wall tap: the front wheel stops too */
        r->wheel[0].spin = 0;
    zactorSetMatrix(r->riderActor, RMAT(r));
    vecCopy(r->physHipOffset, r->rootMotion);
    zmathTransformPoint(&t, RMAT(r), V3(r->physHipOffset));
    vecCopy(r->physHipOffset, &t.x);
    bmxphysPlaceActors(r);
    if (bmxphysGetState(r) == BMXPHYS_GRIND) {
        if (r->frameDist > 3.0f)
            contrlerRumble(r->slot, 0, 1);
        kind = ((NgColEdge *)r->surfaceObj)->kind;
        if (kind == 1)
            sound = 7;
        else if (kind == 2)
            sound = 2;          /* wood: no sparks */
        else
            sound = 0;
        if (kind != 2) {
            n = (uint32_t)(int)(r->frameDist * 0.05f);
            for (i = 0; i < n; i = i + 1, n = (uint32_t)(int)(r->frameDist * 0.05f)) {
                vecSet(&p.x, (float)(zmathRandom() & 0xF), 0.0f, 0.0f);
                zmathVecRotateY(&p, &p, zmathRandom());
                zmathVecAdd(&p, &p, V3(r->pos));
                fxSpark(r->riderActor, 0, &p, zmathRandom(), 1.0f);
            }
        }
    }
    pitch = clampSoundVolume((int)(r->wheel[1].spin * 423.3333f));
    volume = (int)(r->frameDist * 3.0f);
    if (volume > 0x7E)
        volume = 0x7F;
    bmxsoundEvent(4000, r->spawnPos, (uint32_t)r->slot << 28 | sound | 0x200, volume, pitch);
}

/* VA 0x0042ca90, 412 bytes, [I] row, status draft */
void bmxphysSnapToEdge(BmxRider *r, NgColEdge *edge)
{
    /* move onto the edge, keep only the velocity along it, make it the surface */
    RwV3d c;
    RwMatrix m;
    float v;

    bmxphysClosestPointOnEdge(V3(r->pos), edge, &c);
    vecCopy(r->pos, &c.x);
    vecCopy(r->spawnPos, &c.x);
    vecCopy(&m.right.x, edge->dir);
    if (zmathVecDot(&RMAT(r)->right, &m.right) < 0.0f)
        zmathVecNegate(&m.right, &m.right);
    if (bmxphysGetState(r) == BMXPHYS_GRIND && zmathVecDot(&m.right, &RMAT(r)->right) < 0.99f) {
        zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &g_bmxphysUp);
        zmathVecNormalize(&m.at, &m.at);
        zmathTriangleNormal(&m.up, &m.at, &g_zmathOrigin, &m.right);
        zmathVecZero(&m.pos);
        zmathMatrixCopy(MAT(r->physLockMatrix), &m);
    }
    v = zmathVecDot(V3(r->vel), &m.right);
    r->surfaceObj = edge;
    r->vel[0] = v * m.right.x;
    r->vel[1] = v * m.right.y;
    r->vel[2] = v * m.right.z;
    bmxphysCopySurface(r);
}

/* VA 0x0042cca0, 35 bytes, [I] row, status draft */
void bmxphysClosestPointOnEdge(const RwV3d *p, const NgColEdge *e, RwV3d *out)
{
    zbspEdgeClosestPoint(e->v0, e->v1, e->dir, p, out);
}

/* VA 0x0042cce0, 693 bytes, [I] row, status draft */
void bmxphysPlaceActorsRoll(BmxRider *r)
{
    /* rider follows the bike; at low speed the handlebars turn with the yaw rate */
    float *root;
    RwMatrix m, rot;
    RwV3d t;
    float p[3];
    int angle = 0, a;
    int16_t bars[3];

    root = &zactorGetBikeOffset(r->riderActor)->pos.x;
    root[1] = -root[1];
    zactorCopyRootAngles(r->bikeActor, r->riderActor);
    zmathMatrixCopy(RMAT(r), BMAT(r));
    zmathMatrixCopy(&m, BMAT(r));
    if (r->rotVel[1] != 0 && r->frameDist > 0.0f && r->frameDist <= g_bmxphysSteerSpeed) {
        a = (int16_t)(int32_t)((g_bmxphysSteerSpeed - r->frameDist) * r->rotVel[1] / g_bmxphysSteerSpeed);
        angle = a < -0x4000 ? -0x4000 : (a > 0x3FFF ? 0x4000 : a);
    }
    zactorSetMatrix(r->bikeActor, &m);
    zactorSetMatrix(r->riderActor, &m);
    vecCopy(r->pos, r->spawnPos);
    vecCopy(r->physHipOffset, root);
    zmathTransformPoint(&t, &m, V3(r->physHipOffset));
    vecCopy(r->physHipOffset, &t.x);
    zmathVecNegate(V3(r->physHipOffset), V3(r->physHipOffset));
    vecCopy(p, r->spawnPos);
    zactorSetPos(r->bikeActor, (const RwV3d *)p);
    zactorSetHeight(r->bikeActor, 0.0f);
    p[0] = r->physHipOffset[0] + r->pos[0];
    p[1] = r->pos[1];
    p[2] = r->pos[2] + r->physHipOffset[2];
    zactorSetPos(r->riderActor, (const RwV3d *)p);
    zactorSetHeight(r->riderActor, r->physHipOffset[1]);
    if ((int16_t)angle == 0) {
        bars[0] = -0xAAA;                         /* fork rake 15 deg */
        bars[1] = 0;
        bars[2] = 0;
    } else {
        zmathMatrixIdentity(&rot);
        zmathMatrixIdentity(&m);
        zmathMatrixRotateY(&rot, &rot, angle);
        zmathMatrixRotateX(&rot, &rot, -0xAAA);
        bmxphysMatrixDeltaAngles(&m, &rot, 1, bars);
    }
    zactorSetChannel(r->bikeActor, 1, bars);         /* handlebar bone */
    if ((int16_t)angle != 0) {
        dynIkReset(r->riderActor, &r->ik);
        bmxphysSetLimbTargets(r);
        dynIkSolve(r->riderActor, &r->ik);
    }
}

/* VA 0x0042d050, 389 bytes, [I] row, status draft */
void bmxphysPlaceActors(BmxRider *r)
{
    /* grind / stall / lock-on: the bike hangs under the rider, rolled by airLean */
    RwV3d *root, t;
    RwMatrix m, rot;
    float p[3];

    root = &zactorGetBikeOffset(r->riderActor)->pos;
    zactorCopyRootAngles(r->bikeActor, r->riderActor);
    zactorCopyRotation(r->bikeActor, r->riderActor);
    zmathMatrixCopy(BMAT(r), RMAT(r));
    zmathMatrixCopy(&m, RMAT(r));
    zmathMatrixIdentity(&rot);
    zmathMatrixRotateZ(&rot, &rot, r->airLean);
    zmathMatrixMultiply(&m, &m, &rot);
    root->y = -root->y;
    zmathTransformPoint(&t, &m, root);
    *root = t;
    p[0] = r->physHipOffset[0] + r->pos[0];
    p[1] = r->pos[1];
    p[2] = r->pos[2] + r->physHipOffset[2];
    zactorSetPos(r->riderActor, (const RwV3d *)p);
    zactorSetHeight(r->riderActor, r->physHipOffset[1]);
    r->spawnPos[0] = r->physHipOffset[0] + root->x + r->pos[0];
    r->spawnPos[1] = r->pos[1] + r->physHipOffset[1] + root->y;
    r->spawnPos[2] = r->pos[2] + r->physHipOffset[2] + root->z;
    zactorSetPos(r->bikeActor, (const RwV3d *)r->spawnPos);
    zactorSetHeight(r->bikeActor, 0.0f);
}

/* VA 0x0042d250, 279 bytes, [I] row, status draft */
void bmxphysMatrixDeltaAngles(const RwMatrix *from, const RwMatrix *to, int steps, int16_t *euler)
{
    /* Euler angles (x, y, z) of the rotation from -> to, divided by steps */
    RwMatrix m;
    float c;

    zmathMatrixCopy(&m, from);
    zmathMatrixTranspose(&m, &m);
    zmathMatrixMultiply(&m, &m, to);
    c = (float)sqrt(m.right.x * m.right.x + m.right.y * m.right.y);
    if (c > 0.001f) {
        euler[0] = (int16_t)(zmathAtan2(m.up.z, m.at.z) / steps);
        euler[1] = (int16_t)(zmathAtan2(-m.right.z, c) / steps);
        euler[2] = (int16_t)(zmathAtan2(m.right.y, m.right.x) / steps);
    } else {                                   /* gimbal lock */
        euler[0] = (int16_t)(zmathAtan2(-m.at.y, m.up.y) / steps);
        euler[1] = (int16_t)(zmathAtan2(-m.right.z, c) / steps);
        euler[2] = 0;
    }
}

/* VA 0x0042d3c0, 542 bytes, [I] row, status draft */
void bmxphysAlignToRamp(BmxRider *r, NgColEdge *edge, int always)
{
    /* quarterpipe air: tilt up towards the coping's plane normal, stay 120 units off the plane and
     * lose the velocity across it (the rider goes straight up and comes back down) */
    RwMatrix m;
    RwV3d n;
    float plane[4], dist, v;

    if (!edge)
        return;
    vecCopy(&n.x, edge->normal);
    vecCopy(plane, edge->normal);
    plane[3] = edge->d;
    dist = bmxphysEdgePlaneDistance(V3(r->pos), plane);
    if (dist < 0.0f) {
        zmathVecNegate(&n, &n);
        dist = -dist;
    }
    if (!always && zmathAcos(zmathVecDot(&RMAT(r)->up, &n)) >= g_bmxphysQpLimit)
        return;
    if (!(r->stateFlags & RIDER_FLAG_FLIP)) {
        m.right = RMAT(r)->right;
        m.up = RMAT(r)->up;
        blendRow(&m.up, &n);
        zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
        zmathVecNormalize(&m.at, &m.at);
        zmathTriangleNormal(&m.right, &m.up, &g_zmathOrigin, &m.at);
        zmathVecZero(&m.pos);
        zmathMatrixCopy(RMAT(r), &m);
    }
    if (dist < 120.0f) {
        dist = 120.0f - dist;
        r->pos[0] += dist * n.x;
        r->pos[1] += dist * n.y;
        r->pos[2] += dist * n.z;
    }
    v = zmathVecDot(&n, V3(r->vel));
    r->vel[0] -= v * n.x;
    r->vel[1] -= v * n.y;
    r->vel[2] -= v * n.z;
}

/* VA 0x0042d670, 75 bytes, [I] row, status draft */
float bmxphysEdgePlaneDistance(const RwV3d *p, const float *plane)
{
    /* the point is truncated to integers first ([V] three __ftol calls) */
    return (float)(int)p->z * plane[2] + (float)(int)p->y * plane[1] + (float)(int)p->x * plane[0] + plane[3];
}

/* VA 0x0042d6e0, 488 bytes, [I] row, status draft */
void bmxphysClosestPointOnSegment(const int16_t *a, const int16_t *b, const int16_t *p, float *out)
{
    RwV3d d;
    float t;

    d.x = (float)(int16_t)(b[0] - a[0]);
    d.y = (float)(int16_t)(b[1] - a[1]);
    d.z = (float)(int16_t)(b[2] - a[2]);
    zmathVecNormalize(&d, &d);
    if ((float)(int16_t)(a[0] - p[0]) * d.x + (float)(int16_t)(a[1] - p[1]) * d.y +
        (float)(int16_t)(a[2] - p[2]) * d.z >= 0.0f) {
        vecSet(out, a[0], a[1], a[2]);                   /* before a */
        return;
    }
    t = (float)(int16_t)(b[0] - p[0]) * d.x + (float)(int16_t)(b[1] - p[1]) * d.y +
        (float)(int16_t)(b[2] - p[2]) * d.z;
    if (t > 0.0f)
        vecSet(out, b[0] - t * d.x, b[1] - t * d.y, b[2] - t * d.z);
    else
        vecSet(out, b[0], b[1], b[2]);                   /* past b */
}

/* VA 0x0042d950, 302 bytes, [I] row, status draft */
NgColEdge *bmxphysFindTaggedEdge(uint32_t ctx, int id, const RwV3d *pos)
{
    /* the edge whose edge-tag code is `id` (the face tag's id) nearest to pos */
    NgColEdge *e, *best = 0;
    int16_t p[3];
    float c[3], d, bestD = 1e11f;
    int i;

    e = zbspGetTaggedEdge(ctx, id, 0, 0);
    if (!e)
        return 0;
    for (i = 0; e; e = zbspGetTaggedEdge(ctx, id, ++i, 0)) {
        zstringMemCopy(&g_bmxphysEdgeScratch, e, sizeof(NgColEdge));
        p[0] = (int16_t)(int32_t)pos->x;
        p[1] = (int16_t)(int32_t)pos->y;
        p[2] = (int16_t)(int32_t)pos->z;
        bmxphysClosestPointOnSegment(g_bmxphysEdgeScratch.v0, g_bmxphysEdgeScratch.v1, p, c);
        d = (c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) + (c[2] - p[2]) * (c[2] - p[2]);
        if (d < bestD) {
            best = &g_bmxphysTaggedEdge;
            zstringMemCopy(&g_bmxphysTaggedEdge, &g_bmxphysEdgeScratch, sizeof(NgColEdge));
            bestD = d;
        }
    }
    return best;
}

/* VA 0x0042dad0, 396 bytes, [I] row (was "bmxphysUpdatePositionAir", seed), status draft */
int bmxphysSetSurfaceFromHit(BmxRider *r, NgColHit *hit)
{
    /* make what a wheel / the rider landed on the current surface (tag class -> BMXSURF_*) */
    uint16_t code;

    if ((hit->type & 1) && hit->face) {
        code = hit->face->code;
        switch (code >> 12) {
        case 1: case 2: case 3:           /* quarterpipe / spine / vert: the surface is the coping edge */
            r->surfaceObj = bmxphysFindTaggedEdge(hit->unk_04, code & 0xFFF, V3(r->pos));
            r->surface = (uint8_t)(hit->face->code >> 12);
            bmxphysCopySurface(r);
            if (!r->surfaceObj) {
                g_pfnDebugPrintf("bmxphysUpdatePositionAir: quarterpipe face with no assigned edge, ID=%d\n",
                                 hit->face->code & 0xFFF);
                r->surface = BMXSURF_GROUND;
                r->surfaceObj = hit->face;
                bmxphysCopySurface(r);
            }
            if ((r->surface == BMXSURF_QUARTER || r->surface == BMXSURF_VERT) &&
                (r->stateFlags & RIDER_FLAG_FAKIE) && !(r->stateFlags & RIDER_FLAG_FORWARD_HOP))
                bmxphysAlignToRamp(r, (NgColEdge *)r->surfaceObj, 0);
            return 1;
        case 4: case 5: case 6: case 7: case 8:   /* launch ramps */
            r->surface = (uint8_t)(code >> 12);
            r->surfaceObj = hit->face;
            bmxphysCopySurface(r);
            return 1;
        default:
            r->surface = BMXSURF_GROUND;
            r->surfaceObj = hit->face;
            bmxphysCopySurface(r);
            r->stateFlags &= ~RIDER_FLAG_FORWARD_HOP;
            return 1;
        }
    }
    if ((hit->type & 2) && hit->propObj) {
        r->surface = BMXSURF_PROP;
        r->surfaceObj = hit->propObj;
        bmxphysCopySurface(r);
        return 1;
    }
    return 0;
}

/* VA 0x0042dce0, 106 bytes, [I] row, status draft */
void bmxphysCopyContactSurface(BmxContact *c)
{
    /* keep a private copy: the collision layer reuses its buffers every query */
    uint32_t n;

    if (c->obj && (n = surfaceRecordSize(c->type)) != 0) {
        zstringMemCopy(c->unk_008, c->obj, n);
        c->obj = c->unk_008;
    }
}

/* VA 0x0042dda0, 131 bytes, [I] row, status draft */
void bmxphysCopySurface(BmxRider *r)
{
    uint32_t n;

    if (r->surfaceObj && (n = surfaceRecordSize(r->surface)) != 0) {
        zstringMemCopy(r->physSurfaceCopy, r->surfaceObj, n);
        r->surfaceObj = r->physSurfaceCopy;
    }
}

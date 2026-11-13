/*
 * zaxis/Bmx/Src/com/zge/bmx/dyn.c
 * Original: unknown (no path string; the file has no asserts). Name [I]: "dyn" after its functions
 *           (DM-047's prefix); it sits between bmxsound.c and zoverii.c in link order, which within a
 *           directory is alphabetical (com\zge\bmx\).
 * Evidence: [I] one object file: the rag doll (0x00450060-0x00455DD0) and the limb IK (0x00455E60-
 *           0x00457897) share their helpers (dynCross, dynTranspose3, dynMul3, dynMatrixToEuler) and their
 *           .bss statics interleave (0x005746C4-0x00574D0C: IK scratch next to the rag-doll flags);
 *           its .data is 0x0055B958-0x0055B95F (gravity, grab height), right after bmxsound.c's and
 *           before zoverii.c's (0x0055B960); no call edges with the sound or overlay code.
 *           0x004578F0 (the last function of the old gap) is zoverii.c's zoveriiInit (its pools).
 * 74 functions, VA 0x00450060-0x00457897 (4 created by DM-058: dynBrace, dynSetLimitsOff,
 * dynSetLimitsOn, dynIkSetLooseGains; all four unreferenced, reachable only through their
 * incremental-link thunks).
 * DM-058 (draft). Non-matching C (AD-8). Types: dyn.h, rider.h (BmxIk). Notes: docs/re/physics.md 7.
 *
 * The crash rag doll. When the rider crashes (physics.c bmxphysStateInitCrash) the rider and the
 * bike each become a particle body: the rider 14 particles on the skeleton's joints, the bike 3.
 * Every frame physics.c steps them (dynStepRider / dynStepBike) and copies the result back: the
 * rider's position = the body's mass centre, its matrix = the pelvis frame, the pose = the channel
 * angles dynStepRider writes into the actor's decoder block 1 (physics copies block 1 to block 0,
 * the pose NgIFzactor_ApplyMotionPose draws). The player can tuck (ACT_GO_LIMP), grab a nearby
 * rail / ledge with the hands (ACT_RAGDOLL) and spin the body with the stick (rider.c riderStateCrash).
 * Cheats: Super Crash (bounce 1.75 instead of 0.6), Sticky Crash (particles that touch stay put).
 *
 * The second half is the two-bone IK that puts the rider's hands on the handlebars and the feet on
 * the pedals (physics.c bmxphysSetLimbTargets / bmxphysPostAir): dynIkSetTarget per limb, then
 * dynIkSolve writes the arm / leg channel angles of the actor's output pose (block 0).
 *
 * The exe keeps the IK's temporaries in file statics (0x005746C4..); this C uses locals.
 */

#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "ngif.h"
#include "dyn.h"

/* ------------------------------------------------------------------ this file's internal functions */
void dynSolveLimit(DynLimit *l);                                                   /* 0x004505F0 */
void dynSolveLimitB(DynLimit *l);                                                  /* 0x00450990 */
void dynSolveAxisLimit(DynLimit *l);                                               /* 0x00450D30 */
void dynSolveLimits(DynBody *b);                                                   /* 0x00450E00 */
int  dynCollideParticle(DynParticle *p, float *force, NgColHit *hit);              /* 0x00451390 */
void dynSlabProject(const RwV3d *a, const RwV3d *b, float radius, float *centre, float *half, const RwV3d *n); /* 0x00451C80 */
void dynPushOutParticle(DynParticle *p);                                           /* 0x00451D60 */
void dynSolveSpring(DynSpring *s);                                                 /* 0x00452370 */
void dynSolveBendSpring(DynSpring *s);                                             /* 0x004524D0 */
void dynSolveAngleSpring(DynSpring *s);                                            /* 0x00452680 */
void dynPoseFromActor(DynBody *b, ZActor *actor);                                  /* 0x00452B80 */
void dynSelect(void *body);                                                        /* 0x00453460 */
void dynGrabEdges(DynBody *b);                                                     /* 0x004536A0 */
void dynLerpTo(RwV3d *a, RwV3d *b);                                                /* 0x004538E0 */
void dynBracePose(DynBody *b);                                                     /* 0x00453920 */
void dynTuckPose(DynBody *b);                                                      /* 0x00453C30 */
void dynClearLimits(DynBody *b);                                                   /* 0x004542D0 */
void dynStickParticles(DynBody *b);                                                /* 0x004543B0 */
void dynRotateBody(DynBody *b, void *actor, const RwMatrix *rot);                  /* 0x004544B0 */
void dynApplyPitch(DynBody *b, void *actor);                                       /* 0x004545E0 */
void dynApplyRoll(DynBody *b, void *actor);                                        /* 0x004546D0 */
void dynFrameFromPairs(DynBody *b, int i, int j, int k, int l, RwMatrix *out);     /* 0x00455080 */
void dynLimbFrame(DynBody *b, int i, int j, const RwMatrix *parent, RwMatrix *out); /* 0x00455140 */
void dynHingeFrame(DynBody *b, int i, int j, const RwMatrix *thigh, RwMatrix *out, int left); /* 0x00455430 */
void dynLimbFrameMirror(DynBody *b, int i, int j, const RwMatrix *parent, RwMatrix *out); /* 0x00455590 */
void dynElbowFrame(DynBody *b, int i, int j, const RwMatrix *upper, RwMatrix *out, int left); /* 0x00455880 */
void dynFrameFromUp(DynBody *b, RwV3d *dir, const RwMatrix *parent, RwMatrix *out); /* 0x004559C0 */
void dynVecSub(const float *a, const float *b, float *out);                        /* 0x00455B20 */
void dynCross(const RwV3d *a, const float *b, RwV3d *out);                         /* 0x00455B60 */
void dynZeroAngles(int16_t *euler);                                                /* 0x00455BF0 */
void dynTranspose3(const RwMatrix *src, RwMatrix *dst);                            /* 0x00455C20 */
void dynMul3(RwMatrix *out, const RwMatrix *a, const RwMatrix *b);                 /* 0x00455C60 */
void dynMatrixToEuler(const RwMatrix *m, int16_t *euler);                          /* 0x00455CF0 */
void dynSetActorFrame(ZActor *actor, const RwMatrix *m, int16_t *hips);            /* 0x00455DD0 */
void dynIkBlendAxes(const float *w, const float *a, const float *b, const float *c, RwV3d *out); /* 0x00456030 */

/* ------------------------------------------------------------------ data (.data) */
float g_dynGravity = 1.58f;                 /* 0x0055B958 added to vel.y per step (+y is down; physics.c
                                               uses the same 1.58 for the rider) */
float g_dynGrabDrop = -10.0f;               /* 0x0055B95C a grabbing wrist hangs 10 units above the edge */

/* ------------------------------------------------------------------ globals (.bss) */
DynSlab g_dynSlab;                          /* 0x0068FFC0 dynCollideParticle / dynPushOutParticle scratch */

/* Per rider (2 bodies, the slot picked by dynSelect from the body pointer) */
static DynBody *s_dynBodies[2];             /* 0x00574BC4 filled by dynCreateRider (round robin) */
static int32_t  s_dynNumBodies;             /* 0x00574CA4 next slot (wraps at 2) */
static int32_t  s_dynCur;                   /* 0x00574CA8 slot of the body last passed to a dyn* call */
static int32_t  s_dynSticky[2];             /* 0x00574734 Sticky Crash cheat */
static int32_t  s_dynBraced[2];             /* 0x0057473C arms held out (dynBrace) */
static int32_t  s_dynGrabOn[2];             /* 0x005748D4 dynGrab: look for an edge each step */
static int32_t  s_dynGrabbed[2];            /* 0x0057491C a hand caught an edge in this crash (latch) */
static int32_t  s_dynHanging[2];            /* 0x00574924 hanging from an edge (dynIsHanging) */
static int32_t  s_dynTucked[2];             /* 0x00574930 dynTuck */
static int32_t  s_dynNoLimits[2];           /* 0x00574AD4 dynSetLimitsOff */
static int32_t  s_dynRollOn[2];             /* 0x00574B1C */
static int32_t  s_dynPitchOn[2];            /* 0x00574B24 */
static int16_t  s_dynPitch[2];              /* 0x00574CAC stick input angles */
static int16_t  s_dynRoll[2];               /* 0x00574CB0 */
/* indices of the rider body's limits and springs (the same for both bodies) */
static int32_t  s_limRKneeB;                /* 0x00574CB4 DYN_LIMIT_B (2, 3, pivot 1) */
static int32_t  s_limRKnee;                 /* 0x00574CB8 DYN_LIMIT  (2, 3, pivot 1) */
static int32_t  s_limLKneeB;                /* 0x00574CBC DYN_LIMIT_B (1, 4, pivot 2) */
static int32_t  s_limLKnee;                 /* 0x00574CC0 DYN_LIMIT  (1, 4, pivot 2) */
static int32_t  s_limRElbow;                /* 0x00574CC4 DYN_LIMIT  (8, 9, pivot 7) */
static int32_t  s_limLElbow;                /* 0x00574CC8 DYN_LIMIT  (7, 10, pivot 8) */
static int32_t  s_sprRUpperArm;             /* 0x00574CCC (7, 9) */
static int32_t  s_sprLUpperArm;             /* 0x00574CD0 (8, 10) */
static int32_t  s_sprRForeArm;              /* 0x00574CD4 (9, 11) */
static int32_t  s_sprLForeArm;              /* 0x00574CD8 (10, 12) */
static int32_t  s_sprRThigh;                /* 0x00574CDC (1, 3) */
static int32_t  s_sprLThigh;                /* 0x00574CE0 (2, 4) */
static int32_t  s_sprRShin;                 /* 0x00574CE4 (3, 5) */
static int32_t  s_sprLShin;                 /* 0x00574CE8 (4, 6) */
static int32_t  s_sprNeck[3];               /* 0x00574CEC: dynCreateRider stores all three neck springs in
                                               [0] (a bug [V]), so [0] = (13, 8) and [1], [2] stay 0 */
/* frames that persist between steps (the lower limbs keep last frame's when the limits are off) */
static RwMatrix s_dynShinL;                 /* 0x00574794 */
static RwMatrix s_dynShinR;                 /* 0x005747F4 */
static RwMatrix s_dynForeArmR;              /* 0x00574854 */
static RwMatrix s_dynForeArmL;              /* 0x00574938 */
static RwMatrix s_dynHead;                  /* 0x00574A54 */

/* the rider skeleton bone each rider particle starts on (dynPoseFromActor's switch) */
static const uint8_t s_dynParticleBone[DYNP_RIDER_COUNT] = {
    1,   /* 0 pelvis      Chest */
    12,  /* 1 right hip   RightUpLeg */
    9,   /* 2 left hip    LeftUpLeg */
    13,  /* 3 right knee  RightLowLeg */
    10,  /* 4 left knee   LeftLowLeg */
    14,  /* 5 right ankle RightFoot */
    11,  /* 6 left ankle  LeftFoot */
    6,   /* 7 right shoulder RightUpArm */
    3,   /* 8 left shoulder  LeftUpArm */
    7,   /* 9 right elbow RightLowArm */
    4,   /* 10 left elbow LeftLowArm */
    8,   /* 11 right wrist RightHand */
    5,   /* 12 left wrist LeftHand */
    2    /* 13 head (25 units up the bone) Head */
};

#define P(b, i)      (&(b)->particle[i])
#define CUR(arr)     ((arr)[s_dynCur])

/* ------------------------------------------------------------------ the particle system */

/* VA 0x00450060, 36 bytes, [I] row, status draft */
DynBody *dynCreate(void)
{
    DynBody *b = (DynBody *)zmemTempAlloc(sizeof(DynBody));

    b->numParticles = 0;
    b->numSprings = 0;
    b->onImpact = 0;
    b->numLimits = 0;
    return b;
}

/* VA 0x004500a0, 123 bytes, [I] row, status draft */
int dynAddParticle(DynBody *b, const DynParticleDesc *desc)
{
    DynParticle *p = P(b, b->numParticles);

    p->type = desc->type;
    if (desc->type >= 3)
        return -1;
    p->mass = desc->mass;
    p->bounce = desc->bounce;
    p->friction = desc->friction;
    p->pos = desc->pos;
    p->flags = 0;
    zmathVecZero(&p->sum);
    zmathVecZero(&p->vel);
    return (int)b->numParticles++;
}

/* rest length = the particles' current distance */
static float dynDistance(const DynParticle *a, const DynParticle *b)
{
    RwV3d d;

    d.x = a->pos.x - b->pos.x;
    d.y = a->pos.y - b->pos.y;
    d.z = a->pos.z - b->pos.z;
    return zmathVecLength(&d);
}

/* VA 0x00450140, 151 bytes, [I] row, status draft */
int dynAddSpring(DynBody *b, int i, int j, float weight)
{
    DynSpring *s = &b->spring[b->numSprings];

    s->weight = weight;
    s->type = DYN_SPRING;
    s->a = P(b, i);
    s->b = P(b, j);
    s->rest = dynDistance(s->a, s->b);
    return (int)b->numSprings++;
}

/* Unreferenced. */
/* VA 0x00450210, 192 bytes, [I] row, status draft */
int dynAddBendSpring(DynBody *b, int i, int j, int c, int d, float weight, float bend)
{
    DynSpring *s = &b->spring[b->numSprings];

    s->weight = weight;
    s->type = DYN_SPRING_BEND;
    s->a = P(b, i);
    s->b = P(b, j);
    s->rest = dynDistance(s->a, s->b);
    s->c = P(b, c);
    s->bend = bend;
    s->d = P(b, d);
    return (int)b->numSprings++;
}

/* Unreferenced. The rest angle between a -> b and the at axis of the frame of particles i0..i2 is
 * recorded, but the solver (dynSolveAngleSpring) does nothing. */
/* VA 0x00450300, 237 bytes, [I] row, status draft */
int dynAddAngleSpring(DynBody *b, int i, int j, int i0, int i1, int i2, float weight)
{
    DynSpring *s = &b->spring[b->numSprings];
    RwV3d dir;
    RwMatrix frame;
    uint8_t *raw = (uint8_t *)&s->rest;

    s->weight = weight;
    s->type = DYN_SPRING_ANGLE;
    s->a = P(b, i);
    s->b = P(b, j);
    dir.x = s->b->pos.x - s->a->pos.x;
    dir.y = s->b->pos.y - s->a->pos.y;
    dir.z = s->b->pos.z - s->a->pos.z;
    zmathVecNormalize(&dir, &dir);
    dynFrameFromParticles(b, i0, i1, i2, &frame);
    raw[2] = (uint8_t)i0;
    raw[3] = (uint8_t)i1;
    *(uint8_t *)&s->c = (uint8_t)i2;
    *(int16_t *)raw = zmathAcos(zmathVecDot(&dir, &frame.at));
    return (int)b->numSprings++;
}

static int dynCopyLimit(DynBody *b, uint32_t type, const DynLimitDesc *desc)
{
    DynLimit *l = &b->limit[b->numLimits];

    l->type = type;
    l->a = desc->a;
    l->b = desc->b;
    l->pivot = desc->pivot;
    l->frameA = desc->frameA;
    l->frameB = desc->frameB;
    l->strength = desc->strength;
    l->side = desc->side;
    l->axisA = desc->axisA;
    l->axisB = desc->axisB;
    return (int)b->numLimits++;
}

/* VA 0x00450430, 101 bytes, [I] row, status draft */
int dynAddLimit(DynBody *b, const DynLimitDesc *desc)
{
    return dynCopyLimit(b, DYN_LIMIT, desc);
}

/* VA 0x004504c0, 101 bytes, [I] row, status draft */
int dynAddLimitB(DynBody *b, const DynLimitDesc *desc)
{
    return dynCopyLimit(b, DYN_LIMIT_B, desc);
}

/* Unreferenced: keep a spring's b at the spring's length along a's... (dynSolveAxisLimit). */
/* VA 0x00450550, 120 bytes, [I] row, status draft */
int dynAddAxisLimit(DynBody *b, const DynSpring *s)
{
    DynLimit *l = &b->limit[b->numLimits];

    l->type = DYN_LIMIT_AXIS;
    l->a = s->a;
    l->b = s->b;
    l->strength = dynDistance(s->a, s->b);
    return (int)b->numLimits++;
}

/* an axis of a particle's frame (DYN_AXIS_*) */
static RwV3d dynAxis(const DynParticle *p, uint32_t axis)
{
    RwV3d v;

    switch (axis) {
    case DYN_AXIS_RIGHT: return p->frame.right;
    case DYN_AXIS_AT:    return p->frame.at;
    case DYN_AXIS_UP:    return p->frame.up;
    case DYN_AXIS_NRIGHT: v = p->frame.right; break;
    case DYN_AXIS_NAT:   v = p->frame.at; break;
    default:             v = p->frame.up; break;
    }
    zmathVecScale(&v, &v, -1.0f);
    return v;
}

/* the limit acts only on its side of the plane (proj = minus the signed distance) */
static int dynLimitActs(const DynLimit *l, float proj)
{
    if (l->side == DYN_SIDE_BEHIND && proj > 0.0f)
        return 0;
    if (l->side == DYN_SIDE_FRONT && proj < 0.0f)
        return 0;
    return 1;
}

/* Push a onto the plane through the pivot with normal axisB (frameB), then b onto the plane with
 * normal axisA (frameA), each by strength x its share of the two masses. */
/* VA 0x004505f0, 735 bytes, [I] row, status draft */
void dynSolveLimit(DynLimit *l)
{
    RwV3d na = dynAxis(l->frameA, l->axisA);
    RwV3d nb = dynAxis(l->frameB, l->axisB);
    RwV3d d;
    float proj, ma, mb;

    d.x = l->a->pos.x - l->pivot->pos.x;
    d.y = l->a->pos.y - l->pivot->pos.y;
    d.z = l->a->pos.z - l->pivot->pos.z;
    proj = -zmathVecDot(&d, &nb);
    if (!dynLimitActs(l, proj))
        return;
    ma = l->a->mass;
    zmathVecScale(&nb, &nb, proj * l->strength * (ma / (ma + l->b->mass)));
    if (l->a->type != DYN_FIXED)
        zmathVecAdd(&l->a->pos, &l->a->pos, &nb);

    d.x = l->b->pos.x - l->pivot->pos.x;
    d.y = l->b->pos.y - l->pivot->pos.y;
    d.z = l->b->pos.z - l->pivot->pos.z;
    proj = zmathVecDot(&d, &na);
    mb = l->b->mass;
    zmathVecScale(&na, &na, -proj * l->strength * (mb / (mb + l->a->mass)));
    if (l->b->type != DYN_FIXED)
        zmathVecAdd(&l->b->pos, &l->b->pos, &na);
}

/* Push b onto the plane through the pivot with normal axisA (frameA); at most 4 units per step.
 * It tests a's type but moves b (as compiled). axisB is fetched and not used. */
/* VA 0x00450990, 722 bytes, [I] row, status draft */
void dynSolveLimitB(DynLimit *l)
{
    RwV3d na = dynAxis(l->frameA, l->axisA);
    RwV3d push, d;
    float proj, ma;

    (void)dynAxis(l->frameB, l->axisB);
    d.x = l->b->pos.x - l->pivot->pos.x;
    d.y = l->b->pos.y - l->pivot->pos.y;
    d.z = l->b->pos.z - l->pivot->pos.z;
    proj = -zmathVecDot(&d, &na);
    if (!dynLimitActs(l, proj))
        return;
    push = na;
    ma = l->a->mass;
    proj = proj * l->strength * (ma / (ma + l->b->mass));
    zmathVecScale(&na, &na, proj);
    zmathVecScale(&push, &push, proj);
    if (zmathVecDot(&push, &push) > 16.0f) {
        zmathVecNormalize(&push, &push);
        zmathVecScale(&push, &push, 4.0f);
    }
    if (l->a->type != DYN_FIXED)
        zmathVecAdd(&l->b->pos, &l->b->pos, &push);
}

/* Move b 3.875 % of the way towards a.pos - b.up x length. */
/* VA 0x00450d30, 160 bytes, [I] row, status draft */
void dynSolveAxisLimit(DynLimit *l)
{
    RwV3d t = l->b->frame.up;

    zmathVecScale(&t, &t, -l->strength);
    zmathVecAdd(&t, &t, &l->a->pos);
    t.x = l->b->pos.x - t.x;
    t.y = l->b->pos.y - t.y;
    t.z = l->b->pos.z - t.z;
    zmathVecScale(&t, &t, -0.03875f);
    zmathVecAdd(&l->b->pos, &l->b->pos, &t);
}

/* VA 0x00450e00, 95 bytes, [I] row, status draft */
void dynSolveLimits(DynBody *b)
{
    uint32_t i;

    if (b->starting)
        return;
    for (i = 0; i < b->numLimits; i++) {
        DynLimit *l = &b->limit[i];

        if (l->type == DYN_LIMIT)
            dynSolveLimit(l);
        else if (l->type == DYN_LIMIT_B)
            dynSolveLimitB(l);
        else if (l->type == DYN_LIMIT_AXIS)
            dynSolveAxisLimit(l);
    }
}

/* One step: integrate (gravity, velocity), the limits, two relaxation passes of the springs, then the
 * world collision of every moving particle (with the impact callback); the mass centre moves along. */
/* VA 0x00450e80, 814 bytes, [I] row, status draft */
void dynStep(DynBody *b)
{
    uint32_t i;
    int pass;
    RwV3d shift;
    float mass, force;
    NgColHit hit;

    for (i = 0; i < b->numParticles; i++) {
        DynParticle *p = P(b, i);

        p->prevPos = p->pos;
        if (p->type != DYN_FIXED) {
            p->vel.y += g_dynGravity;
            p->pos.x += p->vel.x;
            p->pos.y += p->vel.y;
            p->pos.z += p->vel.z;
        }
        p->pos.x -= b->centre.x;            /* relative to the centre while solving */
        p->pos.y -= b->centre.y;
        p->pos.z -= b->centre.z;
    }
    dynSolveLimits(b);

    for (pass = 2; pass != 0; pass--) {
        for (i = 0; i < b->numParticles; i++) {
            DynParticle *p = P(b, i);

            p->weight = p->mass;
            p->sum.x = p->pos.x * p->mass;
            p->sum.y = p->pos.y * p->mass;
            p->sum.z = p->pos.z * p->mass;
        }
        for (i = 0; i < b->numSprings; i++) {
            DynSpring *s = &b->spring[i];

            if (s->type == DYN_SPRING) {
                dynSolveSpring(s);
            } else if (s->type == DYN_SPRING_BEND) {
                dynSolveBendSpring(s);
                dynSolveSpring(s);          /* (as compiled: the bend case falls into the plain one) */
            } else if (s->type == DYN_SPRING_ANGLE) {
                dynSolveAngleSpring(s);
            }
        }
        for (i = 0; i < b->numParticles; i++) {
            DynParticle *p = P(b, i);

            if (p->type != DYN_FIXED) {
                float w = 1.0f / p->weight;

                p->pos.x = w * p->sum.x;
                p->pos.y = w * p->sum.y;
                p->pos.z = w * p->sum.z;
            }
        }
    }

    zmathVecZero(&shift);
    mass = 0.0f;
    for (i = 0; i < b->numParticles; i++) {
        DynParticle *p = P(b, i);

        if (p->type == DYN_FIXED) {
            p->pos = p->prevPos;
        } else {
            p->pos.x += b->centre.x;
            p->pos.y += b->centre.y;
            p->pos.z += b->centre.z;
            p->vel.x = p->pos.x - p->prevPos.x;
            p->vel.y = p->pos.y - p->prevPos.y;
            p->vel.z = p->pos.z - p->prevPos.z;
            if (dynCollideParticle(p, &force, &hit) && b->onImpact)
                b->onImpact(b, (int)i, force, &hit);
            p->pos.x -= b->centre.x;
            p->pos.y -= b->centre.y;
            p->pos.z -= b->centre.z;
            if (p->type == DYN_FIXED) {     /* the callback may have pinned it */
                p->pos = p->prevPos;
            } else {
                shift.x += p->mass * p->pos.x;
                shift.y += p->mass * p->pos.y;
                shift.z += p->mass * p->pos.z;
                mass += p->mass;
                p->pos.x += b->centre.x;
                p->pos.y += b->centre.y;
                p->pos.z += b->centre.z;
            }
        }
        if (!(p->flags & DYNF_CONTACT)) {   /* the collision response set vel itself */
            p->vel.x = p->pos.x - p->prevPos.x;
            p->vel.y = p->pos.y - p->prevPos.y;
            p->vel.z = p->pos.z - p->prevPos.z;
        }
    }
    if (mass > 0.0f)
        zmathVecScale(&shift, &shift, 1.0f / mass);
    zmathVecAdd(&b->centre, &b->centre, &shift);
}

/* VA 0x00451280, 15 bytes, [I] row, status draft */
void dynSetImpactCallback(void *body, DynImpactFn fn)
{
    ((DynBody *)body)->onImpact = fn;
}

/* A frame from three particles: right = i - midpoint(j, k), at = j - k, up = at x right (zmathTriangleNormal
 * with the origin as the middle corner), no translation. */
/* VA 0x004512a0, 178 bytes, [I] row, status draft */
void dynFrameFromParticles(DynBody *b, int i, int j, int k, RwMatrix *out)
{
    const DynParticle *pi = P(b, i), *pj = P(b, j), *pk = P(b, k);

    out->at.x = pj->pos.x - pk->pos.x;
    out->at.y = pj->pos.y - pk->pos.y;
    out->at.z = pj->pos.z - pk->pos.z;
    out->right.x = pi->pos.x - (out->at.x * 0.5f + pk->pos.x);
    out->right.y = pi->pos.y - (out->at.y * 0.5f + pk->pos.y);
    out->right.z = pi->pos.z - (out->at.z * 0.5f + pk->pos.z);
    zmathVecNormalize(&out->right, &out->right);
    zmathVecNormalize(&out->at, &out->at);
    zmathVecZero(&out->pos);
    zmathTriangleNormal(&out->up, &out->right, &out->pos, &out->at);
}

/* the face plane's d from its RW triangle (zbspColSphere leaves it stale) */
static RwV3d dynFaceNormal(NgColFace *f)
{
    RwV3d n;

    n.x = f->normal[0];
    n.y = f->normal[1];
    n.z = f->normal[2];
    zmathVecNormalize(&n, &n);
    f->d = -zmathVecDot(&n, (const RwV3d *)((const ZrwColTri *)f->rwTri)->v[0]);
    return n;
}

/* Where a sphere of DYN_RADIUS moving from `from` to `to` first touches the face's plane: 4 bisection
 * steps, then pushed out to exactly DYN_RADIUS. 0 if the swept band misses the plane. */
static int dynSweepContact(const NgColFace *f, const RwV3d *from, const RwV3d *to, RwV3d *contact)
{
    RwV3d lo, hi, mid, n;
    int k;

    g_dynSlab.normal.x = f->normal[0];
    g_dynSlab.normal.y = f->normal[1];
    g_dynSlab.normal.z = f->normal[2];
    dynSlabProject(from, to, DYN_RADIUS, &g_dynSlab.centre, &g_dynSlab.half, &g_dynSlab.normal);
    g_dynSlab.dist = -f->d;
    g_dynSlab.zero = 0;
    if (fabsf(g_dynSlab.dist - g_dynSlab.centre) > g_dynSlab.half)
        return 0;
    lo = *from;
    hi = *to;
    mid = lo;
    zmathVecAdd(&mid, &mid, &hi);
    zmathVecScale(&mid, &mid, 0.5f);
    for (k = 4; k != 0; k--) {
        if (zmathVecDot(&mid, &g_dynSlab.normal) - g_dynSlab.dist >= DYN_RADIUS)
            lo = mid;
        else
            hi = mid;
        mid = lo;
        zmathVecAdd(&mid, &mid, &hi);
        zmathVecScale(&mid, &mid, 0.5f);
    }
    n = g_dynSlab.normal;
    zmathVecScale(&n, &n, DYN_RADIUS - (zmathVecDot(&mid, &g_dynSlab.normal) - g_dynSlab.dist));
    zmathVecAdd(contact, &mid, &n);
    return 1;
}

/* Sweep one particle (prevPos -> pos + vel) against the world and the props. Contacts are averaged;
 * the velocity loses its normal part, is scaled by the friction and gets the normal part back x
 * bounce x 0.95; nearly at rest it stops (DYNF_REST). Returns 1 with the impact speed and the contact
 * (type, point, plane, material or prop) when the particle hit something it was moving into. Faster
 * than 100 units per step there is no collision at all. */
/* VA 0x00451390, 1820 bytes, [I] row, status draft */
int dynCollideParticle(DynParticle *p, float *force, NgColHit *hit)
{
    ZbspSphereResult *res;
    ZbspSphereHit *h;
    RwV3d end, centre, contact, sumPos, sumNormal, n, v;
    uint32_t count = 0;
    float impact;

    zmathVecZero(&v);
    zmathVecZero(&sumPos);
    zmathVecZero(&sumNormal);
    p->flags &= ~DYNF_CONTACT;
    if (zmathVecDot(&p->vel, &p->vel) > 10000.0f)
        return 0;
    hit->normal[0] = hit->normal[1] = hit->normal[2] = hit->d = 0.0f;
    zstringMemSet(hit, 0, sizeof(NgColHit));
    end = p->pos;
    zmathVecAdd(&end, &end, &p->vel);
    centre = p->prevPos;
    zmathVecAdd(&centre, &centre, &end);
    zmathVecScale(&centre, &centre, 0.5f);
    res = zbspColSphere(&centre.x, zmathVecLength(&p->vel) * 0.5f + DYN_RADIUS, 1.0f);
    if (res == NULL)
        return 0;
    for (h = res->hits; h->face != NULL; h++) {
        n = dynFaceNormal(h->face);
        if (zmathVecDot(&p->vel, &p->vel) >= 0.1f) {
            if (!dynSweepContact(h->face, &p->prevPos, &end, &contact))
                continue;
        } else {
            contact.x = h->face->normal[0] * DYN_RADIUS + h->point.x;
            contact.y = h->face->normal[1] * DYN_RADIUS + h->point.y;
            contact.z = h->face->normal[2] * DYN_RADIUS + h->point.z;
        }
        /* (the exe also computes pos - contact here and drops it) */
        p->flags |= DYNF_CONTACT;
        count++;
        zmathVecAdd(&sumPos, &sumPos, &contact);
        zmathVecAdd(&sumNormal, &sumNormal, &n);
        hit->normal[0] += h->face->normal[0];
        hit->normal[1] += h->face->normal[1];
        hit->normal[2] += h->face->normal[2];
        hit->d += h->face->d;
        if (h->prop == NULL) {
            hit->propId = 0;
            hit->type = 1;
            hit->material = (uint32_t *)zbspFaceMaterial(h->face);
        } else {
            hit->type = 2;
            hit->propId = h->prop->defIndex;
            hit->propObj = h->prop;
        }
    }
    if (count == 0)
        return 0;

    if (zmathVecDot(&sumNormal, &sumNormal) >= 0.001f) {
        zmathVecNormalize(&sumNormal, &sumNormal);
        zmathVecScale(&sumPos, &sumPos, 1.0f / (float)count);
        hit->normal[0] /= (float)count;
        hit->normal[1] /= (float)count;
        hit->normal[2] /= (float)count;
        hit->d /= (float)count;
    } else {                                /* opposing faces: straight "up" (+y, i.e. down in ZGE) */
        sumNormal.x = 0.0f;
        sumNormal.y = 1.0f;
        sumNormal.z = 0.0f;
        zmathVecScale(&sumPos, &sumPos, 1.0f / (float)count);
        hit->normal[0] = 0.0f;
        hit->normal[1] = 1.0f;
        hit->normal[2] = 0.0f;
        hit->d = 0.0f;
        p->vel.x = 0.0f;
        p->vel.y = 3.0f;
        p->vel.z = 0.0f;
    }
    n = sumNormal;
    impact = zmathVecDot(&n, &p->vel);
    if (impact > 0.0f)                      /* moving away */
        return 0;
    impact = -impact;
    *force = impact;
    if (zmathVecDot(&p->vel, &p->vel) >= 1.0f) {
        zmathVecScale(&n, &n, impact);
        zmathVecAdd(&p->vel, &p->vel, &n);                         /* drop the normal part */
        zmathVecScale(&p->vel, &p->vel, 1.0f - p->friction * 0.35f);
        zmathVecScale(&n, &n, p->bounce * 0.95f);
        zmathVecAdd(&p->vel, &p->vel, &n);                         /* bounce */
    } else {
        zmathVecZero(&p->vel);
        p->flags |= DYNF_REST;
    }
    p->pos = sumPos;
    hit->pos[0] = sumPos.x;
    hit->pos[1] = sumPos.y;
    hit->pos[2] = sumPos.z;
    return 1;
}

/* The segment a -> b against a normal: centre of the two projections, half of the projected length
 * plus `radius`. */
/* VA 0x00451c80, 163 bytes, [I] row, status draft */
void dynSlabProject(const RwV3d *a, const RwV3d *b, float radius, float *centre, float *half, const RwV3d *n)
{
    float da = zmathVecDot(a, n);
    float db = zmathVecDot(b, n);
    RwV3d d;
    float len;

    zmathVecNegate(&d, a);
    zmathVecAdd(&d, &d, b);
    len = zmathVecDot(&d, n);
    if (len < 0.0f)
        len = -len;
    *centre = (db + da) * 0.5f;
    *half = len * 0.5f + radius;
}

/* After dynRotateBody moved a particle: push it out of the world along the move (no velocity
 * response). Particles at rest stay. */
/* VA 0x00451d60, 1220 bytes, [I] row, status draft */
void dynPushOutParticle(DynParticle *p)
{
    ZbspSphereResult *res;
    ZbspSphereHit *h;
    RwV3d move, end, centre, contact, sumPos, sumNormal, unused, n;
    uint32_t count = 0;

    zmathVecNegate(&move, &p->prevPos);
    zmathVecAdd(&move, &move, &p->pos);
    zmathVecZero(&unused);
    zmathVecZero(&sumPos);
    zmathVecZero(&sumNormal);
    if (p->flags & DYNF_REST)
        return;
    end = p->pos;
    zmathVecAdd(&end, &end, &move);
    centre = p->prevPos;
    zmathVecAdd(&centre, &centre, &end);
    zmathVecScale(&centre, &centre, 0.5f);
    res = zbspColSphere(&centre.x, zmathVecLength(&move) * 0.5f + DYN_RADIUS, 1.0f);
    if (res == NULL)
        return;
    for (h = res->hits; h->face != NULL; h++) {
        n = dynFaceNormal(h->face);
        if (zmathVecDot(&move, &move) >= 0.1f) {
            if (!dynSweepContact(h->face, &p->prevPos, &end, &contact))
                continue;
        } else {
            contact.x = h->face->normal[0] * DYN_RADIUS + h->point.x;
            contact.y = h->face->normal[1] * DYN_RADIUS + h->point.y;
            contact.z = h->face->normal[2] * DYN_RADIUS + h->point.z;
        }
        count++;
        zmathVecAdd(&sumPos, &sumPos, &contact);
        zmathVecAdd(&sumNormal, &sumNormal, &n);
    }
    if (count != 0) {
        zmathVecScale(&sumPos, &sumPos, 1.0f / (float)count);
        p->pos = sumPos;
    }
}

/* Relaxation target of a spring: each end proposes the other end + / - rest along the current
 * direction, weighted by the spring's weight. */
/* VA 0x00452370, 258 bytes, [I] row, status draft */
void dynSolveSpring(DynSpring *s)
{
    RwV3d dir, t;

    dir.x = s->a->pos.x - s->b->pos.x;
    dir.y = s->a->pos.y - s->b->pos.y;
    dir.z = s->a->pos.z - s->b->pos.z;
    zmathVecNormalize(&dir, &dir);
    zmathVecScale(&dir, &dir, s->rest);
    t.x = dir.x + s->b->pos.x;
    t.y = dir.y + s->b->pos.y;
    t.z = dir.z + s->b->pos.z;
    zmathVecScale(&t, &t, s->weight);
    zmathVecAdd(&s->a->sum, &s->a->sum, &t);
    t.x = s->a->pos.x - dir.x;
    t.y = s->a->pos.y - dir.y;
    t.z = s->a->pos.z - dir.z;
    zmathVecScale(&t, &t, s->weight);
    zmathVecAdd(&s->b->sum, &s->b->sum, &t);
    s->a->weight += s->weight;
    s->b->weight += s->weight;
}

/* As dynSolveSpring with weight + bend x max(0, -dot(c.up, d.right)). */
/* VA 0x004524d0, 328 bytes, [I] row, status draft */
void dynSolveBendSpring(DynSpring *s)
{
    float w = s->weight;
    float k = zmathVecDot(&s->c->frame.up, &s->d->frame.right);
    RwV3d dir, t;

    if (-k > 0.0f)
        w = -k * s->bend + w;
    dir.x = s->a->pos.x - s->b->pos.x;
    dir.y = s->a->pos.y - s->b->pos.y;
    dir.z = s->a->pos.z - s->b->pos.z;
    zmathVecNormalize(&dir, &dir);
    zmathVecScale(&dir, &dir, s->rest);
    t = dir;
    zmathVecAdd(&t, &t, &s->b->pos);
    zmathVecScale(&t, &t, w);
    zmathVecAdd(&s->a->sum, &s->a->sum, &t);
    t.x = s->a->pos.x - dir.x;
    t.y = s->a->pos.y - dir.y;
    t.z = s->a->pos.z - dir.z;
    zmathVecScale(&t, &t, w);
    zmathVecAdd(&s->b->sum, &s->b->sum, &t);
    s->a->weight = w + s->a->weight;
    s->b->weight = w + s->b->weight;
}

/* VA 0x00452680, 1 bytes, [I] row, status draft */
void dynSolveAngleSpring(DynSpring *s)
{
    (void)s;
}

/* ------------------------------------------------------------------ the bike */

/* A triangle: (0, 40, 0) and (+/-30, 0, 0), mass 0.1, bounce 0.8, friction 0.4, springs of weight 16. */
/* VA 0x004526a0, 211 bytes, [I] row, status draft */
void *dynCreateBike(void)
{
    DynBody *b = dynCreate();
    DynParticleDesc d;

    if (b != NULL) {
        d.type = DYN_BIKE;
        d.mass = 0.1f;
        d.bounce = 0.8f;
        d.friction = 0.4f;
        d.pos.x = 0.0f;   d.pos.y = 40.0f; d.pos.z = 0.0f;
        dynAddParticle(b, &d);
        d.pos.x = 30.0f;  d.pos.y = 0.0f;  d.pos.z = 0.0f;
        dynAddParticle(b, &d);
        d.pos.x = -30.0f; d.pos.y = 0.0f;  d.pos.z = 0.0f;
        dynAddParticle(b, &d);
        dynAddSpring(b, 0, 1, 16.0f);
        dynAddSpring(b, 0, 2, 16.0f);
        dynAddSpring(b, 1, 2, 16.0f);
    }
    return b;
}

/* Place the bike triangle on the bike (matrix m at the actor's position) with the crash velocity plus
 * the spin's tangential part, both halved. physics.c passes no spin. */
/* VA 0x004527c0, 503 bytes, [I] row, status draft */
void dynStartBike(void *body, ZActor *actor, const RwMatrix *m, const float *vel, const int16_t *spin)
{
    DynBody *b = (DynBody *)body;
    RwV3d *at = zactorGetPos(actor);
    RwV3d local, off, turned;
    uint32_t i;

    for (i = 0; i < b->numParticles; i++) {
        DynParticle *p = P(b, i);

        if (i == 0) {
            local.x = 0.0f;   local.z = 0.0f; local.y = 40.0f;
        } else if (i == 1) {
            local.y = 0.0f;   local.z = 0.0f; local.x = 30.0f;
        } else if (i == 2) {
            local.y = 0.0f;   local.z = 0.0f; local.x = -30.0f;
        }
        zmathTransformPoint(&off, m, &local);
        p->pos.x = at->x + off.x;
        p->pos.y = at->y + off.y;
        p->pos.z = at->z + off.z;
        turned = off;
        zmathVecRotateXYZ(&turned, &turned, (const uint16_t *)spin);
        p->vel.x = turned.x - off.x + vel[0];
        p->vel.y = turned.y - off.y + vel[1];
        p->vel.z = turned.z - off.z + vel[2];
        zmathVecScale(&p->vel, &p->vel, 0.5f);
        p->prevPos = p->pos;
        zmathMatrixCopy(&p->frame, m);
        p->flags = 0;
    }
    b->centre = *at;
    actor->rot[0] = actor->rot[1] = actor->rot[2] = 0;
    actor->heading = 0;
}

/* Step the bike and put the actor on it: the triangle's frame with its axes rotated (right <- at,
 * up <- right, at <- up), at the body's centre. */
/* VA 0x00452a40, 251 bytes, [I] row, status draft */
void dynStepBike(void *body, ZActor *actor)
{
    DynBody *b = (DynBody *)body;
    RwMatrix f, m;
    int16_t zero[4];
    int i;

    dynStep(b);
    dynFrameFromParticles(b, 0, 1, 2, &f);
    m = f;
    m.right = f.at;
    m.up = f.right;
    m.at = f.up;
    zmathVecZero(&m.pos);
    for (i = 0; i < 3; i++)
        zmathMatrixCopy(&P(b, i)->frame, &m);
    zero[0] = zero[1] = zero[2] = 0;
    zactorSetOrientation(actor, zero, &m.up, &m.right, &m);
    zactorSetPos(actor, &b->centre);
    zactorSetHeight(actor, 0.0f);
}

/* ------------------------------------------------------------------ the rider */

/* Put the rider particles on the skeleton's joints (the actor's current pose) and clear the angles of
 * the channels the rag doll will drive (decoder block 1). */
/* VA 0x00452b80, 549 bytes, [I] row, status draft */
void dynPoseFromActor(DynBody *b, ZActor *actor)
{
    int16_t (*rot)[3] = actor->anim->block[1].rot;
    static const uint8_t cleared[15] = { 0, 1, 2, 11, 14, 8, 5, 4, 7, 3, 6, 10, 13, 9, 12 };
    RwV3d local;
    int i;

    for (i = 0; i < 15; i++)
        dynZeroAngles(rot[cleared[i]]);
    for (i = 0; i < (int)b->numParticles; i++) {
        zmathVecZero(&local);
        if (i >= DYNP_RIDER_COUNT)
            continue;
        if (i == DYNP_HEAD)
            local.y = -25.0f;               /* the top of the head */
        zactorGetBonePos(actor, s_dynParticleBone[i], &local, &P(b, i)->pos, 1);
    }
}

/* The rider body: 14 particles (pelvis 0.4, the rest 0.1; bounce 0.6, Super Crash 1.75; friction 1.0)
 * on the skeleton's joints, lifted so the lowest is at y = 0 (if below 500); 21 springs (the torso box,
 * the limbs, the neck) and 6 limits (knees, elbows). */
/* VA 0x00452e80, 1200 bytes, [I] row, status draft */
void *dynCreateRider(void *actor, int superCrash)
{
    DynBody *b = dynCreate();
    DynParticleDesc d;
    DynLimitDesc l;
    float lowest = 500.0f;
    uint32_t i;

    if (b != NULL) {
        d.type = DYN_RIDER;                 /* (position left unset: dynPoseFromActor places them) */
        d.mass = 0.4f;
        d.bounce = (superCrash & 1) ? 1.75f : 0.6f;
        d.friction = 1.0f;
        dynAddParticle(b, &d);              /* 0 pelvis */
        d.mass = 0.1f;
        for (i = 1; i < DYNP_RIDER_COUNT; i++)
            dynAddParticle(b, &d);
        dynPoseFromActor(b, (ZActor *)actor);

        /* torso */
        dynAddSpring(b, 0, 1, 24.0f);
        dynAddSpring(b, 0, 2, 24.0f);
        dynAddSpring(b, 1, 2, 24.0f);
        dynAddSpring(b, 7, 0, 48.0f);
        dynAddSpring(b, 8, 0, 48.0f);
        dynAddSpring(b, 7, 8, 24.0f);
        dynAddSpring(b, 7, 1, 48.0f);
        dynAddSpring(b, 7, 2, 48.0f);
        dynAddSpring(b, 8, 1, 48.0f);
        dynAddSpring(b, 8, 2, 48.0f);
        /* limbs */
        s_sprLThigh = dynAddSpring(b, 2, 4, 24.0f);
        s_sprLShin = dynAddSpring(b, 4, 6, 24.0f);
        s_sprRThigh = dynAddSpring(b, 1, 3, 24.0f);
        s_sprRShin = dynAddSpring(b, 3, 5, 24.0f);
        s_sprLUpperArm = dynAddSpring(b, 8, 10, 24.0f);
        s_sprLForeArm = dynAddSpring(b, 10, 12, 24.0f);
        s_sprRUpperArm = dynAddSpring(b, 7, 9, 24.0f);
        s_sprRForeArm = dynAddSpring(b, 9, 11, 24.0f);
        /* neck (all three indices go to s_sprNeck[0], as compiled) */
        s_sprNeck[0] = dynAddSpring(b, 13, 0, 4.8f);
        s_sprNeck[0] = dynAddSpring(b, 13, 7, 24.0f);
        s_sprNeck[0] = dynAddSpring(b, 13, 8, 24.0f);

        /* elbows: keep each elbow on its side of the shoulders */
        l.axisA = DYN_AXIS_NRIGHT;
        l.axisB = DYN_AXIS_UP;
        l.a = P(b, 8);
        l.b = P(b, 9);
        l.pivot = P(b, 7);
        l.frameA = P(b, 8);
        l.frameB = P(b, 9);
        l.side = DYN_SIDE_FRONT;
        l.strength = 1.2f;
        s_limRElbow = dynAddLimit(b, &l);
        l.axisA = DYN_AXIS_RIGHT;
        l.axisB = DYN_AXIS_UP;
        l.a = P(b, 7);
        l.b = P(b, 10);
        l.pivot = P(b, 8);
        l.frameA = P(b, 7);
        l.frameB = P(b, 10);
        l.side = DYN_SIDE_FRONT;
        l.strength = 1.2f;
        s_limLElbow = dynAddLimit(b, &l);
        /* knees */
        l.axisA = DYN_AXIS_NRIGHT;
        l.axisB = DYN_AXIS_UP;
        l.a = P(b, 2);
        l.b = P(b, 3);
        l.pivot = P(b, 1);
        l.frameA = P(b, 2);
        l.frameB = P(b, 3);
        l.side = DYN_SIDE_FRONT;
        l.strength = 0.5f;
        s_limRKnee = dynAddLimit(b, &l);
        l.axisA = DYN_AXIS_RIGHT;
        l.axisB = DYN_AXIS_UP;
        l.a = P(b, 1);
        l.b = P(b, 4);
        l.pivot = P(b, 2);
        l.frameA = P(b, 1);
        l.frameB = P(b, 4);
        l.side = DYN_SIDE_FRONT;
        l.strength = 0.5f;
        s_limLKnee = dynAddLimit(b, &l);
        l.axisA = DYN_AXIS_UP;
        l.axisB = DYN_AXIS_NUP;
        l.a = P(b, 2);
        l.b = P(b, 3);
        l.pivot = P(b, 1);
        l.frameA = P(b, 2);
        l.frameB = P(b, 3);
        l.side = DYN_SIDE_BEHIND;
        l.strength = 1.0f;
        s_limRKneeB = dynAddLimitB(b, &l);
        l.axisA = DYN_AXIS_UP;
        l.axisB = DYN_AXIS_UP;
        l.a = P(b, 1);
        l.b = P(b, 4);
        l.pivot = P(b, 2);
        l.frameA = P(b, 1);
        l.frameB = P(b, 4);
        l.side = DYN_SIDE_BEHIND;
        l.strength = 1.0f;
        s_limLKneeB = dynAddLimitB(b, &l);

        for (i = 0; i < b->numParticles; i++)
            if (P(b, i)->pos.y < lowest)
                lowest = P(b, i)->pos.y;
        for (i = 0; i < b->numParticles; i++)
            P(b, i)->pos.y -= lowest;
    }
    s_dynBodies[s_dynNumBodies++] = b;
    if (s_dynNumBodies > 1)
        s_dynNumBodies = 0;
    return b;
}

/* Every dyn* entry point of the rider starts here: the per-rider flags are indexed by the body's slot. */
/* VA 0x00453460, 34 bytes, [I] row, status draft */
void dynSelect(void *body)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (s_dynBodies[i] == (DynBody *)body) {
            s_dynCur = i;
            return;
        }
    }
}

/* The crash starts: particles back on the skeleton, all moving (DYN_RIDER) with half the rider's
 * velocity, the centre at the pelvis, the actor's rotation cleared; arms and limbs back to normal,
 * no grab. One step without limits and stick input follows (starting). */
/* VA 0x004534a0, 232 bytes, [I] row, status draft */
void dynStartRider(void *body, ZActor *actor, const RwMatrix *m, const float *vel, const int16_t *spin)
{
    DynBody *b = (DynBody *)body;
    int i;

    (void)m;
    (void)spin;
    zactorGetPos(actor);
    dynPoseFromActor(b, actor);
    for (i = 0; i < (int)b->numParticles; i++) {
        DynParticle *p = P(b, i);

        p->vel.x = vel[0];
        p->vel.y = vel[1];
        p->vel.z = vel[2];
        zmathVecScale(&p->vel, &p->vel, 0.5f);
        p->prevPos = p->pos;
        p->flags = 0;
        p->type = DYN_RIDER;
    }
    dynSelect(b);
    b->centre = P(b, DYNP_PELVIS)->pos;
    actor->rot[0] = actor->rot[1] = actor->rot[2] = 0;
    actor->heading = 0;
    b->starting = 1;
    dynUnbrace(b, actor);
    dynRelease(b, actor);
    dynUntuck(b, actor);
    CUR(s_dynGrabbed) = 0;
}

/* ACT_RAGDOLL held: from now on the wrists catch the nearest edge in reach (dynGrabEdges), once per
 * crash. Undoes a tuck, the arm brace and the stick input. (DM-047: dynStart) */
/* VA 0x004535d0, 148 bytes, [I] row, status draft */
void dynGrab(void *body, void *actor)
{
    dynSelect(body);
    if (CUR(s_dynGrabbed))
        return;
    CUR(s_dynGrabOn) = 1;
    if (CUR(s_dynTucked))
        dynUntuck(body, actor);
    if (CUR(s_dynBraced))
        dynUnbrace(body, actor);
    if (CUR(s_dynPitchOn))
        dynClearPitchInput(body, actor);
    if (CUR(s_dynRollOn))
        dynClearRollInput(body, actor);
}

/* Look for grind edges within 400 of the right knee; each wrist (particles 11, 12) whose closest
 * edge point is nearer than 120 is pinned there, 10 units above it: the rider hangs. */
/* VA 0x004536a0, 360 bytes, [I] row, status draft */
void dynGrabEdges(DynBody *b)
{
    NgColEdge **edges, **e;
    uint32_t best, d2;
    RwV3d closest, keep;
    int i;

    if (CUR(s_dynHanging) || CUR(s_dynGrabbed))
        return;
    edges = zbspFindEdgesNear(&P(b, DYNP_RKNEE)->pos.x, 400.0f);
    if (edges == NULL)
        return;
    for (i = DYNP_RWRIST; i <= DYNP_LWRIST; i++) {
        DynParticle *p = P(b, i);

        best = 120 * 120;
        for (e = edges; *e != NULL; e++) {
            int dx, dy, dz;

            zbspEdgeClosestPoint((*e)->v0, (*e)->v1, (*e)->dir, &p->pos.x, &closest.x);
            dx = (int)(closest.x - p->pos.x);       /* __ftol of the three differences [I] */
            dy = (int)(closest.y - p->pos.y);
            dz = (int)(closest.z - p->pos.z);
            d2 = (uint32_t)(dz * dz + dy * dy + dx * dx);
            if (d2 < best) {
                keep = closest;
                best = d2;
            }
        }
        if (best < 120 * 120) {
            p->pos.x = keep.x;
            p->pos.y = g_dynGrabDrop + keep.y;
            p->pos.z = keep.z;
            p->type = DYN_FIXED;
            CUR(s_dynHanging) = 1;
            CUR(s_dynGrabbed) = 1;
        }
    }
}

/* (DM-047: dynIsRunning) */
/* VA 0x00453870, 13 bytes, [I] row, status draft */
int dynIsHanging(void)
{
    return CUR(s_dynHanging);
}

/* Let go: both wrists move again, no more grabbing. (DM-047: dynStop) */
/* VA 0x00453890, 56 bytes, [I] row, status draft */
void dynRelease(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;

    (void)actor;
    dynSelect(body);
    P(b, DYNP_RWRIST)->type = DYN_RIDER;
    P(b, DYNP_LWRIST)->type = DYN_RIDER;
    CUR(s_dynGrabOn) = 0;
    CUR(s_dynHanging) = 0;
}

/* a = a x 0.8 + b x 0.2 (b is scaled in place) */
/* VA 0x004538e0, 48 bytes, [I] row, status draft */
void dynLerpTo(RwV3d *a, RwV3d *b)
{
    zmathVecScale(b, b, 0.2f);
    zmathVecScale(a, a, 0.8f);
    zmathVecAdd(a, a, b);
}

/* Arms held out: the wrists towards 54, the elbows towards 29 units along the shoulders' at axis. */
/* VA 0x00453920, 309 bytes, [I] row, status draft */
void dynBracePose(DynBody *b)
{
    RwV3d local, r, l;

    dynSelect(b);
    local.x = 0.0f;
    local.y = 0.0f;
    local.z = 54.0f;
    zmathTransformPoint(&r, &P(b, DYNP_RSHOULDER)->frame, &local);
    l = r;
    zmathVecAdd(&r, &r, &P(b, DYNP_RSHOULDER)->pos);
    zmathVecAdd(&l, &l, &P(b, DYNP_LSHOULDER)->pos);
    dynLerpTo(&P(b, DYNP_RWRIST)->pos, &r);
    dynLerpTo(&P(b, DYNP_LWRIST)->pos, &l);
    local.x = 0.0f;
    local.y = 0.0f;
    local.z = 29.0f;
    zmathTransformPoint(&r, &P(b, DYNP_RSHOULDER)->frame, &local);
    l = r;
    zmathVecAdd(&r, &r, &P(b, DYNP_RSHOULDER)->pos);
    zmathVecAdd(&l, &l, &P(b, DYNP_LSHOULDER)->pos);
    dynLerpTo(&P(b, DYNP_RELBOW)->pos, &r);
    dynLerpTo(&P(b, DYNP_LELBOW)->pos, &l);
}

/* Unreferenced: hold the arms out (dynBracePose each step), the elbow limits off. */
/* VA 0x00453ab0, 130 bytes, [I] row (DM-058: created), status draft */
void dynBrace(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;

    dynSelect(body);
    if (CUR(s_dynTucked))
        dynUntuck(body, actor);
    if (CUR(s_dynGrabOn))
        dynRelease(body, actor);
    CUR(s_dynBraced) = 1;
    b->limit[s_limRElbow].strength = 0.0f;
    b->limit[s_limLElbow].strength = 0.0f;
}

/* Arm springs and elbow limits back to normal. (DM-047: dynRestoreLimbs) */
/* VA 0x00453b70, 136 bytes, [I] row, status draft */
void dynUnbrace(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;

    (void)actor;
    dynSelect(body);
    b->spring[s_sprRUpperArm].weight = 24.0f;
    b->spring[s_sprLUpperArm].weight = 24.0f;
    b->spring[s_sprRForeArm].weight = 24.0f;
    b->spring[s_sprLForeArm].weight = 24.0f;
    b->limit[s_limRElbow].strength = 1.0f;
    b->limit[s_limLElbow].strength = 1.0f;
    CUR(s_dynBraced) = 0;
}

/* tuck targets: (particle, frame particle, offset in that frame) */
typedef struct DynTuckPoint {
    uint8_t particle, frame, origin;
    float   x, y, z;
} DynTuckPoint;

static const DynTuckPoint s_dynTuck[8] = {
    { DYNP_RELBOW,  DYNP_PELVIS, DYNP_RSHOULDER, 16.76f,  -25.205f, 13.824f },
    { DYNP_LELBOW,  DYNP_PELVIS, DYNP_LSHOULDER, -8.498f, -29.081f, 13.993f },
    { DYNP_RWRIST,  DYNP_PELVIS, DYNP_RSHOULDER, 14.058f, -26.699f, 36.762f },
    { DYNP_LWRIST,  DYNP_PELVIS, DYNP_LSHOULDER, -6.603f, -32.332f, 36.852f },
    { DYNP_RKNEE,   DYNP_RHIP,   DYNP_RHIP,      0.0f,    29.405f,  40.043f },
    { DYNP_LKNEE,   DYNP_LHIP,   DYNP_LHIP,      0.0f,    27.904f,  13.288f },
    { DYNP_RANKLE,  DYNP_RHIP,   DYNP_RHIP,      0.0f,    -7.461f,  60.906f },
    { DYNP_LANKLE,  DYNP_LHIP,   DYNP_LHIP,      0.0f,    -9.087f,  37.628f },
};

/* The tuck: elbows, wrists, knees and ankles 20 % of the way to fixed points in the pelvis / hip
 * frames each step (a ball). The exe writes the eight blocks out; the values are s_dynTuck. */
/* VA 0x00453c30, 660 bytes, [I] row, status draft */
void dynTuckPose(DynBody *b)
{
    RwV3d local, t;
    int i;

    for (i = 0; i < 8; i++) {
        const DynTuckPoint *k = &s_dynTuck[i];

        local.x = k->x;
        local.y = k->y;
        local.z = k->z;
        zmathTransformPoint(&t, &P(b, k->frame)->frame, &local);
        zmathVecAdd(&t, &t, &P(b, k->origin)->pos);
        dynLerpTo(&P(b, k->particle)->pos, &t);
    }
}

/* ACT_GO_LIMP: curl up. Stops grabbing and the arm brace, frees all limb springs and limits (their
 * weights 0) and pulls the limbs in (dynTuckPose) - unless a particle is touching something.
 * (DM-047: dynGoLimp) */
/* VA 0x00453f80, 338 bytes, [I] row, status draft */
void dynTuck(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;
    uint32_t i;

    dynSelect(body);
    if (CUR(s_dynBraced))
        dynUnbrace(body, actor);
    if (CUR(s_dynGrabOn))
        dynRelease(body, actor);
    CUR(s_dynTucked) = 1;
    for (i = 0; i < b->numParticles; i++)
        if (P(b, i)->flags & DYNF_CONTACT)
            return;
    b->spring[s_sprRUpperArm].weight = 0.0f;
    b->spring[s_sprLUpperArm].weight = 0.0f;
    b->spring[s_sprRForeArm].weight = 0.0f;
    b->spring[s_sprLForeArm].weight = 0.0f;
    b->spring[s_sprRThigh].weight = 0.0f;
    b->spring[s_sprLThigh].weight = 0.0f;
    b->spring[s_sprRShin].weight = 0.0f;
    b->spring[s_sprLShin].weight = 0.0f;
    b->limit[s_limRKnee].strength = 0.0f;
    b->limit[s_limLKnee].strength = 0.0f;
    b->limit[s_limRKneeB].strength = 0.0f;
    b->limit[s_limLKneeB].strength = 0.0f;
    b->limit[s_limRElbow].strength = 0.0f;
    b->limit[s_limLElbow].strength = 0.0f;
}

/* All limits back to 1 (the knees' were 0.5 at creation), limb springs to 24, the neck spring
 * s_sprNeck[0] to 4.8 (it was created at 24: the index bug turns this into a weaker neck after the
 * first tuck [V]). (DM-047: dynRestoreJoints) */
/* VA 0x00454140, 311 bytes, [I] row, status draft */
void dynUntuck(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;

    (void)actor;
    dynSelect(body);
    b->limit[s_limRKneeB].strength = 1.0f;
    b->limit[s_limLKneeB].strength = 1.0f;
    b->limit[s_limRKnee].strength = 1.0f;
    b->limit[s_limLKnee].strength = 1.0f;
    b->limit[s_limRElbow].strength = 1.0f;
    b->limit[s_limLElbow].strength = 1.0f;
    b->spring[s_sprNeck[0]].weight = 4.8f;
    b->spring[s_sprNeck[1]].weight = 24.0f;
    b->spring[s_sprNeck[2]].weight = 24.0f;
    b->spring[s_sprRUpperArm].weight = 24.0f;
    b->spring[s_sprLUpperArm].weight = 24.0f;
    b->spring[s_sprRForeArm].weight = 24.0f;
    b->spring[s_sprLForeArm].weight = 24.0f;
    b->spring[s_sprRThigh].weight = 24.0f;
    b->spring[s_sprLThigh].weight = 24.0f;
    b->spring[s_sprRShin].weight = 24.0f;
    b->spring[s_sprLShin].weight = 24.0f;
    CUR(s_dynTucked) = 0;
}

/* VA 0x004542d0, 15 bytes, [I] row, status draft */
void dynClearLimits(DynBody *b)
{
    b->numLimits = 0;
}

/* Unreferenced: drop the limits (dynStepRider then clears them every step). */
/* VA 0x004542f0, 31 bytes, [I] row (DM-058: created), status draft */
void dynSetLimitsOff(void *body, void *actor)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynNoLimits) = 1;
}

/* Unreferenced: the six limits back, every particle's bounce 0.6 and friction 1.0. */
/* VA 0x00454320, 100 bytes, [I] row (DM-058: created), status draft */
void dynSetLimitsOn(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;
    uint32_t i;

    (void)actor;
    dynSelect(body);
    b->numLimits = 6;
    for (i = 0; i < b->numParticles; i++) {
        P(b, i)->bounce = 0.6f;
        P(b, i)->friction = 1.0f;
    }
    CUR(s_dynNoLimits) = 0;
}

/* Sticky Crash: a particle that touched something stops for good. */
/* VA 0x004543b0, 59 bytes, [I] row, status draft */
void dynStickParticles(DynBody *b)
{
    uint32_t i;

    for (i = 0; i < b->numParticles; i++) {
        DynParticle *p = P(b, i);

        if (p->flags & DYNF_CONTACT) {
            p->type = DYN_FIXED;
            p->vel.x = p->vel.y = p->vel.z = 0.0f;
        }
    }
}

/* VA 0x00454400, 31 bytes, [I] row, status draft */
void dynSetStickyCrash(void *body, void *actor)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynSticky) = 1;
}

/* VA 0x00454430, 86 bytes, [I] row, status draft */
void dynSetNormalCrash(void *body, void *actor)
{
    DynBody *b = (DynBody *)body;
    uint32_t i;

    (void)actor;
    dynSelect(body);
    b->numLimits = 6;
    for (i = 0; i < b->numParticles; i++)
        P(b, i)->type = DYN_RIDER;
    CUR(s_dynSticky) = 0;
}

/* Turn particles 1..12 about the pelvis by `rot` (in the pelvis frame) and push them out of the world:
 * the stick spins the tumbling body. The head (13) is left to its springs. */
/* VA 0x004544b0, 226 bytes, [I] row, status draft */
void dynRotateBody(DynBody *b, void *actor, const RwMatrix *rot)
{
    RwMatrix m, inv;
    RwV3d v;
    int i;

    (void)actor;
    zmathMatrixIdentity(&m);
    zmathMatrixIdentity(&inv);
    dynTranspose3(&P(b, DYNP_PELVIS)->frame, &inv);
    zmathMatrixCopy(&m, &P(b, DYNP_PELVIS)->frame);
    zmathMatrixMultiply(&m, &m, rot);
    zmathMatrixMultiply(&m, &m, &inv);
    for (i = 1; i <= 12; i++) {
        DynParticle *p = P(b, i);

        p->prevPos = p->pos;
        dynVecSub(&p->pos.x, &P(b, DYNP_PELVIS)->pos.x, &v.x);
        zmathTransformPoint(&p->pos, &m, &v);
        zmathVecAdd(&p->pos, &p->pos, &P(b, DYNP_PELVIS)->pos);
        dynPushOutParticle(p);
        p->prevPos = p->pos;
    }
}

/* VA 0x004545e0, 92 bytes, [I] row, status draft */
void dynApplyPitch(DynBody *b, void *actor)
{
    int16_t euler[4];
    RwMatrix m;

    euler[1] = euler[2] = euler[3] = 0;
    euler[0] = CUR(s_dynPitch);
    zmathMatrixIdentity(&m);
    zmathMatrixRotateXYZ(&m, &m, euler);
    dynRotateBody(b, actor, &m);
}

/* VA 0x00454660, 43 bytes, [I] row, status draft */
void dynSetPitchInput(void *body, void *actor, int16_t angle)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynPitchOn) = 1;
    CUR(s_dynPitch) = angle;
}

/* VA 0x004546a0, 31 bytes, [I] row, status draft */
void dynClearPitchInput(void *body, void *actor)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynPitchOn) = 0;
}

/* VA 0x004546d0, 92 bytes, [I] row, status draft */
void dynApplyRoll(DynBody *b, void *actor)
{
    int16_t euler[4];
    RwMatrix m;

    euler[0] = euler[2] = euler[3] = 0;
    euler[1] = CUR(s_dynRoll);              /* about the pelvis frame's y axis */
    zmathMatrixIdentity(&m);
    zmathMatrixRotateXYZ(&m, &m, euler);
    dynRotateBody(b, actor, &m);
}

/* VA 0x00454750, 43 bytes, [I] row, status draft */
void dynSetRollInput(void *body, void *actor, int16_t angle)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynRollOn) = 1;
    CUR(s_dynRoll) = angle;
}

/* VA 0x00454790, 31 bytes, [I] row, status draft */
void dynClearRollInput(void *body, void *actor)
{
    (void)actor;
    dynSelect(body);
    CUR(s_dynRollOn) = 0;
}

/* One crash frame of the rider: the player's actions, the step, then the segment frames from the
 * particles and from them the pose: the pelvis frame becomes the actor's orientation (Hips channel),
 * every other channel the rotation of its segment relative to its parent's (decoder block 1, which
 * physics.c copies into the output pose). Hands and feet are straightened. The actor stands at a
 * mix of the pelvis and hips (the y and z terms use the wrong particles, as compiled [V]). */
/* VA 0x004547c0, 1784 bytes, [I] row, status draft */
void dynStepRider(void *body, ZActor *actor)
{
    DynBody *b = (DynBody *)body;
    int16_t (*rot)[3] = actor->anim->block[1].rot;
    RwMatrix shoulders, pelvis, thighL, thighR, upperL, upperR, inv, rel;
    RwV3d at;

    dynSelect(body);
    if (CUR(s_dynSticky))
        dynStickParticles(b);
    if (CUR(s_dynNoLimits))
        dynClearLimits(b);
    if (!CUR(s_dynSticky)) {
        if (CUR(s_dynPitchOn) && !b->starting)
            dynApplyPitch(b, actor);
        if (CUR(s_dynRollOn) && !b->starting)
            dynApplyRoll(b, actor);
    }
    if (CUR(s_dynBraced))
        dynBracePose(b);
    if (CUR(s_dynTucked))
        dynTuckPose(b);
    if (CUR(s_dynGrabOn))
        dynGrabEdges(b);
    dynStep(b);

    /* segment frames */
    dynFrameFromPairs(b, DYNP_RSHOULDER, DYNP_LSHOULDER, DYNP_LSHOULDER, DYNP_PELVIS, &shoulders);
    dynFrameFromPairs(b, DYNP_RHIP, DYNP_LHIP, DYNP_PELVIS, DYNP_RHIP, &pelvis);
    dynLimbFrame(b, DYNP_LHIP, DYNP_LKNEE, &pelvis, &thighL);
    dynLimbFrame(b, DYNP_RHIP, DYNP_RKNEE, &pelvis, &thighR);
    dynLimbFrameMirror(b, DYNP_LSHOULDER, DYNP_LELBOW, &shoulders, &upperL);
    dynLimbFrameMirror(b, DYNP_RSHOULDER, DYNP_RELBOW, &shoulders, &upperR);
    dynFrameFromPairs(b, DYNP_RSHOULDER, DYNP_LSHOULDER, DYNP_HEAD, DYNP_RSHOULDER, &s_dynHead);
    if (!CUR(s_dynNoLimits) && !CUR(s_dynTucked)) {
        dynHingeFrame(b, DYNP_LKNEE, DYNP_LANKLE, &thighL, &s_dynShinL, 1);
        dynHingeFrame(b, DYNP_RKNEE, DYNP_RANKLE, &thighR, &s_dynShinR, 0);
        dynElbowFrame(b, DYNP_LELBOW, DYNP_LWRIST, &upperL, &s_dynForeArmL, 1);
        dynElbowFrame(b, DYNP_RELBOW, DYNP_RWRIST, &upperR, &s_dynForeArmR, 0);
    } else {
        dynLimbFrame(b, DYNP_LKNEE, DYNP_LANKLE, &thighL, &s_dynShinL);
        dynLimbFrame(b, DYNP_RKNEE, DYNP_RANKLE, &thighR, &s_dynShinR);
        dynLimbFrame(b, DYNP_LELBOW, DYNP_LWRIST, &upperL, &s_dynForeArmL);
        dynLimbFrame(b, DYNP_RELBOW, DYNP_RWRIST, &upperR, &s_dynForeArmR);
    }
    if (CUR(s_dynBraced)) {
        dynBracePose(b);
        dynUnbrace(b, actor);
    }
    if (CUR(s_dynTucked)) {
        dynTuckPose(b);
        dynUntuck(b, actor);
    }
    zmathMatrixCopy(&P(b, DYNP_RSHOULDER)->frame, &shoulders);
    zmathMatrixCopy(&P(b, DYNP_LSHOULDER)->frame, &shoulders);
    zmathMatrixCopy(&P(b, DYNP_PELVIS)->frame, &pelvis);
    zmathMatrixCopy(&P(b, DYNP_RHIP)->frame, &pelvis);
    zmathMatrixCopy(&P(b, DYNP_LHIP)->frame, &pelvis);
    zmathMatrixCopy(&P(b, DYNP_HEAD)->frame, &s_dynHead);
    zmathMatrixCopy(&P(b, DYNP_LKNEE)->frame, &thighL);
    zmathMatrixCopy(&P(b, DYNP_RKNEE)->frame, &thighR);
    zmathMatrixCopy(&P(b, DYNP_LANKLE)->frame, &s_dynShinL);
    zmathMatrixCopy(&P(b, DYNP_RANKLE)->frame, &s_dynShinR);
    zmathMatrixCopy(&P(b, DYNP_RELBOW)->frame, &upperR);
    zmathMatrixCopy(&P(b, DYNP_LELBOW)->frame, &upperL);
    zmathMatrixCopy(&P(b, DYNP_RWRIST)->frame, &s_dynForeArmR);
    zmathMatrixCopy(&P(b, DYNP_LWRIST)->frame, &s_dynForeArmL);

    /* the pose: channel = segment relative to its parent segment */
    dynSetActorFrame(actor, &pelvis, rot[0]);                       /* Hips */
    dynTranspose3(&shoulders, &inv);
    dynMul3(&rel, &inv, &s_dynHead);    dynMatrixToEuler(&rel, rot[2]);    /* Head */
    dynMul3(&rel, &inv, &upperL);       dynMatrixToEuler(&rel, rot[3]);    /* LeftUpArm */
    dynMul3(&rel, &inv, &upperR);       dynMatrixToEuler(&rel, rot[6]);    /* RightUpArm */
    dynTranspose3(&pelvis, &inv);
    dynMul3(&rel, &inv, &thighL);       dynMatrixToEuler(&rel, rot[9]);    /* LeftUpLeg */
    dynMul3(&rel, &inv, &thighR);       dynMatrixToEuler(&rel, rot[12]);   /* RightUpLeg */
    dynMul3(&rel, &inv, &shoulders);    dynMatrixToEuler(&rel, rot[1]);    /* Chest */
    dynTranspose3(&thighR, &inv);
    dynMul3(&rel, &inv, &s_dynShinR);   dynMatrixToEuler(&rel, rot[13]);   /* RightLowLeg */
    dynTranspose3(&thighL, &inv);
    dynMul3(&rel, &inv, &s_dynShinL);   dynMatrixToEuler(&rel, rot[10]);   /* LeftLowLeg */
    dynTranspose3(&upperR, &inv);
    dynMul3(&rel, &inv, &s_dynForeArmR); dynMatrixToEuler(&rel, rot[7]);   /* RightLowArm */
    dynTranspose3(&upperL, &inv);
    dynMul3(&rel, &inv, &s_dynForeArmL); dynMatrixToEuler(&rel, rot[4]);   /* LeftLowArm */
    dynZeroAngles(rot[5]);                                          /* LeftHand */
    dynZeroAngles(rot[11]);                                         /* LeftFoot */
    dynZeroAngles(rot[8]);                                          /* RightHand */
    dynZeroAngles(rot[14]);                                         /* RightFoot */

    at.x = P(b, 0)->pos.x * 0.5f + P(b, 1)->pos.x * 0.25f + P(b, 2)->pos.x * 0.25f;
    at.y = P(b, 1)->pos.y * 0.5f + P(b, 1)->pos.y * 0.25f + P(b, 2)->pos.y * 0.25f;
    at.z = P(b, 2)->pos.z * 0.5f + P(b, 2)->pos.z * 0.25f + P(b, 1)->pos.z * 0.25f;
    zactorSetHeight(actor, 0.0f);
    zactorSetPos(actor, &at);
    b->starting = 0;
}

/* ------------------------------------------------------------------ segment frames */

/* right = i - j, up = k - l; at = up x right, up = right x at (normalised), no translation. */
/* VA 0x00455080, 136 bytes, [I] row, status draft */
void dynFrameFromPairs(DynBody *b, int i, int j, int k, int l, RwMatrix *out)
{
    zmathMatrixClearTranslation(out);
    dynVecSub(&P(b, i)->pos.x, &P(b, j)->pos.x, &out->right.x);
    dynVecSub(&P(b, k)->pos.x, &P(b, l)->pos.x, &out->up.x);
    zmathVecNormalize(&out->right, &out->right);
    dynCross(&out->right, &out->up.x, &out->at);
    zmathVecNormalize(&out->at, &out->at);
    dynCross(&out->at, &out->right.x, &out->up);
}

/* A limb segment from joint i to joint j (up = i - j): its right axis is a blend of the parent's
 * axes weighted by how much the segment lies along each: the parent's at (sign by the parent's right
 * component) and, for the other two terms, the parent's right twice (the exe's operands [V]). */
static void dynBlendFrame(DynBody *b, int i, int j, const RwMatrix *parent, RwMatrix *out, int mirror)
{
    RwV3d u, w1, w2, w3;
    float dr, du, da, sum;

    zmathMatrixIdentity(out);
    dynVecSub(&P(b, i)->pos.x, &P(b, j)->pos.x, &out->up.x);
    zmathVecNormalize(&out->up, &out->up);
    dr = zmathVecDot(&parent->right, &out->up);
    du = zmathVecDot(&parent->up, &out->up);
    da = zmathVecDot(&parent->at, &out->up);
    if (!mirror ? (dr >= 0.0f) : (dr > 0.0f)) {
        w1 = parent->at;
        if (mirror)
            zmathVecScale(&w1, &w1, -1.0f);
    } else {
        w1 = parent->at;
        if (!mirror)
            zmathVecScale(&w1, &w1, -1.0f);
        dr = -dr;
    }
    if (du <= 0.0f)
        du = -du;
    w2 = parent->right;
    if (da <= 0.0f)
        da = -da;
    w3 = parent->right;
    sum = da + du + dr;
    zmathVecScale(&w1, &w1, dr / sum);
    zmathVecScale(&w2, &w2, du / sum);
    zmathVecScale(&w3, &w3, da / sum);
    u = w1;
    zmathVecAdd(&u, &u, &w2);
    zmathVecAdd(&u, &u, &w3);
    zmathVecNormalize(&u, &u);
    out->right = u;
    dynCross(&out->right, &out->up.x, &out->at);
    zmathVecNormalize(&out->at, &out->at);
    dynCross(&out->up, &out->at.x, &out->right);
    zmathVecNormalize(&out->right, &out->right);
}

/* VA 0x00455140, 580 bytes, [I] row, status draft */
void dynLimbFrame(DynBody *b, int i, int j, const RwMatrix *parent, RwMatrix *out)
{
    dynBlendFrame(b, i, j, parent, out, 0);
}

/* A knee: the thigh frame turned about its x axis to the shin direction (acos of up . dir; past 90
 * degrees mirrored to -0x8000 - angle). The last argument is not read. */
/* VA 0x00455430, 261 bytes, [I] row, status draft */
void dynHingeFrame(DynBody *b, int i, int j, const RwMatrix *thigh, RwMatrix *out, int left)
{
    RwMatrix tmp;
    RwV3d dir;
    float c;
    int angle;

    (void)left;
    zmathMatrixIdentity(out);
    zmathMatrixIdentity(&tmp);
    dynVecSub(&P(b, i)->pos.x, &P(b, j)->pos.x, &dir.x);
    zmathVecNormalize(&dir, &dir);
    c = zmathVecDot(&dir, &thigh->up);
    if (c < -1.0f)
        c = -1.0f;
    else if (c > 1.0f)
        c = 1.0f;
    angle = zmathAcos(c);
    if (c < 0.0f)
        angle = -0x8000 - angle;
    zmathMatrixRotateX(out, out, angle);
    zmathMatrixCopy(&tmp, thigh);
    zmathMatrixMultiply(&tmp, &tmp, out);
    zmathMatrixCopy(out, &tmp);
}

/* As dynLimbFrame with the at term's sign the other way round (upper arms). */
/* VA 0x00455590, 580 bytes, [I] row, status draft */
void dynLimbFrameMirror(DynBody *b, int i, int j, const RwMatrix *parent, RwMatrix *out)
{
    dynBlendFrame(b, i, j, parent, out, 1);
}

/* An elbow: the forearm direction reflected through the upper arm's right axis (only its outward /
 * inward component), then dynFrameFromUp. */
/* VA 0x00455880, 255 bytes, [I] row, status draft */
void dynElbowFrame(DynBody *b, int i, int j, const RwMatrix *upper, RwMatrix *out, int left)
{
    RwMatrix unused;
    RwV3d dir, r;
    float d;

    zmathMatrixIdentity(out);
    zmathMatrixIdentity(&unused);
    dynVecSub(&P(b, i)->pos.x, &P(b, j)->pos.x, &dir.x);
    d = zmathVecDot(&dir, &upper->right);
    r = upper->right;
    if (left == 0) {
        if (d < 0.0f)
            d = -d;
    } else {
        if (d > 0.0f)
            d = -d;
    }
    zmathVecScale(&r, &r, d + d);
    zmathVecAdd(&dir, &dir, &r);
    dynFrameFromUp(b, &dir, upper, out);
}

/* up = dir; at = parent.at x |c| + (up x parent.right... ) x (1 - |c|) with c = parent.right . up;
 * right = at x up, at = up x right. `dir` is overwritten. */
/* VA 0x004559c0, 268 bytes, [I] row, status draft */
void dynFrameFromUp(DynBody *b, RwV3d *dir, const RwMatrix *parent, RwMatrix *out)
{
    RwV3d at;
    float c;

    (void)b;
    zmathMatrixIdentity(out);
    out->up = *dir;
    zmathVecNormalize(&out->up, &out->up);
    c = zmathVecDot(&parent->right, &out->up);
    zmathVecDot(&parent->up, &out->up);     /* (computed and dropped) */
    zmathVecDot(&parent->at, &out->up);
    at = parent->at;
    dynCross(&parent->right, &out->up.x, dir);
    zmathVecNormalize(dir, dir);
    zmathVecScale(&at, &at, fabsf(c));
    zmathVecScale(dir, dir, 1.0f - fabsf(c));
    zmathVecAdd(&at, &at, dir);
    out->at = at;
    dynCross(&out->up, &out->at.x, &out->right);
    zmathVecNormalize(&out->right, &out->right);
    dynCross(&out->right, &out->up.x, &out->at);
    zmathVecNormalize(&out->at, &out->at);
}

/* ------------------------------------------------------------------ small helpers */

/* VA 0x00455b20, 37 bytes, [I] row, status draft */
void dynVecSub(const float *a, const float *b, float *out)
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

/* out = b x a (zmathTriangleNormal(out, a, origin, b)) */
/* VA 0x00455b60, 108 bytes, [I] row, status draft */
void dynCross(const RwV3d *a, const float *b, RwV3d *out)
{
    RwV3d origin, v;

    origin.x = 0.0f;
    origin.y = 0.0f;
    v.x = b[0];
    v.y = b[1];
    v.z = b[2];
    origin.z = 0.0f;
    zmathVecScale(&v, &v, 1.0f);
    zmathTriangleNormal(out, a, &origin, &v);
}

/* VA 0x00455bf0, 18 bytes, [I] row, status draft */
void dynZeroAngles(int16_t *euler)
{
    euler[0] = 0;
    euler[1] = 0;
    euler[2] = 0;
}

/* The 3 x 3 part of src, transposed, into dst (the rest of dst untouched). */
/* VA 0x00455c20, 48 bytes, [I] row, status draft */
void dynTranspose3(const RwMatrix *src, RwMatrix *dst)
{
    const float *s = &src->right.x;
    float *d = &dst->right.x;
    int r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            d[r * 4 + c] = s[c * 4 + r];
}

/* 3 x 3: out = b . a (row vectors: apply b, then a). */
/* VA 0x00455c60, 110 bytes, [I] row, status draft */
void dynMul3(RwMatrix *out, const RwMatrix *a, const RwMatrix *b)
{
    const float *pa = &a->right.x, *pb = &b->right.x;
    float *o = &out->right.x;
    int r, c, k;

    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            o[r * 4 + c] = 0.0f;
            for (k = 0; k < 3; k++)
                o[r * 4 + c] = pa[k * 4 + c] * pb[r * 4 + k] + o[r * 4 + c];
        }
    }
}

/* Euler angles (x, y, z) of a rotation matrix. */
/* VA 0x00455cf0, 175 bytes, [I] row, status draft */
void dynMatrixToEuler(const RwMatrix *m, int16_t *euler)
{
    const float *f = &m->right.x;
    float cy = (float)sqrt(f[1] * f[1] + f[0] * f[0]);

    if (cy > 0.01f) {
        euler[0] = zmathAtan2(f[6], f[10]);
        euler[1] = zmathAtan2(-f[2], cy);
        euler[2] = zmathAtan2(f[1], f[0]);
        return;
    }
    euler[0] = zmathAtan2(-f[9], f[5]);
    euler[1] = zmathAtan2(-f[2], cy);
    euler[2] = 0;
}

/* The pelvis frame becomes the actor's orientation (zactorSetOrientation with up and at); its angles
 * go to the Hips channel and the actor's own rotation is cleared. */
/* VA 0x00455dd0, 107 bytes, [I] row, status draft */
void dynSetActorFrame(ZActor *actor, const RwMatrix *m, int16_t *hips)
{
    int16_t zero[4];
    RwMatrix out;

    zero[0] = zero[1] = zero[2] = zero[3] = 0;
    zactorSetOrientation(actor, zero, &m->up, &m->at, &out);
    hips[0] = actor->rot[0];
    hips[1] = actor->rot[1];
    hips[2] = actor->rot[2];
    actor->rot[0] = actor->rot[1] = actor->rot[2] = 0;
}

/* ------------------------------------------------------------------ the limb IK */

/* VA 0x00455e60, 25 bytes, [I] row, status draft */
void dynIkSetGains(BmxIk *ik, float gain, float gain2)
{
    ik->gain = gain;
    ik->gain2 = gain2;
}

/* Unreferenced. */
/* VA 0x00455e90, 24 bytes, [I] row (DM-058: created), status draft */
void dynIkSetLooseGains(BmxIk *ik)
{
    dynIkSetGains(ik, 0.5f, 0.01f);
}

/* physics.c bmxphysStateInitRoll */
/* VA 0x00455ec0, 24 bytes, [I] row, status draft */
void dynIkSetDefaultGains(BmxIk *ik)
{
    dynIkSetGains(ik, 0.1f, 0.01f);
}

/* (DM-047: cameraInitRider) */
/* VA 0x00455ef0, 1 bytes, [I] row, status draft */
void dynIkReset(void *actor, BmxIk *ik)
{
    (void)actor;
    (void)ik;
}

/* The limb lengths from the rider model (riderCreate, with that player's part cache selected).
 * (DM-047: cameraMeasureRider) */
/* VA 0x00455f10, 216 bytes, [I] row, status draft */
void dynIkMeasure(void *actor, BmxIk *ik)
{
    ZActor *a = (ZActor *)actor;

    ik->foreArmL = zactorGetPartLength(a, 5);       /* LeftHand */
    ik->upperArmL = zactorGetPartLength(a, 4);      /* LeftLowArm */
    ik->foreArmR = zactorGetPartLength(a, 8);       /* RightHand */
    ik->upperArmR = zactorGetPartLength(a, 7);      /* RightLowArm */
    ik->shinL = zactorGetPartLength(a, 11);         /* LeftFoot */
    ik->thighL = zactorGetPartLength(a, 10);        /* LeftLowLeg */
    ik->shinR = zactorGetPartLength(a, 14);         /* RightFoot */
    ik->thighR = zactorGetPartLength(a, 13);        /* RightLowLeg */
    ik->unk_110 = 0.0f;
    ik->reachLegL = ik->thighL + ik->shinL;
    ik->reachLegR = ik->thighR + ik->shinR;
    ik->reachArmL = (ik->upperArmL + ik->foreArmL) - 1.0f;
    ik->reachArmR = (ik->upperArmR + ik->foreArmR) - 1.0f;
}

/* Unreferenced: out = a x w[0] + b x w[1] + c x w[2]. */
/* VA 0x00456030, 192 bytes, [I] row, status draft */
void dynIkBlendAxes(const float *w, const float *a, const float *b, const float *c, RwV3d *out)
{
    RwV3d va, vb, vc;

    va.x = a[0]; va.y = a[1]; va.z = a[2];
    vb.x = b[0]; vb.y = b[1]; vb.z = b[2];
    vc.x = c[0]; vc.y = c[1]; vc.z = c[2];
    zmathVecScale(&va, &va, w[0]);
    zmathVecScale(&vb, &vb, w[1]);
    zmathVecScale(&vc, &vc, w[2]);
    *out = va;
    zmathVecAdd(out, out, &vb);
    zmathVecAdd(out, out, &vc);
}

/* the actor's part position: the first query poses the model, the later ones reuse that pose */
static void ikPartPos(ZActor *a, int part, int *posed, RwV3d *out)
{
    static const float zero[3] = { 0.0f, 0.0f, 0.0f };

    if (*posed)
        zactorGetAbsolutePartPositionNoPose(a, (uint16_t)part, zero, &out->x);
    else
        zactorGetAbsolutePartPosition(a, (uint16_t)part, zero, &out->x);
    *posed = 1;
}

/* One two-bone limb (the exe writes it out four times): root = the limb's first part; the target
 * clamped to `reach`; the middle joint by the law of cosines (upper `a`, lower `b`) in the plane of
 * the target and the bend direction `dir` (legs bend the other way: sinSign -1); the two segment
 * frames (right = dir), made relative to `parent` and to the upper segment, into two channels. */
static void ikSolveLimb(ZActor *actor, const RwMatrix *parent, int rootPart, int *posed,
                        const float *target, const float *bendDir, float reach, float a, float b,
                        float sinSign, int16_t *upperRot, int16_t *lowerRot)
{
    RwV3d root, v, side, dir, end, mid;
    RwMatrix upper, lower, inv, rel;
    float dist, cosA, sinA;

    ikPartPos(actor, rootPart, posed, &root);
    dynVecSub(target, &root.x, &v.x);
    dist = (float)sqrt(zmathVecDot(&v, &v));
    zmathVecScale(&v, &v, 1.0f / dist);
    if (dist > reach)
        dist = reach;
    dir.x = bendDir[0];
    dir.y = bendDir[1];
    dir.z = bendDir[2];
    dynCross(&v, &dir.x, &side);
    zmathVecNormalize(&side, &side);
    dynCross(&side, &v.x, &dir);
    zmathVecNormalize(&dir, &dir);
    cosA = ((dist * dist + a * a) - b * b) / (a * dist + a * dist);
    sinA = zmathSin(zmathAcos(cosA));
    end = v;
    zmathVecScale(&end, &end, dist);
    zmathVecScale(&v, &v, cosA * a);
    zmathVecScale(&side, &side, sinSign * sinA * a);
    mid = v;
    zmathVecAdd(&mid, &mid, &side);
    zmathVecScale(&mid, &mid, -1.0f);               /* minus the upper segment */
    zmathVecAdd(&end, &end, &mid);                  /* the lower segment */

    upper.right = dir;
    upper.up = mid;
    zmathVecNormalize(&upper.up, &upper.up);
    dynCross(&upper.right, &upper.up.x, &upper.at);
    zmathVecNormalize(&upper.at, &upper.at);
    zmathVecScale(&upper.up, &upper.up, -1.0f);
    lower.right = dir;
    lower.up = end;
    zmathVecNormalize(&lower.up, &lower.up);
    dynCross(&lower.right, &lower.up.x, &lower.at);
    zmathVecNormalize(&lower.at, &lower.at);
    zmathVecScale(&lower.at, &lower.at, -1.0f);

    dynTranspose3(parent, &inv);
    dynMul3(&rel, &inv, &upper);
    dynMatrixToEuler(&rel, upperRot);
    dynTranspose3(&upper, &inv);
    dynMul3(&rel, &inv, &lower);
    dynMatrixToEuler(&rel, lowerRot);
}

/* Solve the limbs that got a target this frame (ik->limbs) into the output pose (block 0): arms
 * relative to the chest (root matrix x the Chest channel), legs relative to the root. A limb without
 * a target is left as the motion posed it (the exe still reads its current hand position). */
/* VA 0x00456120, 4581 bytes, [I] row, status draft */
void dynIkSolve(ZActor *actor, BmxIk *ik)
{
    int16_t (*rot)[3] = actor->anim->block[0].rot;
    RwMatrix root, chestRot, chest;
    float handL[3], dirL[3], handR[3], dirR[3];
    static const float zero[3] = { 0.0f, 0.0f, 0.0f };
    static const float unitX[3] = { 1.0f, 0.0f, 0.0f };
    RwV3d p0;
    int posed = 0;

    if (!(ik->limbs & 1)) {
        zactorGetAbsolutePartPosition(actor, 5, zero, handL);
        posed = 1;
        zactorGetAbsolutePartPosition(actor, 3, unitX, dirL);
        zactorGetAbsolutePartPosition(actor, 3, zero, &p0.x);
        dynVecSub(dirL, &p0.x, dirL);
    } else {
        handL[0] = ik->target[0].pos[0]; handL[1] = ik->target[0].pos[1]; handL[2] = ik->target[0].pos[2];
        dirL[0] = ik->target[0].dir[0];  dirL[1] = ik->target[0].dir[1];  dirL[2] = ik->target[0].dir[2];
    }
    if (!(ik->limbs & 2)) {
        if (posed)
            zactorGetAbsolutePartPositionNoPose(actor, 8, zero, handR);
        else
            zactorGetAbsolutePartPosition(actor, 8, zero, handR);
        posed = 1;
        zactorGetAbsolutePartPosition(actor, 6, unitX, dirR);
        zactorGetAbsolutePartPosition(actor, 6, zero, &p0.x);
        dynVecSub(dirR, &p0.x, dirR);
    } else {
        handR[0] = ik->target[1].pos[0]; handR[1] = ik->target[1].pos[1]; handR[2] = ik->target[1].pos[2];
        dirR[0] = ik->target[1].dir[0];  dirR[1] = ik->target[1].dir[1];  dirR[2] = ik->target[1].dir[2];
    }
    zactorGetRootMatrix(actor, &root);
    zmathMatrixIdentity(&chestRot);
    zmathMatrixRotateXYZ(&chestRot, &chestRot, rot[1]);
    dynMul3(&chest, &root, &chestRot);              /* = chest channel, then the root */

    if (ik->limbs & 2)
        ikSolveLimb(actor, &chest, 6, &posed, handR, dirR, ik->reachArmR, ik->upperArmR, ik->foreArmR,
                    1.0f, rot[6], rot[7]);          /* RightUpArm, RightLowArm */
    if (ik->limbs & 1)
        ikSolveLimb(actor, &chest, 3, &posed, handL, dirL, ik->reachArmL, ik->upperArmL, ik->foreArmL,
                    1.0f, rot[3], rot[4]);          /* LeftUpArm, LeftLowArm */
    if (ik->limbs & 4)
        ikSolveLimb(actor, &root, 9, &posed, ik->target[2].pos, ik->target[2].dir, ik->reachLegL,
                    ik->thighL, ik->shinL, -1.0f, rot[9], rot[10]);    /* LeftUpLeg, LeftLowLeg */
    if (ik->limbs & 8)
        ikSolveLimb(actor, &root, 12, &posed, ik->target[3].pos, ik->target[3].dir, ik->reachLegR,
                    ik->thighR, ik->shinR, -1.0f, rot[12], rot[13]);   /* RightUpLeg, RightLowLeg */
    ik->limbs = 0;
}

/* Give the limbs in `limbs` (bits 1, 2, 4, 8) the target and bend direction for this frame. */
/* VA 0x00457790, 263 bytes, [I] row, status draft */
void dynIkSetTarget(BmxIk *ik, const RwV3d *target, const RwV3d *dir, uint32_t limbs)
{
    int i;

    ik->limbs |= limbs;
    for (i = 0; i < 4; i++) {
        if (limbs & (1u << i)) {
            ik->target[i].pos[0] = target->x;
            ik->target[i].pos[1] = target->y;
            ik->target[i].pos[2] = target->z;
            ik->target[i].dir[0] = dir->x;
            ik->target[i].dir[1] = dir->y;
            ik->target[i].dir[2] = dir->z;
        }
    }
}

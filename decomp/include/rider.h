/*
 * rider.h - per-player rider state (Bmx/Src/com/action/rider.c). The decomp of game.c
 * recovered the fields it touches: trick input, the trick/score block, the trick log, per-area statistics,
 * judging, position/velocity and the two contact records; rider.c added the fields it touches:
 * the input block and direction buffer, the state machine (state/stateTimer), the two motion slots
 * and the root-motion accumulator, the trick-flip rotation, the steering rates and the wheel contacts;
 * physics.c added the physics fields (prefixed `phys`) and BmxWheel, the two wheel records.
 *
 * [V] Global array g_riders at 0x00691F40, stride 0xF70, indexed by player slot (rider_Create
 * 0x004316C0 writes slot * 0xF70 + offset). [I] two slots (2 players, as g_saveProfiles).
 * Offsets are [V] (read/written at those offsets by game.c / rider.c); meanings [I] unless noted.
 * The rest is padding until someone recovers it.
 */
#ifndef RIDER_H
#define RIDER_H
#include "dmtypes.h"

/* One trick of the current trick set (16 bytes; 7 of them). gameUpdateTricks 0x0040D010 fills it. */
typedef struct BmxTrickRec {
    uint8_t  flags;            /* +0x00 bit0/1: x1.2 bonus each (bit1 = landed backwards, 0x0040D010), bit2: x1.3 */
    uint8_t  trick;            /* +0x01 trick id 0..45 (g_trickNames), 0xFF none */
    uint8_t  mod;              /* +0x02 modifier id 0..37 (g_trickNames[46 + mod]), 0xFF none */
    uint8_t  modCount;         /* +0x03 modifiers stacked (rider +0x4C) */
    uint8_t  spins;            /* +0x04 half turns (180s) of spin, max 8 */
    uint8_t  flips;            /* +0x05 0..3 */
    uint16_t ticks;            /* +0x06 frames the trick was held (points += ticks * rate) */
    uint16_t age;              /* +0x08 frames since the trick started (gameUpdateTricks) */
    uint16_t points;           /* +0x0A score of this trick (gameScoreTrickSet) */
    float    length;           /* +0x0C grind / manual length (cm) */
} BmxTrickRec;                 /* 0x10 */

/* Score block at rider +0x5C (gameScoreTrickSet 0x0040FE40 gets a pointer to it). */
typedef struct BmxScoreBlock {
    uint32_t    runScore;      /* +0x00 score of the current run (record stat 1) */
    uint32_t    totalScore;    /* +0x04 sum over the runs (record stat 0) */
    uint32_t    setScore;      /* +0x08 score of the trick set in progress */
    uint32_t    wins;          /* +0x0C rounds won / B-M-X letters / attempts used, per game mode */
    BmxTrickRec trick[7];      /* +0x10 the set; the code indexes it 1-based from the block start */
} BmxScoreBlock;               /* 0x80 */

/* Trick statistics of one 1/16 of the level (4 x 4 grid) or of the whole run (rider +0x2C6). */
typedef struct BmxAreaStats {
    uint16_t tricks;           /* +0x00 air tricks landed */
    uint16_t modifiers;        /* +0x02 */
    uint16_t modified;         /* +0x04 tricks with a modifier */
    uint16_t grinds;           /* +0x06 */
    uint16_t stalls;           /* +0x08 (incl. fast plant / wall tap) */
    uint16_t manuals;          /* +0x0A */
    uint16_t falls;            /* +0x0C */
} BmxAreaStats;                /* 0x0E */

/* One competition run as the judges saw it (rider +0x3C4 + run * 0xC). */
typedef struct BmxJudgeRun {
    int32_t  score;            /* +0x00 run score */
    uint8_t  judge[5];         /* +0x04 0..255 */
    uint8_t  average;          /* +0x09 */
    uint8_t  pad_0a[2];
} BmxJudgeRun;                 /* 0x0C */

/* Contact record: what the bike was on at take-off ([0]) and at landing ([1]). */
typedef struct BmxContact {
    uint8_t  type;             /* +0x000 surface class: 9 prop/object, 10 tagged edge, 12 prop group, ... */
    uint8_t  pad_001[3];
    void    *obj;              /* +0x004 the object (tag id at +0x16, group at +0x02 & 0x1F) */
    uint8_t  unk_008[0x1F4];
    float    pos[3];           /* +0x1FC position at that moment */
    uint8_t  unk_208[4];
} BmxContact;                  /* 0x20C */

/* Controller state copied into the rider every frame (riderReadInput 0x00430E70 from the contrler.c
 * pad 0x0056B628 + 0x10; the sticks are quantised to 1/127 so replays reproduce). contrlerGetStick
 * returns stick[n] with the d-pad folded in; contrlerTestAction tests an action (ACT_*, contrler.h). */
typedef struct BmxInput {
    uint32_t buttons;          /* +0x00 held; bits 0-3 d-pad up/down/left/right */
    uint32_t changed;          /* +0x04 buttons that changed this frame (direction edges, riderUpdateAll) */
    float    stick[2][2];      /* +0x08 two sticks (x, y), -1..1; y < 0 is up */
    float    prevStick[2][2];  /* +0x18 previous frame */
} BmxInput;                    /* 0x28 */

/* One motion player of the rider actor (rider +0xB9C: [0] the motion, [1] the blend target). */
typedef struct BmxMotionSlot {
    uint16_t id;               /* +0x00 motion number (motion.zal id, g_motionTable index) */
    uint16_t pad_02;
    void    *frame;            /* +0x04 decoder output of the last step (zactor_004740c0): root delta x, y, z */
    uint32_t flags;            /* +0x08 MOTION_* (2 restart, 8 mirror, 0x80 busy, 0x100 frozen, 0x200 blend) */
    float    rate;             /* +0x0C [0]: playback speed (1.0); [1]: blend weight */
} BmxMotionSlot;               /* 0x10 */

/* A signature combo (g_signatureTricks 0x00553840: 12 riders x 4): the last `length` directions of
 * the buffer select `trick` (+ `mod`, 0xFF none). */
typedef struct BmxSigCombo {
    uint8_t  length;           /* +0x00 0 = unused slot */
    uint8_t  dir[3];           /* +0x01 RIDER_DIR_* */
    uint8_t  trick;            /* +0x04 */
    uint8_t  mod;              /* +0x05 */
} BmxSigCombo;                 /* 0x06 */

/* One IK goal (dynIkSetTarget): where the end of the limb goes and the direction its frames' right
 * axis takes (0x20). */
typedef struct BmxIkTarget {
    float    pos[4];           /* +0x00 world position of the hand / foot */
    float    dir[4];           /* +0x10 bend direction (the forearm / shin right axis) */
} BmxIkTarget;                 /* 0x20 */

/* The rider's two-bone limb IK (BmxRider +0xD98, 0x1D8; dyn.c dynIk*). Limb bits / target
 * index: 1 left arm, 2 right arm, 4 left leg, 8 right leg. Lengths are the model's part offsets
 * (zactorGetPartLength, dynIkMeasure); reach = the two lengths (arms: minus 1). [V] offsets. */
typedef struct BmxIk {
    uint8_t  unk_000[0x100];   /* +0x000 not used */
    float    unk_100;          /* +0x100 1.0 (riderCreate; formerly camScale), not read */
    float    unk_104;          /* +0x104 0 (riderCreate), not read */
    float    unk_108;          /* +0x108 0 (riderCreate), not read */
    uint32_t unk_10c;
    float    unk_110;          /* +0x110 0 (dynIkMeasure), not read */
    uint32_t unk_114[2];
    float    upperArmL;        /* +0x11C |LeftLowArm offset| */
    float    foreArmL;         /* +0x120 |LeftHand offset| */
    float    shinL;            /* +0x124 |LeftFoot offset| */
    float    thighL;           /* +0x128 |LeftLowLeg offset| */
    float    upperArmR;        /* +0x12C |RightLowArm offset| */
    float    foreArmR;         /* +0x130 |RightHand offset| */
    float    shinR;            /* +0x134 |RightFoot offset| */
    float    thighR;           /* +0x138 |RightLowLeg offset| */
    float    reachArmL;        /* +0x13C upperArmL + foreArmL - 1 */
    float    reachArmR;        /* +0x140 */
    float    reachLegL;        /* +0x144 thighL + shinL */
    float    reachLegR;        /* +0x148 */
    float    gain;             /* +0x14C 0.1 (dynIkSetDefaultGains) / 0.5; not read */
    float    gain2;            /* +0x150 0.01; not read */
    uint32_t limbs;            /* +0x154 limbs with a target this frame (dynIkSolve clears it) */
    BmxIkTarget target[4];     /* +0x158 left arm, right arm, left leg, right leg */
} BmxIk;                       /* 0x1D8 */

/* One wheel of the bike (rider +0xC88 front, +0xD0C rear; physics.c). [V] offsets from
 * bmxphysUpdatePositionRoll/Air (wheel base + i * 0x84) and bmxphysResetRider; meanings [I]. */
typedef struct BmxWheel {
    float    pos[3];           /* +0x00 hub in bike space (front {55.25, -5.6, 0}, rear {-36.52, -5.6, 0}, riderCreate) */
    uint8_t  unk_0c[4];
    float    radius;           /* +0x10 24.65 */
    float    vel[3];           /* +0x14 velocity of the hub (world, per frame) */
    uint8_t  unk_20[4];
    float    speed;            /* +0x24 |vel| (rolling speed; rider.c: < 1.0 = barely moving) */
    float    frame[16];        /* +0x28 RwMatrix: right = rolling direction, up = ground normal, at = axle */
    uint8_t  contact;          /* +0x68 0 rolling, 1 skidding (rear wheel locked), 2 in the air */
    uint8_t  hit;              /* +0x69 bit 0: ran into a wall this frame */
    int16_t  angle;            /* +0x6A rotation (binary angle), += spin every frame (bmxphysUpdate) */
    int32_t  spin;             /* +0x6C rotation speed (binary angle per frame, low 16 bits used) */
    float    travel;           /* +0x70 suspension position, clamped to rider suspension[0..1] */
    float    travelVel;        /* +0x74 suspension velocity */
    uint8_t  unk_78[8];
    uint32_t material;         /* +0x80 surface type under the wheel (material flags & 0xF, or the prop's class) */
} BmxWheel;                    /* 0x84 */

typedef struct BmxRider {
    int16_t  rider;            /* +0x000 rider index 0..11 (returned pointer points here) */
    uint8_t  style;            /* +0x002 costume 0..4 */
    uint8_t  unk_003;
    uint8_t  profileByte2;     /* +0x004 copy of g_saveProfiles[slot].unk_02 = controller configuration 0..2 = A/B/C (contrlerTestAction) */
    uint8_t  dirCount;         /* +0x005 directions in dirBuf (0..4) */
    uint8_t  dirBuf[4];        /* +0x006 RIDER_DIR_* entered since the buffer was reset, oldest first */
    uint8_t  unk_00a[2];
    BmxInput input;            /* +0x00C */
    uint8_t  unk_034[4];
    uint32_t stateFlags;       /* +0x038 RIDER_FLAG_* (rider.c); 0x80 also read by game.c */
    uint8_t  slot;             /* +0x03C player slot */
    uint8_t  state[4];         /* +0x03D state machine: [0] RIDER_STATE_*, [1] sub-state, [2] phase, [3] spare */
    uint8_t  unk_041;
    uint16_t stateTimer[4];    /* +0x042 frames in state[level]; [3] = trick/modifier button buffer timer */
    uint8_t  trickId;          /* +0x04A trick being performed, 0xFF none */
    uint8_t  modId;            /* +0x04B modifier being performed, 0xFF none */
    uint8_t  modCount;         /* +0x04C */
    uint8_t  impactTimer;      /* +0x04D frames until the next "ouch" sound (riderOnRiderImpact) */
    uint8_t  hud4e;            /* +0x04E shown as HUD item 8 every frame */
    uint8_t  crashKind;        /* +0x04F HUD item 7 while crashed (7/8: hint 18) */
    int32_t  spin;             /* +0x050 binary angle, 0x10000 = 360 deg */
    int32_t  flip;             /* +0x054 binary angle */
    uint32_t crash;            /* +0x058 wipeout damage (x100 = score) */
    BmxScoreBlock score;       /* +0x05C */
    uint8_t  unk_0dc[0x10];
    uint8_t  trickState;       /* +0x0EC 0 riding, 1 airborne, 2 trick, 3 landing */
    uint8_t  trickCount;       /* +0x0ED records used in score.trick */
    uint8_t  landTimer;        /* +0x0EE frames until a landing counts */
    uint8_t  trickLog[47][10]; /* +0x0EF [trick + 1][(mod + 1) / 4]: 2-bit "times landed" per trick x modifier */
    uint8_t  pad_2c5;
    BmxAreaStats area[16];     /* +0x2C6 4 x 4 level grid (g_gameAreaSize) */
    BmxAreaStats total;        /* +0x3A6 */
    float    bestWallHeight;   /* +0x3B4 highest fast plant / wall tap of the run */
    float    skidLength;       /* +0x3B8 current skid (record stat 9) */
    uint32_t hintMask;         /* +0x3BC tutorial hints still to show (bit = hint 0..21) */
    uint32_t runRewards;       /* +0x3C0 rewards earned at this level end (bit 2t+k level, 6+t park, 0x200/0x400/0x800 final) */
    BmxJudgeRun judge[2];      /* +0x3C4 competition runs */
    uint8_t  physState;        /* +0x3DC physics state BMXPHYS_ROLL..STALL (physics.h), 0xFF before the first one */
    uint8_t  surface;          /* +0x3DD BMXSURF_* (physics.h): type of what the bike is on (9, 10, 12, ...) */
    int16_t  physTimer;        /* +0x3DE frames left of a timed physics move: the lock-on flight to a rail / wall,
                                  the air auto-rotation (physAirRot) or the spine hop; physics counts down
                                  (formerly snapFrames) */
    float    matrix[16];       /* +0x3E0 orientation */
    float    pos[3];           /* +0x420 position (cm; y up is negative) */
    uint8_t  unk_42c[4];
    float    physHipOffset[3]; /* +0x430 rider actor root relative to pos (world axes) = matrix * rootMotion */
    uint8_t  unk_43c[4];
    float    wallHeight;       /* +0x440 */
    float    grindBalance;     /* +0x444 shown by the HUD while grinding */
    float    vel[3];           /* +0x448 velocity */
    uint8_t  unk_454[4];
    float    physSlopeAccel[3];/* +0x458 acceleration this frame (gravity along an edge / gravity / roll delta-v) */
    uint8_t  unk_464[4];
    float    physLockStep[3];  /* +0x468 per-frame move of the lock-on flight to a grind/stall target */
    uint8_t  unk_474[4];
    int16_t  rotVel[3];        /* +0x478 angular rates (binary angle / frame) about the bike's right, up, at axes:
                                  [0] grind balance, [1] steering / air spin, [2] manual balance */
    uint8_t  unk_47e[2];
    int16_t  airLean;          /* +0x480 pitch lean in the air (down-down / up-up held), +-0x18E3 */
    uint8_t  unk_482[2];
    float    frameDist;        /* +0x484 distance moved this frame */
    void    *surfaceObj;       /* +0x488 object under the bike */
    uint8_t  physSurfaceCopy[0x1F4]; /* +0x48C private copy of the surface record (surfaceObj points here) */
    float    physLockPos[3];   /* +0x680 grind/stall target point */
    uint8_t  unk_68c[4];
    float    physLockMatrix[16]; /* +0x690 RwMatrix: orientation on the grind/stall target */
    float    stats[7];         /* +0x6D0 rider x bike multipliers (tables 0x00553664, 0x005537B4), clamped to 1.0 (stat 6: 2.0) */
    float    physAirMatrix[16];/* +0x6EC RwMatrix: orientation at take-off (frame of the auto-rotation) */
    int16_t  physAirRot[3];    /* +0x72C auto-rotation per frame while physTimer runs (binary angles x, y, z);
                                  [1] was called carriedSpin (spin carried over from the rail / lip) */
    uint8_t  unk_732[2];
    float    trickMatrix[16];  /* +0x734 orientation when the air trick started */
    int16_t  flipVel[3];       /* +0x774 flip rotation rates (x, y, z); flipVel[2] drives flip */
    uint8_t  unk_77a[2];
    BmxContact contact[2];     /* +0x77C [0] take-off, [1] landing */
    void    *riderActor;       /* +0xB94 zroller actor (zrollerCreateRider) */
    void    *animCtl;          /* +0xB98 rider dynamics (rag doll) body, dynCreateRider */
    BmxMotionSlot motion[2];   /* +0xB9C [0] the motion requested by the state machine, [1] blend target */
    float    modBlend;         /* +0xBBC modifier weight 0..1 */
    uint32_t cleared_bc0[15];  /* +0xBC0 zeroed on create; second decoder output (zactor_00477210) */
    float    rootMotion[3];    /* +0xBFC clip root motion in the actor frame: x, z summed per-frame deltas,
                                  y = -height (ZGE units = 100 x model units, Y down) */
    uint8_t  unk_c08[4];
    int32_t  bikeModel;        /* +0xC0C rider * 4 + bike, or 0x30 = secret bike */
    float    spawnPos[3];      /* +0xC10 random-spot start (gameResetRider); physics.c: the bike's position, which
                                  physics integrates every frame (pos = rider hips) */
    uint8_t  unk_c1c[4];
    float    bikeMatrix[16];   /* +0xC20 bike orientation (right/up/at/pos) */
    float    pedalForce;       /* +0xC60 drive added to the wheel speed this frame (cleared every frame) */
    float    wheelDrag;        /* +0xC64 wheel speed damping this frame (1.0 idle, 0.1 skid, 0.01 manual/stall) */
    float    tuneC68;          /* +0xC68 50.0; physics.c: height of the centre of mass above the wheel contact */
    float    tuneC6C;          /* +0xC6C 5.0; physics.c: weight-transfer divisor (pitch torque) */
    float    tuneC70;          /* +0xC70 0.2; physics.c: suspension spring divisor (mass) */
    float    suspension[5];    /* +0xC74 0 or {-15, 25, 0.3, 0.1, 0.1} (cheat bit 4); physics.c: [0] min travel,
                                  [1] max travel, [2] spring stiffness, [3] damping when the spring
                                  extends (velocity < 0), [4] damping when it compresses */
    BmxWheel wheel[2];         /* +0xC88 [0] front, [1] rear (were wheelFront/R/Load/Contact, wheelRear/...) */
    void    *bikeActor;        /* +0xD90 zroller actor (zrollerCreateBike) */
    void    *bikeCtl;          /* +0xD94 */
    BmxIk    ik;               /* +0xD98 limb IK block (dyn.c dynIk*; formerly called the camera block) */
} BmxRider;                    /* 0xF70 */

/* GHIDRA-SKIP-BEGIN */
typedef char rider_h_size_check[(sizeof(BmxTrickRec) == 0x10 && sizeof(BmxScoreBlock) == 0x80 &&
                                 sizeof(BmxAreaStats) == 0x0E && sizeof(BmxJudgeRun) == 0x0C &&
                                 sizeof(BmxContact) == 0x20C && sizeof(BmxRider) == 0xF70 &&
                                 sizeof(BmxInput) == 0x28 && sizeof(BmxMotionSlot) == 0x10 &&
                                 sizeof(BmxSigCombo) == 0x06 && sizeof(BmxWheel) == 0x84 &&
                                 sizeof(BmxIkTarget) == 0x20 && sizeof(BmxIk) == 0x1D8) ? 1 : -1];
extern BmxRider g_riders[2];   /* 0x00691F40 */
/* GHIDRA-SKIP-END */

#endif /* RIDER_H */

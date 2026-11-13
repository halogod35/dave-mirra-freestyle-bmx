/*
 * zaxis/Bmx/Src/com/action/rider.c
 * Original: C:\zaxis\Bmx\Src\com\action\rider.c
 * Evidence: [V] path string 0x005589D0 referenced by the asserts of riderUpdateAll, riderGetState and
 *           riderOnRiderImpact; the others [I] (link order between replay.c and guiinput.c, the
 *           state-procedure tables 0x005588B8-0x00558903, the trick-selection tables 0x00558904-0x00558FBF).
 * 34 functions, VA 0x00430e70-0x004364ff. DM-047 (draft): DM-040 listed 27; DM-047 added
 * riderReadInput (from the replay/rider gap), the empty riderNullProc, the air and coast state
 * procedures and the empty riding sub-state 5 (reached only through the tables), and the two
 * dynamics impact callbacks (registered by riderCreate). Non-matching C (AD-8). Types: rider.h.
 * Answers written into docs/formats/ANIM.md section 8; Python ports in tools/dmbmx/formats/tricks.py
 * (tests/test_decomp_dm047.py).
 *
 * The rider: one BmxRider per player (g_riders, 2 slots). Every frame action.c calls
 * riderReadInput (controller -> rider input block), riderUpdateAll (state machine + motion
 * playback) and then physics. The state machine has three levels (BmxRider.state[0..2]):
 *
 *   state[0]  RIDER_STATE_*: riding, air, grind, stall, manual, landing, crash; one procedure each
 *             (g_riderMainProc). It reads the physics contact state (bmxphysGetState) to change state.
 *   state[1]  sub-state, procedure from a per-state table: riding = coast, pedal, skid, crouch (jump),
 *             turnaround; air = idle, trick, modifier-only trick; grind / stall / manual = one each.
 *   state[2]  phase inside the sub-state (0 enter, 1 start clip, 2 hold, 3 release, 4 done ...).
 *
 * A procedure returns 1 when the frame is done and 0 after a state change, and is then called again
 * (with the new state) in the same frame. riderSetState resets the deeper levels.
 *
 * Motions: a procedure requests a clip by writing its motion number (motion.zal id) to
 * motion[0].id and MOTION_RESTART to motion[0].flags. riderUpdateAll then steps the rider actor's
 * decoder (zactorPlayMotion, or the two-clip blend zactorPlayMotionBlend when MOTION_BLEND is set), copies
 * the bike channels to the bike actor (zactorCopyBikePose), accumulates the clip's root motion in
 * rootMotion and poses the model (NgIFzactor_ApplyMotionPose). RIDER_FLAG_MOTION_DONE tells the
 * procedures that the clip reached its end. The bike section's wheel channels are overwritten by
 * physics (wheel spin, see ANIM.md section 8).
 *
 * Trick input: while airborne (and on the ground before a grind) the stick directions are pushed into
 * a 4-entry buffer (dirBuf, RIDER_DIR_*). The trick button picks an air trick from the buffer:
 * the rider's signature combos first (g_signatureTricks), then the last two directions
 * (g_airTrickDir2), then the last one (g_airTrickDir1). The modifier button picks a modifier the same
 * way (g_modDir2 / g_modDir1) and g_trickModTable filters it per trick. Grind / stall / plant tricks
 * come from the last direction (g_grindTrickDir, g_stallTrickDir, g_plantTrickDir).
 */
#include <math.h>
#include "decomp.h"
#include "zmath.h"     /* DM-045 names; float arrays are passed where it declares RwV3d / RwMatrix */
#include "contrler.h"  /* DM-051: ACT_* actions, ACT_PRESSED/HELD/RELEASED, BmxPad, contrler* */
#include "camera.h"    /* DM-051: cameraSetPlayer / cameraGetPlayer */

#define RIDER_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\rider.c"

#define DEG_TO_ANGLE 182.04445f            /* 65536 / 360 (0x0054D01C) */
#define ANGLE_TO_DEG 0.0054931640625f      /* 360 / 65536 (0x0054D15C) */
#define ANGLE_DEG(d) ((int16_t)((d) * DEG_TO_ANGLE))

/* state[0] */
enum {
    RIDER_STATE_RIDING  = 0,
    RIDER_STATE_AIR     = 1,
    RIDER_STATE_GRIND   = 2,
    RIDER_STATE_STALL   = 3,   /* stalls, and the fast plant / wall tap */
    RIDER_STATE_MANUAL  = 4,   /* manual and nose wheelie */
    RIDER_STATE_LANDING = 5,
    RIDER_STATE_CRASH   = 6
};
/* state[1] while riding */
enum { RIDE_COAST = 0, RIDE_PEDAL = 1, RIDE_SKID = 2, RIDE_CROUCH = 3, RIDE_TURNAROUND = 4, RIDE_NULL = 5 };
/* state[1] in the air */
enum { AIR_IDLE = 0, AIR_TRICK = 1, AIR_MODIFIER = 2 };
/* state[1] while crashed */
enum { CRASH_START = 0, CRASH_RAGDOLL = 1, CRASH_STEER = 2, CRASH_SETTLE = 3, CRASH_SPLASH = 4, CRASH_WAIT = 5 };

/* physics contact state (bmxphysGetState / bmxphysSetState) [I] */
enum {
    CONTACT_GROUND = 0, CONTACT_AIR = 1, CONTACT_CRASH = 2, CONTACT_GRIND_ENTER = 3, CONTACT_GRIND = 4,
    CONTACT_STALL_ENTER = 5, CONTACT_STALL = 6
};

/* BmxRider.stateFlags */
#define RIDER_FLAG_ACTIVE        0x00000001u  /* updated and drawn */
#define RIDER_FLAG_IDLE          0x00000004u  /* standing still for 30 s */
#define RIDER_FLAG_PEDALLING     0x00000008u  /* auto-pedal (single player) */
#define RIDER_FLAG_RECORD_DIRS   0x00000010u  /* directions go into dirBuf */
#define RIDER_FLAG_TRICK_ARMED   0x00000020u  /* trick button tapped before a direction */
#define RIDER_FLAG_MOD_ARMED     0x00000040u  /* modifier button tapped before a direction */
#define RIDER_FLAG_CHAIN         0x00000080u  /* one frame: a trick was cut short by the next one (game.c) */
#define RIDER_FLAG_HOLD          0x00000100u  /* trick / stall button still held: play the hold clip */
#define RIDER_FLAG_MOTION_DONE   0x00000200u  /* motion[0] reached its end this frame */
#define RIDER_FLAG_MODIFIER      0x00000400u  /* a modifier is blended in (modBlend) */
#define RIDER_FLAG_FLIP          0x00000800u  /* the air trick flips (g_trickFlags & 7) */
#define RIDER_FLAG_CARRY_SPIN    0x00001000u  /* spin taken over from the lip (physAirRot[1]) */
#define RIDER_FLAG_FAKIE         0x00002000u
#define RIDER_FLAG_FORWARD_HOP   0x00004000u  /* jumped off a ramp with the stick pushed up */
#define RIDER_FLAG_LAND_ALT      0x00010000u  /* landing plays motion 22 instead of 5 */
#define RIDER_FLAG_GRIND_BUTTON  0x00020000u  /* one frame: grind button pressed while riding */
#define RIDER_FLAG_TURNAROUND    0x00040000u  /* physics asks for a 180 turnaround */
#define RIDER_FLAG_MIRROR        0x00080000u  /* grind / stall from the other side: clips mirrored */
#define RIDER_FLAG_MANUAL        0x00100000u  /* landed into a manual (down, down) */
#define RIDER_FLAG_NOSE_MANUAL   0x00200000u  /* landed into a nose wheelie (up, up) */
#define RIDER_FLAG_CAN_MANUAL    0x00400000u  /* set by physics; game.c shows the manual hint */
#define RIDER_FLAG_LIMP          0x01000000u  /* crash: rag doll gone limp */
#define RIDER_FLAG_RAGDOLL       0x04000000u  /* crash: rag doll steered by the stick */
#define RIDER_FLAG_LEFT_LIP      0x08000000u  /* jumped out of a grind / stall / manual */

/* BmxMotionSlot.flags */
#define MOTION_RESTART  0x002u   /* start the clip from frame 0 (and clear rootMotion) */
#define MOTION_MIRROR   0x008u   /* swap left/right channels (NgIFzactor table 0x0055F364), negate y/z */
#define MOTION_CUT      0x080u   /* restart without the crossfade from the previous clip (zactorPlayMotion);
                                    riderUpdateAll forces it while it steps the decoder */
#define MOTION_FROZEN   0x100u   /* do not step this frame */
#define MOTION_BLEND    0x200u   /* blend motion[0] and motion[1] (weight motion[1].rate) */

/* controller actions ACT_* and statuses ACT_PRESSED / ACT_HELD / ACT_RELEASED: contrler.h (DM-051;
 * contrlerTestAction looks them up in g_contrlerActions 0x00553EE0 [config][action][status]) */

/* RIDER_DIR_*: g_dirFromMask maps the d-pad/stick bits (1 up, 2 down, 4 left, 8 right) to these */
enum {
    RIDER_DIR_NONE = 0, RIDER_DIR_LEFT = 1, RIDER_DIR_RIGHT = 2, RIDER_DIR_UP = 3, RIDER_DIR_DOWN = 4,
    RIDER_DIR_UP_LEFT = 5, RIDER_DIR_UP_RIGHT = 6, RIDER_DIR_DOWN_LEFT = 7, RIDER_DIR_DOWN_RIGHT = 8
};

#define TRICK_NONE        0xFF
#define TRICK_FAST_PLANT  0x2A
#define TRICK_WALL_TAP    0x2B
#define TRICK_MANUAL      0x2C
#define TRICK_NOSE_WHEELIE 0x2D
#define SURFACE_WATER     13      /* BmxRider.surface: crashing into it splashes */

/* motions requested directly (motion.zal ids; the trick clips come from the tables) */
#define MOTION_IDLE          2    /* standing, not moving */
#define MOTION_RIDE          5    /* rolling / landing */
#define MOTION_PEDAL         8
#define MOTION_SKID          10
#define MOTION_SKID_HOLD     11
#define MOTION_CROUCH        13
#define MOTION_CROUCH_HOLD   14
#define MOTION_HOP           15   /* take-off */
#define MOTION_TURN          17   /* 180 turnaround */
#define MOTION_TURN_FAKIE    18   /* turnaround, rolling out backwards */
#define MOTION_MANUAL_IN     19
#define MOTION_MANUAL        20   /* also the entry when coming straight from MOTION_HOP */
#define MOTION_MANUAL_OUT    21
#define MOTION_LAND_ALT      22
#define MOTION_AIR           23   /* airborne, no trick */
#define MOTION_AIR_IDLE      24   /* trick button with no direction / waiting in the air */
#define GRIND_MOTION(t)      ((uint16_t)((t) * 3 + 0x6A))   /* enter; +1 hold; exit = (t + 0x24) * 3 */
#define GRIND_EXIT_MOTION(t) ((uint16_t)(((t) + 0x24) * 3))

typedef int (*RiderProc)(BmxRider *r);

/* ------------------------------------------------------------------ other modules (current names) */
uint32_t bmxphysGetState(BmxRider *r);                               /* contact state */
uint32_t bmxphysSetState(BmxRider *r, int contact);                  /* request a contact state, returns the new one */
void     bmxphysAddVelocity(BmxRider *r, const RwV3d *impulse);
int      bmxphysProbeGround(const float *pos, float *ground);           /* ground probe: ground[2] = height */
void     bmxphysResetRider(BmxRider *r);                               /* put the rider back on the last safe spot */
void     gameCheckTransfer(int player, int landed);
void     bmxsoundEvent(int cmd, const void *pos, uint32_t a, uint32_t b, uint32_t c);
float   *zrollerGetWorldBounds(void);                                  /* level bounding box min[4], max[4] */
void    *zrollerCreateRider(BmxRider *r);                           /* create the rider actor */
void    *zrollerCreateBike(int32_t *bikeModel);                    /* create the bike actor */
uint8_t *zrollerGetPlayer(uint32_t player);                       /* player options; +7 bike */
void     zrollerGetPlayerStartPos(int which, float *pos, int16_t *angles);
void    *zbspGetPropShape(int16_t shape);                            /* collision object of a shape id */
void    *zcolGetPropShape(int16_t shape);
void     NgIFzactor_ApplyMotionPose(ZActor *a);                       /* 0x004654B0 pose the model from the decoder */
void     zrwModelUpdateObj(void *model);                              /* 0x004892A0 */
/* the crash rag doll and the limb IK (dyn.c, DM-058; types in dyn.h, declared loosely here) */
typedef void (*DynImpactFn)(void *body, int particle, float force, void *hit);
void     dynIkReset(void *actor, void *ik);                           /* 0x00455EF0, empty */
void     dynIkMeasure(void *actor, void *ik);                         /* 0x00455F10 limb lengths */
void    *dynCreateRider(void *actor, int superCrash);                 /* 0x00452E80 */
void    *dynCreateBike(void);                                         /* 0x004526A0 */
void     dynSetImpactCallback(void *body, DynImpactFn fn);            /* 0x00451280 */
void     dynSetStickyCrash(void *body, void *actor);                  /* 0x00454400 */
void     dynSetNormalCrash(void *body, void *actor);                  /* 0x00454430 */
void     dynUntuck(void *body, void *actor);                          /* 0x00454140 limbs back after a tuck */
void     dynUnbrace(void *body, void *actor);                         /* 0x00453B70 arms back */
void     dynClearPitchInput(void *body, void *actor);                 /* 0x004546A0 */
void     dynClearRollInput(void *body, void *actor);                  /* 0x00454790 */
void     dynSetPitchInput(void *body, void *actor, int16_t angle);    /* 0x00454660 spin the body (stick) */
void     dynSetRollInput(void *body, void *actor, int16_t angle);     /* 0x00454750 */
void     dynRelease(void *body, void *actor);                         /* 0x00453890 let go of the edge */
void     dynGrab(void *body, void *actor);                            /* 0x004535D0 hands catch a nearby edge */
int      dynIsHanging(void);                                          /* 0x00453870 */
void     dynTuck(void *body, void *actor);                            /* 0x00453F80 curl up */

extern void   *g_motionTable[239];          /* 0x006962FC [motion id], filled by action.c */
extern RwV3d   g_zmathOrigin;               /* 0x006702D8 (0, 0, 0) */
extern const uint8_t  g_trickFlags[28];     /* 0x00558FB4 bits 0-2 flip kind, bit 3 loops while held */
extern const uint8_t  g_modFlags[38];       /* 0x00558FD0 bit 3: counts again every 16 frames held */
extern const uint16_t g_trickMotion[28];    /* 0x00558FF8 */
extern const uint16_t g_modMotion[38];      /* 0x00559030 */

/* ------------------------------------------------------------------ this file's tables */
extern const BmxSigCombo g_signatureTricks[12][4];  /* 0x00553840 [rider] */
extern const float    g_pedalSpeedPerStat;          /* 0x0055886C 43.0: pedalling stops above 43 * stats[3] */
extern const float    g_pedalForcePerStat;          /* 0x00558870 3.0 */
extern const float    g_autoPedalMargin;            /* 0x00558874 2.4 */
extern const float    g_hopMaxPerStat;              /* 0x00558878 21.0: hop impulse 15 .. 21 * stats[0] */
extern const float    g_hopMin;                     /* 0x0055887C 15.0 */
extern const float    g_skidDrag;                   /* 0x00558888 0.1 */
extern const float    g_steerRate;                  /* 0x0055888C 15.0 degrees / frame at full lock */
extern const float    g_steerLimit;                 /* 0x00558890 75.0 */
extern const float    g_steerSpeedFalloff;          /* 0x00558894 0.15 */
extern const float    g_airNudge;                   /* 0x00558898 0.2 degrees / frame */
extern const float    g_spinStart;                  /* 0x0055889C 8.0 degrees / frame */
extern const float    g_spinMaxPerStat;             /* 0x005588A0 12.0 */
extern const float    g_flipAccelPerStat;           /* 0x005588A4 3.0 */
extern const float    g_flipMaxPerStat;             /* 0x005588A8 12.0 */
extern const float    g_balanceRate;                /* 0x005588B0 1.5 degrees / frame (grind, stall) */
extern const uint32_t g_hopChargeFrames;            /* 0x005588B4 25 */
extern const RiderProc g_riderMainProc[7];          /* 0x005588B8 [state[0]] */
extern const RiderProc g_riderRideProc[6];          /* 0x005588D4 [state[1]] riding */
extern const RiderProc g_riderAirProc[3];           /* 0x005588EC air */
extern const RiderProc g_riderGrindProc[1];         /* 0x005588F8 */
extern const RiderProc g_riderStallProc[1];         /* 0x005588FC */
extern const RiderProc g_riderManualProc[1];        /* 0x00558900 */
extern const uint8_t  g_dirFromMask[16];            /* 0x00558904 d-pad/stick bits -> RIDER_DIR_* */
extern const float    g_impactPartScale[14];        /* 0x00558914 crash damage per dynamics body part */
extern const uint8_t  g_airTrickDir1[9];            /* 0x00558AF7 [last direction] */
extern const uint8_t  g_airTrickDir2[8][8];         /* 0x00558B00 [previous - 1][last - 1], 0xFF none */
extern const uint8_t  g_modDir1[9];                 /* 0x00558B40 [last direction] */
extern const uint8_t  g_modDir2[8][8];              /* 0x00558B4C [previous - 1][last - 1], 0xFF none */
extern const uint8_t  g_trickModTable[27][38];      /* 0x00558B8C [air trick][wanted modifier] -> modifier, 0xFF not allowed */
extern const uint8_t  g_grindTrickDir[9];           /* 0x00558F90 [direction] -> 0x1B..0x23 */
extern const uint8_t  g_stallTrickDir[9];           /* 0x00558F9C [direction] -> 0x24..0x29 */
extern const uint8_t  g_plantTrickDir[9];           /* 0x00558FA8 [direction] -> fast plant / wall tap / 0xFF */
extern const float    g_riderStats[12][7];          /* 0x00553664 [rider] */
extern const float    g_bikeStats[5][7];            /* 0x005537B4 [bike] */

/* this file */
int      riderStateRiding(BmxRider *r);
int      riderStateAir(BmxRider *r);
int      riderStateGrind(BmxRider *r);
int      riderStateStall(BmxRider *r);
int      riderStateManual(BmxRider *r);
int      riderStateLanding(BmxRider *r);
int      riderStateCrash(BmxRider *r);
uint32_t riderSetState(BmxRider *r, uint32_t level, uint32_t state);
uint32_t riderGetState(const BmxRider *r, uint32_t level);
void     riderResetDirections(BmxRider *r, int record);
uint8_t  riderCurrentDirection(const BmxRider *r);
uint8_t  riderGrindTrick(const BmxRider *r);
uint8_t  riderStallTrick(const BmxRider *r);
uint8_t  riderPlantTrick(const BmxRider *r);
void     riderAddCrashDamage(BmxRider *r, int part, float force);
void     riderOnRiderImpact(void *dyn, int part, float force, uint32_t *contact);
void     riderOnBikeImpact(void *dyn, int part, float force, uint32_t *contact);

/* ------------------------------------------------------------------ small helpers (inlined in the exe) */

/* d-pad bits of `buttons` plus the stick beyond half way -> the 4-bit direction mask */
static uint32_t dirMask(uint32_t buttons, float x, float y)
{
    uint32_t m = buttons;

    if (fabsf(x) > 0.5f)
        m |= (x < 0.0f) ? 4u : 8u;
    if (fabsf(y) > 0.5f)
        m |= (y < 0.0f) ? 1u : 2u;
    return m & 0xF;
}

static uint8_t lastDir(const BmxRider *r)
{
    return r->dirBuf[r->dirCount - 1];
}

static uint8_t prevDir(const BmxRider *r)
{
    return r->dirBuf[r->dirCount - 2];
}

static int act(BmxRider *r, int action, int status)
{
    return contrlerTestAction(r->profileByte2, &r->input, (uint32_t)action, (uint32_t)status);
}

static float *stick(BmxRider *r)
{
    return contrlerGetStick(&r->input, 0);
}

static void requestMotion(BmxRider *r, uint16_t id)
{
    r->motion[0].id = id;
    r->motion[0].flags |= MOTION_RESTART;
}

/* hop strength: 15 .. 21 * stats[0], growing with the frames the jump button was held (max 25) */
static float hopImpulse(const BmxRider *r, uint16_t held)
{
    uint32_t n = held;

    if (n >= g_hopChargeFrames)
        n = g_hopChargeFrames;
    return (g_hopMaxPerStat * r->stats[0] - g_hopMin) * (float)(int)n / (float)(int)g_hopChargeFrames + g_hopMin;
}

/* turn around: right and at axes of both matrices flip, the roll and pitch rates with them */
static void riderTurnAround(BmxRider *r)
{
    r->motion[0].flags |= MOTION_CUT;
    zmathVecNegate((RwV3d *)&r->matrix[0], (RwV3d *)&r->matrix[0]);
    zmathVecNegate((RwV3d *)&r->matrix[8], (RwV3d *)&r->matrix[8]);
    zmathVecNegate((RwV3d *)&r->bikeMatrix[0], (RwV3d *)&r->bikeMatrix[0]);
    zmathVecNegate((RwV3d *)&r->bikeMatrix[8], (RwV3d *)&r->bikeMatrix[8]);
    r->rotVel[0] = (int16_t)-r->rotVel[0];
    r->rotVel[2] = (int16_t)-r->rotVel[2];
}

/* steering on the ground (riding, manual, landing; the exe inlines it three times):
 * stick x turns the yaw rate rotVel[1]; the rate and its limit drop with speed; no stick: decays */
static void riderSteer(BmxRider *r)
{
    float x = stick(r)[0];
    int16_t yaw, limit;

    if (x == 0.0f) {
        r->rotVel[1] = (int16_t)(r->rotVel[1] - (int16_t)(r->rotVel[1] * ANGLE_TO_DEG * 0.35f * DEG_TO_ANGLE));
        if (fabsf((float)r->rotVel[1]) < 91.0f)         /* half a degree */
            r->rotVel[1] = 0;
        return;
    }
    if (r->frameDist > 0.0f) {
        float k = r->stats[1] / (g_steerSpeedFalloff * r->frameDist + 1.0f);

        if (k < 0.05f)
            k = 0.05f;
        yaw = (int16_t)(r->rotVel[1] + (int16_t)(g_steerRate * k * x * r->stats[1] * DEG_TO_ANGLE));
        limit = (int16_t)(fabsf(x) * g_steerLimit * k * DEG_TO_ANGLE);
    } else {
        yaw = (int16_t)(r->rotVel[1] + (int16_t)(x * 3.0f * r->stats[1] * DEG_TO_ANGLE));
        limit = 0x444;                                    /* 6 degrees */
    }
    if (yaw > limit)
        yaw = limit;
    if (yaw < -limit)
        yaw = (int16_t)-limit;
    r->rotVel[1] = yaw;
}

/* jump out of a grind / stall / manual / skid: impulse along -up (or straight up) and the hop clip */
static void riderJumpOff(BmxRider *r, const RwV3d *impulse, uint16_t motion)
{
    bmxphysAddVelocity(r, impulse);
    bmxphysSetState(r, CONTACT_AIR);
    riderSetState(r, 0, RIDER_STATE_AIR);
    r->motion[0].id = motion;
}

/* ------------------------------------------------------------------ per frame */

/* VA 0x00430e70, 313 bytes, [I] row, status draft */
void riderReadInput(void)
{
    /* Copy each player's pad into the rider. The sticks are quantised to 1/127 (a signed byte), the
     * resolution the replay recorder stores, so live play and replays see identical input. */
    uint32_t p;
    int i, k;

    for (p = 0; p < g_numPlayers; p++) {
        const BmxPad *pad = contrlerGetPad(g_saveProfiles[p].index);
        BmxRider *r = &g_riders[p];

        r->input.buttons = pad->input.buttons;
        r->input.changed = pad->input.changed;
        for (i = 0; i < 2; i++) {
            for (k = 0; k < 2; k++) {
                r->input.prevStick[i][k] = (float)(int8_t)(int)(pad->input.prevStick[i][k] * 127.0f) * (1.0f / 127.0f);
                r->input.stick[i][k] = (float)(int8_t)(int)(pad->input.stick[i][k] * 127.0f) * (1.0f / 127.0f);
            }
        }
    }
}

/* VA 0x00431000, 1376 bytes, [V] direct, status draft */
void riderUpdateAll(void)
{
    uint32_t p, n;

    n = g_numPlayers ? g_numPlayers : 1;
    for (p = 0; p < n; p++) {
        BmxRider *r = &g_riders[p];
        const float *box;
        RiderProc proc;
        int done;

        if (!(r->stateFlags & RIDER_FLAG_ACTIVE))
            continue;

        /* direction buffer: a new direction is appended (the oldest drops out of a full buffer);
         * a direction that only gains a second axis (left -> up-left) replaces the last entry */
        if (r->stateFlags & RIDER_FLAG_RECORD_DIRS) {
            uint32_t cur = r->input.buttons & 0xF;
            uint32_t edge = r->input.changed & 0xF;
            float x = r->input.stick[0][0], px = r->input.prevStick[0][0];
            float y = r->input.stick[0][1], py = r->input.prevStick[0][1];

            if (fabsf(x) > 0.5f) {
                cur |= (x < 0.0f) ? 4u : 8u;
                if (fabsf(px) <= 0.5f)
                    edge |= (x < 0.0f) ? 4u : 8u;      /* pushed this frame */
            } else if (fabsf(px) > 0.5f) {
                edge |= (px < 0.0f) ? 4u : 8u;         /* released this frame */
            }
            if (fabsf(y) > 0.5f) {
                cur |= (y < 0.0f) ? 1u : 2u;
                if (fabsf(py) <= 0.5f)
                    edge |= (y < 0.0f) ? 1u : 2u;
            } else if (fabsf(py) > 0.5f) {
                edge |= (py < 0.0f) ? 1u : 2u;
            }
            if (edge != 0) {
                if (edge == cur) {
                    if (r->dirCount == 4) {
                        uint32_t i;
                        for (i = 0; i < 3u; i++)
                            r->dirBuf[i] = r->dirBuf[i + 1];
                        r->dirCount--;
                    }
                    r->dirBuf[r->dirCount++] = g_dirFromMask[cur];
                } else if (cur & edge) {
                    if (r->dirCount == 0)
                        r->dirBuf[r->dirCount++] = g_dirFromMask[cur];
                    else
                        r->dirBuf[r->dirCount - 1] = g_dirFromMask[cur];
                }
            }
            if (r->dirCount > 4)
                zAssertFail("direction buffer out of range (post)\n", RIDER_SRC, 0x14d, 0);
        }

        /* fell out of the level: back to the last safe spot */
        box = zrollerGetWorldBounds();
        if (r->pos[1] > box[5] + 100.0f || r->pos[0] < box[0] - 100.0f || r->pos[0] > box[4] + 100.0f ||
            r->pos[2] < box[2] - 100.0f || r->pos[2] > box[6] + 100.0f) {
            bmxphysResetRider(r);
            riderSetState(r, 0, RIDER_STATE_RIDING);
        }

        r->stateFlags &= ~(RIDER_FLAG_GRIND_BUTTON | RIDER_FLAG_CHAIN);
        r->pedalForce = 0.0f;
        r->wheelDrag = 0.0f;
        proc = g_riderMainProc[r->state[0]];
        if (proc == 0)
            zAssertFail("Invalid main update proc. (NULL)", RIDER_SRC, 0x168, 0);
        done = proc(r);
        while (!done)
            done = g_riderMainProc[r->state[0]](r);

        r->stateTimer[0]++;
        r->motion[0].flags |= MOTION_CUT;
        if (!(r->motion[0].flags & MOTION_FROZEN)) {
            if (r->motion[0].flags & MOTION_RESTART)
                zmathVecZero((RwV3d *)r->rootMotion);
            if (!(r->motion[0].flags & MOTION_BLEND)) {
                ZactorFrame *f = zactorPlayMotion(r->riderActor, g_motionTable[r->motion[0].id],
                                                  r->motion[0].rate, r->motion[0].flags);
                const uint32_t *next;

                r->motion[0].frame = f;
                if (f != 0) {
                    /* root motion: x/z per-frame deltas summed, y = -(clip height) */
                    r->rootMotion[1] = -zactorGetHeight(r->riderActor);
                    r->rootMotion[0] += f->delta[0];
                    r->motion[0].flags &= ~MOTION_RESTART;
                    r->rootMotion[2] += f->delta[2];
                    zactorCopyBikePose(r->riderActor, r->bikeActor);
                    /* the decoder's next record (anim->block[1].rec); none / terminator = clip over */
                    next = ((ZActor *)r->riderActor)->anim->block[1].rec;
                    if (next != 0 && *next != 0) {
                        r->stateFlags &= ~RIDER_FLAG_MOTION_DONE;
                        goto posed;
                    }
                }
                r->stateFlags |= RIDER_FLAG_MOTION_DONE;
            } else {
                /* two clips blended: motion[1].rate is the weight; root motion from the second */
                uint16_t fl[2];
                ZactorFrame **pf;

                fl[0] = (uint16_t)r->motion[0].flags;
                fl[1] = (uint16_t)r->motion[1].flags;
                pf = zactorPlayMotionBlend(r->riderActor, g_motionTable[r->motion[0].id],
                                           g_motionTable[r->motion[1].id], fl, r->motion[1].rate);
                if (pf == 0 || pf[0] == 0 || pf[1] == 0) {
                    r->motion[0].frame = 0;
                    r->motion[1].frame = 0;
                    r->stateFlags |= RIDER_FLAG_MOTION_DONE;
                } else {
                    r->motion[0].frame = pf[0];
                    r->motion[1].frame = pf[1];
                    r->rootMotion[1] = -zactorGetHeight(r->riderActor);
                    r->rootMotion[0] += pf[1]->delta[0];
                    r->motion[0].flags &= ~MOTION_RESTART;
                    r->motion[1].flags &= ~MOTION_RESTART;
                    r->rootMotion[2] += pf[1]->delta[2];
                    zactorCopyBikePose(r->riderActor, r->bikeActor);
                    r->stateFlags &= ~RIDER_FLAG_MOTION_DONE;
                }
                r->motion[0].flags &= ~MOTION_BLEND;
            }
posed:
            r->motion[0].flags &= ~MOTION_CUT;
            NgIFzactor_ApplyMotionPose((ZActor *)r->riderActor);
            zrwModelUpdateObj(*(void **)((uint8_t *)r->riderActor + 0x1D4));
        }
        if (r->impactTimer != 0)
            r->impactTimer--;
    }
}

/* ------------------------------------------------------------------ create / reset */

/* VA 0x004316c0, 774 bytes, [I] row, status draft */
BmxRider *riderCreate(int slot, int rider, int style, uint32_t bike)
{
    BmxRider *r = &g_riders[slot];
    uint32_t cheats = g_saveBlockA.cheats_active;
    uint32_t saved, i;

    r->slot = (uint8_t)slot;
    r->profileByte2 = g_saveProfiles[slot].unk_02;
    r->rider = (int16_t)rider;
    r->style = (uint8_t)style;
    r->bikeModel = (bike < 4) ? (int32_t)(bike + rider * 4) : 0x30;     /* 0x30: the secret bike */

    r->riderActor = zrollerCreateRider(r);
    if (r->riderActor == 0) {
        g_pfnDebugPrintf("Failed to create rider actor for rider %ld (lookNum %ld)\n", slot, style);
        return 0;
    }
    r->bikeActor = zrollerCreateBike(&r->bikeModel);
    if (r->bikeActor == 0) {
        g_pfnDebugPrintf("Failed to create bike actor for rider %ld (bikeLookNum %ld)\n", slot, bike);
        return 0;
    }
    zrollerGetPlayer(slot)[7] = (uint8_t)bike;

    /* rag doll: cheat 2 Super Crash, cheat 7 Sticky Crash (not in the attract mode) */
    r->animCtl = dynCreateRider(r->riderActor, (cheats & 0x04) && g_numPlayers != 0);
    dynSetImpactCallback(r->animCtl, (DynImpactFn)riderOnRiderImpact);
    if ((cheats & 0x80) && g_numPlayers != 0)
        dynSetStickyCrash(r->animCtl, r->riderActor);
    else
        dynSetNormalCrash(r->animCtl, r->riderActor);
    r->bikeCtl = dynCreateBike();
    dynSetImpactCallback(r->bikeCtl, (DynImpactFn)riderOnBikeImpact);

    for (i = 0; i < 15; i++)
        r->cleared_bc0[i] = 0;

    /* rider x bike stats; stock bikes clamp to 1.0 (stat 6: 2.0), the secret bike does not */
    for (i = 0; i < 7; i++) {
        float s = g_bikeStats[bike][i] * g_riderStats[rider][i];

        r->stats[i] = s;
        if (bike < 4) {
            if (i == 6)
                r->stats[6] = (s > 2.0f) ? 2.0f : s;
            else
                r->stats[i] = (s > 1.0f) ? 1.0f : s;
        }
    }

    r->tuneC68 = 50.0f;
    r->tuneC6C = 5.0f;
    r->tuneC70 = 0.2f;
    r->wheel[0].pos[0] = 55.253f;
    r->wheel[0].pos[1] = -5.596f;
    r->wheel[0].pos[2] = 0.0f;
    r->wheel[0].radius = 24.645f;
    r->wheel[1].pos[0] = -36.52f;
    r->wheel[1].pos[1] = -5.596f;
    r->wheel[1].pos[2] = 0.0f;
    r->wheel[1].radius = 24.645f;
    if ((cheats & 0x10) && g_numPlayers != 0) {        /* cheat 4: Suspension */
        r->suspension[0] = -15.0f;
        r->suspension[1] = 25.0f;
        r->suspension[2] = 0.3f;
        r->suspension[3] = 0.1f;
        r->suspension[4] = 0.1f;
    } else {
        for (i = 0; i < 5; i++)
            r->suspension[i] = 0.0f;
    }

    dynIkReset(r->riderActor, &r->ik);
    saved = cameraGetPlayer();
    cameraSetPlayer((uint32_t)slot);
    dynIkMeasure(r->riderActor, &r->ik);
    cameraSetPlayer(saved);
    r->ik.unk_100 = 1.0f;
    r->ik.unk_104 = 0.0f;
    r->ik.unk_108 = 0.0f;
    return r;
}

/* VA 0x00431aa0, 372 bytes, [I] row, status draft */
void riderReset(int slot, int active)
{
    /* put the rider on the level's start position, standing still, rag doll off */
    BmxRider *r = &g_riders[slot];
    int16_t angles[4];
    int i;

    zrollerGetPlayerStartPos(0, r->pos, angles);
    zmathMatrixIdentity((RwMatrix *)r->matrix);
    zmathMatrixRotateXYZ((RwMatrix *)r->matrix, (RwMatrix *)r->matrix, angles);
    dynUntuck(r->animCtl, r->riderActor);
    dynUnbrace(r->animCtl, r->riderActor);
    dynClearPitchInput(r->animCtl, r->riderActor);
    dynClearRollInput(r->animCtl, r->riderActor);
    dynRelease(r->animCtl, r->riderActor);

    r->motion[0].flags = MOTION_CUT;
    r->input.buttons = 0;
    r->input.changed = 0;
    for (i = 0; i < 2; i++) {
        r->input.stick[i][0] = r->input.stick[i][1] = 0.0f;
        r->input.prevStick[i][0] = r->input.prevStick[i][1] = 0.0f;
    }
    r->dirBuf[0] = r->dirBuf[1] = r->dirBuf[2] = r->dirBuf[3] = 0;
    r->dirCount = 0;
    r->stateFlags |= RIDER_FLAG_MOTION_DONE;
    riderSetState(r, 0, RIDER_STATE_RIDING);
    r->physState = 0xFF;                 /* BMXPHYS_NONE until the first bmxphysSetState (DM-048) */
    bmxphysResetRider(r);
    r->impactTimer = 0;
    r->motion[0].rate = 1.0f;
    if (!active) {
        r->stateFlags &= ~RIDER_FLAG_ACTIVE;
        zactorHide(r->riderActor);
        zactorHide(r->bikeActor);
    } else {
        r->stateFlags |= RIDER_FLAG_ACTIVE;
        zactorShow(r->riderActor);
        zactorShow(r->bikeActor);
    }
    dynIkReset(r->riderActor, &r->ik);
}

/* VA 0x00431c80, 1 bytes, [I] row, status draft */
void riderNullProc(void)
{
    /* empty; only reachable through its /INCREMENTAL thunk 0x00401366, which nothing calls */
}

/* ------------------------------------------------------------------ state machine */

/* VA 0x00431ca0, 214 bytes, [I] row, status draft */
uint32_t riderSetState(BmxRider *r, uint32_t level, uint32_t state)
{
    uint32_t i;

    r->state[level] = (uint8_t)state;
    r->stateTimer[level] = 0;
    for (i = level + 1; i < 4; i++) {
        r->state[i] = 0;
        r->stateTimer[i] = 0;
    }
    r->motion[0].rate = 1.0f;
    r->motion[0].flags &= ~MOTION_FROZEN;

    if (level == 0) {
        switch (state) {
        case RIDER_STATE_AIR:
            riderResetDirections(r, 1);
            r->stateFlags &= ~(RIDER_FLAG_MANUAL | RIDER_FLAG_NOSE_MANUAL);
            break;
        case RIDER_STATE_GRIND:
        case RIDER_STATE_STALL:
            break;
        case RIDER_STATE_LANDING:
            return state;                       /* keeps the flip angle for the landing check */
        default:                                /* riding, manual, crash: the trick is over */
            r->motion[0].flags &= ~(MOTION_FROZEN | MOTION_MIRROR);
            riderResetDirections(r, 0);
            r->trickId = TRICK_NONE;
            r->modId = TRICK_NONE;
            r->spin = 0;
            r->modCount = 0;
            r->stateFlags &= ~(RIDER_FLAG_FLIP | RIDER_FLAG_LEFT_LIP);
            r->motion[1].id = 4;
            break;
        }
    } else if (level != 1) {
        return r->state[level];
    }
    r->flip = 0;
    r->stateFlags &= ~(RIDER_FLAG_IDLE | RIDER_FLAG_MODIFIER);
    return r->state[level];
}

/* VA 0x00431dd0, 47 bytes, [V] direct, status draft */
uint32_t riderGetState(const BmxRider *r, uint32_t level)
{
    if (level > 3)
        zAssertFail("stateLevel out of range.", RIDER_SRC, 0x2c5, 0);
    return r->state[level];
}

/* VA 0x00431e10, 110 bytes, [I] row, status draft */
void riderAddCrashDamage(BmxRider *r, int part, float force)
{
    /* damage = force x part weight (g_impactPartScale; part 13 = 2.0) / 12; a head hit while upside
     * down (up.y > 0, Y down) counts 1 + 5 up.y times; half while the rag doll is limp */
    float k = 1.0f;

    if (r->stateFlags & RIDER_FLAG_LIMP)
        k = 0.5f;
    else if (part == 13 && r->matrix[5] > 0.0f)
        k = r->matrix[5] * 5.0f + 1.0f;
    r->crash += (int32_t)(k * g_impactPartScale[part] * force * (1.0f / 12.0f));
}

/* ------------------------------------------------------------------ state[0] procedures */

/* VA 0x00431ea0, 802 bytes, [I] row, status draft */
int riderStateRiding(BmxRider *r)
{
    int done;

    switch (bmxphysGetState(r)) {
    case CONTACT_AIR:
        if (riderGetState(r, 1) == RIDE_TURNAROUND && riderGetState(r, 2) == 2)
            riderTurnAround(r);                     /* left the ground half way round */
        riderSetState(r, 0, RIDER_STATE_AIR);
        r->motion[0].id = MOTION_AIR;
        return 0;
    case CONTACT_CRASH:
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    case CONTACT_GRIND_ENTER:
        riderSetState(r, 0, RIDER_STATE_GRIND);
        return 0;
    case CONTACT_GRIND:                             /* rolled onto the rail: straight to the hold */
        riderSetState(r, 0, RIDER_STATE_GRIND);
        riderSetState(r, 2, 2);
        r->trickId = riderGrindTrick(r);
        return 0;
    case CONTACT_STALL:
        riderSetState(r, 0, RIDER_STATE_STALL);
        riderSetState(r, 2, 2);
        r->trickId = riderStallTrick(r);
        r->stateFlags |= RIDER_FLAG_HOLD;
        return 0;
    }
    if ((r->stateFlags & RIDER_FLAG_TURNAROUND) && riderGetState(r, 1) != RIDE_TURNAROUND)
        riderSetState(r, 1, RIDE_TURNAROUND);
    riderSteer(r);
    if (act(r, ACT_GRIND, ACT_HELD))
        r->stateFlags |= RIDER_FLAG_GRIND_BUTTON;

    done = g_riderRideProc[r->state[1]](r);
    while (!done && riderGetState(r, 0) == RIDER_STATE_RIDING)
        done = g_riderRideProc[r->state[1]](r);
    return done;
}

/* start a new air trick (or modifier-only trick): the one in progress is cut short */
static void riderAirInterrupt(BmxRider *r)
{
    if (riderGetState(r, 1) != AIR_IDLE) {
        riderSetState(r, 1, AIR_IDLE);
        g_riderAirProc[r->state[1]](r);
        r->stateFlags |= RIDER_FLAG_CHAIN;
    }
}

/* VA 0x004322b0, 2161 bytes, [I] row, status draft */
int riderStateAir(BmxRider *r)
{
    uint32_t s1, contact;
    int left, right, done;
    float x, y;

    switch (bmxphysGetState(r)) {
    case CONTACT_GROUND:
        gameCheckTransfer(r->slot, 1);
        /* landing with down, down (stick down) -> manual; up, up (stick up) -> nose wheelie */
        if ((r->stateFlags & RIDER_FLAG_CAN_MANUAL) && r->dirCount >= 2) {
            y = stick(r)[1];
            if (lastDir(r) == RIDER_DIR_DOWN && prevDir(r) == RIDER_DIR_DOWN && y > 0.5f) {
                r->stateFlags |= RIDER_FLAG_MANUAL;
            } else if (lastDir(r) == RIDER_DIR_UP && prevDir(r) == RIDER_DIR_UP && y < -0.5f) {
                r->stateFlags |= RIDER_FLAG_NOSE_MANUAL;
            } else {
                riderSetState(r, 0, RIDER_STATE_LANDING);
                return 0;
            }
            riderSetState(r, 0, RIDER_STATE_MANUAL);
            r->stateFlags |= RIDER_FLAG_CHAIN;
            return 0;
        }
        riderSetState(r, 0, RIDER_STATE_LANDING);
        return 0;
    case CONTACT_CRASH:
        gameCheckTransfer(r->slot, 0);
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    case CONTACT_GRIND_ENTER:
        riderSetState(r, 0, RIDER_STATE_GRIND);
        return 0;
    }

    if (act(r, ACT_BAIL_A, ACT_PRESSED) || act(r, ACT_BAIL_B, ACT_PRESSED)) {
        bmxphysSetState(r, CONTACT_CRASH);
        r->crashKind = 11;
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    }

    /* spin: the spin buttons start at 8 degrees / frame and build up x1.25 per frame to
     * 12 * stats[2]; without them the stick nudges (0.2 deg) or builds up the same way;
     * nothing held: the spin rate halves every frame */
    left = act(r, ACT_SPIN_LEFT, ACT_HELD);
    right = act(r, ACT_SPIN_RIGHT, ACT_HELD);
    if (left != right) {
        int16_t lim = ANGLE_DEG(g_spinMaxPerStat * r->stats[2]);

        if (r->rotVel[1] != 0 && act(r, ACT_SPIN_LEFT, ACT_HELD) == (r->rotVel[1] < 0)) {
            int16_t v = (int16_t)(r->rotVel[1] * ANGLE_TO_DEG * 1.25f * DEG_TO_ANGLE);

            if (v > lim)
                v = lim;
            if (v < -lim)
                v = (int16_t)-lim;
            r->rotVel[1] = v;
        } else {                                    /* start (or reverse): left negative */
            r->rotVel[1] = ANGLE_DEG(act(r, ACT_SPIN_LEFT, ACT_HELD) ? -g_spinStart : g_spinStart);
        }
    } else if ((x = stick(r)[0]) != 0.0f) {
        float lim = ANGLE_DEG(g_spinMaxPerStat * r->stats[2]) * ANGLE_TO_DEG;
        float v;

        if (r->rotVel[1] != 0 && (x < 0.0f) == (r->rotVel[1] < 0)) {
            v = r->rotVel[1] * ANGLE_TO_DEG * 1.25f;
            if (v > lim)
                v = lim;
            if (v < -lim)
                v = -lim;
        } else {
            v = (x < 0.0f) ? -g_airNudge : g_airNudge;
        }
        r->rotVel[1] = (int16_t)(v * DEG_TO_ANGLE);
    } else {
        r->rotVel[1] = (int16_t)(r->rotVel[1] - (int16_t)(r->rotVel[1] * ANGLE_TO_DEG * 0.5f * DEG_TO_ANGLE));
        if (fabsf((float)r->rotVel[1]) < 91.0f)
            r->rotVel[1] = 0;
    }
    r->spin += r->rotVel[1];
    if ((r->stateFlags & RIDER_FLAG_CARRY_SPIN) && r->physTimer != 0) {
        /* still locked to the lip: keep the carried spin unless the player spins faster */
        if (fabsf((float)r->rotVel[1]) <= fabsf((float)r->physAirRot[1])) {
            r->spin += r->physAirRot[1];
        } else {
            r->physTimer = 0;
            r->stateFlags &= ~RIDER_FLAG_CARRY_SPIN;
        }
    }
    r->rotVel[2] = (int16_t)(r->rotVel[2] >> 1);
    if (fabsf((float)r->rotVel[2]) < 91.0f)
        r->rotVel[2] = 0;

    /* down, down / up, up held: lean the bike forward / back */
    if (r->dirCount > 1) {
        y = stick(r)[1];
        if (fabsf(y) > 0.5f) {
            if (lastDir(r) == RIDER_DIR_DOWN && prevDir(r) == RIDER_DIR_DOWN && y > 0.5f) {
                int v = r->airLean + 0x2D8;
                r->airLean = (int16_t)(v > 0x18E3 ? 0x18E3 : v);
            } else if (lastDir(r) == RIDER_DIR_UP && prevDir(r) == RIDER_DIR_UP && y < -0.5f) {
                int v = r->airLean - 0x1C7;
                r->airLean = (int16_t)(v < -0x18E3 ? -0x18E3 : v);
            }
        }
    }

    /* new tricks only when idle or while the current one is releasing */
    s1 = riderGetState(r, 1);
    if (s1 == AIR_IDLE || (s1 == AIR_TRICK && riderGetState(r, 2) >= 3) ||
        (s1 == AIR_MODIFIER && riderGetState(r, 2) >= 3)) {
        /* trick button: a trick needs a direction; tapped before it, the button stays armed 8 frames */
        if (act(r, ACT_TRICK, ACT_PRESSED) ||
            ((r->stateFlags & RIDER_FLAG_TRICK_ARMED) && r->dirCount != 0)) {
            if (r->dirCount != 0) {
                if (riderGetState(r, 1) != AIR_IDLE) {
                    riderSetState(r, 1, AIR_IDLE);
                    g_riderAirProc[r->state[1]](r);
                    r->rotVel[2] = 0;
                    r->stateFlags |= RIDER_FLAG_CHAIN;
                }
                riderSetState(r, 1, AIR_TRICK);
                return 0;
            }
            r->stateTimer[3] = 0;
            r->stateFlags = (r->stateFlags & ~RIDER_FLAG_MOD_ARMED) | RIDER_FLAG_TRICK_ARMED;
        } else if ((r->stateFlags & RIDER_FLAG_TRICK_ARMED) && ++r->stateTimer[3] > 7) {
            r->stateFlags &= ~RIDER_FLAG_TRICK_ARMED;
        }
        /* modifier button: the same for a modifier-only trick */
        if (act(r, ACT_MODIFIER, ACT_PRESSED) ||
            ((r->stateFlags & RIDER_FLAG_MOD_ARMED) && r->dirCount != 0)) {
            if (r->dirCount != 0) {
                riderAirInterrupt(r);
                riderSetState(r, 1, AIR_MODIFIER);
                r->rotVel[2] = 0;
                return 0;
            }
            r->stateTimer[3] = 0;
            r->stateFlags = (r->stateFlags & ~RIDER_FLAG_TRICK_ARMED) | RIDER_FLAG_MOD_ARMED;
        } else if ((r->stateFlags & RIDER_FLAG_MOD_ARMED) && ++r->stateTimer[3] > 7) {
            r->stateFlags &= ~RIDER_FLAG_MOD_ARMED;
        }
        /* grind button near a rail / lip */
        if (act(r, ACT_GRIND, ACT_HELD) && riderGetState(r, 2) != 0) {
            contact = bmxphysSetState(r, CONTACT_GRIND_ENTER);
            if (contact == CONTACT_GRIND_ENTER) {
                riderAirInterrupt(r);
                riderSetState(r, 0, RIDER_STATE_GRIND);
                return 0;
            }
            if (contact == CONTACT_STALL_ENTER) {
                riderAirInterrupt(r);
                riderSetState(r, 0, RIDER_STATE_STALL);
                r->trickId = riderStallTrick(r);
                return 0;
            }
        }
        /* plant button + left / right: fast plant / wall tap */
        if (act(r, ACT_PLANT, ACT_PRESSED) && riderPlantTrick(r) != TRICK_NONE) {
            if (bmxphysSetState(r, CONTACT_STALL_ENTER) == CONTACT_STALL_ENTER) {
                riderAirInterrupt(r);
                riderSetState(r, 0, RIDER_STATE_STALL);
                r->trickId = riderPlantTrick(r);
                return 0;
            }
            riderResetDirections(r, 1);
        }
    }

    done = g_riderAirProc[r->state[1]](r);
    while (!done && riderGetState(r, 0) == RIDER_STATE_AIR)
        done = g_riderAirProc[r->state[1]](r);
    return done;
}

/* VA 0x00432d50, 179 bytes, [I] row, status draft */
int riderStateGrind(BmxRider *r)
{
    int done;

    switch (bmxphysGetState(r)) {
    case CONTACT_GROUND:
        riderSetState(r, 0, RIDER_STATE_RIDING);
        return 0;
    case CONTACT_AIR:                               /* dropped off the end */
        riderSetState(r, 0, RIDER_STATE_AIR);
        r->motion[0].id = GRIND_EXIT_MOTION(r->trickId);
        r->stateFlags |= RIDER_FLAG_LEFT_LIP;
        return 0;
    case CONTACT_CRASH:
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    }
    done = g_riderGrindProc[r->state[1]](r);
    while (!done && riderGetState(r, 0) == RIDER_STATE_GRIND)
        done = g_riderGrindProc[r->state[1]](r);
    return done;
}

/* VA 0x00432e40, 179 bytes, [I] row, status draft */
int riderStateStall(BmxRider *r)
{
    int done;

    switch (bmxphysGetState(r)) {
    case CONTACT_GROUND:
        riderSetState(r, 0, RIDER_STATE_RIDING);
        return 0;
    case CONTACT_AIR:
        riderSetState(r, 0, RIDER_STATE_AIR);
        r->motion[0].id = GRIND_EXIT_MOTION(r->trickId);
        r->stateFlags |= RIDER_FLAG_LEFT_LIP;
        return 0;
    case CONTACT_CRASH:
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    }
    done = g_riderStallProc[r->state[1]](r);
    while (!done && riderGetState(r, 0) == RIDER_STATE_STALL)
        done = g_riderStallProc[r->state[1]](r);
    return done;
}

/* VA 0x00432f30, 525 bytes, [I] row, status draft */
int riderStateManual(BmxRider *r)
{
    int done;

    switch (bmxphysGetState(r)) {
    case CONTACT_AIR:
        riderSetState(r, 0, RIDER_STATE_AIR);
        r->motion[0].id = MOTION_MANUAL_OUT;
        r->stateFlags |= RIDER_FLAG_LEFT_LIP;
        return 0;
    case CONTACT_CRASH:
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    }
    if (!(r->stateFlags & RIDER_FLAG_CAN_MANUAL)) {
        riderSetState(r, 0, RIDER_STATE_RIDING);
        return 0;
    }
    riderSteer(r);
    done = g_riderManualProc[r->state[1]](r);
    while (!done && riderGetState(r, 0) == RIDER_STATE_MANUAL)
        done = g_riderManualProc[r->state[1]](r);
    return done;
}

/* VA 0x004331d0, 530 bytes, [I] row, status draft */
int riderStateLanding(BmxRider *r)
{
    switch (bmxphysGetState(r)) {
    case CONTACT_AIR:
        riderSetState(r, 0, RIDER_STATE_AIR);
        r->motion[0].id = MOTION_AIR;
        return 0;
    case CONTACT_CRASH:
        riderSetState(r, 0, RIDER_STATE_CRASH);
        return 0;
    }
    riderSteer(r);
    switch (riderGetState(r, 1)) {
    case 0:
        r->motion[0].id = (r->stateFlags & RIDER_FLAG_LAND_ALT) ? MOTION_LAND_ALT : MOTION_RIDE;
        riderSetState(r, 1, 1);
        r->motion[0].flags |= MOTION_RESTART;
        break;
    case 1:
        if (++r->stateTimer[1] > 6) {               /* 7 frames, then riding again */
            riderSetState(r, 0, RIDER_STATE_RIDING);
            return 0;
        }
        break;
    }
    return 1;
}

/* VA 0x00433480, 1058 bytes, [I] row, status draft */
int riderStateCrash(BmxRider *r)
{
    float ground[4];
    float *s;

    switch (riderGetState(r, 1)) {
    case CRASH_START:
        dynRelease(r->animCtl, r->riderActor);
        r->stateFlags &= ~RIDER_FLAG_RAGDOLL;
        riderSetState(r, 1, CRASH_RAGDOLL);
        break;

    case CRASH_RAGDOLL:
        dynUntuck(r->animCtl, r->riderActor);
        dynUnbrace(r->animCtl, r->riderActor);
        dynClearPitchInput(r->animCtl, r->riderActor);
        dynClearRollInput(r->animCtl, r->riderActor);
        r->stateFlags &= ~RIDER_FLAG_LIMP;
        if (r->surface == SURFACE_WATER) {
            riderSetState(r, 1, CRASH_SPLASH);
            return 0;
        }
        /* more than 1 m above the ground: the player can tuck (ACT_GO_LIMP, dynTuck curls the body up)
         * and spin the tumble with the stick; ACT_RAGDOLL held makes the hands grab a nearby rail or
         * ledge (dynGrab), and while hanging (dynIsHanging) the state is CRASH_STEER (DM-058) */
        if (bmxphysProbeGround(r->pos, ground) && fabsf(ground[2] - r->pos[1]) > 100.0f) {
            if (act(r, ACT_GO_LIMP, ACT_HELD) && !(r->stateFlags & RIDER_FLAG_RAGDOLL)) {
                dynTuck(r->animCtl, r->riderActor);
                r->stateFlags |= RIDER_FLAG_LIMP;
            }
            s = stick(r);
            if (fabsf(s[1]) > 0.5f)
                dynSetPitchInput(r->animCtl, r->riderActor, (int16_t)(s[1] * -10.0f * DEG_TO_ANGLE));
            s = stick(r);
            if (fabsf(s[0]) > 0.5f)
                dynSetRollInput(r->animCtl, r->riderActor, (int16_t)(s[0] * 10.0f * DEG_TO_ANGLE));
        }
        if (!(r->stateFlags & RIDER_FLAG_LIMP)) {
            if (!act(r, ACT_RAGDOLL, ACT_HELD)) {
                dynRelease(r->animCtl, r->riderActor);
                r->stateFlags &= ~RIDER_FLAG_RAGDOLL;
            } else {
                dynGrab(r->animCtl, r->riderActor);
                r->stateFlags |= RIDER_FLAG_RAGDOLL;
                if (dynIsHanging()) {
                    riderSetState(r, 1, CRASH_STEER);
                    dynClearPitchInput(r->animCtl, r->riderActor);
                    dynClearRollInput(r->animCtl, r->riderActor);
                    return 0;
                }
            }
        }
        if (r->stateTimer[0] > 150) {               /* 5 s */
            riderSetState(r, 1, CRASH_SETTLE);
            return 0;
        }
        goto slow;

    case CRASH_STEER:
        if (!act(r, ACT_RAGDOLL, ACT_HELD) || !dynIsHanging()) {
            dynRelease(r->animCtl, r->riderActor);
            r->stateFlags &= ~RIDER_FLAG_RAGDOLL;
            riderSetState(r, 1, CRASH_RAGDOLL);
            r->stateTimer[0] = 0;
            return 0;
        }
        if (r->stateTimer[0] > 300) {
            riderSetState(r, 1, CRASH_SETTLE);
            return 0;
        }
slow:
        /* nearly at rest for 16 frames */
        if (r->frameDist < 3.0f && ++r->stateTimer[1] > 15) {
            riderSetState(r, 1, CRASH_SETTLE);
            return 0;
        }
        break;

    case CRASH_SETTLE:
        if ((r->stateFlags & (RIDER_FLAG_IDLE | 0x2u)) == 0) {
            dynClearPitchInput(r->animCtl, r->riderActor);
            dynClearRollInput(r->animCtl, r->riderActor);
            r->stateFlags &= ~RIDER_FLAG_LIMP;
            r->motion[0].flags |= MOTION_CUT;
            bmxphysResetRider(r);
            riderSetState(r, 0, RIDER_STATE_RIDING);
            return 0;
        }
        break;

    case CRASH_SPLASH:
        fxWaterSplash(r->riderActor, 0, &g_zmathOrigin);
        bmxsoundEvent(0xFA2, r->pos, 3, 0, 0);
        zactorHide(r->riderActor);
        zactorHide(r->bikeActor);
        riderSetState(r, 1, CRASH_WAIT);
        /* fall through */
    case CRASH_WAIT:
        if (++r->stateTimer[1] == 60) {             /* 2 s under water, then back on the bank */
            zactorShow(r->riderActor);
            zactorShow(r->bikeActor);
            bmxphysResetRider(r);
            riderSetState(r, 0, RIDER_STATE_RIDING);
            return 0;
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ riding sub-states (state[1]) */

/* bike within 50 degrees of level / upright (0x238E = 49.97 degrees) */
static int riderLevel(const BmxRider *r)
{
    return zmathAsin(fabsf(r->matrix[1])) < 0x238E;           /* right.y */
}

static int riderUpright(const BmxRider *r)
{
    return zmathAcos(-r->matrix[5]) < 0x238E;                  /* up.y (Y down) */
}

/* VA 0x004339d0, 654 bytes, [I] row, status draft */
int riderRideCoast(BmxRider *r)
{
    uint32_t s2;

    if (r->frameDist > 0.0f) {
        /* grind button on a ramp lip / rail at speed */
        if (r->surface != 0 && r->surface < 4 && act(r, ACT_GRIND, ACT_HELD) &&
            bmxphysSetState(r, CONTACT_GRIND) == CONTACT_GRIND)
            return 0;
        if (act(r, ACT_JUMP, ACT_HELD)) {
            riderSetState(r, 1, RIDE_CROUCH);
            return 0;
        }
        if (stick(r)[1] > 0.95f && riderUpright(r)) {
            riderSetState(r, 1, RIDE_SKID);
            return 0;
        }
    }
    if (riderLevel(r)) {
        /* pedal button; single player auto-pedals below top speed once started; standing still
         * the start button pedals off */
        if (act(r, ACT_PEDAL, ACT_HELD) ||
            (r->frameDist != 0.0f && r->profileByte2 < 2 && !(r->stateFlags & RIDER_FLAG_CAN_MANUAL) &&
             (r->stateFlags & RIDER_FLAG_PEDALLING) &&
             r->frameDist < g_pedalSpeedPerStat * r->stats[3] - g_autoPedalMargin) ||
            (r->frameDist == 0.0f && act(r, ACT_START, ACT_PRESSED))) {
            riderSetState(r, 1, RIDE_PEDAL);
            r->stateFlags |= RIDER_FLAG_PEDALLING;
            return 0;
        }
    }

    s2 = riderGetState(r, 2);
    if (s2 == 1) {
        if (riderLevel(r)) {                        /* standing: idle clip loops, 30 s -> RIDER_FLAG_IDLE */
            if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
                r->motion[0].flags |= MOTION_RESTART;
            r->wheelDrag = 1.0f;
            if (r->rotVel[1] != 0) {
                r->stateTimer[2] = 0;
                r->stateFlags &= ~RIDER_FLAG_IDLE;
                return 1;
            }
            if (++r->stateTimer[2] >= 901)
                r->stateFlags |= RIDER_FLAG_IDLE;
            return 1;
        }
    } else {
        if (s2 == 2 && !(r->stateFlags & RIDER_FLAG_MOTION_DONE))
            return 1;
        if (s2 != 0 && s2 != 2)
            return 1;
        if (r->frameDist == 0.0f && riderLevel(r)) {
            r->motion[0].id = MOTION_IDLE;
            riderSetState(r, 2, 1);
            r->motion[0].flags |= MOTION_RESTART;
            return 1;
        }
    }
    r->motion[0].id = MOTION_RIDE;
    riderSetState(r, 2, 2);
    r->motion[0].flags |= MOTION_RESTART;
    return 1;
}

/* VA 0x00433d10, 511 bytes, [I] row, status draft */
int riderRidePedal(BmxRider *r)
{
    float rate;

    if (!riderLevel(r)) {
        riderSetState(r, 1, RIDE_COAST);
        return 0;
    }
    if (act(r, ACT_JUMP, ACT_HELD)) {
        uint16_t t = r->stateTimer[1];

        riderSetState(r, 1, RIDE_CROUCH);
        if (r->profileByte2 == 1)                   /* control config 1: pedalling charges the hop */
            r->stateTimer[1] = t;
        return 0;
    }
    if (stick(r)[1] > 0.95f && riderUpright(r)) {
        riderSetState(r, 1, RIDE_SKID);
        return 0;
    }
    switch (riderGetState(r, 2)) {
    case 0:
        requestMotion(r, MOTION_PEDAL);
        riderSetState(r, 2, 1);
        rate = r->frameDist * 0.05f;                /* the crank turns faster with speed */
        r->motion[0].rate = (rate < 1.0f) ? 1.0f : rate;
        break;
    case 1:
        rate = r->frameDist * 0.05f;
        r->motion[0].rate = (rate < 1.0f) ? 1.0f : rate;
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
            if (!act(r, ACT_PEDAL, ACT_HELD) &&
                (r->profileByte2 > 1 || (r->stateFlags & RIDER_FLAG_CAN_MANUAL) ||
                 !(r->stateFlags & RIDER_FLAG_PEDALLING) ||
                 r->frameDist >= g_pedalSpeedPerStat * r->stats[3] - g_autoPedalMargin)) {
                riderSetState(r, 1, RIDE_COAST);
                return 0;
            }
            requestMotion(r, MOTION_PEDAL);
        }
        break;
    }
    if (r->frameDist < g_pedalSpeedPerStat * r->stats[3])
        r->pedalForce = g_pedalForcePerStat * r->stats[3];
    r->stateTimer[1]++;
    return 1;
}

/* VA 0x00433f90, 205 bytes, [I] row, status draft */
int riderRideSkid(BmxRider *r)
{
    if (stick(r)[1] > 0.95f && riderUpright(r)) {
        switch (riderGetState(r, 2)) {
        case 0:
            requestMotion(r, MOTION_SKID);
            r->skidLength = 0.0f;
            riderSetState(r, 2, 1);
            break;
        case 1:
            if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
                requestMotion(r, MOTION_SKID_HOLD);
            r->wheelDrag = g_skidDrag;
            break;
        }
        return 1;
    }
    riderSetState(r, 1, RIDE_COAST);
    return 0;
}

/* VA 0x004340a0, 463 bytes, [I] row, status draft */
int riderRideCrouch(BmxRider *r)
{
    float *s;

    if (act(r, ACT_JUMP, ACT_RELEASED)) {           /* hop: stronger the longer the button was held */
        RwV3d imp;

        zmathVecZero(&imp);
        imp.y = -hopImpulse(r, r->stateTimer[1]);
        if (r->surface == 1 || r->surface == 3) {
            s = stick(r);
            if (s[1] < -0.5f && fabsf(stick(r)[0]) < 0.25f)
                r->stateFlags |= RIDER_FLAG_FORWARD_HOP;
        }
        riderJumpOff(r, &imp, MOTION_HOP);
        return 0;
    }
    switch (riderGetState(r, 2)) {
    case 0:
        requestMotion(r, MOTION_CROUCH);
        riderSetState(r, 2, 1);
        break;
    case 1:
        if (r->frameDist < g_pedalSpeedPerStat * r->stats[3] && riderLevel(r))
            r->pedalForce = g_pedalForcePerStat * 0.5f * r->stats[3];
        /* fall through */
    case 2:
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
            requestMotion(r, MOTION_CROUCH_HOLD);
            riderSetState(r, 2, 2);
        }
        break;
    }
    r->stateTimer[1]++;
    return 1;
}

/* VA 0x004342f0, 735 bytes, [I] row, status draft */
int riderRideTurnaround(BmxRider *r)
{
    /* physics asks for a 180 (RIDER_FLAG_TURNAROUND): clip 17, then 18 (holding the turnaround
     * button: roll out backwards) or a hop; at the end of the clip the bike's axes are flipped */
    if (!(r->stateFlags & RIDER_FLAG_TURNAROUND) && riderGetState(r, 2) != 2) {
        riderSetState(r, 1, RIDE_COAST);
        return 0;
    }
    switch (riderGetState(r, 2)) {
    case 0:
        requestMotion(r, MOTION_TURN);
        riderSetState(r, 2, 1);
        r->stateTimer[2]++;
        return 1;
    case 1:
        break;
    case 2:
        if (!(r->stateFlags & RIDER_FLAG_MOTION_DONE)) {
            r->stateTimer[2]++;
            r->rotVel[1] = 0;
            return 1;
        }
        r->stateFlags &= ~RIDER_FLAG_TURNAROUND;
        riderTurnAround(r);
        riderSetState(r, 1, RIDE_COAST);
        return 0;
    case 3:
        if (act(r, ACT_JUMP, ACT_RELEASED)) {
            RwV3d imp;

            zmathVecZero(&imp);
            imp.y = -hopImpulse(r, r->stateTimer[2]);
            riderJumpOff(r, &imp, MOTION_HOP);
            return 0;
        }
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
            r->motion[0].flags |= MOTION_RESTART;
        /* fall through */
    default:
        r->stateTimer[2]++;
        return 1;
    }
    if (act(r, ACT_TURNAROUND, ACT_PRESSED)) {      /* DM-051: statuses as the exe passes them (9,0 / 6,1) */
        requestMotion(r, MOTION_TURN_FAKIE);
        riderSetState(r, 2, 2);
    } else if (act(r, ACT_JUMP, ACT_HELD)) {
        riderSetState(r, 2, 3);
        return 0;
    }
    if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
        r->motion[0].flags |= MOTION_RESTART;
    r->stateTimer[2]++;
    r->wheelDrag = (stick(r)[1] > 0.95f && riderUpright(r)) ? g_skidDrag : 0.01f;
    return 1;
}

/* VA 0x004346a0, 6 bytes, [I] row, status draft */
int riderRideNull(BmxRider *r)
{
    /* riding sub-state 5: nothing sets it */
    (void)r;
    return 1;
}

/* ------------------------------------------------------------------ air sub-states */

/* VA 0x004346c0, 121 bytes, [I] row, status draft */
int riderAirIdle(BmxRider *r)
{
    switch (riderGetState(r, 2)) {
    case 0:                                         /* entering the air / a trick ended */
        r->spin = 0;
        r->modCount = 0;
        r->motion[0].flags |= MOTION_RESTART;
        r->trickId = TRICK_NONE;
        r->modId = TRICK_NONE;
        r->stateFlags &= ~RIDER_FLAG_FLIP;
        riderSetState(r, 2, 1);
        break;
    case 1:
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
            requestMotion(r, MOTION_AIR_IDLE);
        break;
    }
    return 1;
}

/* the modifier for the buffered directions (0xFF none): last two, else the last one */
static uint8_t riderModifierSlot(const BmxRider *r)
{
    uint8_t m = 0xFF;

    if (r->dirCount >= 2)
        m = g_modDir2[prevDir(r) - 1][lastDir(r) - 1];
    if (r->dirCount < 2 || m == 0xFF)
        m = g_modDir1[r->dirCount ? lastDir(r) : 0];
    return m;
}

/* VA 0x00434760, 1907 bytes, [I] row, status draft */
int riderAirTrick(BmxRider *r)
{
    uint8_t n, t;
    uint32_t i, k, target;
    int16_t accel, lim;
    int v;
    float b;

    if (act(r, ACT_TRICK, ACT_RELEASED))
        r->stateFlags &= ~RIDER_FLAG_HOLD;
    if (r->modId != TRICK_NONE && act(r, ACT_MODIFIER, ACT_RELEASED))
        r->stateFlags &= ~RIDER_FLAG_HOLD;
    if (r->stateFlags & RIDER_FLAG_FLIP) {
        for (i = 0; i < 3; i++)
            r->flipVel[i] = (int16_t)(r->flipVel[i] - (int16_t)(r->flipVel[i] * ANGLE_TO_DEG * 0.15f * DEG_TO_ANGLE));
    }

    switch (riderGetState(r, 2)) {
    case 0:
        /* pick the trick: signature combo, then the last two directions, then the last one */
        n = r->dirCount;
        r->trickId = TRICK_NONE;
        if (n != 0) {
            const BmxSigCombo *c = g_signatureTricks[g_saveProfiles[r->slot].rider];

            for (i = 0; i < 4; i++, c++) {
                if (c->length == 0 || c->length > n)
                    continue;
                for (k = 0; k < c->length; k++)
                    if (r->dirBuf[n - c->length + k] != c->dir[k])
                        break;
                if (k == c->length) {
                    r->trickId = c->trick;
                    r->modId = c->mod;
                    break;
                }
            }
            if (n > 1 && r->trickId == TRICK_NONE)
                r->trickId = g_airTrickDir2[prevDir(r) - 1][lastDir(r) - 1];
            if (r->trickId == TRICK_NONE)
                r->trickId = g_airTrickDir1[lastDir(r)];
        }
        if (r->trickId == TRICK_NONE) {
            riderResetDirections(r, 1);
            r->motion[0].id = MOTION_AIR_IDLE;
            riderSetState(r, 1, AIR_IDLE);
        } else {
            t = r->trickId;
            r->motion[0].id = g_trickMotion[t];
            r->motion[0].flags = (r->motion[0].flags & ~MOTION_MIRROR) | MOTION_RESTART;
            zmathMatrixCopy((RwMatrix *)r->trickMatrix, (const RwMatrix *)r->matrix);
            r->flipVel[0] = r->flipVel[1] = r->flipVel[2] = 0;
            if (g_trickFlags[t] & 7) {
                uint32_t old = r->stateFlags;

                r->stateFlags |= RIDER_FLAG_FLIP;
                if (old & RIDER_FLAG_CARRY_SPIN) {          /* a flip ends the carried spin */
                    r->physTimer = 0;
                    r->stateFlags &= ~RIDER_FLAG_CARRY_SPIN;
                }
            }
            if (act(r, ACT_TRICK, ACT_HELD))
                r->stateFlags |= RIDER_FLAG_HOLD;
            else
                r->stateFlags &= ~RIDER_FLAG_HOLD;
            riderResetDirections(r, 1);
            riderSetState(r, 2, 1);
            r->motion[0].rate = r->stats[6];
            if (r->modId != TRICK_NONE) {                   /* signature combo with a modifier */
                dynIkReset(r->riderActor, &r->ik);
                r->modBlend = 0.0f;
                r->stateFlags |= RIDER_FLAG_MODIFIER;
            }
        }
        goto done;

    case 1:
    case 2:
        /* modifier on top of the trick, filtered by g_trickModTable */
        if (act(r, ACT_MODIFIER, ACT_PRESSED) ||
            ((r->stateFlags & RIDER_FLAG_MOD_ARMED) && r->dirCount != 0)) {
            if (r->dirCount == 0) {
                r->stateFlags |= RIDER_FLAG_MOD_ARMED;
            } else {
                uint8_t m = g_trickModTable[r->trickId][riderModifierSlot(r)];

                if (m != TRICK_NONE) {
                    r->modId = m;
                    riderResetDirections(r, 1);
                    dynIkReset(r->riderActor, &r->ik);
                    r->stateFlags |= RIDER_FLAG_MODIFIER;
                    r->modBlend = 0.0f;
                    if (act(r, ACT_MODIFIER, ACT_HELD))
                        r->stateFlags |= RIDER_FLAG_HOLD;
                    else
                        r->stateFlags &= ~RIDER_FLAG_HOLD;
                }
            }
        }
        t = r->trickId;
        if ((g_trickFlags[t] & 7) == 0) {
            /* no flip: start clip, hold clip while the button is held (bit 3: exactly once), release */
            switch (riderGetState(r, 2)) {
            case 1:
                if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
                    if (!(r->stateFlags & RIDER_FLAG_HOLD) && !(g_trickFlags[t] & 8)) {
                        riderSetState(r, 2, 3);
                        return 0;
                    }
                    r->motion[0].flags |= MOTION_RESTART;
                    r->motion[0].id = (uint16_t)(g_trickMotion[t] + 1);
                    riderSetState(r, 2, 2);
                    r->motion[0].rate = r->stats[6];
                }
                break;
            case 2:
                if (r->modId == 0 && r->modBlend != 0.0f && (r->stateTimer[1] & 0xF) == 0)
                    r->modCount++;                  /* barspin: one more every 16 frames */
                if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
                    if (g_trickFlags[t] & 8)
                        r->stateFlags &= ~RIDER_FLAG_HOLD;
                    if (!(r->stateFlags & RIDER_FLAG_HOLD)) {
                        riderSetState(r, 2, 3);
                        return 0;
                    }
                    r->motion[0].flags |= MOTION_RESTART;
                }
                break;
            }
            goto blend;
        }
        /* flips: rotate flip (binary angle) until 300 / 660 / 1020 degrees back or 330 / 690 front */
        switch (g_trickFlags[t] & 7) {
        case 2:
            target = 0x1D555;
            break;
        case 3:
            target = 0x2D555;
            break;
        case 4:
            target = 0xEAAA;
            break;
        case 5:
            target = 0x1EAAA;
            break;
        default:
            target = 0xD555;
            break;
        }
        if ((g_trickFlags[t] & 7) >= 4) {           /* front flips: the other way, 3/4 as fast */
            accel = (int16_t)(-(g_flipAccelPerStat * 0.75f * r->stats[2]) * DEG_TO_ANGLE);
            lim = (int16_t)(g_flipMaxPerStat * 0.75f * r->stats[2] * DEG_TO_ANGLE);
        } else {
            accel = (int16_t)(g_flipAccelPerStat * r->stats[2] * DEG_TO_ANGLE);
            lim = (int16_t)(g_flipMaxPerStat * r->stats[2] * DEG_TO_ANGLE);
        }
        v = (int16_t)(accel + r->flipVel[2]);
        if ((uint32_t)(r->flip < 0 ? -r->flip : r->flip) >= target) {
            riderSetState(r, 2, 3);
            return 0;
        }
        if (v > lim)
            v = lim;
        if (v < -lim)
            v = -lim;
        r->flipVel[2] = (int16_t)v;
        r->flip += r->flipVel[2];
        switch (riderGetState(r, 2)) {
        case 1:
            if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
                r->motion[0].flags |= MOTION_RESTART;
                r->motion[0].id = (uint16_t)(g_trickMotion[t] + 1);
                riderSetState(r, 2, 2);
            }
            break;
        case 2:
            if (r->modId == 0 && r->modBlend != 0.0f && (r->stateTimer[1] & 0xF) == 0)
                r->modCount++;
            break;
        }
        goto blend;

    case 3:                                         /* release clip */
        r->motion[0].id = (uint16_t)(g_trickMotion[r->trickId] + 2);
        riderSetState(r, 2, 4);
        r->motion[0].flags |= MOTION_RESTART;
        goto blend;

    case 4:
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
            riderSetState(r, 1, AIR_IDLE);
            r->motion[0].id = MOTION_AIR;
            return 0;
        }
        if (!(r->stateFlags & RIDER_FLAG_MODIFIER))
            goto done;
        b = r->modBlend - 0.3f;                     /* fade the modifier out */
        r->modBlend = (b < 0.0f) ? 0.0f : b;
        goto done;

    default:
        goto done;
    }

blend:
    if (r->stateFlags & RIDER_FLAG_MODIFIER) {      /* fade the modifier in */
        b = r->modBlend + 0.2f;
        r->modBlend = (b > 1.0f) ? 1.0f : b;
    }
done:
    if (r->modBlend != 0.0f)
        r->stateTimer[1]++;
    return 1;
}

/* VA 0x004350f0, 523 bytes, [I] row, status draft */
int riderAirModifier(BmxRider *r)
{
    /* a modifier on its own (no air trick): g_modMotion start / hold / release */
    uint8_t m;

    if (act(r, ACT_MODIFIER, ACT_RELEASED))
        r->stateFlags &= ~RIDER_FLAG_HOLD;
    switch (riderGetState(r, 2)) {
    case 0:
        m = riderModifierSlot(r);
        r->modId = m;
        r->motion[0].id = g_modMotion[m];
        r->motion[0].flags = (r->motion[0].flags & ~MOTION_MIRROR) | MOTION_RESTART;
        r->modCount = 0;
        if (act(r, ACT_MODIFIER, ACT_HELD))
            r->stateFlags |= RIDER_FLAG_HOLD;
        else
            r->stateFlags &= ~RIDER_FLAG_HOLD;
        riderResetDirections(r, 1);
        riderSetState(r, 2, 1);
        r->motion[0].rate = r->stats[6];
        break;
    case 1:
        if (!(r->stateFlags & RIDER_FLAG_MOTION_DONE))
            return 1;
        r->modCount++;
        if (!(r->stateFlags & RIDER_FLAG_HOLD)) {
            r->motion[0].id = (uint16_t)(g_modMotion[r->modId] + 2);
            r->motion[0].flags |= MOTION_RESTART;
            riderSetState(r, 2, 3);
            return 1;
        }
        r->motion[0].flags |= MOTION_RESTART;
        r->motion[0].id = (uint16_t)(g_modMotion[r->modId] + 1);
        riderSetState(r, 2, 2);
        r->motion[0].rate = r->stats[6];
        break;
    case 2:
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
            if (g_modFlags[r->modId] & 8)
                r->modCount++;
            if (r->stateFlags & RIDER_FLAG_HOLD) {
                r->motion[0].flags |= MOTION_RESTART;
                return 1;
            }
            r->motion[0].id = (uint16_t)(g_modMotion[r->modId] + 2);
            r->motion[0].flags |= MOTION_RESTART;
            riderSetState(r, 2, 3);
            return 1;
        }
        break;
    case 3:
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE) {
            riderSetState(r, 1, AIR_IDLE);
            r->motion[0].id = MOTION_AIR;
            return 0;
        }
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ grind / stall / manual */

/* clips of the grind / stall: enter, then hold blended with itself (motion[1]) */
static void riderGrindEnter(BmxRider *r)
{
    r->motion[0].id = GRIND_MOTION(r->trickId);
    r->motion[0].flags |= MOTION_CUT | MOTION_RESTART;
    if (r->stateFlags & RIDER_FLAG_MIRROR)
        r->motion[0].flags |= MOTION_MIRROR;
    r->motion[0].rate = 8.0f / (float)r->physTimer;   /* the enter clip lasts the lock-on */
    riderResetDirections(r, 0);
    riderSetState(r, 2, 1);
}

static void riderGrindHold(BmxRider *r)
{
    uint16_t id = (uint16_t)(GRIND_MOTION(r->trickId) + 1);

    r->motion[0].id = id;
    r->motion[1].id = id;
    r->motion[1].rate = 1.0f;
    r->motion[0].flags |= MOTION_RESTART;
    if (r->stateFlags & RIDER_FLAG_MIRROR)
        r->motion[0].flags |= MOTION_MIRROR;
    r->motion[1].flags = r->motion[0].flags;
    r->motion[0].rate = 1.0f;
    gameCheckTransfer(r->slot, 1);
    riderSetState(r, 2, 3);
}

static void riderBalance(BmxRider *r, float input)
{
    int16_t d = (int16_t)(input * g_balanceRate * DEG_TO_ANGLE);

    if (zmathVecDot((const RwV3d *)r->vel, (const RwV3d *)&r->matrix[0]) < 0.0f)
        r->rotVel[0] = (int16_t)(r->rotVel[0] + d);
    else
        r->rotVel[0] = (int16_t)(r->rotVel[0] - d);
    r->motion[1].rate = 1.0f;
    if (r->motion[0].frame == 0 || r->motion[1].frame == 0) {
        r->motion[0].flags |= MOTION_CUT | MOTION_RESTART;
        r->motion[1].flags |= MOTION_CUT | MOTION_RESTART;
    }
}

/* VA 0x004353a0, 776 bytes, [I] row, status draft */
int riderGrind(BmxRider *r)
{
    switch (riderGetState(r, 2)) {
    case 0:
        r->trickId = riderGrindTrick(r);
        riderGrindEnter(r);
        r->stateTimer[2]++;
        return 1;
    case 1:
        if (bmxphysGetState(r) == CONTACT_GRIND) {
            riderSetState(r, 2, 2);
            return 0;
        }
        break;
    case 2:
        riderGrindHold(r);
        r->stateTimer[2]++;
        return 1;
    case 3:
    case 4:
        if (riderGetState(r, 2) == 3) {
            if (act(r, ACT_JUMP, ACT_HELD)) {
                riderSetState(r, 2, 4);
                return 0;
            }
        } else if (act(r, ACT_JUMP, ACT_RELEASED)) {   /* ollie off the rail along the bike's up axis */
            float h = hopImpulse(r, r->stateTimer[2]);
            RwV3d imp;

            imp.x = h * r->matrix[4];
            imp.y = h * r->matrix[5];
            imp.z = h * r->matrix[6];
            bmxphysAddVelocity(r, &imp);
            bmxphysSetState(r, CONTACT_AIR);
            riderSetState(r, 0, RIDER_STATE_AIR);
            r->motion[0].id = GRIND_EXIT_MOTION(r->trickId);
            r->stateFlags |= RIDER_FLAG_LEFT_LIP;
            return 0;
        }
        riderBalance(r, stick(r)[0]);
        break;
    }
    r->stateTimer[2]++;
    return 1;
}

/* VA 0x00435790, 659 bytes, [I] row, status draft */
int riderStall(BmxRider *r)
{
    if (act(r, ACT_PLANT, ACT_RELEASED) || act(r, ACT_JUMP, ACT_RELEASED))
        r->stateFlags &= ~RIDER_FLAG_HOLD;
    switch (riderGetState(r, 2)) {
    case 0:
        riderGrindEnter(r);
        r->stateFlags |= RIDER_FLAG_HOLD;
        return 1;
    case 1:
        if (bmxphysGetState(r) == CONTACT_STALL) {
            riderSetState(r, 2, 2);
            return 0;
        }
        break;
    case 2:
        riderGrindHold(r);
        return 1;
    case 3:
    case 4:
        if (riderGetState(r, 2) == 3) {
            if (++r->stateTimer[2] == 5)            /* at least 5 frames */
                riderSetState(r, 2, 4);
        } else if (!(r->stateFlags & RIDER_FLAG_HOLD)) {   /* button let go: drop off */
            bmxphysSetState(r, CONTACT_AIR);
            riderSetState(r, 0, RIDER_STATE_AIR);
            r->motion[0].id = GRIND_EXIT_MOTION(r->trickId);
            r->stateFlags |= RIDER_FLAG_LEFT_LIP;
            return 0;
        }
        /* nose / tail stalls (nosepick, tail tap, disaster) balance with the stick's y */
        if (r->trickId >= 0x27 && r->trickId <= 0x29)
            riderBalance(r, -stick(r)[1]);
        else
            riderBalance(r, stick(r)[0]);
        break;
    }
    return 1;
}

/* VA 0x00435af0, 540 bytes, [I] row, status draft */
int riderManual(BmxRider *r)
{
    uint32_t s2, next;

    if (riderGetState(r, 2) < 2)
        r->rotVel[2] = (int16_t)(stick(r)[1] * 2.5f * DEG_TO_ANGLE);    /* balance with the stick's y */
    if (riderGetState(r, 2) < 3) {
        /* the first wheel on the ground decides (DM-048: wheel.contact 2 = in the air): almost
         * stopped (speed < 1) ends the manual */
        if (r->wheel[0].contact != 2) {
            if (r->wheel[0].speed < 1.0f)
                riderSetState(r, 2, 3);
        } else if (r->wheel[1].contact != 2) {
            if (r->wheel[1].speed < 1.0f)
                riderSetState(r, 2, 3);
        }
    }
    s2 = riderGetState(r, 2);
    switch (s2) {
    case 0:
        r->motion[0].id = (uint16_t)((r->motion[0].id == MOTION_HOP) + MOTION_MANUAL_IN);
        r->motion[0].flags |= MOTION_RESTART;
        next = 1;
        break;
    case 1:
        if (act(r, ACT_JUMP, ACT_HELD))
            riderSetState(r, 2, 2);
        /* fall through */
    case 2:
        if (act(r, ACT_JUMP, ACT_RELEASED)) {
            RwV3d imp;

            zmathVecZero(&imp);
            imp.y = -hopImpulse(r, r->stateTimer[2]);
            riderJumpOff(r, &imp, MOTION_HOP);
            return 0;
        }
        if (r->stateFlags & RIDER_FLAG_MOTION_DONE)
            requestMotion(r, MOTION_MANUAL);
        r->trickId = (uint8_t)((r->wheel[0].contact != 2) + TRICK_MANUAL);
        goto out;
    case 3:
        requestMotion(r, MOTION_MANUAL_OUT);
        r->stateFlags &= ~(RIDER_FLAG_MANUAL | RIDER_FLAG_NOSE_MANUAL);
        next = 4;
        break;
    default:
        goto out;
    }
    riderSetState(r, 2, next);
out:
    r->stateTimer[1]++;
    r->stateTimer[2]++;
    r->wheelDrag = 0.002f;
    return 1;
}

/* ------------------------------------------------------------------ direction helpers */

/* VA 0x00435db0, 184 bytes, [I] row, status draft */
void riderResetDirections(BmxRider *r, int record)
{
    uint32_t m;

    *(uint32_t *)r->dirBuf = 0;
    r->dirCount = 0;
    if (!record) {
        r->stateFlags &= ~RIDER_FLAG_RECORD_DIRS;
        r->stateFlags &= ~(RIDER_FLAG_RECORD_DIRS | RIDER_FLAG_TRICK_ARMED | RIDER_FLAG_MOD_ARMED);
        return;
    }
    if (!(r->stateFlags & RIDER_FLAG_RECORD_DIRS)) {
        /* starting to record with the stick already pushed: that direction counts */
        m = dirMask(r->input.buttons, r->input.stick[0][0], r->input.stick[0][1]);
        if (m != 0) {
            r->dirBuf[0] = g_dirFromMask[m];
            r->dirCount++;
        }
    }
    r->stateFlags |= RIDER_FLAG_RECORD_DIRS;
    r->stateFlags &= ~(RIDER_FLAG_TRICK_ARMED | RIDER_FLAG_MOD_ARMED);
}

/* VA 0x00435ea0, 51 bytes, [I] row, status draft */
uint8_t riderGrindTrick(const BmxRider *r)
{
    if (r->dirCount == 0)
        return g_grindTrickDir[riderCurrentDirection(r)];
    return g_grindTrickDir[lastDir(r)];
}

/* VA 0x00435ef0, 51 bytes, [I] row, status draft */
uint8_t riderStallTrick(const BmxRider *r)
{
    if (r->dirCount == 0)
        return g_stallTrickDir[riderCurrentDirection(r)];
    return g_stallTrickDir[lastDir(r)];
}

/* VA 0x00435f40, 51 bytes, [I] row, status draft */
uint8_t riderPlantTrick(const BmxRider *r)
{
    if (r->dirCount == 0)
        return g_plantTrickDir[riderCurrentDirection(r)];
    return g_plantTrickDir[lastDir(r)];
}

/* VA 0x00435f90, 111 bytes, [I] row, status draft */
uint8_t riderCurrentDirection(const BmxRider *r)
{
    return g_dirFromMask[dirMask(r->input.buttons, r->input.stick[0][0], r->input.stick[0][1])];
}

/* ------------------------------------------------------------------ dynamics callbacks */

/* VA 0x00436020, 854 bytes, [V] direct, status draft */
void riderOnRiderImpact(void *dyn, int part, float force, uint32_t *contact)
{
    /* a body part of the rag doll hit something: surface, sound, rumble, sparks, crash damage.
     * contact: +0 flags (1 BSP tag, 2 object), +8 position, +0x1C normal, +0x38 tag, +0x3C shape,
     * +0x40 object */
    BmxRider *r = 0;
    uint32_t p, n, kind = 0, level;
    int hard = 0, rumble;
    RwV3d v;

    if (force <= 0.0f)
        return;
    n = g_numPlayers ? g_numPlayers : 1;
    for (p = 0; p < n; p++) {
        if (g_riders[p].animCtl == dyn) {
            r = &g_riders[p];
            break;
        }
    }
    if (r == 0)
        zAssertFail("Couldn't associate rider with dynamic collision structure", RIDER_SRC, 0xa85, 0);

    if (contact[0] & 1) {
        if (contact[14] != 0)
            kind = *(const uint32_t *)contact[14] & 0xF;
    } else if (contact[0] & 2) {
        const uint8_t *obj = zbspGetPropShape((int16_t)contact[15]);
        const uint8_t *mover;

        if (zcolGetPropShape((int16_t)contact[15]) != 0)
            kind = obj[2];
        mover = *(const uint8_t **)((const uint8_t *)contact[16] + 0x1EC);
        if (mover != 0) {                           /* moving object: the part takes its velocity */
            RwV3d *pv = (RwV3d *)((uint8_t *)dyn + part * 0xB4 + 0x88);

            v.x = *(const float *)&contact[7];
            v.y = *(const float *)&contact[8];
            v.z = *(const float *)&contact[9];
            (void)zmathVecDot(pv, &v);              /* results unused in the exe */
            (void)zmathVecDot((const RwV3d *)(mover + 0x1C), &v);
            zmathVecAdd(pv, pv, (const RwV3d *)(mover + 0x1C));
        }
    }
    switch (kind) {
    case 5:
    case 9:
    case 10:
        hard = 1;                                   /* sparks */
        break;
    case 6:                                         /* water: the hips (part 0) decide the surface */
        if (part == 0)
            r->surface = (((*(const uint32_t *)contact[14] >> 19) & 3) == 2) ? 14 : 13;
        break;
    }

    if (force > 5.0f) {
        level = (force < 10.0f) ? 0 : (force < 25.0f) ? 1 : (force < 50.0f) ? 2 : 3;
        switch (part) {
        case 1:
        case 2:
            bmxsoundEvent(0xBBD, &contact[2], 1, level, 0);
            if (r->impactTimer == 0 && level != 0) {
                bmxsoundEvent(0xBBE, &contact[2], 1, level, 0);
                r->impactTimer = 45;
            }
            break;
        case 5:
        case 6:
            bmxsoundEvent(0xBBD, &contact[2], 0, level, 0);
            if (r->impactTimer == 0 && level > 1) {
                bmxsoundEvent(0xBBE, &contact[2], 0, level, 0);
                r->impactTimer = 45;
            }
            break;
        case 11:
        case 12:
            bmxsoundEvent(0xBBD, &contact[2], 2, level, 0);
            if (r->impactTimer == 0 && level > 1) {
                bmxsoundEvent(0xBBE, &contact[2], 2, level, 0);
                r->impactTimer = 45;
            }
            break;
        case 13:
            bmxsoundEvent(0xBBD, &contact[2], 3, level, 0);
            if (r->impactTimer == 0 && level != 0) {
                bmxsoundEvent(0xBBE, &contact[2], 3, level, 0);
                r->impactTimer = 45;
            }
            break;
        }
        rumble = (int)(force * 8.0f);
        contrlerRumble(r->slot, 1, rumble > 254 ? 255 : rumble);
        if (force > 20.0f) {
            (void)zactorGetPos(r->riderActor);
            zmathVecZero(&v);
            if (hard)
                fxDust((const RwV3d *)&contact[2], &v);
            if (force > 30.0f)
                fxCollectEffect();
        }
    }
    riderAddCrashDamage(r, part, force);
}

/* VA 0x004364b0, 77 bytes, [I] row, status draft */
void riderOnBikeImpact(void *dyn, int part, float force, uint32_t *contact)
{
    int v;

    (void)dyn;
    if (part == 0 && force > 10.0f) {
        v = (int)(force * 6.0f);
        bmxsoundEvent(0xBBC, &contact[2], 0, v > 126 ? 127 : v, 0);
    }
}

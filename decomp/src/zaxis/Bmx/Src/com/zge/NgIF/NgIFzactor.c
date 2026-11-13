/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFzactor.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFzactor.c
 * Evidence: [V] path string 0x0055CA78, printed by zactorGetAbsolutePartPosition (line 0xB5),
 *           zactorGetAbsolutePartPositionNoPose (0xF4), zactorGetPartLength (0x11B),
 *           zactorGetPartOffset (0x153) and NgIFzactor_ApplyMotionPose (0x1FE); [I] the rest by
 *           the shared part tables 0x005DA1E8..0x005DA2DC and link order.
 * 12 functions, VA 0x004646f0-0x00465a23 (DM-057 created zactorGetPartFrame 0x00464FB0 and
 * zactorGetPartOffset 0x00465040 (unreferenced) and took NgIFzactor_ResetPartTables 0x004646F0 from
 * the NgIFpsx gap: it clears this file's tables).
 * DM-057 (draft). Non-matching C (AD-8). Types: zactor.h, prototypes: ngif.h. Description:
 * docs/re/ngif.md, the arm table and channel lists: docs/formats/ANIM.md section 3.
 *
 * The bridge between the game's actors (zactor.c) and their RenderWare models: which model frame
 * each motion channel drives (15 rider channels, 10 bike channels, found by zHashName of the
 * frame name), the per-frame pose (Euler angles -> quaternions, with the arm corrections that turn
 * the motion data's arm axis into the model's), and bone positions for physics / IK.
 */
#include "decomp.h"
#include "zmath.h"
#include "zanim.h"
#include "ngif.h"

#define NGIFZACTOR_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFzactor.c"

/* ------------------------------------------------------------------ other modules */
uint32_t   cameraGetPlayer(void);                                         /* camera.c */
ZanimNode *zanimFindNode(ZanimNodeTable *table, uint32_t hash);           /* zanim.c */
void       zanimNodeSetQuat(ZanimNode *node, const Zquat *q);
void       zrwModelUpdateObj(void *model);                                /* 0x004892A0 */
void      *zrwModelFindFrame(void *model, uint32_t hash);                 /* 0x00489110 */
void       zrwModelFrameToWorld(void *model, void *frame, const float *local, float *out, int update); /* 0x00489150 */
void       zrwModelFrameOffset(void *model, void *frame, RwV3d *out, int update);  /* 0x00489180 */
void       zobjSetPosition(void *obj, const float *pos);                    /* set position */
void       zobjSetRotationEuler(void *obj, const int16_t *euler);                /* set rotation */

/* ------------------------------------------------------------------ the channel lists [V] */
#define RIDER_PARTS 15
#define BIKE_PARTS  10

static const char *const s_riderParts[RIDER_PARTS] = {   /* 0x0055C9EC .. 0x0055C934 */
    "Hips", "Chest", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm",
    "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot"
};
static const uint8_t s_riderMirror[RIDER_PARTS] = { 0, 1, 2, 6, 7, 8, 3, 4, 5, 12, 13, 14, 9, 10, 11 };

static const char *const s_bikeParts[BIKE_PARTS] = {     /* 0x0055CA6C .. 0x0055C9F4 */
    "Hipsdeck", "Handlebars", "Sprocket", "RightPedal", "LeftPedal", "BackWheel", "FrontWheel",
    "LeftGrip", "RightGrip", "SeatGrip"
};
static const uint8_t s_bikeMirror[BIKE_PARTS] = { 0, 1, 2, 4, 3, 5, 6, 7, 8, 9 };

/* The arm table (ANIM.md 3.1) [V: the immediates of ApplyMotionPose's stack table]: four Euler
 * triples (x, y, z, pad) and, per arm channel, the rotation applied before (pre) and after (post)
 * the channel's own: q' = pre * q * post. */
static const int16_t s_armEuler[4][4] = {
    { 0, 0,  0x4000, 0 },      /* Cz(+90) */
    { 0, 0, -0x4000, 0 },      /* Cz(-90) */
    { 0, 0, -0x4000, 0 },      /* Cz(-90) */
    { 0, 0,  0x4000, 0 }       /* Cz(+90) */
};
static const struct { const char *channel; int8_t pre, post; } s_armFix[6] = {
    { "LeftUpArm",   -1, 0 },  /* q Cz(+90) (pre: identity) */
    { "LeftLowArm",   2, 0 },  /* Cz(-90) q Cz(+90) */
    { "LeftHand",     2, 0 },
    { "RightUpArm",  -1, 1 },  /* q Cz(-90) */
    { "RightLowArm",  3, 1 },  /* Cz(+90) q Cz(-90) */
    { "RightHand",    3, 1 },
};

/* ------------------------------------------------------------------ globals (this file) */
static uint32_t s_bikePartsCached[2];          /* 0x005DA1E8 per player */
static void    *s_bikePartFrames[2][BIKE_PARTS];  /* 0x005DA1F0 */
static uint32_t s_riderPartsCached[2];         /* 0x005DA240 */
static void    *s_riderPartFrames[2][RIDER_PARTS]; /* 0x005DA248 */
static uint32_t s_armHash[6];                  /* 0x005DA2C8 zHashName of the s_armFix channels */

/* the model frames of an actor's parts for that player (looked up once) */
static void **partFrames(ZActor *a, uint32_t player)
{
    if (a->skeleton == 0) {
        if (s_riderPartsCached[player] == 0)
            NgIFzactor_CacheRiderParts(a, player);
        return s_riderPartFrames[player];
    }
    if (s_bikePartsCached[player] == 0)
        NgIFzactor_CacheBikeParts(a, player);
    return s_bikePartFrames[player];
}

/* the rider's arm bones lie along x: a local offset along the arm is rotated a quarter turn
 * (left arm parts 3..5: (y, -x), right arm parts 6..8: (-y, x)) */
static void partLocal(const ZActor *a, uint32_t part, const float *in, float *out)
{
    out[0] = in[0];
    out[1] = in[1];
    if (a->skeleton == 0) {
        if (part >= 3 && part <= 5) {
            out[0] = in[1];
            out[1] = -in[0];
        } else if (part >= 6 && part <= 8) {
            out[1] = in[0];
            out[0] = -in[1];
        }
    }
    out[2] = in[2];
}

/* ------------------------------------------------------------------ functions */

/* VA 0x004646f0, 23 bytes, [I] callers, status draft */
void NgIFzactor_ResetPartTables(void)
{
    /* zrollerCreateRider: a new model, so the part frames are looked up again */
    int i;

    for (i = 0; i < 2; i++) {
        s_bikePartsCached[i] = 0;
        s_riderPartsCached[i] = 0;
    }
}

/* VA 0x00464740, 477 bytes, [I] strings, status draft */
void NgIFzactor_CacheRiderParts(ZActor *a, uint32_t player)
{
    int i;

    s_riderPartsCached[player] = 1;
    for (i = 0; i < RIDER_PARTS; i++)
        s_riderPartFrames[player][i] = zrwModelFindFrame(a->model, zHashName(s_riderParts[i]));
}

/* VA 0x004649a0, 326 bytes, [I] strings, status draft */
void NgIFzactor_CacheBikeParts(ZActor *a, uint32_t player)
{
    int i;

    s_bikePartsCached[player] = 1;
    for (i = 0; i < BIKE_PARTS; i++)
        s_bikePartFrames[player][i] = zrwModelFindFrame(a->model, zHashName(s_bikeParts[i]));
}

/* VA 0x00464b50, 313 bytes, [V] direct, status draft */
void zactorGetAbsolutePartPosition(ZActor *a, uint16_t part, const float *local, float *out)
{
    /* world position of `local` in the frame of part `part` (after posing the model this frame);
     * physics.c (limb targets, IK), dyn.c dynIkSolve */
    uint32_t player = cameraGetPlayer();
    float l[3];
    void **frames;

    NgIFzactor_ApplyMotionPose(a);
    zrwModelUpdateObj(a->model);
    partLocal(a, part, local, l);
    frames = partFrames(a, player);
    if (frames[part] == NULL) {
        NGIF_MISSING(NGIFZACTOR_SRC, 0xB5, "testzactorGetAbsolutePartPosition: Unknown part");
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    zrwModelFrameToWorld(a->model, frames[part], l, out, 1);
}

/* VA 0x00464d10, 292 bytes, [V] direct, status draft */
void zactorGetAbsolutePartPositionNoPose(ZActor *a, uint16_t part, const float *local, float *out)
{
    /* as zactorGetAbsolutePartPosition with the model's current pose */
    uint32_t player = cameraGetPlayer();
    float l[3];
    void **frames;

    partLocal(a, part, local, l);
    frames = partFrames(a, player);
    if (frames[part] == NULL) {
        NGIF_MISSING(NGIFZACTOR_SRC, 0xF4, "testzactorGetAbsolutePartPosition: Unknown part");
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    zrwModelFrameToWorld(a->model, frames[part], l, out, 1);
}

/* VA 0x00464ea0, 204 bytes, [V] direct, status draft */
float zactorGetPartLength(ZActor *a, uint32_t part)
{
    /* the length of the part's offset from its parent frame (IK bone lengths) */
    uint32_t player = cameraGetPlayer();
    void **frames = partFrames(a, player);
    RwV3d off;

    if (frames[part & 0xFFFF] == NULL) {
        NGIF_MISSING(NGIFZACTOR_SRC, 0x11B, "testzactorGetPartLength: Unknown part");
        off.x = off.y = off.z = 0.0f;
    } else {
        zrwModelFrameOffset(a->model, frames[part & 0xFFFF], &off, 1);
    }
    return zmathVecLength(&off);
}

/* VA 0x00464fb0, 111 bytes, [I] between, status draft */
void *zactorGetPartFrame(ZActor *a, uint32_t part, uint32_t player)
{
    /* unreferenced */
    return partFrames(a, player)[part & 0xFFFF];
}

/* VA 0x00465040, 177 bytes, [I] between, status draft */
void zactorGetPartOffset(ZActor *a, uint32_t part, RwV3d *out, uint32_t player)
{
    /* unreferenced: the part's offset from its parent frame */
    void **frames = partFrames(a, player);

    if (frames[part] == NULL) {
        NGIF_MISSING(NGIFZACTOR_SRC, 0x153, "testzactorGetPartLength: Unknown part");
        out->x = out->y = out->z = 0.0f;
        return;
    }
    zrwModelFrameOffset(a->model, frames[part], out, 1);
}

/* VA 0x00465130, 410 bytes, [I] row, status draft */
void NgIFzactor_SetRiderChannels(ZActor *a)
{
    /* zrollerCreateRider: the rider's 15 motion channels and their mirror partners */
    int i;

    for (i = 0; i < RIDER_PARTS; i++) {
        a->channelHash[i] = zHashName(s_riderParts[i]);
        a->channelMirror[i] = s_riderMirror[i];
    }
    a->numChannels = RIDER_PARTS;
}

/* VA 0x00465340, 280 bytes, [I] row, status draft */
void NgIFzactor_SetBikeChannels(ZActor *a)
{
    /* zrollerCreateBike: the bike's 10 channels (only the pedals swap when mirrored) */
    int i;

    for (i = 0; i < BIKE_PARTS; i++) {
        a->channelHash[i] = zHashName(s_bikeParts[i]);
        a->channelMirror[i] = s_bikeMirror[i];
    }
    a->numChannels = BIKE_PARTS;
}

/* VA 0x004654b0, 1054 bytes, [V] direct, status draft */
void NgIFzactor_ApplyMotionPose(ZActor *a)
{
    /* The actor's output pose (anim->block[0].rot, Euler per channel) onto both models: each
     * channel's frame is found by the hash of its name; the arm channels get the arm table.
     * Then the models are placed at the actor's position (+ the clip's height) and rotated by
     * (x, -y, -z) of its Euler angles. */
    Zquat q, pre, post;
    int16_t rot[3];
    float pos[3];
    uint32_t c;
    int k;

    if (s_armHash[0] == 0)
        for (k = 0; k < 6; k++)
            s_armHash[k] = zHashName(s_armFix[k].channel);
    for (c = 0; c < a->numChannels; c++) {
        ZanimNode *node = zanimFindNode(*(ZanimNodeTable **)((uint8_t *)a->model + 0x30), a->channelHash[c]);
        ZanimNode *node2 = zanimFindNode(*(ZanimNodeTable **)((uint8_t *)a->model2 + 0x30), a->channelHash[c]);

        if (node == NULL) {
            NGIF_MISSING(NGIFZACTOR_SRC, 0x1FE, "Missing hash function");
            continue;
        }
        zquatFromEuler(&q, a->anim->block[0].rot[c], 0);
        for (k = 0; k < 6; k++) {
            if (a->channelHash[c] != s_armHash[k])
                continue;
            zquatFromEuler(&post, s_armEuler[s_armFix[k].post], 0);
            if (s_armFix[k].pre < 0)
                zquatIdentity(&pre);
            else
                zquatFromEuler(&pre, s_armEuler[s_armFix[k].pre], 0);
            zquatMul(&q, &pre, &q);
            zquatMul(&q, &q, &post);
            break;
        }
        zanimNodeSetQuat(node, &q);
        zanimNodeSetQuat(node2, &q);
    }
    pos[0] = zactorGetPos(a)->x;
    pos[1] = zactorGetPos(a)->y;
    pos[2] = zactorGetPos(a)->z;
    pos[1] += a->anim->block[0].height;
    zobjSetPosition(a->model, pos);
    zobjSetPosition(a->model2, pos);
    rot[0] = a->rot[0];
    rot[1] = (int16_t)-a->rot[1];
    rot[2] = (int16_t)-a->rot[2];
    zobjSetRotationEuler(a->model, rot);
    zobjSetRotationEuler(a->model2, rot);
}

/* VA 0x004659e0, 68 bytes, [I] split, status draft */
void NgIFzactor_PosePlayer(void)
{
    /* ngappFrame: pose the current player's rider and bike for drawing */
    NgIFzactor_ApplyMotionPose((ZActor *)g_riders[cameraGetPlayer()].riderActor);
    NgIFzactor_ApplyMotionPose((ZActor *)g_riders[cameraGetPlayer()].bikeActor);
}

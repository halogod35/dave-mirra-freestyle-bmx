/*
 * zaxis/Bmx/Src/com/zge/zactor.c
 * Original: C:\zaxis\Bmx\src\com\zge\zactor.c
 * Evidence: [V] path string referenced by zactorOpenMotionLibrary / zactorLoadMotionBank; the rest
 * [I]: link order 0x00472630-0x0047785C (between winStrm.c and zprim.c) and the module's globals
 * 0x0055F35C-0x0055F38C, 0x00601D30-0x00641E30, which no code outside this range touches.
 * 85 functions (the clean-up created 40 more, reached only through their /INCREMENTAL thunks: accessors,
 * the LOD/model setters, the motion tags, zactorSampleMotion, ...; zactorInit came over from the winStrm/zactor gap; the ZGE object pass created
 * zactorCloseMotionLibrary 0x00475000). Non-matching C (AD-8).
 * Types: decomp/include/zactor.h. Clip format: docs/formats/ANIM.md section 2.
 *
 * Actors are the game's animated objects (the rider and the bike of each player, made by
 * NgIFzroller; pool of 64). An actor of kind ZACTOR_KIND_PERSON owns a ZActorAnim, the decoder state
 * for motion.zal clips:
 *  - zactorDecodeMotion decodes one clip into one slot: the rider section into block[1 + slot]
 *    (Euler angles per channel, height, root-motion delta) and the bike section into block[5 + slot]
 *    (angles) plus block[1 + slot].bikeTrans / bikeRoot. A restart reads the init records; every
 *    later call advances the clip clock by 34 ms x rate and, per 34 ms frame step, applies the next
 *    group of delta records and adds every channel's current delta. Deltas persist until replaced.
 *  - zactorPlayMotion (rider.c, every frame) decodes slot 0 and copies it into the output pose
 *    block[0], optionally crossfading over three frames from the previous clip (block[2]);
 *    zactorPlayMotionBlend mixes two clips (slots 2 and 3) by a weight.
 *  - NgIFzactor_ApplyMotionPose turns block[0].rot into node quaternions; zactorCopyBikePose and
 *    zactorCopyRootAngles hand the bike section to the bike actor.
 * The motion bank: zactorOpenMotionLibrary opens Assets\MOTION.ZAL, zactorLoadMotionBank loads a
 * group into the 256 KB static buffer and zactorOpenMemBank indexes it; action.c walks it with
 * zactorNextMotion.
 *
 * [V] the crossfade (ZActorAnim.fade) is dead in this build: riderUpdateAll always passes
 * ZMOTION_CUT. Its x-angle mix (zactorAngleMix25) and vector mix (zactorVecMix25) are buggy.
 */
#include <math.h>
#include "decomp.h"
#include "zmath.h"

#define ZACTOR_FILE "C:\\zaxis\\Bmx\\src\\com\\zge\\zactor.c"

#define ZACTOR_BANK_BUFFER_SIZE 0x40000
#define ANGLE_TO_DEG  0.0054931640625f     /* 360 / 65536 (0x0054D15C) */
#define DEG_TO_ANGLE  182.04445f           /* 65536 / 360 (0x0054D01C) */
#define ZAL_MAGIC     0x2A81511Cu

/* other modules (current names until they are decompiled) */
void zobjSetActive(void *obj, int visible);             /* zobj.c: show / hide the model */
int  __ftol(void);                                      /* CRT: float -> int of ST(0) */
#define FTOL(x) ((int32_t)(x))

/* ------------------------------------------------------------------ globals */
float            g_zactorFrameMs = 34.0f;              /* 0x0055F35C clock step per call (x rate); zactorSetFrameRate */
const int32_t    g_zactorPoolSize = 64;                /* 0x0055F360 */
const uint16_t   g_motionMirrorMap[ZACTOR_MAX_CHANNELS] = { /* 0x0055F364 left <-> right channel */
    0, 1, 2, 6, 7, 8, 3, 4, 5, 12, 13, 14, 9, 10, 11 };
int32_t          g_zactorLoadedBank = -1;              /* 0x0055F38C group in g_zactorBankBuffer */
/* 0x0055F390: per skeleton (2) and bone (19), the bone and its ancestors up to the root, 0xFF-terminated
 * (skeleton 0 = the rider: Hips 0 <- Chest 1 <- Head 2, arms 3-5 / 6-8 on the chest, legs 9-11 / 12-14) */
const int8_t     g_zactorBonePath[2][ZACTOR_MAX_CHANNELS][8] = {
    { { 0, -1 }, { 1, 0, -1 }, { 2, 1, 0, -1 }, { 3, 1, 0, -1 }, { 4, 3, 1, 0, -1 }, { 5, 4, 3, 1, 0, -1 },
      { 6, 1, 0, -1 }, { 7, 6, 1, 0, -1 }, { 8, 7, 6, 1, 0, -1 }, { 9, 0, -1 }, { 10, 9, 0, -1 },
      { 11, 10, 9, 0, -1 }, { 12, 0, -1 }, { 13, 12, 0, -1 }, { 14, 13, 12, 0, -1 } } };
/* 0x0055F4C0: per skeleton and bone, the bone's offset from its parent (engine units, w unused) */
const float      g_zactorBoneOffset[2][ZACTOR_MAX_CHANNELS][4] = {
    { { 0.0f, 0.0f, 0.0f }, { 0.0f, -19.943541f, 2.67121f }, { -0.00001f, -36.756161f, 1.55506f },
      { 18.273569f, -35.970139f, -0.57176f }, { 0.26444f, 26.30201f, 0.03595f },
      { -0.88317f, 27.131901f, -0.01658f }, { -18.273569f, -28.970131f, -0.57176f },
      { -0.26458f, 24.30201f, 0.00008f }, { 0.88317f, 27.131901f, -0.01658f },
      { 8.93869f, 0.6647f, 0.01879f }, { -0.01302f, 37.76207f, -0.03539f }, { 0.0f, 33.15057f, 0.45551f },
      { -8.93869f, 1.6647f, 0.01879f }, { 0.01303f, 37.762081f, -0.0354f }, { -0.00001f, 33.150558f, 0.45551f } } };

ZactorBikeOffset g_zactorBikeOffset;                   /* 0x00601D30 zactorGetBikeOffset's result */
ZactorFrame      g_zactorFadeFrame;                    /* 0x00601D48 crossfaded copy of g_zactorFrame (unread) */
ZactorBankEntry  g_zactorBankEntry;                    /* 0x00601D60 zactorNextMotion's result */
ZactorFrame      g_zactorBlendFrameA;                  /* 0x00601D68 zactorPlayMotionBlend, clip A */
ZactorFrame      g_zactorBlendFrameB;                  /* 0x00601D80 clip B */
ZactorFrame      g_zactorMaskFrameA;                   /* 0x00601D98 zactorPlayMotionMasked, clip A */
ZactorFrame      g_zactorMaskFrameB;                   /* 0x00601DB0 clip B */
ZactorBank       g_zactorBanks[1];                     /* 0x00601DC8 (one used: zactorLoadMotionBank reuses it) */
ZactorFrame      g_zactorFrame;                        /* 0x00601DE0 zactorPlayMotion's result */
ZactorFrame      g_zactorPrevFrame;                    /* 0x00601DF8 only .yaw: the frame before a restart */
const uint8_t   *g_zactorBankData;                     /* 0x00601E10 data of the first bank opened */
ZactorFrame     *g_zactorBlendFrames[2];               /* 0x00601E14 the two frames of a blend */
uint8_t          g_zactorBankBuffer[ZACTOR_BANK_BUFFER_SIZE]; /* 0x00601E1C the loaded motion group */
ZActor          *g_zactorPool;                         /* 0x00641E1C [g_zactorPoolSize] */
int32_t          g_zactorNumBanks;                     /* 0x00641E20 */
int16_t          g_zactorHeading;                      /* 0x00641E24 global yaw offset (action.c: -0x4000) */
int32_t          g_zactorHeadingNeg;                   /* 0x00641E28 1: rotate by -heading (action.c: 1) */
ZassetLib       *g_zactorMotionLib;                    /* 0x00641E2C Assets\MOTION.ZAL */
uint8_t         *g_zactorBankBuf;                      /* 0x00641E30 = g_zactorBankBuffer */

/* the angle the decoder rotates root deltas by (and subtracts from the frame yaw) */
#define ZACTOR_HEADING_ANGLE() (g_zactorHeadingNeg ? -(int)g_zactorHeading : (int)g_zactorHeading)

/* ------------------------------------------------------------------ the actor pool */

/* VA 0x00472630, 111 bytes, [I] row, status draft */
void zactorInit(void)
{
    int i;

    g_zactorPool = (ZActor *)zmemTempAlloc(g_zactorPoolSize * sizeof(ZActor));
    zstringMemSet(g_zactorPool, 0, g_zactorPoolSize * sizeof(ZActor));
    for (i = 0; i < g_zactorPoolSize; i++)
        g_zactorPool[i].kind = 0;
    g_zactorNumBanks = 0;
}

/* VA 0x004726c0, 1 bytes, [I] row, status draft */
void zactorStub1(void)
{
    /* new function: a bare ret, thunk 0x00402531 */
}

/* VA 0x004726e0, 56 bytes, [V] direct, status draft */
void zactorCopy(ZActor *dst, const ZActor *src)
{
    /* new function; no caller, thunk 0x00401226 */
    if (src->kind != ZACTOR_KIND_3)
        zAssertFail("Only coded for prop types", ZACTOR_FILE, 0xCE, 0);
    zstringMemCopy(dst, src, sizeof(ZActor));
}

/* VA 0x00472730, 57 bytes, [I] row, status draft */
ZActor *zactorAlloc(void)
{
    ZActor *a;

    for (a = g_zactorPool; a < g_zactorPool + g_zactorPoolSize; a++)
        if (a->kind == 0)
            return a;
    g_pfnDebugPrintf("ZACTOR: Error - failed to allocate actor\n");
    return 0;
}

/* VA 0x00472780, 25 bytes, [I] row, status draft */
ZActor *zactorCreate(int kind)
{
    ZActor *a = zactorAlloc();

    if (a == 0)
        return 0;
    return zactorSetup(kind, a);
}

/* VA 0x004727b0, 206 bytes, [I] row, status draft */
ZActor *zactorSetup(int kind, ZActor *a)
{
    zstringMemSet(a, 0, sizeof(ZActor));
    a->kind = kind;
    a->skeleton = 0;
    a->flags = ZACTOR_VISIBLE;
    a->scale.x = a->scale.y = a->scale.z = 1.0f;
    a->unk_04c = 0;
    a->colour = 0x808080;
    a->unk_05c = 0;
    a->unk_064 = 0;
    a->unk_18e = 2;
    if (kind == ZACTOR_KIND_PERSON) {
        a->anim = (ZActorAnim *)zmemTempAlloc(sizeof(ZActorAnim));
        zstringMemSet(a->anim, 0, sizeof(ZActorAnim));
        a->anim->block[0].rotBytes = sizeof a->anim->block[0].rot;
        a->anim->block[0].numChannels = ZACTOR_MAX_CHANNELS;
        a->anim->block[0].block = 0;
        a->radius = 220.0f;
        a->flags |= ZACTOR_ANIMATED;
    } else if (kind == ZACTOR_KIND_3) {
        a->flags = 0x51;
        a->radius = 80.0f;
    } else if (kind == ZACTOR_KIND_4) {
        a->radius = 50.0f;
    }
    return a;
}

/* VA 0x004728c0, 12 bytes, [I] row, status draft */
void zactorFree(ZActor *a)
{
    /* new function; no caller, thunk 0x00401B54 */
    zactorClear(a);
}

/* VA 0x004728e0, 18 bytes, [I] row, status draft */
void zactorClear(ZActor *a)
{
    /* (new function; zactorFree's body): kind 0 marks a free pool slot */
    a->flags = 0;
    a->kind = 0;
}

/* VA 0x00472910, 26 bytes, [I] row, status draft */
void zactorSetPos(ZActor *a, const RwV3d *pos)
{
    a->pos.x = pos->x;
    a->pos.y = pos->y;
    a->pos.z = pos->z;
}

/* VA 0x00472940, 8 bytes, [I] row, status draft */
RwV3d *zactorGetPos(ZActor *a)
{
    return &a->pos;
}

/* VA 0x00472960, 26 bytes, [I] row, status draft */
void zactorSetYaw(ZActor *a, int yaw)
{
    /* new function; no caller, thunk 0x0040114F */
    a->heading = a->rot[1] = (int16_t)zactorToWorldAngle(yaw);
}

/* VA 0x00472990, 18 bytes, [I] row, status draft */
int zactorGetYaw(ZActor *a)
{
    /* new function; no caller, thunk 0x0040142E */
    return zactorToLocalAngle(a->heading);
}

/* VA 0x004729c0, 36 bytes, [I] row, status draft */
int zactorToWorldAngle(int angle)
{
    /* (new function; no caller, thunk 0x00401E1F): game angle -> engine angle through the global
     * heading */
    return g_zactorHeadingNeg ? g_zactorHeading - angle : g_zactorHeading + angle;
}

/* VA 0x00472a00, 25 bytes, [I] row, status draft */
int zactorToLocalAngle(int angle)
{
    /* new function; no caller, thunk 0x00401CB7 */
    angle -= g_zactorHeading;
    return g_zactorHeadingNeg ? -angle : angle;
}

/* VA 0x00472a30, 133 bytes, [I] row, status draft */
void zactorSetScale(ZActor *a, const RwV3d *scale)
{
    float x = scale->x, y = scale->y, z = scale->z;

    if (a->kind == ZACTOR_KIND_PERSON || a->kind == ZACTOR_KIND_3) {
        if (x == 1.0f && y == 1.0f && z == 1.0f)
            a->flags &= ~ZACTOR_SCALED;
        else
            a->flags |= ZACTOR_SCALED;
    } else if (a->kind != ZACTOR_KIND_8) {
        return;
    }
    a->scale.x = x;
    a->scale.y = y;
    a->scale.z = z;
}

/* ------------------------------------------------------------------ the clip decoder */

/* Read the three angles of one channel: 16-bit (s16) or 8-bit. In 8-bit mode init values are
 * u8 << 8 and delta values s8 << 7. */
#define RD_S16(p, i)   (((const int16_t *)(p))[i])
#define RD_U8(p, i)    (((const uint8_t *)(p))[i])
#define RD_S8(p, i)    (((const int8_t *)(p))[i])
#define ALIGN_REC(p, n) ((n) & 3 ? (const uint8_t *)(p) + (4 - ((n) & 3)) : (const uint8_t *)(p))

/* VA 0x00472af0, 8 bytes, [I] row, status draft */
RwV3d *zactorGetScale(ZActor *a)
{
    /* new function; no caller, thunk 0x00401B95 */
    return &a->scale;
}

/* VA 0x00472b10, 3004 bytes, [I] row, status draft */
ZactorFrame *zactorDecodeMotion(ZActor *a, float rate, uint32_t flags, ZactorFrame *out, uint32_t slot)
{
    ZActorAnim *an = a->anim;
    ZMotionState *st = &an->block[1 + (slot & 0xFF)];     /* rider section + stream state */
    ZMotionState *bk = &an->block[5 + (slot & 0xFF)];     /* bike section angles */
    const ZMotionHeader *m = st->motion;
    const int mirror = (flags & ZMOTION_MIRROR) != 0;
    const ZMotionSection *sec;
    const uint8_t *p;
    uint32_t n;              /* bytes of the record so far, mod 4 (for the padding) */
    int s, b;

    if (a->flags & ZACTOR_HEAD_OVERRIDE) {
        st->rot[2][1] = st->headYaw;
        a->flags &= ~ZACTOR_HEAD_OVERRIDE;
    }
    out->delta[0] = out->delta[1] = out->delta[2] = 0.0f;
    out->yaw = 0;
    out->value = 0.0f;
    st->playFlags = flags;

    if (flags & ZMOTION_RESTART) {
        /* ---- frame 0: the header and one init record per section */
        out->delta[0] = st->lastDelta[0];       /* a loop continues with the last frame's root delta */
        out->delta[2] = st->lastDelta[2];
        st->bikeTrans[0] = st->bikeTrans[1] = st->bikeTrans[2] = 0.0f;
        st->frameMs = m->frameMs;
        st->duration = (m->frames & 0x7FFF) * st->frameMs;
        st->decodeFlags &= 0x21;
        if (m->flags & 0x8000)
            st->decodeFlags |= ZMOTION_WIDE;
        st->time = 0;
        st->nextFrame = 0;
        st->frameCount = 0;
        st->numSections = (uint8_t)((m->flags & 0xFF) >> 4);
        st->sections = m->section;
        st->extra = (const uint8_t *)(m->section + st->numSections) + m->extra;
        st->numChannels = m->section[0].channels;
        st->rec = (const uint32_t *)(m->section + st->numSections);
        if (m->frames & 0x8000) {
            st->block = (const uint8_t *)st->rec;
            st->rec = (const uint32_t *)((const uint8_t *)st->rec + 100);
        }
        for (s = 0, sec = m->section; s < st->numSections; s++, sec++) {
            p = (const uint8_t *)st->rec;
            n = 2;
            if (sec->kind == 0) {
                /* rider: fixed layout u32 mask, u16 time, u16 (unread), f32 value, 3 x s16 trans, all channels */
                st->value = *(const float *)(p + 8);
                out->value = st->value;
                out->delta[0] = mirror ? -(RD_S16(p, 6) * 0.125f) : RD_S16(p, 6) * 0.125f;
                out->delta[2] = RD_S16(p, 8) * 0.125f;
                st->height = RD_S16(p, 7) * 0.125f;
                p += 0x12;
                for (b = 0; b < sec->channels; b++) {
                    int c = mirror ? g_motionMirrorMap[b] : b;
                    if (st->decodeFlags & ZMOTION_WIDE) {
                        st->rot[c][0] = RD_S16(p, 0);
                        st->rot[c][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                        st->rot[c][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                        p += 6;
                        n += 2;
                    } else {
                        st->rot[c][0] = (int16_t)(RD_U8(p, 0) << 8);
                        st->rot[c][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x100 : RD_U8(p, 1) << 8);
                        st->rot[c][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x100 : RD_U8(p, 2) << 8);
                        p += 3;
                        n += 3;
                    }
                }
                st->rec = (const uint32_t *)ALIGN_REC(p, n);
            } else if (!(sec->flags & 2)) {
                /* bike: generic layout; mask bit 2 = all channels present */
                uint32_t mask = *(const uint32_t *)p;

                if (mask & 1) {
                    p += 12;            /* the f32 is not read for this section */
                    n = 0;
                } else {
                    p += 6;
                }
                if (mask & 2) {
                    st->bikeTrans[0] = mirror ? -(RD_S16(p, 0) * 0.125f) : RD_S16(p, 0) * 0.125f;
                    st->bikeTrans[1] = RD_S16(p, 1) * 0.125f;
                    st->bikeTrans[2] = RD_S16(p, 2) * 0.125f;
                    p += 6;
                    n += 2;
                }
                if (!(mask & 4)) {
                    st->bikeRoot[0] = st->bikeRoot[1] = st->bikeRoot[2] = 0;
                    st->bikeRoot2[0] = st->bikeRoot2[1] = st->bikeRoot2[2] = 0;
                } else {
                    for (b = 0; b < sec->channels; b++) {
                        if (st->decodeFlags & ZMOTION_WIDE) {
                            bk->rot[b][0] = RD_S16(p, 0);
                            bk->rot[b][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                            bk->rot[b][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                            p += 6;
                            n += 2;
                        } else {
                            bk->rot[b][0] = (int16_t)(RD_U8(p, 0) << 8);
                            bk->rot[b][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x100 : RD_U8(p, 1) << 8);
                            bk->rot[b][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x100 : RD_U8(p, 2) << 8);
                            p += 3;
                            n += 3;
                        }
                    }
                    st->bikeRoot[0] = bk->rot[0][0];
                    st->bikeRoot[1] = bk->rot[0][1];
                    st->bikeRoot[2] = bk->rot[0][2];
                }
                st->rec = (const uint32_t *)ALIGN_REC(p, n);
            }
            /* a section with flags bit 1 has no init record */
        }
        st->nextFrame += st->frameMs;
    } else {
        /* ---- advance the clock; one frame step per frameMs */
        const uint32_t *rec;

        st->time += FTOL(g_zactorFrameMs * rate);
        rec = st->rec;
        if (rec == 0 || *rec == 0)
            return 0;                              /* the clip is over */
        while (st->nextFrame <= st->time) {
            if (*rec != 0) {
                uint16_t t = (uint16_t)rec[1];     /* u16 time of the first pending record */

                /* the group of records with that time, if it is due */
                if (t <= st->time) {
                    do {
                        uint32_t mask = *rec;
                        int s16bit = (st->decodeFlags & ZMOTION_WIDE) != 0;

                        sec = &st->sections[mask >> 28];
                        p = (const uint8_t *)rec + 6;
                        n = 2;
                        if (sec->kind == 0) {
                            if (mask & 1) {
                                st->value = *(const float *)((const uint8_t *)rec + 8);
                                out->value = st->value;
                                p = (const uint8_t *)rec + 12;
                                n = 0;
                            }
                            if (mask & 2) {
                                st->transDelta[0] = mirror ? -(RD_S16(p, 0) * (1.0f / 256)) : RD_S16(p, 0) * (1.0f / 256);
                                st->transDelta[2] = RD_S16(p, 2) * (1.0f / 256);
                                st->transDelta[1] = RD_S16(p, 1) * (1.0f / 256);
                                p += 6;
                                n += 2;
                            }
                            for (b = 0; b < sec->channels; b++) {
                                int c = mirror ? g_motionMirrorMap[b] : b;
                                if (!(mask & (4u << b)))
                                    continue;
                                if (s16bit) {
                                    st->rotDelta[c][0] = RD_S16(p, 0);
                                    st->rotDelta[c][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                                    st->rotDelta[c][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                                    p += 6;
                                    n += 2;
                                } else {
                                    st->rotDelta[c][0] = (int16_t)(RD_S8(p, 0) << 7);
                                    st->rotDelta[c][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x80 : RD_S8(p, 1) << 7);
                                    st->rotDelta[c][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x80 : RD_S8(p, 2) << 7);
                                    p += 3;
                                    n += 3;
                                }
                            }
                        } else {
                            if (mask & 1) {
                                p = (const uint8_t *)rec + 12;     /* f32 skipped */
                                n = 0;
                            }
                            if (mask & 2) {
                                st->bikeTransDelta[0] = mirror ? -(RD_S16(p, 0) * (1.0f / 256)) : RD_S16(p, 0) * (1.0f / 256);
                                st->bikeTransDelta[1] = RD_S16(p, 1) * (1.0f / 256);
                                st->bikeTransDelta[2] = RD_S16(p, 2) * (1.0f / 256);
                                p += 6;
                                n += 2;
                            }
                            for (b = 0; b < sec->channels; b++) {
                                if (!(mask & (4u << b)))
                                    continue;
                                if (s16bit) {
                                    bk->rotDelta[b][0] = RD_S16(p, 0);
                                    bk->rotDelta[b][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                                    bk->rotDelta[b][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                                    p += 6;
                                    n += 2;
                                } else {
                                    bk->rotDelta[b][0] = (int16_t)(RD_S8(p, 0) << 7);
                                    bk->rotDelta[b][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x80 : RD_S8(p, 1) << 7);
                                    bk->rotDelta[b][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x80 : RD_S8(p, 2) << 7);
                                    p += 3;
                                    n += 3;
                                }
                            }
                        }
                        st->rec = (const uint32_t *)ALIGN_REC(p, n);
                        rec = st->rec;
                    } while ((uint16_t)rec[1] == t);
                }

                /* one frame step: every channel adds its current delta */
                for (s = 0, sec = st->sections; s < st->numSections; s++, sec++) {
                    if (sec->kind == 0) {
                        st->frameCount++;
                        st->height += st->transDelta[1];
                        out->delta[0] = st->transDelta[0];      /* =, not +=: one step's delta */
                        out->delta[2] = st->transDelta[2];
                        for (b = 0; b < sec->channels; b++) {
                            st->rot[b][0] += st->rotDelta[b][0];
                            st->rot[b][1] += st->rotDelta[b][1];
                            st->rot[b][2] += st->rotDelta[b][2];
                        }
                    } else if (!(sec->flags & 2)) {
                        st->bikeTrans[0] += st->bikeTransDelta[0];
                        st->bikeTrans[1] += st->bikeTransDelta[1];
                        st->bikeTrans[2] += st->bikeTransDelta[2];
                        for (b = 0; b < sec->channels; b++) {
                            bk->rot[b][0] += bk->rotDelta[b][0];
                            bk->rot[b][1] += bk->rotDelta[b][1];
                            bk->rot[b][2] += bk->rotDelta[b][2];
                        }
                        st->bikeRoot[0] = bk->rot[0][0];
                        st->bikeRoot[1] = bk->rot[0][1];
                        st->bikeRoot[2] = bk->rot[0][2];
                    }
                }
            }
            st->nextFrame += st->frameMs;
        }
    }

    if (st->nextFrame >= st->duration) {
        st->lastDelta[0] = st->transDelta[0];
        st->lastDelta[1] = st->transDelta[1];
        st->lastDelta[2] = st->transDelta[2];
    }
    out->yaw = st->rot[0][1];
    out->value = st->value;
    if (g_zactorHeadingNeg == 0)
        out->yaw -= g_zactorHeading;
    else
        out->yaw = g_zactorHeading - out->yaw;
    zmathVecRotateY((RwV3d *)out->delta, (RwV3d *)out->delta, ZACTOR_HEADING_ANGLE());
    if (a->flags & ZACTOR_SCALED) {
        out->delta[0] *= a->scale.x;
        out->delta[1] *= a->scale.y;
        out->delta[2] *= a->scale.z;
    }
    return out;
}

/* ------------------------------------------------------------------ playing clips */

/* VA 0x004739c0, 1357 bytes, [I] row, status draft */
void zactorSampleMotion(ZActor *a, int16_t (*rot)[3], const ZMotionHeader *m, int frames, uint8_t flags)
{
    /* (new function; no caller, thunk 0x0040240F) The rider-section pose `frames` frames into a
     * clip, without touching the actor's decoder: the channel angles go to rot[channel][3]. It decodes
     * into an *uninitialised* stack ZMotionState, so the per-channel deltas start as garbage until a
     * record sets them, and the head-yaw override copies a garbage word (it does clear the actor's
     * flag). Original bugs [V, disassembly]: - decodeFlags is masked with 0x21 but ZMOTION_WIDE is
     * never set from the clip header, so the 16-bit angle branches are unreachable: every shipped clip
     * (all 16-bit) decodes wrongly; - other sections only skip the record header and translation, not
     * their channel data. Mirror (flags & ZMOTION_MIRROR) swaps left/right channels and negates the y
     * / z angles. */
    ZMotionState st;         /* not initialised, as in the exe */
    const int mirror = (flags & ZMOTION_MIRROR) != 0;
    const ZMotionSection *sec;
    const uint8_t *p;
    const uint32_t *rec;
    uint32_t frameMs, time = 0, nextFrame, n;
    int s, b;

    if (a->flags & ZACTOR_HEAD_OVERRIDE) {
        st.rot[2][1] = st.headYaw;
        a->flags &= ~ZACTOR_HEAD_OVERRIDE;
    }
    frameMs = m->frameMs;
    st.duration = (m->frames & 0x7FFF) * frameMs;          /* computed, never used */
    st.decodeFlags &= 0x21;                                /* sic: ZMOTION_WIDE is never set */
    st.numSections = (uint8_t)((m->flags & 0xFF) >> 4);
    st.numChannels = m->section[0].channels;
    st.frameCount = 0;
    rec = (const uint32_t *)(m->section + st.numSections);
    if (m->frames & 0x8000)
        rec = (const uint32_t *)((const uint8_t *)rec + 100);

    /* frame 0: the init record of each section */
    for (s = 0, sec = m->section; s < st.numSections; s++, sec++) {
        if (sec->kind == 0) {
            p = (const uint8_t *)rec + 0x12;
            n = 2;
            for (b = 0; b < sec->channels; b++) {
                int c = mirror ? g_motionMirrorMap[b] : b;
                if (st.decodeFlags & ZMOTION_WIDE) {          /* unreachable */
                    rot[c][0] = RD_S16(p, 0);
                    rot[c][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                    rot[c][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                    p += 6;
                    n += 2;
                } else {
                    rot[c][0] = (int16_t)(RD_U8(p, 0) << 8);
                    rot[c][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x100 : RD_U8(p, 1) << 8);
                    rot[c][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x100 : RD_U8(p, 2) << 8);
                    p += 3;
                    n += 3;
                }
            }
            rec = (const uint32_t *)ALIGN_REC(p, n);
        } else if (!(sec->flags & 2)) {
            uint32_t mask = *rec;

            p = (const uint8_t *)rec + ((mask & 1) ? 12 : 6);
            n = (mask & 1) ? 0 : 2;
            if (mask & 2) {
                p += 6;
                n += 2;
            }
            rec = (const uint32_t *)ALIGN_REC(p, n);         /* sic: channel data not skipped */
        }
    }

    /* then `frames` steps of frameMs */
    nextFrame = frameMs;
    while (frames-- > 0) {
        time += frameMs;
        if (rec == 0 || *rec == 0)
            return;
        while (nextFrame <= time) {
            if (*rec != 0) {
                uint16_t t = (uint16_t)rec[1];

                if (t <= time) {
                    do {
                        uint32_t mask = *rec;

                        sec = &m->section[mask >> 28];
                        p = (const uint8_t *)rec + ((mask & 1) ? 12 : 6);
                        n = (mask & 1) ? 0 : 2;
                        if (mask & 2) {                    /* the translation is not used here */
                            p += 6;
                            n += 2;
                        }
                        if (sec->kind == 0) {
                            for (b = 0; b < sec->channels; b++) {
                                int c = mirror ? g_motionMirrorMap[b] : b;
                                if (!(mask & (4u << b)))
                                    continue;
                                if (st.decodeFlags & ZMOTION_WIDE) {      /* unreachable */
                                    st.rotDelta[c][0] = RD_S16(p, 0);
                                    st.rotDelta[c][1] = (int16_t)(mirror ? -RD_S16(p, 1) : RD_S16(p, 1));
                                    st.rotDelta[c][2] = (int16_t)(mirror ? -RD_S16(p, 2) : RD_S16(p, 2));
                                    p += 6;
                                    n += 2;
                                } else {
                                    st.rotDelta[c][0] = (int16_t)(RD_S8(p, 0) << 7);
                                    st.rotDelta[c][1] = (int16_t)(mirror ? RD_S8(p, 1) * -0x80 : RD_S8(p, 1) << 7);
                                    st.rotDelta[c][2] = (int16_t)(mirror ? RD_S8(p, 2) * -0x80 : RD_S8(p, 2) << 7);
                                    p += 3;
                                    n += 3;
                                }
                            }
                        }
                        rec = (const uint32_t *)ALIGN_REC(p, n);
                    } while ((uint16_t)rec[1] == t);
                }
                /* one frame step of the rider sections */
                for (s = 0, sec = m->section; s < st.numSections; s++, sec++) {
                    if (sec->kind != 0)
                        continue;
                    st.frameCount++;
                    for (b = 0; b < sec->channels; b++) {
                        rot[b][0] += st.rotDelta[b][0];
                        rot[b][1] += st.rotDelta[b][1];
                        rot[b][2] += st.rotDelta[b][2];
                    }
                }
            }
            nextFrame += frameMs;
        }
    }
}

/* VA 0x00474070, 60 bytes, [I] row, status draft */
int zactorVecIsZero(const float *v)
{
    return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
}

/* VA 0x004740c0, 1734 bytes, [I] row, status draft */
ZactorFrame *zactorPlayMotion(ZActor *a, const ZMotionHeader *motion, float rate, uint32_t flags)
{
    ZActorAnim *an = a->anim;
    ZMotionState *out = &an->block[0];
    ZMotionState *cur = &an->block[1];          /* slot 0 */
    ZMotionState *prev = &an->block[2];         /* the previous clip's last pose (crossfade source) */
    ZMotionState *bikeCur = &an->block[5];      /* slot 0 bike angles */
    ZMotionState *bikeOut = &an->block[6];      /* bike angles handed to the bike actor */
    ZactorFrame *f;
    int i, yaw;

    a->flags |= ZACTOR_MOTION_POSED;
    cur->motion = motion;
    f = zactorDecodeMotion(a, rate, flags, &g_zactorFrame, 0);
    if (f == 0)
        return 0;
    if (flags & ZMOTION_RESTART) {
        if (flags & ZMOTION_CUT) {
            prev->motion = 0;
            an->fade = 0;
        } else if (prev->motion == 0) {
            an->fade = 0;
        } else {
            /* keep the pose shown so far and fade from it over three frames */
            for (i = 0; i < cur->numChannels * 3; i++)
                (&prev->rot[0][0])[i] = (&out->rot[0][0])[i];
            prev->bikeTrans[0] = out->bikeTrans[0];
            prev->bikeTrans[1] = out->bikeTrans[1];
            prev->bikeTrans[2] = out->bikeTrans[2];
            prev->bikeRoot[0] = out->bikeRoot[0];
            prev->bikeRoot[1] = out->bikeRoot[1];
            prev->bikeRoot[2] = out->bikeRoot[2];
            prev->height = out->height;
            g_zactorPrevFrame.yaw = g_zactorFrame.yaw;
            an->fade = 3;
        }
        out->numChannels = cur->numChannels;
        out->numSections = cur->numSections;
    }

    switch (an->fade) {
    case 0:
        g_zactorFadeFrame.yaw = g_zactorFrame.yaw;
        zstringMemCopy(out->rot, cur->rot, out->numChannels * 6);
        out->bikeTrans[0] = cur->bikeTrans[0];
        out->bikeTrans[1] = cur->bikeTrans[1];
        out->bikeTrans[2] = cur->bikeTrans[2];
        out->bikeRoot[0] = cur->bikeRoot[0];
        out->bikeRoot[1] = cur->bikeRoot[1];
        out->bikeRoot[2] = cur->bikeRoot[2];
        out->height = cur->height;
        zstringMemCopy(bikeOut->rot[1], bikeCur->rot[1], 9 * 6);
        break;
    case 1:                                     /* 3/4 new */
        yaw = g_zactorFrame.yaw * 3 + g_zactorPrevFrame.yaw;
        g_zactorFadeFrame.yaw = (int16_t)(yaw / 4);
        out->height = (cur->height * 3.0f + prev->height) * 0.25f;
        zactorAngleMix25(prev->rot[0], cur->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            zactorAngleMix25(bikeOut->rot[1], bikeCur->rot[1], bikeOut->rot[1], 9);
            zactorAngleMix25(prev->bikeRoot, cur->bikeRoot, out->bikeRoot, 1);
            zactorVecMix25(prev->bikeTrans, cur->bikeTrans, out->bikeTrans);
        } else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
            out->bikeRoot[0] = cur->bikeRoot[0];
            out->bikeRoot[1] = cur->bikeRoot[1];
            out->bikeRoot[2] = cur->bikeRoot[2];
        }
        break;
    case 2:                                     /* half way */
        g_zactorFadeFrame.yaw = (int16_t)((g_zactorPrevFrame.yaw + g_zactorFrame.yaw) / 2);
        out->height = (prev->height + cur->height) * 0.5f;
        zactorAngleMix50(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            zactorAngleMix50(bikeCur->rot[1], bikeOut->rot[1], bikeOut->rot[1], 9);
            zactorAngleMix50(cur->bikeRoot, prev->bikeRoot, out->bikeRoot, 1);
            zactorVecMix50(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        } else {
            out->bikeRoot[0] = cur->bikeRoot[0];
            out->bikeRoot[1] = cur->bikeRoot[1];
            out->bikeRoot[2] = cur->bikeRoot[2];
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 3:                                     /* 1/4 new */
        yaw = g_zactorPrevFrame.yaw * 3 + g_zactorFrame.yaw;
        g_zactorFadeFrame.yaw = (int16_t)(yaw / 4);
        out->height = (prev->height * 3.0f + cur->height) * 0.25f;
        zactorAngleMix25(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            zactorAngleMix25(bikeCur->rot[1], bikeOut->rot[1], bikeOut->rot[1], 9);
            zactorAngleMix25(cur->bikeRoot, prev->bikeRoot, out->bikeRoot, 1);
            zactorVecMix25(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        } else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
            out->bikeRoot[0] = cur->bikeRoot[0];
            out->bikeRoot[1] = cur->bikeRoot[1];
            out->bikeRoot[2] = cur->bikeRoot[2];
        }
        break;
    case 4:                                     /* the old pose (never set: fade starts at 3) */
        g_zactorFadeFrame.yaw = g_zactorPrevFrame.yaw;
        zstringMemCopy(out->rot, prev->rot, out->numChannels * 6);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            out->bikeTrans[0] = prev->bikeTrans[0];
            out->bikeTrans[1] = prev->bikeTrans[1];
            out->bikeTrans[2] = prev->bikeTrans[2];
            out->bikeRoot[0] = prev->bikeRoot[0];
            out->bikeRoot[1] = prev->bikeRoot[1];
            out->bikeRoot[2] = prev->bikeRoot[2];
        } else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
            out->bikeRoot[0] = cur->bikeRoot[0];
            out->bikeRoot[1] = cur->bikeRoot[1];
            out->bikeRoot[2] = cur->bikeRoot[2];
        }
        out->height = prev->height;
        break;
    }
    g_zactorFadeFrame.value = g_zactorFrame.value;
    g_zactorFadeFrame.delta[0] = g_zactorFrame.delta[0];
    g_zactorFadeFrame.delta[1] = g_zactorFrame.delta[1];
    g_zactorFadeFrame.delta[2] = g_zactorFrame.delta[2];
    if (an->fade != 0)
        an->fade--;
    out->frameCount = cur->frameCount;
    out->block = cur->block;
    if (!(flags & ZMOTION_CUT))
        prev->motion = motion;
    out->motion = motion;
    out->bikeTrans[0] = cur->bikeTrans[0];      /* the bike placement is never faded */
    out->bikeTrans[1] = cur->bikeTrans[1];
    out->bikeTrans[2] = cur->bikeTrans[2];
    return f;
}

/* VA 0x00474960, 38 bytes, [I] row, status draft */
void zactorCopyBikePose(ZActor *rider, ZActor *bike)
{
    /* the bike section's channels 1..9 (Handlebars .. SeatGrip); channel 0 goes through bikeRoot */
    zstringMemCopy(bike->anim->block[0].rot[1], rider->anim->block[6].rot[1], 9 * 6);
}

/* (a + b) / 2 per angle, the short way round. */
/* VA 0x004749a0, 276 bytes, [I] row, status draft */
void zactorAngleMix50(const int16_t *a, const int16_t *b, int16_t *out, int channels)
{
    int i, d;

    do {
        for (i = 0; i < 3; i++) {
            d = a[i] - b[i];
            if ((d < 0 ? -d : d) < 0x8000)
                out[i] = (int16_t)((a[i] + b[i]) >> 1);
            else
                out[i] = (int16_t)(((uint16_t)a[i] + (uint32_t)(uint16_t)b[i]) >> 1);
        }
        a += 3;
        b += 3;
        out += 3;
    } while (--channels != 0);
}

/* a / 4 + 3b / 4 per angle. [V] x goes through degrees and is wrong: the test meant to pick the
 * wrap-around case (|a - b| > 180) is "|a - b| >= -180", always true, so 360 is always added to the
 * smaller angle (the result is off by 90 or 270 degrees). y and z are right. Only the dead crossfade
 * uses it. */
/* VA 0x00474b10, 408 bytes, [I] row, status draft */
void zactorAngleMix25(const int16_t *a, const int16_t *b, int16_t *out, int channels)
{
    int i, d;

    do {
        float da = a[0] * ANGLE_TO_DEG;
        float db = b[0] * ANGLE_TO_DEG;
        float r;

        if (!(da - db >= -180.0f || db - da >= -180.0f)) {    /* |da - db| < -180: never */
            r = db * 0.75f + da * 0.25f;
        } else {
            if (db < da)
                db += 360.0f;
            else
                da += 360.0f;
            r = db * 0.75f + da * 0.25f;
            while (r > 360.0f)
                r -= 360.0f;
            while (r < -360.0f)
                r += 360.0f;
        }
        out[0] = (int16_t)FTOL(r * DEG_TO_ANGLE);
        for (i = 1; i < 3; i++) {
            d = a[i] - b[i];
            if ((d < 0 ? -d : d) < 0x8000)
                out[i] = (int16_t)((a[i] + b[i] * 3) >> 2);
            else
                out[i] = (int16_t)(((uint32_t)(uint16_t)b[i] * 3 + (uint16_t)a[i]) >> 2);
        }
        a += 3;
        b += 3;
        out += 3;
    } while (--channels != 0);
}

/* VA 0x00474d20, 55 bytes, [I] row, status draft */
void zactorVecMix50(const float *a, const float *b, float *out)
{
    out[0] = (a[0] + b[0]) * 0.5f;
    out[1] = (a[1] + b[1]) * 0.5f;
    out[2] = (a[2] + b[2]) * 0.5f;
}

/* [V] weights 0.5 and 1.5 (sum 2); 0.25 / 0.75 were probably meant. Only the dead crossfade uses it. */
/* VA 0x00474d70, 79 bytes, [I] row, status draft */
void zactorVecMix25(const float *a, const float *b, float *out)
{
    out[0] = b[0] * 1.5f + a[0] * 0.5f;
    out[1] = b[1] * 1.5f + a[1] * 0.5f;
    out[2] = b[2] * 1.5f + a[2] * 0.5f;
}

/* Where the bike sits relative to the rider (the clip's bike-section translation) and the bike's root
 * angles; physics places the bike with it. NULL unless the clip has two sections. */
/* VA 0x00474de0, 225 bytes, [I] row, status draft */
ZactorBikeOffset *zactorGetBikeOffset(ZActor *a)
{
    ZMotionState *out = &a->anim->block[0];

    if (out->numSections != 2)
        return 0;
    g_zactorBikeOffset.pos.x = out->bikeTrans[0];
    g_zactorBikeOffset.pos.y = out->bikeTrans[1];
    g_zactorBikeOffset.pos.z = out->bikeTrans[2];
    g_zactorBikeOffset.zero = 0;
    zmathVecRotateY(&g_zactorBikeOffset.pos, &g_zactorBikeOffset.pos, ZACTOR_HEADING_ANGLE());
    if (a->flags & ZACTOR_SCALED) {
        g_zactorBikeOffset.pos.x *= a->scale.x;
        g_zactorBikeOffset.pos.y *= a->scale.y;
        g_zactorBikeOffset.pos.z *= a->scale.z;
    }
    g_zactorBikeOffset.rot[0] = out->bikeRoot[0];
    g_zactorBikeOffset.rot[1] = out->bikeRoot[1];
    g_zactorBikeOffset.rot[2] = out->bikeRoot[2];
    return &g_zactorBikeOffset;
}

/* ------------------------------------------------------------------ the motion bank */

/* VA 0x00474f10, 178 bytes, [V] direct, status draft */
int zactorOpenMotionLibrary(const char *path)
{
    uint32_t group = 0;

    if (g_zactorMotionLib != 0)
        zAssertFail("Motion library already open", ZACTOR_FILE, 0x957, 0);
    g_zactorMotionLib = zassetOpenLibrary(path);
    if (g_zactorMotionLib == 0)
        return 1;
    /* every group must fit the static buffer */
    while (zassetFindAsset(g_zactorMotionLib, group, 0) == 1) {
        if (zassetGetAssetSize(g_zactorMotionLib) > ZACTOR_BANK_BUFFER_SIZE)
            zAssertFail("Motions are too large.", ZACTOR_FILE, 0x962, 0);
        group++;
    }
    g_zactorBankBuf = g_zactorBankBuffer;
    zactorLoadMotionBank(0);
    return 0;
}

/* VA 0x00475000, 59 bytes, [I] row, status draft */
void zactorCloseMotionLibrary(void)
{
    /* The ZGE object pass created this function (found during the NgIF pass): NgApp's level shutdown calls it */
    if (g_zactorMotionLib == 0)
        zAssertFail("Motions not opened", ZACTOR_FILE, 0x982, 0);
    zassetCloseLibrary(g_zactorMotionLib);
    g_zactorMotionLib = 0;
}

/* VA 0x00475050, 186 bytes, [V] direct, status draft */
ZactorBank *zactorLoadMotionBank(uint32_t bankId)
{
    uint32_t group = bankId & 0xFFFF;
    uint32_t size;

    if ((int32_t)group == g_zactorLoadedBank) {
        g_pfnDebugPrintf("Not reloading motion library\n");
        return &g_zactorBanks[0];
    }
    if (zassetFindAsset(g_zactorMotionLib, group, 0) == 0) {
        zAssertFail("Could not find motion bank", ZACTOR_FILE, 0x9A0, 0);
        return 0;
    }
    size = zassetGetAssetSize(g_zactorMotionLib);
    if (size > ZACTOR_BANK_BUFFER_SIZE)
        zAssertFail("Too big", ZACTOR_FILE, 0x9A6, 0);
    zassetLoadAsset(g_zactorMotionLib, g_zactorBankBuf);
    g_zactorLoadedBank = group;
    return zactorOpenMemBank(g_zactorBankBuf, size, 1);
}

/* Index an in-memory ZAL: with the 16-byte header (magic 0x2A81511C) or headerless
 * (docs/formats/ZAL.md 2.6); a first byte of 0xFF skips 8 more bytes. reuse != 0 overwrites bank 0
 * once one exists (the motion bank always does). Was zasset_OpenMemLibrary. */
/* VA 0x00475140, 129 bytes, [V] direct, status draft */
void zactorLoadMotionBankTemp(uint32_t bankId)
{
    /* (new function; no caller, thunk 0x004013F7) Loads motion group `bankId` of MOTION.ZAL into
     * temp memory and opens it as a bank (unlike zactorLoadMotionBank, which reuses
     * g_zactorBankBuffer). */
    uint32_t size;
    uint8_t *data;

    if (g_zactorMotionLib == 0)
        zAssertFail("Motion bank not opened", ZACTOR_FILE, 0x9BF, 0);
    if (!zassetFindAsset(g_zactorMotionLib, bankId & 0xFFFF, 0))
        return;
    size = zassetGetAssetSize(g_zactorMotionLib);
    data = (uint8_t *)zmemTempAlloc(size);
    if (data == 0)
        return;
    zassetLoadAsset(g_zactorMotionLib, data);
    zactorOpenMemBank(data, size, 0);
}

/* VA 0x00475200, 131 bytes, [I] row, status draft */
ZactorBank *zactorOpenMemBank(const uint8_t *data, uint32_t size, int reuse)
{
    ZactorBank *bank;

    if (g_zactorNumBanks == 0)
        g_zactorBankData = data;
    if (reuse && g_zactorNumBanks != 0)
        bank = &g_zactorBanks[0];
    else
        bank = &g_zactorBanks[g_zactorNumBanks++];
    bank->base = data;
    bank->size = size;
    if (*(const uint32_t *)data == ZAL_MAGIC) {
        bank->base = data + 0x10;
        data += 0x10;
    } else if (data[0] == 0xFF) {
        data += 8;
    }
    bank->table = (const uint16_t *)data;
    bank->cursor = (const uint32_t *)(data + 4);
    bank->left = bank->table[1];
    return bank;
}

/* Address of motion `id` in the open banks (0 if absent). Unreferenced. */
/* VA 0x004752c0, 1 bytes, [I] row, status draft */
void zactorStub2(void)
{
    /* new function: a bare ret, thunk 0x00401A87 */
}

/* VA 0x004752e0, 15 bytes, [I] row, status draft */
ZactorBank *zactorGetBank(int index)
{
    /* new function; no caller, thunk 0x00401C94 */
    return &g_zactorBanks[index];
}

/* VA 0x00475300, 142 bytes, [I] row, status draft */
const uint8_t *zactorFindMotion(uint32_t id)
{
    int i, g;

    for (i = 0; i < g_zactorNumBanks; i++) {
        const ZactorBank *bank = &g_zactorBanks[i];
        const uint32_t *e = (const uint32_t *)(bank->table + 2);

        for (g = 0; g < bank->table[1]; g++) {
            if (e[0] == id)
                return bank->base + e[1];
            e += 1 + bank->table[0] * 2;          /* u32 id, then {u32 offset, u32 size} per asset */
        }
    }
    return 0;
}

/* The next motion of the bank: {id, data} of its first asset, or NULL after the last one. */
/* VA 0x004753c0, 79 bytes, [I] row, status draft */
ZactorBankEntry *zactorNextMotion(ZactorBank *bank)
{
    const uint32_t *e;

    g_zactorBankEntry.id = 0;
    g_zactorBankEntry.data = 0;
    if (bank->left == 0)
        return 0;
    bank->left--;
    e = bank->cursor;
    g_zactorBankEntry.id = e[0];
    g_zactorBankEntry.data = bank->base + e[1];
    bank->cursor = e + 1 + bank->table[0] * 2;
    return &g_zactorBankEntry;
}

/* ------------------------------------------------------------------ actor state */

/* VA 0x00475430, 24 bytes, [I] row, status draft */
void zactorSetModel(ZActor *a, uint16_t model)
{
    /* new function; no caller, thunk 0x0040107D */
    a->modelId = model;
    ZACTOR_LOD_MODEL(a, 0) = model;
}

/* VA 0x00475460, 78 bytes, [V] direct, status draft */
void zactorSetLodModel(ZActor *a, uint16_t lod, uint16_t model)
{
    /* new function; no caller, thunk 0x00402586 */
    if (lod > 3)
        zAssertFail("Exeeded maximum LODs for actor", ZACTOR_FILE, 0xA76, 0);
    ZACTOR_LOD_MODEL(a, lod) = model;
    if (lod == 0)
        a->modelId = model;
}

/* VA 0x004754d0, 12 bytes, [I] row, status draft */
uint16_t zactorGetModel(ZActor *a)
{
    /* new function; no caller, thunk 0x00401AF0 */
    return a->modelId;
}

/* The tag block of a clip: at clip + header.extra (0 in every shipped clip = no tags). */
#define ZMOTION_TAGS(m) ((const uint8_t *)(m) + (m)->extra)

/* VA 0x004754f0, 19 bytes, [I] row, status draft */
uint8_t zactorGetMotionTagCount(const ZMotionHeader *m)
{
    /* new function; no caller, thunk 0x0040189D */
    if (m->extra == 0)
        return 0;
    return *(ZMOTION_TAGS(m) + 0xC);
}

/* VA 0x00475520, 402 bytes, [I] row, status draft */
uint8_t zactorGetMotionTags(const ZMotionHeader *m, ZactorTag *out)
{
    /* (new function; no caller, thunk 0x00402202) All tags of a clip: u16 count at +0xC of the tag
     * block, records from +0x10 of 4 s16 (time, x, y, z) or, when header flags bits 12-14 are set, 5
     * (+ id). Positions are rotated by the global heading. Returns the count (low byte). */
    const uint8_t *blk;
    const int16_t *r;
    int i, withId;
    uint16_t count;

    if (m->extra == 0)
        return 0;
    blk = ZMOTION_TAGS(m);
    count = *(const uint16_t *)(blk + 0xC);
    r = (const int16_t *)(blk + 0x10);
    withId = (m->flags & 0x7000) != 0;
    for (i = 0; i < count; i++, out++) {
        out->pos.x = r[1] * 0.125f;
        out->pos.y = r[2] * 0.125f;
        out->pos.z = r[3] * 0.125f;
        out->time = r[0] * (1.0f / 1024);
        out->id = withId ? r[4] : 0;
        zmathVecRotateY(&out->pos, &out->pos, ZACTOR_HEADING_ANGLE());
        r += withId ? 5 : 4;
    }
    return (uint8_t)count;
}

/* VA 0x00475730, 347 bytes, [I] row, status draft */
uint8_t zactorGetFirstMotionTag(const ZMotionHeader *m, ZactorTag *out)
{
    /* (new function; no caller, thunk 0x00402338) The first tag only (same record format); returns
     * the tag count (low byte). */
    const int16_t *r;

    if (m->extra == 0)
        return 0;
    r = (const int16_t *)(ZMOTION_TAGS(m) + 0x10);
    out->pos.x = r[1] * 0.125f;
    out->pos.y = r[2] * 0.125f;
    out->pos.z = r[3] * 0.125f;
    out->time = r[0] * (1.0f / 1024);
    out->id = (m->flags & 0x7000) ? r[4] : 0;
    zmathVecRotateY(&out->pos, &out->pos, ZACTOR_HEADING_ANGLE());
    return *(ZMOTION_TAGS(m) + 0xC);
}

/* VA 0x004758f0, 18 bytes, [I] row, status draft */
void zactorSetLod(ZActor *a, uint8_t lod)
{
    /* new function; no caller, thunk 0x004024B4 */
    a->lod = lod;
}

/* VA 0x00475920, 15 bytes, [I] row, status draft */
void zactorSetRadius(ZActor *a, float radius)
{
    /* new function; no caller, thunk 0x0040200E */
    a->radius = radius;
}

/* VA 0x00475940, 97 bytes, [I] row, status draft */
void zactorSetExtent(ZActor *a, const RwV3d *extent)
{
    /* new function; no caller, thunk 0x00401B31 */
    float m;

    a->extent = *extent;
    m = a->extent.x > a->extent.y ? a->extent.x : a->extent.y;
    zactorSetRadius(a, m < extent->z ? extent->z : m);
}

/* VA 0x004759d0, 22 bytes, [I] row, status draft */
void zactorSetHeading(int16_t heading, int negate)
{
    g_zactorHeading = heading;
    g_zactorHeadingNeg = negate;
}

/* VA 0x00475a00, 31 bytes, [I] row, status draft */
int zactorAddAngle(int angle, int delta)
{
    /* new function; no caller, thunk 0x0040240A */
    return g_zactorHeadingNeg ? angle - delta : angle + delta;
}

/* VA 0x00475a30, 31 bytes, [I] row, status draft */
void zactorShow(ZActor *a)
{
    a->flags |= ZACTOR_VISIBLE;
    zobjSetActive(a->model, 1);
}

/* VA 0x00475a60, 31 bytes, [I] row, status draft */
void zactorHide(ZActor *a)
{
    a->flags &= ~ZACTOR_VISIBLE;
    zobjSetActive(a->model, 0);
}

/* VA 0x00475a90, 9 bytes, [I] row, status draft */
void zactorSetFlag2(ZActor *a)
{
    a->flags |= ZACTOR_FLAG_2;
}

/* VA 0x00475ab0, 9 bytes, [I] row, status draft */
void zactorClearFlag2(ZActor *a)
{
    a->flags &= ~ZACTOR_FLAG_2;
}

/* World position of a point 160 units up the spine: (0, -160, 0) turned by the Hips and Chest
 * angles of the pose and the actor's yaw. Unreferenced. */
/* VA 0x00475ad0, 45 bytes, [I] row, status draft */
void zactorSetTarget(ZActor *a, const RwV3d *pos, uint32_t id)
{
    /* new function; no caller, thunk 0x0040131B */
    a->targetId = id;
    a->target = *pos;
}

/* VA 0x00475b10, 35 bytes, [I] row, status draft */
void zactorEnableTag(ZActor *a, int tag)
{
    /* new function; no caller, thunk 0x00401F69): only tag 0 exists (flag 0x800 */
    if (tag == 0)
        a->flags |= 0x800;
    else
        g_pfnDebugPrintf("ZACTOR: Unknown tag identifier \n");
}

/* VA 0x00475b50, 35 bytes, [I] row, status draft */
void zactorDisableTag(ZActor *a, int tag)
{
    /* new function; no caller, thunk 0x00401474 */
    if (tag == 0)
        a->flags &= ~0x800u;
    else
        g_pfnDebugPrintf("ZACTOR: Unknown tag identifier \n");
}

/* VA 0x00475b90, 9 bytes, [I] row, status draft */
void zactorSetFlag20(ZActor *a)
{
    /* new function; no caller, thunk 0x00401E5B */
    a->flags |= 0x20;
}

/* VA 0x00475bb0, 9 bytes, [I] row, status draft */
void zactorClearFlag20(ZActor *a)
{
    /* new function; no caller, thunk 0x00401D34 */
    a->flags &= ~0x20u;
}

/* VA 0x00475bd0, 1 bytes, [I] row, status draft */
void zactorStub3(void)
{
    /* new function: a bare ret, thunk 0x00401C44 */
}

/* VA 0x00475bf0, 26 bytes, [I] row, status draft */
void zactorSetFrameRate(int16_t rate)
{
    /* (new function; no caller, thunk 0x00401E1A) The decoder's clock step for `rate` frames per
     * second (1024 / rate; 34 is the default). */
    g_zactorFrameMs = 1024.0f / (float)rate;
}

/* VA 0x00475c20, 1 bytes, [I] row, status draft */
void zactorStub4(void)
{
    /* new function: a bare ret, thunk 0x004010FA */
}

/* VA 0x00475c40, 15 bytes, [I] row, status draft */
void zactorSetUser(ZActor *a, void *user)
{
    /* new function; no caller, thunk 0x004015E1 */
    a->user = user;
}

/* VA 0x00475c60, 11 bytes, [I] row, status draft */
void *zactorGetUser(ZActor *a)
{
    /* new function; no caller, thunk 0x004024A5 */
    return a->user;
}

/* VA 0x00475c80, 44 bytes, [I] row, status draft */
void zactorGetRootPos(ZActor *a, RwV3d *out)
{
    /* (new function; no caller, thunk 0x0040132F) The actor position raised by the decoded rider
     * height (scaled for a scaled actor). */
    float h = a->anim->block[0].height;

    if (a->flags & ZACTOR_SCALED)
        h *= a->scale.y;
    out->x = a->pos.x;
    out->y = h + a->pos.y;
    out->z = a->pos.z;
}

/* VA 0x00475cc0, 276 bytes, [I] row, status draft */
void zactorGetSpinePoint(ZActor *a, RwV3d *out)
{
    const ZMotionState *pose = &a->anim->block[0];
    RwV3d v;

    v.x = 0.0f;
    v.y = -160.0f;
    v.z = 0.0f;
    zmathVecRotateZ(&v, &v, pose->rot[0][2]);
    zmathVecRotateY(&v, &v, pose->rot[0][1]);
    zmathVecRotateX(&v, &v, pose->rot[0][0]);
    zmathVecRotateZ(&v, &v, pose->rot[1][2]);
    zmathVecRotateY(&v, &v, pose->rot[1][1]);
    zmathVecRotateX(&v, &v, pose->rot[1][0]);
    zmathVecRotateY(&v, &v, -a->rot[1]);
    v.y += pose->height;
    if (a->flags & ZACTOR_SCALED) {
        v.x *= a->scale.x;
        v.y *= a->scale.y;
        v.z *= a->scale.z;
    }
    out->x = v.x + a->pos.x;
    out->y = v.y + a->pos.y;
    out->z = v.z + a->pos.z;
}

/* Position of `local` (flags & 1; else the origin) in the frame of `bone`, by forward kinematics over
 * the bone's path to the root (g_zactorBonePath / g_zactorBoneOffset, the pose's Euler angles).
 * flags & 2: use `local` as the result instead; flags & 4: stay in actor space (no actor rotation and
 * position). Used by the rag doll (FUN_00452B80). */
/* VA 0x00475e30, 499 bytes, [I] row, status draft */
void zactorGetBonePos(ZActor *a, int bone, const RwV3d *local, RwV3d *out, uint8_t flags)
{
    const ZMotionState *pose = &a->anim->block[0];
    const int8_t *path = &g_zactorBonePath[0][0][0] + (bone + a->skeleton * ZACTOR_MAX_CHANNELS) * 8;
    RwV3d v;

    if (flags & 1) {
        v = *local;
    } else {
        v.x = v.y = v.z = 0.0f;
    }
    for (; *path != -1; path++) {
        int b = *path;
        const float *o = &g_zactorBoneOffset[0][0][0] + (b + a->skeleton * ZACTOR_MAX_CHANNELS) * 4;

        zmathVecRotateX(&v, &v, pose->rot[b][0]);
        zmathVecRotateY(&v, &v, -pose->rot[b][1]);
        zmathVecRotateZ(&v, &v, pose->rot[b][2]);
        v.x += o[0];
        v.y += o[1];
        v.z += o[2];
    }
    v.y += pose->height;
    if (flags & 2)
        v = *local;
    if (a->flags & ZACTOR_SCALED) {
        v.x *= a->scale.x;
        v.y *= a->scale.y;
        v.z *= a->scale.z;
    }
    if (flags & 4) {
        *out = v;
        return;
    }
    zmathVecRotateX(&v, &v, a->rot[0]);
    zmathVecRotateY(&v, &v, -a->rot[1]);
    zmathVecRotateZ(&v, &v, a->rot[2]);
    out->x = v.x + a->pos.x;
    out->y = v.y + a->pos.y;
    out->z = v.z + a->pos.z;
}

/* VA 0x004760b0, 107 bytes, [V] direct, status draft */
void zactorTurnHead(ZActor *a, int16_t delta)
{
    /* (new function; no caller, thunk 0x00401FC3) Turns the head (channel 2 yaw) by delta relative
     * to the decoded pose: the first call saves the pose's yaw in headYaw and sets
     * ZACTOR_HEAD_OVERRIDE (the next decode restores it). */
    ZMotionState *pose;

    if (a->kind != ZACTOR_KIND_PERSON)
        zAssertFail("Not a person type", ZACTOR_FILE, 0xDF3, 0);
    pose = &a->anim->block[0];
    if (!(a->flags & ZACTOR_HEAD_OVERRIDE)) {
        pose->headYaw = pose->rot[2][1];
        pose->rot[2][1] += delta;
        a->flags |= ZACTOR_HEAD_OVERRIDE;
        return;
    }
    pose->rot[2][1] = (int16_t)(pose->headYaw + delta);
}

/* VA 0x00476140, 53 bytes, [I] row, status draft */
void zactorSetChannel(ZActor *a, int channel, const int16_t *euler)
{
    int16_t *r = a->anim->block[0].rot[channel & 0xFFFF];

    r[0] = euler[0];
    r[1] = euler[1];
    r[2] = euler[2];
}

/* VA 0x00476190, 53 bytes, [I] row, status draft */
void zactorGetChannel(ZActor *a, uint16_t channel, int16_t *euler)
{
    /* new function; no caller, thunk 0x004021A3 */
    const int16_t *r = a->anim->block[0].rot[channel];

    euler[0] = r[0];
    euler[1] = r[1];
    euler[2] = r[2];
}

/* VA 0x004761e0, 12 bytes, [I] row, status draft */
void zactorSetSkeleton(ZActor *a, int skeleton)
{
    a->skeleton = skeleton;
}

/* VA 0x00476200, 8 bytes, [I] row, status draft */
int zactorGetSkeleton(ZActor *a)
{
    /* new function; no caller, thunk 0x0040175D */
    return a->skeleton;
}

/* VA 0x00476220, 18 bytes, [I] row, status draft */
void zactorSetHeight(ZActor *a, float height)
{
    a->anim->block[0].height = height;
}

/* VA 0x00476250, 22 bytes, [I] row, status draft */
uint8_t zactorGetFrameCount(ZActor *a)
{
    /* (new function; no caller, thunk 0x0040227A): rider frames decoded since the last restart, 0
     * for other kinds */
    if (a->kind == ZACTOR_KIND_PERSON)
        return a->anim->block[0].frameCount;
    return 0;
}

/* VA 0x00476280, 14 bytes, [I] row, status draft */
float zactorGetHeight(ZActor *a)
{
    return a->anim->block[0].height;
}

/* VA 0x004762a0, 53 bytes, [I] row, status draft */
void zactorGetRotation(ZActor *a, int16_t *euler)
{
    /* (new function; no caller, thunk 0x004020BD): the Euler angles with the yaw back in game terms */
    int16_t yaw = (int16_t)(a->rot[1] - g_zactorHeading);

    if (g_zactorHeadingNeg)
        yaw = (int16_t)-yaw;
    euler[0] = a->rot[0];
    euler[1] = yaw;
    euler[2] = a->rot[2];
}

/* VA 0x004762f0, 49 bytes, [I] row, status draft */
void zactorGetRootMatrix(ZActor *a, RwMatrix *m)
{
    zmathMatrixIdentity(m);
    zmathMatrixRotateXYZ(m, m, a->anim->block[0].rot[0]);
    zmathMatrixRotateXYZ(m, m, a->rot);
}

/* VA 0x00476340, 71 bytes, [I] row, status draft */
void zactorSetRotation(ZActor *a, int16_t *euler)
{
    /* (new function; no caller, thunk 0x00401C3F) The inverse; like the exe it converts the caller's
     * yaw in place. */
    if (g_zactorHeadingNeg)
        euler[1] = (int16_t)-euler[1];
    euler[1] = (int16_t)(euler[1] + g_zactorHeading);
    a->rot[0] = euler[0];
    a->rot[1] = euler[1];
    a->rot[2] = euler[2];
    a->heading = euler[1];
}

/* VA 0x004763b0, 69 bytes, [I] row, status draft */
void zactorSetMatrix(ZActor *a, const RwMatrix *m)
{
    int16_t zero[3];
    RwMatrix copy;

    zmathMatrixCopy(&copy, m);
    zero[0] = zero[1] = zero[2] = 0;
    zactorSetOrientation(a, zero, &copy.up, &copy.right, &copy);
}

/* Build an orthonormal orientation from an up and a right vector (up is negated: engine Y is down),
 * apply `euler`, take the actor's angles from it (zactorSetRotFromMatrix, with the global heading
 * added for that step) and return it in `out` with its up row flipped back. */
/* VA 0x00476420, 657 bytes, [I] row, status draft */
void zactorSetOrientation(ZActor *a, const int16_t *euler, const RwV3d *up, const RwV3d *right, RwMatrix *out)
{
    RwV3d r, u, at, origin;
    RwMatrix rotY, e;
    int angle = 0;

    u.x = -up->x;
    u.y = -up->y;
    u.z = -up->z;
    r = *right;
    origin.x = origin.y = origin.z = 0.0f;
    if (fabsf(zmathVecDot(&u, &r)) >= 0.01f) {
        /* not perpendicular: rebuild right from up and at */
        zmathTriangleNormal(&at, &origin, &r, &u);
        zmathTriangleNormal(&r, &origin, &u, &at);
        zmathVecNormalize(&r, &r);
        zmathVecNormalize(&at, &at);
    } else {
        zmathTriangleNormal(&at, &origin, &r, &u);
        if (fabsf(zmathVecDot(&r, &r) - 1.0f) > 0.01f)
            zmathVecNormalize(&r, &r);
        zmathVecNormalize(&at, &at);
    }
    zmathMatrixIdentity(&rotY);
    if (g_zactorHeading != 0) {
        angle = g_zactorHeadingNeg ? g_zactorHeading : -g_zactorHeading;
        zmathMatrixRotateY(&rotY, &rotY, angle);
    }
    zmathMatrixIdentity(&e);
    zmathMatrixRotateXYZ(&e, &e, euler);
    zmathMatrixIdentity(out);
    out->right = r;
    out->up = u;
    out->at = at;
    zmathMatrixMultiply(out, out, &e);
    if (g_zactorHeading != 0)
        zmathMatrixMultiply(out, out, &rotY);
    zactorSetRotFromMatrix(a, out);
    if (g_zactorHeading != 0) {
        zmathMatrixIdentity(&rotY);
        zmathMatrixRotateY(&rotY, &rotY, -angle);
        zmathMatrixMultiply(out, out, &rotY);
    }
    out->up.x = -out->up.x;
    out->up.y = -out->up.y;
    out->up.z = -out->up.z;
    zmathVecNormalize(&out->up, &out->up);
}

/* VA 0x00476770, 201 bytes, [I] row, status draft */
void zactorSetRotFromMatrix(ZActor *a, const RwMatrix *m)
{
    float xy = sqrtf(m->right.y * m->right.y + m->right.x * m->right.x);

    if (xy <= 0.001f) {                     /* gimbal lock: no z rotation */
        a->rot[0] = zmathAtan2(-m->at.y, m->up.y);
        a->rot[1] = zmathAtan2(-m->right.z, xy);
        a->rot[2] = 0;
    } else {
        a->rot[0] = zmathAtan2(m->up.z, m->at.z);
        a->rot[1] = zmathAtan2(-m->right.z, xy);
        a->rot[2] = zmathAtan2(m->right.y, m->right.x);
    }
    a->heading = zmathAtan2(-m->right.z, m->right.x);
}

/* The bike's channel 0 (Hipsdeck) takes the rider clip's bike-section root angles. */
/* VA 0x00476880, 71 bytes, [I] row, status draft */
void zactorCopyRootAngles(ZActor *bike, ZActor *rider)
{
    bike->anim->block[0].rot[0][0] = rider->anim->block[0].bikeRoot[0];
    bike->anim->block[0].rot[0][1] = rider->anim->block[0].bikeRoot[1];
    bike->anim->block[0].rot[0][2] = rider->anim->block[0].bikeRoot[2];
}

/* VA 0x004768f0, 41 bytes, [I] row, status draft */
void zactorCopyRotation(ZActor *dst, const ZActor *src)
{
    dst->rot[0] = src->rot[0];
    dst->rot[1] = src->rot[1];
    dst->rot[2] = src->rot[2];
    dst->heading = src->heading;
}

/* out = a * w + b * (1 - w) per angle, no wrap handling. */
/* VA 0x00476930, 175 bytes, [I] row, status draft */
void zactorLerpAngles(const int16_t *a, const int16_t *b, int16_t *out, float w, uint8_t channels)
{
    float v = 1.0f - w;
    int i;

    do {
        for (i = 0; i < 3; i++)
            out[i] = (int16_t)FTOL(a[i] * w + b[i] * v);
        a += 3;
        b += 3;
        out += 3;
    } while (--channels != 0);
}

/* Sets every channel weight to 1.0 and rewrites `angles` with themselves (x unscaled, y and z times
 * that weight); `other` is never read. */
/* VA 0x00476a10, 148 bytes, [I] row, status draft */
void zactorResetWeights(int16_t *angles, const int16_t *other, float *weights, uint8_t channels)
{
    uint8_t i = 0;

    (void)other;
    do {
        weights[i] = 1.0f;
        angles[0] = (int16_t)FTOL((float)angles[0]);
        angles[1] = (int16_t)FTOL(angles[1] * weights[i]);
        angles[2] = (int16_t)FTOL(angles[2] * weights[i]);
        i++;
        angles += 3;
    } while (--channels != 0);
}

/* Two clips at once (slots 2 and 3), mixed by `w` into slot 0, then the same crossfade and output
 * as zactorPlayMotion. flags[0] / flags[1] are the play flags of A / B. Returns g_zactorBlendFrames. */
/* VA 0x00476ae0, 1445 bytes, [I] row, status draft */
ZactorFrame **zactorPlayMotionBlend(ZActor *a, const ZMotionHeader *motionA, const ZMotionHeader *motionB,
                                    const uint16_t *flags, float w)
{
    ZActorAnim *an = a->anim;
    ZMotionState *out = &an->block[0];
    ZMotionState *cur = &an->block[1];
    ZMotionState *prev = &an->block[2];
    ZMotionState *sa = &an->block[3];
    ZMotionState *sb = &an->block[4];
    float v;
    int i;
    uint8_t k;

    a->flags |= ZACTOR_MOTION_POSED;
    sa->motion = motionA;
    sb->motion = motionB;
    g_zactorBlendFrames[1] = zactorDecodeMotion(a, 1.0f, flags[1], &g_zactorBlendFrameB, 3);
    g_zactorBlendFrames[0] = zactorDecodeMotion(a, 1.0f, flags[0], &g_zactorBlendFrameA, 2);
    zactorLerpAngles(sa->rot[0], sb->rot[0], cur->rot[0], w, out->numChannels);
    v = 1.0f - w;
    cur->height = v * sb->height + w * sa->height;
    for (i = 0; i < 3; i++) {
        cur->bikeTrans[i] = v * sb->bikeTrans[i] + w * sa->bikeTrans[i];
        cur->bikeRoot[i] = (int16_t)FTOL(sb->bikeRoot[i] * v + sa->bikeRoot[i] * w);
    }
    if (flags[0] & ZMOTION_RESTART) {
        if (flags[0] & ZMOTION_CUT) {
            prev->motion = 0;
            an->fade = 0;
        } else if (prev->motion == 0) {
            an->fade = 0;
        } else {
            for (k = 0; k < cur->numChannels * 3; k++)
                (&prev->rot[0][0])[k] = (&out->rot[0][0])[k];
            prev->bikeTrans[0] = out->bikeTrans[0];
            prev->bikeTrans[1] = out->bikeTrans[1];
            prev->bikeTrans[2] = out->bikeTrans[2];
            prev->height = out->height;
            g_zactorBlendFrameB.yaw = g_zactorBlendFrameA.yaw;
            an->fade = 3;
        }
        out->numChannels = cur->numChannels;
        out->numSections = cur->numSections;
    }
    switch (an->fade) {
    case 0:
        zstringMemCopy(out->rot, cur->rot, out->numChannels * 6);
        out->bikeTrans[0] = cur->bikeTrans[0];
        out->bikeTrans[1] = cur->bikeTrans[1];
        out->bikeTrans[2] = cur->bikeTrans[2];
        out->height = cur->height;
        break;
    case 1:
        out->height = (cur->height * 3.0f + prev->height) * 0.25f;
        zactorAngleMix25(prev->rot[0], cur->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix25(prev->bikeTrans, cur->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 2:
        out->height = (prev->height + cur->height) * 0.5f;
        zactorAngleMix50(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix50(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 3:
        out->height = (prev->height * 3.0f + cur->height) * 0.25f;
        zactorAngleMix25(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix25(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 4:
        zstringMemCopy(out->rot, prev->rot, out->numChannels * 6);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            out->bikeTrans[0] = prev->bikeTrans[0];
            out->bikeTrans[1] = prev->bikeTrans[1];
            out->bikeTrans[2] = prev->bikeTrans[2];
        } else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        out->height = prev->height;
        break;
    }
    if (an->fade != 0)
        an->fade--;
    out->frameCount = cur->frameCount;
    out->block = cur->block;
    if (!(flags[0] & ZMOTION_CUT))
        prev->motion = motionA;
    out->motion = motionA;
    out->bikeTrans[0] = cur->bikeTrans[0];
    out->bikeTrans[1] = cur->bikeTrans[1];
    out->bikeTrans[2] = cur->bikeTrans[2];
    out->bikeRoot[0] = cur->bikeRoot[0];
    out->bikeRoot[1] = cur->bikeRoot[1];
    out->bikeRoot[2] = cur->bikeRoot[2];
    return g_zactorBlendFrames;
}

/* The second clip of the rider (physics, crash state): decode into slot 2 and reset the channel
 * weights. Returns its frame. */
/* VA 0x00477210, 99 bytes, [I] row, status draft */
ZactorFrame *zactorPlaySecondMotion(ZActor *a, const ZMotionHeader *motion, uint32_t flags, float *weights)
{
    ZActorAnim *an = a->anim;
    ZactorFrame frame;          /* the decoder's output buffer is a local here */

    an->block[3].motion = motion;
    g_zactorBlendFrames[0] = zactorDecodeMotion(a, 1.0f, flags & 0xFFFF, &frame, 2);
    zactorResetWeights(an->block[1].rot[0], an->block[3].rot[0], weights, an->block[0].numChannels);
    return g_zactorBlendFrames[0];   /* [V] points at the dead local (or NULL) */
}

/* out[i] = mask[i] ? b[i] : a[i] per channel. */
/* VA 0x004772a0, 101 bytes, [I] row, status draft */
void zactorSelectAngles(const int16_t *a, const int16_t *b, int16_t *out, const uint8_t *mask, uint32_t channels)
{
    int i;

    for (i = 0; i < (int)(channels & 0xFF); i++) {
        const int16_t *src = mask[i] ? b : a;

        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        a += 3;
        b += 3;
        out += 3;
    }
}

/* Like zactorPlayMotionBlend, but each channel comes from A or B by `mask`; height and bike from A.
 * Unreferenced. */
/* VA 0x00477330, 1325 bytes, [I] row, status draft */
ZactorFrame **zactorPlayMotionMasked(ZActor *a, const ZMotionHeader *motionA, const ZMotionHeader *motionB,
                                     const uint16_t *flags, const uint8_t *mask)
{
    ZActorAnim *an = a->anim;
    ZMotionState *out = &an->block[0];
    ZMotionState *cur = &an->block[1];
    ZMotionState *prev = &an->block[2];
    ZMotionState *sa = &an->block[3];
    ZMotionState *sb = &an->block[4];
    int i;
    uint8_t k;

    a->flags |= ZACTOR_MOTION_POSED;
    sa->motion = motionA;
    sb->motion = motionB;
    g_zactorBlendFrames[1] = zactorDecodeMotion(a, 1.0f, flags[1], &g_zactorMaskFrameB, 3);
    g_zactorBlendFrames[0] = zactorDecodeMotion(a, 1.0f, flags[0], &g_zactorMaskFrameA, 2);
    zactorSelectAngles(sa->rot[0], sb->rot[0], cur->rot[0], mask, out->numChannels);
    cur->height = sa->height;
    for (i = 0; i < 3; i++) {
        cur->bikeTrans[i] = sa->bikeTrans[i];
        cur->bikeRoot[i] = sa->bikeRoot[i];
    }
    if (flags[0] & ZMOTION_RESTART) {
        if (flags[0] & ZMOTION_CUT) {
            prev->motion = 0;
            an->fade = 0;
        } else if (prev->motion == 0) {
            an->fade = 0;
        } else {
            for (k = 0; k < cur->numChannels * 3; k++)
                (&prev->rot[0][0])[k] = (&out->rot[0][0])[k];
            prev->bikeTrans[0] = out->bikeTrans[0];
            prev->bikeTrans[1] = out->bikeTrans[1];
            prev->bikeTrans[2] = out->bikeTrans[2];
            prev->height = out->height;
            g_zactorMaskFrameB.yaw = g_zactorMaskFrameA.yaw;
            an->fade = 3;
        }
        out->numChannels = cur->numChannels;
        out->numSections = cur->numSections;
    }
    switch (an->fade) {     /* as in zactorPlayMotionBlend */
    case 0:
        zstringMemCopy(out->rot, cur->rot, out->numChannels * 6);
        out->bikeTrans[0] = cur->bikeTrans[0];
        out->bikeTrans[1] = cur->bikeTrans[1];
        out->bikeTrans[2] = cur->bikeTrans[2];
        out->height = cur->height;
        break;
    case 1:
        out->height = (cur->height * 3.0f + prev->height) * 0.25f;
        zactorAngleMix25(prev->rot[0], cur->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix25(prev->bikeTrans, cur->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 2:
        out->height = (prev->height + cur->height) * 0.5f;
        zactorAngleMix50(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix50(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 3:
        out->height = (prev->height * 3.0f + cur->height) * 0.25f;
        zactorAngleMix25(cur->rot[0], prev->rot[0], out->rot[0], out->numChannels);
        if (!zactorVecIsZero(prev->bikeTrans))
            zactorVecMix25(cur->bikeTrans, prev->bikeTrans, out->bikeTrans);
        else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        break;
    case 4:
        zstringMemCopy(out->rot, prev->rot, out->numChannels * 6);
        if (!zactorVecIsZero(prev->bikeTrans)) {
            out->bikeTrans[0] = prev->bikeTrans[0];
            out->bikeTrans[1] = prev->bikeTrans[1];
            out->bikeTrans[2] = prev->bikeTrans[2];
        } else {
            out->bikeTrans[0] = cur->bikeTrans[0];
            out->bikeTrans[1] = cur->bikeTrans[1];
            out->bikeTrans[2] = cur->bikeTrans[2];
        }
        out->height = prev->height;
        break;
    }
    if (an->fade != 0)
        an->fade--;
    out->frameCount = cur->frameCount;
    out->block = cur->block;
    if (!(flags[0] & ZMOTION_CUT))
        prev->motion = motionA;
    out->motion = motionA;
    out->bikeTrans[0] = cur->bikeTrans[0];
    out->bikeTrans[1] = cur->bikeTrans[1];
    out->bikeTrans[2] = cur->bikeTrans[2];
    out->bikeRoot[0] = cur->bikeRoot[0];
    out->bikeRoot[1] = cur->bikeRoot[1];
    out->bikeRoot[2] = cur->bikeRoot[2];
    return g_zactorBlendFrames;
}

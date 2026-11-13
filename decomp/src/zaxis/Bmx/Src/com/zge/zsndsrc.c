/*
 * zaxis/Bmx/Src/com/zge/zsndsrc.c
 * Original: C:\zaxis\Bmx\src\com\zge\zsndsrc.c
 * Evidence: [V] path string 0x0055FF50 (zAssertFail "already initialized", "not initialized",
 *           "invalid handle", "source not allocated") in 9 functions. [I] for the rest:
 *           - the twelve zsndMusic* wrappers 0x0047DE20-0x0047E09D before the first [V] function
 *             have no asserts; their globals (0x0064FCB8/BC) sit right before zsndsrc's
 *             (0x0064FCC8-0x006500A3), so they are taken as the head of this file (they could also
 *             be a small file of their own between zsndseq.c and zsndsrc.c);
 *           - zsndSetUpdateRate / zsndStub (0x0047EBA0 / 0x0047EBC0) follow the last [V] function
 *             and own g_zsndUpdateRate (0x0055FFC0), which sits right after this file's literals and
 *             before zdebug.c's.
 * 26 functions, VA 0x0047DE20-0x0047EBC2. Draft. Non-matching C (AD-8).
 * Types: zsound.h. Description: docs/re/sound.md section 5.
 *
 * 3-D sound sources ("zsndSrc"): 24 positional sounds. Each update (game frame) a source whose
 * life has not run out gets a channel if it is within maxDist of the listener (the camera), a
 * level 127 x (1 - d^2 / maxDist^2), a pan from its sideways offset, and a Doppler pitch from the
 * change of d^2 (always 0: g_zsndSrcDopplerScale = 0 / maxDist). zsndSrcSetPosition re-arms the
 * life (lifeMs, 100 ms = 3 frames), so a source the game stops moving goes quiet by itself.
 * bmxsound uses sources for the rolling / freewheel loops of each rider and the level's props.
 * The sound pass created all twelve zsndMusic* wrappers, zsndSrcGetChannel, zsndSetUpdateRate and
 * zsndStub (ILT thunks only; the game streams its music through winStrm.c directly).
 */
#include "decomp.h"
#include "zmath.h"
#include "zsound.h"

#define ZSNDSRC_FILE "C:\\zaxis\\Bmx\\src\\com\\zge\\zsndsrc.c"
#define SRC_CHECK(h, l1, l2)                                                       \
    do {                                                                           \
        if (!g_zsndSrcInitialized)                                                 \
            zAssertFail("not initialized", ZSNDSRC_FILE, (l1), 0);                 \
        if ((h) == 0xFFFF)                                                         \
            zAssertFail("invalid handle", ZSNDSRC_FILE, (l2), 0);                  \
    } while (0)

/* NgIF (0x0045A340, not named yet): pos relative to the camera (zobjCameraGetMatrix of the camera
 * object 0x0068FF98, then zmathTransformPoint) */
void ngappWorldToCamera(const RwV3d *world, RwV3d *out);

/* ------------------------------------------------------------------ globals */
uint32_t    g_zsndUpdateRate = 30;       /* 0x0055FFC0 game frames per second (bmxsoundOpen: 30) */
ZsndStream *g_zsndMusic;                 /* 0x0064FCB8 */
uint32_t    g_zsndMusicBufBytes;         /* 0x0064FCBC */
int32_t     g_zsndSrcNoDoppler;          /* 0x0064FCC8 set by zsndSrcResetDoppler, cleared by the update */
uint32_t    g_zsndSrcMaxDist;            /* 0x0064FCCC */
ZsndSrc     g_zsndSrcs[ZSND_MAX_SOURCES];/* 0x0064FCD0 */
float       g_zsndSrcDopplerScale;       /* 0x00650090 0.0 / maxDist */
uint32_t    g_zsndSrcMaxDistSq;          /* 0x00650094 */
int32_t     g_zsndSrcInitialized;        /* 0x00650098 */
uint32_t    g_zsndSrcNext;               /* 0x0065009C */
uint32_t    g_zsndSrcLife;               /* 0x006500A0 updates */

/* ------------------------------------------------------------------ the music stream wrappers */

/* VA 0x0047de20, 20 bytes, [I] row, status draft */
void zsndMusicInit(uint32_t bufBytes)
{
    if (zsndStreamInit() == 0)
        g_zsndMusicBufBytes = bufBytes;
}

/* VA 0x0047de50, 36 bytes, [I] row, status draft */
int zsndMusicShutdown(void)
{
    if (g_zsndMusic) {
        zsndStreamDestroy(g_zsndMusic);
        g_zsndMusic = 0;
    }
    zsndStreamShutdown();
    return 0;
}

/* VA 0x0047de90, 3 bytes, [I] row, status draft */
int zsndMusicUpdate(void)
{
    return 0;
}

/* VA 0x0047deb0, 107 bytes, [I] row, status draft
 * flags: bit 0 11025 Hz, bit 1 22050 Hz (else 44100), bit 2 mono (else stereo). */
int zsndMusicPlay(const ZsndFileDesc *file, uint8_t flags, uint32_t loops)
{
    uint32_t sf;
    int      rc;

    if (g_zsndMusic)
        zsndStreamDestroy(g_zsndMusic);      /* [V] the pointer is not cleared */
    sf = ZSND_STRM_44K;
    if (flags & 2)
        sf = 0;
    else if (flags & 1)
        sf = ZSND_STRM_11K;
    if (!(flags & 4))
        sf |= ZSND_STRM_STEREO;
    rc = zsndStreamCreate(&g_zsndMusic, file, 0, g_zsndMusicBufBytes, sf);
    if (rc == 0) {
        zsndStreamSetLoops(g_zsndMusic, loops);
        rc = 0;
    }
    return rc;
}

/* VA 0x0047df40, 27 bytes, [I] row, status draft */
int zsndMusicGetState(uint32_t *state)
{
    return g_zsndMusic ? zsndStreamGetState(g_zsndMusic, state) : 0;
}

/* VA 0x0047df70, 22 bytes, [I] row, status draft */
int zsndMusicResume(void)
{
    return g_zsndMusic ? zsndStreamPlay(g_zsndMusic) : 0;
}

/* VA 0x0047dfa0, 22 bytes, [I] row, status draft */
int zsndMusicStop(void)
{
    return g_zsndMusic ? zsndStreamStop(g_zsndMusic) : 0;
}

/* VA 0x0047dfd0, 22 bytes, [I] row, status draft
 * [V] the same call as zsndMusicStop: pausing the music stops it. */
int zsndMusicPause(void)
{
    return g_zsndMusic ? zsndStreamStop(g_zsndMusic) : 0;
}

/* VA 0x0047e000, 32 bytes, [I] row, status draft */
int zsndMusicSetVolume(uint8_t volume, int8_t pan)
{
    return g_zsndMusic ? zsndStreamSetVolume(g_zsndMusic, volume, pan) : 0;
}

/* VA 0x0047e030, 37 bytes, [I] row, status draft */
int zsndMusicFade(uint32_t ms, uint8_t target, uint32_t flags)
{
    return g_zsndMusic ? zsndStreamFade(g_zsndMusic, ms, target, flags) : 0;
}

/* VA 0x0047e070, 14 bytes, [I] row, status draft */
void zsndMusicHold(int hold)
{
    zsndStreamHold(hold);
}

/* VA 0x0047e090, 14 bytes, [I] row, status draft */
void zsndMusicGetCaps(uint32_t *caps)
{
    zsndStreamGetCaps(caps);
}

/* ------------------------------------------------------------------ 3-D sources */

/* VA 0x0047e0b0, 155 bytes, [V] direct, status draft */
int zsndSrcInit(uint32_t maxDist, uint32_t lifeMs)
{
    int i;

    if (g_zsndSrcInitialized)
        zAssertFail("already initialized", ZSNDSRC_FILE, 0x52, 0);
    for (i = 0; i < ZSND_MAX_SOURCES; i++)
        g_zsndSrcs[i].flags = 0;
    g_zsndSrcMaxDist = maxDist;
    g_zsndSrcLife = g_zsndUpdateRate * lifeMs / 1000;
    g_zsndSrcMaxDistSq = maxDist * maxDist;
    g_zsndSrcNoDoppler = 0;
    g_zsndSrcInitialized = 1;
    g_zsndSrcDopplerScale = 0.0f / (float)maxDist;     /* [V] a zero numerator: no Doppler */
    return 0;
}

/* VA 0x0047e180, 90 bytes, [V] direct, status draft */
void zsndSrcShutdown(void)
{
    int i;

    if (!g_zsndSrcInitialized)
        zAssertFail("not initialized", ZSNDSRC_FILE, 0x6B, 0);
    for (i = 0; i < ZSND_MAX_SOURCES; i++)
        if (g_zsndSrcs[i].flags & 0x8000)
            zsndSrcFree((uint16_t)i);
    g_zsndSrcNext = 0;
    g_zsndSrcInitialized = 0;
}

static uint32_t distSq(const RwV3d *v)
{
    int32_t x = (int32_t)v->x, y = (int32_t)v->y, z = (int32_t)v->z;
    return (uint32_t)(z * z + y * y + x * x);
}

/* VA 0x0047e200, 543 bytes, [I] row, status draft
 * Once per game frame. Returns 2 if a source in range found no channel. */
int zsndSrcUpdate(void)
{
    RwV3d    rel;
    uint32_t d2;
    int32_t  x, doppler;
    int      rc = 0, i;

    if (!g_zsndSrcInitialized)
        return 0;
    for (i = 0; i < ZSND_MAX_SOURCES; i++) {
        ZsndSrc *s = &g_zsndSrcs[i];
        if (!(s->flags & 0x8000) || s->life == 0)
            continue;
        if (--s->life == 0) {                 /* not refreshed: silence it */
            if (s->channel != 0xFF) {
                zsndcFree(s->channel);
                s->channel = 0xFF;
            }
            continue;
        }
        if (s->channel != 0xFF && zsndcGetVoice(s->channel) != s->voice) {
            zsndcFree(s->channel);            /* the voice changed (or the channel was stolen) */
            s->channel = 0xFF;
        }
        ngappWorldToCamera(&s->pos, &rel);
        x = (int32_t)rel.x;
        d2 = distSq(&rel);
        if (d2 >= g_zsndSrcMaxDistSq) {       /* out of range */
            s->level = 0;
            if (s->channel != 0xFF) {
                zsndcFree(s->channel);
                s->channel = 0xFF;
            }
            s->distSq = d2;
            continue;
        }
        if (s->channel == 0xFF) {
            s->channel = zsndcAlloc(s->voice, 0xFE);
            if (s->channel == 0xFF) {
                rc = 2;
                continue;
            }
            zsndcKeyOn(s->channel);
        }
        s->level = (uint8_t)(127 - (d2 >> 7) * 127 / (g_zsndSrcMaxDistSq >> 7));
        zsndcSetVolume(s->channel, (uint8_t)(s->volume * s->level >> 7));
        s->pan = x ? (int8_t)(x * 127 / (int32_t)g_zsndSrcMaxDist) : 0;
        zsndcSetPan(s->channel, s->pan);
        if (!g_zsndSrcNoDoppler) {
            doppler = -(int32_t)((float)(int32_t)(d2 - s->distSq) * g_zsndSrcDopplerScale);
            if ((int16_t)doppler < -700)
                s->doppler = -700;
            else if ((int16_t)doppler >= 700)
                s->doppler = 700;
            else
                s->doppler = (int16_t)doppler;
        } else {
            s->doppler = 0;
        }
        zsndcSetPitch(s->channel, (int16_t)(s->pitch + s->doppler));
        s->distSq = d2;
    }
    g_zsndSrcNoDoppler = 0;
    return rc;
}

/* VA 0x0047e4b0, 67 bytes, [I] row, status draft
 * Skip the Doppler shift for one update (after a camera cut) and drop the current one. */
void zsndSrcResetDoppler(void)
{
    int i;

    g_zsndSrcNoDoppler = 1;
    for (i = 0; i < ZSND_MAX_SOURCES; i++) {
        ZsndSrc *s = &g_zsndSrcs[i];
        if ((s->flags & 0x8000) && s->life != 0 && s->doppler != 0)
            zsndSrcSetPitch((uint16_t)i, s->pitch);
    }
}

/* VA 0x0047e520, 240 bytes, [V] direct, status draft
 * [V] only the slot g_zsndSrcNext is tried: if it is in use the loop gives up after one step
 * (`cmp esi, eax; jne fail`), so creation fails whenever the next slot in turn is busy. */
uint16_t zsndSrcCreate(ZsndVoice *voice, const RwV3d *pos)
{
    ZsndSrc *s;
    uint32_t i;

    if (!g_zsndSrcInitialized)
        zAssertFail("not initialized", ZSNDSRC_FILE, 0xEF, 0);
    i = g_zsndSrcNext;
    if (g_zsndSrcs[i].flags & 0x8000) {
        if (++i == ZSND_MAX_SOURCES)
            i = 0;
        if (i != g_zsndSrcNext)
            return 0xFFFF;
    }
    s = &g_zsndSrcs[i];
    s->flags = 0x8000;
    g_zsndSrcNext = i + 1 == ZSND_MAX_SOURCES ? 0 : i + 1;
    s->pitch = 0;
    s->channel = 0xFF;
    s->voice = voice;
    s->volume = 0x7F;
    s->distSq = distSq(pos);                  /* world, not listener, distance */
    zsndSrcSetPosition((uint16_t)i, pos);
    s->life = 0;                              /* silent until the first zsndSrcSetPosition */
    return (uint16_t)i;
}

/* VA 0x0047e650, 149 bytes, [V] direct, status draft */
void zsndSrcFree(uint16_t hSrc)
{
    SRC_CHECK(hSrc, 0x117, 0x118);
    if (!(g_zsndSrcs[hSrc].flags & 0x8000))
        zAssertFail("source not allocated", ZSNDSRC_FILE, 0x11A, 0);
    if (g_zsndSrcs[hSrc].channel != 0xFF) {
        zsndcFree(g_zsndSrcs[hSrc].channel);
        g_zsndSrcs[hSrc].channel = 0xFF;
    }
    g_zsndSrcs[hSrc].flags = 0;
}

/* VA 0x0047e720, 144 bytes, [V] direct, status draft */
int zsndSrcSetVoice(uint16_t hSrc, ZsndVoice *voice)
{
    SRC_CHECK(hSrc, 0x12C, 0x12D);
    if (!(g_zsndSrcs[hSrc].flags & 0x8000))
        zAssertFail("source not allocated", ZSNDSRC_FILE, 0x12F, 0);
    g_zsndSrcs[hSrc].voice = voice;
    return 0;
}

/* VA 0x0047e7e0, 121 bytes, [V] direct, status draft */
int zsndSrcSetPosition(uint16_t hSrc, const RwV3d *pos)
{
    SRC_CHECK(hSrc, 0x13F, 0x140);
    g_zsndSrcs[hSrc].pos = *pos;
    g_zsndSrcs[hSrc].life = g_zsndSrcLife;
    return 0;
}

/* VA 0x0047e880, 97 bytes, [V] direct, status draft */
int zsndSrcSetPitch(uint16_t hSrc, int16_t cents)
{
    SRC_CHECK(hSrc, 0x159, 0x15A);
    g_zsndSrcs[hSrc].pitch = cents;
    return 0;
}

/* VA 0x0047e910, 95 bytes, [V] direct, status draft */
int zsndSrcSetVolume(uint16_t hSrc, uint8_t volume)
{
    SRC_CHECK(hSrc, 0x16D, 0x16E);
    g_zsndSrcs[hSrc].volume = volume;
    return 0;
}

/* VA 0x0047e990, 89 bytes, [I] row, status draft */
uint8_t zsndSrcGetChannel(uint16_t hSrc)
{
    SRC_CHECK(hSrc, 0x17E, 0x17F);
    return g_zsndSrcs[hSrc].channel;
}

/* VA 0x0047ea10, 320 bytes, [V] direct, status draft
 * Level and pan of a one-shot at `pos` (bmxsoundPlace). Returns 1 if out of range. */
int zsndSrcGetLevelPan(const RwV3d *pos, uint8_t *level, int8_t *pan)
{
    RwV3d    rel;
    uint32_t d2;

    if (!g_zsndSrcInitialized)
        zAssertFail("not initialized", ZSNDSRC_FILE, 0x190, 0);
    ngappWorldToCamera(pos, &rel);
    d2 = distSq(&rel);
    if (d2 >= g_zsndSrcMaxDistSq) {
        *level = 0;
        *pan = 0;
        return 1;
    }
    *level = (uint8_t)(127 - d2 * 127 / g_zsndSrcMaxDistSq);   /* [V] no >> 7 here: overflows */
    if (rel.x == 0.0f || rel.z == 0.0f) {
        *pan = 0;
        return 0;
    }
    if (rel.y != 0.0f)                       /* bring the offset into the horizontal plane */
        zmathVecRotateX(&rel, &rel, -zmathAtan2(rel.y, rel.z));
    *pan = (int8_t)(int32_t)(rel.x * 127.0f / (int32_t)g_zsndSrcMaxDist);
    return 0;
}

/* VA 0x0047eba0, 10 bytes, [I] row, status draft */
void zsndSetUpdateRate(uint32_t hz)
{
    g_zsndUpdateRate = hz;
}

/* VA 0x0047ebc0, 3 bytes, [I] row, status draft */
int zsndStub(void)
{
    return 0;
}

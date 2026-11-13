/*
 * zaxis/Bmx/Src/Pc/zge/sound/winChan.c
 * Original: C:\zaxis\Bmx\Src\Pc\zge\sound\winChan.c
 * Evidence: [V] path string 0x0055EEA8 (CRT assert()s: "init->hWnd", "fzsndcInitialized",
 *           "hChan < kzsndNumChannelHandles", ...) in 12 functions; [I] the rest: between them,
 *           and they share the module's data 0x005FB760-0x005FFF2B and tables 0x0055E820-0x0055ED7F.
 * 21 functions, VA 0x0046D940-0x0046FA0A. Draft. Non-matching C (AD-8).
 * Types: zsound.h. Description: docs/re/sound.md section 2.
 *
 * The channel manager ("zsndc") is a software mixer: DirectSound (DSOUND.DLL, loaded with
 * LoadLibrary) only gets one looping 16-bit stereo secondary buffer of two blocks (2 x 50 ms).
 *  - 64 channel handles map onto 24 mixer channels; a new sound steals the channel with the lowest
 *    priority if that is <= its own (zsndcAlloc).
 *  - Each channel plays one voice of a resident bank: 16-bit (or 8-bit) mono (or stereo) PCM from
 *    memory, 20.12 fixed-point resampling without interpolation, gains from a 128-entry table by
 *    volume and pan, and a four-stage envelope (attack / decay / sustain / release) whose times
 *    come from the voice bytes +6..+9 through g_zsndcEnvTimeMs.
 *  - Loops always run to the end of the sample file: ZsndSample.loopEnd is never read.
 *  - After the dry mix: a chorus (1024-frame line, raised-cosine LFO) and an echo line, both off
 *    unless zsndcSetEffect turns them on; bmxsound only ever asks for effect type 2 (PSX reverb
 *    parameters), which this file rejects with 0x10D, and gives the echo line no room
 *    (ZsndcInitParams.echoSeconds = 0), so the PC build mixes dry.
 * zsndcUpdate runs from the 16 ms multimedia timer (bmxsoundService), mixes the time that has
 * passed into a ring buffer and copies one block ahead of the play cursor into DirectSound.
 * The sound pass created zsndcFreeAll, zsndcGetState and zsndcProbe (only reached through ILT thunks).
 */
#include <math.h>
#include <stdlib.h>
#include <windows.h>

#include "decomp.h"
#include "zsound.h"

#define WINCHAN_FILE "C:\\zaxis\\Bmx\\src\\Pc\\zge\\sound\\winChan.c"
#define ASSERT_AT(cond, text, line) do { if (!(cond)) _assert((text), WINCHAN_FILE, (line)); } while (0)

/* IDirectSound / IDirectSoundBuffer vtable slots used here (dsound.h order) */
#define DS_CreateSoundBuffer(ds, d, pb)        ((*(int (__stdcall ***)(void *, const void *, void **, void *))(ds))[3]((ds), (d), (pb), 0))
#define DS_SetCooperativeLevel(ds, w, lvl)     ((*(int (__stdcall ***)(void *, void *, uint32_t))(ds))[6]((ds), (w), (lvl)))
#define COM_Release(p)                         ((*(int (__stdcall ***)(void *))(p))[2]((p)))
#define DSB_GetCurrentPosition(b, pp, pw)      ((*(int (__stdcall ***)(void *, uint32_t *, uint32_t *))(b))[4]((b), (pp), (pw)))
#define DSB_GetStatus(b, ps)                   ((*(int (__stdcall ***)(void *, uint32_t *))(b))[9]((b), (ps)))
#define DSB_Lock(b, o, n, p1, n1, p2, n2, f)   ((*(int (__stdcall ***)(void *, uint32_t, uint32_t, void **, uint32_t *, void **, uint32_t *, uint32_t))(b))[11]((b), (o), (n), (p1), (n1), (p2), (n2), (f)))
#define DSB_Play(b, r1, r2, f)                 ((*(int (__stdcall ***)(void *, uint32_t, uint32_t, uint32_t))(b))[12]((b), (r1), (r2), (f)))
#define DSB_SetFormat(b, w)                    ((*(int (__stdcall ***)(void *, const void *))(b))[14]((b), (w)))
#define DSB_Stop(b)                            ((*(int (__stdcall ***)(void *))(b))[18]((b)))
#define DSB_Unlock(b, p1, n1, p2, n2)          ((*(int (__stdcall ***)(void *, void *, uint32_t, void *, uint32_t))(b))[19]((b), (p1), (n1), (p2), (n2)))
#define DSB_Restore(b)                         ((*(int (__stdcall ***)(void *))(b))[20]((b)))
#define DSERR_BUFFERLOST       ((int)0x88780096)
#define DSSCL_NORMAL           1
#define DSSCL_PRIORITY         2
#define DSBCAPS_PRIMARYBUFFER  0x01
#define DSBCAPS_CTRL_FPV       0xE0      /* CTRLFREQUENCY | CTRLPAN | CTRLVOLUME */
#define DSBPLAY_LOOPING        1
#define DSBSTATUS_PLAYING      1

typedef struct DsBufferDesc {            /* DSBUFFERDESC (DirectX 5+), 0x24 */
    uint32_t dwSize, dwFlags, dwBufferBytes, dwReserved;
    void    *lpwfxFormat;
    uint32_t guid3DAlgorithm[4];
} DsBufferDesc;

typedef struct WaveFormat {               /* WAVEFORMATEX, 0x12 */
    uint16_t wFormatTag, nChannels;
    uint32_t nSamplesPerSec, nAvgBytesPerSec;
    uint16_t nBlockAlign, wBitsPerSample, cbSize;
} WaveFormat;

/* ------------------------------------------------------------------ tables (.rdata) */
/* 0x0055E820: 2^(n/12) in 4.12 for n = -36..36 (g_zsndcSemitone[36] = 0x1000) */
extern const uint16_t g_zsndcSemitone[73];
/* 0x0055E8B4: 2^(c/1200) in 4.12 for c = 0..99 */
extern const uint16_t g_zsndcCent[100];
/* 0x0055E97C: channel gain by level 0..127 (0, 0.031 .. 0.973, 1.0), an audio taper */
extern const float    g_zsndcGain[128];
/* 0x0055EB7C: envelope stage times in ms by index 0..127 (1, 2, 3, .. 28577, 30000, 0) */
extern const uint16_t g_zsndcEnvTimeMs[128];
const uint32_t        g_zsndcMsPerSecond = 1000;       /* 0x0055ED7C */
#define SEMITONE(n) (g_zsndcSemitone[36 + (n)])

/* ------------------------------------------------------------------ globals */
void        *g_zsndcBuffer;              /* 0x005FB760 the secondary (mix) buffer */
uint32_t     g_zsndcChorusOffset;        /* 0x005FB764 20.12 frames back into the chorus line */
int32_t      g_zsndcNumPlaying;          /* 0x005FB768 */
uint32_t     g_zsndcEchoLen;             /* 0x005FB76C frames in g_zsndcEchoBuf */
ZsndcChannel g_zsndcChannels[ZSNDC_NUM_CHANNELS];   /* 0x005FB770 */
int32_t      g_zsndcMixR;                /* 0x005FBE30 scratch (8-bit stereo right / output R) */
float        g_zsndcEchoLevel;           /* 0x005FBE34 */
ZsndcHandle  g_zsndcHandles[ZSNDC_NUM_HANDLES];     /* 0x005FBE38 (fzsndcHandle) */
int32_t      g_zsndcMixL;                /* 0x005FBEB8 scratch (8-bit left / output L) */
float        g_zsndcChorusBuf[ZSNDC_CHORUS_LEN][2]; /* 0x005FBEBC */
uint32_t     g_zsndcReadPos;             /* 0x005FDEBC next byte of g_zsndcMixBuf to hand over */
uint32_t     g_zsndcWritePos;            /* 0x005FDEC0 next byte of the DirectSound buffer to fill */
uint32_t     g_zsndcEchoPos;             /* 0x005FDEC4 */
uint16_t     g_zsndcSine[4096];          /* 0x005FDEC8 raised cosine 0..4096 (chorus LFO) */
uint32_t     g_zsndcChorusPhase;         /* 0x005FFECC 12.12 */
float       *g_zsndcEchoBuf;             /* 0x005FFED0 [g_zsndcEchoLen][2] */
HMODULE      g_zsndcDsoundDll;           /* 0x005FFED4 */
uint8_t     *g_zsndcMixBuf;              /* 0x005FFED8 ring of g_zsndcBufBytes */
uint32_t     g_zsndcBlockBytes;          /* 0x005FFEDC bufferMs of stereo 16-bit frames */
uint32_t     g_zsndcChorusDelay;         /* 0x005FFEE0 frames */
uint32_t     g_zsndcRate;                /* 0x005FFEE4 output rate */
float        g_zsndcChorusLevel;         /* 0x005FFEE8 */
uint32_t     g_zsndcEchoDelay;           /* 0x005FFEEC frames, 0 = echo off */
float        g_zsndcEchoFeedback;        /* 0x005FFEF0 */
uint32_t     g_zsndcMixPos;              /* 0x005FFEF4 next byte of g_zsndcMixBuf to mix */
uint32_t     g_zsndcPlayPos;             /* 0x005FFEF8 last play cursor */
int32_t      g_zsndcChorusDepth;         /* 0x005FFEFC frames, 0 = chorus off */
int32_t      g_zsndcChorusPos;           /* 0x005FFF00 */
uint32_t     g_zsndcChorusRate;          /* 0x005FFF04 */
uint32_t     g_zsndcBufBytes;            /* 0x005FFF08 two blocks */
int32_t      g_zsndcInitialized;         /* 0x005FFF0C fzsndcInitialized */
int32_t      g_zsndcLock;                /* 0x005FFF10 > 0: zsndcUpdate does nothing (API in progress) */
uint32_t     g_zsndcNextChannel;         /* 0x005FFF14 */
uint32_t     g_zsndcNextHandle;          /* 0x005FFF18 fzsndcNextChannelHandleNum */
int (__stdcall *g_pfnDirectSoundCreate)(void *guid, void **ds, void *outer);   /* 0x005FFF1C */
void        *g_zsndcPrimary;             /* 0x005FFF20 */
uint32_t     g_zsndcUpdates;             /* 0x005FFF24 */
void        *g_zsndcDS;                  /* 0x005FFF28 IDirectSound* */

static uint32_t msToFrames(uint32_t ms)
{
    return ms * g_zsndcRate / g_zsndcMsPerSecond;
}

/* VA 0x0046d940, 1189 bytes, [V] direct, status draft */
int zsndcInit(ZsndcInitParams *init)
{
    DsBufferDesc desc;
    WaveFormat   wfx;
    void        *p1, *p2;
    uint32_t     n1, n2, frames, i;
    int          err;

    /* [V] `<= 0x5FBEB9`: 65 handles are cleared, the 65th lands on g_zsndcMixL */
    for (i = 0; i <= ZSNDC_NUM_HANDLES; i++) {
        g_zsndcHandles[i].allocated = 0;
        g_zsndcHandles[i].channel = 0xFF;
    }
    for (i = 0; i < ZSNDC_NUM_CHANNELS; i++)
        g_zsndcChannels[i].state = ZSNDC_FREE;
    g_zsndcNumPlaying = 0;
    if (init->bufferMs == 0)
        init->bufferMs = 50;
    frames = init->bufferMs * init->rate / 1000;        /* [V] init->rate, before the 0 -> 22050 default */
    g_zsndcBlockBytes = frames * 4;
    g_zsndcBufBytes = g_zsndcBlockBytes * 2;
    g_zsndcMixBuf = (uint8_t *)malloc(g_zsndcBufBytes);
    if (!g_zsndcMixBuf)
        return 0x108;
    memset(g_zsndcMixBuf, 0, g_zsndcBufBytes);
    g_zsndcMixPos = 0;
    g_zsndcReadPos = g_zsndcBufBytes - g_zsndcBlockBytes;
    g_zsndcEchoLen = (uint32_t)((double)init->rate * init->echoSeconds);
    g_zsndcEchoBuf = (float *)malloc(g_zsndcEchoLen * 8);
    if (!g_zsndcEchoBuf)
        return 0x108;
    g_zsndcEchoDelay = 0;
    g_zsndcEchoLevel = 0.0f;
    g_zsndcEchoFeedback = 0.0f;
    g_zsndcEchoPos = 0;
    for (i = 0; i < 4096; i++)
        g_zsndcSine[i] = (uint16_t)((0.5 - cos(i * (6.283185307179586 / 4096)) * 0.5) * 4096.0);
    g_zsndcChorusDelay = 0;
    g_zsndcChorusLevel = 0.0f;
    g_zsndcChorusDepth = 0;
    g_zsndcChorusRate = 0;
    g_zsndcChorusPhase = 0;
    g_zsndcChorusPos = 0;
    g_zsndcChorusOffset = 0;

    g_zsndcDsoundDll = LoadLibraryA("DSOUND.DLL");
    if (!g_zsndcDsoundDll) {
        err = 0x101;
        goto fail_free;
    }
    g_pfnDirectSoundCreate = (int (__stdcall *)(void *, void **, void *))GetProcAddress(g_zsndcDsoundDll, "DirectSoundCreate");
    if (!g_pfnDirectSoundCreate) {
        err = 0x102;
        goto fail_free;
    }
    if (g_pfnDirectSoundCreate(0, &g_zsndcDS, 0) != 0) {
        err = 0x107;
        goto fail_free;
    }
    ASSERT_AT(init->hwnd, "init->hWnd", 0x16A);
    if (DS_SetCooperativeLevel(g_zsndcDS, init->hwnd, DSSCL_NORMAL) != 0) {
        err = 0x105;
        goto fail_ds;
    }
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    if (DS_CreateSoundBuffer(g_zsndcDS, &desc, &g_zsndcPrimary) != 0) {
        err = 0x106;
        goto fail_ds;
    }
    if (DS_SetCooperativeLevel(g_zsndcDS, init->hwnd, DSSCL_PRIORITY) != 0) {
        err = 0x105;
        goto fail_primary;
    }
    /* the primary format: the requested rate, 8/16 bit, mono/stereo */
    wfx.wFormatTag = 1;
    wfx.nSamplesPerSec = init->rate ? init->rate : 22050;
    wfx.cbSize = 0;
    wfx.wBitsPerSample = (init->flags & 1) ? 8 : 16;
    wfx.nChannels = (init->flags & 2) ? 1 : 2;
    wfx.nBlockAlign = (uint16_t)((wfx.wBitsPerSample >> 3) * wfx.nChannels);
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    if (DSB_SetFormat(g_zsndcPrimary, &wfx) != 0) {
        err = 0x104;
        goto fail_primary;
    }
    g_zsndcRate = wfx.nSamplesPerSec;
    DS_SetCooperativeLevel(g_zsndcDS, init->hwnd, DSSCL_NORMAL);
    if (DSB_Play(g_zsndcPrimary, 0, 0, DSBPLAY_LOOPING) != 0) {
        err = 0x103;
        goto fail_primary;
    }
    /* the mix buffer: always 16-bit stereo at the output rate */
    wfx.nChannels = 2;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = g_zsndcRate * 4;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags |= DSBCAPS_CTRL_FPV;
    desc.dwBufferBytes = g_zsndcBufBytes;
    desc.lpwfxFormat = &wfx;
    if (DS_CreateSoundBuffer(g_zsndcDS, &desc, &g_zsndcBuffer) != 0) {
        err = 0x10A;
        goto fail_primary;
    }
    if (DSB_Lock(g_zsndcBuffer, 0, g_zsndcBufBytes, &p1, &n1, &p2, &n2, 0) != 0) {
        err = 0x109;
        goto fail_primary;
    }
    memset(p1, 0, g_zsndcBufBytes);
    DSB_Unlock(g_zsndcBuffer, p1, g_zsndcBufBytes, 0, 0);
    g_zsndcWritePos = 0;
    g_zsndcPlayPos = 0;
    if (DSB_Play(g_zsndcBuffer, 0, 0, DSBPLAY_LOOPING) != 0) {
        err = 0x10B;
        goto fail_primary;
    }
    g_zsndcInitialized = 1;
    return 0;

fail_primary:
    COM_Release(g_zsndcPrimary);
fail_ds:
    COM_Release(g_zsndcDS);
    g_zsndcDS = 0;
fail_free:
    free(g_zsndcMixBuf);       /* [V] the echo buffer leaks; the DLL stays loaded */
    return err;
}

/* VA 0x0046df20, 184 bytes, [V] direct, status draft */
void zsndcShutdown(void)
{
    uint8_t h;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x1D1);
    for (h = 0; h < ZSNDC_NUM_HANDLES; h++)
        if (g_zsndcHandles[h].allocated)
            zsndcFree(h);
    DSB_Stop(g_zsndcBuffer);
    COM_Release(g_zsndcBuffer);
    DSB_Stop(g_zsndcPrimary);
    COM_Release(g_zsndcPrimary);
    COM_Release(g_zsndcDS);
    g_zsndcDS = 0;
    FreeLibrary(g_zsndcDsoundDll);
    free(g_zsndcMixBuf);       /* [V] g_zsndcEchoBuf is never freed */
    g_zsndcInitialized = 0;
}

/* VA 0x0046e010, 742 bytes, [I] row, status draft */
int zsndcUpdate(void)
{
    uint32_t status, play, write, elapsed, n1, n2;
    void    *p1, *p2;
    int      rc = 0;

    if (g_zsndcLock)
        return 0;
    g_zsndcUpdates++;
    if (DSB_GetStatus(g_zsndcBuffer, &status) != 0)
        return 0x10C;
    if (!(status & DSBSTATUS_PLAYING))
        return 0;
    DSB_GetCurrentPosition(g_zsndcBuffer, &play, &write);
    if (play >= g_zsndcPlayPos) {
        elapsed = play - g_zsndcPlayPos;
    } else {
        elapsed = g_zsndcBufBytes - g_zsndcPlayPos + play;
        if (elapsed > g_zsndcBlockBytes) {
            /* fell behind by more than a block: skip one */
            g_zsndcWritePos += g_zsndcBlockBytes;
            if (g_zsndcWritePos >= g_zsndcBufBytes)
                g_zsndcWritePos -= g_zsndcBufBytes;
            g_zsndcReadPos += g_zsndcBlockBytes;
            if (g_zsndcReadPos >= g_zsndcBufBytes)
                g_zsndcReadPos -= g_zsndcBufBytes;
        }
    }
    g_zsndcPlayPos = play;
    /* the block at the write position is free once the play cursor is inside it (or far past) */
    if ((g_zsndcWritePos > play && g_zsndcWritePos < play + g_zsndcBlockBytes) ||
        (g_zsndcWritePos < play && g_zsndcWritePos + g_zsndcBlockBytes < play)) {
        int hr = DSB_Lock(g_zsndcBuffer, g_zsndcWritePos, g_zsndcBlockBytes, &p1, &n1, &p2, &n2, 0);
        if (hr == DSERR_BUFFERLOST) {
            DSB_Restore(g_zsndcBuffer);
            hr = DSB_Lock(g_zsndcBuffer, g_zsndcWritePos, g_zsndcBlockBytes, &p1, &n1, &p2, &n2, 0);
        }
        if (hr == 0) {
            if (g_zsndcReadPos + n1 < g_zsndcBufBytes) {
                memcpy(p1, g_zsndcMixBuf + g_zsndcReadPos, n1);
                g_zsndcReadPos += n1;
            } else {
                uint32_t first = g_zsndcBufBytes - g_zsndcReadPos;
                memcpy(p1, g_zsndcMixBuf + g_zsndcReadPos, first);
                memcpy((uint8_t *)p1 + first, g_zsndcMixBuf, g_zsndcReadPos - g_zsndcBufBytes + n1);
                g_zsndcReadPos += n1 - g_zsndcBufBytes;
            }
            DSB_Unlock(g_zsndcBuffer, p1, n1, p2, n2);
            g_zsndcWritePos += g_zsndcBlockBytes;
            if (g_zsndcWritePos >= g_zsndcBufBytes)
                g_zsndcWritePos -= g_zsndcBufBytes;
        } else {
            rc = 0x109;
        }
    }
    /* mix the time that has passed into the ring */
    if (g_zsndcMixPos + elapsed < g_zsndcBufBytes) {
        zsndcMix((int16_t *)(g_zsndcMixBuf + g_zsndcMixPos), elapsed);
        g_zsndcMixPos += elapsed;
    } else {
        zsndcMix((int16_t *)(g_zsndcMixBuf + g_zsndcMixPos), g_zsndcBufBytes - g_zsndcMixPos);
        zsndcMix((int16_t *)g_zsndcMixBuf, g_zsndcMixPos - g_zsndcBufBytes + elapsed);
        g_zsndcMixPos += elapsed - g_zsndcBufBytes;
    }
    return rc;
}

/* VA 0x0046e3c0, 60 bytes, [I] row, status draft */
void zsndcFreeAll(void)
{
    uint8_t h;

    for (h = 0; h < 0x41; h++)              /* [V] 0x41: one past the table, as in zsndcInit */
        if (g_zsndcHandles[h].allocated)
            zsndcFree(h);
    g_zsndcNextChannel = 0;
}

/* VA 0x0046e410, 912 bytes, [V] direct, status draft */
uint8_t zsndcAlloc(ZsndVoice *voice, uint8_t priority)
{
    ZsndFileDesc *fd = 0;
    ZsndcChannel *ch = 0;
    ZsndSample   *s;
    uint32_t      h, c, victim = 0;
    uint8_t       lowest = 0xFF;

    if (voice) {
        if (voice->sample == 0xFFFF)
            return 0xFF;
        s = zsndGetSample(voice->bank, voice->sample);
        if (s->file == 0xFFFF)
            return 0xFF;
        fd = zsndGetSampleFile(voice->bank, s->file);
    }
    g_zsndcLock++;
    /* a handle */
    h = g_zsndcNextHandle;
    while (g_zsndcHandles[h].allocated) {
        if (++h == ZSNDC_NUM_HANDLES)
            h = 0;
        ASSERT_AT(h != g_zsndcNextHandle, "i != fzsndcNextChannelHandleNum", 0x2B2);
    }
    g_zsndcHandles[h].allocated = 1;
    g_zsndcHandles[h].channel = 0xFF;
    g_zsndcNextHandle = h + 1;
    if (g_zsndcNextHandle == ZSNDC_NUM_HANDLES)
        g_zsndcNextHandle = 0;

    /* a mixer channel: the next free one, else steal the lowest priority (ties: the last seen) */
    c = g_zsndcNextChannel;
    for (;;) {
        if (g_zsndcChannels[c].state == ZSNDC_FREE) {
            ch = &g_zsndcChannels[c];
            g_zsndcNextChannel = c + 1 == ZSNDC_NUM_CHANNELS ? 0 : c + 1;
            break;
        }
        if (g_zsndcChannels[c].priority <= lowest) {
            lowest = g_zsndcChannels[c].priority;
            victim = c;
        }
        if (++c == ZSNDC_NUM_CHANNELS)
            c = 0;
        if (c == g_zsndcNextChannel)
            break;
    }
    if (!ch) {
        if (lowest <= priority) {
            ch = &g_zsndcChannels[victim];
            zsndcRelease(ch);
            g_zsndcNextChannel = victim + 1 == ZSNDC_NUM_CHANNELS ? 0 : victim + 1;
            c = victim;
        }
        if (!ch) {
            g_zsndcHandles[h].allocated = 0;
            g_zsndcLock--;
            return 0xFF;
        }
    }

    ch->priority = priority;
    ch->state = ZSNDC_STOPPED;
    ch->flags = 0;
    ch->handle = (uint8_t)h;
    ch->voice = voice;
    if (voice) {
        s = zsndGetSample(voice->bank, voice->sample);
        ch->sample = s;
        ch->numFrames = fd->size / (((s->flags & ZSND_SAMPLE_8BIT) ? 1 : 2) *
                                    ((s->flags & ZSND_SAMPLE_STEREO) ? 2 : 1));
        ch->step = voice->pitch * s->rate / g_zsndcRate;
        ch->pos = 0;
        ch->frac = 0;
        ch->volume = voice->volume;
        ch->pan = voice->pan;
        if (ch->volume == 0) {
            ch->gainR = 0.0f;
            ch->gainL = 0.0f;
        } else if (ch->pan < 1) {
            ch->gainL = g_zsndcGain[voice->volume];
            ch->gainR = g_zsndcGain[((ch->pan + 127) * voice->volume >> 7) & 0xFF];
        } else {
            ch->gainR = g_zsndcGain[voice->volume];
            ch->gainL = g_zsndcGain[((127 - ch->pan) * voice->volume >> 7) & 0xFF];
        }
        ch->attackRate = zsndcEnvRate(127, g_zsndcEnvTimeMs[voice->attack]);
        /* [V] positive: the decay stage would rise from 1.0; harmless, every voice with a decay
         * time has sustainLevel 127, which makes the rate 0 */
        if (voice->decay == 0x7F || voice->sustainLevel == 0x7F)
            ch->decayRate = 0.0f;
        else
            ch->decayRate = zsndcEnvRate(127 - voice->sustainLevel, g_zsndcEnvTimeMs[voice->decay]);
        ch->sustainLevel = g_zsndcGain[voice->sustainLevel];
        if (voice->sustainTime == 0x7F)
            ch->sustainRate = 0.0f;
        else
            ch->sustainRate = zsndcEnvRate(-(int)voice->sustainLevel, g_zsndcEnvTimeMs[voice->sustainTime]);
        g_zsndcNumPlaying++;
    }
    g_zsndcHandles[h].channel = (uint8_t)c;
    g_zsndcLock--;
    return (uint8_t)h;
}

/* VA 0x0046e890, 18 bytes, [I] row, status draft */
int zsndcFree(uint8_t hChan)
{
    zsndcKeyOff(hChan, 2);
    return 0;
}

/* VA 0x0046e8c0, 163 bytes, [V] direct, status draft */
int zsndcPlayVoice(ZsndVoice *voice, int16_t cents, uint8_t volume, int8_t pan, uint8_t priority)
{
    uint8_t h;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x332);
    if (!voice)
        return 0;
    g_zsndcLock++;
    h = zsndcAlloc(voice, priority);
    if (h != 0xFF) {
        zsndcSetVolume(h, volume);
        zsndcSetPan(h, pan);
        zsndcSetPitch(h, cents);
        zsndcKeyOn(h);
        g_zsndcChannels[g_zsndcHandles[h].channel].flags |= ZSNDC_AUTOFREE;   /* fire and forget */
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046e9a0, 340 bytes, [V] direct, status draft */
int zsndcKeyOn(uint8_t hChan)
{
    ZsndcChannel *ch;
    ZsndVoice    *v;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x351);
    g_zsndcLock++;
    ch = zsndcGetChannel(hChan);
    if (ch) {
        ASSERT_AT(zsndGetSample(ch->voice->bank, ch->voice->sample), "sample", 0x357);
        v = ch->voice;
        ch->pos = 0;
        ch->frac = 0;
        if (v->attack) {
            ch->envLevel = 0.0f;
            ch->envRate = ch->attackRate;
            ch->envCount = msToFrames(g_zsndcEnvTimeMs[v->attack]);
            ch->envStage = ZSNDC_ENV_ATTACK;
        } else if (v->decay) {
            ch->envLevel = 1.0f;
            ch->envRate = ch->decayRate;
            ch->envCount = msToFrames(g_zsndcEnvTimeMs[v->decay]);
            ch->envStage = ZSNDC_ENV_DECAY;
        } else {
            ch->envLevel = ch->sustainLevel;
            ch->envRate = ch->sustainRate;
            if (v->sustainTime < 0x7F) {
                ch->envCount = msToFrames(g_zsndcEnvTimeMs[v->sustainTime]);
                if (ch->envCount == 0)
                    ch->envCount = 1;
            } else {
                ch->envCount = 0;            /* hold */
            }
            ch->envStage = ZSNDC_ENV_SUSTAIN;
        }
        ch->state = ZSNDC_PLAYING;
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046eb60, 286 bytes, [V] direct, status draft
 * how: bit 0 = cut at once (no release), bit 1 = also free the handle */
int zsndcKeyOff(uint8_t hChan, uint8_t how)
{
    ZsndcChannel *ch;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x392);
    g_zsndcLock++;
    ch = zsndcGetChannel(hChan);
    if (!ch) {
        if (how & 2)
            g_zsndcHandles[hChan].allocated = 0;
    } else {
        ZsndVoice *v = ch->voice;
        if (!(how & 1) && v->release) {
            /* fade from the current level to 0 over the release time */
            ch->envRate = zsndcEnvRate((int)(ch->envLevel * -127.0f), g_zsndcEnvTimeMs[v->release]);
            ch->envCount = msToFrames(g_zsndcEnvTimeMs[v->release]);
            ch->envStage = ZSNDC_ENV_RELEASE;
        } else {
            ch->state = ZSNDC_STOPPED;
        }
        if (how & 2) {
            if (ch->state == ZSNDC_STOPPED) {
                zsndcRelease(ch);
                g_zsndcHandles[ch->handle].allocated = 0;
            } else {
                ch->flags |= ZSNDC_AUTOFREE;   /* the mixer frees it after the release */
            }
        }
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046ecd0, 53 bytes, [I] row, status draft */
int zsndcGetState(uint8_t hChan)
{
    ZsndcChannel *ch = zsndcGetChannel(hChan);

    if (ch && (ch->state == 1 || ch->state == 2 || ch->state == 3))
        return ch->state;
    return 0;
}

static void setGains(ZsndcChannel *ch)
{
    if (ch->pan < 0) {
        ch->gainL = g_zsndcGain[ch->volume];
        ch->gainR = g_zsndcGain[((ch->pan + 127) * ch->volume >> 7) & 0xFF];
    } else {
        ch->gainR = g_zsndcGain[ch->volume];
        ch->gainL = g_zsndcGain[((127 - ch->pan) * ch->volume >> 7) & 0xFF];
    }
}

/* VA 0x0046ed20, 264 bytes, [V] direct, status draft */
int zsndcSetVolume(uint8_t hChan, uint8_t volume)
{
    ZsndcChannel *ch;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x3E1);
    g_zsndcLock++;
    ch = zsndcGetChannel(hChan);
    if (ch) {
        ch->volume = (uint8_t)(ch->voice->volume * volume >> 7);
        if (ch->volume)
            setGains(ch);
        else
            ch->gainR = ch->gainL = 0.0f;
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046ee80, 233 bytes, [V] direct, status draft */
int zsndcSetPan(uint8_t hChan, int8_t pan)
{
    ZsndcChannel *ch;
    int           p;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x407);
    g_zsndcLock++;
    ch = zsndcGetChannel(hChan);
    if (ch) {
        p = ch->voice->pan + pan;
        if (p < -127)
            p = -127;
        else if (p >= 127)
            p = 127;
        ch->pan = (int8_t)p;
        setGains(ch);
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046efb0, 350 bytes, [V] direct, status draft */
int zsndcSetPitch(uint8_t hChan, int16_t cents)
{
    ZsndcChannel *ch;
    ZsndSample   *s;
    uint32_t      rate;
    int           semis, rest;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x428);
    g_zsndcLock++;
    ch = zsndcGetChannel(hChan);
    if (ch) {
        s = zsndGetSample(ch->voice->bank, ch->voice->sample);
        ASSERT_AT(s, "sample", 0x42D);
        rate = ch->voice->pitch * s->rate >> 12;
        if (cents) {
            semis = cents / 100;
            rest = cents % 100;
            if (rest < 0) {                     /* floor division */
                int borrow = (99 - rest) / 100;
                semis -= borrow;
                rest += borrow * 100;
            }
            if (semis < -36)
                semis = -36;
            else if (semis > 35)
                semis = 36;
            rate = ((int)(SEMITONE(semis) * rate) >> 12) * g_zsndcCent[rest] >> 12;
        }
        if (rate == g_zsndcRate) {
            ch->step = 0x1000;
            ch->flags |= ZSNDC_UNITY_PITCH;
        } else {
            ch->step = (rate << 12) / g_zsndcRate;
            ch->flags &= ~ZSNDC_UNITY_PITCH;
        }
    }
    g_zsndcLock--;
    return 0;
}

/* VA 0x0046f170, 56 bytes, [V] direct, status draft */
ZsndVoice *zsndcGetVoice(uint8_t hChan)
{
    ZsndcChannel *ch;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x455);
    ch = zsndcGetChannel(hChan);
    return ch ? ch->voice : 0;
}

/* VA 0x0046f1c0, 106 bytes, [V] direct, status draft */
void zsndcStopVoice(ZsndVoice *voice)
{
    int c;

    ASSERT_AT(g_zsndcInitialized, "fzsndcInitialized", 0x467);
    g_zsndcLock++;
    for (c = 0; c < ZSNDC_NUM_CHANNELS; c++)
        if (g_zsndcChannels[c].state != ZSNDC_FREE && g_zsndcChannels[c].voice == voice)
            zsndcKeyOff(g_zsndcChannels[c].handle, 3);
    g_zsndcLock--;
}

/* VA 0x0046f250, 237 bytes, [I] row, status draft */
int zsndcSetEffect(const ZsndcEffect *fx)
{
    uint32_t frames;

    switch (fx->type) {
    case ZSNDC_EFFECT_ECHO:
        frames = msToFrames(fx->delayMs);
        g_zsndcEchoDelay = frames <= g_zsndcEchoLen ? frames : g_zsndcEchoLen;
        g_zsndcEchoLevel = fx->level * (1.0f / 127.0f);
        g_zsndcEchoFeedback = fx->feedback;
        return 0;
    case ZSNDC_EFFECT_CHORUS:
        g_zsndcChorusDelay = msToFrames(fx->delayMs);
        if (g_zsndcChorusDelay > ZSNDC_CHORUS_LEN)
            g_zsndcChorusDelay = ZSNDC_CHORUS_LEN;
        g_zsndcChorusLevel = fx->level * (1.0f / 127.0f);
        g_zsndcChorusDepth = (int32_t)((double)(ZSNDC_CHORUS_LEN - g_zsndcChorusDelay) * fx->feedback);
        g_zsndcChorusRate = (uint32_t)(fx->rate * 4096.0);
        return 0;
    }
    return 0x10D;              /* bmxsound's PSX reverb requests (type 2) end here */
}

/* VA 0x0046f380, 177 bytes, [I] row, status draft
 * Can DirectSound be created (and the cooperative level set for hwnd)? 0 = yes. */
int zsndcProbe(void *hwnd)
{
    int rc;

    g_zsndcDsoundDll = LoadLibraryA("DSOUND.DLL");
    if (!g_zsndcDsoundDll)
        return 0x101;
    g_pfnDirectSoundCreate = (int (__stdcall *)(void *, void **, void *))GetProcAddress(g_zsndcDsoundDll, "DirectSoundCreate");
    if (!g_pfnDirectSoundCreate) {
        rc = 0x102;
    } else if (g_pfnDirectSoundCreate(0, &g_zsndcDS, 0) != 0) {
        rc = 0x107;
    } else {
        rc = 0;
        if (hwnd && DS_SetCooperativeLevel(g_zsndcDS, hwnd, DSSCL_NORMAL) != 0)
            rc = 0x105;
        COM_Release(g_zsndcDS);
        g_zsndcDS = 0;
    }
    g_pfnDirectSoundCreate = 0;
    FreeLibrary(g_zsndcDsoundDll);
    return rc;
}

/* VA 0x0046f470, 140 bytes, [V] direct, status draft */
ZsndcChannel *zsndcGetChannel(uint8_t hChan)
{
    ASSERT_AT(hChan < ZSNDC_NUM_HANDLES, "hChan < kzsndNumChannelHandles", 0x4C1);
    ASSERT_AT(g_zsndcHandles[hChan].allocated, "fzsndcHandle[hChan].allocated", 0x4C2);
    if (g_zsndcHandles[hChan].channel == 0xFF)
        return 0;                       /* stolen by a higher priority sound, or finished */
    ASSERT_AT(g_zsndcHandles[hChan].channel < ZSNDC_NUM_CHANNELS,
              "fzsndcHandle[hChan].channelNum < kzsndNumChannels", 0x4C4);
    return &g_zsndcChannels[g_zsndcHandles[hChan].channel];
}

/* VA 0x0046f530, 53 bytes, [I] row, status draft */
void zsndcRelease(ZsndcChannel *ch)
{
    ch->state = ZSNDC_FREE;
    g_zsndcHandles[ch->handle].channel = 0xFF;
    if (ch->flags & ZSNDC_AUTOFREE)
        g_zsndcHandles[ch->handle].allocated = 0;
    g_zsndcNumPlaying--;
}

/* VA 0x0046f580, 75 bytes, [I] row, status draft
 * Envelope slope per output frame: `level` (/127) over `ms`. */
float zsndcEnvRate(int level, int ms)
{
    if (ms == 0)
        return (float)(level * (1.0 / 127.0));
    return (float)((level * 1000.0) / ((double)g_zsndcRate * ms * 127.0));
}

/* VA 0x0046f5f0, 1051 bytes, [I] row, status draft
 * Mix `bytes` of 16-bit stereo output. */
void zsndcMix(int16_t *dst, uint32_t bytes)
{
    while (bytes) {
        double accL = 0.0, accR = 0.0;
        int    c, j;

        for (c = 0; c < ZSNDC_NUM_CHANNELS; c++) {
            ZsndcChannel *ch = &g_zsndcChannels[c];
            ZsndSample   *s;
            double        l, r;

            if (ch->state != ZSNDC_PLAYING)
                continue;
            s = ch->sample;
            l = (double)ch->gainL * ch->envLevel;
            r = (double)ch->gainR * ch->envLevel;
            if (!(s->flags & ZSND_SAMPLE_8BIT)) {
                const int16_t *pcm = (const int16_t *)s->data;
                if (!(s->flags & ZSND_SAMPLE_STEREO)) {
                    l *= pcm[ch->pos];
                    r *= pcm[ch->pos];
                } else {
                    l *= pcm[ch->pos * 2];
                    r *= pcm[ch->pos * 2 + 1];
                }
            } else if (!(s->flags & ZSND_SAMPLE_STEREO)) {
                g_zsndcMixL = (int16_t)((s->data[ch->pos] << 8) ^ 0x8000);
                l *= g_zsndcMixL;
                r *= g_zsndcMixL;
            } else {
                g_zsndcMixL = (int16_t)((s->data[ch->pos * 2] << 8) ^ 0x8000);
                g_zsndcMixR = (int16_t)((s->data[ch->pos * 2 + 1] << 8) ^ 0x8000);
                l *= g_zsndcMixL;
                r *= g_zsndcMixR;
            }
            accL += l;
            accR += r;

            /* envelope */
            if (ch->envCount) {
                float next = ch->envLevel + ch->envRate;
                if (--ch->envCount == 0) {
                    if (ch->envStage >= ZSNDC_ENV_SUSTAIN)
                        goto stop;              /* end of sustain / release */
                    if (ch->envStage == ZSNDC_ENV_ATTACK && ch->voice->decay) {
                        ch->envStage++;
                        ch->envRate = ch->decayRate;
                        ch->envLevel = 1.0f;
                        ch->envCount = msToFrames(g_zsndcEnvTimeMs[ch->voice->decay]);
                    } else {
                        ch->envStage = (int8_t)(ch->envStage + (ch->envStage == ZSNDC_ENV_ATTACK ? 2 : 1));
                        ch->envRate = ch->sustainRate;
                        ch->envLevel = ch->sustainLevel;
                        ch->envCount = ch->voice->sustainTime == 0x7F ? 0
                                     : msToFrames(g_zsndcEnvTimeMs[ch->voice->sustainTime]);
                    }
                }
                /* [V] the old ramp's next value overwrites the start level just set, so a new
                 * stage continues from where the previous one ended */
                ch->envLevel = next;
            }

            /* advance */
            if (ch->step < 0) {                 /* ping-pong, going back */
                int32_t f = (int32_t)ch->frac + ch->step;
                ch->frac = (uint32_t)f & 0xFFF;
                ch->pos += f >> 12;
                if (ch->pos < (int32_t)s->loopStart) {
                    ch->step = -ch->step;
                    ch->pos = s->loopStart;
                    ch->frac = 0;
                }
            } else {
                uint32_t f = ch->frac + ch->step;
                ch->frac = f & 0xFFF;
                ch->pos += f >> 12;
                if (ch->pos >= ch->numFrames) {
                    if (!(s->flags & ZSND_SAMPLE_LOOP))
                        goto stop;
                    if (!(s->flags & ZSND_SAMPLE_PINGPONG)) {
                        ch->pos -= ch->numFrames - s->loopStart;   /* loop to loopStart */
                    } else {
                        ch->step = -ch->step;
                        ch->pos = ch->numFrames - 1;
                        ch->frac = 0;
                    }
                }
            }
            continue;
stop:
            ch->state = ZSNDC_STOPPED;
            ch->envCount = 0;
            if (ch->flags & ZSNDC_AUTOFREE)
                zsndcRelease(ch);
        }

        /* chorus: the dry mix goes into the line; out = dry x (1 - level) + delayed x level */
        g_zsndcChorusBuf[g_zsndcChorusPos][0] = (float)accL;
        g_zsndcChorusBuf[g_zsndcChorusPos][1] = (float)accR;
        if (g_zsndcChorusDepth) {
            j = g_zsndcChorusPos;
            if (j < (int32_t)(g_zsndcChorusOffset >> 12))
                j += ZSNDC_CHORUS_LEN;
            j -= g_zsndcChorusOffset >> 12;
            accL = g_zsndcChorusBuf[j][0] * g_zsndcChorusLevel + accL * (1.0 - g_zsndcChorusLevel);
            accR = g_zsndcChorusLevel * g_zsndcChorusBuf[j][1] + (1.0 - g_zsndcChorusLevel) * accR;
        }
        if (++g_zsndcChorusPos >= ZSNDC_CHORUS_LEN)
            g_zsndcChorusPos = 0;
        g_zsndcChorusOffset = g_zsndcSine[g_zsndcChorusPhase >> 12] * g_zsndcChorusDepth + g_zsndcChorusDelay * 0x1000;
        g_zsndcChorusPhase += g_zsndcChorusRate;
        if (g_zsndcChorusPhase > 0xFFFFFF)
            g_zsndcChorusPhase -= 0x1000000;

        /* echo */
        if (g_zsndcEchoDelay == 0) {
            g_zsndcEchoBuf[g_zsndcEchoPos * 2] = (float)accL;
            g_zsndcEchoBuf[g_zsndcEchoPos * 2 + 1] = (float)accR;
        } else {
            double eL, eR;
            j = g_zsndcEchoPos;
            if ((uint32_t)j < g_zsndcEchoDelay)
                j += g_zsndcEchoLen;
            j -= g_zsndcEchoDelay;
            eR = g_zsndcEchoBuf[j * 2 + 1];
            eL = g_zsndcEchoBuf[j * 2];
            g_zsndcEchoBuf[g_zsndcEchoPos * 2] = (float)(g_zsndcEchoFeedback * eL + accL);
            g_zsndcEchoBuf[g_zsndcEchoPos * 2 + 1] = (float)(g_zsndcEchoFeedback * eR + accR);
            accL += eL * g_zsndcEchoLevel;
            accR += eR * g_zsndcEchoLevel;
        }
        if (++g_zsndcEchoPos >= g_zsndcEchoLen)
            g_zsndcEchoPos = 0;

        /* out: round, clamp to 17 bits, halve */
        g_zsndcMixL = (int32_t)floor(accL + 0.5);
        g_zsndcMixR = (int32_t)floor(accR + 0.5);
        dst[0] = (int16_t)((g_zsndcMixL >= 0x10000 ? 0xFFFF : g_zsndcMixL < -0x10000 ? -0x10000 : g_zsndcMixL) >> 1);
        dst[1] = (int16_t)((g_zsndcMixR >= 0x10000 ? 0xFFFF : g_zsndcMixR < -0x10000 ? -0x10000 : g_zsndcMixR) >> 1);
        dst += 2;
        bytes -= 4;
    }
}

/*
 * zaxis/Bmx/Src/Pc/zge/sound/winStrm.c
 * Original: C:\zaxis\Bmx\Src\Pc\zge\sound\winStrm.c
 * Evidence: [V] path string 0x0055F2EC (CRT assert()s "fzsndStreamInitialized",
 *           "hResult == DS_OK", "dwLen2 == 0") in 18 functions; [I] the rest: between them, and they
 *           use the module's data 0x00601328-0x00601B83 (DM-053 moved zactorInit, which followed,
 *           to zactor.c).
 * 27 functions, VA 0x00470AA0-0x004725D1. DM-055 (draft). Non-matching C (AD-8).
 * Types: zsound.h. Description: docs/re/sound.md section 3.
 *
 * Streamed audio ("zsndStream"): up to 8 streams, each its own DirectSound buffer (64 KB by
 * default; the game's music uses 256 KB) that is refilled from a file while it plays.
 *  - A stream plays a queue of files (ZsndStreamQueue: file + loop count); the head entry lives in
 *    the stream itself, zsndStreamQueueFile appends (or inserts after the head).
 *  - Files are byte ranges of a bank file (zsndGetSampleFile: path, offset, size) opened with
 *    CreateFile; the data is either raw PCM or (default) IMA-ADPCM: mono, low nibble first, one
 *    continuous stream from predictor 0 / step 7 (index 0), no block headers (zsndImaDecode).
 *  - zsndStreamService (from the 16 ms timer) refills `refillBytes` (half the buffer) behind the
 *    play cursor; when a file runs out the rest of the buffer is filled with silence and the
 *    stream stops (or is destroyed: ZSND_STRM_AUTOFREE) once the play cursor gets there.
 *  - zsndStreamUpdate (per frame) runs volume fades, in a linear-dB approximation.
 * DM-055 created zsndStreamSetLoops, zsndStreamQueueFile, zsndStreamHold, zsndStreamGetCaps,
 * zsndStreamPlayOnce and zsndStreamAllocQueue (only reached through ILT thunks; the game uses
 * none of them).
 */
#include <string.h>
#include <windows.h>

#include "decomp.h"
#include "zsound.h"

#define WINSTRM_FILE "C:\\zaxis\\Bmx\\src\\Pc\\zge\\sound\\winStrm.c"
#define ASSERT_AT(cond, text, line) do { if (!(cond)) _assert((text), WINSTRM_FILE, (line)); } while (0)
#define DS_OK_AT(hr, line)          ASSERT_AT((hr) == 0, "hResult == DS_OK", (line))
/* the same check where the compiler shared one call for two lines (no push-line/call pattern) */
#define DS_OK_SHARED(hr, line)      do { if ((hr) != 0) _assert("hResult == DS_OK", WINSTRM_FILE, (line)); } while (0)

/* IDirectSound(Buffer) vtable slots (dsound.h order) */
#define DS_CreateSoundBuffer(ds, d, pb)        ((*(int (__stdcall ***)(void *, const void *, void **, void *))(ds))[3]((ds), (d), (pb), 0))
#define COM_Release(p)                         ((*(int (__stdcall ***)(void *))(p))[2]((p)))
#define DSB_GetCurrentPosition(b, pp, pw)      ((*(int (__stdcall ***)(void *, uint32_t *, uint32_t *))(b))[4]((b), (pp), (pw)))
#define DSB_Lock(b, o, n, p1, n1, p2, n2, f)   ((*(int (__stdcall ***)(void *, uint32_t, uint32_t, void **, uint32_t *, void **, uint32_t *, uint32_t))(b))[11]((b), (o), (n), (p1), (n1), (p2), (n2), (f)))
#define DSB_Play(b, r1, r2, f)                 ((*(int (__stdcall ***)(void *, uint32_t, uint32_t, uint32_t))(b))[12]((b), (r1), (r2), (f)))
#define DSB_SetCurrentPosition(b, p)           ((*(int (__stdcall ***)(void *, uint32_t))(b))[13]((b), (p)))
#define DSB_SetVolume(b, v)                    ((*(int (__stdcall ***)(void *, int32_t))(b))[15]((b), (v)))
#define DSB_SetPan(b, p)                       ((*(int (__stdcall ***)(void *, int32_t))(b))[16]((b), (p)))
#define DSB_Stop(b)                            ((*(int (__stdcall ***)(void *))(b))[18]((b)))
#define DSB_Unlock(b, p1, n1, p2, n2)          ((*(int (__stdcall ***)(void *, void *, uint32_t, void *, uint32_t))(b))[19]((b), (p1), (n1), (p2), (n2)))
#define DSB_Restore(b)                         ((*(int (__stdcall ***)(void *))(b))[20]((b)))

typedef struct DsBufferDesc {            /* DSBUFFERDESC, 0x24 */
    uint32_t dwSize, dwFlags, dwBufferBytes, dwReserved;
    void    *lpwfxFormat;
    uint32_t guid3DAlgorithm[4];
} DsBufferDesc;
typedef struct WaveFormat {               /* WAVEFORMATEX */
    uint16_t wFormatTag, nChannels;
    uint32_t nSamplesPerSec, nAvgBytesPerSec;
    uint16_t nBlockAlign, wBitsPerSample, cbSize;
} WaveFormat;

/* ------------------------------------------------------------------ data */
/* 0x0055EC7C: DirectSound attenuation (1/100 dB) by volume 0..127: -10000, -4200 .. -13, 0 */
extern const int16_t g_zsndStreamAtten[128];
/* 0x0055F1AC / 0x0055F1EC: the IMA-ADPCM index (int32) and step (uint16) tables, standard values */
extern const int32_t g_imaIndexTable[16];
extern const uint16_t g_imaStepTable[89];
extern void         *g_zsndcDS;           /* winChan.c: the IDirectSound */
extern uint32_t      g_zsndUpdateRate;    /* zsndsrc.c 0x0055FFC0: game frames per second (30) */

uint32_t         g_zsndNextStreamFile;                     /* 0x00601328 */
CRITICAL_SECTION g_zsndStreamLock;                         /* 0x00601330 (0x18) */
uint32_t         g_zsndNextStream;                         /* 0x00601348 */
ZsndStreamFile   g_zsndStreamFiles[ZSND_MAX_STREAM_FILES]; /* 0x00601350; size != 0 = in use */
ZsndStream       g_zsndStreams[ZSND_MAX_STREAMS];          /* 0x006017D0 */
uint32_t         g_zsndNextQueue;                          /* 0x006019F0 */
ZsndStreamQueue  g_zsndStreamQueue[ZSND_MAX_STREAM_QUEUE]; /* 0x006019F8; loops != 0 = in use */
int32_t          g_zsndStreamInitialized;                  /* 0x00601B78 fzsndStreamInitialized */
int32_t          g_zsndStreamHoldCount;                    /* 0x00601B7C > 0: no refills */
int32_t          g_zsndStreamFrame;                        /* 0x00601B80 */

/* after a Fill: remember where the data ends (the stream stops there) */
static void markEnd(ZsndStream *s, uint32_t filled, uint32_t want, uint32_t end)
{
    if (filled < want) {
        s->flags |= ZSND_STREAM_ENDING;
        s->endPos = end;
        s->remaining = end;
    } else {
        s->flags &= ~ZSND_STREAM_ENDING;
    }
}

/* VA 0x00470aa0, 96 bytes, [V] direct, status draft */
int zsndStreamInit(void)
{
    ASSERT_AT(!g_zsndStreamInitialized, "!fzsndStreamInitialized", 0x73);
    memset(g_zsndStreams, 0, sizeof(g_zsndStreams));
    memset(g_zsndStreamQueue, 0, sizeof(g_zsndStreamQueue));
    g_zsndNextStream = 0;
    g_zsndNextQueue = 0;
    g_zsndStreamHoldCount = 0;
    InitializeCriticalSection(&g_zsndStreamLock);
    g_zsndStreamInitialized = 1;
    return 0;
}

/* VA 0x00470b20, 89 bytes, [V] direct, status draft */
int zsndStreamShutdown(void)
{
    int i;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x8F);
    for (i = 0; i < ZSND_MAX_STREAMS; i++)
        if (g_zsndStreams[i].flags & ZSND_STREAM_ALLOC)
            zsndStreamDestroy(&g_zsndStreams[i]);
    DeleteCriticalSection(&g_zsndStreamLock);
    g_zsndStreamInitialized = 0;
    return 0;
}

/* VA 0x00470ba0, 285 bytes, [V] direct, status draft
 * Per game frame: volume fades. */
int zsndStreamUpdate(void)
{
    int32_t frame;
    int     i, n;

    if (!g_zsndStreamInitialized)
        return 0;
    frame = g_zsndStreamFrame + 1;
    for (i = 0; i < ZSND_MAX_STREAMS; i++) {
        ZsndStream *s = &g_zsndStreams[i];
        if (!(s->flags & ZSND_STREAM_ALLOC) || s->state != ZSND_STREAM_PLAYING || !(s->fadeFlags & 1))
            continue;
        for (n = frame - g_zsndStreamFrame; n; n--) {     /* always one step */
            s->fadeVol += s->fadeStep;
            if (s->fadeStep < 1 ? s->fadeVol <= s->fadeTarget : s->fadeVol >= s->fadeTarget) {
                s->fadeVol = s->fadeTarget;
                s->fadeFlags &= ~1u;
                break;
            }
        }
        /* [V] 12/100 dB per volume step below 127, not g_zsndStreamAtten */
        DS_OK_AT(DSB_SetVolume(s->buffer, ((s->fadeVol >> 16) - 127) * 12), 0xD3);
        s->volume = (uint8_t)(s->fadeVol >> 16);
        if (!(s->fadeFlags & 1) && (s->fadeFlags & 2)) {
            /* a fade-out finished: stop (or destroy); [V] the other streams wait a frame */
            if (s->flags & ZSND_STRM_AUTOFREE)
                zsndStreamDestroy(s);
            else
                zsndStreamStop(s);
            g_zsndStreamFrame = frame;
            return 0;
        }
    }
    g_zsndStreamFrame = frame;
    return 0;
}

/* VA 0x00470d10, 513 bytes, [V] direct, status draft
 * From the 16 ms timer: refill the part of each buffer the play cursor has left behind. */
int zsndStreamService(void)
{
    uint32_t play, write, moved, end, got, tail;
    int      i;

    if (!g_zsndStreamInitialized)
        return 0;
    EnterCriticalSection(&g_zsndStreamLock);
    for (i = 0; i < ZSND_MAX_STREAMS; i++) {
        ZsndStream *s = &g_zsndStreams[i];
        if (!(s->flags & ZSND_STREAM_ALLOC) || s->state != ZSND_STREAM_PLAYING || g_zsndStreamHoldCount > 0)
            continue;
        DS_OK_AT(DSB_GetCurrentPosition(s->buffer, &play, &write), 0xFB);
        moved = play < s->lastPlayPos ? play + s->bufBytes - s->lastPlayPos : play - s->lastPlayPos;
        if (s->flags & ZSND_STREAM_ENDING) {
            if (s->remaining <= moved) {               /* played up to the end of the data */
                if (s->flags & ZSND_STRM_AUTOFREE)
                    zsndStreamDestroy(s);
                else
                    zsndStreamStop(s);
                continue;
            }
            s->remaining -= moved;
        }
        s->played += moved;
        while (s->played >= s->refillBytes) {
            if (s->writePos + s->refillBytes < s->bufBytes) {
                got = zsndStreamFill(s, s->refillBytes, &end);
                if (got < s->refillBytes && !(s->flags & ZSND_STREAM_ENDING)) {
                    s->flags |= ZSND_STREAM_ENDING;
                    s->endPos = end;
                    s->remaining = end > play ? end - play : s->bufBytes - play + end;
                }
            } else {
                /* the refill wraps around the end of the buffer */
                tail = s->bufBytes - s->writePos;
                got = zsndStreamFill(s, tail, &end);
                if (got < tail) {
                    if (!(s->flags & ZSND_STREAM_ENDING)) {
                        s->flags |= ZSND_STREAM_ENDING;
                        s->endPos = end;
                        s->remaining = (end > play ? 0 : s->bufBytes) - play + end;
                    }
                    s->writePos = 0;
                    zsndStreamFill(s, s->refillBytes - tail, &end);
                } else {
                    s->writePos = 0;
                    if (s->refillBytes - tail) {
                        got = zsndStreamFill(s, s->refillBytes - tail, &end);
                        if (got < s->refillBytes - tail && !(s->flags & ZSND_STREAM_ENDING)) {
                            s->flags |= ZSND_STREAM_ENDING;
                            s->endPos = end;
                            s->remaining = end > play ? end - play : s->bufBytes - play + end;
                        }
                    }
                }
            }
            s->flags &= ~ZSND_STREAM_PRIMED;
            s->played -= s->refillBytes;
        }
        s->lastPlayPos = play;
    }
    LeaveCriticalSection(&g_zsndStreamLock);
    return 0;
}

/* VA 0x00470fb0, 652 bytes, [V] direct, status draft
 * Open `file`, make a stopped stream with a filled buffer. Errors: 0x201 open, 0x202 no free
 * stream, 0x204 buffer size 0. */
int zsndStreamCreate(ZsndStream **out, const ZsndFileDesc *file, int unused, uint32_t bufBytes, uint32_t flags)
{
    ZsndStreamFile *f;
    ZsndStream     *s = 0;
    DsBufferDesc    desc;
    WaveFormat      wfx;
    uint32_t        i, end, got;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x152);
    f = zsndStreamOpenFile(file, flags);
    if (!f) {
        *out = 0;
        return 0x201;
    }
    EnterCriticalSection(&g_zsndStreamLock);
    i = g_zsndNextStream;
    do {
        if (!(g_zsndStreams[i].flags & ZSND_STREAM_ALLOC)) {
            s = &g_zsndStreams[i];
            s->flags = ZSND_STREAM_ALLOC;
            s->state = ZSND_STREAM_STOPPED;
            g_zsndNextStream = i + 1 == ZSND_MAX_STREAMS ? 0 : i + 1;
            break;
        }
        if (++i == ZSND_MAX_STREAMS)
            i = 0;
    } while (i != g_zsndNextStream);
    LeaveCriticalSection(&g_zsndStreamLock);
    if (!s) {
        zsndStreamCloseFile(f);
        *out = 0;
        return 0x202;
    }
    s->queue.file = f;
    s->queue.loops = 1;
    s->queue.next = 0;
    s->fadeFlags = 0;
    s->volume = 0x7F;
    s->pan = 0;
    s->bufBytes = bufBytes ? bufBytes & ~3u : 0x10000;
    if (s->bufBytes == 0) {
        zsndStreamCloseFile(f);
        *out = 0;
        return 0x204;
    }
    zsndStreamSetRefill(s, 50);
    if (flags & ZSND_STRM_AUTOFREE)
        s->flags |= ZSND_STRM_AUTOFREE;
    else
        s->flags &= ~ZSND_STRM_AUTOFREE;
    wfx.wFormatTag = 1;
    wfx.nChannels = f->channels;
    wfx.nSamplesPerSec = f->rate;
    wfx.nBlockAlign = (uint16_t)(f->bits * f->channels / 8);
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    wfx.wBitsPerSample = f->bits;
    wfx.cbSize = 0;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags |= 0xE0;                     /* CTRLFREQUENCY | CTRLPAN | CTRLVOLUME */
    desc.dwBufferBytes = s->bufBytes;
    desc.lpwfxFormat = &wfx;
    DS_OK_AT(DS_CreateSoundBuffer(g_zsndcDS, &desc, &s->buffer), 0x184);
    s->writePos = 0;
    got = zsndStreamFill(s, s->bufBytes, &end);
    markEnd(s, got, s->bufBytes, end);
    s->flags |= ZSND_STREAM_PRIMED;
    s->played = 0;
    s->state = ZSND_STREAM_STOPPED;
    *out = s;
    return 0;
}

/* VA 0x004712f0, 197 bytes, [V] direct, status draft */
int zsndStreamDestroy(ZsndStream *s)
{
    ZsndStreamQueue *q, *next;
    int              i;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x1B1);
    EnterCriticalSection(&g_zsndStreamLock);
    DS_OK_AT(COM_Release(s->buffer), 0x1B6);
    for (q = &s->queue; q; q = next) {
        next = q->next;
        zsndStreamCloseFile(q->file);
        zsndStreamFreeQueue(q);              /* a no-op for the head entry inside the stream */
    }
    for (i = 0; i < ZSND_MAX_STREAMS; i++) {
        if (&g_zsndStreams[i] == s) {
            g_zsndNextStream = i;
            g_zsndStreams[i].flags = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_zsndStreamLock);
    return 0;
}

/* VA 0x00471400, 63 bytes, [V] direct, status draft */
int zsndStreamSetRefill(ZsndStream *s, uint32_t percent)
{
    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x1D3);
    s->refillBytes = s->bufBytes * percent / 100 & ~3u;
    return 0;
}

/* VA 0x00471450, 347 bytes, [V] direct, status draft
 * Switch a stream to another file of the same format (0x206 if the format differs). The music
 * player uses this to change tracks. */
int zsndStreamSetFile(ZsndStream *s, const ZsndFileDesc *file, uint32_t flags)
{
    ZsndStreamFile *f;
    uint32_t        end, got;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x1E9);
    f = zsndStreamOpenFile(file, flags);
    if (!f)
        return 0x201;
    if (f->channels != s->queue.file->channels || f->rate != s->queue.file->rate ||
        f->bits != s->queue.file->bits) {
        zsndStreamCloseFile(f);
        return 0x206;
    }
    EnterCriticalSection(&g_zsndStreamLock);
    zsndStreamCloseFile(s->queue.file);
    DS_OK_AT(DSB_Stop(s->buffer), 0x1F1);
    s->queue.file = f;
    s->queue.loops = 1;
    s->writePos = 0;
    got = zsndStreamFill(s, s->bufBytes, &end);
    markEnd(s, got, s->bufBytes, end);
    s->flags |= ZSND_STREAM_PRIMED;
    s->lastPlayPos = 0;
    s->played = 0;
    if (s->state == ZSND_STREAM_PLAYING)
        DS_OK_AT(DSB_Play(s->buffer, 0, 0, 1), 0x202);
    LeaveCriticalSection(&g_zsndStreamLock);
    return 0;
}

/* refill the buffer from the end of the data (after the queue changed) */
static void refillFromEnd(ZsndStream *s)
{
    uint32_t end, got, want;

    EnterCriticalSection(&g_zsndStreamLock);
    s->writePos = s->endPos;
    want = s->bufBytes - s->endPos;
    got = zsndStreamFill(s, want, &end);
    markEnd(s, got, want, end);
    s->played = 0;
    LeaveCriticalSection(&g_zsndStreamLock);
}

/* VA 0x00471610, 128 bytes, [I] row, status draft */
int zsndStreamSetLoops(ZsndStream *s, uint32_t loops)
{
    if (loops == 0)
        return 0x207;
    s->queue.loops = loops;
    /* [V] if the data already ended in the buffer, one of the new loops is spent right away */
    if ((s->flags & ZSND_STREAM_ENDING) && --s->queue.loops != 0)
        refillFromEnd(s);
    return 0;
}

/* VA 0x004716b0, 313 bytes, [I] row, status draft
 * Queue `file` behind the current one (ZSND_STRM_QUEUE_FRONT in flags: right after it).
 * Errors: 0x201 open, 0x206 format, 0x205 queue full. */
int zsndStreamQueueFile(ZsndStream *s, const ZsndFileDesc *file, uint32_t flags)
{
    ZsndStreamFile  *f;
    ZsndStreamQueue *q, *tail;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x242);
    f = zsndStreamOpenFile(file, flags);
    if (!f)
        return 0x201;
    if (f->channels != s->queue.file->channels || f->rate != s->queue.file->rate ||
        f->bits != s->queue.file->bits)
        return 0x206;                          /* [V] f stays open */
    q = zsndStreamAllocQueue();
    if (!q)
        return 0x205;
    EnterCriticalSection(&g_zsndStreamLock);
    tail = &s->queue;
    if (flags & ZSND_STRM_QUEUE_FRONT) {
        q->next = s->queue.next;
    } else {
        while (tail->next)
            tail = tail->next;
        q->next = 0;
    }
    tail->next = q;
    q->file = f;
    q->loops = 1;
    if (s->flags & ZSND_STREAM_ENDING)
        refillFromEnd(s);
    LeaveCriticalSection(&g_zsndStreamLock);
    return 0;
}

/* VA 0x00471840, 50 bytes, [V] direct, status draft */
int zsndStreamGetState(ZsndStream *s, uint32_t *state)
{
    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x27A);
    *state = s->state;
    return 0;
}

/* VA 0x00471890, 317 bytes, [V] direct, status draft
 * Paused -> resume; playing -> restart from the top; stopped -> start (from the top unless the
 * buffer still holds the start of the file). */
int zsndStreamPlay(ZsndStream *s)
{
    uint32_t end, got;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x28E);
    switch (s->state) {
    case ZSND_STREAM_PAUSED:
        s->pauseCount = 0;
        s->state = ZSND_STREAM_PLAYING;
        DS_OK_SHARED(DSB_Play(s->buffer, 0, 0, 1), 0x296);
        return 0;
    case ZSND_STREAM_PLAYING:
        DS_OK_AT(DSB_Stop(s->buffer), 0x29A);
        s->state = ZSND_STREAM_STOPPED;
        /* fall through */
    case ZSND_STREAM_STOPPED:
        break;
    default:
        return 0;
    }
    DS_OK_AT(DSB_SetCurrentPosition(s->buffer, 0), 0x29E);
    if (!(s->flags & ZSND_STREAM_PRIMED)) {
        zsndStreamRewind(s->queue.file);
        s->writePos = 0;
        got = zsndStreamFill(s, s->bufBytes, &end);
        markEnd(s, got, s->bufBytes, end);
        s->flags |= ZSND_STREAM_PRIMED;
    } else if (s->flags & ZSND_STREAM_ENDING) {
        s->remaining = s->endPos;
    }
    s->lastPlayPos = 0;
    s->played = 0;
    s->state = ZSND_STREAM_PLAYING;
    DS_OK_AT(DSB_Play(s->buffer, 0, 0, 1), 0x2B5);
    return 0;
}

/* VA 0x00471a20, 88 bytes, [V] direct, status draft */
int zsndStreamStop(ZsndStream *s)
{
    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x2C4);
    s->state = ZSND_STREAM_STOPPED;
    s->fadeFlags &= ~1u;
    DS_OK_AT(DSB_Stop(s->buffer), 0x2C9);
    return 0;
}

/* VA 0x00471aa0, 133 bytes, [V] direct, status draft
 * Counted pause: the stream plays again when every pause has been undone. */
int zsndStreamPause(ZsndStream *s, int pause)
{
    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x2D7);
    s->pauseCount += pause ? 1 : -1;
    if (s->pauseCount == 0) {
        s->state = ZSND_STREAM_PLAYING;
        DS_OK_AT(DSB_Play(s->buffer, 0, 0, 1), 0x2E5);
    } else {
        s->state = ZSND_STREAM_PAUSED;
        DS_OK_SHARED(DSB_Stop(s->buffer), 0x2E1);
    }
    return 0;
}

/* VA 0x00471b60, 168 bytes, [V] direct, status draft */
int zsndStreamSetVolume(ZsndStream *s, uint8_t volume, int8_t pan)
{
    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x2F5);
    s->fadeFlags = 0;
    DS_OK_AT(DSB_SetVolume(s->buffer, g_zsndStreamAtten[volume]), 0x2FC);
    s->volume = volume;
    DS_OK_AT(DSB_SetPan(s->buffer, pan * 78), 0x300);     /* +-127 -> +-9906 */
    s->pan = pan;
    return 0;
}

/* VA 0x00471c40, 121 bytes, [V] direct, status draft
 * Fade to `target` over `ms` (counted in game frames); flags bit 1: stop at the end. */
int zsndStreamFade(ZsndStream *s, uint32_t ms, uint8_t target, uint32_t flags)
{
    uint32_t frames;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x311);
    frames = g_zsndUpdateRate * ms / 1000;
    if (frames == 0)
        frames = 1;
    s->fadeTarget = target << 16;
    s->fadeVol = s->volume << 16;
    s->fadeFlags = (flags & 2) | 1;
    s->fadeStep = (s->fadeTarget - (s->volume << 16)) / (int32_t)frames;
    return 0;
}

/* VA 0x00471ce0, 31 bytes, [I] row, status draft */
int zsndStreamHold(int hold)
{
    if (hold)
        g_zsndStreamHoldCount++;
    else
        g_zsndStreamHoldCount--;
    return 0;
}

/* VA 0x00471d10, 13 bytes, [I] row, status draft */
int zsndStreamGetCaps(uint32_t *caps)
{
    *caps = 1;
    return 0;
}

/* VA 0x00471d30, 106 bytes, [I] row, status draft */
void zsndStreamPlayOnce(const ZsndFileDesc *file, uint32_t bufBytes, uint8_t volume, int8_t pan)
{
    ZsndStream *s;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x345);
    if (zsndStreamCreate(&s, file, 0, bufBytes, ZSND_STRM_AUTOFREE) == 0 &&
        zsndStreamSetVolume(s, volume, pan) == 0)
        zsndStreamPlay(s);
}

/* VA 0x00471dc0, 494 bytes, [V] direct, status draft
 * Write `bytes` at writePos from the queue (looping / moving to the next file as needed); pad
 * with silence when the data runs out. Returns the bytes of real data; *endPos = where it ends. */
uint32_t zsndStreamFill(ZsndStream *s, uint32_t bytes, uint32_t *endPos)
{
    uint8_t *p1, *p2, *dst;
    uint32_t n1, n2, got, total = 0, left = bytes;
    ZsndStreamQueue *head, *next;

    ASSERT_AT(g_zsndStreamInitialized, "fzsndStreamInitialized", 0x364);
    if (bytes == 0)
        return 0;
    EnterCriticalSection(&g_zsndStreamLock);
    if (DSB_Lock(s->buffer, s->writePos, bytes, (void **)&p1, &n1, (void **)&p2, &n2, 0) != 0) {
        DSB_Restore(s->buffer);
        DS_OK_AT(DSB_Lock(s->buffer, s->writePos, bytes, (void **)&p1, &n1, (void **)&p2, &n2, 0), 0x36D);
    }
    ASSERT_AT(n2 == 0, "dwLen2 == 0", 0x36F);   /* callers never wrap */
    head = &s->queue;
    dst = p1;
    for (;;) {
        got = zsndStreamRead(head->file, dst, left);
        total += got;
        dst += got;
        if (got >= left)
            break;
        left -= got;
        if (head->loops == 0) {                 /* this file is done: move on to the next one */
            next = head->next;
            if (!next)
                break;
            head->file = next->file;
            head->loops = next->loops;
            head->next = next->next;
            zsndStreamFreeQueue(next);
        } else if (--head->loops != 0) {
            zsndStreamRewind(head->file);
        }
    }
    if (total < bytes) {
        memset(dst, head->file->bits != 16 ? 0x80 : 0x00, bytes - total);
        *endPos = s->writePos + total;
    }
    DS_OK_AT(DSB_Unlock(s->buffer, p1, n1, p2, n2), 0x395);
    s->writePos += bytes;
    if (s->writePos >= s->bufBytes)
        s->writePos -= s->bufBytes;
    LeaveCriticalSection(&g_zsndStreamLock);
    return total;
}

/* VA 0x00472030, 365 bytes, [I] row, status draft */
ZsndStreamFile *zsndStreamOpenFile(const ZsndFileDesc *file, uint32_t flags)
{
    ZsndStreamFile *f;
    uint32_t        i = g_zsndNextStreamFile;

    while (g_zsndStreamFiles[i].size != 0) {
        if (++i == ZSND_MAX_STREAM_FILES)
            i = 0;
        if (i == g_zsndNextStreamFile)
            return 0;
    }
    f = &g_zsndStreamFiles[i];
    g_zsndNextStreamFile = i + 1 == ZSND_MAX_STREAM_FILES ? 0 : i + 1;
    f->flags = 0;
    f->hFile = CreateFileA(file->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | (file->offset ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN),
                           NULL);
    if (f->hFile != INVALID_HANDLE_VALUE) {
        if (SetFilePointer(f->hFile, file->offset, NULL, FILE_BEGIN) == file->offset) {
            f->start = file->offset;
            f->size = file->size;
            f->channels = (flags & ZSND_STRM_STEREO) ? 2 : 1;
            f->rate = (flags & ZSND_STRM_44K) ? 44100 : (flags & ZSND_STRM_16K) ? 16000
                    : (flags & ZSND_STRM_11K) ? 11025 : 22050;
            f->bits = (flags & ZSND_STRM_8BIT) ? 8 : 16;
            if (!(flags & ZSND_STRM_PCM)) {      /* IMA-ADPCM decodes to 16-bit mono */
                f->channels = 1;
                f->imaStep = 7;
                f->flags |= 1;
                f->imaPred = 0;
                f->imaIndex = 0;
            }
            f->pos = 0;
            return f;
        }
        CloseHandle(f->hFile);
    }
    /* [V] "un-claim" frees the slot before this one, not slot i (which was never marked) */
    g_zsndNextStreamFile = i == 0 ? ZSND_MAX_STREAM_FILES - 1 : i - 1;
    g_zsndStreamFiles[g_zsndNextStreamFile].size = 0;
    return 0;
}

/* VA 0x00472200, 60 bytes, [I] row, status draft */
void zsndStreamCloseFile(ZsndStreamFile *f)
{
    int i;

    CloseHandle(f->hFile);
    for (i = 0; i < ZSND_MAX_STREAM_FILES; i++) {
        if (f == &g_zsndStreamFiles[i]) {
            g_zsndNextStreamFile = i;
            g_zsndStreamFiles[i].size = 0;
            return;
        }
    }
}

/* VA 0x00472250, 270 bytes, [I] row, status draft
 * Read `bytes` of decoded audio; returns the bytes produced (0 at the end of the file). */
uint32_t zsndStreamRead(ZsndStreamFile *f, void *dst, uint32_t bytes)
{
    uint8_t  chunk[0x4000];                 /* _chkstk frame of 0x4008 */
    uint32_t want, n, got, total = 0;
    int16_t *out = (int16_t *)dst;

    if (!(f->flags & 1)) {
        n = bytes < f->size - f->pos ? bytes : f->size - f->pos;
        if (n == 0)
            return 0;
        ReadFile(f->hFile, dst, n, (DWORD *)&got, NULL);
        f->pos += got;
        return got;
    }
    /* IMA-ADPCM: one input byte = two 16-bit samples = four output bytes */
    want = bytes >> 2;
    for (;;) {
        n = f->size - f->pos;
        if ((want < n ? want : n) > sizeof(chunk))
            n = sizeof(chunk);
        else if (want < n)
            n = want;
        if (n == 0)
            return total;
        ReadFile(f->hFile, chunk, n, (DWORD *)&got, NULL);
        f->pos += n;
        want -= n;
        zsndImaDecode(out, chunk, got * 2, &f->imaPred, &f->imaStep, &f->imaIndex);
        out += got * 2;
        total += got * 4;
    }
}

/* VA 0x004723b0, 61 bytes, [I] row, status draft */
int zsndStreamRewind(ZsndStreamFile *f)
{
    SetFilePointer(f->hFile, f->start, NULL, FILE_BEGIN);
    f->pos = 0;
    if (f->flags & 1) {
        f->imaPred = 0;
        f->imaStep = 7;
        f->imaIndex = 0;
    }
    return 0;
}

/* VA 0x00472400, 86 bytes, [I] row, status draft */
ZsndStreamQueue *zsndStreamAllocQueue(void)
{
    uint32_t i = g_zsndNextQueue;

    do {
        if (g_zsndStreamQueue[i].loops == 0) {  /* loops != 0 = in use; the caller sets it */
            g_zsndNextQueue = i + 1 == ZSND_MAX_STREAM_QUEUE ? 0 : i + 1;
            return &g_zsndStreamQueue[i];
        }
        if (++i == ZSND_MAX_STREAM_QUEUE)
            i = 0;
    } while (i != g_zsndNextQueue);
    return 0;
}

/* VA 0x00472480, 48 bytes, [I] row, status draft */
void zsndStreamFreeQueue(ZsndStreamQueue *q)
{
    int i;

    for (i = 0; i < ZSND_MAX_STREAM_QUEUE; i++) {
        if (q == &g_zsndStreamQueue[i]) {
            g_zsndNextQueue = i;
            g_zsndStreamQueue[i].loops = 0;
            return;
        }
    }
}

/* VA 0x004724c0, 274 bytes, [I] row, status draft
 * IMA-ADPCM, low nibble first; state carried in *pred / *step / *index. Ported 1:1 by
 * dmbmx.formats.audio.ima_decode (tests/test_decomp_dm055.py checks both on every music track). */
void zsndImaDecode(int16_t *dst, const uint8_t *src, uint32_t n, int32_t *pred, int32_t *step, int32_t *index)
{
    int32_t  p = *pred, st = *step, idx = *index, diff;
    uint32_t i, nib = 0;
    uint8_t  byte = 0;

    for (i = 0; i != n; i++) {
        if ((i & 1) == 0) {
            byte = src[i >> 1];
            nib = byte & 0xF;
        } else {
            nib = byte >> 4;
        }
        diff = 0;
        if (nib & 4)
            diff = st;
        if (nib & 2)
            diff += st >> 1;
        if (nib & 1)
            diff += st >> 2;
        diff += st >> 3;
        if (nib & 8)
            diff = -diff;
        p += diff;
        if (p > 32767)
            p = 32767;
        else if (p < -32768)
            p = -32768;
        dst[i] = (int16_t)p;
        idx += g_imaIndexTable[nib];
        if (idx > 88)
            idx = 88;
        else if (idx < 0)
            idx = 0;
        st = g_imaStepTable[idx];
    }
    *pred = p;
    *step = st;
    *index = idx;
}

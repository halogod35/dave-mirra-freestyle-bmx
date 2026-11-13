/*
 * NgEngine/Zge/Pc/pcmovie.c
 * Original: unknown (no path string; name invented in the original style, DM-040)
 * Evidence: [I] the "Movie : " literals 0x005634DC-0x0056353C (DM-040 seed), the PcMovie layout shared
 * by every function, link order after zge.c. DM-061 created the eight hole functions and moved
 * 0x004A1050 in from the pcmovie/zpanorama gap.
 * 22 functions, VA 0x0049fb30-0x004a105a.
 * DM-061 (draft). Non-matching C (AD-8). Types: zpc.h. Description: docs/re/zge.md 5.
 *
 * An AVI player: the streams of one file, the first video stream drawn with DrawDib at the current
 * time (GetTickCount based), the first audio stream streamed through waveOut in up to sixteen
 * 32 KB buffers that the window procedure refills on MM_WOM_DONE (pcmovieAudioDone). The game's
 * only user is zrwZglPlayMovie (DM-059); the shipped bin\movie folder is empty (DM-056), so no
 * movie is ever played.
 *
 * The audio stop sequence (waveOutReset + close + free the format) is inlined in five places in
 * the exe; the C calls pcmovieStopAudio / pcmovieCloseAudio instead.
 */
#include "decomp.h"
#include "zpc.h"

#define FCC_VIDS 0x73646976u    /* 'vids' */
#define FCC_AUDS 0x73647561u    /* 'auds' */
#define WAVE_MAPPER       0xFFFFFFFFu
#define CALLBACK_WINDOW   0x00010000u
#define WHDR_DONE         0x00000001u
#define DDF_BACKGROUND    0x0200u

typedef struct {               /* WAVEHDR, followed by PCMOVIE_BUFFER_SIZE bytes of data */
    char    *lpData;           /* +0x00 */
    uint32_t dwBufferLength;   /* +0x04 */
    uint32_t dwBytesRecorded, dwUser, dwFlags, dwLoops;
    void    *lpNext;
    uint32_t reserved;
} PcWaveHdr;                   /* 0x20 */

typedef struct {               /* AVISTREAMINFOA, 0x8C */
    uint32_t fccType, fccHandler, dwFlags, dwCaps;
    uint16_t wPriority, wLanguage;
    uint32_t dwScale, dwRate, dwStart, dwLength, dwInitialFrames, dwSuggestedBufferSize;
    uint32_t dwQuality, dwSampleSize;             /* +0x2C, +0x30 */
    uint8_t  rest[0x8C - 0x34];
} PcAviStreamInfo;

typedef struct { uint16_t wFormatTag, nChannels; uint32_t nSamplesPerSec, nAvgBytesPerSec; } PcWaveFormat;

/* AVIFIL32 / MSVFW32 / WINMM / USER32 imports */
void    __stdcall AVIFileInit(void);
void    __stdcall AVIFileExit(void);
int32_t __stdcall AVIFileOpenA(void **file, const char *path, uint32_t mode, void *handler);
int32_t __stdcall AVIFileGetStream(void *file, void **stream, uint32_t type, int32_t n);
uint32_t __stdcall AVIFileRelease(void *file);
uint32_t __stdcall AVIStreamAddRef(void *stream);
uint32_t __stdcall AVIStreamRelease(void *stream);
int32_t __stdcall AVIStreamInfoA(void *stream, PcAviStreamInfo *info, int32_t size);
int32_t __stdcall AVIStreamStart(void *stream);
int32_t __stdcall AVIStreamLength(void *stream);
int32_t __stdcall AVIStreamSampleToTime(void *stream, int32_t sample);
int32_t __stdcall AVIStreamTimeToSample(void *stream, int32_t ms);
int32_t __stdcall AVIStreamReadFormat(void *stream, int32_t pos, void *format, int32_t *size);
int32_t __stdcall AVIStreamRead(void *stream, int32_t start, int32_t samples, void *buf, int32_t size,
                                int32_t *bytes, int32_t *samplesRead);
void   *__stdcall AVIStreamGetFrameOpen(void *stream, void *want);
void   *__stdcall AVIStreamGetFrame(void *getFrame, int32_t pos);
int32_t __stdcall AVIStreamGetFrameClose(void *getFrame);
int32_t __stdcall AVISaveOptionsFree(int count, PcAviOptions **options);
void   *__stdcall DrawDibOpen(void);
int     __stdcall DrawDibClose(void *dd);
int     __stdcall DrawDibDraw(void *dd, HDC dc, int x, int y, int w, int h, void *dib, void *bits,
                              int sx, int sy, int sw, int sh, uint32_t flags);
uint32_t __stdcall waveOutOpen(void **wo, uint32_t device, const void *format, DWORD_PTR cb,
                               DWORD_PTR inst, uint32_t flags);
uint32_t __stdcall waveOutClose(void *wo);
uint32_t __stdcall waveOutPrepareHeader(void *wo, PcWaveHdr *hdr, uint32_t size);
uint32_t __stdcall waveOutUnprepareHeader(void *wo, PcWaveHdr *hdr, uint32_t size);
uint32_t __stdcall waveOutWrite(void *wo, PcWaveHdr *hdr, uint32_t size);
uint32_t __stdcall waveOutPause(void *wo);
uint32_t __stdcall waveOutRestart(void *wo);
uint32_t __stdcall waveOutReset(void *wo);
int      __stdcall sndPlaySoundA(const char *sound, uint32_t flags);
uint32_t __stdcall GetTickCount(void);
HDC      __stdcall GetDC(HWND hwnd);
int      __stdcall ReleaseDC(HWND hwnd, HDC dc);
int      __stdcall GetClientRect(HWND hwnd, int32_t *rect);

/* DirectDraw (RenderWare's D3D7 driver): the primary surface and its DirectDraw object */
typedef struct DDObj { void **vtbl; } DDObj;
extern DDObj *g_rwDDPrimarySurface;   /* 0x0069730C IDirectDrawSurface7 */
extern DDObj *g_rwDDInterface;        /* 0x006977CC IDirectDraw7 (GetDDInterface writes it) */

/* VA 0x0049fb30, 175 bytes, [I] row, status draft */
void pcmovieInit(PcMovie *m)
{
    int i;

    g_pfnDebugPrintf("Movie : Init\n");
    AVIFileInit();
    for (i = 0; i < PCMOVIE_MAX_STREAMS; i++)
        m->optionPtr[i] = &m->options[i];
    for (i = 0; i < PCMOVIE_MAX_STREAMS; i++) {
        m->getFrame[i] = 0;
        m->drawDib[i] = 0;
        m->stream[i] = 0;
        zstringMemSet(&m->options[i], 0, sizeof(PcAviOptions));
    }
    m->numStreams = 0;
    m->waveOut = 0;
    m->time = 0;
    m->audioOpen = 0;
    m->audioEnabled = 1;
    m->playing = 0;
    m->queued = 0;
    m->audioStream = -1;
    m->videoStream = -1;
    m->fullscreen = 0;
}

/* VA 0x0049fbe0, 270 bytes, [I] row, status draft */
void pcmovieShutdown(PcMovie *m)
{
    g_pfnDebugPrintf("Movie : Shutdown\n");
    pcmovieStop(m);                     /* inlined */
    pcmovieClose(m);
    AVIFileExit();
}

/* VA 0x0049fcf0, 666 bytes, [I] row, status draft */
int pcmovieOpenAudio(PcMovie *m, DWORD_PTR callback)
{
    PcAviStreamInfo info;
    int32_t size;
    void *s;
    PcWaveHdr *hdr;

    if (m->audioStream < 0 || !m->audioEnabled)
        return 0;
    if (m->waveOut)
        return 1;
    s = m->stream[m->audioStream];
    AVIStreamInfoA(s, &info, sizeof(info));
    m->sampleSize = info.dwSampleSize;
    if (m->sampleSize <= 0 || m->sampleSize > PCMOVIE_BUFFER_SIZE)
        return 0;
    AVIStreamReadFormat(s, 0, 0, &size);
    m->waveFormat = zmemAlloc(size);
    if (m->waveFormat == 0)
        return 0;
    AVIStreamReadFormat(s, 0, m->waveFormat, &size);
    m->samplesPerSec = (uint16_t)((PcWaveFormat *)m->waveFormat)->nSamplesPerSec;
    m->avgBytesPerSec = (uint16_t)((PcWaveFormat *)m->waveFormat)->nAvgBytesPerSec;
    if (waveOutOpen(&m->waveOut, WAVE_MAPPER, m->waveFormat, callback, 0, CALLBACK_WINDOW) != 0) {
        sndPlaySoundA(0, 0);            /* free the device if a sound holds it, then retry */
        if (waveOutOpen(&m->waveOut, WAVE_MAPPER, m->waveFormat, callback, 0, CALLBACK_WINDOW) != 0)
            return 0;
    }
    for (m->numBuffers = 0; m->numBuffers < PCMOVIE_MAX_BUFFERS; m->numBuffers++) {
        hdr = (PcWaveHdr *)zmemAlloc(sizeof(PcWaveHdr) + PCMOVIE_BUFFER_SIZE);
        m->buffer[m->numBuffers] = hdr;
        if (hdr == 0)
            break;
        hdr->dwFlags = WHDR_DONE;
        hdr->lpData = (char *)(hdr + 1);
        hdr->dwBufferLength = PCMOVIE_BUFFER_SIZE;
        if (waveOutPrepareHeader(m->waveOut, hdr, sizeof(PcWaveHdr)) != 0) {
            zmemFree(hdr);
            break;
        }
    }
    if (m->numBuffers > 1) {
        m->audioOpen = 1;
        m->queued = 0;
        m->nextBuffer = 0;
        return 1;
    }
    pcmovieCloseAudio(m);               /* inlined; the format block is not freed here */
    return 0;
}

/* VA 0x0049ff90, 161 bytes, [I] row, status draft */
void pcmovieCloseAudio(PcMovie *m)
{
    g_pfnDebugPrintf("Movie : Close Audio\n");
    if (m->waveOut) {
        while (m->numBuffers != 0) {
            m->numBuffers--;
            waveOutUnprepareHeader(m->waveOut, (PcWaveHdr *)m->buffer[m->numBuffers], sizeof(PcWaveHdr));
            zmemFree(m->buffer[m->numBuffers]);
        }
        waveOutClose(m->waveOut);
        m->waveOut = 0;
    }
    m->queued = 0;
    m->nextBuffer = 0;
    m->audioOpen = 0;
}

/* VA 0x004a0040, 555 bytes, [I] row, status draft */
int pcmovieFillAudio(PcMovie *m)
{
    int32_t n, read;
    PcWaveHdr *hdr;

    g_pfnDebugPrintf("Movie : Fill Audio\n");
    if (!m->audioOpen)
        return 1;
    while (m->queued < m->numBuffers) {
        if (m->audioPos >= m->audioEnd) {
            if (!m->loop)
                break;
            m->audioPos = m->audioStart;
        }
        n = PCMOVIE_BUFFER_SIZE / m->sampleSize;
        if (n > m->audioEnd - m->audioPos)
            n = m->audioEnd - m->audioPos;
        hdr = (PcWaveHdr *)m->buffer[m->nextBuffer];
        AVIStreamRead(m->stream[m->audioStream], m->audioPos, n, hdr->lpData, PCMOVIE_BUFFER_SIZE,
                      (int32_t *)&hdr->dwBufferLength, &read);
        if (read != n)
            return 0;
        m->audioPos += read;
        if (waveOutWrite(m->waveOut, hdr, sizeof(PcWaveHdr)) != 0)
            return 0;
        m->queued++;
        if (++m->nextBuffer >= m->numBuffers)
            m->nextBuffer = 0;
    }
    if (m->queued == 0 && m->audioPos >= m->audioEnd)
        pcmovieStopAudio(m);            /* inlined: everything played */
    return 1;
}

/* VA 0x004a0270, 163 bytes, [I] row, status draft */
int pcmovieStartAudio(PcMovie *m, DWORD_PTR callback)
{
    void *s;
    int32_t sample, length, start;

    if (m->audioStream == -1)
        return 0;
    s = m->stream[m->audioStream];
    sample = AVIStreamTimeToSample(s, m->time);
    length = AVIStreamLength(s);
    start = AVIStreamStart(s);
    pcmovieOpenAudio(m, callback);
    if (sample >= length + start)
        return 0;
    if (!m->playing) {
        waveOutPause(m->waveOut);
        m->audioStart = sample;
        m->audioPos = sample;
    }
    m->audioEnd = length + start;
    pcmovieFillAudio(m);
    waveOutRestart(m->waveOut);
    return 1;
}

/* VA 0x004a0320, 221 bytes, [I] row, status draft */
void pcmovieStopAudio(PcMovie *m)
{
    if (m->audioOpen && m->audioStream != -1 && m->waveOut) {
        waveOutReset(m->waveOut);
        pcmovieCloseAudio(m);           /* inlined */
        zmemFree(m->waveFormat);
    }
}

/* VA 0x004a0400, 547 bytes, [I] row, status draft */
void pcmovieScanStreams(PcMovie *m)
{
    PcAviStreamInfo info;
    int32_t size, t;
    int i;

    m->timeStart = 0x7FFFFFFF;
    m->timeEnd = 0;
    for (i = 0; i < m->numStreams; i++) {
        void *s = m->stream[i];
        PcAviOptions *o = &m->options[i];

        AVIStreamInfoA(s, &info, sizeof(info));
        m->optionPtr[i] = o;
        zstringMemSet(o, 0, sizeof(PcAviOptions));
        o->fccType = info.fccType;
        if (info.fccType == FCC_VIDS) {
            o->dwFlags = 0x0E;          /* AVICOMPRESSF_DATARATE | KEYFRAMES | VALID */
            o->fccHandler = 0;
            o->dwQuality = 0xFFFFFFFFu;
            o->dwKeyFrameEvery = 0xFFFFFFFFu;
            o->dwBytesPerSecond = 0;
            o->dwInterleaveEvery = 1;
        } else if (info.fccType == FCC_AUDS) {
            o->dwFlags |= 0x08;         /* AVICOMPRESSF_VALID */
            o->dwInterleaveEvery = 1;
            AVIStreamReadFormat(s, AVIStreamStart(s), 0, &size);
            o->cbFormat = size;
            if (size)
                o->lpFormat = zmemAlloc(size);
            if (o->lpFormat)
                AVIStreamReadFormat(s, AVIStreamStart(s), o->lpFormat, &size);
        }
        t = AVIStreamSampleToTime(s, AVIStreamStart(s));
        if ((uint32_t)t <= (uint32_t)m->timeStart)
            m->timeStart = t;
        t = AVIStreamSampleToTime(s, AVIStreamLength(s) + AVIStreamStart(s));
        if ((uint32_t)t >= (uint32_t)m->timeEnd)
            m->timeEnd = t;
        if (info.fccType == FCC_VIDS) {
            m->getFrame[i] = AVIStreamGetFrameOpen(s, 0);
            if (m->getFrame[i]) {
                m->drawDib[i] = DrawDibOpen();
                if (m->videoStream < 0)
                    m->videoStream = i;
            }
        } else if (info.fccType == FCC_AUDS) {
            m->getFrame[i] = 0;
            m->drawDib[i] = 0;
            if (m->audioStream < 0)
                m->audioStream = i;
        }
    }
    m->timeLength = m->timeEnd - m->timeStart;
    if (m->timeLength == 0)
        m->timeLength = 1;
}

/* VA 0x004a0630, 91 bytes, [I] row, status draft */
int pcmovieOpenFile(PcMovie *m, void *aviFile, int unused)
{
    int n = 0;
    void *s;

    while (AVIFileGetStream(aviFile, &s, 0, n) == 0)
        m->stream[n++] = s;
    AVIFileRelease(aviFile);
    if (n == 0)
        return 0;
    m->numStreams = n;
    pcmovieScanStreams(m);
    return 1;
}

/* VA 0x004a0690, 83 bytes, [I] row, status draft */
int pcmovieOpenStreams(PcMovie *m, int count, void *const *streams, int unused)
{
    int i;

    if (count <= 0)
        return 0;
    for (i = 0; i < count; i++) {
        m->stream[i] = streams[i];
        AVIStreamAddRef(streams[i]);
    }
    m->numStreams = count;
    pcmovieScanStreams(m);
    return 1;
}

/* VA 0x004a06f0, 135 bytes, [I] row, status draft */
int pcmovieOpen(PcMovie *m, const char *path, int unused)
{
    void *file, *s;
    int n = 0;

    pcmovieClose(m);
    m->audioStream = -1;
    m->videoStream = -1;
    if (AVIFileOpenA(&file, path, 0 /* OF_READ */, 0) != 0)
        return 0;
    while (AVIFileGetStream(file, &s, 0, n) == 0)    /* pcmovieOpenFile, inlined */
        m->stream[n++] = s;
    AVIFileRelease(file);
    if (n == 0)
        return 0;
    m->numStreams = n;
    pcmovieScanStreams(m);
    return 1;
}

/* VA 0x004a0780, 540 bytes, [I] row, status draft */
void pcmovieClose(PcMovie *m)
{
    int i;

    g_pfnDebugPrintf("Movie : Close\n");
    if (m->playing == 1) {
        pcmovieStopAudio(m);            /* inlined */
        m->playing = 0;
    }
    for (i = 0; i < m->numStreams; i++) {
        if (m->getFrame[i]) {
            AVIStreamGetFrameClose(m->getFrame[i]);
            m->getFrame[i] = 0;
        }
        if (m->drawDib[i]) {
            DrawDibClose(m->drawDib[i]);
            m->drawDib[i] = 0;
        }
        if (m->optionPtr[i]->lpFormat)
            zmemFree(m->optionPtr[i]->lpFormat);
    }
    AVISaveOptionsFree(m->numStreams, m->optionPtr);
    for (i = 0; i < m->numStreams; i++)
        AVIStreamRelease(m->stream[i]);
    pcmovieCloseAudio(m);               /* inlined */
    m->audioStream = -1;
    m->videoStream = -1;
    m->numStreams = 0;
}

/* VA 0x004a09a0, 8 bytes, [I] row, status draft */
int32_t pcmovieGetLength(PcMovie *m)
{
    return m->timeLength;
}

/* VA 0x004a09b0, 12 bytes, [I] row, status draft */
void pcmovieSetTime(PcMovie *m, int32_t ms)
{
    m->time = ms;
}

/* VA 0x004a09c0, 8 bytes, [I] row, status draft */
int32_t pcmovieGetTime(PcMovie *m)
{
    return m->time;
}

/* VA 0x004a09d0, 153 bytes, [I] row, status draft */
void pcmoviePlay(PcMovie *m, HWND hwnd, int fullscreen, int loop, const int32_t *rect)
{
    if (m->playing)
        return;
    m->fullscreen = fullscreen;
    m->loop = loop;
    m->startTick = GetTickCount() - m->time;
    m->hwnd = hwnd;
    pcmovieStartAudio(m, (DWORD_PTR)hwnd);   /* MM_WOM_DONE goes to the window */
    if (rect) {
        m->rect[0] = rect[0];
        m->rect[1] = rect[1];
        m->rect[2] = rect[2];
        m->rect[3] = rect[3];
    } else {
        GetClientRect(hwnd, m->rect);
    }
    m->playing = 1;
}

/* VA 0x004a0a70, 19 bytes, [I] row, status draft */
void pcmovieAudioDone(PcMovie *m)
{
    m->queued--;
    pcmovieFillAudio(m);
}

/* VA 0x004a0a90, 544 bytes, [I] row, status draft */
void pcmovieUpdate(PcMovie *m, HDC dc, const int32_t *rect)
{
    int ownDC = 0;
    uint32_t t;

    if (rect) {
        m->rect[0] = rect[0];
        m->rect[1] = rect[1];
        m->rect[2] = rect[2];
        m->rect[3] = rect[3];
    }
    if (dc == 0) {
        if (m->fullscreen && g_rwDDPrimarySurface) {
            /* IDirectDrawSurface7::GetDDInterface, IDirectDraw7::FlipToGDISurface */
            ((int32_t (__stdcall *)(DDObj *, DDObj **))g_rwDDPrimarySurface->vtbl[0x90 / 4])(g_rwDDPrimarySurface, &g_rwDDInterface);
            ((int32_t (__stdcall *)(DDObj *))g_rwDDInterface->vtbl[0x28 / 4])(g_rwDDInterface);
        }
        dc = GetDC(m->hwnd);
        ownDC = 1;
    }
    if (m->playing == 1) {
        t = GetTickCount() - m->startTick;
        if (t > (uint32_t)m->timeLength) {
            g_pfnDebugPrintf("Movie : End Of Movie\n");
            if (m->loop == 1) {
                m->startTick = GetTickCount();
                m->time = 0;
                t = 0;
            } else {
                t = m->timeLength - 1;  /* hold the last frame */
                pcmovieStop(m);         /* inlined */
            }
        }
        m->time = t;
    } else {
        t = m->time;
    }
    pcmovieDraw(m, dc, m->rect[0], m->rect[1], m->rect[2], m->rect[3], t);
    if (ownDC)
        ReleaseDC(m->hwnd, dc);
}

/* VA 0x004a0cb0, 242 bytes, [I] row, status draft */
void pcmovieStop(PcMovie *m)
{
    if (m->playing == 1) {
        pcmovieStopAudio(m);            /* inlined */
        m->playing = 0;
    }
}

/* A stream's sample at `ms`; past the end the time is counted from the end again. */
static int32_t pcmovieSampleAt(void *s, int32_t ms)
{
    int32_t end = AVIStreamSampleToTime(s, AVIStreamLength(s) + AVIStreamStart(s));

    if (end < ms)
        return AVIStreamTimeToSample(s, ms - end) + AVIStreamLength(s) + AVIStreamStart(s);
    return AVIStreamTimeToSample(s, ms);
}

/* VA 0x004a0db0, 387 bytes, [I] row, status draft */
void pcmovieDraw(PcMovie *m, HDC dc, int x0, int y0, int x1, int y1, int32_t ms)
{
    PcAviStreamInfo info;
    int i;

    if (ms == -1)
        ms = m->time;
    else
        m->time = ms;
    for (i = 0; i < m->numStreams; i++) {
        void *s = m->stream[i];
        int32_t sample;
        void *dib;

        AVIStreamInfoA(s, &info, sizeof(info));
        if (info.fccType != FCC_VIDS || m->getFrame[i] == 0)
            continue;
        sample = pcmovieSampleAt(s, ms);
        if (m->getFrame[i] && sample >= AVIStreamStart(s)
            && (dib = AVIStreamGetFrame(m->getFrame[i], sample)) != 0)
            DrawDibDraw(m->drawDib[i], dc, x0, y0, x1 - x0, y1 - y0, dib, 0, 0, 0, -1, -1,
                        i != m->videoStream ? DDF_BACKGROUND : 0);
    }
}

/* VA 0x004a0f40, 269 bytes, [I] row, status draft */
void *pcmovieGetFrame(PcMovie *m, int32_t ms)
{
    PcAviStreamInfo info;
    int i;

    if (ms == -1)
        ms = m->time;
    else
        m->time = ms;
    for (i = 0; i < m->numStreams; i++) {
        void *s = m->stream[i];
        int32_t sample;

        AVIStreamInfoA(s, &info, sizeof(info));
        if (info.fccType != FCC_VIDS || m->getFrame[i] == 0)
            continue;
        sample = pcmovieSampleAt(s, ms);  /* the first video stream decides */
        if (sample < AVIStreamStart(s))
            return 0;
        return AVIStreamGetFrame(m->getFrame[i], sample);
    }
    return 0;
}

/* VA 0x004a1050, 11 bytes, [I] row, status draft */
int pcmovieIsPlaying(PcMovie *m)
{
    return m->playing;
}

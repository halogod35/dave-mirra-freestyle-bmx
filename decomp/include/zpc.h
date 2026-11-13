/*
 * zpc.h - the PC platform layer of ZGE (DM-061): NgEngine/Zge/Pc/pcinput.c (DirectInput 7 keyboard
 * and joystick) and pcmovie.c (AVI playback through AVIFile, DrawDib and waveOut).
 *
 * Offsets [V] (field use in these files); names [I]; the DirectInput / AVIFile structures are the
 * DirectX 7 / Video for Windows SDK ones. Description: docs/re/input-camera.md 2.4, docs/re/zge.md 5.
 */
#ifndef ZPC_H
#define ZPC_H
#include "dmtypes.h"

/* ------------------------------------------------------------------ pcinput.c */
struct ZInputDevice;

typedef struct PcGuid {           /* GUID */
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t  d4[8];
} PcGuid;                          /* 0x10 */

/* A joystick map: the product names / product GUIDs it applies to, then (button, channel) byte
 * pairs ending with 0xFF (the default map has neither list). [V] tables 0x00561EA8-0x00561F37. */
typedef struct PcJoyMapHeader {
    const char *const  *names;     /* +0x00 NULL-terminated, compared with DIDEVICEINSTANCE.tszProductName */
    const void *const  *products;  /* +0x04 NULL-terminated GUID pointers (guidProduct) */
    /* uint8_t pairs[] follow at +0x08 */
} PcJoyMapHeader;                  /* 0x08 + pairs */

/* The platform record behind a ZInputDevice (0x14, g_pcinputPool). */
typedef struct PcInputDevice {
    struct ZInputDevice *owner;    /* +0x00 */
    void       *joystick;          /* +0x04 IDirectInputDevice7A (c_dfDIJoystick), NULL = keyboard only */
    uint32_t    flags;             /* +0x08 bit 0: joystick acquired */
    const uint8_t *keymap;         /* +0x0C (DIK scan code, channel) pairs, 0xFF-terminated */
    const PcJoyMapHeader *joyMap;  /* +0x10 */
} PcInputDevice;                   /* 0x14 */

/* ------------------------------------------------------------------ pcmovie.c */
#define PCMOVIE_MAX_STREAMS 8
#define PCMOVIE_MAX_BUFFERS 16
#define PCMOVIE_BUFFER_SIZE 0x8000

/* AVICOMPRESSOPTIONS (vfw.h), one per stream; pcmovie fills fccType, the format and dwFlags. */
typedef struct PcAviOptions {
    uint32_t fccType;              /* +0x00 'vids' / 'auds' */
    uint32_t fccHandler;           /* +0x04 */
    uint32_t dwKeyFrameEvery;      /* +0x08 */
    uint32_t dwQuality;            /* +0x0C */
    uint32_t dwBytesPerSecond;     /* +0x10 */
    uint32_t dwFlags;              /* +0x14 */
    void    *lpFormat;             /* +0x18 the audio stream's WAVEFORMATEX copy */
    uint32_t cbFormat;             /* +0x1C */
    void    *lpParms;              /* +0x20 */
    uint32_t cbParms;              /* +0x24 */
    uint32_t dwInterleaveEvery;    /* +0x28 */
} PcAviOptions;                    /* 0x2C */

typedef struct PcMovie {
    int          videoStream;      /* +0x000 index of the first video stream, -1 */
    int          audioStream;      /* +0x004 index of the first audio stream, -1 */
    int          numStreams;       /* +0x008 */
    void        *stream[8];        /* +0x00C PAVISTREAM */
    int32_t      timeStart;        /* +0x02C ms, min over the streams */
    int32_t      timeEnd;          /* +0x030 ms, max over the streams */
    int32_t      timeLength;       /* +0x034 end - start (at least 1) */
    int32_t      time;             /* +0x038 current position, ms */
    PcAviOptions options[8];       /* +0x03C */
    PcAviOptions *optionPtr[8];    /* +0x19C freed with AVISaveOptionsFree */
    void        *getFrame[8];      /* +0x1BC PGETFRAME (video streams) */
    void        *drawDib[8];       /* +0x1DC HDRAWDIB (video streams) */
    int          playing;          /* +0x1FC */
    uint32_t     startTick;        /* +0x200 GetTickCount() - time at play */
    HWND         hwnd;             /* +0x204 */
    int32_t      rect[4];          /* +0x208 destination RECT */
    int          loop;             /* +0x218 */
    int          fullscreen;       /* +0x21C flip DirectDraw to the GDI surface before drawing */
    void        *waveFormat;       /* +0x220 WAVEFORMATEX (zmemAlloc) */
    int32_t      sampleSize;       /* +0x224 AVISTREAMINFO.dwSampleSize of the audio stream */
    uint16_t     samplesPerSec;    /* +0x228 */
    uint16_t     avgBytesPerSec;   /* +0x22A */
    void        *waveOut;          /* +0x22C HWAVEOUT */
    uint16_t     numBuffers;       /* +0x230 prepared WAVEHDRs (2..16) */
    uint16_t     queued;           /* +0x232 buffers written and not yet returned */
    uint16_t     nextBuffer;       /* +0x234 */
    uint16_t     pad236;           /* +0x236 */
    void        *buffer[16];        /* +0x238 WAVEHDR + 0x8000 bytes each */
    int32_t      audioStart;       /* +0x278 first sample (loop point) */
    int32_t      audioPos;         /* +0x27C next sample to read */
    int32_t      audioEnd;         /* +0x280 */
    int          audioOpen;        /* +0x284 */
    int          audioEnabled;     /* +0x288 set to 1 by pcmovieInit, never cleared */
} PcMovie;                         /* 0x28C */

/* GHIDRA-SKIP-BEGIN */
typedef char zpc_h_size_check[(sizeof(PcGuid) == 0x10 && sizeof(PcJoyMapHeader) == 0x08 && sizeof(PcInputDevice) == 0x14 &&
                               sizeof(PcAviOptions) == 0x2C && sizeof(PcMovie) == 0x28C) ? 1 : -1];

/* DirectInput 7 interfaces, only the slots pcinput.c calls (dinput.h order); the devices are
 * created through CreateDeviceEx as IDirectInputDevice2A (same slots). */
typedef struct DIDevice7 DIDevice7;
typedef struct DIDevice7Vtbl {
    void *QueryInterface, *AddRef;
    uint32_t (__stdcall *Release)(DIDevice7 *d);                                    /* +0x08 */
    void *GetCapabilities, *EnumObjects, *GetProperty;
    int32_t  (__stdcall *SetProperty)(DIDevice7 *d, uint32_t prop, const void *hdr); /* +0x18 */
    int32_t  (__stdcall *Acquire)(DIDevice7 *d);                                    /* +0x1C */
    int32_t  (__stdcall *Unacquire)(DIDevice7 *d);                                  /* +0x20 */
    int32_t  (__stdcall *GetDeviceState)(DIDevice7 *d, uint32_t size, void *state); /* +0x24 */
    void *GetDeviceData;
    int32_t  (__stdcall *SetDataFormat)(DIDevice7 *d, const void *format);          /* +0x2C */
    void *SetEventNotification;
    int32_t  (__stdcall *SetCooperativeLevel)(DIDevice7 *d, HWND hwnd, uint32_t flags); /* +0x34 */
    void *GetObjectInfo;
    int32_t  (__stdcall *GetDeviceInfo)(DIDevice7 *d, void *instance);              /* +0x3C */
    void *RunControlPanel, *Initialize;                                             /* +0x40 */
    void *CreateEffect, *EnumEffects, *GetEffectInfo, *GetForceFeedbackState;       /* +0x48 IDirectInputDevice2 */
    void *SendForceFeedbackCommand, *EnumCreatedEffectObjects, *Escape;             /* +0x58 */
    int32_t  (__stdcall *Poll)(DIDevice7 *d);                                       /* +0x64 */
} DIDevice7Vtbl;
#define DIERR_INPUTLOST ((int32_t)0x8007001E)
struct DIDevice7 { const DIDevice7Vtbl *vtbl; };

typedef struct DInput7 DInput7;
typedef int (__stdcall *DIEnumDevicesFn)(const void *instance, void *ref);
typedef struct DInput7Vtbl {
    void *QueryInterface, *AddRef;
    uint32_t (__stdcall *Release)(DInput7 *di);                                       /* +0x08 */
    void *CreateDevice;
    int32_t  (__stdcall *EnumDevices)(DInput7 *di, uint32_t type, DIEnumDevicesFn fn, void *ref, uint32_t flags); /* +0x10 */
    void *GetDeviceStatus, *RunControlPanel, *Initialize, *FindDevice;
    int32_t  (__stdcall *CreateDeviceEx)(DInput7 *di, const void *guid, const void *iid, DIDevice7 **out, void *outer); /* +0x24 */
} DInput7Vtbl;
struct DInput7 { const DInput7Vtbl *vtbl; };


int        pcinputInit(void);                                               /* 0x00493160 */
int        pcinputEnumJoystick(const void *instance, void *ref);            /* 0x00493220 DIEnumDevicesCallback */
int        pcinputShutdown(void);                                           /* 0x00493240 */
PcInputDevice *pcinputOpen(int type, struct ZInputDevice *owner);           /* 0x00493310 */
DIDevice7 *pcinputCreateDevice(HWND hwnd, PcGuid guid, const void *format, uint32_t coop); /* 0x00493760 */
int        pcinputReleaseDevice(struct ZInputDevice *dev);                  /* 0x004937F0 zinputClose */
int        pcinputAcquireAll(void);                                         /* 0x00493830 MainWndProc: activated */
int        pcinputUnacquireAll(void);                                       /* 0x00493890 MainWndProc: deactivated */
int        pcinputPollDevices(void);                                        /* 0x00493910 once per frame */
int        pcinputStub(struct ZInputDevice *device);                        /* 0x00493CB0 returns 0 (zinput: no force feedback) */
int        zinputSetKeyLayout(struct ZInputDevice *device, const void *layout); /* 0x00493CC0 */
extern const uint8_t g_pcinputKeymap1[];                                    /* 0x00561E68 (DM-051) */
extern const uint8_t g_pcinputKeymap2[];                                    /* 0x00561E88 (DM-051) */
extern const uint8_t g_pcinputJoyMapDefault[];                              /* 0x00561EA8 (DM-051): a PcJoyMapHeader + pairs */
extern const PcJoyMapHeader *const g_pcinputJoyMaps[];                      /* 0x00561F2C, NULL-terminated */

void       pcmovieInit(PcMovie *m);                                         /* 0x0049FB30 */
void       pcmovieShutdown(PcMovie *m);                                     /* 0x0049FBE0 */
int        pcmovieOpenAudio(PcMovie *m, DWORD_PTR callback);                /* 0x0049FCF0 */
void       pcmovieCloseAudio(PcMovie *m);                                   /* 0x0049FF90 */
int        pcmovieFillAudio(PcMovie *m);                                    /* 0x004A0040 */
int        pcmovieStartAudio(PcMovie *m, DWORD_PTR callback);               /* 0x004A0270 */
void       pcmovieStopAudio(PcMovie *m);                                    /* 0x004A0320 */
void       pcmovieScanStreams(PcMovie *m);                                  /* 0x004A0400 */
int        pcmovieOpenFile(PcMovie *m, void *aviFile, int unused);          /* 0x004A0630 */
int        pcmovieOpenStreams(PcMovie *m, int count, void *const *streams, int unused); /* 0x004A0690 */
int        pcmovieOpen(PcMovie *m, const char *path, int unused);           /* 0x004A06F0 */
void       pcmovieClose(PcMovie *m);                                        /* 0x004A0780 */
int32_t    pcmovieGetLength(PcMovie *m);                                    /* 0x004A09A0 */
void       pcmovieSetTime(PcMovie *m, int32_t ms);                          /* 0x004A09B0 */
int32_t    pcmovieGetTime(PcMovie *m);                                      /* 0x004A09C0 */
void       pcmoviePlay(PcMovie *m, HWND hwnd, int fullscreen, int loop, const int32_t *rect); /* 0x004A09D0 */
void       pcmovieAudioDone(PcMovie *m);                                    /* 0x004A0A70 waveOut WOM_DONE */
void       pcmovieUpdate(PcMovie *m, HDC dc, const int32_t *rect);          /* 0x004A0A90 */
void       pcmovieStop(PcMovie *m);                                         /* 0x004A0CB0 */
void       pcmovieDraw(PcMovie *m, HDC dc, int x0, int y0, int x1, int y1, int32_t ms); /* 0x004A0DB0 */
void      *pcmovieGetFrame(PcMovie *m, int32_t ms);                         /* 0x004A0F40 */
int        pcmovieIsPlaying(PcMovie *m);                                    /* 0x004A1050 */
/* GHIDRA-SKIP-END */

#endif /* ZPC_H */

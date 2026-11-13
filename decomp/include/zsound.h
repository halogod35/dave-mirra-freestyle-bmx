/*
 * zsound.h - the sound system (DM-055): sound banks (winLoad.c), the software mixer / channel
 * manager on DirectSound (winChan.c), streamed audio (winStrm.c), the ZTRK sequencer (zsndseq.c),
 * 3-D sound sources (zsndsrc.c) and the game layer (bmxsound.c).
 *
 * The API prefix of the original is "zsnd" (assert texts: "fzsndcInitialized",
 * "hChan < kzsndNumChannelHandles", "fzsndStreamInitialized", "hTrack < kzsndSeqMaxTracks",
 * "noteEvent->flags & kzsndNoteEvent...").  Bank format: docs/formats/AUDIO.md; the event map and
 * the mixer: docs/re/sound.md.  Offsets [V] (the code's reads and writes); names [I].
 */
#ifndef ZSOUND_H
#define ZSOUND_H
#include "dmtypes.h"
#include "rwtypes.h"

/* ------------------------------------------------------------------ bank file (ZSND / WIN) */

/* table A: voice (0x14).  What the mixer plays: a sample, a pitch, a level and an envelope. */
typedef struct ZsndVoice {
    uint16_t sample;          /* +0x00 index into the bank's samples (table B), 0xFFFF = none */
    uint16_t pitch;           /* +0x02 4.12 fixed playback-rate factor (0x1000 = the sample's rate) */
    uint8_t  volume;          /* +0x04 0..127 */
    int8_t   pan;             /* +0x05 -127 (left) .. 0 .. 127 (right) */
    uint8_t  attack;          /* +0x06 envelope time index (g_zsndcEnvTimeMs), 0 = start at full level */
    uint8_t  decay;           /* +0x07 time index of the fall from 1.0 to sustainLevel, 0 = none */
    uint8_t  sustainTime;     /* +0x08 time index of the fall from sustainLevel to 0; 0x7F = hold */
    uint8_t  release;         /* +0x09 time index of the fade after a key-off, 0 = cut at once */
    uint8_t  sustainLevel;    /* +0x0A 0..127 */
    uint8_t  unused_0b[6];    /* +0x0B 0 in every bank; not read on the PC */
    uint8_t  psxFlag;         /* +0x11 0/1 (29 voices); not read on the PC [I: a PSX SPU voice flag] */
    uint16_t bank;            /* +0x12 runtime: owning asset handle (0 in files) */
} ZsndVoice;                  /* 0x14 */

/* ZsndSample.flags */
#define ZSND_SAMPLE_LOOP     0x0001u  /* loop back to loopStart at the end of the sample file */
#define ZSND_SAMPLE_STEREO   0x0002u  /* interleaved stereo frames (unused: every bank is mono) */
#define ZSND_SAMPLE_8BIT     0x0004u  /* unsigned 8-bit (unused: every bank is 16-bit) */
#define ZSND_SAMPLE_PINGPONG 0x0008u  /* with LOOP: play back and forth (unused) */

/* table B: sample (0x18) */
typedef struct ZsndSample {
    uint16_t file;            /* +0x00 index into the sample files (table C), 0xFFFF = none */
    uint16_t flags;           /* +0x02 ZSND_SAMPLE_* */
    uint32_t rate;            /* +0x04 Hz */
    uint8_t *data;            /* +0x08 runtime: the PCM data (resident banks) */
    uint32_t loopStart;       /* +0x0C frames */
    uint32_t loopEnd;         /* +0x10 frames; never read on the PC (the loop always runs to the
                                 end of the sample file), so 0 = "not set" and harmless */
    uint16_t pad_14;          /* +0x14 0 */
    uint16_t bank;            /* +0x16 runtime: owning asset handle */
} ZsndSample;                 /* 0x18 */

/* table C: sample file (8) */
typedef struct ZsndSampleFile {
    uint32_t offset;          /* +0x00 from the start of the bank file */
    uint32_t size;            /* +0x04 bytes */
} ZsndSampleFile;             /* 0x08 */

/* table E: track definition (0x10) */
typedef struct ZsndTrackDef {
    uint8_t  volume;          /* +0x00 track volume (0x7F in every bank) */
    int8_t   pan;             /* +0x01 */
    uint8_t  voice;           /* +0x02 default voice, 0xFF = none (set by a ZTRK control event) */
    uint8_t  keymap;          /* +0x03 default keymap (table F), 0xFF = none */
    uint32_t pad_04;          /* +0x04 0 */
    uint8_t *ztrk;            /* +0x08 offset -> pointer of the "ZTRK" record (code starts at +4) */
    uint16_t pad_0c;          /* +0x0C 0 */
    uint16_t bank;            /* +0x0E runtime: owning asset handle */
} ZsndTrackDef;               /* 0x10 */

/* table F record: keymap note -> (voice, transpose) (no bank uses one) */
typedef struct ZsndKeymapKey {
    uint8_t  voice;           /* +0x00 0xFF = silent */
    int8_t   semitones;       /* +0x01 transpose */
} ZsndKeymapKey;              /* 0x02 */
typedef struct ZsndKeymap {
    uint8_t  firstNote;       /* +0x00 */
    uint8_t  count;           /* +0x01 */
    uint16_t bank;            /* +0x02 runtime: owning asset handle */
    ZsndKeymapKey key[1];     /* +0x04 [count] */
} ZsndKeymap;

/* the 0x48-byte bank header; RegisterAsset turns the table offsets into pointers */
typedef struct ZsndBankHeader {
    char            magic[4];       /* +0x00 "ZSND" */
    char            platform[4];    /* +0x04 "WIN " */
    uint32_t        fileSize;       /* +0x08 */
    uint32_t        headerSize;     /* +0x0C end of tables + ZTRK code (what a streamed bank loads) */
    uint32_t        numVoices;      /* +0x10 A */
    ZsndVoice      *voices;         /* +0x14 */
    uint32_t        numSamples;     /* +0x18 B */
    ZsndSample     *samples;        /* +0x1C */
    uint32_t        numFiles;       /* +0x20 C */
    ZsndSampleFile *files;          /* +0x24 */
    uint32_t        numPhrases;     /* +0x28 D ("phrase number out of range"; empty everywhere) */
    uint8_t       **phrases;        /* +0x2C */
    uint32_t        numTrackDefs;   /* +0x30 E */
    ZsndTrackDef   *trackDefs;      /* +0x34 */
    uint32_t        unused_38;      /* +0x38 0 */
    uint32_t        unused_3c;      /* +0x3C end of table E, not relocated */
    uint32_t        numKeymaps;     /* +0x40 F ("keymap number out of range"; empty everywhere) */
    ZsndKeymap    **keymaps;        /* +0x44 */
} ZsndBankHeader;                   /* 0x48 */

/* ------------------------------------------------------------------ winLoad.c */

#define ZSND_MAX_ASSETS      16
#define ZSND_ASSET_ALL       0xFFFEu        /* zsndUnloadAsset: every loaded asset */
#define ZSND_ASSET_LOADED    0x01u
#define ZSND_ASSET_RESIDENT  0x02u          /* whole file in memory (sfx); else header only (streams) */
#define ZSND_ASSET_OWNED     0x04u          /* zsndLoadAsset allocated the data: free it on unload */

typedef struct ZsndAsset {
    uint32_t        flags;          /* +0x00 ZSND_ASSET_* */
    ZsndBankHeader *bank;           /* +0x04 */
    char            path[64];       /* +0x08 file name, kept for streamed (non-resident) banks */
} ZsndAsset;                        /* 0x48; g_zsndAssets[16] 0x00600D98 */

/* what zsndGetSampleFile returns: where a stream reads its data from */
typedef struct ZsndFileDesc {
    const char *path;               /* +0x00 the bank's file name */
    uint32_t    offset;             /* +0x04 */
    uint32_t    size;               /* +0x08 */
} ZsndFileDesc;                     /* 0x0C; one static instance 0x00600D88 */

/* ------------------------------------------------------------------ winChan.c */

#define ZSNDC_NUM_HANDLES    64     /* kzsndNumChannelHandles */
#define ZSNDC_NUM_CHANNELS   24     /* mixer voices */
#define ZSNDC_CHORUS_LEN     1024   /* chorus delay line, stereo float frames */

/* zsndcInit parameters (0x14; bmxsoundOpen zeroes them) */
typedef struct ZsndcInitParams {
    void    *hwnd;                  /* +0x00 for SetCooperativeLevel */
    uint32_t flags;                 /* +0x04 bit 0: 8-bit output, bit 1: mono output */
    uint32_t rate;                  /* +0x08 output rate, 0 = 22050 */
    uint32_t bufferMs;              /* +0x0C mix block length, 0 = 50 (the buffer holds two blocks) */
    float    echoSeconds;           /* +0x10 echo line length; bmxsoundOpen leaves 0 (no echo room) */
} ZsndcInitParams;                  /* 0x14 */

/* zsndcSetEffect parameters (0x14) */
#define ZSNDC_EFFECT_ECHO    0
#define ZSNDC_EFFECT_CHORUS  1
typedef struct ZsndcEffect {
    int32_t  type;                  /* +0x00 ZSNDC_EFFECT_*; anything else -> error 0x10D */
    int8_t   level;                 /* +0x04 wet level, -127..127 (/127) */
    int8_t   psxDepth;              /* +0x05 bmxsound writes a PSX reverb depth here (type 2) */
    uint8_t  pad_06[2];
    int32_t  delayMs;               /* +0x08 echo delay (<= echoSeconds) / chorus delay (<= 1024 frames) */
    float    feedback;              /* +0x0C echo: feedback; chorus: depth (x free delay range) */
    float    rate;                  /* +0x10 chorus: phase step x 4096 per frame (sine of 4096) */
} ZsndcEffect;                      /* 0x14 */

/* ZsndcChannel.state */
#define ZSNDC_FREE           0
#define ZSNDC_STOPPED        1
#define ZSNDC_PLAYING        2
/* ZsndcChannel.flags */
#define ZSNDC_AUTOFREE       0x01u  /* free the handle when the sound ends (one-shots) */
#define ZSNDC_UNITY_PITCH    0x02u  /* step == 1.0 */
/* envelope stages */
#define ZSNDC_ENV_ATTACK     0
#define ZSNDC_ENV_DECAY      1
#define ZSNDC_ENV_SUSTAIN    2
#define ZSNDC_ENV_RELEASE    3

typedef struct ZsndcChannel {
    uint8_t     state;              /* +0x00 ZSNDC_FREE / STOPPED / PLAYING */
    uint8_t     flags;              /* +0x01 ZSNDC_AUTOFREE | ZSNDC_UNITY_PITCH */
    uint8_t     priority;           /* +0x02 a new sound may steal the lowest priority <= its own */
    uint8_t     handle;             /* +0x03 owning handle */
    uint32_t    pad_04;
    ZsndVoice  *voice;              /* +0x08 */
    ZsndSample *sample;             /* +0x0C */
    int32_t     numFrames;          /* +0x10 length of the sample file in frames */
    int32_t     step;               /* +0x14 20.12 frames per output frame; < 0 = ping-pong back */
    int32_t     pos;                /* +0x18 frame */
    uint32_t    frac;               /* +0x1C 12-bit fraction */
    uint8_t     volume;             /* +0x20 voice volume x channel volume / 128 */
    int8_t      pan;                /* +0x21 */
    int8_t      envStage;           /* +0x22 ZSNDC_ENV_* */
    uint8_t     pad_23;
    float       gainL;              /* +0x24 from g_zsndcGain by volume and pan */
    float       gainR;              /* +0x28 */
    int32_t     envCount;           /* +0x2C output frames left in the stage, 0 = hold */
    float       envLevel;           /* +0x30 0..1 */
    float       envRate;            /* +0x34 added per output frame */
    float       attackRate;         /* +0x38 */
    float       decayRate;          /* +0x3C */
    float       sustainLevel;       /* +0x40 */
    float       sustainRate;        /* +0x44 */
} ZsndcChannel;                     /* 0x48; g_zsndcChannels[24] 0x005FB770 */

typedef struct ZsndcHandle {
    uint8_t  allocated;             /* +0x00 */
    uint8_t  channel;               /* +0x01 mixer channel, 0xFF = none (stolen or finished) */
} ZsndcHandle;                      /* 0x02; g_zsndcHandles[64] 0x005FBE38 */

/* ------------------------------------------------------------------ winStrm.c */

#define ZSND_MAX_STREAMS     8
#define ZSND_MAX_STREAM_FILES 32
#define ZSND_MAX_STREAM_QUEUE 32
/* zsndStreamOpenFile / zsndStreamCreate flags */
#define ZSND_STRM_AUTOFREE   0x0002u        /* destroy the stream when it ends (else just stop) */
#define ZSND_STRM_11K        0x0008u
#define ZSND_STRM_16K        0x0010u
#define ZSND_STRM_44K        0x0040u        /* none of 8/10/40: 22050 Hz */
#define ZSND_STRM_STEREO     0x0100u        /* forced off for IMA-ADPCM */
#define ZSND_STRM_8BIT       0x0400u
#define ZSND_STRM_PCM        0x0800u        /* raw PCM; without it the data is IMA-ADPCM */
/* ZsndStreamQueue.loops: zsndStreamQueueFile flag */
#define ZSND_STRM_QUEUE_FRONT 0x0001u

typedef struct ZsndStreamFile {
    void    *hFile;                 /* +0x00 Win32 HANDLE */
    uint32_t size;                  /* +0x04 bytes of (compressed) data */
    uint16_t rate;                  /* +0x08 Hz */
    uint8_t  bits;                  /* +0x0A 16 or 8 (of the decoded data) */
    uint8_t  channels;              /* +0x0B */
    uint32_t start;                 /* +0x0C file offset of the data */
    uint32_t pos;                   /* +0x10 bytes read */
    uint32_t flags;                 /* +0x14 bit 0: IMA-ADPCM */
    int32_t  imaPred;               /* +0x18 decoder state: predictor (0 at the start) */
    int32_t  imaStep;               /* +0x1C step (7 at the start) */
    int32_t  imaIndex;              /* +0x20 step index (0) */
} ZsndStreamFile;                   /* 0x24; g_zsndStreamFiles[32] 0x00601350 */

typedef struct ZsndStreamQueue {
    uint32_t                loops;  /* +0x00 plays left (0 = until stopped) */
    ZsndStreamFile         *file;   /* +0x04 */
    struct ZsndStreamQueue *next;   /* +0x08 */
} ZsndStreamQueue;                  /* 0x0C; g_zsndStreamQueue[32] 0x006019F8 */

/* ZsndStream.flags */
#define ZSND_STREAM_ALLOC    0x8000u
#define ZSND_STREAM_PRIMED   0x4000u        /* the buffer holds data from the start of the file */
#define ZSND_STREAM_ENDING   0x2000u        /* the data ran out: stop after `remaining` bytes */
/* ZsndStream.state */
#define ZSND_STREAM_STOPPED  1
#define ZSND_STREAM_PLAYING  2
#define ZSND_STREAM_PAUSED   3

typedef struct ZsndStream {
    uint16_t         flags;         /* +0x00 ZSND_STREAM_* | ZSND_STRM_AUTOFREE */
    uint8_t          state;         /* +0x02 */
    uint8_t          pauseCount;    /* +0x03 */
    uint32_t         bufBytes;      /* +0x04 DirectSound buffer size (default 64 KB) */
    uint32_t         refillBytes;   /* +0x08 refill granularity (zsndStreamSetRefill) */
    uint32_t         writePos;      /* +0x0C next byte to fill */
    uint32_t         lastPlayPos;   /* +0x10 */
    uint32_t         endPos;        /* +0x14 where the data ends in the buffer */
    uint32_t         played;        /* +0x18 bytes played since the last refill */
    ZsndStreamQueue  queue;         /* +0x1C the playing file (head of the queue) */
    uint32_t         fadeFlags;     /* +0x28 bit 0: fading, bit 1: stop at the end of the fade */
    int32_t          fadeVol;       /* +0x2C 16.16 */
    int32_t          fadeTarget;    /* +0x30 16.16 */
    int32_t          fadeStep;      /* +0x34 per frame */
    void            *buffer;        /* +0x38 IDirectSoundBuffer* */
    uint8_t          volume;        /* +0x3C 0..127 */
    int8_t           pan;           /* +0x3D */
    uint8_t          pad_3e[2];
    uint32_t         remaining;     /* +0x40 bytes left to play once ENDING */
} ZsndStream;                       /* 0x44; g_zsndStreams[8] 0x006017D0 */

/* ------------------------------------------------------------------ zsndseq.c */

#define ZSND_SEQ_MAX_TRACKS  32     /* kzsndSeqMaxTracks */
#define ZSND_SEQ_MAX_NOTES   64

/* ZsndSeqTrack.flags */
#define ZSND_TRK_ALLOC       0x8000u
#define ZSND_TRK_PENDING     0x4000u        /* a stop / pause / mute waits for the next zsndSeqSetMode */
#define ZSND_TRK_MUTED       0x2000u
#define ZSND_TRK_GROUPMUTE   0x1000u        /* muted while g_zsndSeqGroupMute is set */
#define ZSND_TRK_LOOP        0x0800u        /* the end marker restarts the track */
#define ZSND_TRK_AUTOFREE    0x0004u        /* free the track when it stops (zsndSeqPlayTrackDef) */
#define ZSND_TRK_MODE_MASK   0x0003u        /* 0: act at once, 1: ignore, 2/3: defer (PENDING) */
/* ZsndSeqTrack.state */
#define ZSND_TRK_STOPPED     0
#define ZSND_TRK_PLAYING     1
#define ZSND_TRK_PAUSED      2

struct ZsndSeqNote;
struct ZsndSeqEvent;

typedef struct ZsndSeqTrack {
    uint16_t            flags;      /* +0x00 ZSND_TRK_* */
    uint8_t             state;      /* +0x02 ZSND_TRK_STOPPED / PLAYING / PAUSED */
    uint8_t             velocity;   /* +0x03 running velocity (note events without one use it) */
    uint8_t             volume;     /* +0x04 track volume (track def, zsndSeqSetVolume) */
    uint8_t             expression; /* +0x05 control 0 */
    int8_t              pan;        /* +0x06 control 1 / zsndSeqSetVolume */
    uint8_t             pad_07;
    ZsndTrackDef       *def;        /* +0x08 */
    uint32_t            reg[4];     /* +0x0C sources of the 0xF8 indirect-parameter form */
    ZsndVoice          *voice;      /* +0x1C control 4; notes play this voice when set */
    ZsndKeymap         *keymap;     /* +0x20 control 5; else notes go through this keymap */
    uint8_t            *start;      /* +0x24 first event */
    uint8_t            *pc;         /* +0x28 next event */
    uint32_t            wait;       /* +0x2C 20.12 ms to the next event */
    uint32_t            msPerTick;  /* +0x30 20.12: 1000 / g_zsndUpdateRate x tempo */
    uint32_t            ticksPerMs; /* +0x34 20.12: g_zsndUpdateRate / 1000 x tempo */
    struct ZsndSeqNote *notes;      /* +0x38 sounding notes, delta-sorted by end time */
    void      (__cdecl *callback)(void *user, int marker, int value); /* +0x3C */
    void               *user;       /* +0x40 */
    uint16_t            bank;       /* +0x44 */
    uint16_t            pad_46;
} ZsndSeqTrack;                     /* 0x48; g_zsndSeqTrackPool[32] 0x0064F048 */

/* ZsndSeqNote.flags */
#define ZSND_NOTE_USED       0x01u          /* kzsndNoteEventAllocated */
#define ZSND_NOTE_SILENT     0x02u          /* no channel (muted track / paused) */

typedef struct ZsndSeqNote {
    uint8_t             flags;      /* +0x00 ZSND_NOTE_* */
    uint8_t             velocity;   /* +0x01 */
    int16_t             cents;      /* +0x02 pitch offset (note - 60) x 100 */
    ZsndVoice          *voice;      /* +0x04 */
    uint8_t             channel;    /* +0x08 zsndc handle, 0xFF = none */
    uint8_t             pad_09[3];
    uint32_t            ticks;      /* +0x0C ticks after the previous note in the list */
    struct ZsndSeqNote *next;       /* +0x10 */
} ZsndSeqNote;                      /* 0x14; g_zsndSeqNotes[64] 0x0064EB38 */

/* one decoded ZTRK event (zsndSeqDecodeEvent) */
typedef struct ZsndSeqEvent {
    ZsndSeqTrack *track;            /* +0x00 */
    uint32_t      length;           /* +0x04 the length field (note duration in ms) */
    uint8_t       p0;               /* +0x08 first parameter (a u32 for the jump event) */
    uint8_t       p1;               /* +0x09 */
    uint8_t       pad_0a[2];
    int     (__cdecl *handler)(struct ZsndSeqEvent *ev); /* +0x0C */
} ZsndSeqEvent;                     /* 0x10 */

/* ZTRK event byte: bits 0-2 = length form (ZSND_LEN_*), bits 3-7 = opcode */
#define ZSND_OP_NOTE         0x00   /* note, velocity: 2 bytes */
#define ZSND_OP_NOTE_RUN     0x08   /* note (running velocity): 1 byte */
#define ZSND_OP_CONTROL      0x10   /* control, value: 2 bytes */
#define ZSND_OP_MARKER       0x18   /* marker, value: 2 bytes (marker 1 = end of track) */
#define ZSND_OP_JUMP         0x30   /* u32 offset from the first event: 4 bytes */
#define ZSND_OP_INDIRECT     0xF8   /* opcode byte, source byte, 4 parameter bytes (unused) */
/* controls */
#define ZSND_CTL_EXPRESSION  0
#define ZSND_CTL_PAN         1
#define ZSND_CTL_VOICE       4
#define ZSND_CTL_KEYMAP      5

/* ------------------------------------------------------------------ zsndsrc.c */

#define ZSND_MAX_SOURCES     24

typedef struct ZsndSrc {
    uint16_t   flags;               /* +0x00 0x8000 = allocated */
    uint8_t    channel;             /* +0x02 zsndc handle, 0xFF = none */
    uint8_t    pad_03;
    ZsndVoice *voice;               /* +0x04 */
    uint32_t   life;                /* +0x08 updates left; zsndSrcSetPosition re-arms it */
    RwV3d      pos;                 /* +0x0C world position */
    uint32_t   pad_18;
    uint32_t   distSq;              /* +0x1C to the listener, last update */
    int16_t    doppler;             /* +0x20 cents, from the change of distSq, +-700 */
    int16_t    pitch;               /* +0x22 cents (zsndSrcSetPitch) */
    uint8_t    level;               /* +0x24 distance attenuation 0..127 */
    uint8_t    volume;              /* +0x25 zsndSrcSetVolume */
    int8_t     pan;                 /* +0x26 */
    uint8_t    pad_27;
} ZsndSrc;                          /* 0x28; g_zsndSrcs[24] 0x0064FCD0 */

/* ------------------------------------------------------------------ bmxsound.c */

/* bmxsoundEvent codes (docs/re/sound.md has the full map) */
#define BMXSND_SET_MODE        1000 /* a: 0 off, 1 menu, 2 game, 3 game paused */
#define BMXSND_SET_MUSIC       1001 /* a: music mode 0-2 on, 3 stop, 4 fade out */
#define BMXSND_SET_VOLUMES     1002 /* a: 0 sfx, 1 music; b: volume */
#define BMXSND_SET_REVERB      1003 /* a, b: PSX reverb mode / depth (no effect on the PC) */
#define BMXSND_GET_TRACK       1004
#define BMXSND_SET_TRACK       1005 /* a: music track */
#define BMXSND_MENU_FIRST      2000 /* 2000-2009: SFXCOM menu sounds */
#define BMXSND_GAME_FIRST      3000 /* 3000-3006: rider / bike sounds */
#define BMXSND_3D_FIRST        4000 /* 4000-4003: positional (rolling, camera, props) */
#define BMXSND_JUDGE_FIRST     5000 /* 5000-5011: scoring / HUD */
#define BMXSND_PAUSE           6000

/* GHIDRA-SKIP-BEGIN */
typedef char zsound_h_size_check[(sizeof(ZsndVoice) == 0x14 && sizeof(ZsndSample) == 0x18 &&
                                  sizeof(ZsndSampleFile) == 0x08 && sizeof(ZsndTrackDef) == 0x10 &&
                                  sizeof(ZsndKeymapKey) == 0x02 && sizeof(ZsndBankHeader) == 0x48 &&
                                  sizeof(ZsndAsset) == 0x48 && sizeof(ZsndFileDesc) == 0x0C &&
                                  sizeof(ZsndcInitParams) == 0x14 && sizeof(ZsndcEffect) == 0x14 &&
                                  sizeof(ZsndcChannel) == 0x48 && sizeof(ZsndcHandle) == 0x02 &&
                                  sizeof(ZsndStreamFile) == 0x24 && sizeof(ZsndStreamQueue) == 0x0C &&
                                  sizeof(ZsndStream) == 0x44 && sizeof(ZsndSeqTrack) == 0x48 &&
                                  sizeof(ZsndSeqNote) == 0x14 && sizeof(ZsndSeqEvent) == 0x10 &&
                                  sizeof(ZsndSrc) == 0x28) ? 1 : -1];

void __cdecl _assert(const char *expr, const char *file, unsigned line);   /* MSVC CRT 0x004A4DE8 */
extern uint32_t g_zsndUpdateRate;                                           /* 0x0055FFC0 */
/* bmxsound.c */
void bmxsoundSetUpdateRate(uint32_t hz);                                                             /* 0x0044DD40 */
int bmxsoundProbe(void *hwnd);                                                                       /* 0x0044DD60 */
int bmxsoundStub(int unused);                                                                        /* 0x0044DD80 */
int bmxsoundOpen(void);                                                                              /* 0x0044DDA0 */
void bmxsoundClose(void);                                                                            /* 0x0044E090 */
void bmxsoundUpdate(void);                                                                           /* 0x0044E120 */
void bmxsoundService(void);                                                                          /* 0x0044E2E0 */
int bmxsoundEvent(uint32_t event, uint32_t a, uint32_t b, uint32_t c, uint32_t d);                   /* 0x0044E300 */
int bmxsoundPlace(const RwV3d *pos, uint8_t volume, uint8_t *outVolume, int8_t *outPan);             /* 0x0044FFB0 */
void __stdcall bmxsoundTimerProc(uint32_t id, uint32_t msg, uint32_t user, uint32_t dw1, uint32_t dw2); /* 0x00450020 */
/* winChan.c */
int zsndcInit(ZsndcInitParams *init);                                                                /* 0x0046D940 */
void zsndcShutdown(void);                                                                            /* 0x0046DF20 */
int zsndcUpdate(void);                                                                               /* 0x0046E010 */
void zsndcFreeAll(void);                                                                             /* 0x0046E3C0 */
uint8_t zsndcAlloc(ZsndVoice *voice, uint8_t priority);                                              /* 0x0046E410 */
int zsndcFree(uint8_t hChan);                                                                        /* 0x0046E890 */
int zsndcPlayVoice(ZsndVoice *voice, int16_t cents, uint8_t volume, int8_t pan, uint8_t priority);   /* 0x0046E8C0 */
int zsndcKeyOn(uint8_t hChan);                                                                       /* 0x0046E9A0 */
int zsndcKeyOff(uint8_t hChan, uint8_t how);                                                         /* 0x0046EB60 */
int zsndcGetState(uint8_t hChan);                                                                    /* 0x0046ECD0 */
int zsndcSetVolume(uint8_t hChan, uint8_t volume);                                                   /* 0x0046ED20 */
int zsndcSetPan(uint8_t hChan, int8_t pan);                                                          /* 0x0046EE80 */
int zsndcSetPitch(uint8_t hChan, int16_t cents);                                                     /* 0x0046EFB0 */
ZsndVoice * zsndcGetVoice(uint8_t hChan);                                                            /* 0x0046F170 */
void zsndcStopVoice(ZsndVoice *voice);                                                               /* 0x0046F1C0 */
int zsndcSetEffect(const ZsndcEffect *fx);                                                           /* 0x0046F250 */
int zsndcProbe(void *hwnd);                                                                          /* 0x0046F380 */
ZsndcChannel * zsndcGetChannel(uint8_t hChan);                                                       /* 0x0046F470 */
void zsndcRelease(ZsndcChannel *ch);                                                                 /* 0x0046F530 */
float zsndcEnvRate(int level, int ms);                                                               /* 0x0046F580 */
void zsndcMix(int16_t *dst, uint32_t bytes);                                                         /* 0x0046F5F0 */
/* winLoad.c */
int zsndLoadAsset(const char *path, int resident, uint16_t *handle);                                 /* 0x0046FB20 */
int zsndRegisterAsset(ZsndBankHeader *bank, int resident, uint16_t *handle);                         /* 0x0046FD00 */
void zsndUnloadAsset(uint16_t handle);                                                               /* 0x004700B0 */
int zsndIsAssetLoaded(uint16_t handle);                                                              /* 0x00470100 */
uint32_t zsndGetNumVoices(uint16_t handle);                                                          /* 0x00470140 */
uint32_t zsndGetNumSamples(uint16_t handle);                                                         /* 0x004701D0 */
uint32_t zsndGetNumSampleFiles(uint16_t handle);                                                     /* 0x00470260 */
uint32_t zsndGetNumPhrases(uint16_t handle);                                                         /* 0x004702F0 */
uint32_t zsndGetNumTrackDefs(uint16_t handle);                                                       /* 0x00470380 */
uint32_t zsndGetNumKeymaps(uint16_t handle);                                                         /* 0x00470410 */
ZsndVoice * zsndGetVoice(uint16_t handle, uint32_t index);                                           /* 0x004704A0 */
ZsndSample * zsndGetSample(uint16_t handle, uint32_t index);                                         /* 0x00470560 */
ZsndFileDesc * zsndGetSampleFile(uint16_t handle, uint32_t index);                                   /* 0x00470620 */
uint8_t * zsndGetPhrase(uint16_t handle, uint32_t index);                                            /* 0x00470700 */
ZsndTrackDef * zsndGetTrackDef(uint16_t handle, uint32_t index);                                     /* 0x004707C0 */
ZsndKeymap * zsndGetKeymap(uint16_t handle, uint32_t index);                                         /* 0x00470880 */
void zsndFreeAsset(uint16_t handle);                                                                 /* 0x00470940 */
/* winStrm.c */
int zsndStreamInit(void);                                                                            /* 0x00470AA0 */
int zsndStreamShutdown(void);                                                                        /* 0x00470B20 */
int zsndStreamUpdate(void);                                                                          /* 0x00470BA0 */
int zsndStreamService(void);                                                                         /* 0x00470D10 */
int zsndStreamCreate(ZsndStream **out, const ZsndFileDesc *file, int unused, uint32_t bufBytes, uint32_t flags); /* 0x00470FB0 */
int zsndStreamDestroy(ZsndStream *s);                                                                /* 0x004712F0 */
int zsndStreamSetRefill(ZsndStream *s, uint32_t percent);                                            /* 0x00471400 */
int zsndStreamSetFile(ZsndStream *s, const ZsndFileDesc *file, uint32_t flags);                      /* 0x00471450 */
int zsndStreamSetLoops(ZsndStream *s, uint32_t loops);                                               /* 0x00471610 */
int zsndStreamQueueFile(ZsndStream *s, const ZsndFileDesc *file, uint32_t flags);                    /* 0x004716B0 */
int zsndStreamGetState(ZsndStream *s, uint32_t *state);                                              /* 0x00471840 */
int zsndStreamPlay(ZsndStream *s);                                                                   /* 0x00471890 */
int zsndStreamStop(ZsndStream *s);                                                                   /* 0x00471A20 */
int zsndStreamPause(ZsndStream *s, int pause);                                                       /* 0x00471AA0 */
int zsndStreamSetVolume(ZsndStream *s, uint8_t volume, int8_t pan);                                  /* 0x00471B60 */
int zsndStreamFade(ZsndStream *s, uint32_t ms, uint8_t target, uint32_t flags);                      /* 0x00471C40 */
int zsndStreamHold(int hold);                                                                        /* 0x00471CE0 */
int zsndStreamGetCaps(uint32_t *caps);                                                               /* 0x00471D10 */
void zsndStreamPlayOnce(const ZsndFileDesc *file, uint32_t bufBytes, uint8_t volume, int8_t pan);    /* 0x00471D30 */
uint32_t zsndStreamFill(ZsndStream *s, uint32_t bytes, uint32_t *endPos);                            /* 0x00471DC0 */
ZsndStreamFile * zsndStreamOpenFile(const ZsndFileDesc *file, uint32_t flags);                       /* 0x00472030 */
void zsndStreamCloseFile(ZsndStreamFile *f);                                                         /* 0x00472200 */
uint32_t zsndStreamRead(ZsndStreamFile *f, void *dst, uint32_t bytes);                               /* 0x00472250 */
int zsndStreamRewind(ZsndStreamFile *f);                                                             /* 0x004723B0 */
ZsndStreamQueue * zsndStreamAllocQueue(void);                                                        /* 0x00472400 */
void zsndStreamFreeQueue(ZsndStreamQueue *q);                                                        /* 0x00472480 */
void zsndImaDecode(int16_t *dst, const uint8_t *src, uint32_t n, int32_t *pred, int32_t *step, int32_t *index); /* 0x004724C0 */
/* zsndseq.c */
int zsndSeqInit(void);                                                                               /* 0x0047C4C0 */
int zsndSeqShutdown(void);                                                                           /* 0x0047C530 */
int zsndSeqUpdate(void);                                                                             /* 0x0047C580 */
uint8_t zsndSeqCreateTrack(ZsndTrackDef *def, uint16_t flags);                                       /* 0x0047C6B0 */
int zsndSeqFreeTrack(uint8_t hTrack);                                                                /* 0x0047C870 */
int zsndSeqPlay(uint8_t hTrack);                                                                     /* 0x0047C940 */
int zsndSeqStop(uint8_t hTrack);                                                                     /* 0x0047CA50 */
int zsndSeqPause(uint8_t hTrack);                                                                    /* 0x0047CB30 */
int zsndSeqMute(uint8_t hTrack, int mute);                                                           /* 0x0047CC20 */
int zsndSeqGroupMute(uint8_t hTrack, int mute);                                                      /* 0x0047CE30 */
int zsndSeqGetState(uint8_t hTrack);                                                                 /* 0x0047CF10 */
int zsndSeqSetMode(uint8_t hTrack, uint8_t mode);                                                    /* 0x0047CFE0 */
int zsndSeqSetTempo(uint8_t hTrack, float tempo);                                                    /* 0x0047D0D0 */
int zsndSeqSetVolume(uint8_t hTrack, uint8_t volume, int8_t pan);                                    /* 0x0047D1B0 */
int zsndSeqSetLoop(uint8_t hTrack, int loop);                                                        /* 0x0047D290 */
uint8_t zsndSeqPlayTrackDef(ZsndTrackDef *def, uint8_t volume, int8_t pan);                          /* 0x0047D320 */
int zsndSeqSetCallback(uint8_t hTrack, void *callback, void *user);                                  /* 0x0047D3A0 */
int zsndSeqIsRunning(const ZsndSeqTrack *t);                                                         /* 0x0047D420 */
uint32_t zsndSeqPeekDelta(const ZsndSeqTrack *t);                                                    /* 0x0047D450 */
int zsndSeqDecodeEvent(ZsndSeqTrack *t, ZsndSeqEvent *ev);                                           /* 0x0047D490 */
int zsndSeqEvNote(ZsndSeqEvent *ev);                                                                 /* 0x0047D7B0 */
int zsndSeqEvControl(ZsndSeqEvent *ev);                                                              /* 0x0047DA10 */
int zsndSeqEvMarker(ZsndSeqEvent *ev);                                                               /* 0x0047DB90 */
int zsndSeqEvJump(ZsndSeqEvent *ev);                                                                 /* 0x0047DC10 */
void zsndSeqInsertNote(ZsndSeqTrack *t, ZsndSeqNote *n);                                             /* 0x0047DC50 */
void zsndSeqNoteOff(ZsndSeqNote *n);                                                                 /* 0x0047DCD0 */
int zsndSeqKeymapLookup(const ZsndKeymap *km, uint8_t note, ZsndVoice **voice, int16_t *cents);      /* 0x0047DD40 */
uint32_t zsndSeqReadU32(const uint8_t *p);                                                           /* 0x0047DDE0 */
/* zsndsrc.c */
void zsndMusicInit(uint32_t bufBytes);                                                               /* 0x0047DE20 */
int zsndMusicShutdown(void);                                                                         /* 0x0047DE50 */
int zsndMusicUpdate(void);                                                                           /* 0x0047DE90 */
int zsndMusicPlay(const ZsndFileDesc *file, uint8_t flags, uint32_t loops);                          /* 0x0047DEB0 */
int zsndMusicGetState(uint32_t *state);                                                              /* 0x0047DF40 */
int zsndMusicResume(void);                                                                           /* 0x0047DF70 */
int zsndMusicStop(void);                                                                             /* 0x0047DFA0 */
int zsndMusicPause(void);                                                                            /* 0x0047DFD0 */
int zsndMusicSetVolume(uint8_t volume, int8_t pan);                                                  /* 0x0047E000 */
int zsndMusicFade(uint32_t ms, uint8_t target, uint32_t flags);                                      /* 0x0047E030 */
void zsndMusicHold(int hold);                                                                        /* 0x0047E070 */
void zsndMusicGetCaps(uint32_t *caps);                                                               /* 0x0047E090 */
int zsndSrcInit(uint32_t maxDist, uint32_t lifeMs);                                                  /* 0x0047E0B0 */
void zsndSrcShutdown(void);                                                                          /* 0x0047E180 */
int zsndSrcUpdate(void);                                                                             /* 0x0047E200 */
void zsndSrcResetDoppler(void);                                                                      /* 0x0047E4B0 */
uint16_t zsndSrcCreate(ZsndVoice *voice, const RwV3d *pos);                                          /* 0x0047E520 */
void zsndSrcFree(uint16_t hSrc);                                                                     /* 0x0047E650 */
int zsndSrcSetVoice(uint16_t hSrc, ZsndVoice *voice);                                                /* 0x0047E720 */
int zsndSrcSetPosition(uint16_t hSrc, const RwV3d *pos);                                             /* 0x0047E7E0 */
int zsndSrcSetPitch(uint16_t hSrc, int16_t cents);                                                   /* 0x0047E880 */
int zsndSrcSetVolume(uint16_t hSrc, uint8_t volume);                                                 /* 0x0047E910 */
uint8_t zsndSrcGetChannel(uint16_t hSrc);                                                            /* 0x0047E990 */
int zsndSrcGetLevelPan(const RwV3d *pos, uint8_t *level, int8_t *pan);                               /* 0x0047EA10 */
void zsndSetUpdateRate(uint32_t hz);                                                                 /* 0x0047EBA0 */
int zsndStub(void);                                                                                  /* 0x0047EBC0 */
/* GHIDRA-SKIP-END */

#endif /* ZSOUND_H */

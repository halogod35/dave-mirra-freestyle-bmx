/*
 * replay.h - run recording and playback (Bmx/Src/com/action/replay.c).
 *
 * A replay records one player's pad (buttons and both sticks) as a run-length event stream, plus
 * optional snapshots of the rider, the props and the goal progress so that playback starts from
 * the same state. Up to 5 replays exist at once (g_replays[5]); game.c records every run, action.c
 * plays them back ("view replay") and runs the attract demo from replay.zal.
 *
 * One allocation (zmemTempAlloc) holds a 0x40-byte header (BmxReplay), the event stream and the
 * snapshot blocks; the header keeps their offsets. A saved replay (replay.zal) is the buffer from
 * +0x0C to its end (bmxreplayGetSaveArea), so its first 0x34 bytes are header fields +0x0C..+0x3F.
 * Format and stream encoding: docs/formats/REPLAY.md; reader/writer tools/dmbmx/formats/replay.py.
 * Offsets [V] (replay.c, action.c, game.c); names [I] unless noted.
 */
#ifndef REPLAY_H
#define REPLAY_H
#include "dmtypes.h"
#include "save.h"

#define REPLAY_MAX          5        /* g_replays[5]; handles 0..4, 0xFF = none */
#define REPLAY_HEADER       0x40     /* BmxReplay; the event stream starts here (dataOffset) */
#define REPLAY_SAVE_START   0x0C     /* the saved / loaded part starts at header +0x0C */
#define REPLAY_DEFAULT_DATA 0x2000   /* stream bytes when BmxReplayDesc.size is 0 */
#define REPLAY_RIDER_SIZE   0x2DE8   /* BmxReplayRider */
#define REPLAY_PROPS_SIZE   0xB40    /* BmxPropState (bmxprop.h) */
#define REPLAY_GOALS_SIZE   0x27     /* SaveGoalInfo (save.h) */

/* BmxReplay.mode / bmxreplaySetMode [V] */
enum {
    REPLAY_OFF    = 0,
    REPLAY_RECORD = 1,
    REPLAY_PLAY   = 2,
    REPLAY_MODE3  = 3,   /* never set by the exe */
    REPLAY_END    = 4    /* playback reached BmxReplay.frames (bmxreplayUpdate) */
};

/* BmxReplay.flags = BmxReplayDesc.flags: blocks NOT kept [V] (bmxreplayCreate sizes the buffer) */
#define REPLAYF_NO_RIDER 0x01u   /* no rider snapshots (2 x 0x2DE8) */
#define REPLAYF_NO_PROPS 0x02u   /* no prop snapshots (2 x 0xB40) */
#define REPLAYF_NO_GOALS 0x04u   /* no goal-info snapshots (2 x 0x27); skipped anyway outside proquest 0..5 */

/* Event stream (REPLAY.md): every event is 3 bytes, [kind << 5 | delta] [lo] [hi]. delta (1..31) =
 * frames since the previous event; kind 0 = buttons XOR mask (u16), 1 / 2 = stick 0 / 1 as two s8
 * (lo = y, hi = x, value x 127). Recording writes a kind-0 event with mask 0 when delta reaches 31
 * and one more when it stops. */
#define REPLAY_EV_BUTTONS 0
#define REPLAY_EV_STICK0  1
#define REPLAY_EV_STICK1  2
#define REPLAY_DELTA_MASK 0x1Fu
#define REPLAY_DELTA_MAX  0x1F

/* argument of bmxreplayCreate [V] (game.c: flags 1 per player / 7 shared; action.c demo: 7) */
typedef struct BmxReplayDesc {
    uint8_t  flags;      /* +0x00 REPLAYF_* */
    uint8_t  player;     /* +0x01 rider slot recorded */
    uint8_t  pad[2];
    uint32_t size;       /* +0x04 event-stream bytes (rounded up to 4); 0 = 0x2000 */
} BmxReplayDesc;         /* 0x08 */

/* Header of a replay buffer (g_replays[n]) [V]. Offsets are from the start of the buffer. */
typedef struct BmxReplay {
    int32_t  tempMark;     /* +0x00 zmemTempMark() before the allocation (released by bmxreplayDestroy) */
    uint8_t  mode;         /* +0x04 REPLAY_* */
    uint8_t  delta;        /* +0x05 record: frames since the last event; play: frames to the next */
    uint8_t  player;       /* +0x06 rider slot */
    uint8_t  flags;        /* +0x07 REPLAYF_* */
    uint32_t frame;        /* +0x08 current frame */
    uint32_t controlConfig;/* +0x0C rider controlConfig (BmxRider +4) when recording started */
    uint32_t frames;       /* +0x10 length in frames (30 per second) */
    uint32_t riderStart;   /* +0x14 BmxReplayRider at the start of the recording (0 = none) */
    uint32_t riderLive;    /* +0x18 the live rider, saved while playing back */
    uint32_t propsStart;   /* +0x1C BmxPropState at the start */
    uint32_t propsLive;    /* +0x20 */
    uint32_t goalsStart;   /* +0x24 SaveGoalInfo at the start */
    uint32_t goalsLive;    /* +0x28 */
    uint32_t dataOffset;   /* +0x2C event stream (0x40) */
    uint32_t dataSize;     /* +0x30 */
    uint8_t *cursor;       /* +0x34 next event (stale pointer in a saved replay) */
    uint32_t buttons;      /* +0x38 buttons after the last event */
    int8_t   stick[2][2];  /* +0x3C last stick values as stored: [n][0] = y, [n][1] = x (x 127) */
} BmxReplay;               /* 0x40 */

/* A rider snapshot (REPLAY_RIDER_SIZE) [V] (bmxreplaySetMode copies). */
typedef struct BmxReplayRider {
    uint8_t  rider[0xF70];       /* +0x0000 BmxRider */
    uint8_t  riderActor[0x278];  /* +0x0F70 *rider->riderActor */
    uint8_t  actorData[0xD60];   /* +0x11E8 *(rider->riderActor + 0x48) */
    uint8_t  dynamics[0xE88];    /* +0x1F48 *rider->animCtl (rag doll) */
    float    bikePos[3];         /* +0x2DD0 bike actor position (zactorGetPos) */
    uint32_t pad_2ddc;
    float    bikeHeight;         /* +0x2DE0 zactorGetHeight (restored with zactorSetHeight) */
    uint32_t buttons;            /* +0x2DE4 pad buttons at the snapshot */
} BmxReplayRider;                /* 0x2DE8 */

/* GHIDRA-SKIP-BEGIN */
typedef char replay_h_size_check[(sizeof(BmxReplayDesc) == 0x08 && sizeof(BmxReplay) == 0x40 &&
                                  sizeof(BmxReplayRider) == 0x2DE8) ? 1 : -1];

void      bmxreplayInit(void);                                       /* 0x0042FCE0 */
void      bmxreplayShutdown(void);                                   /* 0x0042FD10 */
void      bmxreplayUpdate(void);                                     /* 0x0042FD50 */
uint8_t   bmxreplayCreate(const BmxReplayDesc *desc);                /* 0x00430180 0xFF = none free */
void      bmxreplayDestroy(uint8_t handle);                          /* 0x00430380 */
uint8_t   bmxreplaySetMode(uint8_t handle, uint32_t mode);           /* 0x00430410 */
uint8_t   bmxreplayGetMode(uint8_t handle);                          /* 0x00430B70 */
uint8_t   bmxreplaySetPlayer(uint8_t handle, uint8_t player);        /* 0x00430BF0 */
uint8_t   bmxreplayGetPlayer(uint8_t handle);                        /* 0x00430C70 */
uint32_t  bmxreplayGetFrame(uint8_t handle);                         /* 0x00430CF0 */
void      bmxreplayGetSaveArea(uint8_t handle, void **start, uint32_t *size);  /* 0x00430D70 */
void      bmxreplayEmpty1(void);                                     /* 0x00430E30 */
void      bmxreplayEmpty2(void);                                     /* 0x00430E50 */

extern BmxReplay *g_replays[REPLAY_MAX];                             /* 0x0056D278 */
/* GHIDRA-SKIP-END */

#endif /* REPLAY_H */

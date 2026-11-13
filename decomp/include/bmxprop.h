/*
 * bmxprop.h - game-side props (Bmx/Src/com/action/prop.c) (DM-052).
 *
 * The engine loads the placed props of Prop._sd (zprop.h, Zprop) and NgIFzbsp gives every prop a
 * 500-byte collision record (NgPropCol below, allocated lazily by zbspPropColCreate). prop.c adds
 * game behaviour on top, driven by a hard-coded table of 16 slots per level (g_bmxpropTable):
 *
 *   - moving props (types 0..3): doors, gates, lifts, carts, falling logs, ... found by the prop's
 *     group id (Prop OBJI byte 7) and animated here (BmxProp, 16 per level);
 *   - switchable model pairs (type 5): the props whose OBJI u16 at +8 is `id` are shown in state 0,
 *     the ones with `id + 1` in state 1 (fence whole / fence broken);
 *   - breakable props (BmxPropSwitch, up to 64): every prop with a non-zero OBJI u16 id; touching a
 *     class-9 trigger face with code `id` breaks prop `id` (hides it, shows `id + 1`, shards, sound).
 *
 * The 16 BmxProp slots and the 64 switches are one 0xB40-byte block (BmxPropState, g_bmxprops)
 * that the replay saves and restores (replay.h). Offsets [V] (prop.c, replay.c, game.c,
 * physics.c); names and meanings [I] unless noted. Units: ZGE world units (cm), y DOWN, 16-bit
 * angles (0x10000 = 360 degrees), velocities per frame. Description: docs/re/props-replay-fx.md.
 */
#ifndef BMXPROP_H
#define BMXPROP_H
#include "dmtypes.h"
#include "rwtypes.h"
#include "zlist.h"
#include "zprop.h"

#define BMXPROP_SLOTS     16    /* g_bmxpropTable entries per level = BmxPropState.props */
#define BMXPROP_SWITCHES  64    /* BmxPropState.switches (assert "state info overflow") */
#define BMXPROP_LEVELS    14    /* g_bmxpropTable rows: the 14 scene names (12 levels + Test A/B) */

/* BmxPropDef.type [V]: how the slot is found and driven. */
enum {
    BMXPROP_MOVER = 0,   /* moves when triggered (BmxPropDef.motion); found by OBJI byte 7 */
    BMXPROP_GOAL1 = 1,   /* goal props, switched by game.c (bmxpropActivate / bmxpropActivateNow) and */
    BMXPROP_GOAL2 = 2,   /*   touches (gameOnPropTouched); 2 swings back after link[0] seconds, */
    BMXPROP_GOAL3 = 3,   /*   3 has a one-way collision box while moved (NgIFzbsp mode 2) */
    BMXPROP_SOUND = 4,   /* only a looping sound at the prop whose OBJI u16 id == BmxPropDef.id */
    BMXPROP_PAIR  = 5    /* switchable model pair (OBJI u16 id / id + 1) */
};

/* BmxPropDef.motion [V] (bmxpropTrigger's switch). For BMXPROP_MOVER unless noted; the goal types
 * use only 1..3. Velocities are per frame; an action lasts BmxProp.timer frames. */
enum {
    BMXMOVE_NONE     = 0,
    BMXMOVE_RISE     = 1,  /* goal: +-110 vertical (vel.y -+1.8333 for 60 frames); mover: flips at once */
    BMXMOVE_LIFT     = 2,  /* mover: +-400 vertical over 240 frames; goal: turns +-90 deg about x in 60 */
    BMXMOVE_SWING_X  = 3,  /* turns +90 deg about x in 30 frames (3, 4) or -90 deg (5); goal: turns */
    BMXMOVE_SWING_X2 = 4,  /*   30 deg about y (0x5B / frame for 60 frames) */
    BMXMOVE_SWING_XN = 5,
    BMXMOVE_SWING_Z  = 6,  /* turns -90 deg about z in 240 frames (6, 7) */
    BMXMOVE_SWING_Z2 = 7,
    BMXMOVE_FALL     = 8,  /* traffic: runs along -z at 40 / frame for ever (z < -8200 wraps by +12000); */
    BMXMOVE_FALL2    = 9,  /*   a rider inside its box crashes (thrown with 1.05 x its velocity, damage) */
    BMXMOVE_BOB      = 10, /* +-200 vertical over 120 frames */
    BMXMOVE_KNOCK    = 11, /* knocked away with 1.25 x the rider's sideways velocity (damped 0.94 /
                              frame), spins about y; the rider passes through (bmxpropTouched == 0) */
    BMXMOVE_TUMBLE   = 12, /* as 11 for 15 frames, also tumbling */
    BMXMOVE_SLIDE_L  = 13, /* slides along its right axis +-1200 over 150 frames (13) / +-400 (14) */
    BMXMOVE_SLIDE_S  = 14,
    BMXMOVE_TRAVEL_L = 15, /* travels along its right axis +-3200 over 360 frames */
    BMXMOVE_TRAVEL_3 = 16, /* -1100 over 90 frames, turns 90 frames, +2400 over 270 (bmxpropStep) */
    BMXMOVE_DELAY1   = 17, /* 17..20: nothing moves for 60 frames */
    BMXMOVE_DELAY2   = 18,
    BMXMOVE_DELAY3   = 19,
    BMXMOVE_DELAY4   = 20
};

/* One slot of the per-level prop table (g_bmxpropTable[14][16] 0x00557D28) [V]. */
typedef struct BmxPropDef {
    uint16_t id;         /* +0x00 0 = unused; types 0..3: OBJI byte 7; types 4/5: OBJI u16 at +8 */
    uint8_t  type;       /* +0x02 BMXPROP_* */
    uint8_t  motion;     /* +0x03 BMXMOVE_* */
    uint16_t link[2];    /* +0x04 motions 13..20: ids (BmxPropDef.id) switched when the move ends,
                            on (link[0]) or off; BMXPROP_GOAL2: link[0] = seconds until it swings back */
    int8_t   sfxTrigger; /* +0x08 sound when triggered (bmxsound 0xFA3 mode 3), -1 none */
    int8_t   sfxLoop;    /* +0x09 looping sound at the prop (0xFA3: mode 0 start, 2 position, 1 stop), -1 none */
} BmxPropDef;            /* 0x0A */

/* NgIFzbsp's per-prop collision record (Zprop.info.user, 500 bytes, zbspPropColCreate). Only the
 * fields prop.c / game.c / physics.c use; the rest belongs to NgIFzbsp.c / NgIFzcollide.c. */
typedef struct NgPropCol {
    uint16_t defIndex;   /* +0x000 index of the prop def in zpropGetDefList (zbspGetPropDefIndex) */
    uint16_t flags;      /* +0x002 NGPROPCOL_*; bits 0-4 = group id (OBJI byte 7 & 0x1F) */
    int16_t  rot[3];     /* +0x004 euler angles of the scene object */
    int16_t  pos[3];     /* +0x00A position, whole units */
    float    scale[3];   /* +0x010 1.0 */
    uint32_t unk_1c[2];  /* +0x01C */
    void    *rails;      /* +0x024 NgColEdge[4 or 1]: grind rails from the def's shape (zcolCreatePropRails) */
    uint8_t  unk_28[0x1C4]; /* +0x028 zeroed; no PC code reads or writes it (PSX leftovers [I]) */
    void    *game;       /* +0x1EC BmxProp * (bmxpropInit), read by physics.c */
    Zprop   *prop;       /* +0x1F0 the engine prop */
} NgPropCol;             /* 0x1F4 */

/* NgPropCol.flags (DM-057: every reader in the exe, [V] by a scan of the game code for tests of +2 / +3).
 * NGPROPCOL_OFF = "knocked": zbspPropSetMode 0, set by physics.c when a rider crashes hard into the
 * prop (wall impact, knock_prop) and never cleared; zbspFindEdgesNear and the debug draw skip the
 * rails of props with MOVING, OFF, FLAG100 or HIDDEN, and zbspPropSetRotation stops moving the
 * rails of a prop that is both MOVING and OFF. DRIVEN, OBJI4 and OBJI5 are set but never read. */
#define NGPROPCOL_GROUP    0x001Fu  /* OBJI byte 7 & 0x1F (goal "prop group"; gameOnPropTouched) */
#define NGPROPCOL_MOVING   0x0040u  /* zbspPropSetMode 2 (model flag 8 set) / 3 clears */
#define NGPROPCOL_OFF      0x0080u  /* zbspPropSetMode 0 (knocked) */
#define NGPROPCOL_FLAG100  0x0100u  /* zbspPropSetMode 1 (no caller) */
#define NGPROPCOL_HIDDEN   0x0200u  /* zbspPropSetMode 4 (scene object hidden) / 5 clears */
#define NGPROPCOL_DRIVEN   0x0400u  /* OBJI byte 6 bit 0; bmxpropInit sets it on table-slot props */
#define NGPROPCOL_OBJI4    0x0800u  /* OBJI byte 4 bit 0 */
#define NGPROPCOL_OBJI5    0x1000u  /* OBJI byte 5 bit 0 */

/* zbspPropSetMode (NgIFzbsp.c 0x00466610) modes */
enum { NGPROPCOL_SET_OFF = 0, NGPROPCOL_SET_1 = 1, NGPROPCOL_SET_MOVING = 2, NGPROPCOL_SET_STATIC = 3,
       NGPROPCOL_SET_HIDE = 4, NGPROPCOL_SET_SHOW = 5 };

/* A prop slot (BmxPropState.props[16], 0x84 bytes) [V offsets]. */
typedef struct BmxProp {
    uint8_t    state;    /* +0x00 BMXPROPST_* */
    uint8_t    flags;    /* +0x01 BMXPROPF_* */
    uint8_t    index;    /* +0x02 slot = index into g_bmxpropTable[level] */
    uint8_t    player;   /* +0x03 who switched it last */
    int32_t    timer;    /* +0x04 frames left of the current action (-1: for ever) */
    NgPropCol *col;      /* +0x08 collision record of the (first) prop of the slot */
    float      pos[3];   /* +0x0C position */
    uint32_t   pad_18;
    float      vel[3];   /* +0x1C velocity per frame */
    uint32_t   pad_28;
    RwMatrix   matrix;   /* +0x2C orientation: right +0x2C, up +0x3C, at +0x4C */
    int16_t    spin[3];  /* +0x6C rotation added per frame (x, y, z) */
    uint16_t   pad_72;
    int32_t    sound;    /* +0x74 looping-sound handle, -1 none */
    uint32_t   toggled;  /* +0x78 which way it went last (0/1; BMXMOVE_TRAVEL_3: stage 0..3) */
    uint32_t   link[2];  /* +0x7C BmxPropDef.link */
} BmxProp;               /* 0x84 */

/* BmxProp.state [V] */
enum {
    BMXPROPST_NONE    = 0,  /* unused slot */
    BMXPROPST_IDLE    = 1,
    BMXPROPST_WAIT    = 2,  /* timer, then triggered and 3 */
    BMXPROPST_MOVING  = 3,  /* timer (or -1), then idle */
    BMXPROPST_WAIT2   = 4,  /* as 2 / 3, the second half of a two-way move (BMXPROP_GOAL2) */
    BMXPROPST_MOVING2 = 5
};

#define BMXPROPF_MOVED   0x01u  /* moved by a trigger; on stopping toggled != 0 hides it */
#define BMXPROPF_HIDDEN  0x02u  /* hidden + collision off (zbspPropSetMode modes 4, 2) */

/* A breakable prop (BmxPropState.switches[64], 12 bytes) [V]. */
typedef struct BmxPropSwitch {
    uint32_t unk_00;     /* +0x00 cleared, never read */
    uint32_t flags;      /* +0x04 bit 0: broken (the `id` prop hidden, `id + 1` shown) */
    Zprop   *prop;       /* +0x08 the prop with OBJI u16 id != 0; NULL ends the list */
} BmxPropSwitch;         /* 0x0C */

/* All prop state (g_bmxprops 0x00694460; initial copy g_bmxpropsStart 0x0056C4F8) [V]. */
typedef struct BmxPropState {
    BmxProp       props[16];      /* +0x000 BMXPROP_SLOTS */
    BmxPropSwitch switches[64];   /* +0x840 BMXPROP_SWITCHES */
} BmxPropState;          /* 0xB40 */

/* GHIDRA-SKIP-BEGIN */
struct NgColFace;
typedef char bmxprop_h_size_check[(sizeof(BmxPropDef) == 0x0A && sizeof(NgPropCol) == 0x1F4 &&
                                   sizeof(BmxProp) == 0x84 && sizeof(BmxPropSwitch) == 0x0C &&
                                   sizeof(BmxPropState) == 0xB40) ? 1 : -1];

Zprop     *bmxpropFindProp(uint32_t id);                         /* 0x0042DE70 by OBJI u16 id */
void       bmxpropInit(void);                                    /* 0x0042DEB0 */
void       bmxpropShutdown(void);                                /* 0x0042E270 */
void       bmxpropUpdate(void);                                  /* 0x0042E2F0 */
int        bmxpropDraw(void);                                    /* 0x0042EB50 */
void       bmxpropSetState(const BmxPropState *state);           /* 0x0042EB70 */
void       bmxpropReset(void);                                   /* 0x0042ECF0 */
BmxProp   *bmxpropGetPropByID(uint32_t id);                      /* 0x0042ED10 */
void       bmxpropActivate(uint32_t player, BmxProp *prop);      /* 0x0042ED80 animated */
void       bmxpropActivateNow(uint32_t player, BmxProp *prop);   /* 0x0042EE40 at once */
int        bmxpropTouched(uint32_t player, BmxProp *prop);       /* 0x0042EEC0 0 = pass through */
void       bmxpropHitTrigger(uint32_t player, const struct NgColFace *face); /* 0x0042EF60 */
void       bmxpropTrigger(BmxProp *prop, uint32_t player, int immediate);   /* 0x0042F0D0 */
void       bmxpropSpin(BmxProp *prop);                           /* 0x0042F8F0 */
void       bmxpropRotate(BmxProp *prop);                         /* 0x0042F930 */
void       bmxpropIdle(BmxProp *prop);                           /* 0x0042FAB0 */
void       bmxpropStep(BmxProp *prop);                           /* 0x0042FAD0 */
void       bmxpropShowPair(BmxProp *prop, uint32_t on);          /* 0x0042FC00 */

extern const BmxPropDef g_bmxpropTable[BMXPROP_LEVELS][BMXPROP_SLOTS];   /* 0x00557D28 */
extern BmxPropState     g_bmxprops;                                      /* 0x00694460 */
extern BmxPropState     g_bmxpropsStart;                                 /* 0x0056C4F8 */
/* GHIDRA-SKIP-END */

#endif /* BMXPROP_H */

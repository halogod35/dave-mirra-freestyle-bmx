/*
 * contrler.h - controllers: the game's pads (Bmx/Src/com/action/contrler.c) on top of the engine
 * input devices (NgEngine/Zge/zinput.c, Pc/pcinput.c).
 *
 * Offsets are [V] (read/written at those offsets by contrler.c, zinput.c, pcinput.c, rider.c and
 * the frontend's controller screen 0x00441CB0/0x004420A0); names and meanings [I] unless noted.
 * Full description: docs/re/input-camera.md.
 *
 * Data flow, once per frame (actionUpdate -> contrlerUpdate):
 *   DirectInput keyboard / joystick --pcinput keymaps--> ZInputDevice.channel[22] (0.0 / 1.0, axes)
 *   --> ZInputDevice.buttons (PAD_* bits, fixed channel->bit table in the poll 0x00493910)
 *   --> contrlerUpdate: the 7 remappable PAD_* bits go through SaveProfile.control[] of the
 *       player's profile, the others pass through --> BmxPad.input (held + changed + sticks)
 *   --> riderReadInput copies BmxPad.input into BmxRider.input; rider.c asks
 *       contrlerTestAction(config, &input, ACT_*, ACT_PRESSED/HELD/RELEASED), which looks the pair
 *       up in g_contrlerActions[config][action][status] = (button mask, CONTRLER_TEST_*).
 */
#ifndef CONTRLER_H
#define CONTRLER_H
#include "dmtypes.h"
#include "rider.h"

/* Pad button bits (ZInputDevice.buttons, BmxInput.buttons). [V] channel -> bit table of the input
 * poll 0x00493910 and the frontend's button names 0x0055A3F4 ("Up" .. "Fire 6"). The PSX names
 * are [I]: the default keyboard layout puts E/S/F/D in a diamond (Triangle/Square/Circle/Cross),
 * and the controller-screen labels (STRS 0x14C-0x157) match the PSX game's pad. */
enum {
    PAD_UP       = 0x00001,   /* "Up"        d-pad / stick y < -0.5     keyboard: Up arrow */
    PAD_DOWN     = 0x00002,   /* "Down"                                 Down arrow */
    PAD_LEFT     = 0x00004,   /* "Left"                                 Left arrow */
    PAD_RIGHT    = 0x00008,   /* "Right"                                Right arrow */
    PAD_FIRE1    = 0x00010,   /* "Fire 1"    Triangle: GRIND/STALL      E */
    PAD_FIRE2    = 0x00020,   /* "Fire 2"    Square: TRICK              S */
    PAD_FIRE3    = 0x00040,   /* "Fire 3"    Circle: MODIFIER           F */
    PAD_FIRE4    = 0x00080,   /* "Fire 4"    Cross: PEDAL / JUMP        D */
    PAD_LTRIG1   = 0x00100,   /* "L Trig 1"  L1: SPIN LEFT              W */
    PAD_RTRIG1   = 0x00200,   /* "R Trig 1"  R1: SPIN RIGHT             R */
    PAD_LTRIG2   = 0x00400,   /* "L Trig 2"  L2: spin left (configs A, B) 3 */
    PAD_RTRIG2   = 0x00800,   /* "R Trig 2"  R2: spin right (configs A, B) 4 */
    PAD_LTRIG3   = 0x01000,   /* "L Trig 3"  (no channel feeds it on PC) */
    PAD_RTRIG3   = 0x02000,   /* "R Trig 3"  (no channel feeds it on PC) */
    PAD_START    = 0x04000,   /* "Start"     pause                      Space */
    PAD_SELECT   = 0x08000,   /* "Select"    overlays on/off in replays; Start+Select 60 frames = quit */
    PAD_FIRE5    = 0x10000,   /* "Fire 5"    PC: Escape (pause, like Start) */
    PAD_FIRE6    = 0x20000,   /* "Fire 6"    PC: Enter */
    PAD_DPAD     = 0x0000F,
    PAD_FACE     = 0x000F0,   /* all four face buttons (ACT_BAIL_A: pressed together) */
    PAD_PAUSE    = PAD_START | PAD_FIRE5
};

/* Tests of contrlerTestMask (the second word of a g_contrlerActions entry). [V] its switch. */
enum {
    CONTRLER_TEST_PRESSED_ANY  = 0,   /* a mask button went down this frame */
    CONTRLER_TEST_PRESSED_ALL  = 1,   /* all mask buttons held and one of them changed this frame */
    CONTRLER_TEST_HELD_ALL     = 2,   /* all mask buttons held (an empty mask is always true) */
    CONTRLER_TEST_HELD_ANY     = 3,
    CONTRLER_TEST_RELEASED_ALL = 4,   /* none held and one of them changed this frame */
    CONTRLER_TEST_RELEASED_ANY = 5,   /* a mask button went up this frame */
    CONTRLER_TEST_ALWAYS       = 6,
    CONTRLER_TEST_NEVER        = 7    /* (default: of the switch) */
};

/* Logical action status asked by the game (contrlerTestAction asserts < 3). */
enum { ACT_PRESSED = 0, ACT_HELD = 1, ACT_RELEASED = 2, ACT_STATUSES = 3 };

/* Controller actions (contrlerTestAction asserts action <= 20). [V] ids, the table
 * g_contrlerActions 0x00553EE0 and the call sites in rider.c; names [I] from the bindings and the
 * controller-screen labels (STRS 0x14C PEDAL, 0x150 TRICK, 0x151 BRAKE, 0x152 JUMP, 0x153 PEDAL/JUMP,
 * 0x154 START/JUMP, 0x155 TURN AROUND, 0x156 GRIND/STALL, 0x157 MODIFIER, 0x14E/F SPIN LEFT/RIGHT). */
enum {
    ACT_START      = 0,    /* Cross released: pedal off from standing still */
    ACT_PEDAL      = 1,    /* A: Cross pressed or Up held; B: Cross; C: Up */
    ACT_BRAKE      = 2,    /* Down (not asked by the game code) */
    ACT_TRICK      = 3,    /* Square */
    ACT_MODIFIER   = 4,    /* Circle */
    ACT_05         = 5,    /* Cross held (not asked by the game code) */
    ACT_JUMP       = 6,    /* Cross; config B: all three statuses on release (jump on release) */
    ACT_GRIND      = 7,    /* Triangle (pressed / held; never "released") */
    ACT_PLANT      = 8,    /* Cross pressed; held = always, released = never */
    ACT_TURNAROUND = 9,    /* Up pressed */
    ACT_SPIN_LEFT  = 10,   /* L1 or L2 (config C: L1) */
    ACT_SPIN_RIGHT = 11,   /* R1 or R2 (config C: R1) */
    ACT_12 = 12, ACT_13 = 13, ACT_14 = 14, ACT_15 = 15,   /* never asked; config C: empty mask */
    ACT_BAIL_A     = 16,   /* all four face buttons pressed together: bail on purpose */
    ACT_BAIL_B     = 17,   /* never true on PC (all statuses CONTRLER_TEST_NEVER) */
    ACT_GO_LIMP    = 18,   /* Circle (crash: go limp) */
    ACT_RAGDOLL    = 19,   /* Triangle (crash: keep the ragdoll) */
    ACT_20         = 20,   /* Square (never asked) */
    ACT_COUNT      = 21
};

/* Controller configurations (SaveProfile.unk_02 -> BmxRider.profileByte2), STRS 0x23D-0x23F. */
enum { CONTRLER_CONFIG_A = 0, CONTRLER_CONFIG_B = 1, CONTRLER_CONFIG_C = 2, CONTRLER_CONFIGS = 3 };

/* Remappable buttons: SaveProfile.control[k] holds the device bits that produce logical bit k.
 * [V] contrlerUpdate (profile + 0x24 + 4k) and the defaults comdataInitDefaults copies. */
#define CONTRLER_REMAP_COUNT 7
/* logical bits, in control[] order: Cross, Square, Circle, Triangle, L1, R1, Start */

typedef struct BmxActionBinding {
    uint32_t mask;            /* +0x00 PAD_* buttons */
    uint32_t test;            /* +0x04 CONTRLER_TEST_* */
} BmxActionBinding;           /* 0x08 */

/* Engine input device (zinput.c, 0x74 bytes, pool of 2; zinputOpen allocates it). */
typedef struct ZInputDevice {
    uint32_t type;            /* +0x00 0 joystick, 1 keyboard, 2 joystick + keyboard (pcinputOpen) */
    uint32_t flags;           /* +0x04 bit 0: connected */
    uint32_t buttons;         /* +0x08 PAD_* held */
    uint32_t changed;         /* +0x0C buttons ^ previous */
    uint32_t released;        /* +0x10 changed & previous */
    float    channel[22];     /* +0x14 analog channels: [0..3] the two sticks (x, y) that contrlerUpdate
                                 copies, [4..7] left/right/up/down, [8..21] buttons (poll table) */
    uint32_t unk_6c;          /* +0x6C cleared by every poll */
    void    *platform;        /* +0x70 pcinput device record (0x14 bytes) */
} ZInputDevice;               /* 0x74 */

/* A game pad (g_pads[2] 0x0056B628). */
typedef struct BmxPad {
    ZInputDevice *device;     /* +0x00 zinputOpen(2) (joystick + keyboard), else (0) */
    uint32_t ignore;          /* +0x04 device buttons masked until released (held across a pause) */
    uint32_t pausedHeld;      /* +0x08 input.buttons when the pads were paused */
    uint16_t flags;           /* +0x0C BMXPAD_* */
    uint16_t repeat;          /* +0x0E menu auto-repeat timer (actionUpdate, pause menu) */
    BmxInput input;           /* +0x10 held / changed / sticks; riderReadInput copies it */
    uint32_t unk_38;          /* +0x38 */
} BmxPad;                     /* 0x3C */

#define BMXPAD_DISCONNECTED 0x0001u   /* the device lost its connection; input cleared */
#define BMXPAD_RECONNECTED  0x0002u   /* came back this frame (contrlerUpdate returns 2) */

/* GHIDRA-SKIP-BEGIN */
typedef char contrler_h_size_check[(sizeof(BmxActionBinding) == 0x08 && sizeof(ZInputDevice) == 0x74 &&
                                    sizeof(BmxPad) == 0x3C) ? 1 : -1];

extern BmxPad           g_pads[2];                                      /* 0x0056B628 */
extern uint8_t          g_contrlerPaused;                               /* 0x0056B620 */
extern float            g_contrlerStick[2];                             /* 0x0056B6A0 */
extern const BmxActionBinding g_contrlerActions[CONTRLER_CONFIGS][ACT_COUNT][ACT_STATUSES];  /* 0x00553EE0 */

void          contrlerInit(void);
void          contrlerShutdown(void);
uint32_t      contrlerUpdate(void);                                     /* 1 = a pad lost, 2 = a pad came back */
void          contrlerSetPaused(uint8_t paused);
int           contrlerTestAction(uint32_t config, const BmxInput *in, uint32_t action, uint32_t status);
int           contrlerTestButtons(uint32_t player, uint32_t mask, uint32_t test);
float        *contrlerGetStick(const BmxInput *in, uint32_t stick);
void          contrlerRumble(uint32_t slot, int motor, int strength);   /* empty on PC */
BmxPad       *contrlerGetPad(uint32_t pad);
int           contrlerTestMask(const BmxInput *in, uint32_t mask, uint32_t test);

/* engine side: zinput.c */
int           zinputInit(void);                                     /* 0x004802F0 */
int           zinputShutdown(void);                                 /* 0x00480340 */
ZInputDevice *zinputOpen(int type);                                 /* 0x00480370 0 joystick, 1 keyboard, 2 both */
int           zinputClose(void *device);                            /* 0x004803C0 (free-list destructor) */
ZInputDevice *zinputGetDevice(int index);                           /* 0x00480400 n-th open device */
int           zinputSetRumble(ZInputDevice *device);                /* 0x00480440 */
/* GHIDRA-SKIP-END */

#endif /* CONTRLER_H */

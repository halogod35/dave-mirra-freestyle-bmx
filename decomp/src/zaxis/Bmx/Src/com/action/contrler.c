/*
 * zaxis/Bmx/Src/com/action/contrler.c
 * Original: C:\zaxis\Bmx\Src\com\action\contrler.c
 * Evidence: [V] path string 0x00554624, referenced by the asserts of contrlerInit (line 0x1B8),
 *           contrlerTestAction (0x2E2-0x2E4), contrlerTestButtons (0x2F8), contrlerGetStick (0x30B)
 *           and contrlerGetPad (0x346); the other five [I] (between them in link order, and they
 *           use the same pad array 0x0056B628).
 * 11 functions, 1702 bytes, VA 0x00408030-0x004088c5 (DM-064 created contrlerClearAll 0x00408390).
 * DM-051 (draft). Non-matching C (AD-8). Types: contrler.h. Description: docs/re/input-camera.md.
 *
 * Two pads (g_pads), one per player profile. Each frame contrlerUpdate reads the engine input
 * device of each pad, remaps the seven configurable buttons through the profile's control[] masks
 * (save profile +0x24, SAVE.md 2.5) and keeps "held" and "changed this frame" bit sets; buttons
 * that were already down when the pads were paused or resumed are ignored until released. The game
 * never looks at keys: it asks for actions (ACT_* x pressed/held/released), which the binding
 * table g_contrlerActions turns into a button mask and a test, per controller configuration A/B/C.
 */

#include <math.h>

#include "decomp.h"

#include "contrler.h"

#define CONTRLER_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\contrler.c"

/* The logical bits SaveProfile.control[k] produces, in control[] order (comdataInitDefaults gives
 * control[k] = this bit, i.e. no remapping). [V] the switch in contrlerUpdate. */
static const uint32_t contrlerRemapBits[CONTRLER_REMAP_COUNT] = {
    PAD_FIRE4, PAD_FIRE2, PAD_FIRE3, PAD_FIRE1, PAD_LTRIG1, PAD_RTRIG1, PAD_START
};

/* bit -> control[] slot, or -1 when the bit is not remappable */
static int contrlerRemapSlot(uint32_t bit)
{
    int k;

    for (k = 0; k < CONTRLER_REMAP_COUNT; k++)
        if (contrlerRemapBits[k] == bit)
            return k;
    return -1;
}

/* a pad without a device: nothing held, nothing changed, sticks centred */
static void contrlerClearInput(BmxPad *pad)
{
    int i;

    pad->input.buttons = 0;
    pad->input.changed = 0;
    pad->ignore = 0;
    for (i = 0; i < 2; i++) {
        pad->input.stick[i][0] = 0.0f;
        pad->input.stick[i][1] = 0.0f;
        pad->input.prevStick[i][0] = 0.0f;
        pad->input.prevStick[i][1] = 0.0f;
    }
}

/* PAD_LEFT / PAD_RIGHT (x) or PAD_UP / PAD_DOWN (y) for a stick value past the 0.5 threshold */
static uint32_t stickDir(float v, uint32_t neg, uint32_t pos)
{
    return v < 0.0f ? neg : pos;
}

/* VA 0x00408030, 112 bytes, [V] direct, status draft */
void contrlerInit(void)
{
    BmxPad *pad;

    for (pad = &g_pads[0]; pad < &g_pads[2]; pad++) {
        pad->device = zinputOpen(2);           /* joystick + keyboard */
        if (pad->device == NULL)
            pad->device = zinputOpen(0);       /* joystick only */
        pad->flags = 0;
        pad->ignore = 0;
        pad->repeat = 0;
        if (pad->device == NULL)
            zAssertFail("could not create controller device", CONTRLER_SRC, 0x1B8, NULL);
        if (!(pad->device->flags & 1))
            pad->flags |= BMXPAD_DISCONNECTED;
    }
    g_contrlerPaused = 1;
}

/* VA 0x004080c0, 30 bytes, [I] between, status draft */
void contrlerShutdown(void)
{
    BmxPad *pad;

    for (pad = &g_pads[0]; pad < &g_pads[2]; pad++)
        zinputClose(pad->device);
}

/* VA 0x004080f0, 519 bytes, [I] between, status draft */
uint32_t contrlerUpdate(void)
{
    /* Returns 1 when a pad was lost this frame (actionUpdate opens the pause menu) and 2 when one
     * came back. Pad i uses the control[] masks of profile i (not of profiles[i].index). */
    uint32_t events = 0;
    int i;

    for (i = 0; i < 2; i++) {
        BmxPad *pad = &g_pads[i];
        const SaveProfile *prof = &g_saveProfiles[i];
        uint16_t flags = pad->flags;
        uint32_t logical = 0;

        pad->flags = (uint16_t)(flags & ~BMXPAD_RECONNECTED);
        if (flags & BMXPAD_DISCONNECTED) {
            if (!(pad->device->flags & 1)) {
                contrlerClearInput(pad);
            } else {
                events |= 2;
                pad->repeat = 0;
                pad->flags = (uint16_t)((flags & ~(BMXPAD_DISCONNECTED | BMXPAD_RECONNECTED)) | BMXPAD_RECONNECTED);
            }
        }
        if (!(pad->flags & BMXPAD_DISCONNECTED)) {
            const ZInputDevice *dev = pad->device;

            if (!(dev->flags & 1)) {
                contrlerClearInput(pad);
                pad->flags |= BMXPAD_DISCONNECTED;
                events |= 1;
            } else {
                uint32_t bit, changed;

                for (bit = 0; bit < 32; bit++) {
                    uint32_t m = 1u << bit;
                    int k = contrlerRemapSlot(m);

                    if ((dev->buttons & (k >= 0 ? prof->control[k] : m)) != 0)
                        logical |= m;
                }
                /* ignored buttons keep their old state (no edge) until they are released */
                changed = (pad->input.buttons ^ logical) & ~pad->ignore;
                pad->input.changed = changed;
                pad->input.buttons ^= changed;
                pad->input.prevStick[0][0] = pad->input.stick[0][0];
                pad->input.prevStick[0][1] = pad->input.stick[0][1];
                pad->ignore &= logical;
                pad->input.prevStick[1][0] = pad->input.stick[1][0];
                pad->input.prevStick[1][1] = pad->input.stick[1][1];
                pad->input.stick[0][0] = dev->channel[0];
                pad->input.stick[0][1] = dev->channel[1];
                pad->input.stick[1][0] = dev->channel[2];
                pad->input.stick[1][1] = dev->channel[3];
            }
        }
    }
    return events;
}

/* VA 0x00408390, 55 bytes, [I] row, status draft */
void contrlerClearAll(void)
{
    /* (DM-064 created: no caller, thunk 0x00401889) Clears both pads' input (held, changed, sticks,
     * previous sticks) and the menu repeat timer. */
    BmxPad *pad;
    int i;

    for (pad = &g_pads[0]; pad < &g_pads[2]; pad++) {
        pad->input.buttons = 0;
        pad->input.changed = 0;
        pad->repeat = 0;
        for (i = 0; i < 2; i++) {
            pad->input.stick[i][0] = pad->input.stick[i][1] = 0.0f;
            pad->input.prevStick[i][0] = pad->input.prevStick[i][1] = 0.0f;
        }
    }
}

/* VA 0x004083e0, 154 bytes, [I] between, status draft */
void contrlerSetPaused(uint8_t paused)
{
    /* actionSetMode: 1 entering the pause menu, 0 back to riding (contrlerInit starts paused).
     * Pausing remembers what was held and ignores every button down at that moment; resuming
     * restores the held set from before the pause, so the button that closed the menu (and anything
     * pressed during the pause) does not reach the rider until it is released. */
    BmxPad *pad;

    if (paused == g_contrlerPaused)
        return;
    if (!g_contrlerPaused) {
        for (pad = &g_pads[0]; pad < &g_pads[2]; pad++) {
            pad->pausedHeld = pad->input.buttons;
            if (!(pad->flags & BMXPAD_DISCONNECTED) && (pad->device->flags & 1))
                pad->ignore = pad->device->buttons;
            else
                pad->ignore = 0;
        }
    }
    if (!paused) {
        for (pad = &g_pads[0]; pad < &g_pads[2]; pad++) {
            pad->input.buttons = pad->pausedHeld;
            if (!(pad->flags & BMXPAD_DISCONNECTED) && (pad->device->flags & 1))
                pad->ignore = pad->device->buttons & ~pad->pausedHeld;
            else
                pad->ignore = 0;
        }
    }
    g_contrlerPaused = paused;
}

/* VA 0x004084b0, 164 bytes, [V] direct, status draft */
int contrlerTestAction(uint32_t config, const BmxInput *in, uint32_t action, uint32_t status)
{
    /* rider.c: act(r, ACT_*, ACT_PRESSED/HELD/RELEASED) with config = the profile's controller
     * configuration (BmxRider.profileByte2). */
    const BmxActionBinding *b;

    if (config > 2)
        zAssertFail("controller config out of range", CONTRLER_SRC, 0x2E2, NULL);
    if (action > 20)
        zAssertFail("action out of range", CONTRLER_SRC, 0x2E3, NULL);
    if (status > 2)
        zAssertFail("actionStatus out of range", CONTRLER_SRC, 0x2E4, NULL);
    b = &g_contrlerActions[config][action][status];
    return contrlerTestMask(in, b->mask, b->test);
}

/* VA 0x00408590, 87 bytes, [V] direct, status draft */
int contrlerTestButtons(uint32_t player, uint32_t mask, uint32_t test)
{
    /* Raw button test for menus / the replay viewer (actionUpdate): PAD_* mask, CONTRLER_TEST_*. */
    if (test > 7)
        zAssertFail("status out of range", CONTRLER_SRC, 0x2F8, NULL);
    return contrlerTestMask(&g_pads[g_saveProfiles[player].index].input, mask, test);
}

/* VA 0x00408610, 140 bytes, [V] direct, status draft */
float *contrlerGetStick(const BmxInput *in, uint32_t stick)
{
    /* Stick n (x, y) in a static buffer; for stick 0 the d-pad overrides it with +-1. */
    if (stick > 1)
        zAssertFail("joystick number out of range", CONTRLER_SRC, 0x30B, NULL);
    g_contrlerStick[0] = in->stick[stick][0];
    g_contrlerStick[1] = in->stick[stick][1];
    if (stick == 0) {
        if (in->buttons & PAD_LEFT)
            g_contrlerStick[0] = -1.0f;
        else if (in->buttons & PAD_RIGHT)
            g_contrlerStick[0] = 1.0f;
        if (in->buttons & PAD_UP)
            g_contrlerStick[1] = -1.0f;
        else if (in->buttons & PAD_DOWN)
            g_contrlerStick[1] = 1.0f;
    }
    return g_contrlerStick;
}

/* VA 0x004086d0, 1 bytes, [I] between, status draft */
void contrlerRumble(uint32_t slot, int motor, int strength)
{
    /* Force feedback (physics.c: landings, crashes, grinds). The PC build has no vibration: the
     * function is a bare `ret` (the pause menu still lists "vibration", text 0x1084). */
    (void)slot;
    (void)motor;
    (void)strength;
}

/* VA 0x004086f0, 50 bytes, [V] direct, status draft */
BmxPad *contrlerGetPad(uint32_t pad)
{
    if (pad > 1)
        zAssertFail("controller number out of range", CONTRLER_SRC, 0x346, NULL);
    return &g_pads[pad];
}

/* VA 0x00408740, 390 bytes, [I] callers, status draft */
int contrlerTestMask(const BmxInput *in, uint32_t mask, uint32_t test)
{
    /* Stick 0 counts as the d-pad beyond 0.5: it adds to "held", and crossing the threshold in
     * either direction (out, or back in) counts as a change of that direction bit. */
    uint32_t held = in->buttons;
    uint32_t changed = in->changed;
    float x = in->stick[0][0], px = in->prevStick[0][0];
    float y = in->stick[0][1], py = in->prevStick[0][1];

    if (fabsf(x) > 0.5f) {
        held |= stickDir(x, PAD_LEFT, PAD_RIGHT);
        if (fabsf(px) <= 0.5f)
            changed |= stickDir(x, PAD_LEFT, PAD_RIGHT);
    } else if (fabsf(px) > 0.5f) {
        changed |= stickDir(px, PAD_LEFT, PAD_RIGHT);
    }
    if (fabsf(y) > 0.5f) {
        held |= stickDir(y, PAD_UP, PAD_DOWN);
        if (fabsf(py) <= 0.5f)
            changed |= stickDir(y, PAD_UP, PAD_DOWN);
    } else if (fabsf(py) > 0.5f) {
        changed |= stickDir(py, PAD_UP, PAD_DOWN);
    }

    switch (test) {
    case CONTRLER_TEST_PRESSED_ANY:
        return (changed & held & mask) != 0;
    case CONTRLER_TEST_PRESSED_ALL:
        return (held & mask) == mask && (mask & changed) != 0;
    case CONTRLER_TEST_HELD_ALL:
        return (held & mask) == mask;
    case CONTRLER_TEST_HELD_ANY:
        return (held & mask) != 0;
    case CONTRLER_TEST_RELEASED_ALL:
        return (~held & mask) == mask && (mask & changed) != 0;
    case CONTRLER_TEST_RELEASED_ANY:
        return (~held & changed & mask) != 0;
    case CONTRLER_TEST_ALWAYS:
        return 1;
    default:                                         /* CONTRLER_TEST_NEVER */
        return 0;
    }
}

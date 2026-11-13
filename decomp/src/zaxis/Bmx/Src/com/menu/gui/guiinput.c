/*
 * zaxis/Bmx/Src/com/menu/gui/guiinput.c
 * Original: C:\zaxis\Bmx\src\com\menu\gui\guiinput.c
 * Evidence: [V] path string 0x00559D14 referenced by guiInputInit (line 0x267) and guiInputGetPad
 *           (0x307); [I] the rest by link order (0x00436C20-0x0043744E) and because they only read
 *           g_guiPad. The assert messages are shared literals of contrler.c (string pooling).
 * 20 functions, VA 0x00436c20-0x0043744e (DM-056 created 0x004373E0, guiInputClear).
 * DM-056 (draft). Non-matching C (AD-8). Types: gui.h, contrler.h. Notes: docs/re/frontend.md.
 *
 * The menu pad: pad 0 only (guiInputGetPad asserts it), read straight from the zinput device
 * (no SaveProfile remapping, unlike contrler.c). guiUpdate turns these tests into GUI_MSG_*.
 * The d-pad tests auto-repeat: after 20 frames held, every 5th frame (frontendUpdateApp adds a
 * second, faster repeat by re-pressing the d-pad: every 3 frames, every frame with L1 / R1).
 */
#include "decomp.h"
#include "gui.h"

#define GUIINPUT_SRC "C:\\zaxis\\Bmx\\src\\com\\menu\\gui\\guiinput.c"

/* zinputOpen / zinputClose: contrler.h (DM-061) */

GuiPad   g_guiPad;               /* 0x0056D298 */
int32_t  g_guiPadPrev[2];        /* 0x0056D2B8 the pad's +0x10 / +0x14 of the previous poll */
uint32_t g_guiInputLast;         /* 0x00691F2C buttons held when the repeat started */
uint32_t g_guiInputRepeat;       /* 0x00691F3C frames the direction has been held */

/* one auto-repeating direction: 'dir' pressed alone, or held for > 20 frames (every 5th) */
static int repeatTest(int pad, uint32_t dir)
{
    GuiPad *p = guiInputGetPad(pad);
    uint32_t down;

    if (p->flags & 1)
        return 0;
    down = p->pressed & p->held;
    if (down & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT) & ~dir) {
        g_guiInputRepeat = 0;               /* another direction: restart */
        g_guiInputLast = p->held;
    } else if (down & dir) {
        g_guiInputRepeat = 0;
        g_guiInputLast = p->held;
        return 1;
    } else if (p->held & g_guiInputLast & dir) {
        g_guiInputRepeat++;
        if (g_guiInputRepeat > 20 && g_guiInputRepeat % 5 == 0)
            return 1;
    }
    return 0;
}

/* 'dir' held without another direction (every frame) */
static int heldTest(int pad, uint32_t dir)
{
    GuiPad *p = guiInputGetPad(pad);
    uint32_t held;

    if (p->flags & 1)
        return 0;
    held = p->held;
    if (held & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT) & ~dir) {
        g_guiInputLast = held;
    } else if (held & dir) {
        g_guiInputLast = held;
        return 1;
    }
    return 0;
}

static int pressTest(int pad, uint32_t mask)
{
    GuiPad *p = guiInputGetPad(pad);

    return !(p->flags & 1) && (p->pressed & p->held & mask) != 0;
}

/* VA 0x00436c20, 132 bytes, [I] row, status draft */
int guiInputUp(int pad)
{
    return repeatTest(pad, PAD_UP);
}

/* VA 0x00436ce0, 132 bytes, [I] row, status draft */
int guiInputDown(int pad)
{
    return repeatTest(pad, PAD_DOWN);
}

/* VA 0x00436da0, 132 bytes, [I] row, status draft */
int guiInputLeft(int pad)
{
    return repeatTest(pad, PAD_LEFT);
}

/* VA 0x00436e60, 132 bytes, [I] row, status draft */
int guiInputRight(int pad)
{
    return repeatTest(pad, PAD_RIGHT);
}

/* VA 0x00436f20, 51 bytes, [I] row, status draft */
int guiInputLeftHeld(int pad)
{
    return heldTest(pad, PAD_LEFT);
}

/* VA 0x00436f70, 51 bytes, [I] row, status draft */
int guiInputRightHeld(int pad)
{
    return heldTest(pad, PAD_RIGHT);
}

/* VA 0x00436fc0, 46 bytes, [I] row, status draft */
int guiInputAccept(int pad)
{
    return pressTest(pad, PAD_FIRE4 | PAD_FIRE6);      /* Cross (D) or Enter */
}

/* VA 0x00437000, 43 bytes, [I] row, status draft */
int guiInputCircle(int pad)
{
    return pressTest(pad, PAD_FIRE3);                  /* F */
}

/* VA 0x00437040, 43 bytes, [I] row, status draft */
int guiInputStart(int pad)
{
    return pressTest(pad, PAD_START);                  /* Space */
}

/* VA 0x00437080, 46 bytes, [I] row, status draft */
int guiInputBack(int pad)
{
    return pressTest(pad, PAD_FIRE1 | PAD_FIRE5);      /* Triangle (E) or Esc */
}

/* VA 0x004370c0, 43 bytes, [I] row, status draft */
int guiInputL1(int pad)
{
    return pressTest(pad, PAD_LTRIG1);
}

/* VA 0x00437100, 43 bytes, [I] row, status draft */
int guiInputL2(int pad)
{
    return pressTest(pad, PAD_LTRIG2);
}

/* VA 0x00437140, 43 bytes, [I] row, status draft */
int guiInputR1(int pad)
{
    return pressTest(pad, PAD_RTRIG1);
}

/* VA 0x00437180, 43 bytes, [I] row, status draft */
int guiInputR2(int pad)
{
    return pressTest(pad, PAD_RTRIG2);
}

/* VA 0x004371c0, 43 bytes, [I] row, status draft */
int guiInputSquare(int pad)
{
    return pressTest(pad, PAD_FIRE2);                  /* S */
}

/* VA 0x00437200, 104 bytes, [V] direct, status draft */
void guiInputInit(void)
{
    g_guiPad.device = zinputOpen(2);
    if (g_guiPad.device == NULL)
        g_guiPad.device = zinputOpen(0);
    g_guiPad.flags = 0;
    g_guiPad.repeat = 0;
    if (g_guiPad.device == NULL)
        zAssertFail("could not create controller device", GUIINPUT_SRC, 0x267, NULL);
    if (!(g_guiPad.device->flags & 1))
        g_guiPad.flags |= 1;                           /* not connected */
}

/* VA 0x00437290, 13 bytes, [I] row, status draft */
void guiInputShutdown(void)
{
    zinputClose(g_guiPad.device);
}

/* VA 0x004372b0, 229 bytes, [I] row, status draft */
uint32_t guiInputUpdate(void)
{
    /* returns bit 0 = the device was disconnected, bit 1 = it came back */
    uint32_t r = 0;

    if ((g_guiPad.flags & 1) && (g_guiPad.device->flags & 1)) {
        g_guiPad.flags &= ~1;
        g_guiPad.repeat = 0;
        r = 2;
    }
    if (!(g_guiPad.flags & 1)) {
        if (!(g_guiPad.device->flags & 1)) {
            g_guiPad.flags |= 1;
            r |= 1;
        } else {
            g_guiPad.held = g_guiPad.device->buttons;
            g_guiPad.pressed = g_guiPad.device->changed;
            g_guiPadPrev[0] = g_guiPad.unk_10;
            g_guiPadPrev[1] = g_guiPad.unk_14;
            g_guiPad.unk_10 = 0;
            g_guiPad.unk_14 = 0;
            g_guiPad.unk_18 = 0;
            g_guiPad.unk_1c = 0;
        }
    }
    return r;
}

/* VA 0x004373e0, 39 bytes, [I] row, status draft */
void guiInputClear(void)
{
    g_guiPad.held = 0;
    g_guiPad.pressed = 0;
    g_guiPad.repeat = 0;
    g_guiPad.unk_10 = 0;
    g_guiPad.unk_14 = 0;
    g_guiPad.unk_18 = 0;
    g_guiPad.unk_1c = 0;
}

/* VA 0x00437420, 47 bytes, [V] direct, status draft */
GuiPad *guiInputGetPad(int pad)
{
    if (pad != 0)
        zAssertFail("controller number out of range", GUIINPUT_SRC, 0x307, NULL);
    return &g_guiPad + pad;
}

/*
 * NgEngine/Zge/Pc/pcinput.c
 * Original: C:\NgEngine\Zge\Pc\pcinput.c
 * Evidence: [V] path string referenced by 3 function(s); the others [I] (the globals
 * 0x00674028-0x00674050, the enum callback passed by pcinputInit). DM-061 created 0x00493220
 * (the DirectInput enum callback) and 0x00493240 (pcinputShutdown).
 * 11 functions, VA 0x00493160-0x00493cde.
 * DM-061 (draft): 0x00493160-0x004937EF. DM-064 moved DM-059's drafts 0x004937F0-0x00493CDE in from
 * gap_pcinput__zrwCol.c (device release, acquire / unacquire on window activation, the per-frame
 * poll, a stub and the key-layout setter) [I]: they use the pcinput globals 0x00674028-0x00674153.
 * Non-matching C (AD-8). Types: zpc.h, contrler.h. Description: docs/re/input-camera.md 2.4.
 *
 * DirectInput 7: one shared keyboard device (non-exclusive, foreground) and, per pad, the first
 * attached joystick (exclusive, foreground; axes -127..127, dead zone 10 %). DM-059 renamed the poll
 * 0x00493910 (DM-057: zglRunCallbacks) to pcinputPollDevices: it polls the DirectInput devices.
 */
#include "decomp.h"
#include "zobj.h"
#include "zpc.h"
#include "ngif.h"

#define PCINPUT_FILE "C:\\NgEngine\\Zge\\Pc\\pcinput.c"

/* dinput.h values */
#define DIRECTINPUT_VERSION_7 0x0700
#define DIDEVTYPE_JOYSTICK    4
#define DIEDFL_ATTACHEDONLY   1
#define DISCL_EXCLUSIVE       0x01
#define DISCL_NONEXCLUSIVE    0x02
#define DISCL_FOREGROUND      0x04
#define DIPROP_RANGE          4
#define DIPROP_DEADZONE       5
#define DIPH_BYOFFSET         1
#define DIJOFS_X              0
#define DIJOFS_Y              4

typedef struct { uint32_t dwSize, dwHeaderSize, dwObj, dwHow; int32_t lMin, lMax; } DIPROPRANGE;
typedef struct { uint32_t dwSize, dwHeaderSize, dwObj, dwHow; uint32_t dwData; } DIPROPDWORD;
typedef struct {                       /* DIDEVICEINSTANCEA, 0x244 */
    uint32_t dwSize;                   /* +0x000 */
    PcGuid   guidInstance;             /* +0x004 */
    PcGuid   guidProduct;              /* +0x014 */
    uint32_t dwDevType;                /* +0x024 */
    char     tszInstanceName[260];     /* +0x028 */
    char     tszProductName[260];      /* +0x12C */
    PcGuid   guidFFDriver;             /* +0x230 */
    uint16_t wUsagePage, wUsage;       /* +0x240 */
} DIDEVICEINSTANCEA;

int32_t __stdcall DirectInputCreateEx(void *hinst, uint32_t version, const void *iid, void **out, void *outer); /* DINPUT import */
long    __stdcall GetWindowLongA(HWND hwnd, int index);                                                    /* USER32 */
extern const PcGuid g_IID_IDirectInput7A;         /* 0x0054F448 */
extern const PcGuid g_IID_IDirectInputDevice2A;   /* 0x0054F408 (the devices are created as Device2) */
extern const PcGuid g_GUID_SysKeyboard;           /* 0x0054F2F8 */
extern const uint8_t c_dfDIKeyboard[];            /* 0x0054E7E0 (dinput.lib) */
extern const uint8_t c_dfDIJoystick[];            /* 0x0054E7C8 (dinput.lib) */

static int           g_pcinputInitialised;   /* 0x00674028 */
static ZmemFreeList *g_pcinputPool;          /* 0x0067402C PcInputDevice, 2 per block */
static HWND          g_pcinputHwnd;          /* 0x00674034 */
static DInput7      *g_pcinputDI;            /* 0x00674038 */
static PcGuid        g_pcinputJoystickGuid;  /* 0x0067403C the first attached joystick */
static DIDevice7    *g_pcinputKeyboard;      /* 0x0067404C shared by all pads */
static uint32_t      g_pcinputKeyboardFlags; /* 0x00674050 bit 0: acquired */
static uint16_t      g_pcinputFlags;         /* 0x00674030 bit 0: keyboard read this frame */
static uint8_t       g_pcinputKeys[256];     /* 0x00674054 DIK key states */

/* VA 0x00493160, 186 bytes, [V] direct, status draft */
int pcinputInit(void)
{
    HWND *hwnd;

    if (g_pcinputInitialised)
        return 1;
    g_pcinputInitialised = 1;
    /* [V] the pool's destructor is zinputClose (0x004803C0), as in the exe */
    g_pcinputPool = zmemFreeListCreate(sizeof(PcInputDevice), 2, (ZmemFreeListCallback)zinputClose);
    g_zglGet(0x15, &hwnd, 0);                  /* the application window */
    g_pcinputHwnd = *hwnd;
    if (DirectInputCreateEx((void *)GetWindowLongA(g_pcinputHwnd, -6 /* GWL_HINSTANCE */),
                            DIRECTINPUT_VERSION_7, &g_IID_IDirectInput7A, (void **)&g_pcinputDI, 0) < 0) {
        zAssertFail("DirectInputCreate() failed.", PCINPUT_FILE, 0x145, 0);
        return 0;
    }
    zstringMemSet(&g_pcinputJoystickGuid, 0, sizeof(PcGuid));
    g_pcinputDI->vtbl->EnumDevices(g_pcinputDI, DIDEVTYPE_JOYSTICK, pcinputEnumJoystick,
                                   &g_pcinputJoystickGuid, DIEDFL_ATTACHEDONLY);
    return 1;
}

/* VA 0x00493220, 28 bytes, [I] row, status draft */
int pcinputEnumJoystick(const void *instance, void *ref)
{
    /* keep the first joystick's guidInstance and stop (DIENUM_STOP) */
    zstringMemCopy(ref, &((const DIDEVICEINSTANCEA *)instance)->guidInstance, sizeof(PcGuid));
    return 0;
}

/* VA 0x00493240, 204 bytes, [I] row, status draft */
int pcinputShutdown(void)
{
    ZmemFreeNode *node;

    if (!g_pcinputInitialised)
        return 1;
    if (g_pcinputKeyboard) {
        g_pcinputKeyboard->vtbl->Unacquire(g_pcinputKeyboard);
        g_pcinputKeyboardFlags &= ~1u;
    }
    for (node = zmemFreeListFirst(g_pcinputPool); node; node = zmemFreeListNext(node)) {
        PcInputDevice *pd = (PcInputDevice *)zmemFreeListNodeData(node);
        DIDevice7 *joy = (DIDevice7 *)pd->joystick;

        if (joy) {
            joy->vtbl->Unacquire(joy);
            pd->flags &= ~1u;
        }
    }
    if (g_pcinputKeyboard) {
        g_pcinputKeyboard->vtbl->Unacquire(g_pcinputKeyboard);
        g_pcinputKeyboard->vtbl->Release(g_pcinputKeyboard);
        g_pcinputKeyboard = 0;
    }
    zmemFreeListDestroy(g_pcinputPool);
    if (g_pcinputDI)
        g_pcinputDI->vtbl->Release(g_pcinputDI);
    g_pcinputDI = 0;
    g_pcinputInitialised = 0;
    return 1;
}

/* The keyboard is shared: the first record gets keymap 1, every later one keymap 2. */
static void pcinputPickKeymap(PcInputDevice *pd)
{
    pd->keymap = zmemFreeListGetUsedCount(g_pcinputPool) == 1 ? g_pcinputKeymap1 : g_pcinputKeymap2;
}

/* VA 0x00493310, 1103 bytes, [V] direct, status draft */
PcInputDevice *pcinputOpen(int type, struct ZInputDevice *owner)
{
    PcInputDevice *pd;
    DIDevice7 *joy;
    DIPROPRANGE range;
    DIPROPDWORD dead;
    DIDEVICEINSTANCEA info;
    const PcJoyMapHeader *const *map;

    pd = (PcInputDevice *)zmemFreeListAlloc(g_pcinputPool);
    zstringMemSet(pd, 0, sizeof(PcInputDevice));

    if (type == 1 || type == 2) {                       /* keyboard */
        if (g_pcinputKeyboard == 0) {
            g_pcinputKeyboard = pcinputCreateDevice(g_pcinputHwnd, g_GUID_SysKeyboard, c_dfDIKeyboard,
                                                    DISCL_NONEXCLUSIVE | DISCL_FOREGROUND); /* inlined */
            if (g_pcinputKeyboard == 0) {
                zAssertFail("Could not create an input device", PCINPUT_FILE, 0x37C, 0);
                zmemFreeListFree(g_pcinputPool, pd);
                return 0;
            }
            g_pcinputKeyboardFlags &= ~1u;
        }
        pcinputPickKeymap(pd);
    }

    if (type == 0 || type == 2) {                       /* joystick */
        joy = pcinputCreateDevice(g_pcinputHwnd, g_pcinputJoystickGuid, c_dfDIJoystick,
                                  DISCL_EXCLUSIVE | DISCL_FOREGROUND);                       /* inlined */
        pd->joystick = joy;
        if (joy == 0) {
            /* no joystick: fall back to the keyboard */
            g_pfnDebugPrintf("PCINPUT: Could not create controller\n");
            if (g_pcinputKeyboard == 0) {
                g_pcinputKeyboard = pcinputCreateDevice(g_pcinputHwnd, g_GUID_SysKeyboard, c_dfDIKeyboard,
                                                        DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
                if (g_pcinputKeyboard == 0) {
                    zAssertFail("Could not create an input device", PCINPUT_FILE, 0x389, 0);
                    zmemFreeListFree(g_pcinputPool, pd);
                    return 0;
                }
                g_pcinputKeyboardFlags &= ~1u;
            }
            pcinputPickKeymap(pd);
        } else {
            /* both axes -127..127 with a 10 % dead zone */
            range.dwSize = sizeof(DIPROPRANGE);
            range.dwHeaderSize = 0x10;
            range.dwHow = DIPH_BYOFFSET;
            range.lMin = -127;
            range.lMax = 127;
            range.dwObj = DIJOFS_X;
            joy->vtbl->SetProperty(joy, DIPROP_RANGE, &range);
            range.dwObj = DIJOFS_Y;
            joy->vtbl->SetProperty(joy, DIPROP_RANGE, &range);
            dead.dwSize = sizeof(DIPROPDWORD);
            dead.dwHeaderSize = 0x10;
            dead.dwHow = DIPH_BYOFFSET;
            dead.dwData = 1000;
            dead.dwObj = DIJOFS_X;
            joy->vtbl->SetProperty(joy, DIPROP_DEADZONE, &dead);
            dead.dwObj = DIJOFS_Y;
            joy->vtbl->SetProperty(joy, DIPROP_DEADZONE, &dead);
            pd->flags &= ~1u;

            /* pick the button map by product name, else by product GUID, else the default */
            info.dwSize = sizeof(DIDEVICEINSTANCEA);
            joy->vtbl->GetDeviceInfo(joy, &info);
            pd->joyMap = (const PcJoyMapHeader *)g_pcinputJoyMapDefault;
            for (map = g_pcinputJoyMaps; *map; map++) {
                const char *const *name = (*map)->names;
                const void *const *product = (*map)->products;

                for (; *name; name++) {
                    if (zstringCompare(*name, info.tszProductName) == 0) {
                        pd->joyMap = *map;
                        break;
                    }
                }
                if (*name == 0 && product) {
                    for (; *product; product++) {
                        const uint32_t *a = (const uint32_t *)*product;     /* repe cmpsd, 4 dwords */
                        const uint32_t *b = (const uint32_t *)&info.guidProduct;

                        if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]) {
                            pd->joyMap = *map;
                            break;
                        }
                    }
                }
            }
        }
    }
    pd->owner = owner;
    *((uint32_t *)owner + 1) |= 1;                      /* ZInputDevice.flags: connected */
    return pd;
}

/* VA 0x00493760, 144 bytes, [V] direct, status draft */
DIDevice7 *pcinputCreateDevice(HWND hwnd, PcGuid guid, const void *format, uint32_t coop)
{
    DIDevice7 *dev;

    if (g_pcinputDI->vtbl->CreateDeviceEx(g_pcinputDI, &guid, &g_IID_IDirectInputDevice2A, &dev, 0) < 0)
        return 0;
    if (dev->vtbl->SetDataFormat(dev, format) < 0) {
        zAssertFail("SetDataFormat() failed", PCINPUT_FILE, 0x175, 0);
        return 0;
    }
    if (dev->vtbl->SetCooperativeLevel(dev, hwnd, coop) < 0) {
        zAssertFail("SetCooperativeLevel() failed", PCINPUT_FILE, 0x17D, 0);
        return 0;
    }
    return dev;
}

/* VA 0x004937f0, 59 bytes, [I] row, status draft */
int pcinputReleaseDevice(struct ZInputDevice *dev)
{
    /* zinputClose; DM-059 */
    PcInputDevice *pd = (PcInputDevice *)dev->platform;
    DIDevice7 *joy = (DIDevice7 *)pd->joystick;

    if (joy != 0) {
        joy->vtbl->Unacquire(joy);
        joy->vtbl->Release(joy);
        pd->joystick = 0;
    }
    zmemFreeListFree(g_pcinputPool, pd);
    return 1;
}

/* VA 0x00493830, 96 bytes, [I] row, status draft */
int pcinputAcquireAll(void)
{
    /* MainWndProc: window activated; DM-059 */
    ZmemFreeNode *node;

    if (!g_pcinputInitialised)
        return 1;
    if (g_pcinputKeyboard != 0)
        g_pcinputKeyboard->vtbl->Acquire(g_pcinputKeyboard);
    for (node = zmemFreeListFirst(g_pcinputPool); node != 0; node = zmemFreeListNext(node)) {
        DIDevice7 *joy = (DIDevice7 *)((PcInputDevice *)zmemFreeListNodeData(node))->joystick;

        if (joy != 0)
            joy->vtbl->Acquire(joy);
    }
    return 1;
}

/* VA 0x00493890, 120 bytes, [I] row, status draft */
int pcinputUnacquireAll(void)
{
    /* MainWndProc: window deactivated; DM-059 */
    ZmemFreeNode *node;

    if (!g_pcinputInitialised)
        return 1;
    if (g_pcinputKeyboard != 0) {
        g_pcinputKeyboard->vtbl->Unacquire(g_pcinputKeyboard);
        g_pcinputKeyboardFlags &= ~1u;
    }
    for (node = zmemFreeListFirst(g_pcinputPool); node != 0; node = zmemFreeListNext(node)) {
        PcInputDevice *pd = (PcInputDevice *)zmemFreeListNodeData(node);
        DIDevice7 *joy = (DIDevice7 *)pd->joystick;

        if (joy != 0) {
            joy->vtbl->Unacquire(joy);
            pd->flags &= ~1u;
        }
    }
    return 1;
}

/* VA 0x00493910, 924 bytes, [I] row, status draft */
int pcinputPollDevices(void)
{
    /* (NgApp: once per frame; DM-059) For every device: clear the state, read the joystick (axes ->
     * channels 4..7 scaled by 1/127, buttons through the joystick map's (button, channel) pairs), the
     * keyboard (DIK codes through the keymap, read once per frame for all pads), then build the PAD_*
     * button word from the channels: held, changed, released. */
    static const struct { int ch; uint32_t bit; } map[] = {
        { 6, 0x1 }, { 7, 0x2 }, { 4, 0x4 }, { 5, 0x8 }, { 8, 0x10 }, { 9, 0x20 }, { 10, 0x40 },
        { 11, 0x80 }, { 12, 0x10000 }, { 13, 0x20000 }, { 14, 0x100 }, { 16, 0x400 },
        { 15, 0x200 }, { 17, 0x800 }, { 20, 0x4000 }, { 21, 0x8000 }
    };
    ZmemFreeNode *node;
    int32_t js[20];              /* DIJOYSTATE: lX, lY, ..., rgbButtons[32] at +0x30 */
    uint8_t jsButtons[32];

    g_pcinputFlags &= 0xFFFE;
    for (node = zmemFreeListFirst(g_pcinputPool); node != 0; node = zmemFreeListNext(node)) {
        PcInputDevice *pd = (PcInputDevice *)zmemFreeListNodeData(node);
        DIDevice7 *joy = (DIDevice7 *)pd->joystick;
        ZInputDevice *d = pd->owner;
        uint32_t prev = d->buttons, now = 0;
        unsigned i;

        zstringMemSet(&d->buttons, 0, 4);
        zstringMemSet(d->channel, 0, sizeof(d->channel));
        zstringMemSet(&d->unk_6c, 0, 4);
        if (joy != 0 && ((pd->flags & 1) || (joy->vtbl->Acquire(joy) >= 0 && (pd->flags |= 1)))) {
            int32_t r = joy->vtbl->Poll(joy);

            if (r >= 0 && (joy->vtbl->GetDeviceState(joy, 0x50, js) != DIERR_INPUTLOST ||
                           joy->vtbl->Acquire(joy) < 0)) {
                const uint8_t *b;

                d->channel[6] = js[1] < 0 ? (float)-js[1] * 0.007874016f : 0.0f;   /* up */
                d->channel[7] = js[1] > 0 ? (float)js[1] * 0.007874016f : 0.0f;    /* down */
                d->channel[4] = js[0] < 0 ? (float)-js[0] * 0.007874016f : 0.0f;   /* left */
                d->channel[5] = js[0] > 0 ? (float)js[0] * 0.007874016f : 0.0f;    /* right */
                zstringMemCopy(jsButtons, &js[12], sizeof(jsButtons));
                for (b = (const uint8_t *)(pd->joyMap + 1); b[0] != 0xFF; b += 2)
                    d->channel[b[1]] = (jsButtons[b[0]] & 0x80) ? 1.0f : 0.0f;
            }
        }
        if (pd->keymap != 0) {
            int ok = 1;

            if ((g_pcinputFlags & 1) == 0) {
                if ((g_pcinputKeyboardFlags & 1) == 0) {
                    if (g_pcinputKeyboard->vtbl->Acquire(g_pcinputKeyboard) < 0)
                        ok = 0;
                    else
                        g_pcinputKeyboardFlags |= 1;
                }
                if (ok && g_pcinputKeyboard->vtbl->GetDeviceState(g_pcinputKeyboard, 0x100, g_pcinputKeys) == DIERR_INPUTLOST &&
                    g_pcinputKeyboard->vtbl->Acquire(g_pcinputKeyboard) >= 0)
                    ok = 0;
                if (ok)
                    g_pcinputFlags |= 1;
            }
            if (ok) {
                const uint8_t *k;

                for (k = pd->keymap; k[0] != 0xFF; k += 2)
                    if (g_pcinputKeys[k[0]] & 0x80)
                        d->channel[k[1]] = 1.0f;
            }
        }
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (d->channel[map[i].ch] > 0.0f)
                now |= map[i].bit;
        d->buttons = now;
        d->changed = now ^ prev;
        d->released = (now ^ prev) & prev;
    }
    return 1;
}

/* VA 0x00493cb0, 3 bytes, [I] row, status draft */
int pcinputStub(struct ZInputDevice *device)
{
    /* zinput: a stub that returns 0; DM-059 */
    (void)device;
    return 0;
}

/* VA 0x00493cc0, 31 bytes, [I] row, status draft */
int zinputSetKeyLayout(struct ZInputDevice *device, const void *layout)
{
    /* DM-059 */
    if (device->platform == 0 || device == 0)      /* sic: the record is read before the NULL test */
        return 0;
    ((PcInputDevice *)device->platform)->keymap = (const uint8_t *)layout;
    return 1;
}

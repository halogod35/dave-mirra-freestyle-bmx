/*
 * NgEngine/Zge/zinput.c
 * Original: C:\NgEngine\Zge\zinput.c
 * Evidence: [V] path string referenced by 1 function(s); the others [I] (link order, the globals
 * 0x0065E2A8-0x0065E2AC, the ZInputDevice pool). The ZGE object pass created zinputShutdown (0x00480340) and
 * zinputSetRumble (0x00480440).
 * 6 functions, VA 0x004802f0-0x0048044d.
 * Draft. Non-matching C (AD-8). Types: contrler.h (ZInputDevice), zpc.h (pcinput).
 * Description: docs/re/input-camera.md 2.4.
 *
 * The platform-neutral input layer: a pool of two ZInputDevice records; each is backed by a pcinput
 * record (DirectInput keyboard and/or joystick) in device->platform.
 */
#include "decomp.h"
#include "zobj.h"
#include "zpc.h"

#define ZINPUT_FILE "C:\\NgEngine\\Zge\\zinput.c"


static int           g_zinputInitialised;  /* 0x0065E2A8 */
static ZmemFreeList *g_zinputPool;         /* 0x0065E2AC ZInputDevice, 2 per block */

/* VA 0x004802f0, 74 bytes, [V] direct, status draft */
int zinputInit(void)
{
    if (g_zinputInitialised)
        zAssertFail("Already initialized", ZINPUT_FILE, 0x49, 0);
    g_zinputInitialised = 1;
    g_zinputPool = zmemFreeListCreate(sizeof(ZInputDevice), 2, (ZmemFreeListCallback)zinputClose);
    pcinputInit();
    return 1;
}

/* VA 0x00480340, 44 bytes, [I] row, status draft */
int zinputShutdown(void)
{
    if (g_zinputInitialised) {
        zmemFreeListDestroy(g_zinputPool);
        pcinputShutdown();
        g_zinputInitialised = 0;
    }
    return 1;
}

/* VA 0x00480370, 74 bytes, [I] row, status draft */
ZInputDevice *zinputOpen(int type)
{
    ZInputDevice *dev = (ZInputDevice *)zmemFreeListAlloc(g_zinputPool);

    zstringMemSet(dev, 0, sizeof(ZInputDevice));
    dev->platform = pcinputOpen(type, dev);
    if (dev->platform == 0) {
        zmemFreeListFree(g_zinputPool, dev);
        return 0;
    }
    dev->type = type;
    return dev;
}

/* VA 0x004803c0, 33 bytes, [I] row, status draft */
int zinputClose(void *device)
{
    pcinputReleaseDevice((ZInputDevice *)device);
    zmemFreeListFree(g_zinputPool, device);
    return 1;
}

/* VA 0x00480400, 57 bytes, [I] row, status draft */
ZInputDevice *zinputGetDevice(int index)
{
    ZmemFreeNode *node;

    for (node = zmemFreeListFirst(g_zinputPool); node; node = zmemFreeListNext(node), index--) {
        if (index == 0)
            return (ZInputDevice *)zmemFreeListNodeData(node);
    }
    return 0;
}

/* VA 0x00480440, 14 bytes, [I] row, status draft */
int zinputSetRumble(ZInputDevice *device)
{
    return pcinputStub(device);      /* 0x00493CB0 returns 0: no force feedback on PC. No caller in the exe; contrlerRumble is a bare ret too */
}

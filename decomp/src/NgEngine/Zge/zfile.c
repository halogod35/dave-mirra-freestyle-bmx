/*
 * NgEngine/Zge/zfile.c
 * Original: C:\NgEngine\Zge\zfile.c
 * Evidence: [V] path string 0x00560724 referenced by nearly every function here; the five [I]
 *           ones by link order and literals.
 * 35 functions, VA 0x00483a60-0x004859a7 (the clean-up created zfileSetHeap / ClearHeap / GetSeparator).
 * Decompiled (non-matching C, AD-8). Types: decomp/include/zfile.h.
 *
 * Device-independent file API. A path is "dddd:name" (explicit device, 4-char name) or "name"
 * (default device = the first registered one, "rw00"). Every call is checked with asserts whose
 * line numbers are kept below. In the binary the helpers splitPath()/findDevice()/
 * zfileRegisterErrorMessage() are inlined at most call sites; here they are called.
 */
#include "decomp.h"

#define ZFILE_SRC "C:\\NgEngine\\Zge\\zfile.c"
#define CHECK_INIT(line) \
    ZASSERT(g_zfileInitialized, "File system has not been intialized.", ZFILE_SRC, (line))
#define CHECK_DEVICE(dev, line) \
    ZASSERT((dev) != NULL, "An invalid device identifier was given", ZFILE_SRC, (line))
#define CHECK_HANDLE(h, line) \
    ZASSERT((h)->device != NULL, "Invalid file handle", ZFILE_SRC, (line))

char             g_zfileErrorBuf[0x40];          /* 0x00670528 */
zfileDevice     *g_zfileDefaultDevice;           /* 0x00670568 */
char             g_zfileLastError[0x40];         /* 0x0067056C */
char             g_zfilePathBuf[0x80];           /* 0x006705AC */
zfileDeviceList  g_zfileDevices;                 /* 0x00670630 */
int              g_zfileInitialized;             /* 0x00670674 */
void            *g_zfileHandlePool;              /* 0x00670678 */
void            *g_zfileHeap;                    /* 0x0067067C optional memory pool (ZmemPool) for zfileLoad* */
zfileDevice     *g_zfileRamDiskDevice = (zfileDevice *)-1;   /* 0x0056062C */

/* Static helper (inlined in the binary, lines 0x98-0xa5): split "dddd:name" into the device
 * and the device-local name. With no (matching) prefix the whole path is the name. */
static const char *splitPath(const char *path, zfileDevice **dev)
{
    const char *devName;

    ZASSERT(dev != NULL && path != NULL, "Invalid paramters", ZFILE_SRC, 0x98);
    *dev = zfileFindDevice(path);
    ZASSERT(*dev != NULL, "Could not determine device for file name.", ZFILE_SRC, 0x9b);
    devName = zfileGetDeviceName(*dev);
    if (devName == NULL)
        return NULL;
    ZASSERT(zstringLength(devName) == 4, "Device name is not valid", ZFILE_SRC, 0xa5);
    if (zstringCompareN(path, devName, 4) == 0 && path[4] == ':')
        return path + 5;
    return path;
}

/* VA 0x00483a60, 59 bytes, [I] strings, status draft */
void zfileRegisterErrorMessage(const char *msg)
{
    /* [V] As compiled, the message is only kept when the buffer is already non-empty. The buffer
     * starts zeroed and zfileGetLastError clears it, so nothing is ever recorded in this build
     * (probably an inverted test in the original). */
    if (g_zfileErrorBuf[0] != '\0' && zstringLength(msg) < 0x3f)
        zstringCopy(g_zfileErrorBuf, msg);
    g_pfnDebugPrintf("zfileRegisterErrorMessage(): %s\n", msg);
}

/* VA 0x00483aa0, 224 bytes, [V] direct, status draft */
int zfileInit(void)
{
    if (!zfileGetPlatformDevices(&g_zfileDevices)) {
        zAssertFail("Error initializing platform file system", ZFILE_SRC, 0xed, 0);
        return 0;
    }
    ZASSERT(g_zfileDevices.count != 0, "There are no devices connected to the system.", ZFILE_SRC, 0xf0);
    ZASSERT(g_zfileHandlePool == NULL, "File system should not be initialized", ZFILE_SRC, 0xf2);
    /* 8 handles of 8 bytes; the callback closes handles still open when the pool goes away */
    g_zfileHandlePool = zmemFreeListCreate(sizeof(zfileHandle), 8, (void (__cdecl *)(void *))zfileHandlePoolClose);
    if (g_zfileHandlePool == NULL) {
        zAssertFail("Failed to create file handle allocator", ZFILE_SRC, 0xfa, 0);
        return 0;
    }
    g_zfileInitialized = 1;
    zfileSetDefaultDevice(g_zfileDevices.devices[0]);   /* inlined, lines 0x197/0x198 */
    return 1;
}

/* VA 0x00483b80, 181 bytes, [V] direct, status draft */
int zfileHandlePoolClose(zfileHandle *h)
{
    /* Pool element callback: an inlined zfileClose(h, NULL) that always reports success. */
    CHECK_INIT(0x280);
    CHECK_HANDLE(h, 0x281);
    if (!h->device->Close(h, NULL)) {
        zfileRegisterErrorMessage("Error when trying to close file through device.");
        return 1;
    }
    zmemFreeListFree(g_zfileHandlePool, h);
    return 1;
}

/* VA 0x00483c40, 98 bytes, [I] between, status draft */
int zfileShutdown(void)
{
    if (g_zfileInitialized) {
        zmemFreeListDestroy(g_zfileHandlePool);
        zfileReleasePlatformDevices();
        g_zfileHandlePool = NULL;
        CHECK_INIT(0x197);                               /* inlined zfileSetDefaultDevice(-1) */
        g_zfileDefaultDevice = (zfileDevice *)-1;
        g_zfileInitialized = 0;
    }
    return 1;
}

/* VA 0x00483cb0, 40 bytes, [V] direct, status draft */
zfileDeviceList *zfileGetDeviceList(void)
{
    CHECK_INIT(0x123);
    return &g_zfileDevices;
}

/* VA 0x00483ce0, 75 bytes, [V] direct, status draft */
const char *zfileGetDeviceName(zfileDevice *dev)
{
    CHECK_INIT(0x133);
    CHECK_DEVICE(dev, 0x134);
    return dev->GetName(dev);
}

/* VA 0x00483d30, 91 bytes, [V] direct, status draft */
char *zfileMakeFileName(zfileDevice *dev, int count, const char **names, void **values)
{
    CHECK_INIT(0x14d);
    CHECK_DEVICE(dev, 0x14e);
    return dev->MakeFileName(dev, count, names, values);
}

/* VA 0x00483d90, 372 bytes, [V] direct, status draft */
zfileDevice *zfileFindDevice(const char *path)
{
    char prefix[8];
    const char *name;
    uint32_t i;

    CHECK_INIT(0x162);
    ZASSERT(path != NULL, "Invalid parameter", ZFILE_SRC, 0x163);
    if (zstringLength(path) > 3) {
        for (i = 0; i < g_zfileDevices.count; i++) {
            name = zfileGetDeviceName(g_zfileDevices.devices[i]);
            if (name == NULL)
                continue;
            zstringCopy(prefix, name);
            zstringCat(prefix, ":");
            /* only 4 characters are compared, so "rw00x..." also matches [V] */
            if (zstringCompareN(path, prefix, 4) == 0) {
                /* [V] the binary asserts when the match is device 0 (test ebx,ebx): every
                 * explicit "rw00:" path triggers this assert. Probably an inverted check. */
                ZASSERT(i != 0, "Device list may be corrupt", ZFILE_SRC, 0x172);
                return g_zfileDevices.devices[i];
            }
        }
    }
    return zfileGetDefaultDevice();
}

/* VA 0x00483f10, 73 bytes, [V] direct, status draft */
zfileDevice *zfileGetDefaultDevice(void)
{
    CHECK_INIT(0x186);
    ZASSERT(g_zfileDefaultDevice != NULL, "The default device is invalid", ZFILE_SRC, 0x187);
    return g_zfileDefaultDevice;
}

/* VA 0x00483f60, 80 bytes, [I] between, status draft */
int zfileSetDefaultDevice(zfileDevice *dev)
{
    CHECK_INIT(0x197);
    CHECK_DEVICE(dev, 0x198);
    g_zfileDefaultDevice = dev;
    return 1;
}

/* VA 0x00483fb0, 76 bytes, [I] between, status draft */
char *zfileGetLastError(void)
{
    CHECK_INIT(0x1ab);
    if (g_zfileErrorBuf[0] == '\0')
        return NULL;
    zstringCopy(g_zfileLastError, g_zfileErrorBuf);
    g_zfileErrorBuf[0] = '\0';
    return g_zfileLastError;
}

/* VA 0x00484000, 86 bytes, [I] between, status draft */
int zfileCheckDevice(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0x1c9);
    CHECK_DEVICE(dev, 0x1ca);
    return dev->CheckDevice(dev, result, cb);
}

/* VA 0x00484060, 81 bytes, [I] between, status draft */
int zfileFormatDevice(zfileDevice *dev, zfileCallback *cb)
{
    CHECK_INIT(0x1dc);
    CHECK_DEVICE(dev, 0x1dd);
    return dev->Format(dev, cb);
}

/* VA 0x004840c0, 86 bytes, [I] between, status draft */
int zfileCheckFormatted(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0x1ed);
    CHECK_DEVICE(dev, 0x1ee);
    return dev->CheckFormatted(dev, result, cb);
}

/* VA 0x00484120, 91 bytes, [I] between, status draft */
int zfileGetFreeSpace(zfileDevice *dev, int arg, int *result, zfileCallback *cb)
{
    CHECK_INIT(0x200);
    CHECK_DEVICE(dev, 0x201);
    return dev->GetFreeSpace(dev, arg, result, cb);
}

/* VA 0x00484180, 704 bytes, [I] between, status draft */
int zfileExists(const char *path, int *exists, zfileCallback *cb)
{
    zfileDevice *dev;
    const char *local;

    CHECK_INIT(0x21e);
    ZASSERT(zstringLength(path) <= 0x7f, "An invalid file name was given", ZFILE_SRC, 0x220);
    local = splitPath(path, &dev);
    ZASSERT(local != NULL, "Failed to identify device from file name", ZFILE_SRC, 0x223);
    return dev->Exists(dev, local, exists, cb);
}

/* VA 0x00484440, 699 bytes, [I] between, status draft */
int zfileDelete(const char *path, zfileCallback *cb)
{
    zfileDevice *dev;
    const char *local;

    CHECK_INIT(0x239);
    ZASSERT(zstringLength(path) <= 0x7f, "An invalid file name was given", ZFILE_SRC, 0x23b);
    local = splitPath(path, &dev);
    ZASSERT(local != NULL, "Failed to identify device from file name", ZFILE_SRC, 0x23e);
    return dev->Delete(dev, local, cb);
}

/* VA 0x00484700, 854 bytes, [V] direct, status draft */
zfileHandle *zfileOpen(const char *path, int mode, zfileCallback *cb)
{
    zfileDevice *dev;
    const char *local;
    zfileHandle *h;

    CHECK_INIT(0x254);
    ZASSERT(zstringLength(path) <= 0x7f, "An invalid file name was given", ZFILE_SRC, 0x256);
    local = splitPath(path, &dev);
    ZASSERT(local != NULL, "Failed to identify device from file name", ZFILE_SRC, 0x259);
    ZASSERT(g_zfileHandlePool != NULL, "File system should be initialized.", ZFILE_SRC, 0x25c);
    h = (zfileHandle *)zmemFreeListAlloc(g_zfileHandlePool);
    h->devFile = NULL;                   /* no NULL check on the pool result [V] */
    h->device = dev;
    if (dev->Open(h, local, mode, cb))
        return h;
    zfileRegisterErrorMessage("Error opening file.");
    zmemFreeListFree(g_zfileHandlePool, h);
    return NULL;
}

/* VA 0x00484a60, 181 bytes, [V] direct, status draft */
int zfileClose(zfileHandle *h, zfileCallback *cb)
{
    CHECK_INIT(0x280);
    CHECK_HANDLE(h, 0x281);
    if (h->device->Close(h, cb)) {
        zmemFreeListFree(g_zfileHandlePool, h);
        return 1;
    }
    zfileRegisterErrorMessage("Error when trying to close file through device.");
    return 0;                            /* the handle stays allocated [V] */
}

/* VA 0x00484b20, 108 bytes, [V] direct, status draft */
int zfileRead(zfileHandle *h, void *buf, int n, zfileCallback *cb)
{
    CHECK_INIT(0x297);
    if (n == 0)
        return 0;
    CHECK_HANDLE(h, 0x29c);
    return h->device->Read(h, buf, n, cb);
}

/* VA 0x00484b90, 108 bytes, [V] direct, status draft */
int zfileWrite(zfileHandle *h, const void *buf, int n, zfileCallback *cb)
{
    CHECK_INIT(0x2ab);
    if (n == 0)
        return 0;
    CHECK_HANDLE(h, 0x2b0);
    return h->device->Write(h, buf, n, cb);
}

/* VA 0x00484c00, 98 bytes, [V] direct, status draft */
int zfileSeek(zfileHandle *h, int offset, int origin, zfileCallback *cb)
{
    CHECK_INIT(0x2bf);
    CHECK_HANDLE(h, 0x2c0);
    return h->device->Seek(h, offset, origin, cb);
}

/* VA 0x00484c70, 93 bytes, [V] direct, status draft */
int zfileTell(zfileHandle *h, int *pos, zfileCallback *cb)
{
    CHECK_INIT(0x2cf);
    CHECK_HANDLE(h, 0x2d0);
    return h->device->Tell(h, pos, cb);
}

/* VA 0x00484cd0, 93 bytes, [V] direct, status draft */
int zfileGetSize(zfileHandle *h, int *size, zfileCallback *cb)
{
    CHECK_INIT(0x2df);
    CHECK_HANDLE(h, 0x2e0);
    return h->device->GetSize(h, size, cb);
}

static void *loadAlloc(uint32_t size)
{
    return g_zfileHeap == NULL ? zmemAlloc(size) : zmemPoolAlloc(g_zfileHeap, size);
}

/* VA 0x00484d30, 820 bytes, [V] direct, status draft */
void *zfileLoadAll(const char *path, int *outSize)
{
    /* Open (read), get the size, allocate (g_zfileHeap or zmem), read everything, close.
     * Returns the buffer (free with zfileFreeLoaded) or NULL. Open/size/read/close are inlined. */
    zfileHandle *h;
    void *buf, *result = NULL;
    int size;

    h = zfileOpen(path, ZFILE_MODE_READ, NULL);
    if (h == NULL)
        return NULL;
    if (zfileGetSize(h, &size, NULL) == 1) {
        buf = loadAlloc((uint32_t)size);
        if (buf != NULL) {
            if (zfileRead(h, buf, size, NULL) == size) {
                result = buf;
                if (outSize != NULL)
                    *outSize = size;
            } else {
                zmemFree(buf);           /* zmemFree even when it came from g_zfileHeap [V] */
            }
        }
    }
    zfileClose(h, NULL);
    return result;
}

/* VA 0x00485070, 753 bytes, [V] direct, status draft */
void *zfileLoad(const char *path, int size)
{
    /* Like zfileLoadAll with a known size (no GetSize). size 0 gives an empty allocation. */
    zfileHandle *h;
    void *buf, *result = NULL;

    h = zfileOpen(path, ZFILE_MODE_READ, NULL);
    if (h == NULL)
        return NULL;
    buf = loadAlloc((uint32_t)size);
    if (buf != NULL) {
        if (size == 0 || zfileRead(h, buf, size, NULL) == size)
            result = buf;
        else
            zmemFree(buf);
    }
    zfileClose(h, NULL);
    return result;
}

/* VA 0x00485370, 36 bytes, [I] between, status draft */
void zfileFreeLoaded(void *buf)
{
    if (g_zfileHeap == NULL)
        zmemFree(buf);
    else
        zmemPoolFree(g_zfileHeap, buf);
}

/* VA 0x004853a0, 10 bytes, [I] row, status draft */
void zfileSetHeap(void *pool)
{
    /* (new function; no caller in the exe) Routes zfileLoad*'s buffers to a memory pool (see
     * loadAlloc). */
    g_zfileHeap = pool;
}

/* VA 0x004853b0, 11 bytes, [I] row, status draft */
void zfileClearHeap(void)
{
    /* new function; no caller */
    g_zfileHeap = NULL;
}

/* VA 0x004853c0, 152 bytes, [V] direct, status draft */
char *zfileGets(zfileHandle *h, char *buf, int size)
{
    /* fgets: read one byte at a time up to and including '\n' or size-1 bytes. */
    char c = '\0';
    char *p = buf;
    int left;

    if (size == 0)
        return NULL;
    left = size - 1;
    do {
        if (left == 0)
            break;
        if (zfileRead(h, p, 1, NULL) == 1) {
            c = *p++;
            left--;
        } else {
            left = 0;
        }
    } while (c != '\n');
    *p = '\0';
    return buf[0] != '\0' ? buf : NULL;
}

/* VA 0x00485460, 81 bytes, [V] direct, status draft */
char *zfileBuildPath(const char *dir, const char *name)
{
    /* dir + name in a static buffer; the overflow is only detected afterwards [V] */
    zstringCopy(g_zfilePathBuf, dir);
    zstringCat(g_zfilePathBuf, name);
    ZASSERT(zstringLength(g_zfilePathBuf) <= 0x7f, "Memory corruption when building path name", ZFILE_SRC, 0x3e0);
    return g_zfilePathBuf;
}

/* VA 0x004854c0, 3 bytes, [I] row, status draft */
char zfileGetSeparator(void)
{
    /* (new function; no caller): the path separator */
    return 0x5C;   /* '\\' */
}

/* VA 0x004854d0, 80 bytes, [I] between, status draft */
char *zfileGetDirectory(const char *path, char *out)
{
    /* everything up to and including the last '\' ("" if there is none) */
    int i;
    if (path == NULL)
        return NULL;
    i = zstringLength(path);
    if (i > 0x7f)
        return NULL;
    while (i >= 0 && path[i] != '\\')
        i--;
    zstringCopyN(out, path, (size_t)(i + 1));
    out[i + 1] = '\0';
    return out;
}

/* VA 0x00485520, 86 bytes, [I] between, status draft */
char *zfileGetExtension(const char *path, char *out)
{
    /* the text after the last '.', if it is at most 3 characters; otherwise NULL */
    int i;
    const char *ext;
    if (path == NULL)
        return NULL;
    i = zstringLength(path);
    if (i > 0x7f)
        return NULL;
    while (i >= 0 && path[i] != '.')
        i--;
    ext = path + i + 1;
    if (zstringLength(ext) < 4) {
        zstringCopy(out, ext);
        return out;
    }
    return NULL;
}

/* VA 0x00485580, 1064 bytes, [V] direct, status draft */
zfileHandle *zfileOpenMemory(void *address, int size)
{
    /* Open a memory block for reading through the RAM disk ("rd00:<address>:<size>"). */
    static const char *names[2] = { "ADDRESS", "SIZE" };
    void *values[2];
    const char *name;
    uint32_t i;

    CHECK_INIT(0x123);                   /* inlined zfileGetDeviceList */
    for (i = 0; i < g_zfileDevices.count; i++) {
        if (zstringCompare(zfileGetDeviceName(g_zfileDevices.devices[i]), "rd00") == 0) {
            g_zfileRamDiskDevice = g_zfileDevices.devices[i];
            break;
        }
    }
    ZASSERT(g_zfileRamDiskDevice != (zfileDevice *)-1, "Could not locate a RAM disk device on this system!",
            ZFILE_SRC, 0x44e);
    values[0] = address;
    values[1] = (void *)size;
    name = zfileMakeFileName(g_zfileRamDiskDevice, 2, names, values);
    if (name == NULL) {
        zAssertFail("Could not generate file name for RAM disk", ZFILE_SRC, 0x459, 0);
        return NULL;
    }
    return zfileOpen(name, ZFILE_MODE_READ, NULL);
}

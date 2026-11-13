/*
 * NgEngine/Zge/ZgeMods/zramdisk.c
 * Original: C:\NgEngine\Zge\ZgeMods\zramdisk.c
 * Evidence: [V] path string 0x00563280 referenced by every function here.
 * 17 functions, VA 0x0049eaa0-0x0049f4cf (the clean-up created zramdiskShutdown 0x0049F490) (+ the device table zfileDevice2 0x00563240).
 * Decompiled (non-matching C, AD-8).
 *
 * The "rd00" device: a block of memory read and written like a file. File names are
 * "rd00:<address hex>:<size hex>" (zfileOpenMemory builds them with MakeFileName).
 * Assert line numbers are the original ones.
 */
#include "decomp.h"

#define ZRAMDISK_SRC "C:\\NgEngine\\Zge\\ZgeMods\\zramdisk.c"
#define CHECK_INIT(line) \
    ZASSERT(g_zramdiskPool != NULL, "File system has not been intialized.", ZRAMDISK_SRC, (line))
#define CHECK_FILE(f, line) \
    ZASSERT((f) != NULL && (f)->base != NULL, "Invalid RAM disk file handle", ZRAMDISK_SRC, (line))

void *g_zramdiskPool;              /* 0x0067536C pool of 16 zramdiskFile; also the init flag */
char  g_zramdiskNameBuf[0x80];     /* 0x006752EC MakeFileName result */

static void notify(zfileCallback *cb, void *subject, int ok, int value)
{
    zfileCallbackInfo info;
    if (cb == NULL)
        return;
    info.user = cb->user;
    info.subject = subject;
    info.ok = ok;
    info.value = value;
    cb->fn(&info);
}

/* +0x08/+0x10/+0x14/+0x18 all answer "yes" on the RAM disk (shared shape of four slots). */
static int answerYes(void *subject, int *result, zfileCallback *cb, int line)
{
    if (result == NULL && cb == NULL) {
        zAssertFail("Must specify at least either a return value or a call back", ZRAMDISK_SRC, line, 0);
        return 1;
    }
    if (result != NULL)
        *result = 1;
    notify(cb, subject, 1, 1);
    return 1;
}

/* VA 0x0049eaa0, 40 bytes, [V] direct, status draft */
const char *zramdisk_GetName(zfileDevice *dev)
{
    CHECK_INIT(0x9b);
    return "rd00";                       /* 0x0055BF38 */
}

/* VA 0x0049ead0, 175 bytes, [V] direct, status draft */
char *zramdisk_MakeFileName(zfileDevice *dev, int count, const char **names, void **values)
{
    CHECK_INIT(0xbb);
    if (count == 2 && zstringCompare(names[0], "ADDRESS") == 0 && zstringCompare(names[1], "SIZE") == 0) {
        zstringPrintf(g_zramdiskNameBuf, "rd00:%#10x:%#10x", (uint32_t)values[0], (uint32_t)values[1]);
        /* always true (static buffer); kept from the binary */
        ZASSERT(g_zramdiskNameBuf != NULL, "Invalid file name created for RAM disk.", ZRAMDISK_SRC, 0xd8);
        return g_zramdiskNameBuf;
    }
    zfileRegisterErrorMessage("Invalid RAM disk file name attributes");
    return NULL;
}

/* VA 0x0049eb80, 150 bytes, [V] direct, status draft */
int zramdisk_CheckDevice(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0xe6);
    return answerYes(dev, result, cb, 0xe9);
}

/* VA 0x0049ec20, 50 bytes, [V] direct, status draft */
int zramdisk_Format(zfileDevice *dev, zfileCallback *cb)
{
    CHECK_INIT(0x108);
    zfileRegisterErrorMessage("Function not supported on this device");
    return 0;
}

/* VA 0x0049ec60, 150 bytes, [V] direct, status draft */
int zramdisk_CheckFormatted(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0x118);
    return answerYes(dev, result, cb, 0x11b);
}

/* VA 0x0049ed00, 150 bytes, [V] direct, status draft */
int zramdisk_GetFreeSpace(zfileDevice *dev, int arg, int *result, zfileCallback *cb)
{
    CHECK_INIT(0x13a);
    return answerYes(dev, result, cb, 0x13d);
}

/* VA 0x0049eda0, 150 bytes, [V] direct, status draft */
int zramdisk_Exists(zfileDevice *dev, const char *path, int *exists, zfileCallback *cb)
{
    CHECK_INIT(0x15c);
    return answerYes(dev, exists, cb, 0x15f);     /* any name "exists" */
}

/* VA 0x0049ee40, 50 bytes, [V] direct, status draft */
int zramdisk_Delete(zfileDevice *dev, const char *path, zfileCallback *cb)
{
    CHECK_INIT(0x17e);
    zfileRegisterErrorMessage("Function not supported on this device");
    return 0;
}

/* VA 0x0049ee80, 333 bytes, [V] direct, status draft */
int zramdisk_Open(zfileHandle *h, const char *path, int mode, zfileCallback *cb)
{
    uint8_t *base;
    uint32_t size;
    const char *colon;
    zramdiskFile *f;

    CHECK_INIT(0x193);
    ZASSERT(path != NULL && mode <= 3 && h != NULL, "Invalid parameters", ZRAMDISK_SRC, 0x194);
    /* "<address>:<size>", both hex (the "rd00:" prefix was stripped by zfile.c) */
    base = (uint8_t *)zstringToUInt(path, 16);
    colon = zstringFindChar(path, ':');
    if (colon == NULL) {
        zfileRegisterErrorMessage("Invalid file name");
        return 0;
    }
    size = zstringToUInt(colon + 1, 16);
    if (size == 0 || base == NULL)
        zfileRegisterErrorMessage("Invalid file name given for RAM disk");   /* not fatal [V] */

    f = (zramdiskFile *)zmemFreeListAlloc(g_zramdiskPool);
    ZASSERT(f != NULL, "Error allocating RAM disk file handle", ZRAMDISK_SRC, 0x1b7);
    f->pos = 0;
    f->base = base;
    f->size = size;
    f->flags = ((mode & ZFILE_MODE_READ) ? ZRAMDISK_READ : 0) | ((mode & ZFILE_MODE_WRITE) ? ZRAMDISK_WRITE : 0);
    h->devFile = f;
    notify(cb, h, 1, 0);                 /* [V] value uninitialised in the binary */
    return 1;
}

/* VA 0x0049efd0, 164 bytes, [V] direct, status draft */
int zramdisk_Close(zfileHandle *h, zfileCallback *cb)
{
    zramdiskFile *f;
    CHECK_INIT(0x1dc);
    f = (zramdiskFile *)h->devFile;
    CHECK_FILE(f, 0x1df);
    f->pos = 0;
    f->size = 0;
    f->base = NULL;
    zmemFreeListFree(g_zramdiskPool, f);
    notify(cb, h, 1, 0);                 /* [V] value uninitialised in the binary */
    return 1;
}

/* VA 0x0049f080, 229 bytes, [V] direct, status draft */
int zramdisk_Read(zfileHandle *h, void *buf, int n, zfileCallback *cb)
{
    zramdiskFile *f;
    int count;

    CHECK_INIT(0x1ff);
    f = (zramdiskFile *)h->devFile;
    CHECK_FILE(f, 0x202);
    if (!(f->flags & ZRAMDISK_READ)) {
        zfileRegisterErrorMessage("File access violation");
        return 0;
    }
    count = (f->pos + (uint32_t)n > f->size) ? (int)(f->size - f->pos) : n;   /* clamp at the end */
    if (count == 0)
        return 0;                        /* no callback at end of file [V] */
    zstringMemCopy(buf, f->base + f->pos, count);
    f->pos += count;
    notify(cb, h, count == n, count);
    return count;
}

/* VA 0x0049f170, 229 bytes, [V] direct, status draft */
int zramdisk_Write(zfileHandle *h, const void *buf, int n, zfileCallback *cb)
{
    zramdiskFile *f;
    int count;

    CHECK_INIT(0x234);
    f = (zramdiskFile *)h->devFile;
    CHECK_FILE(f, 0x237);
    if (!(f->flags & ZRAMDISK_WRITE)) {
        zfileRegisterErrorMessage("File access violation");
        return 0;
    }
    count = (f->pos + (uint32_t)n > f->size) ? (int)(f->size - f->pos) : n;   /* never grows */
    if (count == 0)
        return 0;
    zstringMemCopy(f->base + f->pos, buf, count);
    f->pos += count;
    notify(cb, h, count == n, count);
    return count;
}

/* VA 0x0049f260, 261 bytes, [V] direct, status draft */
int zramdisk_Seek(zfileHandle *h, int offset, int origin, zfileCallback *cb)
{
    zramdiskFile *f;
    uint32_t pos;

    CHECK_INIT(0x26b);
    f = (zramdiskFile *)h->devFile;
    CHECK_FILE(f, 0x26d);
    if (origin == ZFILE_SEEK_SET) {
        pos = (uint32_t)offset;
        if (pos > f->size) {
            zfileRegisterErrorMessage("Seek past end of file");
            return 0;                    /* position unchanged [V] */
        }
    } else if (origin == ZFILE_SEEK_CUR) {
        pos = f->pos + (uint32_t)offset;
        if (pos > f->size) {
            zfileRegisterErrorMessage("Seek past end of file");
            return 0;
        }
    } else {
        ZASSERT(origin == ZFILE_SEEK_END, "Invalid seek mode", ZRAMDISK_SRC, 0x298);
        pos = f->size - (uint32_t)offset;  /* offset counts back from the end; no range check [V] */
    }
    f->pos = pos;
    notify(cb, h, 1, (int)f->pos);
    return 1;
}

/* VA 0x0049f370, 92 bytes, [V] direct, status draft */
int zramdisk_Tell(zfileHandle *h, int *pos, zfileCallback *cb)
{
    zramdiskFile *f;
    CHECK_INIT(0x2b4);
    f = (zramdiskFile *)h->devFile;
    CHECK_FILE(f, 0x2b6);
    *pos = (int)f->pos;                  /* the callback is ignored [V] */
    return 1;
}

/* VA 0x0049f3d0, 123 bytes, [V] direct, status draft */
int zramdisk_GetSize(zfileHandle *h, int *size, zfileCallback *cb)
{
    zramdiskFile *f = (zramdiskFile *)h->devFile;
    CHECK_INIT(0x2c8);
    ZASSERT(size != NULL, "Invalid pointer for file size", ZRAMDISK_SRC, 0x2ca);
    CHECK_FILE(f, 0x2cb);
    *size = (int)f->size;                /* the callback is ignored [V] */
    return 1;
}

/* VA 0x0049f450, 63 bytes, [V] direct, status draft */
int zramdiskInit(void)
{
    ZASSERT(g_zramdiskPool == NULL, "File system has already been intialized.", ZRAMDISK_SRC, 0x2df);
    g_zramdiskPool = zmemFreeListCreate(sizeof(zramdiskFile), 16, NULL);
    return g_zramdiskPool != NULL;
}

/* The device table (data, 0x00563240). */
zfileDevice zfileDevice2 = {
    zramdisk_GetName, zramdisk_MakeFileName, zramdisk_CheckDevice, zramdisk_Format,
    zramdisk_CheckFormatted, zramdisk_GetFreeSpace, zramdisk_Exists, zramdisk_Delete,
    zramdisk_Open, zramdisk_Close, zramdisk_Read, zramdisk_Write,
    zramdisk_Seek, zramdisk_Tell, zramdisk_GetSize, NULL
};

/* VA 0x0049f490, 64 bytes, [V] direct, status draft */
int zramdiskShutdown(void)
{
    /* new function; zrwZgl.c's shutdown calls it */
    ZASSERT(g_zramdiskPool != NULL, "File system has already been shut down.", ZRAMDISK_SRC, 0x2f4);
    zmemFreeListDestroy(g_zramdiskPool);
    g_zramdiskPool = NULL;
    return 1;
}

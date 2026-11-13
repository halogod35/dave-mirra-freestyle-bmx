/*
 * NgEngine/Zge/Com/ZglRw/zrwFile.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwFile.c
 * Evidence: [V] path string 0x00563058 referenced by every function here.
 * 17 functions, VA 0x0049e1f0-0x0049ea91 (+ the device table zrwFileDevice 0x00562FF8).
 * Decompiled by DM-041 (non-matching C, AD-8).
 *
 * The "rw00" device: ordinary files through RenderWare's file interface (RwOsGetFileInterface),
 * so the engine and RenderWare share one set of stdio functions. File names are used as given
 * (after zfile.c strips an optional "rw00:" prefix). Assert line numbers are the original ones.
 */
#include "decomp.h"

#define ZRWFILE_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwFile.c"
#define CHECK_INIT(line) \
    ZASSERT(g_zrwFileInitialized, "File system has not been intialized.", ZRWFILE_SRC, (line))

RwFileFunctions g_zrwFileFuncs;          /* 0x006752C0 copy of RwOsGetFileInterface() */
int             g_zrwFileInitialized;    /* 0x006752E8 */
char            g_zrwFileNameBuf[0x80];  /* 0x00675240 MakeFileName result */
static const int s_seekOrigin[3] = { 0, 1, 2 };   /* 0x0054E130: ZFILE_SEEK_* -> SEEK_* */

/* Report through the optional callback (inlined at every call site in the binary). */
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

/* VA 0x0049e1f0, 37 bytes, [V] direct, status draft */
const char *zrwFile_GetName(zfileDevice *dev)
{
    CHECK_INIT(0x7c);
    return "rw00";                       /* 0x00563050 */
}

/* VA 0x0049e220, 137 bytes, [V] direct, status draft */
char *zrwFile_MakeFileName(zfileDevice *dev, int count, const char **names, void **values)
{
    CHECK_INIT(0x91);
    /* one attribute, "PATH", whose value is the file name */
    if (count == 1 && zstringCompare(names[0], "PATH") == 0) {
        const char *path = (const char *)values[0];
        ZASSERT(path != NULL, "Invalid file name given", ZRWFILE_SRC, 0x98);
        zstringCopy(g_zrwFileNameBuf, path);
        return g_zrwFileNameBuf;
    }
    zfileRegisterErrorMessage("Invalid attributes for RenderWare file name");
    return NULL;
}

/* VA 0x0049e2b0, 150 bytes, [V] direct, status draft */
int zrwFile_CheckDevice(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0xab);
    if (result == NULL && cb == NULL) {
        zAssertFail("Must specify at least either a return value or a call back", ZRWFILE_SRC, 0xae, 0);
        return 1;
    }
    if (result != NULL)
        *result = 1;                     /* the disk is always there */
    notify(cb, dev, 1, 1);
    return 1;
}

/* VA 0x0049e350, 50 bytes, [V] direct, status draft */
int zrwFile_Format(zfileDevice *dev, zfileCallback *cb)
{
    CHECK_INIT(0xcd);
    zfileRegisterErrorMessage("Function not permitted on this device");
    return 0;
}

/* VA 0x0049e390, 150 bytes, [V] direct, status draft */
int zrwFile_CheckFormatted(zfileDevice *dev, int *result, zfileCallback *cb)
{
    CHECK_INIT(0xdd);
    if (result == NULL && cb == NULL) {
        zAssertFail("Must specify at least either a return value or a call back", ZRWFILE_SRC, 0xe0, 0);
        return 1;
    }
    if (result != NULL)
        *result = 1;
    notify(cb, dev, 1, 1);
    return 1;
}

/* VA 0x0049e430, 50 bytes, [V] direct, status draft */
int zrwFile_GetFreeSpace(zfileDevice *dev, int arg, int *result, zfileCallback *cb)
{
    CHECK_INIT(0xff);
    zfileRegisterErrorMessage("Function not implemented for this device");
    return 0;
}

/* VA 0x0049e470, 157 bytes, [V] direct, status draft */
int zrwFile_Exists(zfileDevice *dev, const char *path, int *exists, zfileCallback *cb)
{
    int found;
    CHECK_INIT(0x110);
    if (exists == NULL && cb == NULL)
        zAssertFail("Must specify at least either a return value or a call back", ZRWFILE_SRC, 0x113, 0);
    found = g_zrwFileFuncs.rwfexist(path) != 0;
    if (exists != NULL)
        *exists = found;
    notify(cb, dev, 1, found);
    return 1;
}

/* VA 0x0049e510, 50 bytes, [V] direct, status draft */
int zrwFile_Delete(zfileDevice *dev, const char *path, zfileCallback *cb)
{
    CHECK_INIT(0x133);
    zfileRegisterErrorMessage("Function not implemented for this device");
    return 0;
}

/* VA 0x0049e550, 221 bytes, [V] direct, status draft */
int zrwFile_Open(zfileHandle *h, const char *path, int mode, zfileCallback *cb)
{
    static const char *const modes[4] = { "rb", "rb", "wb", "r+b" };   /* zrwFile_ModeTable 0x00563038 */
    char name[128];
    void *fp;

    CHECK_INIT(0x14c);
    ZASSERT(mode <= 3 && h != NULL && path != NULL, "Invalid parameters", ZRWFILE_SRC, 0x14e);
    zstringCopy(name, path);
    fp = g_zrwFileFuncs.rwfopen(name, modes[mode]);
    if (fp == NULL) {
        zfileRegisterErrorMessage("rwfopen() failed.");
        return 0;
    }
    h->devFile = fp;
    notify(cb, h, 1, 0);                 /* [V] value is left uninitialised in the binary */
    return 1;
}

/* VA 0x0049e630, 159 bytes, [V] direct, status draft */
int zrwFile_Close(zfileHandle *h, zfileCallback *cb)
{
    CHECK_INIT(0x171);
    ZASSERT(h != NULL && h->devFile != NULL, "Invalid file handle", ZRWFILE_SRC, 0x173);
    if (g_zrwFileFuncs.rwfclose(h->devFile) != 0) {
        zfileRegisterErrorMessage("rwfclose() failed");
        return 0;
    }
    notify(cb, h, 1, 0);                 /* [V] subject/value uninitialised in the binary */
    return 1;
}

/* VA 0x0049e6d0, 174 bytes, [V] direct, status draft */
int zrwFile_Read(zfileHandle *h, void *buf, int n, zfileCallback *cb)
{
    int got;
    CHECK_INIT(400);
    ZASSERT(h != NULL && h->devFile != NULL, "Invalid file handle", ZRWFILE_SRC, 0x192);
    got = (int)g_zrwFileFuncs.rwfread(buf, 1, n, h->devFile);
    if (got != n)
        zfileRegisterErrorMessage("rwfread() failed.");
    notify(cb, h, got == n, got);
    return got;
}

/* VA 0x0049e780, 174 bytes, [V] direct, status draft */
int zrwFile_Write(zfileHandle *h, const void *buf, int n, zfileCallback *cb)
{
    int put;
    CHECK_INIT(0x1b4);
    ZASSERT(h != NULL && h->devFile != NULL, "Invalid file handle", ZRWFILE_SRC, 0x1b6);
    put = (int)g_zrwFileFuncs.rwfwrite(buf, 1, n, h->devFile);
    if (put != n)
        zfileRegisterErrorMessage("rwfwrite() failed.");
    notify(cb, h, put == n, put);
    return put;
}

/* VA 0x0049e830, 224 bytes, [V] direct, status draft */
int zrwFile_Seek(zfileHandle *h, int offset, int origin, zfileCallback *cb)
{
    CHECK_INIT(0x1dd);
    ZASSERT(origin <= 2, "Invalid seek mode", ZRWFILE_SRC, 0x1de);
    ZASSERT(h != NULL && h->devFile != NULL, "Invalid file handle", ZRWFILE_SRC, 0x1e0);
    if (g_zrwFileFuncs.rwfseek(h->devFile, offset, s_seekOrigin[origin]) != 0) {
        zfileRegisterErrorMessage("rwfseek() failed");
        return 0;
    }
    /* the reported position is only right for SEEK_SET; other origins report 0 [V] */
    notify(cb, h, 1, origin == ZFILE_SEEK_SET ? offset : 0);
    return 1;
}

/* VA 0x0049e910, 50 bytes, [V] direct, status draft */
int zrwFile_Tell(zfileHandle *h, int *pos, zfileCallback *cb)
{
    CHECK_INIT(0x208);
    zfileRegisterErrorMessage("Function not implemented for this device");
    return 0;
}

/* VA 0x0049e950, 187 bytes, [V] direct, status draft */
int zrwFile_GetSize(zfileHandle *h, int *size, zfileCallback *cb)
{
    long pos;
    FILE *fp;

    CHECK_INIT(0x21a);
    ZASSERT(size != NULL, "Invalid pointer to file size.", ZRWFILE_SRC, 0x21b);
    /* Uses the CRT directly on the FILE * (not the RW table): remember the position, seek to
     * the end, read the position, seek back. ftell is called twice at the start [V]. */
    fp = (FILE *)h->devFile;
    pos = ftell(fp);
    *size = (int)ftell(fp);
    fseek(fp, 0, ZFILE_SEEK_END);
    *size = (int)ftell(fp);
    fseek(fp, pos, ZFILE_SEEK_SET);
    notify(cb, h, 1, *size);
    return 1;
}

/* VA 0x0049ea10, 68 bytes, [V] direct, status draft */
int zrwFileInit(void)
{
    ZASSERT(!g_zrwFileInitialized, "File system has already been intialized.", ZRWFILE_SRC, 0x24b);
    g_zrwFileFuncs = *RwOsGetFileInterface();     /* 10 pointers, 0x28 bytes */
    g_zrwFileInitialized = 1;
    return 1;
}

/* VA 0x0049ea60, 50 bytes, [V] direct, status draft */
int zrwFileShutdown(void)
{
    ZASSERT(g_zrwFileInitialized, "File system has already been shut down.", ZRWFILE_SRC, 0x25e);
    g_zrwFileInitialized = 0;
    return 1;
}

/* The device table (data, 0x00562FF8). */
zfileDevice zrwFileDevice = {
    zrwFile_GetName, zrwFile_MakeFileName, zrwFile_CheckDevice, zrwFile_Format,
    zrwFile_CheckFormatted, zrwFile_GetFreeSpace, zrwFile_Exists, zrwFile_Delete,
    zrwFile_Open, zrwFile_Close, zrwFile_Read, zrwFile_Write,
    zrwFile_Seek, zrwFile_Tell, zrwFile_GetSize, NULL
};

/*
 * NgEngine/Zge/Com/ZglRw/zrwRamDisk.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwRamDisk.c
 * Evidence: [V] zrwRamDiskOpen asserts with the path string 0x005638B8 (DM-002 had it as an orphan
 * path: the code was not a Ghidra function); the others [I]: they are the ten slots zrwRamDiskBegin
 * installs, and Begin/AddFile/End share the globals 0x006763E8-0x00676438. The card that owns this
 * gap (DM-061) created 0x004A3200, 0x004A3260 and 0x004A33F0-0x004A34E0; the four others were the
 * gap_zutil__zprofile functions. (Not one of DM-059's zrw files: no code had been found for it.)
 * 13 functions, VA 0x004a3200-0x004a35e6.
 * DM-061 (draft). Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 6.
 *
 * Lets RenderWare stream files from memory: zrwRamDiskBegin replaces RenderWare's file interface
 * (RwOsGetFileInterface) with the functions below, zrwRamDiskAddFile registers memory blocks, and
 * every rwfopen opens the next registered block through the RAM-disk zfile device ("rd00", file
 * name made from ADDRESS and SIZE) - the requested name is ignored, only its extension is checked
 * by rwfexist. One file can be open at a time. zrwRamDiskEnd restores the interface. Nothing in the
 * shipped exe calls Begin / AddFile / End [V, no xref].
 */
#include "decomp.h"
#include "ztools.h"

#define ZRWRAMDISK_FILE "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwRamDisk.c"
#define ZRWRAMDISK_MAX  2           /* g_zrwRamDiskFiles holds 2 entries (0x006763EC-0x00676403) */

RwFileFunctions *RwOsGetFileInterface(void);     /* RenderWare 0x004E8280 */

static zfileHandle    *g_zrwRamDiskHandle;      /* 0x006763E8 the open file; rwfopen returns &it */
static ZrwRamDiskFile  g_zrwRamDiskFiles[ZRWRAMDISK_MAX]; /* 0x006763EC */
static ZrwRamDiskFile *g_zrwRamDiskEnd;         /* 0x00676404 next free entry */
static ZrwRamDiskFile *g_zrwRamDiskNext;        /* 0x00676408 next file to open */
static RwFileFunctions g_zrwRamDiskSaved;       /* 0x00676410 RenderWare's own interface */
static zfileDevice    *g_zrwRamDiskDevice = (zfileDevice *)-1;  /* 0x00563874 found on first open */

/* VA 0x004a3200, 82 bytes, [I] row, status draft */
int zrwRamDiskExists(const char *name)
{
    char ext[64];

    if (g_zrwRamDiskNext->extension) {
        zfileGetExtension(name, ext);
        zstringUpper(ext);
        if (zstringCompare(ext, g_zrwRamDiskNext->extension) != 0)
            return 0;
    }
    return 1;
}

/* VA 0x004a3260, 347 bytes, [V] direct, status draft */
void *zrwRamDiskOpen(const char *name, const char *access)
{
    static const char *keys[2] = { "ADDRESS", "SIZE" };
    void *values[2];
    zfileDeviceList *list;
    const char *path;
    uint32_t i;

    if (g_zrwRamDiskNext == g_zrwRamDiskEnd) {
        g_pfnDebugPrintf("zrwFileRamDiskSetInf() not called\n");
        return 0;
    }
    if (g_zrwRamDiskHandle)
        zAssertFail("Too many RAM disk files in use", ZRWRAMDISK_FILE, 0x7B, 0);
    if (g_zrwRamDiskDevice == (zfileDevice *)-1) {
        list = zfileGetDeviceList();
        if (list == 0)
            zAssertFail("No devices!", ZRWRAMDISK_FILE, 0x83, 0);
        for (i = 0; i < list->count; i++) {
            if (zstringCompare(zfileGetDeviceName(list->devices[i]), "rd00") == 0) {
                g_zrwRamDiskDevice = list->devices[i];
                break;
            }
        }
        if (g_zrwRamDiskDevice == (zfileDevice *)-1) {
            zAssertFail("Could not locate a RAM disk device on this system!", ZRWRAMDISK_FILE, 0x91, 0);
            return 0;
        }
    }
    values[0] = g_zrwRamDiskNext->address;
    values[1] = (void *)g_zrwRamDiskNext->size;
    path = zfileMakeFileName(g_zrwRamDiskDevice, 2, (const char **)keys, values);
    if (path == 0)
        return 0;
    g_zrwRamDiskHandle = zfileOpen(path, 1, 0);
    if (g_zrwRamDiskHandle == 0)
        zAssertFail("Error opening RAM disk file.", ZRWRAMDISK_FILE, 0xA7, 0);
    return &g_zrwRamDiskHandle;
}

/* VA 0x004a33c0, 41 bytes, [I] row, status draft */
int zrwRamDiskClose(void *fp)
{
    zfileClose(*(zfileHandle **)fp, 0);
    *(zfileHandle **)fp = 0;
    g_zrwRamDiskNext++;                 /* the next open gets the next block */
    return 0;
}

/* VA 0x004a33f0, 33 bytes, [I] row, status draft */
unsigned zrwRamDiskRead(void *addr, unsigned size, unsigned count, void *fp)
{
    /* [V] returns zfileRead's result, not the element count RenderWare expects */
    return (unsigned)zfileRead(*(zfileHandle **)fp, addr, size * count, 0);
}

/* VA 0x004a3420, 33 bytes, [I] row, status draft */
unsigned zrwRamDiskWrite(const void *addr, unsigned size, unsigned count, void *fp)
{
    return (unsigned)zfileWrite(*(zfileHandle **)fp, (void *)addr, size * count, 0);
}

/* VA 0x004a3450, 26 bytes, [I] row, status draft */
char *zrwRamDiskGets(char *buf, int maxLen, void *fp)
{
    return zfileGets(*(zfileHandle **)fp, buf, maxLen);
}

/* VA 0x004a3470, 3 bytes, [I] row, status draft */
int zrwRamDiskFlush(void *fp)
{
    return 0;
}

/* VA 0x004a3480, 3 bytes, [I] row, status draft */
int zrwRamDiskPuts(const char *buf, void *fp)
{
    return 0;
}

/* VA 0x004a3490, 75 bytes, [I] row, status draft */
int zrwRamDiskEof(void *fp)
{
    int pos = 0, size = 0;

    zfileTell(*(zfileHandle **)fp, &pos, 0);
    zfileGetSize(*(zfileHandle **)fp, &size, 0);
    return (uint32_t)pos >= (uint32_t)size;
}

/* VA 0x004a34e0, 28 bytes, [I] row, status draft */
int zrwRamDiskSeek(void *fp, long offset, int origin)
{
    return zfileSeek(*(zfileHandle **)fp, offset, origin, 0);
}

/* VA 0x004a3500, 120 bytes, [I] row, status draft */
void zrwRamDiskBegin(void)
{
    RwFileFunctions *rw = RwOsGetFileInterface();

    zstringMemCopy(&g_zrwRamDiskSaved, rw, sizeof(RwFileFunctions));
    rw->rwfexist = zrwRamDiskExists;
    rw->rwfopen = zrwRamDiskOpen;
    rw->rwfclose = zrwRamDiskClose;
    rw->rwfread = zrwRamDiskRead;
    rw->rwfwrite = zrwRamDiskWrite;
    rw->rwfgets = zrwRamDiskGets;
    rw->rwfputs = zrwRamDiskPuts;
    rw->rwfeof = zrwRamDiskEof;
    rw->rwfseek = zrwRamDiskSeek;
    rw->rwfflush = zrwRamDiskFlush;
    g_zrwRamDiskEnd = g_zrwRamDiskFiles;
    g_zrwRamDiskNext = g_zrwRamDiskFiles;
    g_zrwRamDiskHandle = 0;
}

/* VA 0x004a3580, 51 bytes, [I] row, status draft */
void zrwRamDiskAddFile(void *address, uint32_t size, const char *extension)
{
    /* (no bound check against the two entries) */
    g_zrwRamDiskEnd->address = address;
    g_zrwRamDiskEnd->size = size;
    g_zrwRamDiskEnd->extension = extension;
    g_zrwRamDiskEnd++;
}

/* VA 0x004a35c0, 39 bytes, [I] row, status draft */
void zrwRamDiskEnd(void)
{
    zstringMemCopy(RwOsGetFileInterface(), &g_zrwRamDiskSaved, sizeof(RwFileFunctions));
    g_zrwRamDiskEnd = 0;
    g_zrwRamDiskNext = 0;
    g_zrwRamDiskHandle = 0;
}

/*
 * NgEngine/Zge/zasset.c
 * Original: C:\NgEngine\Zge\zasset.c
 * Evidence: [V] path string referenced by 4 function(s); the others [I] (link order, literals, calls)
 * 11 functions, VA 0x00480450-0x00480c36.
 * The ZGE object pass drafted zasset_DumpLibraryInfo and created zassetSkipAsset / zassetLoadAlloc (hole after
 * zassetLoadAsset).
 * Draft. Non-matching C (AD-8). ZAL asset libraries; format: docs/formats/ZAL.md,
 * Python reference: tools/dmbmx/formats/zal.py (the code below agrees with it; differences noted inline).
 *
 * Typical use: lib = zassetOpenLibrary(path); zassetFindAsset(lib, group, n);
 * buf = zmemAlloc(zassetGetAssetSize(lib)); zassetLoadAsset(lib, buf); ... zassetCloseLibrary(lib).
 * A library keeps a cursor (lib->cur) on one directory entry; LoadAsset advances it, NextGroup jumps.
 */
#include "decomp.h"

/* other modules: zfile.h/zengine.h via decomp.h; the rest until they are decompiled */
void *memcpy(void *dst, const void *src, size_t n);   /* the exe uses zstring_00482a50 */
int   zmem_00486290(void *pool);          /* number of live elements [I] */

#define ZASSET_FILE "C:\\NgEngine\\Zge\\zasset.c"
#define ZAL_DATA_BASE 0x10                 /* entry offsets are relative to the end of the magic header */

static void *g_zassetLibPool;              /* 0x0065E2B0 pool of 32 ZassetLib */

/* directory entry: u32 offset, u32 size (low 24 bits) [, u32 storedSize when compressed] */
#define ENTRY_OFFSET(e)  ((e)[0])
#define ENTRY_SIZE(e)    ((e)[1] & ZAL_SIZE_MASK)
#define ENTRY_STORED(e)  ((e)[2])

/* VA 0x00480450, 128 bytes, [I] row, status draft */
void zasset_DumpLibraryInfo(ZassetLib *lib)
{
    const uint8_t *group = lib->directory;
    int entrySize = lib->compressed ? 12 : 8;
    int g;

    g_pfnDebugPrintf("NumGroups=%d, AssetsPerGroup=%d\n", lib->numGroups, lib->assetsPerGroup);
    for (g = 0; g < lib->numGroups; g++) {
        /* only the group id and its first entry's size, and nothing for compressed libraries */
        if (!lib->compressed)
            g_pfnDebugPrintf("Asset=%d, Size=%d\n",*(const uint32_t *)group,
                             *(const uint32_t *)(group + 8) & ZAL_SIZE_MASK);
        group += lib->assetsPerGroup * entrySize + 4;
    }
}

/* VA 0x004804d0, 492 bytes, [V] direct, status draft */
ZassetLib *zassetOpenLibrary(const char *path)
{
    ZassetLib *lib;
    zfileHandle *h;
    uint32_t raw[7];                       /* 0x1C bytes: covers both header layouts */
    uint32_t hdrShift;                     /* 0 with the magic, -8 for the old 8-byte header */
    uint16_t words[2];                     /* flags, count */
    const char *err;

    g_pfnDebugPrintf("OPEN LIBRARY: %s\n", path);
    if (g_zassetLibPool == 0)
        g_zassetLibPool = zmemFreeListCreate(sizeof(ZassetLib), 0x20, 0);
    lib = (ZassetLib *)zmemFreeListAlloc(g_zassetLibPool);
    if (lib == 0) {
        g_pfnDebugPrintf("FAILED - Could not alocate memory for library\n");
        return 0;
    }
    h = zfileOpen(path, 1, 0);
    if (h == 0) {
        zmemFreeListFree(g_zassetLibPool, lib);
        return 0;
    }

    zfileSeek(h, 0, 0, 0);
    if (zfileRead(h, raw, sizeof(raw), 0) != (int)sizeof(raw)) {
        err = "FAILED - Could not load header\n";
        goto fail;
    }
    /* Newer files start with ZAL_MAGIC and have the flags/count words at 0x10; older ones have
     * no magic and keep them at 0x08 (ZAL.md 2.1). */
    hdrShift = (raw[0] == ZAL_MAGIC) ? 0 : (uint32_t)-8;
    memcpy(words, (uint8_t *)raw + 0x10 + hdrShift, 4);

    lib->file = h;
    /* compressed = (flags bit 15) * 2 + (count bit 15); either bit makes entries 12 bytes */
    lib->compressed = (int16_t)(((words[0] >> 15) & 1) * 2 + ((words[1] >> 15) & 1));
    lib->method = (uint16_t)((words[1] >> 14) & 1);           /* 0: LZRW3-A, 1: LZO1X */
    lib->entrySize = lib->compressed ? 12 : 8;
    lib->numGroups = (uint16_t)(words[1] & ZAL_GROUPS_MASK);
    lib->assetsPerGroup = (uint16_t)(words[0] & ZAL_ASSETS_MASK);
    lib->dirSize = (lib->assetsPerGroup * (uint32_t)lib->entrySize + 4) * lib->numGroups;
    lib->refMode = (uint8_t)((words[0] >> 13) & 1);
    if ((words[0] & ZAL_FLAG_REFMODE) && lib->compressed)
        zAssertFail("Compression in ref mode", ZASSET_FILE, 0xFB, 0);

    lib->directory = (uint8_t *)zmemAlloc(lib->dirSize);
    if (lib->directory == 0)
        return 0;                          /* NOTE(decomp): leaks the handle and the pool slot (as the exe) */
    zfileSeek(h, 0x14 + hdrShift, 0, 0);
    if ((uint32_t)zfileRead(h, lib->directory, lib->dirSize, 0) != lib->dirSize) {
        err = "FAILED to load library directory\n";
        goto fail;
    }
    lib->cur = (uint32_t *)(lib->directory + 4);              /* first asset of group 0 */
    return lib;

fail:
    g_pfnDebugPrintf(err);
    zfileClose(h, 0);
    return 0;                              /* NOTE(decomp): the pool slot is not freed here (as the exe) */
}

/* VA 0x004806c0, 93 bytes, [I] row, status draft */
void zassetCloseLibrary(ZassetLib *lib)
{
    g_pfnDebugPrintf("CLOSE LIBRARY\n");
    zfileClose(lib->file, 0);
    zmemFree(lib->directory);
    zmemFreeListFree(g_zassetLibPool, lib);
    if (zmem_00486290(g_zassetLibPool) == 0) {          /* last library closed: drop the pool */
        zmemFreeListDestroy(g_zassetLibPool);
        g_zassetLibPool = 0;
    }
}

/* VA 0x00480720, 11 bytes, [I] row, status draft */
int zassetGetNumGroups(ZassetLib *lib)
{
    return lib->numGroups;
}

/* VA 0x00480730, 73 bytes, [I] row, status draft */
int zassetNextGroup(ZassetLib *lib)
{
    /* Move the cursor to the first asset of the group after the current one. */
    uint32_t groupSize = (uint32_t)lib->entrySize * lib->assetsPerGroup + 4;
    uint32_t next = (uint32_t)((uint8_t *)lib->cur - 4 - lib->directory) / groupSize + 1;

    if ((int)next < (int)lib->numGroups) {
        lib->cur = (uint32_t *)(lib->directory + next * groupSize + 4);
        return 1;
    }
    return 0;
}

/* VA 0x00480780, 194 bytes, [V] direct, status draft */
int zassetFindAsset(ZassetLib *lib, uint32_t groupId, uint32_t assetNum)
{
    /* Ids normally equal their index, so try table[groupId] first, then search linearly
     * (ZAL.md 2.2; dmbmx ZalArchive.find does the linear search only). */
    uint32_t entrySize, groupSize, n;
    uint8_t *group;

    if (lib == 0)
        zAssertFail("lib", ZASSET_FILE, 0x18A, 0);
    entrySize = lib->entrySize;
    groupSize = lib->assetsPerGroup * entrySize + 4;
    assetNum &= 0xFFFF;

    if (groupId < lib->numGroups && *(uint32_t *)(lib->directory + groupSize * groupId) == groupId) {
        lib->cur = (uint32_t *)(lib->directory + groupSize * groupId + 4 + assetNum * entrySize);
        return 1;
    }
    group = lib->directory;
    for (n = lib->numGroups; (int)n > 0; n--) {
        if (*(uint32_t *)group == groupId) {
            lib->cur = (uint32_t *)(group + 4 + assetNum * entrySize);
            return 1;
        }
        group += groupSize;
    }
    g_pfnDebugPrintf("Failed to find asset Group=%d, AssetNum=%d\n", groupId, assetNum);
    return 0;
}

/* VA 0x00480850, 208 bytes, [V] direct, status draft */
uint32_t zassetGetAssetSize(ZassetLib *lib)
{
    /* Uncompressed size of the current asset. In ref mode the entry's data is the name of an
     * external file: open it, return its size and cache (entry, size) for zassetLoadAsset. */
    uint32_t size = ENTRY_SIZE(lib->cur);
    zfileHandle *ext;
    int extSize;

    if (!lib->refMode)
        return size;

    zfileSeek(lib->file, ENTRY_OFFSET(lib->cur) + ZAL_DATA_BASE, 0, 0);
    if ((uint32_t)zfileRead(lib->file, lib->refPath, size, 0) != size)
        zAssertFail("", ZASSET_FILE, 0x1CF, 0);
    ext = zfileOpen(lib->refPath, 1, 0);
    if (ext == 0) {
        g_pfnDebugPrintf("Not found - %s\n", lib->refPath);
        zAssertFail("", ZASSET_FILE, 0x1D8, 0);
    }
    zfileGetSize(ext, &extSize, 0);
    zfileClose(ext, 0);
    lib->refEntry = lib->cur;
    lib->refSize = (uint32_t)extSize;
    return (uint32_t)extSize;
}

/* VA 0x00480920, 11 bytes, [I] row, status draft */
int zassetGetAssetsPerGroup(ZassetLib *lib)
{
    return lib->assetsPerGroup;
}

/* VA 0x00480930, 643 bytes, [V] direct, status draft */
int zassetLoadAsset(ZassetLib *lib, void *dest)
{
    /* Load the current asset into dest (sized with zassetGetAssetSize) and advance the cursor.
     * Returns 0 if a read came up short. */
    uint32_t *e = lib->cur;
    uint32_t stored = lib->compressed ? ENTRY_STORED(e) : ENTRY_SIZE(e);
    int ok = 1;

    zfileSeek(lib->file, ENTRY_OFFSET(e) + ZAL_DATA_BASE, 0, 0);

    if (!lib->compressed && !lib->refMode) {                 /* plain asset */
        if ((uint32_t)zfileRead(lib->file, dest, stored, 0) != stored) {
            g_pfnDebugPrintf("FAILED - Could not load asset\n");
            ok = 0;
        }
    } else if (!lib->compressed) {                           /* ref mode: data lives in another file */
        zfileHandle *ext;
        if (lib->refEntry == lib->cur) {                     /* name and size cached by GetAssetSize */
            ext = zfileOpen(lib->refPath, 1, 0);
            if (ext == 0) {
                g_pfnDebugPrintf("Not found - %s\n", lib->refPath);
                zAssertFail("", ZASSET_FILE, 0x22D, 0);
            }
            zfileRead(ext, dest, lib->refSize, 0);
            zfileClose(ext, 0);
        } else {
            int extSize;
            ok = (uint32_t)zfileRead(lib->file, lib->refPath, stored, 0) == stored;
            if (!ok)
                g_pfnDebugPrintf("FAILED - Could not load asset\n");
            ext = zfileOpen(lib->refPath, 1, 0);
            if (ext == 0) {
                g_pfnDebugPrintf("Not found - %s\n", lib->refPath);
                zAssertFail("", ZASSET_FILE, 0x241, 0);
            }
            zfileGetSize(ext, &extSize, 0);
            zfileRead(ext, dest, extSize, 0);
            zfileClose(ext, 0);
            lib->refEntry = lib->cur;
            lib->refSize = (uint32_t)extSize;
        }
    } else {                                                 /* compressed asset */
        uint8_t *packed = (uint8_t *)zmemAlloc(stored);
        uint32_t outLen;
        ok = (uint32_t)zfileRead(lib->file, packed, stored, 0) == stored;
        if (!ok)
            g_pfnDebugPrintf("FAILED - Could not load asset\n");
        if (lib->method == 0)
            zcompresDecompress(ZCOMPRES_LZRW3A, packed, stored, (uint8_t *)dest, &outLen);
        else if (lib->method == 1)
            zcompresDecompress(ZCOMPRES_LZO1X, packed, stored, (uint8_t *)dest, &outLen);
        else
            zAssertFail("Unknown decompression method", ZASSET_FILE, 0x264, 0);
        if (outLen != ENTRY_SIZE(lib->cur)) {
            g_pfnDebugPrintf("ERROR - Size mismatch in decompressed data\n");
            zAssertFail("ERROR - Size mismatch in decompressed data", ZASSET_FILE, 0x269, 0);
        }
        zmemFree(packed);
    }
    lib->cur = (uint32_t *)((uint8_t *)lib->cur + lib->entrySize);
    return ok;
}

/* VA 0x00480bc0, 19 bytes, [I] row, status draft */
void zassetSkipAsset(ZassetLib *lib)
{
    lib->cur = (uint32_t *)((uint8_t *)lib->cur + lib->entrySize);
}

/* VA 0x00480be0, 87 bytes, [I] row, status draft */
void *zassetLoadAlloc(ZassetLib *lib, uint32_t group, uint32_t index, uint32_t *sizeOut)
{
    uint32_t size;
    void *buf;

    if (!zassetFindAsset(lib, group, index))
        return 0;
    size = zassetGetAssetSize(lib);
    buf = zmemAlloc(size);
    if (buf == 0)
        return 0;
    zassetLoadAsset(lib, buf);
    if (sizeOut)
        *sizeOut = size;
    return buf;
}

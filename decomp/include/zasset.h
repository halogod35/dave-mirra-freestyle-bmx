/*
 * zasset.h - ZAL asset library handle (NgEngine/Zge/zasset.c).
 *
 * Recovered from zassetOpenLibrary 0x004804D0 (pool element size 0x128, zmem pool of 0x20),
 * zassetFindAsset 0x00480780, zassetGetAssetSize 0x00480850, zassetLoadAsset 0x00480930.
 * Complete: every field is accounted for by the 8 zasset.c functions.
 * File format: docs/formats/ZAL.md. Offsets [V] (field use in the code), names [I].
 */
#ifndef ZASSET_H
#define ZASSET_H
#include "dmtypes.h"
#include "zfile.h"

typedef struct ZassetLib {
    zfileHandle *file;         /* +0x000 open .zal file */
    uint32_t     unk_004;      /* +0x004 never accessed by zasset.c [V]; pool memory, not cleared */
    uint16_t     numGroups;    /* +0x008 header u16 @0x12 & 0x3FFF (zasset_GetNumGroups) */
    uint16_t     assetsPerGroup; /* +0x00A header u16 @0x10 & 0x1FFF (zasset_GetAssetsPerGroup) */
    uint32_t    *cur;          /* +0x00C current directory entry (advanced by entrySize per load) */
    uint8_t     *directory;    /* +0x010 directory: per group u32 groupId + assetsPerGroup entries */
    uint32_t     dirSize;      /* +0x014 (assetsPerGroup * entrySize + 4) * numGroups */
    uint16_t     entrySize;    /* +0x018 8 (raw) or 12 (compressed) */
    int16_t      compressed;   /* +0x01A (flags>>15)*2 + (count>>15): != 0 -> 12-byte entries, data is zcompres'd */
    uint16_t     method;       /* +0x01C count bit 14: 0 -> zcompres method 1 (LZRW3-A), 1 -> method 2 (LZO1X) */
    uint8_t      refMode;      /* +0x01E 1: entry data is the name of an external file */
    char         refPath[0x101]; /* +0x01F external file name buffer (ref mode) */
    uint32_t    *refEntry;     /* +0x120 entry whose external size is cached */
    uint32_t     refSize;      /* +0x124 cached external file size */
} ZassetLib;                   /* 0x128 */

/* GHIDRA-SKIP-BEGIN */
typedef char zasset_h_size_check[(sizeof(ZassetLib) == 0x128) ? 1 : -1];
/* GHIDRA-SKIP-END */

/* On-disk header (docs/formats/ZAL.md 2.1). Files without the magic use an older 8-byte header:
 * the flags/count words then sit at offset 8 and the directory at 0x0C. */
typedef struct ZalHeader {
    uint32_t magic;            /* +0x00 ZAL_MAGIC */
    uint8_t  reserved[12];     /* +0x04 never read */
    uint16_t flags;            /* +0x10 bits 0-12 assets/group, 13 ref mode, 15 compressed (a) */
    uint16_t count;            /* +0x12 bits 0-13 groups, 14 codec select, 15 compressed (b) */
} ZalHeader;                   /* 0x14; the loader reads 0x1C bytes to cover both layouts */

#define ZAL_MAGIC          0x2A81511Cu
#define ZAL_ASSETS_MASK    0x1FFF
#define ZAL_FLAG_REFMODE   0x2000
#define ZAL_GROUPS_MASK    0x3FFF
#define ZAL_COUNT_METHOD   0x4000
#define ZAL_SIZE_MASK      0x00FFFFFFu

/* GHIDRA-SKIP-BEGIN */
typedef char zalheader_size_check[(sizeof(ZalHeader) == 0x14) ? 1 : -1];

ZassetLib *zassetOpenLibrary(const char *path);
void       zassetCloseLibrary(ZassetLib *lib);
int        zassetGetNumGroups(ZassetLib *lib);
int        zassetNextGroup(ZassetLib *lib);
int        zassetFindAsset(ZassetLib *lib, uint32_t groupId, uint32_t assetNum);
uint32_t   zassetGetAssetSize(ZassetLib *lib);
int        zassetGetAssetsPerGroup(ZassetLib *lib);
int        zassetLoadAsset(ZassetLib *lib, void *dest);
/* GHIDRA-SKIP-END */

#endif /* ZASSET_H */

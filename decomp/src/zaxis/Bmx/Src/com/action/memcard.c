/*
 * zaxis/Bmx/Src/com/action/memcard.c
 * Original: C:\zaxis\Bmx\Src\com\action\memcard.c
 * Evidence: [V] path string 0x00554FDC referenced by memcard_LoadSave; the other three [I]
 *           (link order + the shared ".\save\SAVE.DAT" literal 0x00554F88).
 * 4 functions, 678 bytes, VA 0x00412130-0x0041246e.
 * Decompiled by DM-041 (non-matching C, AD-8). Format: docs/formats/SAVE.md.
 *
 * On PC the "memory card" is the file .\save\SAVE.DAT (0x235C bytes):
 *   +0x0000 u32 magic 0x2A81511C   (written, never checked)
 *   +0x0004 u32 size  0x235C       (checked on load)
 *   +0x0008 SaveBlockA     (0xC3C)  <-> g_saveBlockA
 *   +0x0C44 SaveRecordEntry[12][24] (0x1680) <-> g_saveRecords
 *   +0x22C4 SaveProfile[2] (0x98)   <-> g_saveProfiles
 * There is no CRC or checksum (DM-012, docs/re/integrity-checks.md).
 */
#include "decomp.h"

#define MEMCARD_SRC   "C:\\zaxis\\Bmx\\Src\\com\\action\\memcard.c"
#define SAVE_PATH     ".\\save\\SAVE.DAT"
#define SAVE_MAGIC    0x2A81511Cu
#define SAVE_SIZE     0x235C

/* Result codes (values from the binary; names [I], they look like the PSX memory-card codes). */
#define MEMCARD_OK           0
#define MEMCARD_NO_FILE      5   /* the file could not be opened */
#define MEMCARD_FILE_EXISTS  6   /* memcard_CheckSaveExists: the file is there */
#define MEMCARD_BAD_DATA     8   /* size field wrong: "probably old version" */
#define MEMCARD_IO_ERROR     9   /* short read / short write */
#define MEMCARD_NO_MEMORY   (-1)

/* The file image of SAVE.DAT. */
typedef struct SaveFile {
    uint32_t        magic;              /* +0x0000 */
    uint32_t        size;               /* +0x0004 */
    SaveBlockA      blockA;             /* +0x0008 */
    SaveRecordEntry records[12][24];    /* +0x0C44 */
    SaveProfile     profiles[2];        /* +0x22C4 */
} SaveFile;                             /* 0x235C */

/* GHIDRA-SKIP-BEGIN */
typedef char memcard_c_size_check[(sizeof(SaveFile) == SAVE_SIZE) ? 1 : -1];
/* GHIDRA-SKIP-END */

/* VA 0x00412130, 47 bytes, [I] strings, status draft */
int memcard_CheckSaveExists(void)
{
    /* Probe: open for reading and close again. */
    zfileHandle *f = zfileOpen(SAVE_PATH, ZFILE_MODE_READ, NULL);
    if (f == NULL)
        return MEMCARD_NO_FILE;
    zfileClose(f, NULL);
    return MEMCARD_FILE_EXISTS;
}

/* VA 0x00412170, 3 bytes, [I] between, status draft */
int memcard_Stub0(void)
{
    /* Empty on PC (xor eax,eax; ret). Probably a PSX card operation (format/delete) compiled out. */
    return MEMCARD_OK;
}

/* VA 0x00412190, 325 bytes, [V] direct, status draft */
int memcard_LoadSave(void)
{
    /* The file name for the messages: a 200-byte local that the PC build never fills (the "%s"
     * prints stack garbage). Kept as in the binary [V]; probably a PSX leftover. */
    char name[200];
    int mark;
    SaveFile *buf;
    zfileHandle *f;
    int result;

    mark = zmemTempMark();
    buf = (SaveFile *)zmemTempAlloc(SAVE_SIZE);
    if (buf == NULL) {
        zAssertFail("error allocating temporary buffer for mem card load", MEMCARD_SRC, 0xd7, 0);
        return MEMCARD_NO_MEMORY;           /* note: the temp mark is not released here [V] */
    }

    f = zfileOpen(SAVE_PATH, ZFILE_MODE_READ, NULL);
    if (f == NULL) {
        result = MEMCARD_NO_FILE;
        g_pfnDebugPrintf("Error: couldn't open file \"%s\" for reading\n", name);
    }
    /* Check 1 (SAVE.md section 3): the read must return exactly 0x235C bytes. */
    else if (zfileRead(f, buf, SAVE_SIZE, NULL) != SAVE_SIZE) {
        result = MEMCARD_IO_ERROR;
        g_pfnDebugPrintf("Error: couldn't read from file \"%s\"\n", name);
        zfileClose(f, NULL);
    }
    /* Check 2: the size field at +4 must be 0x235C. The magic at +0 is NOT checked. */
    else if (buf->size != SAVE_SIZE) {
        result = MEMCARD_BAD_DATA;
        g_pfnDebugPrintf("Error: memory card data is invalid (probably old version)\n");
        zfileClose(f, NULL);
    }
    else {
        /* No CRC/checksum: the three blocks are copied as they are (order as in the binary). */
        result = MEMCARD_OK;
        g_saveBlockA = buf->blockA;
        zstringMemCopy(g_saveProfiles, buf->profiles, sizeof(g_saveProfiles));
        zstringMemCopy(g_saveRecords, buf->records, sizeof(g_saveRecords));
        zfileClose(f, NULL);
    }
    zmemTempRelease(mark);
    return result;
}

/* VA 0x00412340, 303 bytes, [I] strings, status draft */
int memcard_WriteSave(void)
{
    char name[200];                         /* never filled, as in memcard_LoadSave */
    int mark;
    SaveFile *buf;
    zfileHandle *f;

    mark = zmemTempMark();
    buf = (SaveFile *)zmemTempAlloc(SAVE_SIZE);
    if (buf == NULL)
        return MEMCARD_NO_MEMORY;           /* no assert on the write path [V] */

    buf->blockA = g_saveBlockA;
    zstringMemCopy(buf->profiles, g_saveProfiles, sizeof(buf->profiles));
    zstringMemCopy(buf->records, g_saveRecords, sizeof(buf->records));
    buf->magic = SAVE_MAGIC;                /* 0x004123A7 */
    buf->size = SAVE_SIZE;                  /* 0x004123AD */

    f = zfileOpen(SAVE_PATH, ZFILE_MODE_WRITE, NULL);
    if (f == NULL) {
        g_pfnDebugPrintf("Error: couldn't open file \"%s\" for writing\n", name);
        zmemTempRelease(mark);
        return MEMCARD_NO_FILE;
    }
    if (zfileWrite(f, buf, SAVE_SIZE, NULL) == SAVE_SIZE) {
        zfileClose(f, NULL);
        zmemTempRelease(mark);
        return MEMCARD_OK;
    }
    g_pfnDebugPrintf("Error: couldn't write to file \"%s\"\n", name);
    zfileClose(f, NULL);
    zmemTempRelease(mark);
    return MEMCARD_IO_ERROR;
}

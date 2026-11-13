/*
 * ziff.h - NgEngine IFF chunk reader (NgEngine/Zge/ziff.c) (DM-042).
 *
 * Chunks are { u32 id; u32 size; u8 data[size] } (LE), nested. A ZiffFile keeps a stack of up to
 * 8 open chunks (ziffChunkOpen 0x0049B130 asserts "Nested too deep" beyond that). Every open chunk
 * remembers the header of its *current child* (childId/childSize/childStart), which the
 * find/read/process calls work on. Handlers for chunk ids are registered globally
 * (ziffRegisterHandler 0x0049AAD0) and called by ziffParse 0x0049A8E0 / ziffChunkProcess.
 * Offsets [V] (field use in the code), names [I].
 */
#ifndef ZIFF_H
#define ZIFF_H
#include "dmtypes.h"
#include "zfile.h"

typedef struct ZiffChunk {
    uint32_t id;          /* +0x00 this chunk's id (4CC) */
    uint32_t size;        /* +0x04 data size */
    int32_t  start;       /* +0x08 file offset of this chunk's 8-byte header (-8 for a headerless root) */
    int32_t  end;         /* +0x0C start + 8 + size */
    int32_t  pos;         /* +0x10 current file offset */
    uint32_t childId;     /* +0x14 header of the current child chunk */
    uint32_t childSize;   /* +0x18 */
    int32_t  childStart;  /* +0x1C file offset of the current child's header */
} ZiffChunk;              /* 0x20 */

#define ZIFF_MAX_DEPTH 8  /* ziffChunkOpen fails once cur == &stack[7] ("Nested too deep") */

typedef struct ZiffFile {
    zfileHandle *file;    /* +0x00 */
    ZiffChunk   *cur;     /* +0x04 innermost open chunk (NULL before the root is opened) */
    ZiffChunk    stack[8];/* +0x08 */
} ZiffFile;               /* 0x108, zmem pool of 16 */

typedef int (__cdecl *ZiffHandlerFn)(ZiffFile *iff, void *user);

typedef struct ZiffHandler {
    uint32_t             id;    /* +0x00 chunk id */
    ZiffHandlerFn        fn;    /* +0x04 */
    struct ZiffHandler  *next;  /* +0x08 */
    uint32_t             unk0C; /* +0x0C set to 0 */
} ZiffHandler;                  /* 0x10, zmem pool of 64 */

typedef struct ZiffGlobals {
    int          initialized;  /* +0x00 0x006746D0 */
    void        *filePool;     /* +0x04 0x006746D4: pool of ZiffFile */
    void        *handlerPool;  /* +0x08 0x006746D8: pool of ZiffHandler */
    ZiffHandler *handlers;     /* +0x0C 0x006746DC: registered handlers (LIFO list) */
    uint32_t     rootId;       /* +0x10 0x006746E0: root header read by ziffOpen */
    uint32_t     rootSize;     /* +0x14 0x006746E4 */
    int32_t      rootHeaderLen;/* +0x18 0x006746E8: 8, or 0 for a headerless root */
} ZiffGlobals;                 /* 0x1C, cleared by ziffInit */

#define ZIFF_SEEK_DATA 0       /* offset from the first byte after the chunk header */
#define ZIFF_SEEK_CUR  1       /* offset from the current position */
#define ZIFF_ANY_ID    0xFFFFFFFFu

/* GHIDRA-SKIP-BEGIN */
typedef char ziff_h_size_check[(sizeof(ZiffChunk) == 0x20 && sizeof(ZiffFile) == 0x108 &&
                                sizeof(ZiffHandler) == 0x10 && sizeof(ZiffGlobals) == 0x1C) ? 1 : -1];

void        ziffInit(void);
int         ziffShutdown(void);                    /* 0x0049A740 (DM-064) */
ZiffFile   *ziffOpen(zfileHandle *file, uint32_t id, uint32_t size);
void        ziffClose(ZiffFile *iff);
int         ziffParse(ZiffFile *iff, void *user);
void        ziffPrintContents(ZiffFile *iff);      /* 0x0049AA00 (DM-064) */
ZiffHandler *ziffRegisterHandler(uint32_t id, ZiffHandlerFn fn);
uint32_t    ziffChunkGetId(ZiffFile *iff);         /* 0x0049ABB0 (DM-064) */
uint32_t    ziffChunkGetSize(ZiffFile *iff);
int32_t     ziffChunkGetOffset(ZiffFile *iff);     /* 0x0049ABD0 (DM-064) */
uint32_t    ziffChunkRead(ZiffFile *iff, void *dest);
uint32_t    ziffChunkReadData(ZiffFile *iff, void *dest, uint32_t n);
void        ziffChunkReadLong(ZiffFile *iff, int32_t *dest);
void        ziffChunkReadFloat(ZiffFile *iff, float *dest);
int32_t     ziffChunkNext(ZiffFile *iff);          /* 0x0049AD30 (DM-064) */
int         ziffChunkFindFirst(ZiffFile *iff, uint32_t id);
int         ziffChunkFindNext(ZiffFile *iff, uint32_t id);
void        ziffReadChunkHeader(ZiffFile *iff);
int32_t     ziffSeek(ZiffFile *iff, int mode, int32_t offset);
int         ziffChunkOpen(ZiffFile *iff);
int         ziffChunkClose(ZiffFile *iff);
int         ziffChunkProcess(ZiffFile *iff, void *user);
/* GHIDRA-SKIP-END */

#endif /* ZIFF_H */

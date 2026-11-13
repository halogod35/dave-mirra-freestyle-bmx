/*
 * NgEngine/Zge/ziff.c
 * Original: C:\NgEngine\Zge\ziff.c
 * Evidence: [V] path string referenced by 13 function(s); the others [I] (link order, literals, calls)
 * 22 functions, VA 0x0049a6d0-0x0049b328 (the clean-up created ziffShutdown, ziffPrintContents,
 * ziffChunkGetId, ziffChunkGetOffset, ziffChunkNext).
 * Draft. Non-matching C (AD-8). Types: decomp/include/ziff.h.
 *
 * The zanim node-table functions the zasset pass found here (0x0049A4E0-0x0049A630) were moved to zanim.c
 * when the zasset pass was merged (RANGE_FILES in tools/ghidra/build_symbols.py).
 *
 * IFF model: a chunk is { u32 id; u32 size; data[size] }. ZiffFile.stack holds the open chunks;
 * each remembers its current child's header. ziffChunkFindFirst/Next walk the children,
 * ziffChunkOpen enters the current child, ziffChunkClose leaves it, ziffParse calls the registered
 * handler for every child of the current chunk.
 */
#include "decomp.h"
#include "zanim.h"

/* other modules: zfile.h/zengine.h via decomp.h; the rest until they are decompiled */
void *memset(void *dst, int c, size_t n);            /* the exe uses zstring_00482b90 */
char *zstring_00482860(const char *fmt, ...);        /* sprintf into a static buffer [I] */

#define ZIFF_FILE "C:\\NgEngine\\Zge\\ziff.c"
#define FOURCC_ARGS(id) (int)((id) & 0xFF), (int)(((id) >> 8) & 0xFF), (int)(((id) >> 16) & 0xFF), (int)((id) >> 24)

static ZiffGlobals g_ziff;                 /* 0x006746D0 */

/* ------------------------------------------------------------------ ziff proper */

/* VA 0x0049a6d0, 97 bytes, [V] direct, status draft */
void ziffInit(void)
{
    if (g_ziff.initialized)
        zAssertFail("Already initialized", ZIFF_FILE, 0x11A, 0);
    memset(&g_ziff, 0, sizeof(g_ziff));
    g_ziff.handlerPool = zmemFreeListCreate(sizeof(ZiffHandler), 0x40, 0);
    g_ziff.filePool = zmemFreeListCreate(sizeof(ZiffFile), 0x10, 0);
    g_ziff.initialized = 1;
}

/* VA 0x0049a740, 76 bytes, [V] direct, status draft */
int ziffShutdown(void)
{
    /* new function; zgeShutdown calls it */
    if (!g_ziff.initialized)
        zAssertFail("Not initialized", ZIFF_FILE, 0x139, 0);
    zmemFreeListDestroy(g_ziff.handlerPool);
    zmemFreeListDestroy(g_ziff.filePool);
    g_ziff.initialized = 0;
    return 1;
}

/* VA 0x0049a790, 265 bytes, [V] direct, status draft */
ZiffFile *ziffOpen(zfileHandle *file, uint32_t id, uint32_t size)
{
    /*
     * id == ZIFF_ANY_ID: the file has no root header; the root chunk is the whole file (or
     * `size` bytes). Otherwise the file must start with chunk `id`.
     */
    ZiffFile *iff = (ZiffFile *)zmemFreeListAlloc(g_ziff.filePool);

    if (id == ZIFF_ANY_ID) {
        int fileSize = (int)size;
        if (size == 0)
            zfileGetSize(file, &fileSize, 0);
        g_ziff.rootId = ZIFF_ANY_ID;
        g_ziff.rootSize = (uint32_t)fileSize;
        g_ziff.rootHeaderLen = 0;
    } else {
        zfileRead(file, &g_ziff.rootId, 8, 0);              /* rootId, rootSize */
        g_ziff.rootHeaderLen = 8;
        if (g_ziff.rootId != id) {
            zmemFreeListFree(g_ziff.filePool, iff);
            g_pfnDebugPrintf("ZIFF: Non-matching chunkID('%c%c%c%c != %c%c%c%c')\n",
                             FOURCC_ARGS(id), FOURCC_ARGS(g_ziff.rootId));
            zAssertFail("ZIFF: Bad IFF file", ZIFF_FILE, 0x171, 0);
            return 0;
        }
    }
    iff->file = file;
    iff->cur = 0;
    ziffChunkOpen(iff);                                      /* opens the root on stack[0] */
    return iff;
}

/* VA 0x0049a8a0, 58 bytes, [V] direct, status draft */
void ziffClose(ZiffFile *iff)
{
    if (iff->cur != &iff->stack[0])
        zAssertFail("ZIFF: Chunks still open", ZIFF_FILE, 0x18E, 0);
    zmemFreeListFree(g_ziff.filePool, iff);
}

/* calls the handler registered for the current child, or asserts (shared by Parse/ChunkProcess) */
static int ziffDispatch(ZiffFile *iff, void *user)
{
    uint32_t id = iff->cur->childId;
    ZiffHandler *h;

    for (h = g_ziff.handlers; h; h = h->next) {
        if (h->id == id)
            return h->fn(iff, user) != 0;
    }
    zAssertFail(zstring_00482860("Unregistered header '%c%c%c%c'", FOURCC_ARGS(id)), ZIFF_FILE, 0x373, 0);
    return 0;
}

/* VA 0x0049a8e0, 280 bytes, [V] direct, status draft */
int ziffParse(ZiffFile *iff, void *user)
{
    /* Call the registered handler for every child of the current chunk, in file order. */
    ziffSeek(iff, ZIFF_SEEK_DATA, 0);
    ziffReadChunkHeader(iff);
    for (;;) {
        ZiffChunk *c = iff->cur;
        if (c->pos < c->end && !ziffDispatch(iff, user))
            return 0;
        c = iff->cur;
        /* skip to the next child: its header follows this child's data */
        if (ziffSeek(iff, ZIFF_SEEK_DATA, (c->childStart - c->start) + (int32_t)c->childSize) == -1)
            return 1;
        ziffReadChunkHeader(iff);
        if (iff->cur->pos == -1)
            return 1;
    }
}

/* VA 0x0049aa00, 206 bytes, [I] row, status draft */
void ziffPrintContents(ZiffFile *iff)
{
    /* (new function; no caller) Debug listing of the current chunk: one "%c%c%c%c" line per child,
     * indented. Like the exe, the loop only ends when the position reaches the chunk's end. */
    ZiffChunk *c;

    g_pfnDebugPrintf("ZIFF: Contents for 0x%x\n", iff);
    g_pfnDebugIndent(1);
    ziffSeek(iff, ZIFF_SEEK_DATA, 0);
    ziffReadChunkHeader(iff);
    for (;;) {
        c = iff->cur;
        if (c->pos >= c->end)
            break;
        g_pfnDebugPrintf("%c%c%c%c\n", FOURCC_ARGS(c->childId));
        c = iff->cur;
        if (ziffSeek(iff, ZIFF_SEEK_DATA, (c->childStart - c->start) + (int32_t)c->childSize) == -1)
            continue;                    /* the exe retries the same test */
        ziffReadChunkHeader(iff);
    }
    g_pfnDebugIndent(-1);
}

/* VA 0x0049aad0, 214 bytes, [V] direct, status draft */
ZiffHandler *ziffRegisterHandler(uint32_t id, ZiffHandlerFn fn)
{
    ZiffHandler *h;

    if (!g_ziff.initialized)
        zAssertFail("Not initialized", ZIFF_FILE, 0x1E0, 0);
    for (h = g_ziff.handlers; h; h = h->next) {
        if (h->id == id) {
            if (h->fn == fn) {
                g_pfnDebugPrintf("ZIFF: Not re-registering '%c%c%c%c'\n", FOURCC_ARGS(id));
            } else {
                h->fn = fn;
                g_pfnDebugPrintf("ZIFF: Changing function for '%c%c%c%c'\n", FOURCC_ARGS(id));
            }
            return h;
        }
    }
    h = (ZiffHandler *)zmemFreeListAlloc(g_ziff.handlerPool);
    h->id = id;
    h->fn = fn;
    h->unk0C = 0;
    h->next = g_ziff.handlers;                               /* newest first */
    g_ziff.handlers = h;
    return h;
}

/* VA 0x0049abb0, 11 bytes, [I] row, status draft */
uint32_t ziffChunkGetId(ZiffFile *iff)
{
    /* new function; no caller */
    return iff->cur->childId;
}

/* VA 0x0049abc0, 11 bytes, [I] between, status draft */
uint32_t ziffChunkGetSize(ZiffFile *iff)
{
    return iff->cur->childSize;
}

/* VA 0x0049abd0, 16 bytes, [I] row, status draft */
int32_t ziffChunkGetOffset(ZiffFile *iff)
{
    /* (new function; no caller): bytes from the current child's header to the current position */
    return iff->cur->pos - iff->cur->childStart;
}

/* VA 0x0049abe0, 69 bytes, [V] direct, status draft */
uint32_t ziffChunkRead(ZiffFile *iff, void *dest)
{
    /* Read the whole current child; the position must be right after its header. */
    ZiffChunk *c = iff->cur;

    if (c->pos - c->childStart != 8) {
        zAssertFail("Not at start of chunk", ZIFF_FILE, 0x251, 0);
        return 0;
    }
    return ziffChunkReadData(iff, dest, c->childSize);
}

/* VA 0x0049ac30, 186 bytes, [V] direct, status draft */
uint32_t ziffChunkReadData(ZiffFile *iff, void *dest, uint32_t n)
{
    /* Read up to n bytes of the current child's data (clamped to what is left).
     * Returns the clamped count, not the count zfileRead delivered. */
    ZiffChunk *c = iff->cur;
    uint32_t left = (uint32_t)(c->childStart + 8 + (int32_t)c->childSize - c->pos);

    if (n == 0) {
        zAssertFail("Trying to read 0 bytes", ZIFF_FILE, 0x265, 0);
        return 0;
    }
    if (left == 0) {
        zAssertFail("All data read", ZIFF_FILE, 0x266, 0);
        return 0;
    }
    if (n > left) {
        g_pfnDebugPrintf("ZIFF: ziffChunkReadData: Clamping size\n");
        n = left;
    }
    iff->cur->pos += zfileRead(iff->file, dest, (int)n, 0);
    return n;
}

/* VA 0x0049acf0, 21 bytes, [I] between, status draft */
void ziffChunkReadLong(ZiffFile *iff, int32_t *dest)
{
    ziffChunkReadData(iff, dest, 4);
}

/* VA 0x0049ad10, 21 bytes, [I] between, status draft */
void ziffChunkReadFloat(ZiffFile *iff, float *dest)
{
    /* identical code to ziffChunkReadLong; [I] the float variant by analogy */
    ziffChunkReadData(iff, dest, 4);
}

/* checks that the current child's size is a multiple of 4 (line = assert line of the caller) */
static int ziffChildSizeOk(ZiffFile *iff, int line)
{
    uint32_t size = iff->cur->childSize;
    if (size & 3) {
        uint32_t id = iff->cur->childId;
        zAssertFail(zstring_00482860("Data length not word size for header '%c%c%c%c'", FOURCC_ARGS(id)),
                    ZIFF_FILE, line, 0);
        return 0;
    }
    return 1;
}

/* walks forward from the current child until one with `id` (or any, for ZIFF_ANY_ID) is found */
static int ziffScanFor(ZiffFile *iff, uint32_t id)
{
    ZiffChunk *c = iff->cur;

    if (id == ZIFF_ANY_ID)
        return c->pos - c->childStart != -1;
    while (c = iff->cur, c->pos < c->end) {
        if (c->childId == id)
            return iff->cur->pos - iff->cur->childStart != -1;
        if (!ziffChildSizeOk(iff, 0xE4))
            return 0;
        if (ziffSeek(iff, ZIFF_SEEK_DATA, (c->childStart - c->start) + (int32_t)c->childSize) != -1)
            ziffReadChunkHeader(iff);
    }
    return 0;
}

/* VA 0x0049ad30, 92 bytes, [I] row, status draft */
int32_t ziffChunkNext(ZiffFile *iff)
{
    /* (new function; no caller) Skip to the next child: seek past the current one and read its
     * header. Returns the position after the header, -1 at the end of the chunk. */
    ZiffChunk *c = iff->cur;

    if (ziffSeek(iff, ZIFF_SEEK_DATA, (c->childStart - c->start) + (int32_t)c->childSize) == -1)
        return -1;
    ziffReadChunkHeader(iff);
    return iff->cur->pos;
}

/* VA 0x0049ad90, 320 bytes, [V] direct, status draft */
int ziffChunkFindFirst(ZiffFile *iff, uint32_t id)
{
    /* Rewind to the first child of the current chunk and find the first child with `id`. */
    ziffSeek(iff, ZIFF_SEEK_DATA, 0);
    ziffReadChunkHeader(iff);
    if (!ziffChildSizeOk(iff, 0xCD))
        return 0;
    return ziffScanFor(iff, id);
}

/* VA 0x0049aed0, 335 bytes, [V] direct, status draft */
int ziffChunkFindNext(ZiffFile *iff, uint32_t id)
{
    /* Skip the current child and find the next child with `id`. */
    ZiffChunk *c = iff->cur;

    if (!ziffChildSizeOk(iff, 0xCD))
        return 0;
    if (ziffSeek(iff, ZIFF_SEEK_DATA, (c->childStart - c->start) + (int32_t)c->childSize) == -1)
        return 0;
    ziffReadChunkHeader(iff);
    if (iff->cur->pos == -1)
        return 0;
    return ziffScanFor(iff, id);
}

/* VA 0x0049b020, 49 bytes, [I] between, status draft */
void ziffReadChunkHeader(ZiffFile *iff)
{
    /* Read the 8-byte header at the current position as the current child. */
    ZiffChunk *c = iff->cur;
    c->childStart = c->pos;
    c->pos += zfileRead(iff->file, &c->childId, 8, 0);     /* childId, childSize */
}

/* VA 0x0049b060, 194 bytes, [V] direct, status draft */
int32_t ziffSeek(ZiffFile *iff, int mode, int32_t offset)
{
    /* Seek inside the current chunk. Returns the new position, or -1 at the chunk's end
     * (and on errors, after asserting). */
    ZiffChunk *c = iff->cur;
    int32_t pos;

    if (mode == ZIFF_SEEK_DATA)
        pos = c->start + 8 + offset;
    else if (mode == ZIFF_SEEK_CUR)
        pos = c->pos + offset;
    else {
        zAssertFail("Invalid seek mode", ZIFF_FILE, 0x2EA, 0);
        return -1;
    }
    if (pos > c->end) {
        zAssertFail("Seeked passed the end", ZIFF_FILE, 0x2ED, 0);
        return -1;
    }
    if (pos < c->start) {
        zAssertFail("Seeked passed the start", ZIFF_FILE, 0x2EE, 0);
        return -1;
    }
    zfileSeek(iff->file, pos, 0, 0);
    iff->cur->pos = pos;
    return (pos == iff->cur->end) ? -1 : pos;
}

/* VA 0x0049b130, 281 bytes, [V] direct, status draft */
int ziffChunkOpen(ZiffFile *iff)
{
    /* Enter the current child (or, the first time, the root described by g_ziff). */
    ZiffChunk *c;

    if (iff->cur == 0) {
        c = iff->cur = &iff->stack[0];
        c->id = g_ziff.rootId;
        c->size = g_ziff.rootSize;
        c->start = g_ziff.rootHeaderLen - 8;                 /* -8 for a headerless root */
    } else {
        ZiffChunk *parent = iff->cur;
        if (parent >= &iff->stack[ZIFF_MAX_DEPTH - 1]) {
            zAssertFail("Nested too deep", ZIFF_FILE, 0x31A, 0);
            return 0;
        }
        if (parent->childStart + 8 != parent->pos) {
            zAssertFail("Not at the start of the chunk", ZIFF_FILE, 0x31E, 0);
            return 0;
        }
        c = iff->cur = parent + 1;
        c->id = parent->childId;
        c->size = parent->childSize;
        c->start = parent->pos - 8;
    }
    c->end = c->start + 8 + (int32_t)c->size;
    c->pos = c->start;
    c->childStart = c->pos;
    if (c->childStart < 0) {
        /* headerless root: present the root itself as the current child */
        c->childId = c->id;
        c->childSize = c->size;
        c->pos += 8;
        return 1;
    }
    /* re-read this chunk's own header as the "current child", so GetSize/Read work on it */
    c->childStart = c->pos;
    c->pos += zfileRead(iff->file, &c->childId, 8, 0);
    return 1;
}

/* VA 0x0049b250, 72 bytes, [V] direct, status draft */
int ziffChunkClose(ZiffFile *iff)
{
    /* Leave the innermost chunk; the file position returns to the parent's position. */
    if ((uint8_t *)iff->cur > (uint8_t *)&iff->stack[0]) {
        iff->cur--;
        return ziffSeek(iff, ZIFF_SEEK_CUR, 0) != -1;
    }
    zAssertFail("Too many closes", ZIFF_FILE, 0x348, 0);
    return 0;
}

/* VA 0x0049b2a0, 137 bytes, [V] direct, status draft */
int ziffChunkProcess(ZiffFile *iff, void *user)
{
    /* Call the handler for the current child (nothing to do at the end of the chunk). */
    if (iff->cur->pos >= iff->cur->end)
        return 1;
    return ziffDispatch(iff, user);
}

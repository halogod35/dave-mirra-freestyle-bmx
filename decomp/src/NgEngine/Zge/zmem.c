/*
 * NgEngine/Zge/zmem.c
 * Original: C:\NgEngine\Zge\zmem.c
 * Evidence: [V] path string 0x00560904 referenced by most functions here (the string-referencing
 *           functions that Ghidra only created in DM-044 are marked "[I] row" by the generator);
 *           the rest by link order, globals and calls.
 * 41 functions, VA 0x004859b0-0x00486909 (DM-064 created zmemDump 0x004859B0 and zmemSetHeader
 * 0x004859C0 in front).
 * Decompiled by DM-044 (non-matching C, AD-8). Types: decomp/include/zmem.h.
 *
 * Four allocators live here (see zmem.h): the general zmemAlloc/zmemFree (platform heap, or a
 * "redirect" memory pool), the static/temp stack arena, fixed-size "free lists" and first-fit
 * "memory pools". The names follow the exe's own messages: "Free list pool has been exhausted."
 * vs "zmemPoolFree(): Memory has been previously freed." (DM-041/042 had the two swapped).
 * zmemAlloc is inlined into most callers in the binary; here it is called.
 */
#include "decomp.h"

#define ZMEM_SRC "C:\\NgEngine\\Zge\\zmem.c"
#define ROUND4(n) (((n) + 3u) & ~3u)

/* Platform allocator hooks, set by App_Open (zrwZgl.c 0x0048AF48) to the RenderWare memory functions. */
extern void *(__cdecl *g_zmemPlatformAlloc)(uint32_t size);              /* 0x0056149C */
extern void  (__cdecl *g_zmemPlatformFree)(void *p);                     /* 0x005614A0 */
extern void *(__cdecl *g_zmemPlatformRealloc)(void *p, uint32_t size);   /* 0x005614A4 */

int           g_zmemFreeListUsed[ZMEM_MAX_FREELISTS];     /* 0x00670680 */
ZmemGlobals   g_zmem;                                     /* 0x00670880 */
ZmemFreeList  g_zmemFreeLists[ZMEM_MAX_FREELISTS];        /* 0x006708A8 */
ZmemPoolFuncs g_zmemPoolDefaultFuncs;                     /* 0x006718A8 */
ZmemPool      g_zmemPools[ZMEM_MAX_POOLS];                /* 0x006718B0 */
int           g_zmemPoolSystemInitialized;                /* 0x006722B0 */

/* Header word of a zmemAlloc/zmemTempAlloc block. */
#define HDR(p)          (((uint32_t *)(p))[-1])
#define HDR_KIND(h)     ((h) >> ZMEM_KIND_SHIFT)
#define HDR_SET_SIZE(h, size) ((h) = ((h) & ~ZMEM_SIZE_MASK) | ((size) & ZMEM_SIZE_MASK))

#define BLOCK_PREV(b)   ((ZmemBlock *)((b)->prevFree & ~ZMEM_BLOCK_FREE))
#define BLOCK_IS_FREE(b) (((b)->prevFree & ZMEM_BLOCK_FREE) != 0)
#define BLOCK_SIZE(b)   ((uint32_t)((uint8_t *)(b)->next - (uint8_t *)(b)) - sizeof(ZmemBlock))
#define BLOCK_SET_PREV(b, p) ((b)->prevFree = ((b)->prevFree & ZMEM_BLOCK_FREE) | (uint32_t)(p))

/* VA 0x004859b0, 1 bytes, [I] row, status draft */
void zmemDump(void)
{
    /* (DM-064 created; zgeDump 0x0049FA60 calls it after zobjDumpAll). An empty debug dump: the
     * release build compiled its body out. It sits between zfile.c's last and zmem.c's first function;
     * zmem.c is the guess [I] (a memory report is what a ZGE dump would print next). */
}

/* VA 0x004859c0, 42 bytes, [I] row, status draft */
void zmemSetHeader(uint32_t *hdr, uint32_t size, uint32_t unused, uint32_t kind)
{
    /* (DM-064 created; no caller) Writes a zmemAlloc block header: the size in bits 0-29, the kind
     * (ZMEM_KIND_*) in bits 30-31. The third argument is unused. */
    (void)unused;
    *hdr = (kind << ZMEM_KIND_SHIFT) | (size & ZMEM_SIZE_MASK);
}

/* VA 0x004859f0, 67 bytes, [V] direct, status draft */
int zmemInit(void)
{
    ZASSERT(!g_zmem.initialized, "Already initialized", ZMEM_SRC, 0x202);
    zmemPoolSystemInit();
    zstringMemSet(&g_zmem, 0, sizeof(g_zmem));
    g_zmem.initialized = 1;
    return 1;
}

/* VA 0x00485a40, 83 bytes, [I] row, status draft */
int zmemShutdown(void)
{
    if (g_zmem.initialized) {
        zmemPoolSystemShutdown();
        if (g_zmem.staticBase != NULL)
            zmemFree(g_zmem.staticBase);            /* inlined */
        g_zmem.staticBase = NULL;
        g_zmem.initialized = 0;
    }
    return 1;
}

/* VA 0x00485aa0, 116 bytes, [V] direct, status draft */
void *zmemAlloc(uint32_t size)
{
    uint32_t sz = ROUND4(size);
    uint32_t *hdr;

    /* the block is 8 bytes larger than needed; only 4 hold the header [V] */
    if (g_zmem.redirectPool == NULL)
        hdr = (uint32_t *)g_zmemPlatformAlloc(ROUND4(sz) + 8);
    else
        hdr = (uint32_t *)zmemPoolAlloc(g_zmem.redirectPool, ROUND4(sz) + 8);
    if (hdr == NULL) {
        zAssertFail("Memory allocation failed", ZMEM_SRC, 0x26b, 0);
        return NULL;
    }
    *hdr = (ZMEM_KIND_DYNAMIC << ZMEM_KIND_SHIFT) | (sz & ZMEM_SIZE_MASK);
    return hdr + 1;
}

/* VA 0x00485b20, 36 bytes, [I] row, status draft */
void zmemFree(void *p)
{
    /* No kind check: a static (temp) block passed here would go to the platform heap. */
    if (g_zmem.redirectPool == NULL)
        g_zmemPlatformFree((uint32_t *)p - 1);
    else
        zmemPoolFree(g_zmem.redirectPool, (uint32_t *)p - 1);
}

/* VA 0x00485b50, 304 bytes, [V] direct, status draft */
void *zmemResize(void *p, uint32_t size)
{
    uint32_t *hdr = (uint32_t *)p - 1;
    uint32_t sz;
    uint8_t *newTop;

    if (size == 0)
        zAssertFail("Use zmemFree to resize block to 0", ZMEM_SRC, 0x2ac, 0);
    sz = ROUND4(size);
    switch (HDR_KIND(*hdr)) {
    case ZMEM_KIND_STATIC:
        /* [V] g_zmem.lastAlloc holds the *header* address (zmemTempAlloc stores the old top), but it
         * is compared with the user pointer, so this always fails: temp blocks cannot be resized. */
        if (g_zmem.lastAlloc != (uint8_t *)p) {
            zAssertFail("Can only resize last allocated block", ZMEM_SRC, 0x2bd, 0);
            return NULL;
        }
        newTop = (uint8_t *)(((uint32_t)p + sz + 0xb) & ~3u);
        if (newTop >= g_zmem.staticEnd) {
            zAssertFail("Cant resize", ZMEM_SRC, 0x2c7, 0);
            return NULL;
        }
        g_zmem.staticTop = newTop;
        break;
    case ZMEM_KIND_DYNAMIC:
        if (g_zmem.redirectPool != NULL) {
            zAssertFail("Cannot resize memory when allocations are re-directed to a pool.", ZMEM_SRC, 0x2e2, 0);
            return NULL;
        }
        hdr = (uint32_t *)g_zmemPlatformRealloc(hdr, sz + 8);
        if (hdr == NULL) {
            zAssertFail("Cant resize", ZMEM_SRC, 0x2e8, 0);
            return NULL;
        }
        break;
    default:
        zAssertFail("Unknown pool", ZMEM_SRC, 0x2f7, 0);
        return NULL;
    }
    HDR_SET_SIZE(*hdr, sz);
    return hdr + 1;
}

/* VA 0x00485c80, 173 bytes, [V] direct, status draft */
int zmemStaticInit(uint32_t size)
{
    g_zmem.staticBase = (uint8_t *)zmemAlloc(size);      /* inlined */
    if (g_zmem.staticBase == NULL) {
        zAssertFail("Could not initialize static memory manager", ZMEM_SRC, 0x32b, 0);
        return 0;
    }
    g_zmem.staticTop = g_zmem.staticBase;
    g_zmem.staticEnd = g_zmem.staticBase + size;
    return 1;
}

/* VA 0x00485d30, 73 bytes, [I] row, status draft */
int zmemStaticShutdown(void)
{
    if (g_zmem.staticBase != NULL)
        zmemFree(g_zmem.staticBase);                      /* inlined */
    g_zmem.staticBase = NULL;
    return 1;
}

/* VA 0x00485d80, 107 bytes, [V] direct, status draft */
void *zmemTempAlloc(uint32_t size)
{
    uint32_t sz = ROUND4(size);
    uint8_t *hdr = g_zmem.staticTop;
    uint8_t *newTop = (uint8_t *)(((uint32_t)hdr + ROUND4(sz) + 0xb) & ~3u);  /* header 4 + 4 slack */

    if (newTop > g_zmem.staticEnd) {
        zAssertFail("Memory allocation failed - Out of RAM", ZMEM_SRC, 0x360, 0);
        return NULL;
    }
    g_zmem.lastAlloc = hdr;
    g_zmem.staticTop = newTop;
    /* kind bits are cleared (= ZMEM_KIND_STATIC), size stored */
    *(uint32_t *)hdr = sz & ZMEM_SIZE_MASK;
    return hdr + 4;
}

/* VA 0x00485df0, 17 bytes, [I] row, status draft */
uint32_t zmemTempGetFree(void)
{
    return (uint32_t)(g_zmem.staticEnd - g_zmem.staticTop) - 4;
}

/* VA 0x00485e10, 35 bytes, [I] row, status draft */
void zmemTempRelease(int mark)
{
    /* only moves the top down; releasing to a mark above the top does nothing */
    if ((uint8_t *)mark < g_zmem.staticTop)
        g_zmem.staticTop = (uint8_t *)ROUND4((uint32_t)mark);
    g_zmem.lastAlloc = NULL;
}

/* VA 0x00485e40, 6 bytes, [I] row, status draft */
int zmemTempMark(void)
{
    return (int)g_zmem.staticTop;
}

/* VA 0x00485e50, 10 bytes, [I] row, status draft */
void zmemSetPool(ZmemPool *pool)
{
    g_zmem.redirectPool = pool;
}

/* VA 0x00485e60, 11 bytes, [I] row, status draft */
void zmemClearPool(void)
{
    g_zmem.redirectPool = NULL;
}

/* VA 0x00485e70, 6 bytes, [I] row, status draft */
ZmemPool *zmemGetPool(void)
{
    return g_zmem.redirectPool;
}

/* Thread `count` nodes of `stride` bytes starting at `first` into a NULL-terminated free chain. */
static void linkNodes(ZmemFreeNode *first, uint32_t stride, uint32_t count)
{
    ZmemFreeNode *node = first;

    while (--count != 0) {
        node->next = (ZmemFreeNode *)((uint8_t *)node + stride);
        node = node->next;
    }
    node->next = NULL;
}

/* VA 0x00485e80, 291 bytes, [V] direct, status draft */
ZmemFreeList *zmemFreeListCreate(uint32_t elemSize, uint32_t count, ZmemFreeListCallback destroyCb)
{
    ZmemFreeList *list;
    void **chunk;
    int i;

    for (i = 0; i < ZMEM_MAX_FREELISTS; i++)
        if (!g_zmemFreeListUsed[i])
            break;
    if (i == ZMEM_MAX_FREELISTS) {
        zAssertFail("Free list pool has been exhausted.", ZMEM_SRC, 0x4dd, 0);
        return NULL;
    }
    g_zmemFreeListUsed[i] = 1;
    list = &g_zmemFreeLists[i];
    if (list == NULL)                          /* [V] dead check, kept by the compiler */
        return NULL;

    if (count < ZMEM_FREELIST_MIN)
        count = ZMEM_FREELIST_MIN;
    list->elemSize = ROUND4(elemSize);
    list->destroyCb = destroyCb;
    list->growCount = count;
    /* one chunk: a link word, then `count` nodes of (8 + elemSize) bytes */
    chunk = (void **)zmemAlloc(4 + count * (list->elemSize + sizeof(ZmemFreeNode)));   /* inlined */
    /* [V] no NULL check: an allocation failure (already asserted) crashes in linkNodes */
    linkNodes((ZmemFreeNode *)(chunk + 1), list->elemSize + sizeof(ZmemFreeNode), count);
    list->chunks = chunk;
    chunk[0] = NULL;
    list->freeCount = count;
    list->freeHead = (ZmemFreeNode *)(chunk + 1);
    list->usedHead = NULL;
    list->usedCount = 0;
    return list;
}

/* VA 0x00485fb0, 156 bytes, [V] direct, status draft */
int zmemFreeListDestroy(ZmemFreeList *list)
{
    void **chunk = list->chunks;
    void **nextChunk;
    ZmemFreeNode *node = list->usedHead;
    ZmemFreeNode *next;

    if (list->destroyCb != NULL) {
        while (node != NULL) {
            next = node->next;
            list->destroyCb(node + 1);              /* element data follows the node */
            node = next;
        }
    }
    while (chunk != NULL) {
        nextChunk = (void **)chunk[0];
        zmemFree(chunk);                            /* inlined */
        chunk = nextChunk;
    }
    /* checked only after the memory has been released [V] */
    if (list < &g_zmemFreeLists[0] || list > &g_zmemFreeLists[ZMEM_MAX_FREELISTS - 1])
        zAssertFail("An invalid free list was destroyed.", ZMEM_SRC, 0x4eb, 0);
    g_zmemFreeListUsed[list - g_zmemFreeLists] = 0;
    return 1;
}

/* VA 0x00486050, 273 bytes, [V] direct, status draft */
void *zmemFreeListAlloc(ZmemFreeList *list)
{
    ZmemFreeNode *node;
    void **chunk;
    uint32_t count;

    if (list->freeHead == NULL) {
        if (list->freeCount != 0)
            zAssertFail("Free list should be empty, but free blocks still exist.", ZMEM_SRC, 0x56c, 0);
        /* grow: a new chunk twice the size of the last one */
        count = list->growCount * 2;
        list->growCount = count;
        chunk = (void **)zmemAlloc(4 + count * (list->elemSize + sizeof(ZmemFreeNode)));  /* inlined */
        linkNodes((ZmemFreeNode *)(chunk + 1), list->elemSize + sizeof(ZmemFreeNode), count);
        chunk[0] = list->chunks;
        list->freeHead = (ZmemFreeNode *)(chunk + 1);
        list->chunks = chunk;
        list->freeCount = list->growCount;
    }
    /* pop a free node and push it on the used list */
    node = list->freeHead;
    list->freeHead = node->next;
    node->next = list->usedHead;
    node->prev = NULL;
    if (list->usedHead != NULL)
        list->usedHead->prev = node;
    list->usedHead = node;
    list->usedCount++;
    list->freeCount--;
    return node + 1;
}

/* VA 0x00486170, 149 bytes, [V] direct, status draft */
void zmemFreeListFree(ZmemFreeList *list, void *elem)
{
    ZmemFreeNode *node = (ZmemFreeNode *)elem - 1;
    ZmemFreeNode *it;

    for (it = list->usedHead; it != NULL; it = it->next)
        if (it == node)
            break;
    if (it == NULL)   /* reported, but the block is released anyway [V] */
        zAssertFail("Invalid block released from free list.", ZMEM_SRC, 0x5b1, 0);

    if (node == list->usedHead) {
        if (node->prev != NULL)
            zAssertFail("Free list is corrupt: Head of used block list is not the sentinel.", ZMEM_SRC, 0x5ba, 0);
        list->usedHead = node->next;
    }
    if (node->prev != NULL)
        node->prev->next = node->next;
    if (node->next != NULL)
        node->next->prev = node->prev;
    node->next = list->freeHead;
    list->freeHead = node;
    list->usedCount--;
    list->freeCount++;
}

/* VA 0x00486210, 59 bytes, [I] row, status draft */
int zmemFreeListForEach(ZmemFreeList *list, ZmemFreeListIterFn fn, void *user)
{
    ZmemFreeNode *node = list->usedHead;
    ZmemFreeNode *next;

    if (fn == NULL || node == NULL)
        return 1;
    do {
        next = node->next;                         /* fn may free the element */
        fn(node + 1, user);
        node = next;
    } while (node != NULL);
    return 1;
}

/* VA 0x00486250, 8 bytes, [I] row, status draft */
ZmemFreeNode *zmemFreeListFirst(ZmemFreeList *list)
{
    return list->usedHead;
}

/* VA 0x00486260, 7 bytes, [I] row, status draft */
ZmemFreeNode *zmemFreeListNext(ZmemFreeNode *node)
{
    return node->next;
}

/* VA 0x00486270, 8 bytes, [I] row, status draft */
void *zmemFreeListNodeData(ZmemFreeNode *node)
{
    return node + 1;
}

/* VA 0x00486280, 8 bytes, [I] row, status draft */
ZmemFreeNode *zmemFreeListDataNode(void *elem)
{
    return (ZmemFreeNode *)elem - 1;
}

/* VA 0x00486290, 8 bytes, [I] row, status draft */
uint32_t zmemFreeListGetUsedCount(ZmemFreeList *list)
{
    return list->usedCount;
}

/* VA 0x004862a0, 8 bytes, [I] row, status draft */
uint32_t zmemFreeListGetFreeCount(ZmemFreeList *list)
{
    return list->freeCount;
}

/* VA 0x004862b0, 8 bytes, [I] row, status draft */
uint32_t zmemFreeListGetElemSize(ZmemFreeList *list)
{
    return list->elemSize;
}

/* VA 0x004862c0, 41 bytes, [I] row, status draft */
int zmemFreeListIsUsed(ZmemFreeList *list, void *elem)
{
    ZmemFreeNode *node = (ZmemFreeNode *)elem - 1;
    ZmemFreeNode *it;

    for (it = list->usedHead; it != NULL; it = it->next)
        if (it == node)
            return 1;
    return 0;
}

/* VA 0x004862f0, 1 bytes, [I] row, status draft */
void zmemFreeListDump(ZmemFreeList *list)
{
    /* [I] debug output compiled out of the release build (uncalled) */
    (void)list;
}

/* VA 0x00486300, 1 bytes, [I] row, status draft */
void zmemCheck(void)
{
    /* [I] debug consistency check compiled out (called 4 times from game code around 0x0045BB61) */
}

/* VA 0x00486310, 65 bytes, [V] direct, status draft */
void zmemPoolSystemInit(void)
{
    ZASSERT(!g_zmemPoolSystemInitialized, "Memory pool system has been previously initialized.", ZMEM_SRC, 0x814);
    g_zmemPoolDefaultFuncs.alloc = zmemPoolDefaultAlloc;
    g_zmemPoolDefaultFuncs.free = zmemFree;
    g_zmemPoolSystemInitialized = 1;
}

/* VA 0x00486360, 116 bytes, [V] direct, status draft */
void *zmemPoolDefaultAlloc(uint32_t size)
{
    /* the default backing store of a pool: a zmemAlloc block (inlined) */
    return zmemAlloc(ROUND4(size));
}

/* VA 0x004863e0, 45 bytes, [I] row, status draft */
void zmemPoolSystemShutdown(void)
{
    ZASSERT(g_zmemPoolSystemInitialized, "Memory pool system has been previously shutdown.", ZMEM_SRC, 0x81c);
    g_zmemPoolSystemInitialized = 0;
}

/* Release a pool slot (inlined in zmemPoolCreate and zmemPoolDestroy). */
static void releaseSlot(ZmemPool *pool)
{
    if (pool < &g_zmemPools[0] || pool >= &g_zmemPools[ZMEM_MAX_POOLS])
        zAssertFail("An invalid memory pool was destroyed.", ZMEM_SRC, 0x809, 0);
    g_zmemPools[pool - g_zmemPools].inUse = 0;
}

/* VA 0x00486410, 232 bytes, [I] row, status draft */
ZmemPool *zmemPoolCreate(uint32_t size, const ZmemPoolFuncs *funcs)
{
    ZmemPool *pool = zmemPoolGetSlot();
    ZmemBlock *first, *sentinel;

    if (pool == NULL)
        return NULL;
    if (funcs != NULL) {
        pool->alloc = funcs->alloc;
        pool->free = funcs->free;
    } else {
        pool->alloc = g_zmemPoolDefaultFuncs.alloc;
        pool->free = g_zmemPoolDefaultFuncs.free;
    }
    size = size < ZMEM_POOL_MIN ? ZMEM_POOL_MIN : ROUND4(size);
    first = (ZmemBlock *)pool->alloc(size);
    if (first == NULL) {
        releaseSlot(pool);
        return NULL;
    }
    pool->first = first;
    /* one free block spanning everything, then a used 8-byte sentinel at the very end */
    sentinel = (ZmemBlock *)((uint8_t *)first + size - sizeof(ZmemBlock));
    sentinel->prevFree &= ~ZMEM_BLOCK_FREE;
    first->prevFree |= ZMEM_BLOCK_FREE;
    sentinel->next = NULL;
    first->next = sentinel;
    first->prevFree &= ZMEM_BLOCK_FREE;           /* prev = NULL, keep the free bit */
    BLOCK_SET_PREV(sentinel, first);
    pool->sentinel = sentinel;
    return pool;
}

/* VA 0x00486500, 74 bytes, [I] row, status draft */
ZmemPool *zmemPoolGetSlot(void)
{
    int i;

    for (i = 0; i < ZMEM_MAX_POOLS; i++) {
        if (!g_zmemPools[i].inUse) {
            g_zmemPools[i].inUse = 1;
            return &g_zmemPools[i];
        }
    }
    zAssertFail("Memory pool chache has been been exhausted.", ZMEM_SRC, 0x802, 0);
    return NULL;
}

/* VA 0x00486550, 94 bytes, [I] row, status draft */
void zmemPoolDestroy(ZmemPool *pool)
{
    pool->free(pool->first);
    releaseSlot(pool);
}

/* VA 0x004865b0, 146 bytes, [I] row, status draft */
void *zmemPoolAlloc(ZmemPool *pool, uint32_t size)
{
    ZmemBlock *blk, *best = NULL, *rest;
    uint32_t bestSize = 0xFFFFFFFFu, avail, need, payload;

    payload = size < ZMEM_POOL_MIN ? ZMEM_POOL_MIN : ROUND4(size);
    /* [V] a block must have room for the payload *plus* another header, so an exact fit of the
     * payload alone is never taken */
    need = payload + sizeof(ZmemBlock);
    for (blk = pool->first; blk != NULL; blk = blk->next) {
        if (!BLOCK_IS_FREE(blk))
            continue;
        avail = BLOCK_SIZE(blk);
        if (avail < bestSize && avail >= need) {   /* best fit, stop at an exact one */
            best = blk;
            bestSize = avail;
            if (avail == need)
                break;
        }
    }
    if (best == NULL)
        return NULL;
    if (bestSize - need >= ZMEM_POOL_SPLIT) {
        /* split: the rest becomes a new free block right after the payload */
        rest = (ZmemBlock *)((uint8_t *)best + sizeof(ZmemBlock) + payload);
        rest->next = best->next;
        BLOCK_SET_PREV(best->next, rest);
        best->next = rest;
        BLOCK_SET_PREV(rest, best);
        rest->prevFree |= ZMEM_BLOCK_FREE;
    }
    best->prevFree &= ~ZMEM_BLOCK_FREE;
    return best + 1;
}

/* Carve a free block of `payload` bytes out of the start of [blk, next) and link the rest
 * (inlined three times in zmemPoolRealloc). */
static void splitAfter(ZmemBlock *blk, uint32_t payload, ZmemBlock *next)
{
    ZmemBlock *rest = (ZmemBlock *)((uint8_t *)blk + sizeof(ZmemBlock) + payload);

    BLOCK_SET_PREV(rest, blk);
    blk->next = rest;
    BLOCK_SET_PREV(next, rest);
    rest->next = next;
    rest->prevFree |= ZMEM_BLOCK_FREE;
}

/* VA 0x00486650, 449 bytes, [I] row, status draft */
void *zmemPoolRealloc(ZmemPool *pool, void *p, uint32_t size)
{
    ZmemBlock *blk, *next, *prev, *moved;
    uint32_t payload, avail, total;
    void *q;

    if (p == NULL)
        return NULL;                               /* [V] not an allocation */
    if ((uint8_t *)p < (uint8_t *)pool->first + sizeof(ZmemBlock) || (uint8_t *)p >= (uint8_t *)pool->sentinel)
        zAssertFail("Memory being released is not from this pool.", ZMEM_SRC, 0x8d1, 0);

    payload = size < ZMEM_POOL_MIN ? ZMEM_POOL_MIN : ROUND4(size);
    blk = (ZmemBlock *)p - 1;
    next = blk->next;
    avail = BLOCK_SIZE(blk);

    if (payload <= avail) {
        /* shrink in place; give the tail back if it is big enough, merged with a free successor */
        if (avail - payload >= ZMEM_POOL_SPLIT)
            splitAfter(blk, payload, BLOCK_IS_FREE(next) ? next->next : next);
        return p;
    }
    if (BLOCK_IS_FREE(next)) {
        /* grow in place into the following free block */
        total = (uint32_t)((uint8_t *)next->next - (uint8_t *)next) + avail;
        if (total >= payload) {
            if (total - payload >= ZMEM_POOL_SPLIT) {
                splitAfter(blk, payload, next->next);
            } else {
                blk->next = next->next;
                BLOCK_SET_PREV(next->next, blk);
            }
            return p;
        }
    }
    prev = BLOCK_PREV(blk);
    if (prev != NULL && BLOCK_IS_FREE(prev)) {
        /* move down into the preceding free block (plus this one) */
        total = BLOCK_SIZE(prev) + avail;
        if (total >= payload) {
            if (total - payload >= ZMEM_POOL_MIN) {    /* [V] 0x20 here, 0x28 elsewhere */
                /* the data goes to the end; prev keeps the leading part, still free */
                moved = (ZmemBlock *)((uint8_t *)prev + sizeof(ZmemBlock) + (total - payload));
                BLOCK_SET_PREV(moved, prev);
                prev->next = moved;
                BLOCK_SET_PREV(next, moved);
                moved->next = next;
                moved->prevFree &= ~ZMEM_BLOCK_FREE;
            } else {
                prev->next = next;
                BLOCK_SET_PREV(next, prev);
                prev->prevFree &= ~ZMEM_BLOCK_FREE;
                moved = prev;
            }
            /* [V] copies `size` (the new size) bytes, i.e. reads past the old block when growing */
            zstringMemMove(moved + 1, p, size);
            return moved + 1;
        }
    }
    /* fall back to alloc + copy + free */
    q = zmemPoolAlloc(pool, size);
    if (q == NULL)
        return NULL;
    zstringMemCopy(q, p, size);                    /* [V] same over-read */
    zmemPoolFree(pool, p);
    return q;
}

/* VA 0x00486820, 153 bytes, [V] direct, status draft */
void zmemPoolFree(ZmemPool *pool, void *p)
{
    ZmemBlock *blk, *next, *prev;

    if (p == NULL)
        return;
    if ((uint8_t *)p < (uint8_t *)pool->first + sizeof(ZmemBlock) || (uint8_t *)p >= (uint8_t *)pool->sentinel)
        zAssertFail("Memory being released is not from this pool.", ZMEM_SRC, 0x951, 0);
    blk = (ZmemBlock *)p - 1;
    if (BLOCK_IS_FREE(blk))
        zAssertFail("zmemPoolFree(): Memory has been previously freed.", ZMEM_SRC, 0x955, 0);
    blk->prevFree |= ZMEM_BLOCK_FREE;
    prev = BLOCK_PREV(blk);
    next = blk->next;
    if (BLOCK_IS_FREE(next)) {                     /* merge with the next block */
        next = next->next;
        blk->next = next;
        BLOCK_SET_PREV(next, blk);
    }
    if (prev != NULL && BLOCK_IS_FREE(prev)) {     /* merge into the previous block */
        prev->next = next;
        BLOCK_SET_PREV(next, prev);
    }
}

/* VA 0x004868c0, 68 bytes, [I] row, status draft */
void zmemPoolWalk(ZmemPool *pool, ZmemPoolWalkFn fn, void *user)
{
    ZmemBlock *blk = pool->first;
    ZmemBlock *next;

    if (fn == NULL)
        return;
    while (blk != pool->sentinel) {
        next = blk->next;
        fn(blk + 1, BLOCK_SIZE(blk), BLOCK_IS_FREE(blk), user);
        blk = next;
    }
}

/* 0x00486910 zpropListsInit, 0x00486930 zpropShutdown and 0x00486940 zpropListsFree (DM-044 drafted them
 * here) moved to ZgeMods/zprop.c in the DM-043 integration. */

/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIfzmemDMM.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIfzmemDMM.c
 * Evidence: [V] path string 0x0055D674 referenced by 8 of the 10 functions; the other two by link
 *           order and the shared globals 0x005F8F30-0x005F8F54.
 * 11 functions, VA 0x0046aca0-0x0046b793 (DM-057 created zmemDMMStats 0x0046B730). The first ten
 * are reached through incremental-link thunks
 * (0x00401037 ...), i.e. called from game code.
 * Decompiled by DM-044 (non-matching C, AD-8). Types: decomp/include/zmem.h (ZmemDMM*).
 *
 * "DMM" (dynamic memory manager, the messages say "ZMEMDMM"): a handle-based heap whose blocks can
 * be moved by zmemDMMOptimize (compaction), so users hold handles and lock them to get a pointer.
 * It lives in one zmemTempAlloc block: data grows up from the base, the handle table grows down
 * from the top (handle n at top - n*16), and `limit` is the boundary. Free blocks form an
 * address-ordered doubly linked list whose header (prev, next, size) is stored in the free space.
 * The return convention of zmemDMMInit is inverted (0 = success).
 */
#include "decomp.h"

#define DMM_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIfzmemDMM.c"

ZmemDMMGlobals g_zmemDMM;   /* 0x005F8F30 */

#define HANDLE(h) ((ZmemDMMHandle *)(g_zmemDMM.top - (h) * sizeof(ZmemDMMHandle)))

int zmemDMMAllocBlock(ZmemDMMFree *blk, uint32_t size);
int zmemDMMFindHandle(void *ptr);
int zmemDMMNewHandle(void);

static ZmemDMMFree *lastFree(ZmemDMMFree *f)
{
    while (f->next != NULL)
        f = f->next;
    return f;
}

/* VA 0x0046aca0, 252 bytes, [V] direct, status draft */
int zmemDMMInit(void *unused, uint32_t size)
{
    ZmemDMMFree *f;

    (void)unused;
    if (g_zmemDMM.initialized) {
        g_pfnDebugPrintf("ZMEMDMM: Error - DMM already initialized\n");
        zAssertFail("ZMEMDMM: Error - DMM already initialized\n", DMM_SRC, 0xc5, 0);
        return 1;
    }
    g_zmemDMM.tempMark = zmemTempMark();
    g_zmemDMM.size = (size + 3) & ~3u;
    g_zmemDMM.base = (uint8_t *)zmemTempAlloc(g_zmemDMM.size);
    if (g_zmemDMM.base == NULL) {
        g_pfnDebugPrintf("ZMEMDMM: Failed to initialize DMM\n");
        zAssertFail("ZMEMDMM: Failed to initialize DMM\n", DMM_SRC, 0xd4, 0);
        return 1;
    }
    g_zmemDMM.highWater = g_zmemDMM.base;
    g_zmemDMM.top = g_zmemDMM.base + g_zmemDMM.size;
    g_zmemDMM.limit = g_zmemDMM.top;            /* no handle slots yet */
    g_zmemDMM.lockCount = 0;
    g_zmemDMM.justGrew = 0;
    zstringMemSet(g_zmemDMM.base, 0, g_zmemDMM.size);
    /* one free block; its size leaves 16 bytes below `top` that the first zmemDMMNewHandle can
     * take for handle 1 */
    f = (ZmemDMMFree *)g_zmemDMM.base;
    g_zmemDMM.freeList = f;
    f->prev = NULL;
    f->next = NULL;
    f->size = g_zmemDMM.size - sizeof(ZmemDMMHandle);
    g_zmemDMM.initialized = 1;
    return 0;
}

/* VA 0x0046ade0, 71 bytes, [V] direct, status draft */
void zmemDMMShutdown(void)
{
    if (!g_zmemDMM.initialized) {
        g_pfnDebugPrintf("ZMEMDMM: DMM Shutdown without being initialized\n");
        zAssertFail("ZMEMDMM: DMM Shutdown without being initialized\n", DMM_SRC, 0x10c, 0);
        return;
    }
    zmemTempRelease(g_zmemDMM.tempMark);
    g_zmemDMM.initialized = 0;
}

/* VA 0x0046ae50, 380 bytes, [V] direct, status draft */
int zmemDMMAllocBlock(ZmemDMMFree *blk, uint32_t size)
{
    /* Use the first `size` bytes of free block `blk`; returns the new handle or 0. */
    int h = zmemDMMNewHandle();
    ZmemDMMHandle *e;
    ZmemDMMFree *prev, *next, *rest;
    uint8_t *end;
    uint32_t blkSize;

    if (h == 0) {
        zAssertFail("ZMEM: Out of DMM handles", DMM_SRC, 0x139, 0);
        return 0;
    }
    e = HANDLE(h);
    end = (uint8_t *)blk + size;
    if (end + 0x10 > g_zmemDMM.limit) {
        /* does not fit below the (possibly just lowered) handle table: give the slot back */
        if (g_zmemDMM.justGrew == 1) {
            if (g_zmemDMM.freeList == ZMEM_DMM_NONE)
                return 0;
            lastFree(g_zmemDMM.freeList)->size += sizeof(ZmemDMMHandle);
            g_zmemDMM.limit += sizeof(ZmemDMMHandle);
            g_zmemDMM.justGrew = 0;
        }
        return 0;
    }
    g_zmemDMM.justGrew = 0;
    e->used = 1;
    e->lockCount = 0;
    e->unk02 = 0;
    e->ptr = blk;
    e->size = size;
    if (end > g_zmemDMM.highWater)
        g_zmemDMM.highWater = end;

    prev = blk->prev;
    next = blk->next;
    /* re-read after zmemDMMNewHandle: if the new slot was taken from this very block (it was the
     * last free one), blkSize can now be smaller than `size`, and the whole-block branch below
     * then records a block smaller than requested [V via the port, tests/test_decomp_dm044.py] */
    blkSize = blk->size;
    if (g_zmemDMM.highWater == g_zmemDMM.limit) {
        /* [V] the data reached the handle table: the whole free list is dropped, including any
         * holes further down (they stay lost until the next zmemDMMDealloc rebuilds the list) */
        g_zmemDMM.freeList = ZMEM_DMM_NONE;
    } else if ((uint8_t *)blk + size + sizeof(ZmemDMMFree) > (uint8_t *)blk + blkSize) {
        /* the rest cannot hold a free header: hand out the whole block and unlink it */
        e->size = blkSize;
        if (prev == NULL) {
            if (next == NULL) {
                g_zmemDMM.freeList = ZMEM_DMM_NONE;
            } else {
                g_zmemDMM.freeList = next;
                next->prev = NULL;
            }
        } else {
            prev->next = next;
            if (next != NULL)
                next->prev = prev;
        }
    } else {
        /* split: the free header moves up behind the allocation */
        rest = (ZmemDMMFree *)((uint8_t *)e->ptr + size);
        rest->prev = prev;
        rest->next = next;
        rest->size = blkSize - size;
        if (prev != NULL)
            prev->next = rest;
        if (rest->next != NULL)
            rest->next->prev = rest;
        if (rest->prev == NULL)
            g_zmemDMM.freeList = rest;
    }
    zstringMemSet(e->ptr, 0, e->size);
    return h;
}

/* VA 0x0046b030, 196 bytes, [V] direct, status draft */
int zmemDMMAlloc(uint32_t size)
{
    ZmemDMMFree *f;

    if (!g_zmemDMM.initialized) {
        zAssertFail("ZMEMDMM: DMM used without being initialized\n", DMM_SRC, 0x1ea, 0);
        return 0;
    }
    if (size == 0) {
        g_pfnDebugPrintf("ZMEMDMM: DMM allocation size = 0\n");
        zAssertFail("ZMEMDMM: DMM allocation size = 0\n", DMM_SRC, 0x1f1, 0);
        return 0;
    }
    if (g_zmemDMM.freeList == ZMEM_DMM_NONE) {
        zAssertFail("ZMEM: Out of RAM", DMM_SRC, 0x204, 0);
        return 0;
    }
    if (size < sizeof(ZmemDMMFree))
        size = sizeof(ZmemDMMFree);              /* a freed block must hold the free header */
    size = (size + 3) & ~3u;
    for (f = g_zmemDMM.freeList; f != NULL; f = f->next)   /* first fit */
        if (size <= f->size)
            return zmemDMMAllocBlock(f, size);
    zAssertFail("ZMEMDMM: DMM failed to allocate", DMM_SRC, 0x245, 0);
    return 0;
}

/* VA 0x0046b140, 405 bytes, [V] direct, status draft */
void zmemDMMDealloc(int handle)
{
    ZmemDMMHandle *e = HANDLE(handle);
    ZmemDMMFree *blk, *it, *next, *prev;

    if (handle == 0 || (uint8_t *)e < g_zmemDMM.limit || !e->used) {
        g_pfnDebugPrintf("ZMEMDMM: invalid handle passed to zmemDMMDealloc\n");
        return;
    }
    if (e->lockCount != 0)
        zAssertFail("Locked", DMM_SRC, 0x277, 0);   /* freed anyway */
    blk = (ZmemDMMFree *)e->ptr;
    e->used = 0;
    zstringMemSet(blk, 0, e->size);
    blk->size = e->size;

    /* insert into the address-ordered free list (ZMEM_DMM_NONE compares above every address) */
    if (blk < g_zmemDMM.freeList) {
        blk->prev = NULL;
        blk->next = g_zmemDMM.freeList == ZMEM_DMM_NONE ? NULL : g_zmemDMM.freeList;
        if (blk->next != NULL)
            blk->next->prev = blk;
        g_zmemDMM.freeList = blk;
    } else {
        it = g_zmemDMM.freeList;
        if (blk > it)
            while (it->next != NULL && blk > (it = it->next))
                ;
        if (it->next == NULL && blk > it) {
            it->next = blk;                       /* new last block */
            blk->prev = it;
            blk->next = NULL;
        } else {
            /* insert before `it`. [V] blk == head (a double free) reaches this with it->prev == NULL
             * and crashes */
            blk->next = it;
            blk->prev = it->prev;
            it->prev->next = blk;
            it->prev = blk;
        }
    }
    /* coalesce with the following free block */
    next = blk->next;
    if (next != NULL && (uint8_t *)blk + blk->size == (uint8_t *)next) {
        blk->size += next->size;
        if (next->next != NULL) {
            next->next->prev = blk;
            blk->next = next->next;
        } else {
            blk->next = NULL;
        }
        zstringMemSet(next, 0, sizeof(ZmemDMMFree));
    }
    /* coalesce with the preceding free block */
    prev = blk->prev;
    if (prev != NULL && (uint8_t *)prev + prev->size == (uint8_t *)blk) {
        prev->size += blk->size;
        prev->next = blk->next;
        if (blk->next != NULL)
            blk->next->prev = prev;
        zstringMemSet(blk, 0, sizeof(ZmemDMMFree));
    }
    /* [V] highWater becomes the address of the last free block, not the end of the data */
    g_zmemDMM.highWater = (uint8_t *)lastFree(g_zmemDMM.freeList);
}

/* VA 0x0046b350, 53 bytes, [I] row, status draft */
int zmemDMMFindHandle(void *ptr)
{
    ZmemDMMHandle *e;
    int h = 0;

    for (;;) {
        h++;
        e = HANDLE(h);
        if (e->used && e->ptr == ptr)
            return h;
        if ((uint8_t *)e <= g_zmemDMM.limit)
            return 0;
    }
}

/* VA 0x0046b3a0, 154 bytes, [I] row, status draft */
int zmemDMMNewHandle(void)
{
    ZmemDMMFree *last;
    int h;

    g_zmemDMM.justGrew = 0;
    for (h = 1; (uint8_t *)HANDLE(h) >= g_zmemDMM.limit; h++)
        if (HANDLE(h)->used != 1)
            return h;
    /* all slots taken: lower the limit by one slot and charge the 16 bytes to the last free block,
     * whether or not that block actually ends at the limit [V] */
    if (g_zmemDMM.highWater >= g_zmemDMM.limit - sizeof(ZmemDMMHandle))
        return 0;
    if (g_zmemDMM.freeList == ZMEM_DMM_NONE)
        return 0;
    last = lastFree(g_zmemDMM.freeList);
    if (last->size <= sizeof(ZmemDMMHandle))
        return 0;
    last->size -= sizeof(ZmemDMMHandle);
    g_zmemDMM.justGrew = 1;
    g_zmemDMM.limit -= sizeof(ZmemDMMHandle);
    return h;
}

/* VA 0x0046b470, 85 bytes, [V] direct, status draft */
void *zmemDMMLock(int handle)
{
    ZmemDMMHandle *e = HANDLE(handle);

    if (handle == 0) {
        zAssertFail("ZMEMDMM: Trying to lock handle 0\n", DMM_SRC, 0x370, 0);
        return NULL;
    }
    if ((uint8_t *)e >= g_zmemDMM.limit && e->used) {
        g_zmemDMM.lockCount++;
        e->lockCount++;
        return e->ptr;
    }
    return NULL;
}

/* VA 0x0046b4f0, 87 bytes, [V] direct, status draft */
void zmemDMMUnlock(int handle)
{
    ZmemDMMHandle *e = HANDLE(handle);

    if (handle == 0) {
        zAssertFail("ZMEMDMM: Trying to unlock handle 0\n", DMM_SRC, 0x3a5, 0);
        return;
    }
    if ((uint8_t *)e >= g_zmemDMM.limit && e->used && e->lockCount != 0) {
        g_zmemDMM.lockCount--;
        e->lockCount--;
    }
}

/* VA 0x0046b570, 337 bytes, [V] direct, status draft */
int zmemDMMOptimize(void)
{
    /* Compaction: repeatedly move the used block that follows the first free block down into it,
     * so the free space bubbles up; returns 1 while blocks are locked (nothing done) or on error. */
    ZmemDMMFree *f = g_zmemDMM.freeList, *next, *nf;
    ZmemDMMHandle *e;
    uint8_t *src;
    uint32_t freeSize, i;
    int h;

    if (g_zmemDMM.lockCount > 0)
        return 1;
    if (f == ZMEM_DMM_NONE || (f->prev == NULL && f->next == NULL))
        return 0;
    while (f->next != NULL) {
        freeSize = f->size;
        next = f->next;
        src = (uint8_t *)f + freeSize;            /* the used block right after the free one */
        h = zmemDMMFindHandle(src);
        if (h == 0) {
            g_pfnDebugPrintf("ZMEMDMM: DMM optimize found unrecognized pointer, memory corruption likely\n");
            zAssertFail("ZMEMDMM: DMM optimize found unrecognized pointer, memory corruption likely\n",
                        DMM_SRC, 0x404, 0);
            return 1;
        }
        e = HANDLE(h);
        if ((uint8_t *)f + e->size > src) {
            for (i = 0; i < e->size / 4; i++)       /* overlapping: forward dword copy */
                ((uint32_t *)f)[i] = ((uint32_t *)src)[i];
        } else {
            zstringMemCopy(f, src, e->size);
        }
        e->ptr = f;
        /* the free block now starts after the moved data; merge it with the next one if adjacent */
        nf = (ZmemDMMFree *)((uint8_t *)f + e->size);
        nf->prev = NULL;
        nf->next = next;
        nf->size = freeSize;
        if (next != NULL) {
            next->prev = nf;
            if ((uint8_t *)nf + nf->size == (uint8_t *)nf->next) {
                nf->size += next->size;
                if (next->next != NULL) {
                    next->next->prev = nf;
                    nf->next = nf->next->next;
                } else {
                    nf->next = NULL;
                }
            }
        }
        g_zmemDMM.freeList = nf;
        f = nf;
    }
    g_zmemDMM.highWater = (uint8_t *)lastFree(f);
    return 0;
}

/* VA 0x0046b730, 99 bytes, [I] between, status draft */
void zmemDMMStats(void)
{
    /* DM-057: an unreferenced debug helper (ZMEM_DMM_NONE = -1): it measures the largest free
     * block and the number of handle slots and drops both (the print was compiled out) */
    uint32_t largest = 0;
    int handles = 0;
    ZmemDMMFree *f;

    if (g_zmemDMM.freeList != (ZmemDMMFree *)-1)
        for (f = g_zmemDMM.freeList; f != NULL; f = f->next)
            if (f->size > largest)
                largest = f->size;
    if (g_zmemDMM.limit != g_zmemDMM.top)
        do
            handles++;
        while (g_zmemDMM.limit < g_zmemDMM.top - handles * 0x10);
    (void)largest;
    (void)handles;
}

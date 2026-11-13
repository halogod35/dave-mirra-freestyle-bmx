/*
 * zmem.h - NgEngine memory manager (NgEngine/Zge/zmem.c) and the game's movable-block manager
 * (Bmx/src/com/zge/NgIF/NgIfzmemDMM.c) (DM-044).
 *
 * zmem.c has four allocators [V offsets, I names; the terms follow the exe's own messages]:
 *  1. zmemAlloc/zmemFree: a 4-byte header { size:30, kind:2 } in front of every block; kind 2 =
 *     "dynamic" (platform malloc via g_zmemPlatformAlloc, or a redirect pool), kind 0 = "static".
 *  2. the static (temp) arena: one big zmemAlloc block, used as a stack (zmemTempAlloc/Mark/Release).
 *  3. "free lists" (fixed-size element allocators, 128 slots): "Free list pool has been exhausted."
 *     DM-041/042 called these zmemPool*; renamed by DM-044 to zmemFreeList*.
 *  4. "memory pools" (first-fit heaps with block headers, 128 slots): "zmemPoolFree(): Memory has
 *     been previously freed." DM-041 called these zmemHeap*; renamed by DM-044 to zmemPool*.
 * NgIfzmemDMM.c ("DMM") is a handle-based, compactable heap on top of a temp block.
 */
#ifndef ZMEM_H
#define ZMEM_H
#include "dmtypes.h"

/* ---- 1. zmemAlloc block header (the pointer returned is header + 4) ---- */
#define ZMEM_SIZE_MASK   0x3FFFFFFFu
#define ZMEM_KIND_SHIFT  30
#define ZMEM_KIND_STATIC 0u      /* from the static arena (zmemTempAlloc) */
#define ZMEM_KIND_DYNAMIC 2u     /* platform heap or redirect pool (zmemAlloc) */

typedef void *(__cdecl *ZmemAllocFn)(uint32_t size);
typedef void  (__cdecl *ZmemFreeFn)(void *p);
typedef void *(__cdecl *ZmemReallocFn)(void *p, uint32_t size);

/* ---- 4. memory pool (first-fit heap). Blocks are a doubly linked list in address order. ---- */
typedef struct ZmemBlock {
    struct ZmemBlock *next;      /* +0x00 next block (the sentinel's is NULL); size = next - this - 8 */
    uint32_t          prevFree;  /* +0x04 previous block pointer | bit 0 = block is free */
} ZmemBlock;                     /* 0x08, followed by the payload */

#define ZMEM_BLOCK_FREE   1u
#define ZMEM_POOL_MIN     0x20u  /* smallest payload / pool size */
#define ZMEM_POOL_SPLIT   0x28u  /* a free block is split only if >= this much is left over */

typedef struct ZmemPoolFuncs {
    ZmemAllocFn alloc;           /* +0x00 gets the pool's backing memory */
    ZmemFreeFn  free;            /* +0x04 releases it */
} ZmemPoolFuncs;                 /* 0x08; default {zmemPoolDefaultAlloc, zmemFree} at 0x006718A8 */

typedef struct ZmemPool {
    ZmemBlock  *first;           /* +0x00 first block = the backing memory */
    ZmemBlock  *sentinel;        /* +0x04 last 8 bytes of the backing memory, never free */
    ZmemAllocFn alloc;           /* +0x08 */
    ZmemFreeFn  free;            /* +0x0C */
    int         inUse;           /* +0x10 slot taken */
} ZmemPool;                      /* 0x14; g_zmemPools[128] at 0x006718B0 */

#define ZMEM_MAX_POOLS 128

typedef void (__cdecl *ZmemPoolWalkFn)(void *data, uint32_t size, int isFree, void *user);

/* ---- 3. free list (fixed-size elements). Elements carry an 8-byte node header. ---- */
typedef struct ZmemFreeNode {
    struct ZmemFreeNode *next;   /* +0x00 next node (free list or used list) */
    struct ZmemFreeNode *prev;   /* +0x04 previous node (used list only) */
} ZmemFreeNode;                  /* 0x08, followed by elemSize bytes of element data */

typedef void (__cdecl *ZmemFreeListCallback)(void *elem);
typedef void (__cdecl *ZmemFreeListIterFn)(void *elem, void *user);

typedef struct ZmemFreeList {
    ZmemFreeNode *freeHead;      /* +0x00 free nodes (singly linked) */
    ZmemFreeNode *usedHead;      /* +0x04 used nodes (doubly linked, newest first) */
    void        **chunks;        /* +0x08 allocated chunks; word 0 of each links to the previous */
    uint32_t      elemSize;      /* +0x0C rounded up to 4 */
    uint32_t      growCount;     /* +0x10 nodes in the last chunk (>= 32, doubles on each grow) */
    uint32_t      usedCount;     /* +0x14 */
    uint32_t      freeCount;     /* +0x18 */
    ZmemFreeListCallback destroyCb; /* +0x1C called for every used element by zmemFreeListDestroy */
} ZmemFreeList;                  /* 0x20; g_zmemFreeLists[128] at 0x006708A8, in-use flags at 0x00670680 */

#define ZMEM_MAX_FREELISTS 128
#define ZMEM_FREELIST_MIN  0x20u

/* ---- module globals 0x00670880 (cleared by zmemInit) ---- */
typedef struct ZmemGlobals {
    int        initialized;      /* +0x00 0x00670880 */
    uint8_t   *staticBase;       /* +0x04 0x00670884 static arena (a zmemAlloc block) */
    ZmemPool  *redirectPool;     /* +0x08 0x00670888 zmemAlloc/zmemFree use this pool when set */
    uint8_t   *staticTop;        /* +0x0C 0x0067088C next free byte (the temp "mark") */
    uint32_t   unk10;            /* +0x10 0x00670890 never referenced */
    uint8_t   *lastAlloc;        /* +0x14 0x00670894 header of the last temp block */
    uint8_t   *staticEnd;        /* +0x18 0x00670898 */
    uint32_t   unk1C;            /* +0x1C */
    uint32_t   unk20;            /* +0x20 */
} ZmemGlobals;                   /* 0x24 */

/* ---- NgIfzmemDMM.c: handles grow down from the top of the DMM block, data grows up ---- */
typedef struct ZmemDMMHandle {
    uint8_t   used;              /* +0x00 */
    uint8_t   lockCount;         /* +0x01 */
    uint8_t   unk02;             /* +0x02 set to 0 */
    uint8_t   pad03;
    void     *ptr;               /* +0x04 current address of the block (moves on zmemDMMOptimize) */
    uint32_t  size;              /* +0x08 */
    uint32_t  pad0C;
} ZmemDMMHandle;                 /* 0x10; handle n lives at g_zmemDMM.top - n * 0x10 (n >= 1) */

typedef struct ZmemDMMFree {
    struct ZmemDMMFree *prev;    /* +0x00 */
    struct ZmemDMMFree *next;    /* +0x04 */
    uint32_t            size;    /* +0x08 bytes, including this header */
} ZmemDMMFree;                   /* 0x0C, free blocks in address order */

#define ZMEM_DMM_NONE ((ZmemDMMFree *)-1)   /* free list exhausted */

typedef struct ZmemDMMGlobals {
    uint8_t     *limit;          /* +0x00 0x005F8F30 lowest handle slot = end of the data area */
    int          justGrew;       /* +0x04 0x005F8F34 zmemDMMNewHandle took 0x10 bytes from the last free block */
    ZmemDMMFree *freeList;       /* +0x08 0x005F8F38 first free block, or ZMEM_DMM_NONE */
    int          tempMark;       /* +0x0C 0x005F8F3C zmemTempMark() before the DMM block */
    uint32_t     size;           /* +0x10 0x005F8F40 */
    uint8_t     *highWater;      /* +0x14 0x005F8F44 end of the highest used block (or last free block) */
    int          lockCount;      /* +0x18 0x005F8F48 locked handles; zmemDMMOptimize does nothing while > 0 */
    uint8_t     *top;            /* +0x1C 0x005F8F4C end of the DMM block (handle 0 would be here) */
    uint8_t     *base;           /* +0x20 0x005F8F50 */
    int          initialized;    /* +0x24 0x005F8F54 */
} ZmemDMMGlobals;                /* 0x28 */

/* GHIDRA-SKIP-BEGIN */
typedef char zmem_h_size_check[(sizeof(ZmemBlock) == 0x08 && sizeof(ZmemPoolFuncs) == 0x08 &&
                                sizeof(ZmemPool) == 0x14 && sizeof(ZmemFreeNode) == 0x08 &&
                                sizeof(ZmemFreeList) == 0x20 && sizeof(ZmemGlobals) == 0x24 &&
                                sizeof(ZmemDMMHandle) == 0x10 && sizeof(ZmemDMMFree) == 0x0C &&
                                sizeof(ZmemDMMGlobals) == 0x28) ? 1 : -1];

/* zmem.c: general allocator */
int      zmemInit(void);                                            /* 0x004859F0 */
int      zmemShutdown(void);                                        /* 0x00485A40 */
void    *zmemAlloc(uint32_t size);                                  /* 0x00485AA0 */
void     zmemFree(void *p);                                         /* 0x00485B20 */
void    *zmemResize(void *p, uint32_t size);                        /* 0x00485B50 */
int      zmemStaticInit(uint32_t size);                             /* 0x00485C80 */
int      zmemStaticShutdown(void);                                  /* 0x00485D30 */
void    *zmemTempAlloc(uint32_t size);                              /* 0x00485D80 */
uint32_t zmemTempGetFree(void);                                     /* 0x00485DF0 */
void     zmemTempRelease(int mark);                                 /* 0x00485E10 */
int      zmemTempMark(void);                                           /* 0x00485E40 */
void     zmemSetPool(ZmemPool *pool);                               /* 0x00485E50 */
void     zmemClearPool(void);                                       /* 0x00485E60 */
ZmemPool *zmemGetPool(void);                                        /* 0x00485E70 */
/* zmem.c: free lists */
ZmemFreeList *zmemFreeListCreate(uint32_t elemSize, uint32_t count, ZmemFreeListCallback destroyCb); /* 0x00485E80 */
int      zmemFreeListDestroy(ZmemFreeList *list);                   /* 0x00485FB0 */
void    *zmemFreeListAlloc(ZmemFreeList *list);                     /* 0x00486050 */
void     zmemFreeListFree(ZmemFreeList *list, void *elem);          /* 0x00486170 */
int      zmemFreeListForEach(ZmemFreeList *list, ZmemFreeListIterFn fn, void *user); /* 0x00486210 */
ZmemFreeNode *zmemFreeListFirst(ZmemFreeList *list);                /* 0x00486250 */
ZmemFreeNode *zmemFreeListNext(ZmemFreeNode *node);                 /* 0x00486260 */
void    *zmemFreeListNodeData(ZmemFreeNode *node);                  /* 0x00486270 */
ZmemFreeNode *zmemFreeListDataNode(void *elem);                     /* 0x00486280 */
uint32_t zmemFreeListGetUsedCount(ZmemFreeList *list);              /* 0x00486290 */
uint32_t zmemFreeListGetFreeCount(ZmemFreeList *list);              /* 0x004862A0 */
uint32_t zmemFreeListGetElemSize(ZmemFreeList *list);               /* 0x004862B0 */
int      zmemFreeListIsUsed(ZmemFreeList *list, void *elem);        /* 0x004862C0 */
void     zmemFreeListDump(ZmemFreeList *list);                      /* 0x004862F0 */
void     zmemCheck(void);                                           /* 0x00486300 */
/* zmem.c: memory pools */
void     zmemPoolSystemInit(void);                                  /* 0x00486310 */
void    *zmemPoolDefaultAlloc(uint32_t size);                       /* 0x00486360 */
ZmemPool *zmemPoolGetSlot(void);                                    /* 0x00486500 */
void     zmemPoolSystemShutdown(void);                              /* 0x004863E0 */
ZmemPool *zmemPoolCreate(uint32_t size, const ZmemPoolFuncs *funcs); /* 0x00486410 */
void     zmemPoolDestroy(ZmemPool *pool);                           /* 0x00486550 */
void    *zmemPoolAlloc(ZmemPool *pool, uint32_t size);              /* 0x004865B0 */
void    *zmemPoolRealloc(ZmemPool *pool, void *p, uint32_t size);   /* 0x00486650 */
void     zmemPoolFree(ZmemPool *pool, void *p);                     /* 0x00486820 */
void     zmemPoolWalk(ZmemPool *pool, ZmemPoolWalkFn fn, void *user); /* 0x004868C0 */

/* NgIfzmemDMM.c */
int      zmemDMMInit(void *unused, uint32_t size);                  /* 0x0046ACA0 */
void     zmemDMMShutdown(void);                                     /* 0x0046ADE0 */
int      zmemDMMAlloc(uint32_t size);                               /* 0x0046B030, returns a handle */
void     zmemDMMDealloc(int handle);                                /* 0x0046B140 */
void    *zmemDMMLock(int handle);                                   /* 0x0046B470 */
void     zmemDMMUnlock(int handle);                                 /* 0x0046B4F0 */
int      zmemDMMOptimize(void);                                     /* 0x0046B570 */
/* GHIDRA-SKIP-END */

#endif /* ZMEM_H */

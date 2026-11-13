/*
 * zlist.h - NgEngine list containers (NgEngine/Zge/zList.c) (DM-044).
 *
 * Two kinds [V offsets, I names]:
 *  - zList: an intrusive doubly linked list. Nodes start with a zListNode header (next, prev);
 *    used for the zprop lists 0x006722B8/0x006722C4 and others.
 *  - zHeapList: a growable array of non-NULL pointers with a built-in iteration cursor
 *    ("Cannot insert NULL values into a heap list."). Removal swaps the last item into the hole,
 *    so the order is not stable.
 */
#ifndef ZLIST_H
#define ZLIST_H
#include "dmtypes.h"

typedef struct zListNode {
    struct zListNode *next;   /* +0x00 */
    struct zListNode *prev;   /* +0x04 */
} zListNode;                  /* 0x08 */

typedef struct zList {
    zListNode *head;          /* +0x00 */
    zListNode *tail;          /* +0x04 */
    int32_t    count;         /* +0x08 */
} zList;                      /* 0x0C */

typedef struct zHeapList {
    void   **items;           /* +0x00 zmemAlloc(capacity * 4) */
    int32_t  count;           /* +0x04 */
    int32_t  capacity;        /* +0x08 doubles when full */
    int32_t  cursor;          /* +0x0C index of the current item, -1 = no iteration */
} zHeapList;                  /* 0x10 */

typedef int (__cdecl *zHeapListFn)(void *item, void *user);   /* return 0 to stop */

/* GHIDRA-SKIP-BEGIN */
typedef char zlist_h_size_check[(sizeof(zListNode) == 0x08 && sizeof(zList) == 0x0C &&
                                 sizeof(zHeapList) == 0x10) ? 1 : -1];

void       zListInit(zList *list);                                   /* 0x0048C110 */
void       zListAddHead(zList *list, zListNode *node);               /* 0x0048C120 */
void       zListAddTail(zList *list, zListNode *node);               /* 0x0048C150 */
void       zListInsertAfter(zList *list, zListNode *node, zListNode *after); /* 0x0048C180 */
void       zListRemove(zList *list, zListNode *node);                /* 0x0048C1F0 */
int        zListContains(zList *list, zListNode *node);              /* 0x0048C230 */

zHeapList *zHeapListCreate(int32_t capacity);                        /* 0x0048C260 */
void       zHeapListDestroy(zHeapList *list);                        /* 0x0048C2B0 */
void       zHeapListClear(zHeapList *list, zHeapListFn fn, void *user); /* 0x0048C2D0 */
void      *zHeapListFirst(zHeapList *list);                          /* 0x0048C330 */
void      *zHeapListNext(zHeapList *list);                           /* 0x0048C360 */
void      *zHeapListCurrent(zHeapList *list);                        /* 0x0048C390 */
void      *zHeapListGet(zHeapList *list, int32_t index);             /* 0x0048C3B0 */
int32_t    zHeapListFind(zHeapList *list, void *item);               /* 0x0048C3D0 */
int32_t    zHeapListCount(zHeapList *list);                          /* 0x0048C400 */
int32_t    zHeapListCapacity(zHeapList *list);                       /* 0x0048C410 */
void       zHeapListForEach(zHeapList *list, zHeapListFn fn, void *user); /* 0x0048C420 */
void       zHeapListAdd(zHeapList *list, void *item);                /* 0x0048C460 */
void      *zHeapListRemoveAt(zHeapList *list, int32_t index);        /* 0x0048C510 */
void      *zHeapListRemoveCurrent(zHeapList *list);                  /* 0x0048C570 */
/* GHIDRA-SKIP-END */

#endif /* ZLIST_H */

/*
 * NgEngine/Zge/zheap.c
 * Original: unknown (no path string, no literal; name invented in the original style)
 * Evidence: [I] new file (DM-061): nine functions on one Zheap layout after pcProfile.c and before
 * the C runtime (0x004A4A20 _fpmath); only ztimer.c uses them (the static pop result 0x006764E0
 * follows pcProfile.c's globals).
 * 9 functions, VA 0x004a46c0-0x004a4a1a.
 * DM-061 (draft). Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 7.
 *
 * A binary heap of (u32 key, value) pairs: items[1..count], items[0] a sentinel (0 for a min-heap,
 * 0xFFFFFFFF for a max-heap) that stops the sift-up at the root.
 */
#include "decomp.h"
#include "ztools.h"

static ZheapItem g_zheapPopped;          /* 0x006764E0 zheapPop's result */

/* Sift `item` down from slot `hole` (the exe repeats this loop in Pop and Remove). */
static void zheapSiftDown(Zheap *h, int hole, ZheapItem item)
{
    int child;

    while (hole * 2 <= h->count) {
        child = hole * 2;
        if (child != h->count) {
            if (h->maxHeap ? h->items[child + 1].key > h->items[child].key
                           : h->items[child + 1].key < h->items[child].key)
                child++;
        }
        if (h->maxHeap ? h->items[child].key <= item.key : item.key <= h->items[child].key)
            break;
        h->items[hole] = h->items[child];
        hole = child;
    }
    h->items[hole] = item;
}

/* VA 0x004a46c0, 66 bytes, [I] row, status draft */
void zheapInit(Zheap *h, int maxHeap, int capacity)
{
    if (capacity < 10)
        capacity = 10;
    h->items = (ZheapItem *)zmemAlloc(capacity * sizeof(ZheapItem) + sizeof(ZheapItem));
    h->capacity = capacity;
    h->maxHeap = maxHeap;
    h->count = 0;
    h->items[0].key = maxHeap ? 0xFFFFFFFFu : 0;
    h->items[0].value = 0;
}

/* VA 0x004a4710, 12 bytes, [I] row, status draft */
void zheapClear(Zheap *h)
{
    h->count = 0;
}

/* VA 0x004a4720, 168 bytes, [I] row, status draft */
int zheapInsert(uint32_t key, void *value, Zheap *h)
{
    int hole;

    if (h->count == h->capacity)
        return 0;
    hole = ++h->count;
    /* sift up; the sentinel ends the loop at the root */
    while (h->maxHeap ? h->items[hole / 2].key < key : key < h->items[hole / 2].key) {
        h->items[hole] = h->items[hole / 2];
        hole /= 2;
    }
    h->items[hole].key = key;
    h->items[hole].value = value;
    return 1;
}

/* VA 0x004a47d0, 227 bytes, [I] row, status draft */
ZheapItem *zheapPop(Zheap *h)
{
    ZheapItem last;

    if (h->count == 0)
        return 0;
    g_zheapPopped = h->items[1];
    last = h->items[h->count--];
    zheapSiftDown(h, 1, last);
    return &g_zheapPopped;
}

/* VA 0x004a48c0, 21 bytes, [I] row, status draft */
ZheapItem *zheapPeek(Zheap *h)
{
    if (h->count == 0)
        return 0;
    return &h->items[1];
}

/* VA 0x004a48e0, 15 bytes, [I] row, status draft */
int zheapIsEmpty(const Zheap *h)
{
    return h->count == 0;
}

/* VA 0x004a48f0, 21 bytes, [I] row, status draft */
int zheapIsFull(const Zheap *h)
{
    return h->count == h->capacity;
}

/* VA 0x004a4910, 15 bytes, [I] row, status draft */
void zheapFree(Zheap *h)
{
    zmemFree(h->items);
}

/* VA 0x004a4920, 251 bytes, [I] row, status draft */
int zheapRemove(void *value, Zheap *h)
{
    ZheapItem last;
    int i;

    if (h->count == 0)
        return 0;
    /* [V] the search starts at slot 1 and stops before slot `count`, so the last item is never
     * found (and a heap of one item never matches) */
    for (i = 1; i < h->count; i++)
        if (h->items[i].value == value)
            break;
    if (i >= h->count)
        return 0;
    last = h->items[h->count--];
    zheapSiftDown(h, i, last);           /* (no sift-up: the heap order can break) */
    return 1;
}

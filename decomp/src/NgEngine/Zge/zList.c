/*
 * NgEngine/Zge/zList.c
 * Original: C:\NgEngine\Zge\zList.c
 * Evidence: [V] path string 0x00561704 (zHeapListAdd, zHeapListRemoveAt); [I] the rest by link
 *           order: the code from 0x0048C110 (after zrwZgl.c's last function, which ends in exit())
 *           up to 0x0048C5B4 is list code only. The skeleton had put the first four functions in zrwZgl.c
 *           and the others in a gap file.
 * 20 functions, VA 0x0048c110-0x0048c5b4.
 * Decompiled (non-matching C, AD-8). Types: decomp/include/zlist.h.
 */
#include "decomp.h"

#define ZLIST_SRC "C:\\NgEngine\\Zge\\zList.c"

/* ------------------------------------------------------------------ zList (doubly linked) */

/* VA 0x0048c110, 15 bytes, [I] row, status draft */
void zListInit(zList *list)
{
    list->tail = NULL;
    list->head = NULL;
    list->count = 0;
}

/* VA 0x0048c120, 44 bytes, [I] row, status draft */
void zListAddHead(zList *list, zListNode *node)
{
    node->prev = NULL;
    node->next = list->head;
    list->head = node;
    if (list->tail == NULL)
        list->tail = node;
    if (node->next != NULL)
        node->next->prev = node;
    list->count++;
}

/* VA 0x0048c150, 44 bytes, [I] row, status draft */
void zListAddTail(zList *list, zListNode *node)
{
    node->next = NULL;
    node->prev = list->tail;
    list->tail = node;
    if (list->head == NULL)
        list->head = node;
    if (node->prev != NULL)
        node->prev->next = node;
    list->count++;
}

/* VA 0x0048c180, 112 bytes, [I] row, status draft */
void zListInsertAfter(zList *list, zListNode *node, zListNode *after)
{
    if (after == NULL) {
        /* [V] zListAddHead is inlined here *and* count is incremented again below, so inserting
         * at the head through this function counts the node twice */
        zListAddHead(list, node);
    } else {
        node->prev = after;
        node->next = after->next;
        if (after->next == NULL)
            list->tail = node;
        else
            after->next->prev = node;
        after->next = node;
    }
    list->count++;
}

/* VA 0x0048c1f0, 58 bytes, [I] row, status draft */
void zListRemove(zList *list, zListNode *node)
{
    if (node->prev == NULL)
        list->head = node->next;
    else
        node->prev->next = node->next;
    if (node->next == NULL)
        list->tail = node->prev;
    else
        node->next->prev = node->prev;
    list->count--;
}

/* VA 0x0048c230, 33 bytes, [I] row, status draft */
int zListContains(zList *list, zListNode *node)
{
    zListNode *it;

    for (it = list->head; it != NULL; it = it->next)
        if (it == node)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ zHeapList (pointer array) */

/* VA 0x0048c260, 66 bytes, [I] row, status draft */
zHeapList *zHeapListCreate(int32_t capacity)
{
    zHeapList *list = (zHeapList *)zmemAlloc(sizeof(zHeapList));   /* [V] not NULL-checked */

    list->cursor = -1;
    list->items = (void **)zmemAlloc(capacity * sizeof(void *));
    zstringMemSet(list->items, 0, capacity * sizeof(void *));
    list->capacity = capacity;
    list->count = 0;
    return list;
}

/* VA 0x0048c2b0, 24 bytes, [I] row, status draft */
void zHeapListDestroy(zHeapList *list)
{
    zmemFree(list->items);
    zmemFree(list);
}

/* VA 0x0048c2d0, 92 bytes, [I] row, status draft */
void zHeapListClear(zHeapList *list, zHeapListFn fn, void *user)
{
    int32_t i;

    /* optional per-item callback first (stops early when it returns 0), then empty the list */
    if (fn != NULL) {
        for (i = 0; i < list->count; i++)
            if (!fn(list->items[i], user))
                break;
    }
    zstringMemSet(list->items, 0, list->capacity * sizeof(void *));
    list->cursor = -1;
    list->count = 0;
}

/* VA 0x0048c330, 33 bytes, [I] row, status draft */
void *zHeapListFirst(zHeapList *list)
{
    list->cursor = -1;
    if (list->count <= 0)
        return NULL;
    list->cursor = 0;
    return list->items[0];
}

/* VA 0x0048c360, 38 bytes, [I] row, status draft */
void *zHeapListNext(zHeapList *list)
{
    if (list->cursor >= 0) {
        list->cursor++;
        if (list->cursor < list->count)
            return list->items[list->cursor];
    }
    list->cursor = -1;
    return NULL;
}

/* VA 0x0048c390, 20 bytes, [I] row, status draft */
void *zHeapListCurrent(zHeapList *list)
{
    if (list->cursor < 0)
        return NULL;
    return list->items[list->cursor];
}

/* VA 0x0048c3b0, 22 bytes, [I] row, status draft */
void *zHeapListGet(zHeapList *list, int32_t index)
{
    if (index >= list->count)      /* [V] no check for a negative index */
        return NULL;
    return list->items[index];
}

/* VA 0x0048c3d0, 37 bytes, [I] row, status draft */
int32_t zHeapListFind(zHeapList *list, void *item)
{
    int32_t i;

    for (i = 0; i < list->count; i++)
        if (list->items[i] == item)
            return i;
    return -1;
}

/* VA 0x0048c400, 8 bytes, [I] row, status draft */
int32_t zHeapListCount(zHeapList *list)
{
    return list->count;
}

/* VA 0x0048c410, 8 bytes, [I] row, status draft */
int32_t zHeapListCapacity(zHeapList *list)
{
    return list->capacity;
}

/* VA 0x0048c420, 58 bytes, [I] row, status draft */
void zHeapListForEach(zHeapList *list, zHeapListFn fn, void *user)
{
    int32_t i;

    if (fn == NULL)
        return;
    for (i = 0; i < list->count; i++)
        if (!fn(list->items[i], user))
            break;
}

/* VA 0x0048c460, 166 bytes, [V] direct, status draft */
void zHeapListAdd(zHeapList *list, void *item)
{
    void **old;

    if (item == NULL)   /* reported, then inserted anyway [V] */
        zAssertFail("Cannot insert NULL values into a heap list.", ZLIST_SRC, 0x1cf, 0);
    if (list->count == list->capacity) {
        old = list->items;
        list->items = (void **)zmemAlloc(list->capacity * 2 * sizeof(void *));
        zstringMemSet(list->items, 0, list->capacity * 2 * sizeof(void *));
        zstringMemCopy(list->items, old, list->capacity * sizeof(void *));
        list->capacity *= 2;
        zmemFree(old);
    }
    if (list->cursor == -1) {
        list->items[list->count++] = item;
    } else {
        /* during an iteration the new item takes the current slot and the current item moves to
         * the end, so zHeapListNext still visits it */
        list->items[list->count] = list->items[list->cursor];
        list->count++;
        list->items[list->cursor] = item;
    }
}

/* VA 0x0048c510, 81 bytes, [I] row, status draft */
void *zHeapListRemoveAt(zHeapList *list, int32_t index)
{
    void *item;

    if (index < 0 || index >= list->count) {
        zAssertFail("Out of range", ZLIST_SRC, 0x1f4, 0);
        return NULL;
    }
    /* the last item fills the hole */
    item = list->items[index];
    list->count--;
    list->items[index] = list->items[list->count];
    list->items[list->count] = NULL;
    return item;
}

/* VA 0x0048c570, 69 bytes, [I] row, status draft */
void *zHeapListRemoveCurrent(zHeapList *list)
{
    void *item;

    if (list->cursor == -1) {
        /* no iteration: pop the last item */
        list->count--;
        item = list->items[list->count];
        list->items[list->count] = NULL;
        return item;
    }
    /* the last item moves into the current slot; the cursor is not changed, so the next
     * zHeapListNext skips the moved item [V] */
    item = list->items[list->cursor];
    list->count--;
    list->items[list->cursor] = list->items[list->count];
    list->items[list->count] = NULL;
    return item;
}

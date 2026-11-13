/*
 * NgEngine/Zge/zstring.c
 * Original: C:\NgEngine\Zge\zstring.c
 * Evidence: [V] path string 0x00560590 (string sets, zTextGet, macros); [I] the rest by link order
 *           (0x00482420-0x00483014, between zobjCamera.c and zquat.c).
 * 39 functions, VA 0x00482420-0x00483014.
 * Decompiled (non-matching C, AD-8). Types: decomp/include/zstring.h.
 *
 * Three parts:
 *  1. text-id tables ("STRS" string sets), zTextGet and "=name=" macro expansion;
 *  2. thin wrappers of the C runtime (MSVC inlines strcpy/strcat/strcmp/strlen, so those wrappers
 *     contain the inline code), memcpy/memmove/memset written out by hand, and zHashName;
 *  3. 0x00482BD0-0x00483014: name registries of the particle-effect system (effect types,
 *     affectors, spawn functions; registered by 0x0045F780 with names such as "spark", "gravity").
 *     They follow zstring.c in link order and use no other module's literals, so the skeleton put them
 *     here [I]; they may well be a separate object without an assert path (open question).
 */
#include <stdarg.h>
#include "decomp.h"

#define ZSTRING_SRC "C:\\NgEngine\\Zge\\zstring.c"

/* MSVC CRT (statically linked, not decompiled; docs/re/libraries.md) */
char *__cdecl strcpy(char *dst, const char *src);
char *__cdecl strcat(char *dst, const char *src);
int   __cdecl strcmp(const char *a, const char *b);
size_t __cdecl strlen(const char *s);
char *__cdecl strncpy(char *dst, const char *src, size_t n);          /* 0x004A5BC0-ish, lib */
char *__cdecl strchr(const char *s, int c);
int   __cdecl strncmp(const char *a, const char *b, size_t n);
unsigned long __cdecl strtoul(const char *s, char **end, int base);   /* 0x004A56FE */
char *__cdecl _strupr(char *s);                                       /* 0x0051D2F0 */
int   __cdecl _strcmpi(const char *a, const char *b);
int   __cdecl vsprintf(char *dst, const char *fmt, va_list ap);       /* 0x004A5338 */
int   __cdecl toupper(int c);                                         /* 0x004A593C */

ZstringSet *g_zstringSets[ZSTRING_MAX_SETS];   /* 0x006704D8 */
ZstringSet *g_zstringMacros;                   /* 0x00670514 never written in this build [V] */
char        g_zstringExpandBuf[200];           /* 0x00670310 */
char        g_zstringFormatBuf[256];           /* 0x006703D8 */
int         g_zfxRegistryInit;                 /* 0x00670518 */
zHeapList  *g_zfxTypes;                        /* 0x0067051C */
zHeapList  *g_zfxAffectors;                    /* 0x00670520 */
zHeapList  *g_zfxSpawners;                     /* 0x00670524 */

/* ------------------------------------------------------------------ string sets */

/* VA 0x00482420, 22 bytes, [I] row, status draft */
int zstringInit(void)
{
    zstringMemSet(g_zstringSets, 0, sizeof(g_zstringSets));   /* inlined rep stosd */
    return 1;
}

/* VA 0x00482440, 6 bytes, [I] row, status draft */
int zstringShutdown(void)
{
    return 1;
}

/* VA 0x00482450, 75 bytes, [V] direct, status draft */
int zstringAddSet(ZstringSet *set)
{
    if (strncmp(set->magic, "STRS", 4) == 0) {
        g_zstringSets[set->firstId >> 12] = set;
        return 1;
    }
    zAssertFail("Error", ZSTRING_SRC, 0x9a, 0);
    return 0;
}

/* VA 0x004824a0, 67 bytes, [V] direct, status draft */
int zstringRemoveSet(ZstringSet *set)
{
    uint32_t idx = set->firstId >> 12;

    if (g_zstringSets[idx] == set) {
        g_zstringSets[idx] = NULL;
        return 1;
    }
    zAssertFail("Error", ZSTRING_SRC, 0xbd, 0);
    return 0;
}

/* VA 0x004824f0, 113 bytes, [V] direct, status draft */
char *zTextGet(uint32_t id)
{
    uint32_t setIdx;
    ZstringSet *set;
    const uint16_t *offsets;

    id &= 0xFFFF;
    setIdx = id >> 12;
    set = g_zstringSets[setIdx];
    if (set != NULL && id >= set->firstId && id < (uint32_t)set->firstId + set->count) {
        offsets = (const uint16_t *)(set + 1);
        return (char *)set + offsets[id - set->firstId];
    }
    zAssertFail(zstringFormat("String(0x%x) not found in set:%d", id, setIdx), ZSTRING_SRC, 0xeb, 0);
    return NULL;
}

/* VA 0x00482570, 223 bytes, [I] row, status draft */
char *zstringFindMacro(const char *name)
{
    /* g_zstringMacros is a string set of "NAME=value" entries (each preceded by one byte that is
     * skipped). Nothing in the exe assigns it, so in this build every lookup asserts. */
    char key[88];
    const uint16_t *offsets;
    const char *p;
    int i, n;

    if (g_zstringMacros != NULL) {
        offsets = (const uint16_t *)(g_zstringMacros + 1);
        for (i = 0; i < g_zstringMacros->count; i++) {
            p = (const char *)g_zstringMacros + offsets[i] + 1;
            for (n = 0; *p != '\0' && *p != '='; p++)
                key[n++] = *p;                       /* [V] unbounded copy */
            key[n] = '\0';
            if (*p == '=')
                p++;
            if (strcmp(key, name) == 0)
                return (char *)p;
        }
    }
    zAssertFail("Not found", ZSTRING_SRC, 0x121, 0);
    return NULL;
}

/* VA 0x00482650, 173 bytes, [I] row, status draft */
char *zstringExpandMacros(const char *src)
{
    /* Copies src to a static buffer, replacing every "=NAME=" by the macro's value. */
    char name[100];
    char *out = g_zstringExpandBuf;
    const char *val;
    int n;

    if (src == g_zstringExpandBuf)
        return g_zstringExpandBuf;
    while (*src != '\0') {
        if (*src != '=') {
            *out++ = *src++;
            continue;
        }
        src++;
        for (n = 0; *src != '\0' && *src != '='; src++)
            name[n++] = *src;
        name[n] = '\0';
        if (*src == '=')
            src++;
        val = zstringFindMacro(name);
        if (val == NULL) {
            zAssertFail("GUIMAIN: Bad macro expansion\n", ZSTRING_SRC, 0x14c, 0);
            break;
        }
        while (*val != '\0')
            *out++ = *val++;
    }
    *out = '\0';
    return g_zstringExpandBuf;
}

/* ------------------------------------------------------------------ CRT wrappers */

/* VA 0x00482700, 44 bytes, [I] row, status draft */
char *zstringCopy(char *dst, const char *src)
{
    return strcpy(dst, src);
}

/* VA 0x00482730, 24 bytes, [I] row, status draft */
void zstringCopyN(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n);
}

/* VA 0x00482750, 54 bytes, [I] row, status draft */
char *zstringCat(char *dst, const char *src)
{
    return strcat(dst, src);
}

/* VA 0x00482790, 59 bytes, [I] row, status draft */
int zstringCompare(const char *a, const char *b)
{
    return strcmp(a, b);
}

/* VA 0x004827d0, 19 bytes, [I] row, status draft */
int zstringLength(const char *s)
{
    return (int)strlen(s);
}

/* VA 0x004827f0, 19 bytes, [I] row, status draft */
char *zstringFindChar(const char *s, int c)
{
    return strchr(s, c);
}

/* VA 0x00482810, 24 bytes, [I] row, status draft */
uint32_t zstringToUInt(const char *s, int base)
{
    char *end;      /* [V] the exe passes &base as the end pointer (it reuses the argument slot) */

    return strtoul(s, &end, base);
}

/* VA 0x00482830, 14 bytes, [I] row, status draft */
void zstringUpper(char *s)
{
    _strupr(s);
}

/* VA 0x00482840, 24 bytes, [I] row, status draft */
int zstringPrintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsprintf(dst, fmt, ap);
    va_end(ap);
    return n;
}

/* VA 0x00482860, 29 bytes, [I] row, status draft */
char *zstringFormat(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsprintf(g_zstringFormatBuf, fmt, ap);
    va_end(ap);
    return g_zstringFormatBuf;
}

/* VA 0x00482880, 19 bytes, [I] row, status draft */
int zstringCompareNoCase(const char *a, const char *b)
{
    return _strcmpi(a, b);
}

/* VA 0x004828a0, 24 bytes, [I] row, status draft */
int zstringCompareN(const char *a, const char *b, size_t n)
{
    return strncmp(a, b, n);
}

/* VA 0x004828c0, 110 bytes, [I] row, status draft */
uint32_t zHashName(const char *name)
{
    /* The asset/bone id hash (dmbmx.core.hash.zhash): case-insensitive,
     * h = sum((i+1) * C[i]) + sum((C[i] + C[i-1] * 0xFFFF) << 16), C = toupper as signed char. */
    uint32_t h = 0;
    int i;
    signed char prev = 0;
    const char *p;

    for (i = 1; name[i - 1] != '\0'; i++)
        h += (uint32_t)(toupper(name[i - 1]) * i);
    for (p = name; *p != '\0'; p++) {
        h += ((uint32_t)toupper(*p) + (uint32_t)prev * 0xFFFFu) << 16;
        prev = (signed char)toupper(*p);
    }
    return h;
}

/* VA 0x00482930, 86 bytes, [I] row, status draft */
char *zstringFindAny(const char *s, const char *set)
{
    int len = zstringLength(s), setLen = zstringLength(set);   /* inlined */
    int i, j;

    for (i = 0; i < len; i++)
        for (j = 0; j < setLen; j++)
            if (s[i] == set[j])
                return (char *)s + i;
    return NULL;
}

/* VA 0x00482990, 92 bytes, [I] row, status draft */
int zstringSpan(const char *s, const char *set)
{
    /* strspn: length of the leading part of s made of characters in set */
    int len = zstringLength(s), setLen = zstringLength(set);
    int i, j, found;

    for (i = 0; i < len; i++) {
        found = 0;
        for (j = 0; j < setLen; j++)
            if (s[i] == set[j])
                found = 1;
        if (!found)
            break;
    }
    return i;
}

/* VA 0x004829f0, 92 bytes, [I] row, status draft */
int zstringSpanNot(const char *s, const char *set)
{
    /* strcspn, except that [V] the loop runs to i <= len, so a string with none of the characters
     * gives len + 1, not len */
    int len = zstringLength(s), setLen = zstringLength(set);
    int i, j, found;

    for (i = 0; i <= len; i++) {
        found = 0;
        for (j = 0; j < setLen; j++)
            if (s[i] == set[j])
                found = 1;
        if (found)
            break;
    }
    return i;
}

/* VA 0x00482a50, 105 bytes, [I] row, status draft */
void *zstringMemCopy(void *dst, const void *src, uint32_t n)
{
    /* copies by 4, 2 or 1 bytes depending on the common alignment of dst, src and n */
    uint32_t align = (uint32_t)dst | (uint32_t)src | n;
    uint32_t i;

    if ((align & 3) == 0) {
        for (i = 0; i < n / 4; i++)
            ((uint32_t *)dst)[i] = ((const uint32_t *)src)[i];
    } else if ((align & 1) == 0) {
        for (i = 0; i < n / 2; i++)
            ((uint16_t *)dst)[i] = ((const uint16_t *)src)[i];
    } else {
        for (i = 0; (int32_t)i < (int32_t)n; i++)
            ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
    }
    return dst;
}

/* VA 0x00482ac0, 202 bytes, [I] row, status draft */
void *zstringMemMove(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    int32_t i;

    if (n == 0)
        return dst;
    if (s < d + n && d < s + n) {
        /* overlapping: byte copy in the safe direction */
        if (s > d) {
            for (i = 0; i < (int32_t)n; i++)
                d[i] = s[i];
        } else if (s < d) {
            for (i = (int32_t)n - 1; i >= 0; i--)
                d[i] = s[i];
        }
        return dst;
    }
    return zstringMemCopy(dst, src, n);   /* inlined */
}

/* VA 0x00482b90, 49 bytes, [I] row, status draft */
void *zstringMemSet(void *dst, int c, uint32_t n)
{
    uint32_t fill = (uint8_t)c * 0x01010101u;
    uint32_t *d4 = (uint32_t *)dst;
    uint8_t *d1;
    uint32_t i;

    for (i = 0; i < n / 4; i++)
        *d4++ = fill;
    d1 = (uint8_t *)d4;
    for (i = 0; i < (n & 3); i++)
        *d1++ = (uint8_t)c;
    return dst;
}

/* ------------------------------------------------------------------ particle-effect registries */

static void freeAll(zHeapList *list)
{
    void *e;

    for (e = zHeapListFirst(list); e != NULL; e = zHeapListFirst(list)) {
        zmemFree(e);
        zHeapListRemoveCurrent(list);
    }
    zHeapListDestroy(list);
}

static void addEntry(zHeapList *list, void *value, const char *name)
{
    ZfxRegEntry *e = (ZfxRegEntry *)zmemAlloc(sizeof(ZfxRegEntry));

    e->value = value;
    zstringCopy(e->name, name);                    /* [V] unbounded */
    e->hash = zHashName(e->name);
    zHeapListAdd(list, e);
}

static ZfxRegEntry *findEntry(zHeapList *list, const char *name)
{
    ZfxRegEntry *e;

    for (e = (ZfxRegEntry *)zHeapListFirst(list); e != NULL; e = (ZfxRegEntry *)zHeapListNext(list))
        if (zstringCompare(e->name, name) == 0)
            return e;
    return NULL;
}

static void *removeEntry(zHeapList *list, const char *name)
{
    ZfxRegEntry *e = findEntry(list, name);
    void *value;

    if (e == NULL)
        return NULL;
    value = e->value;
    zmemFree(e);                                   /* freed before it leaves the list [V] */
    zHeapListRemoveCurrent(list);
    return value;
}

/* VA 0x00482bd0, 50 bytes, [I] row, status draft */
void zfxRegistryInit(void)
{
    g_zfxTypes = zHeapListCreate(0x40);
    g_zfxAffectors = zHeapListCreate(0x10);
    g_zfxSpawners = zHeapListCreate(0x10);
    g_zfxRegistryInit = 1;
}

/* VA 0x00482c10, 227 bytes, [I] row, status draft */
void zfxRegistryShutdown(void)
{
    if (!g_zfxRegistryInit)
        return;
    freeAll(g_zfxTypes);
    freeAll(g_zfxAffectors);
    freeAll(g_zfxSpawners);
    g_zfxRegistryInit = 0;
}

/* VA 0x00482d00, 59 bytes, [I] row, status draft */
void zfxRegisterType(void *type, const char *name)
{
    addEntry(g_zfxTypes, type, name);
}

/* VA 0x00482d40, 96 bytes, [I] row, status draft */
void *zfxUnregisterType(const char *name)
{
    return removeEntry(g_zfxTypes, name);
}

/* VA 0x00482da0, 74 bytes, [I] row, status draft */
void *zfxFindType(const char *name)
{
    ZfxRegEntry *e = findEntry(g_zfxTypes, name);

    return e != NULL ? e->value : NULL;
}

/* VA 0x00482df0, 59 bytes, [I] row, status draft */
void zfxRegisterAffector(void *affector, const char *name)
{
    addEntry(g_zfxAffectors, affector, name);
}

/* VA 0x00482e30, 96 bytes, [I] row, status draft */
void *zfxUnregisterAffector(const char *name)
{
    return removeEntry(g_zfxAffectors, name);
}

/* VA 0x00482e90, 74 bytes, [I] row, status draft */
void *zfxFindAffector(const char *name)
{
    ZfxRegEntry *e = findEntry(g_zfxAffectors, name);

    return e != NULL ? e->value : NULL;
}

/* An effect object keeps its type descriptor (a table of functions, e.g. 0x0055C278) at +0x2C. */
typedef int (__cdecl *ZfxTypeFn1)(void *type, void *obj);
typedef int (__cdecl *ZfxTypeFn2)(void *type, void *obj, void *arg);
#define ZFX_OBJ_TYPE(obj) (*(void ***)((uint8_t *)(obj) + 0x2C))

/* VA 0x00482ee0, 27 bytes, [I] row, status draft */
int zfxObjCallSlot7(void *obj)
{
    void **type;

    if (obj == NULL || (type = ZFX_OBJ_TYPE(obj)) == NULL)
        return 0;
    return ((ZfxTypeFn1)type[7])(type, obj);
}

/* VA 0x00482f00, 32 bytes, [I] row, status draft */
int zfxObjCallSlot8(void *obj, void *arg)
{
    void **type;

    if (obj == NULL || (type = ZFX_OBJ_TYPE(obj)) == NULL)
        return 0;
    return ((ZfxTypeFn2)type[8])(type, obj, arg);
}

/* VA 0x00482f20, 59 bytes, [I] row, status draft */
void zfxRegisterSpawner(void *fn, const char *name)
{
    addEntry(g_zfxSpawners, fn, name);
}

/* VA 0x00482f60, 96 bytes, [I] row, status draft */
void *zfxUnregisterSpawner(const char *name)
{
    return removeEntry(g_zfxSpawners, name);
}

/* VA 0x00482fc0, 85 bytes, [I] row, status draft */
void zfxCallSpawner(const char *name, void *a, void *b)
{
    ZfxRegEntry *e = findEntry(g_zfxSpawners, name);

    if (e != NULL)
        ((void (__cdecl *)(void *, void *))e->value)(a, b);
}

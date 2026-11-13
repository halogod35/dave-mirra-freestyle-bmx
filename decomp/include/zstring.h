/*
 * zstring.h - NgEngine string helpers (NgEngine/Zge/zstring.c).
 *
 * Besides thin CRT wrappers, zstring.c holds the text-id tables ("STRS" string sets, zTextGet) and,
 * by link order [I], the name registries of the particle-effect system (see zstring.c).
 */
#ifndef ZSTRING_H
#define ZSTRING_H
#include "dmtypes.h"

/* A string set: a "STRS" resource. Text id = (set << 12) | n; the set index is the top nibble of
 * firstId. String n (firstId <= id < firstId + count) is at (char *)set + offsets[id - firstId]. */
typedef struct ZstringSet {
    char     magic[4];        /* +0x00 "STRS" */
    uint16_t firstId;         /* +0x04 */
    uint16_t count;           /* +0x06 */
    /* uint16_t offsets[count] follows at +0x08 */
} ZstringSet;                 /* 0x08 */

#define ZSTRING_MAX_SETS 16   /* g_zstringSets[16] at 0x006704D8 */

/* One entry of a particle-effect name registry (three zHeapLists at 0x0067051C/20/24). */
typedef struct ZfxRegEntry {
    void     *value;          /* +0x00 effect type, affector or spawn function */
    char      name[0x40];     /* +0x04 */
    uint32_t  hash;           /* +0x44 zHashName(name), stored but never used for lookup */
} ZfxRegEntry;                /* 0x48 */

/* GHIDRA-SKIP-BEGIN */
typedef char zstring_h_size_check[(sizeof(ZstringSet) == 0x08 && sizeof(ZfxRegEntry) == 0x48) ? 1 : -1];

int       zstringInit(void);                                       /* 0x00482420 */
int       zstringShutdown(void);                                   /* 0x00482440 */
int       zstringAddSet(ZstringSet *set);                          /* 0x00482450 */
int       zstringRemoveSet(ZstringSet *set);                       /* 0x004824A0 */
char     *zTextGet(uint32_t id);                                   /* 0x004824F0 */
char     *zstringFindMacro(const char *name);                      /* 0x00482570 */
char     *zstringExpandMacros(const char *src);                    /* 0x00482650 */
char     *zstringCopy(char *dst, const char *src);                 /* 0x00482700 strcpy */
void      zstringCopyN(char *dst, const char *src, size_t n);      /* 0x00482730 strncpy */
char     *zstringCat(char *dst, const char *src);                  /* 0x00482750 strcat */
int       zstringCompare(const char *a, const char *b);            /* 0x00482790 strcmp */
int       zstringLength(const char *s);                            /* 0x004827D0 strlen */
char     *zstringFindChar(const char *s, int c);                   /* 0x004827F0 strchr */
uint32_t  zstringToUInt(const char *s, int base);                  /* 0x00482810 strtoul */
void      zstringUpper(char *s);                                   /* 0x00482830 _strupr */
int       zstringPrintf(char *dst, const char *fmt, ...);          /* 0x00482840 sprintf */
char     *zstringFormat(const char *fmt, ...);                     /* 0x00482860 static buffer */
int       zstringCompareNoCase(const char *a, const char *b);      /* 0x00482880 _strcmpi */
int       zstringCompareN(const char *a, const char *b, size_t n); /* 0x004828A0 strncmp */
uint32_t  zHashName(const char *name);                             /* 0x004828C0 */
char     *zstringFindAny(const char *s, const char *set);          /* 0x00482930 strpbrk */
int       zstringSpan(const char *s, const char *set);             /* 0x00482990 strspn */
int       zstringSpanNot(const char *s, const char *set);          /* 0x004829F0 strcspn (+1) */
void     *zstringMemCopy(void *dst, const void *src, uint32_t n);  /* 0x00482A50 memcpy */
void     *zstringMemMove(void *dst, const void *src, uint32_t n);  /* 0x00482AC0 memmove */
void     *zstringMemSet(void *dst, int c, uint32_t n);             /* 0x00482B90 memset */

void      zfxRegistryInit(void);                                   /* 0x00482BD0 */
void      zfxRegistryShutdown(void);                               /* 0x00482C10 */
void      zfxRegisterType(void *type, const char *name);           /* 0x00482D00 */
void     *zfxUnregisterType(const char *name);                     /* 0x00482D40 */
void     *zfxFindType(const char *name);                           /* 0x00482DA0 */
void      zfxRegisterAffector(void *affector, const char *name);   /* 0x00482DF0 */
void     *zfxUnregisterAffector(const char *name);                 /* 0x00482E30 */
void     *zfxFindAffector(const char *name);                       /* 0x00482E90 */
int       zfxObjCallSlot7(void *obj);                              /* 0x00482EE0 */
int       zfxObjCallSlot8(void *obj, void *arg);                   /* 0x00482F00 */
void      zfxRegisterSpawner(void *fn, const char *name);          /* 0x00482F20 */
void     *zfxUnregisterSpawner(const char *name);                  /* 0x00482F60 */
void      zfxCallSpawner(const char *name, void *a, void *b);      /* 0x00482FC0 */
/* GHIDRA-SKIP-END */

#endif /* ZSTRING_H */

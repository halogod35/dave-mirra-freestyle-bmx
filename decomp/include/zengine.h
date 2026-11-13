/*
 * zengine.h - NgEngine core: memory (zmem.h), lists (zlist.h), strings (zstring.h) and the debug /
 * assert layer (zdebug.c). The memory, list and string modules keep their
 * zmem/zList/zstring declarations in their own headers. Not imported into Ghidra.
 */
#ifndef ZENGINE_H
#define ZENGINE_H
#include "dmtypes.h"
#include "zmem.h"
#include "zlist.h"
#include "zstring.h"

/* zdebug.c (VA 0x0047EC40-0x0047EDFD) */
typedef void (__cdecl *ZassertHandler)(const char *why, const char *file, int line, const char *func);

int      zdebugInit(void);                                          /* 0x0047EC40 */
int      zdebugShutdown(void);                                      /* 0x0047EC80 */
void     zdebugOpenLog(void);                                       /* 0x0047ECB0 */
void     zdebugCloseLog(void);                                      /* 0x0047ECD0 */
int      zdebugChecksum(const uint8_t *data, int len);              /* 0x0047ECF0 */
void     zAssertFail(const char *msg, const char *file, int line, const char *func); /* 0x0047ED10 */
void     zdebugPrintMatrix(const char *name, const float *m);       /* 0x0047ED30 */

/* The platform debug driver: a table of 20 function pointers at 0x00560030 filled with the
 * zrwDbg.c implementations (0x0047F850...). zdebug.c and others call through these slots. */
extern int  (__cdecl *g_pfnDebugInit)(void);                        /* 0x00560030 -> 0x0047F850 */
extern int  (__cdecl *g_pfnDebugShutdown)(void);                    /* 0x00560034 -> 0x0047F900 */
extern int  (__cdecl *g_pfnDebugPrintf)(const char *fmt, ...);      /* 0x00560074 -> 0x00480240 */
extern void (__cdecl *g_pfnDebugIndent)(int delta);                 /* 0x00560078 -> 0x004802C0, clamps to 0..32 */
extern ZassertHandler g_assertHandler;                              /* 0x0056147C = zAssertMessageBox */

#define ZASSERT(cond, msg, file, line) \
    do { if (!(cond)) zAssertFail((msg), (file), (line), 0); } while (0)

#endif /* ZENGINE_H */

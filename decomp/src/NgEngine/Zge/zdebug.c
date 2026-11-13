/*
 * NgEngine/Zge/zdebug.c
 * Original: C:\NgEngine\Zge\zdebug.c
 * Evidence: [V] path string 0x0055FFD8 (zdebugInit); [I] the rest by link order and globals
 *           (0x00650170-0x00650178 are used only here). zAssertFail was put in zrwDbg.c by the skeleton
 *           split; it sits between zdebug.c's functions and calls the handler zdebug.c owns.
 * 7 functions, VA 0x0047ec40-0x0047edfd.
 * Decompiled (non-matching C, AD-8). Declarations: decomp/include/zengine.h.
 *
 * The platform part (text output, debug drawing) is a table of 20 function pointers at 0x00560030
 * filled with zrwDbg.c functions; zdebug.c only calls through it. zAssertFail forwards to the
 * assert handler pointer g_assertHandler (0x0056147C), which App code points at zAssertMessageBox
 * (zrwZgl.c 0x00489CF0): the "RenderWare(tm) Application Error / ***ASSERT***" message box
 * (docs/re/integrity-checks.md #8). Asserts are live in the release build.
 */
#include "decomp.h"

#define ZDEBUG_SRC "C:\\NgEngine\\Zge\\zdebug.c"

int          g_zdebugInitialized;   /* 0x00650170 */
zfileHandle *g_zdebugLog;           /* 0x00650174 */
char        *g_zdebugLogName;       /* 0x00650178 never assigned in this build [V], so no log */

/* VA 0x0047ec40, 54 bytes, [V] direct, status draft */
int zdebugInit(void)
{
    if (g_zdebugInitialized)   /* the assert macro calls the handler directly here (inlined) */
        g_assertHandler("Already initialized", ZDEBUG_SRC, 0x48, 0);
    if (!g_pfnDebugInit())
        return 0;
    g_zdebugInitialized = 1;
    return 1;
}

/* VA 0x0047ec80, 36 bytes, [I] row, status draft */
int zdebugShutdown(void)
{
    if (g_zdebugInitialized) {
        if (!g_pfnDebugShutdown())
            return 0;
        g_zdebugInitialized = 0;
    }
    return 1;
}

/* VA 0x0047ecb0, 28 bytes, [I] row, status draft */
void zdebugOpenLog(void)
{
    if (g_zdebugLogName != NULL)
        g_zdebugLog = zfileOpen(g_zdebugLogName, ZFILE_MODE_WRITE, NULL);
}

/* VA 0x0047ecd0, 21 bytes, [I] row, status draft */
void zdebugCloseLog(void)
{
    if (g_zdebugLog != NULL)
        zfileClose(g_zdebugLog, NULL);
}

/* VA 0x0047ecf0, 27 bytes, [I] row, status draft */
int zdebugChecksum(const uint8_t *data, int len)
{
    /* Plain byte sum. [V] Uncalled: nothing in the exe references 0x0047ECF0, so it is no
     * integrity check (the integrity-check survey was right to find none). */
    int sum = 0;

    while (len-- > 0)
        sum += *data++;
    return sum;
}

/* VA 0x0047ed10, 30 bytes, [I] row, status draft */
void zAssertFail(const char *msg, const char *file, int line, const char *func)
{
    /* 252 callers. The `assert-continue` patch (tools/dmbmx/patch/patches.json) turns this into `ret`. */
    g_assertHandler(msg, file, line, func);
}

/* VA 0x0047ed30, 206 bytes, [I] row, status draft */
void zdebugPrintMatrix(const char *name, const float *m)
{
    /* m is a RenderWare matrix (right +0x00, up +0x10, at +0x20, pos +0x30). The three axes are
     * printed as columns, then the position; lines after the first are indented by strlen(name). */
    int indent = name != NULL ? zstringLength(name) : 0;

    g_pfnDebugPrintf("%s% 10.4f,% 10.4f,% 10.4f\n", name, m[0], m[4], m[8]);
    g_pfnDebugIndent(indent);
    g_pfnDebugPrintf("% 10.4f,% 10.4f,% 10.4f\n", m[1], m[5], m[9]);
    g_pfnDebugPrintf("% 10.4f,% 10.4f,% 10.4f\n", m[2], m[6], m[10]);
    g_pfnDebugPrintf("% 10.4f,% 10.4f,% 10.4f\n", m[12], m[13], m[14]);
    g_pfnDebugIndent(-indent);
}

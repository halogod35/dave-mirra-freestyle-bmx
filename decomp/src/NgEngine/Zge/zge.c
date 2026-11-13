/*
 * NgEngine/Zge/zge.c
 * Original: C:\NgEngine\Zge\zge.c
 * Evidence: [V] path string referenced by 1 function(s); the others [I] (the shared global
 * 0x00675378, link order). The ZGE object pass created zgeShutdown and the two unit converters; the clean-up the
 * debug dump zgeDump 0x0049FA60.
 * 5 functions, VA 0x0049f900-0x0049fb29.
 * Draft. Non-matching C (AD-8). Description: docs/re/zge.md 1.
 *
 * Engine start-up and shut-down: the order in which App_Open (via NgApp) brings the ZGE modules up
 * and App_Start's exit path takes them down again (reverse order).
 */
#include "decomp.h"
#include "zobj.h"
#include "ztools.h"
#include "ngif.h"
#include "zglrw.h"

#define ZGE_FILE "C:\\NgEngine\\Zge\\zge.c"

int  zstringInit(void);              /* 0x00482430 */
int  zmemShutdown(void);             /* 0x00485A50 */
int  zsceneInit(char *database);     /* 0x00490880 */
int  ziffShutdown(void);             /* 0x0049A740 (ziff.c, created by the clean-up) */
void zmemDump(void);                 /* 0x004859B0 an empty debug dump (zmem.c, the clean-up) */

static int g_zgeInitialised;         /* 0x00675378 */

/* VA 0x0049f900, 192 bytes, [V] direct, status draft */
int zgeInit(const char *sceneFile)
{
    if (g_zgeInitialised)
        zAssertFail("Already initialized", ZGE_FILE, 0x41, 0);
    g_zgeInitialised = 1;
    if (!zmemInit())
        return 0;
    if (!zmathInit())
        return 0;
    if (!zfileInit())
        return 0;
    if (!zstringInit())
        return 0;
    ziffInit();                       /* returns 1 in EAX; the exe tests it */
    if (!g_zrwZglInit())
        return 0;
    if (!zinputInit())
        return 0;
    if (!zdebugInit())
        return 0;
    if (!zobjInit(0x32))
        return 0;
    if (!zrwColTagsInit())
        return 0;
    if (!zsceneInit((char *)sceneFile))
        return 0;
    if (!ztimerInit(0x40))
        return 0;
    return zprofileInit() != 0;
}

/* VA 0x0049f9c0, 160 bytes, [I] row, status draft */
int zgeShutdown(void)
{
    if (!g_zgeInitialised)
        return 1;
    if (!zprofileShutdown())
        return 0;
    if (!ztimerShutdown())
        return 0;
    if (!zsceneShutdown())
        return 0;
    if (!zrwColTagsShutdown())
        return 0;
    if (!zobjShutdown())
        return 0;
    if (!zdebugShutdown())
        return 0;
    if (!zinputShutdown())
        return 0;
    if (!g_zrwZglShutdown())
        return 0;
    if (!ziffShutdown())
        return 0;
    if (!zstringShutdown())
        return 0;
    if (!zfileShutdown())
        return 0;
    if (!zmathShutdown())
        return 0;
    if (!zmemShutdown())
        return 0;
    g_zgeInitialised = 0;
    return 1;
}

/* VA 0x0049fa60, 141 bytes, [I] row, status draft */
void zgeDump(void)
{
    /* (new function; no caller in the exe) Debug dump: the ZGE objects (zobjDumpAll, then the empty
     * zmemDump), then the ZGL materials, models and textures through the driver tables. */
    g_pfnDebugPrintf("-------------------------------------------------------\n");
    g_pfnDebugPrintf("ZGE DUMP\n");
    g_pfnDebugPrintf("-------------------------------------------------------\n");
    g_pfnDebugIndent(1);
    zobjDumpAll();
    zmemDump();
    g_pfnDebugIndent(-1);
    g_pfnDebugPrintf("-------------------------------------------------------\n");
    g_pfnDebugPrintf("ZGL DUMP\n");
    g_pfnDebugPrintf("-------------------------------------------------------\n");
    g_pfnDebugIndent(1);
    g_zglMaterialApi.printStats();
    g_zglModelApi.printStats();
    g_texDriverVtbl.printAll();
    g_pfnDebugIndent(-1);
    g_pfnDebugPrintf("-------------------------------------------------------\n");
}

/* VA 0x0049faf0, 27 bytes, [I] row, status draft */
float zgeScalarFromRw(float v)
{
    float out;

    g_zglGet(0x13, &out, (int)&v);    /* v / the world scale */
    return out;
}

/* VA 0x0049fb10, 26 bytes, [I] row, status draft */
RwV3d *zgeVecFromRw(RwV3d *out, const RwV3d *v)
{
    g_zglGet(0x11, out, (int)v);      /* scaled, y and z negated */
    return out;
}

/*
 * zaxis/Bmx/Src/com/zge/NgIF/MissingZGE.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\MissingZGE.c
 * Evidence: [V] path string 0x0055BAC4, printed by all 7 stubs with their line numbers
 *           (0x56 .. 0x81) and the name of the ZGE function they stand for; [I] ngappEmptyHook
 *           (0x0045A1A0, a bare "ret" between zoverii.c and this file).
 * 8 functions, VA 0x0045a1a0-0x0045a322.
 * Draft. Non-matching C (AD-8). Types / prototypes: ngif.h. Description: docs/re/ngif.md.
 *
 * ZGE (the PlayStation engine) functions the PC port does not have: each one prints
 * "<file> <line> <name>" through the debug printf (silent in the release build) and returns.
 * zsystemSetActionMode / zsystemSetMenuMode also switch g_ngappMode, which NgApp.c's frame
 * function uses to swap the front end for the game and back.
 */
#include "decomp.h"
#include "ngif.h"

#define MISSINGZGE_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\MissingZGE.c"

/* VA 0x0045a1a0, 1 bytes, [I] between, status draft */
void ngappEmptyHook(void)
{
    /* empty; action.c calls it when a run ends and guimain.c at start-up */
}

/* VA 0x0045a1c0, 27 bytes, [V] direct, status draft */
void zconsoleLog(const char *msg)
{
    (void)msg;
    NGIF_MISSING(MISSINGZGE_SRC, 0x56, "zconsoleLog");
}

/* VA 0x0045a1f0, 27 bytes, [V] direct, status draft */
void zglEnableHorizon(void)
{
    NGIF_MISSING(MISSINGZGE_SRC, 0x5B, "Enable Horizon");
}

/* VA 0x0045a220, 27 bytes, [V] direct, status draft */
void zglDisableHorizon(void)
{
    NGIF_MISSING(MISSINGZGE_SRC, 0x60, "zgl disable horizon");
}

/* VA 0x0045a250, 34 bytes, [V] direct, status draft */
void zsystemSetActionMode(void)
{
    g_ngappMode = NGAPP_MODE_ACTION;     /* ngappFrame loads the level next frame */
    NGIF_MISSING(MISSINGZGE_SRC, 0x66, "zsystemSetActionMode()");
}

/* VA 0x0045a290, 34 bytes, [V] direct, status draft */
void zsystemSetMenuMode(void)
{
    g_ngappMode = NGAPP_MODE_MENU;       /* ngappFrame unloads the level and restarts the front end */
    NGIF_MISSING(MISSINGZGE_SRC, 0x6C, "zsystemSetMenuMode");
}

/* VA 0x0045a2d0, 27 bytes, [V] direct, status draft */
void zfxDestroyAll(uint32_t which)
{
    (void)which;
    NGIF_MISSING(MISSINGZGE_SRC, 0x7A, "zfxDestroyAll");
}

/* VA 0x0045a300, 35 bytes, [V] direct, status draft */
int zfxRegisterFuncTable(void *const *funcs, int count)
{
    (void)funcs;
    (void)count;
    NGIF_MISSING(MISSINGZGE_SRC, 0x81, "zfxRegisterFuncTable");
    return 1;
}

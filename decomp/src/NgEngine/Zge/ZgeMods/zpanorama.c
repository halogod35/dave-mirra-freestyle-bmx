/*
 * NgEngine/Zge/ZgeMods/zpanorama.c
 * Original: unknown (no path string; name invented in the original style)
 * Evidence: [I] new file. The five functions share the global 0x0067537C (between the
 * pcmovie.c globals and the ztoken parser's 0x00675380), zsceneInit/zsceneClose/zsceneShutdown call
 * Init/Close/Shutdown as a unit, and the "Panorama" literal 0x00563554 sits between the pcmovie.c
 * and ztoken literals. [V] for zpanoramaLoad (handler 'SDPA' in zsceneSceneHandlers 0x00561908).
 * 5 functions, VA 0x004a1060-0x004a1111.
 * Draft. Non-matching C (AD-8). Format: docs/formats/LEVEL.md 5.3 (SDPA).
 *
 * The panorama is the sky/backdrop model of a level: Scene._sd's SDPA form holds one MODL. It is
 * loaded as a scene *template* with the id zHashName("Panorama") (0x00410A3B) and has no placement
 * data (no matrix, no position). Nothing in the exe uses it afterwards [V, static check 2026-11-12]:
 * zpanoramaGet has no caller, no pointer and no thunk; "Panorama" is the only such literal; the hash
 * 0x00410A3B occurs nowhere in the image, so no code asks zsceneCreateModel for it. [I] The shipped
 * game therefore never instantiates or draws the panorama (unverified in game; a play-test could
 * confirm it by blanking the SDPA model); its placement is therefore unspecified.
 */
#include "decomp.h"

void    *zobjModelLoadIff(ZiffFile *iff, int flags);   /* 0x00489090 reads the MODL chunk */

#define ID_MODL 0x4C444F4Du   /* 'MODL' */

static void *g_zpanorama;     /* 0x0067537C the panorama model, NULL if the level has none */

/* VA 0x004a1060, 16 bytes, [I] row, status draft */
int zpanoramaInit(void)
{
    g_zpanorama = 0;
    return 1;
}

/* VA 0x004a1070, 6 bytes, [I] row, status draft */
int zpanoramaShutdown(void)
{
    return 1;
}

/* VA 0x004a1080, 1 bytes, [I] row, status draft */
void zpanoramaClose(void)
{
    /* empty: the model is a scene template, released by zsceneFreeObjects; g_zpanorama is not cleared */
}

/* VA 0x004a1090, 6 bytes, [I] row, status draft */
void *zpanoramaGet(void)
{
    return g_zpanorama;
}

/* VA 0x004a10a0, 114 bytes, [I] row, status draft */
int zpanoramaLoad(ZiffFile *iff)
{
    if (!ziffChunkOpen(iff))
        return 1;
    if (!ziffChunkFindFirst(iff, ID_MODL)) {
        ziffChunkClose(iff);
        return 0;                       /* an SDPA without MODL fails the Scene._sd load */
    }
    g_zpanorama = zobjModelLoadIff(iff, 0);
    if (g_zpanorama)
        zsceneAddObject(0, zHashName("Panorama"), ZSCENE_OBJ_MODEL, 1, g_zpanorama);
    ziffChunkClose(iff);
    return 1;
}

/*
 * NgEngine/Zge/ZgeMods/zFont.c
 * Original: C:\NgEngine\Zge\ZgeMods\zFont.c
 * Evidence: [V] path string referenced by 3 function(s); the others [I]: the globals
 * 0x006763D0-0x006763E4, the Zfont / ZfontGlyph layouts. DM-061 created the constructors /
 * destructors 0x004A2620-0x004A26B0 (before the ZFNT handler) and 0x004A2800, 0x004A2820,
 * 0x004A2950 in the holes.
 * 11 functions, VA 0x004a2620-0x004a2c62.
 * DM-061 (draft). Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 4.
 *
 * C++ in the original: `new Zfont[1]` and `new ZfontGlyph[n]` put a count word in front of the
 * block (the constructors and the array destructor below are the compiler's). A ZFNT chunk (or
 * file) holds vector glyphs: u16 version 0x0101, u16 glyph count, then per glyph u16 points (0 =
 * none), u16 strokes, points * 16 bytes, and per stroke u16 n + n u16 point indices. Fonts stay
 * loaded (nothing frees them); only zfontDrawTest, a debug spinner, draws them.
 */
#include "decomp.h"
#include "ztools.h"
#include "zmath.h"

#define ZFONT_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zFont.c"
#define ID_ZFNT 0x544E465Au

extern int (__cdecl *g_zglDrawStrokes)(uint32_t flags, const ZfontStroke *strokes, int count,
                                       const RwMatrix *m, int arg);   /* 0x00561734 -> 0x0048D170 [I] */

static int   g_zfontInitialised;  /* 0x006763D0 */
static zList g_zfontList;         /* 0x006763D4 Zfont.node; Init clears both (0x10 bytes) */
static int g_zfontTestGlyph;    /* 0x006763E0 */
static int g_zfontTestFrame;    /* 0x006763E4 */

/* VA 0x004a2620, 20 bytes, [I] row, status draft */
void zfontGlyphConstruct(ZfontGlyph *g)
{
    g->numPoints = 0;
    g->numStrokes = 0;
    g->points = 0;
    g->strokes = 0;
}

/* VA 0x004a2640, 86 bytes, [I] row, status draft */
void zfontGlyphDestruct(ZfontGlyph *g)
{
    uint32_t i;

    if (g->points)
        zmemFree(g->points);
    if (g->strokes) {
        for (i = 0; i < g->numStrokes; i++)
            zmemFree(g->strokes[i].points);
        zmemFree(g->strokes);
    }
}

/* VA 0x004a26a0, 14 bytes, [I] row, status draft */
void zfontConstruct(Zfont *font)
{
    font->numGlyphs = 0;
    font->glyphs = 0;
}

/* VA 0x004a26b0, 162 bytes, [I] row, status draft */
void zfontDestruct(Zfont *font)
{
    uint32_t *cookie, i;

    if (font->glyphs == 0)
        return;
    cookie = (uint32_t *)font->glyphs - 1;      /* delete[] font->glyphs */
    for (i = 0; i < *cookie; i++)
        zfontGlyphDestruct(&font->glyphs[i]);   /* inlined */
    zmemFree(cookie);
}

/* VA 0x004a2760, 56 bytes, [I] row, status draft */
int zfontChunkZFNT(void *iff)
{
    uint32_t size = ziffChunkGetSize((ZiffFile *)iff);
    void *data = zmemAlloc(size);

    ziffChunkReadData((ZiffFile *)iff, data, size);
    zfontLoad(data, size);
    zmemFree(data);
    return 1;
}

/* VA 0x004a27a0, 89 bytes, [V] direct, status draft */
int zfontInit(void)
{
    if (g_zfontInitialised)
        zAssertFail("Already initialized", ZFONT_FILE, 0x97, 0);
    g_zfontInitialised = 0;
    zstringMemSet(&g_zfontList, 0, sizeof(g_zfontList));
    ziffRegisterHandler(ID_ZFNT, (ZiffHandlerFn)zfontChunkZFNT);
    zListInit(&g_zfontList);
    g_zfontInitialised = 1;
    return 1;
}

/* VA 0x004a2800, 25 bytes, [I] row, status draft */
int zfontShutdown(void)
{
    if (g_zfontInitialised)
        g_zfontInitialised = 0;               /* the fonts are not freed */
    return 1;
}

/* VA 0x004a2820, 303 bytes, [I] row, status draft */
void zfontDrawTest(void)
{
    /* a debug spinner: the first font's glyphs one after the other (a new one every 30 frames),
     * rotating one degree per frame at (200, 300) */
    static const RwV3d scale = { 0.15f, -0.15f, 0.15f };
    static const RwV3d where = { 200.0f, 300.0f, 0.0f };
    Zfont *font = (Zfont *)g_zfontList.head;
    ZfontGlyph *g = &font->glyphs[g_zfontTestGlyph];
    RwMatrix m;

    g_zfontTestFrame++;
    while (g->numPoints == 0) {
        g_zfontTestGlyph = (g_zfontTestGlyph + 1) % font->numGlyphs;
        g = &font->glyphs[g_zfontTestGlyph];
    }
    if (g_zfontTestFrame % 30 == 0)
        g_zfontTestGlyph = (g_zfontTestGlyph + 1) % font->numGlyphs;
    zmathMatrixIdentity(&m);
    zmathMatrixScale(&m, &m, &scale);
    zmathMatrixRotateZ(&m, &m, (int)(g_zfontTestFrame * (65536.0f / 360.0f)));
    zmathMatrixTranslate(&m, &m, &where);
    g_zglDrawStrokes(0x2000000, g->strokes, g->numStrokes, &m, 0);
}

/* VA 0x004a2950, 55 bytes, [I] row, status draft */
int zfontLoadFile(const char *path)
{
    int size;
    void *data = zfileLoadAll(path, &size);
    int ok;

    if (data == 0)
        return 0;
    ok = zfontLoad(data, size);
    zfileFreeLoaded(data);
    return ok;
}

/* VA 0x004a2990, 68 bytes, [V] direct, status draft */
int zfontLoad(const void *data, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t *cookie;
    Zfont *font;
    ZfontGlyph *g;
    uint16_t version, n, j, k;
    uint32_t i;

    /* the 68 bytes at 0x004A2990 check the version; the rest is 0x004A29D4, which Ghidra split off
     * as its own function (zfontLoadBody) */
    zstringMemCopy(&version, p, 2);
    p += 2;
    if (version != 0x0101) {
        zAssertFail("Bad font version", ZFONT_FILE, 0xFE, 0);
        return 0;
    }

    cookie = (uint32_t *)zmemAlloc(4 + sizeof(Zfont));          /* new Zfont[1] */
    cookie[0] = 1;
    font = (Zfont *)(cookie + 1);
    zfontConstruct(font);                                       /* inlined */
    zstringMemCopy(&font->numGlyphs, p, 2);
    p += 2;
    if (font->numGlyphs == 0)
        zAssertFail("Bad font glyph Number", ZFONT_FILE, 0x105, 0);
    zListAddTail(&g_zfontList, &font->node);

    cookie = (uint32_t *)zmemAlloc(font->numGlyphs * sizeof(ZfontGlyph) + 4);   /* new ZfontGlyph[n] */
    cookie[0] = font->numGlyphs;
    font->glyphs = (ZfontGlyph *)(cookie + 1);
    for (i = 0; i < font->numGlyphs; i++)
        zfontGlyphConstruct(&font->glyphs[i]);                  /* inlined */

    for (i = 0; i < font->numGlyphs; i++) {
        g = &font->glyphs[i];
        zstringMemCopy(&g->numPoints, p, 2);
        p += 2;
        if (g->numPoints == 0)
            continue;
        zstringMemCopy(&g->numStrokes, p, 2);
        p += 2;
        g->strokes = (ZfontStroke *)zmemAlloc(g->numStrokes * sizeof(ZfontStroke));
        g->points = (ZfontPoint *)zmemAlloc(g->numPoints * sizeof(ZfontPoint));
        for (j = 0; j < g->numPoints; j++) {
            zstringMemCopy(&g->points[j].x, p, 4);
            zstringMemCopy(&g->points[j].y, p + 4, 4);
            zstringMemCopy(&g->points[j].u, p + 8, 4);
            zstringMemCopy(&g->points[j].v, p + 12, 4);
            g->points[j].unk08 = 0;
            p += 16;
        }
        for (j = 0; j < g->numStrokes; j++) {
            ZfontStroke *s = &g->strokes[j];

            zstringMemCopy(&n, p, 2);
            p += 2;
            s->points = (ZfontPoint **)zmemAlloc(n * sizeof(ZfontPoint *));
            s->count = n;
            s->unk00 = 0;
            for (k = 0; k < n; k++) {
                uint16_t index;

                zstringMemCopy(&index, p, 2);
                p += 2;
                s->points[k] = &g->points[index];
            }
        }
    }
    return 1;
}

/* VA 0x004a29d4, 655 bytes, [V] direct, status draft */
int zfontLoadBody(void)
{
    /* not a real entry point: the tail of zfontLoad after the version check (data pointer in EDI),
     * see there */
    return 1;
}

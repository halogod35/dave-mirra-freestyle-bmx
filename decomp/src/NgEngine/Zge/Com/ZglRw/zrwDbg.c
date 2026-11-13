/*
 * NgEngine/Zge/Com/ZglRw/zrwDbg.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwDbg.c
 * Evidence: [V] path string 0x00560150 (asserts of zrwDbgInit 0x33B, AddTextCallback 0x513);
 *           "zrwDbg:" / "ZrwDbg:" / "zdebugPrint:" messages; the debug driver table 0x00560030
 *           (20 slots g_pfnDebug*, all here; the ZglRw pass moved slot 18 0x004802C0 from
 *           gap_zrwDbg__zinput.c).
 * 33 functions, VA 0x0047EE00-0x004802EC (17 new functions).
 * Draft. Non-matching C (AD-8). Types: zglrw.h, ngif.h. Description: docs/re/zrw.md 9.
 *
 * The debug driver: printf to the debugger (OutputDebugString, with an indent), on-screen text
 * (RtCharset, queued and drawn by the ZGL callback list 4 after the frame), and queued debug shapes
 * (line, axes, triangle, sphere, box: up to 512 per frame) drawn through zrwPrim.
 */
#include "decomp.h"
#include "zmath.h"
#include "ngif.h"
#include "zglrw.h"

#define ZRWDBG_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwDbg.c"
#define ZRWDBG_MAX_SHAPES 0x200


int  zrwDbgDrawText(const char *text, int x, int y);
int  RtCharsetPrint(void *charset, const char *text, int x, int y);   /* 0x004CBEF0 */
void *RtCharsetCreate(const RwRGBA *fg, const RwRGBA *bg);           /* 0x004CBFB0 */
void RtCharsetDestroy(void *charset);                                 /* 0x004CBF60 */
void *RtCharsetGetDesc(void *charset, int32_t *desc);                 /* 0x004CB3F0 { width, height, ... } */
int  zstringVPrintf(char *dst, const char *fmt, void *args);         /* 0x004A5338 (CRT vsprintf) */
float zmathSin(int a);
float zmathCos(int a);
RwMatrix *zmathMatrixScale(RwMatrix *dst, const RwMatrix *src, const RwV3d *s);
float *zobjCameraGetMatrix(void *cam);                                /* zobjCamera.c: the camera matrix */

extern int32_t  g_zrwFps;              /* 0x00697740 frames in the last second */
extern int32_t  g_zrwPvsEnabled;       /* 0x006976E8 */
extern int32_t  g_zrwPvsVisible;       /* 0x006976EC */
extern int32_t  g_zrwPvsTotal;         /* 0x006976F0 */
extern RwRGBA   g_zrwClearColour;      /* 0x00697730 */
extern RwRGBA   g_zrwTextColour;       /* 0x00697734 */
extern const ZglPrimApi g_zglPrimApi;  /* 0x00561720 */

/* ------------------------------------------------------------------ globals */
static int32_t     s_lineInit = 1, s_axesInit = 1, s_triInit = 1, s_boxInit = 1;  /* 0x00560080.. */
static const uint8_t s_boxEdges[24] = { 0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7, 7, 6, 6, 4,
                                        0, 4, 1, 5, 2, 6, 3, 7 };                 /* 0x00560090 */
static ZglVertex   s_lineV[2];          /* 0x00650210 */
static ZglLine     s_line;              /* 0x00650438 */
static ZglVertex   s_triV[3];           /* 0x00650248 */
static ZglTri      s_tri;               /* 0x00650380 */
static ZglVertex   s_boxV[8];           /* 0x006502A0 */
static ZglLine     s_boxLines[12];      /* 0x00650180 */
static ZglVertex   s_axesV[6];          /* 0x00650390 */
static ZglLine     s_axesLines[3];      /* 0x00650448 */
static int32_t     s_initialized;       /* 0x00650470 */
static int32_t     s_statsMode;         /* 0x00650474 0 off, 1 fps, 2 + text callbacks */
static void       *s_renderCallback;    /* 0x00650478 */
static zList       s_textCallbacks;     /* 0x0065047C */
static void       *s_charset;           /* 0x00650488 */
static int32_t     s_charW, s_charH;    /* 0x0065048C / 0x00650490 */
static char        s_fmt[0x200];        /* 0x00650494 */
static uint8_t     s_text[0x400];       /* 0x00650694 queued { s16 x, s16 y, text } */
static uint8_t    *s_textEnd;           /* 0x00650A94 */
static int32_t     s_numShapes;         /* 0x00650A98 */
static ZrwDbgShape s_shapes[ZRWDBG_MAX_SHAPES];   /* 0x00650A9C */
static int32_t     s_indent;            /* 0x0065E29C */
static ZglVertex  *s_sphereV;           /* 0x0065E2A0 8 x 9 x 2 vertices */
static ZglVertex **s_sphereP;           /* 0x0065E2A4 */

static uint32_t zrwDbgPrimFlags(uint16_t f)
{
    uint32_t r = (f & 1) ? 1 : 0;
    if (f & 2)
        r |= 0x6502;    /* the default render state (zrwZglResetRenderState) */
    return r;
}

static void zrwDbgPrintStatsLine(void)
{
    int32_t *s = (int32_t *)RwEngineGetMetrics();
    if (s != NULL) {
        zdebugPrint(0, -1, "FPS:%02d numTri:%05d (Proc:%05d) Verts:%05d ResAlloc:%02d TxrUpload:%02d",
                    g_zrwFps, s[0], s[1], s[2], s[5], s[3]);
        return;
    }
    zdebugPrint(0, -1, "FPS:%02d", g_zrwFps);
    if (g_zrwPvsEnabled)
        zdebugPrint(0, -1, "PVS:Saved=%02d", g_zrwPvsVisible - g_zrwPvsTotal);
}

/* VA 0x0047ee00, 125 bytes, [I] new function, status draft (no caller found) */
int zrwDbgPrintStats(void)
{
    int32_t *s = (int32_t *)RwEngineGetMetrics();
    zrwDbgPrintStatsLine();
    return s != NULL;
}

/* VA 0x0047ee80, 185 bytes, [I] range, status draft (shape 1: a line, 2 ZGE points) */
void zrwDbgDrawLine(ZrwDbgShape *sh)
{
    const float *d = sh->data;
    int i;
    if (s_lineInit) {
        s_lineInit = 0;
        for (i = 0; i < 2; i++)
            s_lineV[i].u = s_lineV[i].v = 0.0f;
        s_line.material = 0;
        s_line.v[0] = &s_lineV[0];
        s_line.v[1] = &s_lineV[1];
    }
    for (i = 0; i < 2; i++) {
        s_lineV[i].pos.x = d[i * 4 + 0];
        s_lineV[i].pos.y = d[i * 4 + 1];
        s_lineV[i].pos.z = d[i * 4 + 2];
        s_lineV[i].colour = *(const RwRGBA *)&d[8];
    }
    g_zglPrimApi.drawLines(zrwDbgPrimFlags(*(const uint16_t *)&d[9]), &s_line, 1, NULL);
}

/* VA 0x0047ef40, 745 bytes, [I] range, status draft (shape 0: RGB axes of a matrix, half a unit
 * long; data[16] u16 0 bit 0 = drawn in front of the camera, 5 units away; u16 1 = render flags) */
void zrwDbgDrawAxes(ZrwDbgShape *sh)
{
    const float *m = sh->data;
    RwV3d origin;
    uint32_t flags = 0;
    int i;

    if (s_axesInit) {
        static const RwRGBA c[3] = { { 0xFF, 0, 0, 0xFF }, { 0, 0xFF, 0, 0xFF }, { 0, 0, 0xFF, 0xFF } };
        s_axesInit = 0;
        for (i = 0; i < 6; i++) {
            s_axesV[i].colour = c[i / 2];
            s_axesV[i].u = s_axesV[i].v = 0.0f;
        }
        for (i = 0; i < 3; i++) {
            s_axesLines[i].material = 0;
            s_axesLines[i].v[0] = &s_axesV[i * 2];
            s_axesLines[i].v[1] = &s_axesV[i * 2 + 1];
        }
    }
    for (i = 0; i < 3; i++)
        s_axesV[i * 2].pos.x = s_axesV[i * 2].pos.y = s_axesV[i * 2].pos.z = 0.0f;
    if ((*(const uint16_t *)&m[16] & 1) == 0) {
        for (i = 0; i < 3; i++) {
            s_axesV[i * 2 + 1].pos.x = m[i * 4 + 0];
            s_axesV[i * 2 + 1].pos.y = m[i * 4 + 1];
            s_axesV[i * 2 + 1].pos.z = m[i * 4 + 2];
        }
        origin.x = m[12]; origin.y = m[13]; origin.z = m[14];
    } else {
        const float *cm = zobjCameraGetMatrix(zrwCamGetUser(zrwCamGetCurrent()));
        for (i = 0; i < 3; i++) {
            s_axesV[i * 2 + 1].pos.x = cm[i * 4 + 0];
            s_axesV[i * 2 + 1].pos.y = cm[i * 4 + 1];
            s_axesV[i * 2 + 1].pos.z = cm[i * 4 + 2];
        }
        origin.x = 0.0f; origin.y = 0.0f; origin.z = 5.0f;
        flags = 0x01000000;   /* camera space */
    }
    for (i = 0; i < 6; i++) {
        zmathVecScale(&s_axesV[i].pos, &s_axesV[i].pos, 0.5f);
        zmathVecAdd(&s_axesV[i].pos, &s_axesV[i].pos, &origin);
    }
    flags |= zrwDbgPrimFlags(((const uint16_t *)&m[16])[1]);
    g_zglPrimApi.drawLines(flags, s_axesLines, 3, NULL);
}

/* VA 0x0047f230, 185 bytes, [I] range, status draft (shape 2: a triangle) */
void zrwDbgDrawTri(ZrwDbgShape *sh)
{
    const float *d = sh->data;
    int i;
    if (s_triInit) {
        s_triInit = 0;
        for (i = 0; i < 3; i++)
            s_triV[i].u = s_triV[i].v = 0.0f;
        s_tri.material = 0;
        for (i = 0; i < 3; i++)
            s_tri.v[i] = &s_triV[i];
    }
    for (i = 0; i < 3; i++) {
        s_triV[i].pos.x = d[i * 4 + 0];
        s_triV[i].pos.y = d[i * 4 + 1];
        s_triV[i].pos.z = d[i * 4 + 2];
        s_triV[i].colour = *(const RwRGBA *)&d[12];
    }
    g_zglPrimApi.drawTris(zrwDbgPrimFlags(*(const uint16_t *)&d[13]), &s_tri, 1, NULL);
}

/* VA 0x0047f2f0, 572 bytes, [I] range, status draft (shape 3: a wire sphere, 8 x 9 segments,
 * drawn as one 0x90-vertex polygon strip scaled by the radius) */
void zrwDbgDrawSphere(ZrwDbgShape *sh)
{
    const float *d = sh->data;
    RwMatrix m;
    RwV3d s;
    struct { uint32_t material; ZglVertex **v; int32_t count; } poly;
    int ring, seg, i;
    ZglVertex *v;

    if (s_sphereV == NULL) {
        s_sphereV = (ZglVertex *)zstringMemSet(zmemAlloc(0x90 * sizeof(ZglVertex)), 0, 0x90 * sizeof(ZglVertex));
        s_sphereP = (ZglVertex **)zmemAlloc(0x90 * sizeof(ZglVertex *));
        for (i = 0; i < 0x90; i++)
            s_sphereP[0x8F - i] = &s_sphereV[0x8F - i];
    }
    v = s_sphereV;
    for (ring = 0; ring < 8; ring++) {
        int a0 = ring * 0x1000, a1 = (ring + 1) * 0x1000;   /* 8 rings over half a turn [I] */
        float s0 = zmathSin(a0), s1 = zmathSin(a1), c0 = zmathCos(a0), c1 = zmathCos(a1);
        for (seg = 0; seg < 9; seg++, v += 2) {
            int b = seg * 0x2000;
            v[0].pos.x = zmathSin(b) * s0; v[0].pos.y = c0; v[0].pos.z = zmathCos(b) * s0;
            v[1].pos.x = zmathSin(b) * s1; v[1].pos.y = c1; v[1].pos.z = zmathCos(b) * s1;
            v[0].colour = v[1].colour = *(const RwRGBA *)&d[5];
        }
    }
    zmathMatrixIdentity(&m);
    s.x = s.y = s.z = d[4];
    zmathMatrixScale(&m, &m, &s);
    m.pos.x = d[0];
    m.pos.y = d[1];
    m.pos.z = d[2];
    poly.material = 0;
    poly.v = s_sphereP;
    poly.count = 0x90;
    g_zglPrimApi.drawStrips(zrwDbgPrimFlags(*(const uint16_t *)&d[6]), (const void *)&poly, 1, &m);
}

/* VA 0x0047f530, 262 bytes, [I] range, status draft (shape 4: a box min/max under a matrix, 12
 * edges) */
void zrwDbgDrawBox(ZrwDbgShape *sh)
{
    const float *d = sh->data;
    int i;
    if (s_boxInit) {
        s_boxInit = 0;
        for (i = 0; i < 8; i++)
            s_boxV[i].u = s_boxV[i].v = 0.0f;
        for (i = 0; i < 12; i++) {
            s_boxLines[i].material = 0;
            s_boxLines[i].v[0] = &s_boxV[s_boxEdges[i * 2]];
            s_boxLines[i].v[1] = &s_boxV[s_boxEdges[i * 2 + 1]];
        }
    }
    for (i = 0; i < 8; i++) {   /* corner i: bit 0 x, bit 1 y, bit 2 z (max when set) */
        s_boxV[i].pos.x = (i & 1) ? d[0] : d[4];
        s_boxV[i].pos.y = (i & 2) ? d[1] : d[5];
        s_boxV[i].pos.z = (i & 4) ? d[2] : d[6];
        s_boxV[i].colour = *(const RwRGBA *)&d[24];
    }
    g_zglPrimApi.drawLines(zrwDbgPrimFlags(sh->flags[0]), s_boxLines, 12, (const RwMatrix *)&d[8]);
}

/* VA 0x0047f640, 30 bytes, [I] callers, status draft (zrwWorld: on-screen text) */
int zrwDbgDrawText(const char *text, int x, int y)
{
    return RtCharsetPrint(s_charset, text, x, y);
}

/* VA 0x0047f660, 412 bytes, [I] new function, status draft (ZGL callback list 4: after the frame)
 * Stats line, text callbacks, the queued text (y < 0: rows below the previous line), then every
 * queued shape. */
int zrwDbgRender(void)
{
    if (s_charset != NULL) {
        uint8_t *p = s_text;
        int lastY = 0;
        if (s_statsMode) {
            zrwDbgPrintStatsLine();
            if (s_statsMode > 1)
                zrwZglRunCallbackList(&s_textCallbacks);
            s_statsMode = 0;
        }
        while (p < s_textEnd) {
            int16_t x = *(int16_t *)p, y = *(int16_t *)(p + 2);
            char *t = (char *)(p + 4);
            int yy = y;
            if (yy < 0)
                yy = lastY - (yy + 1) * s_charH;
            if (yy < 1)
                yy = 0;
            lastY = yy + s_charH;
            RtCharsetPrint(s_charset, t, x > 0 ? x : 0, yy);
            while (*t != '\0')
                t++;
            p = (uint8_t *)(((size_t)t + 2) & ~(size_t)1);
        }
        s_textEnd = s_text;
    }
    zrwZglResetRenderState();
    {
        int i;
        for (i = 0; i < s_numShapes; i++) {
            switch (s_shapes[i].type) {
            case 0: zrwDbgDrawAxes(&s_shapes[i]); break;
            case 1: zrwDbgDrawLine(&s_shapes[i]); break;
            case 2: zrwDbgDrawTri(&s_shapes[i]); break;
            case 3: zrwDbgDrawSphere(&s_shapes[i]); break;
            case 4: zrwDbgDrawBox(&s_shapes[i]); break;
            default: g_pfnDebugPrintf("zrwDbg: Unkown geometry type"); break;
            }
        }
    }
    s_numShapes = 0;
    return 1;
}

/* VA 0x0047f810, 24 bytes, [I] new function (follows a jump table), status draft (App_Reopen) */
void zrwDbgCreateCharset(void)
{
    s_charset = RtCharsetCreate(&g_zrwTextColour, &g_zrwClearColour);
}

/* VA 0x0047f830, 29 bytes, [I] new function, status draft (App_Reopen) */
void zrwDbgDestroyCharset(void)
{
    if (s_charset != NULL)
        RtCharsetDestroy(s_charset);
    s_charset = NULL;
}

/* VA 0x0047f850, 174 bytes, [I] row, status draft (debug slot 0) */
int zrwDbgInit(void)
{
    int32_t desc[2];
    s_initialized = 1;
    zListInit(&s_textCallbacks);
    s_charset = RtCharsetCreate(&g_zrwTextColour, &g_zrwClearColour);
    if (s_charset == NULL) {
        zAssertFail("Cannot create raster chraset.", ZRWDBG_SRC, 0x33B, 0);
        return 0;
    }
    RtCharsetGetDesc(s_charset, desc);
    s_charW = desc[0];
    s_charH = desc[1];
    s_renderCallback = zrwZglAddCallback(4, (void *)zrwDbgRender, NULL, 1);
    s_textEnd = s_text;
    s_numShapes = 0;
    s_indent = 0;
    return 1;
}

/* VA 0x0047f900, 63 bytes, [I] new function, status draft (slot 1) */
int zrwDbgShutdown(void)
{
    if (s_renderCallback != NULL)
        zrwZglRemoveCallback(s_renderCallback);
    if (s_charset != NULL)
        RtCharsetDestroy(s_charset);
    s_charset = NULL;
    s_initialized = 0;
    return 1;
}

/* VA 0x0047f940, 154 bytes, [I] row, status draft (slot 2)
 * Queues on-screen text at (x, y) pixels; y < 0 = |y| rows below the previous text. */
int zdebugPrint(int16_t x, int16_t y, const char *fmt, ...)
{
    int n;
    zstringVPrintf(s_fmt, fmt, (void *)(&fmt + 1));
    n = zstringLength(s_fmt);
    if ((size_t)(s_textEnd + n + 6) >= (size_t)(s_text + sizeof(s_text))) {
        g_pfnDebugPrintf("zdebugPrint: Out of buffer space printing:'%s'\n", fmt);
        return 0;
    }
    *(int16_t *)s_textEnd = x;
    *(int16_t *)(s_textEnd + 2) = y;
    s_textEnd += 4;
    zstringCopy((char *)s_textEnd, s_fmt);
    s_textEnd = (uint8_t *)(((size_t)s_textEnd + n + 2) & ~(size_t)1);
    return n;
}

/* VA 0x0047f9e0, 73 bytes, [I] new function, status draft (slot 3) */
void zrwDbgPrintFloat(int x, int y, const char *label, float f)
{
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "a:% 10.4f", (double)f);
    else
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f", label, (double)f);
}

/* VA 0x0047fa30, 263 bytes, [I] range, status draft (slot 4: a matrix, one column per row) */
void zrwDbgPrintMatrix(int x, int y, const char *label, const float *m)
{
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "% 10.4f,% 10.4f,% 10.4f", (double)m[0], (double)m[4], (double)m[8]);
    else {
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f,% 10.4f,% 10.4f", label, (double)m[0], (double)m[4],
                    (double)m[8]);
        x += s_charW * zstringLength(label);
    }
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[1], (double)m[5], (double)m[9]);
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[2], (double)m[6], (double)m[10]);
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[12], (double)m[13], (double)m[14]);
}

/* VA 0x0047fb40, 130 bytes, [I] new function, status draft (slot 5: 4 floats) */
void zrwDbgPrintVec4(int x, int y, const char *label, const float *v)
{
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "% 10.4f,% 10.4f,% 10.4f,% 10.4f", (double)v[0], (double)v[1],
                    (double)v[2], (double)v[3]);
    else
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f,% 10.4f,% 10.4f,% 10.4f", label, (double)v[0],
                    (double)v[1], (double)v[2], (double)v[3]);
}

/* VA 0x0047fbd0, 110 bytes, [I] new function, status draft (slot 6: 3 floats) */
void zrwDbgPrintVec3(int x, int y, const char *label, const float *v)
{
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "% 10.4f,% 10.4f,% 10.4f", (double)v[0], (double)v[1], (double)v[2]);
    else
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f,% 10.4f,% 10.4f", label, (double)v[0], (double)v[1],
                    (double)v[2]);
}

/* VA 0x0047fc40, 102 bytes, [I] new function, status draft (slot 8: a 16-bit angle in degrees) */
void zrwDbgPrintAngle(int x, int y, const char *label, int16_t a)
{
    double deg = (double)((float)a * 0.005493164f);
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "a:% 10.4f", deg);
    else
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f", label, deg);
}

/* VA 0x0047fcb0, 216 bytes, [I] range, status draft (slot 7: three 16-bit angles in degrees) */
void zrwDbgPrintEuler(int x, int y, const char *label, const int16_t *a)
{
    double d0 = a[0] * 0.005493164, d1 = a[1] * 0.005493164, d2 = a[2] * 0.005493164;
    if (label == NULL)
        zdebugPrint((int16_t)x, (int16_t)y, "a:% 10.4f,% 10.4f,% 10.4f", d0, d1, d2);
    else
        zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f,% 10.4f,% 10.4f", label, d0, d1, d2);
}

/* VA 0x0047fd90, 382 bytes, [I] new function, status draft (no caller found: the matrix printer
 * with the first row at the given position and the rest at x = 0 when unlabelled) */
void zrwDbgPrintMatrixAt(int x, int y, const char *label, const float *m)
{
    if (label == NULL) {
        zdebugPrint(0, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[0], (double)m[4], (double)m[8]);
        zdebugPrint(0, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[1], (double)m[5], (double)m[9]);
        zdebugPrint(0, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[2], (double)m[6], (double)m[10]);
        zdebugPrint(0, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[12], (double)m[13], (double)m[14]);
        return;
    }
    zdebugPrint((int16_t)x, (int16_t)y, "%s% 10.4f,% 10.4f,% 10.4f", label, (double)m[0], (double)m[4],
                (double)m[8]);
    x += s_charW * zstringLength(label);
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[1], (double)m[5], (double)m[9]);
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[2], (double)m[6], (double)m[10]);
    zdebugPrint((int16_t)x, -1, "% 10.4f,% 10.4f,% 10.4f", (double)m[12], (double)m[13], (double)m[14]);
}

/* VA 0x0047ff10, 110 bytes, [I] new function, status draft (no caller found: copy of slot 6) */
void zrwDbgPrintVec3b(int x, int y, const char *label, const float *v)
{
    zrwDbgPrintVec3(x, y, label, v);
}

/* VA 0x0047ff80, 12 bytes, [I] new function, status draft (slot 9: stats for this frame) */
void zrwDbgShowStats(int8_t mode)
{
    s_statsMode = mode + 1;
}

static ZrwDbgShape *zrwDbgNewShape(uint32_t type)
{
    ZrwDbgShape *s;
    if (s_numShapes == ZRWDBG_MAX_SHAPES) {
        g_pfnDebugPrintf("ZrwDbg: Out of buffer space\n");
        return NULL;
    }
    s = &s_shapes[s_numShapes++];
    s->type = type;
    return s;
}

/* VA 0x0047ff90, 116 bytes, [I] range, status draft (slot 13) */
void zrwDbgAddLine(const RwV3d *a, const RwV3d *b, const RwRGBA *colour, uint16_t flags)
{
    ZrwDbgShape *s = zrwDbgNewShape(1);
    if (s == NULL)
        return;
    s->data[0] = a->x; s->data[1] = a->y; s->data[2] = a->z;
    s->data[4] = b->x; s->data[5] = b->y; s->data[6] = b->z;
    *(RwRGBA *)&s->data[8] = *colour;
    *(uint16_t *)&s->data[9] = flags;
}

/* VA 0x00480010, 95 bytes, [I] new function, status draft (slot 12) */
void zrwDbgAddAxes(const RwMatrix *m, uint16_t flags0, uint16_t flags1)
{
    ZrwDbgShape *s = zrwDbgNewShape(0);
    if (s == NULL)
        return;
    ((uint16_t *)&s->data[16])[0] = flags0;
    ((uint16_t *)&s->data[16])[1] = flags1;
    if (m != NULL)
        zmathMatrixCopy((RwMatrix *)s->data, m);
}

/* VA 0x00480070, 137 bytes, [I] range, status draft (slot 14) */
void zrwDbgAddTri(const RwV3d *a, const RwV3d *b, const RwV3d *c, const RwRGBA *colour, uint16_t flags)
{
    ZrwDbgShape *s = zrwDbgNewShape(2);
    if (s == NULL)
        return;
    s->data[0] = a->x; s->data[1] = a->y; s->data[2] = a->z;
    s->data[4] = b->x; s->data[5] = b->y; s->data[6] = b->z;
    s->data[8] = c->x; s->data[9] = c->y; s->data[10] = c->z;
    *(RwRGBA *)&s->data[12] = *colour;
    *(uint16_t *)&s->data[13] = flags;
}

/* VA 0x00480100, 102 bytes, [I] range, status draft (slot 15) */
void zrwDbgAddSphere(const RwV3d *c, float radius, const RwRGBA *colour, uint16_t flags)
{
    ZrwDbgShape *s = zrwDbgNewShape(3);
    if (s == NULL)
        return;
    s->data[0] = c->x; s->data[1] = c->y; s->data[2] = c->z;
    s->data[4] = radius;
    *(RwRGBA *)&s->data[5] = *colour;
    *(uint16_t *)&s->data[6] = flags;
}

/* VA 0x00480170, 123 bytes, [I] new function, status draft (slot 16: box { max, min } + matrix) */
void zrwDbgAddBox(const float *box, const RwMatrix *m, const RwRGBA *colour, uint16_t flags)
{
    ZrwDbgShape *s = zrwDbgNewShape(4);
    int i;
    if (s == NULL)
        return;
    for (i = 0; i < 8; i++)
        s->data[i] = box[i];
    if (m == NULL)
        zmathMatrixIdentity((RwMatrix *)&s->data[8]);
    else
        *(RwMatrix *)&s->data[8] = *m;
    *(RwRGBA *)&s->data[24] = *colour;
    s->flags[0] = flags;
}

/* VA 0x004801f0, 60 bytes, [V] direct, status draft (slot 10: text callbacks, stats mode 2) */
void *zrwDbgAddTextCallback(int unused, void *fn, void *arg)
{
    (void)unused;
    if (!s_initialized)
        zAssertFail("Not initialized", ZRWDBG_SRC, 0x513, 0);
    return zrwZglAddCallbackToList(&s_textCallbacks, fn, arg, 0);
}

/* VA 0x00480230, 14 bytes, [I] new function, status draft (slot 11) */
void zrwDbgRemoveTextCallback(void *node)
{
    zrwZglRemoveCallback(node);
}

/* VA 0x00480240, 71 bytes, [I] range, status draft (slot 17: g_pfnDebugPrintf) */
int zrwDbgPrintf(const char *fmt, ...)
{
    int i;
    for (i = 0; i < s_indent; i++)
        s_fmt[i] = ' ';
    zstringVPrintf(s_fmt + s_indent, fmt, (void *)(&fmt + 1));
    zrwZglDebugString(s_fmt);
    return 0;
}

/* VA 0x00480290, 40 bytes, [I] new function, status draft (slot 19) */
void zrwDbgSetIndent(int n)
{
    s_indent = n < 0 ? 0 : (n > 0x20 ? 0x20 : n);
}

/* VA 0x004802c0, 45 bytes, unassigned, status draft (slot 18: g_pfnDebugIndent) */
void zrwDbgIndent(int delta)
{
    s_indent += delta;
    if (s_indent < 0)
        s_indent = 0;
    else if (s_indent > 0x20)
        s_indent = 0x20;
}

/* The debug driver table 0x00560030 (zengine.h names the slots g_pfnDebug*) */
void *const g_zrwDbgDriver[20] = {
    (void *)zrwDbgInit, (void *)zrwDbgShutdown, (void *)zdebugPrint, (void *)zrwDbgPrintFloat,
    (void *)zrwDbgPrintMatrix, (void *)zrwDbgPrintVec4, (void *)zrwDbgPrintVec3,
    (void *)zrwDbgPrintEuler, (void *)zrwDbgPrintAngle, (void *)zrwDbgShowStats,
    (void *)zrwDbgAddTextCallback, (void *)zrwDbgRemoveTextCallback, (void *)zrwDbgAddAxes,
    (void *)zrwDbgAddLine, (void *)zrwDbgAddTri, (void *)zrwDbgAddSphere, (void *)zrwDbgAddBox,
    (void *)zrwDbgPrintf, (void *)zrwDbgIndent, (void *)zrwDbgSetIndent
};

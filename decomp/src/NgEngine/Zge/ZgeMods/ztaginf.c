/*
 * NgEngine/Zge/ZgeMods/ztaginf.c
 * Original: C:\NgEngine\Zge\ZgeMods\ztaginf.c
 * Evidence: [V] path string at 0x005617dc (asserts in ztaginfLoad); the rest [I]: they share the
 * globals 0x006728B8-0x006737BB (root node, face array, hit buffers, node pool). DM-043 moved
 * 0x0048D6F0-0x0048D810 (Init/Shutdown/SetDebugDraw) here from the zrwPrim/ztaginf gap.
 * 28 functions, VA 0x0048d6f0-0x0048f3b1.
 * DM-043 (draft). Non-matching C (AD-8). Types: decomp/include/ztaginf.h.
 * Format: docs/formats/LEVEL.md 6; Python reference tools/dmbmx/formats/level.py TagSd (agrees; notes inline).
 *
 * Positions and faces are in ZGE units. Users: NgIFzbsp.c (face tags under the rider:
 * ztaginfFindNearestFace, ztaginfGetFaces/GetFaceCount) and NgIFzcollide.c (ztaginfFindEdges for
 * grind edges). FindNearestEdge/FindNearestVertex/FindFaces/FindVertices and the debug draw switches
 * have no caller in the exe [V: no call, pointer or thunk].
 */
#include "decomp.h"

/* other modules (current names until their cards land) */
void    *zrwZglAddCallback(int list, int (__cdecl *fn)(void), void *arg, int on); /* add render callback */
void     zrwZglRemoveCallback(void *task);                                   /* remove render callback */
int      _finite(double x);                                             /* CRT 0x004A60D9 */
double   fabs(double x);                                                /* CRT (inline x87 FABS) */
double   sqrt(double x);                                                /* CRT (inline x87 FSQRT) */
/* zmath.c vector helpers (ZtagVec / float[3]) [I] from their bodies */
void     zmathVecSub(float *out, const void *a, const void *b);      /* out = a - b */
float    zmathVecNormalize(float *out, const float *v);                 /* out = 0.9999 * v / |v|, returns |v| */
void     zmathVecScale(float *out, const void *v, float s);            /* out = v * s */
float    zmathVecLengthSq(const float *v);                                /* |v|^2 */
float    zmathVecLength(const float *v);                                /* |v| */
float    zmathVecDot(const float *a, const void *b);                 /* dot(a, b) */
void     zmathTriangleNormal(float *out, const void *a, const void *b, const float *p); /* normal of (a, b, p) */
/* debug draw table of zrwDbg.c */
extern void (__cdecl *g_pfnDbgDrawLine)(const float *a, const float *b, const RwRGBA *c, int f);   /* 0x00560064 */
extern void (__cdecl *g_pfnDbgDrawTriangle)(const float *a, const float *b, const float *c,
                                             const RwRGBA *col, int f);                             /* 0x00560068 */
extern void (__cdecl *g_pfnDbgDrawSphere)(const float *centre, float radius, const RwRGBA *c, int f); /* 0x0056006C */

#define ZTAGINF_FILE "C:\\NgEngine\\Zge\\ZgeMods\\ztaginf.c"
#define ID_TAGD 0x44474154u   /* 'TAGD' */
#define ID_TAGI 0x49474154u   /* 'TAGI' */
#define ID_TAGF 0x46474154u   /* 'TAGF' */
#define ID_TAGN 0x4E474154u   /* 'TAGN' */
#define ZTAG_EPS 0.01f

static uint32_t    g_ztagDebugFlags;   /* 0x006728B8 bit 0 faces, 1 edges, 2 vertices */
static uint32_t    g_ztagFaceCount;    /* 0x006728BC */
static ZtagFace   *g_ztagFaces;        /* 0x006728C0 g_ztagFaceCount * 0x6C bytes */
static ZtagNode    g_ztagRoot;         /* 0x006728C4 */
static uint8_t    *g_ztagData;         /* 0x006728FC TAGI blob */
static void       *g_ztagDebugTask;    /* 0x00672900 render callback handle (also "debug draw on") */
static ZtagHit     g_ztagHits[ZTAG_MAX_HITS]; /* 0x00672908 */
static ZtagHit     g_ztagFaceHit;      /* 0x00673708 */
static ZtagHit     g_ztagVertexHit;    /* 0x00673740 */
static ZtagHit     g_ztagEdgeHit;      /* 0x00673778 */
static ZtagHitList g_ztagHitList;      /* 0x006737B0 */
static void       *g_ztagNodePool;     /* 0x006737B8 */

static RwRGBA rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    RwRGBA c;
    c.red = r; c.green = g; c.blue = b; c.alpha = a;
    return c;
}

static void copyVec(ZtagVec *dst, const ZtagVec *src)
{
    dst->x = src->x; dst->y = src->y; dst->z = src->z;   /* w is not copied */
}

/* VA 0x0048d6f0, 72 bytes, [I] row, status draft */
int ztaginfInit(void)
{
    g_ztagRoot.kind = ZTAG_NODE_LEAF;
    g_ztagDebugFlags = 0;
    g_ztagFaces = 0;
    g_ztagData = 0;
    g_ztagDebugTask = 0;
    g_ztagRoot.high = 0;
    g_ztagRoot.low = 0;
    g_ztagRoot.count = 0;
    g_ztagNodePool = zmemFreeListCreate(sizeof(ZtagNode), 0x100, 0);
    return 1;
}

/* VA 0x0048d740, 193 bytes, [I] row, status draft */
int ztaginfShutdown(void)
{
    ztaginfUnload();           /* inlined in the exe */
    zmemFreeListDestroy(g_ztagNodePool);
    g_ztagNodePool = 0;
    return 1;
}

/* VA 0x0048d810, 78 bytes, [I] row, status draft */
void ztaginfSetDebugDraw(int on, uint32_t flags)
{
    g_ztagDebugFlags = flags;
    if (on == 1) {
        if (!g_ztagDebugTask)
            g_ztagDebugTask = zrwZglAddCallback(2, ztaginfDebugRender, 0, 1);
    } else if (g_ztagDebugTask) {
        zrwZglRemoveCallback(g_ztagDebugTask);
        g_ztagDebugTask = 0;
    }
}

/* VA 0x0048d860, 107 bytes, [I] row, status draft */
void ztaginfFreeNode(ZtagNode *node)
{
    /* frees the node's face list and its subtrees (not the node itself) */
    if (node->faces) {
        zmemFree(node->faces);
        node->faces = 0;
    }
    if (node->low) {
        ztaginfFreeNode(node->low);
        zmemFreeListFree(g_ztagNodePool, node->low);
        node->low = 0;
    }
    if (node->high) {
        ztaginfFreeNode(node->high);
        zmemFreeListFree(g_ztagNodePool, node->high);
        node->high = 0;
    }
}

/* VA 0x0048d8d0, 167 bytes, [I] row, status draft */
void ztaginfUnload(void)
{
    /* called by zsceneClose (and inlined at the start of ztaginfLoad) */
    if (g_ztagRoot.faces) {
        zmemFree(g_ztagRoot.faces);
        g_ztagRoot.faces = 0;
    }
    if (g_ztagRoot.low) {
        ztaginfFreeNode(g_ztagRoot.low);
        zmemFreeListFree(g_ztagNodePool, g_ztagRoot.low);
        g_ztagRoot.low = 0;
    }
    if (g_ztagRoot.high) {
        ztaginfFreeNode(g_ztagRoot.high);
        zmemFreeListFree(g_ztagNodePool, g_ztagRoot.high);
        g_ztagRoot.high = 0;
    }
    if (g_ztagFaces) {
        zmemFree(g_ztagFaces);
        g_ztagFaces = 0;
        g_ztagFaceCount = 0;
    }
    if (g_ztagData) {
        zmemFree(g_ztagData);
        g_ztagData = 0;
    }
}

/* VA 0x0048d980, 6 bytes, [I] row, status draft */
ZtagFace *ztaginfGetFaces(void)
{
    return g_ztagFaces;
}

/* VA 0x0048d990, 6 bytes, [I] row, status draft */
uint32_t ztaginfGetFaceCount(void)
{
    return g_ztagFaceCount;
}

/* VA 0x0048d9a0, 319 bytes, [I] row, status draft */
int ztaginfLoadNode(ZtagNode *node, ZiffFile *iff)
{
    /*
     * Reads the current TAGN chunk into node, then (for split nodes) the next two TAGN chunks as
     * its low and high subtrees, recursively: the file is a pre-order dump (LEVEL.md 6.3).
     * Nothing checks that the next chunk exists or that the record size is 0x28.
     */
    ZtagTagnRecord rec;

    ziffChunkReadData(iff, &rec, sizeof(rec));
    node->kind = rec.kind;
    node->boundB[0] = rec.boundB[0];
    node->split = rec.split;
    node->boundB[2] = rec.boundB[2];
    node->boundB[1] = rec.boundB[1];
    node->boundA[1] = rec.boundA[1];
    node->boundA[0] = rec.boundA[0];
    node->count = (uint16_t)rec.count;           /* only the low 16 bits are kept */
    node->boundA[2] = rec.boundA[2];
    if (node->count == 0) {
        node->faces = 0;
    } else {
        uint32_t n = (uint32_t)node->count * 4;
        uint32_t *index = (uint32_t *)zmemAlloc(n);
        int i;

        ziffChunkReadData(iff, index, n);
        node->faces = (ZtagFace **)zmemAlloc((uint32_t)node->count * 4);
        for (i = 0; i < (int)node->count; i++)
            node->faces[i] = &g_ztagFaces[index[i]];   /* no range check against the face count */
        zmemFree(index);
    }
    if (node->kind == ZTAG_NODE_LEAF) {
        node->high = 0;
        node->low = 0;
        return 1;
    }
    node->low = (ZtagNode *)zmemFreeListAlloc(g_ztagNodePool);
    ziffChunkFindNext(iff, ID_TAGN);
    ztaginfLoadNode(node->low, iff);
    node->high = (ZtagNode *)zmemFreeListAlloc(g_ztagNodePool);
    ziffChunkFindNext(iff, ID_TAGN);
    ztaginfLoadNode(node->high, iff);
    return 1;
}

static uint8_t *tagPtr(int32_t off)
{
    return off == -1 ? 0 : g_ztagData + off;
}

/* VA 0x0048dae0, 1018 bytes, [V] direct, status draft */
int ztaginfLoad(ZiffFile *iff)
{
    ZtagTagdRecord tagd;
    ZtagTagfRecord rec;
    uint32_t dataSize;
    ZtagFace *f;

    ztaginfUnload();                                 /* inlined in the exe */
    if (!ziffChunkOpen(iff))
        return 1;
    if (!ziffChunkFindFirst(iff, ID_TAGD)) {
        zAssertFail("Tag data not found", ZTAGINF_FILE, 0x157, 0);
        return 0;
    }
    ziffChunkReadData(iff, &tagd, sizeof(tagd));
    if (tagd.size != sizeof(tagd)) {
        zAssertFail("Chunk sync error", ZTAGINF_FILE, 0x15D, 0);
        return 0;
    }
    g_ztagFaceCount = tagd.faceCount;
    if (g_ztagFaceCount != 0) {
        if (ziffChunkFindFirst(iff, ID_TAGI)) {
            /* TAGI: u32 8; u32 dataSize; blob. The blob length is taken from the chunk size, not from
             * dataSize (which is 1 less in the corpus when the last record is 3 bytes, LEVEL.md 6.2). */
            ziffChunkReadData(iff, &tagd, sizeof(tagd));
            dataSize = ziffChunkGetSize(iff) - 8;
            if (dataSize != 0) {
                g_ztagData = (uint8_t *)zmemAlloc(dataSize);
                ziffChunkReadData(iff, g_ztagData, dataSize);
            }
        }
        g_ztagFaces = (ZtagFace *)zmemAlloc(g_ztagFaceCount * sizeof(ZtagFace));
        if (ziffChunkFindFirst(iff, ID_TAGF)) {
            /* every TAGF chunk is read: more TAGF chunks than faceCount overrun the array */
            f = g_ztagFaces;
            do {
                ziffChunkReadData(iff, &rec, sizeof(rec));
                if (rec.size != sizeof(rec))
                    zAssertFail("ztaginf : Tagging info face structure mismatch", ZTAGINF_FILE, 0x17A, 0);
                f->normal.x = rec.normal[0];
                f->normal.y = rec.normal[1];
                f->normal.z = rec.normal[2];
                f->v[0].x = rec.v0[0]; f->v[0].y = rec.v0[1]; f->v[0].z = rec.v0[2];
                f->v[1].x = rec.v1[0]; f->v[1].y = rec.v1[1]; f->v[1].z = rec.v1[2];
                f->v[2].x = rec.v2[0]; f->v[2].y = rec.v2[1]; f->v[2].z = rec.v2[2];
                f->edgeTag[0] = tagPtr(rec.edgeTag[0]);
                f->edgeTag[1] = tagPtr(rec.edgeTag[1]);
                f->edgeTag[2] = tagPtr(rec.edgeTag[2]);
                f->vertexTag[0] = tagPtr(rec.vertexTag[0]);
                f->vertexTag[1] = tagPtr(rec.vertexTag[1]);
                f->vertexTag[2] = tagPtr(rec.vertexTag[2]);
                f->faceTag = tagPtr(rec.faceTag);
                f->runtime[0] = 0;
                f->runtime[1] = 0;
                f->runtime[2] = 0;
                f->runtime[3] = 0;
                f++;
            } while (ziffChunkFindNext(iff, ID_TAGF));
        }
    }
    /* the tree is read even without faces (then every leaf is empty) */
    if (ziffChunkFindFirst(iff, ID_TAGN)) {
        ztaginfLoadNode(&g_ztagRoot, iff);
        if (ziffChunkFindNext(iff, ID_TAGN))
            zAssertFail("ztaginf : Extra nodes in tree???", ZTAGINF_FILE, 0x1A0, 0);
    }
    ziffChunkClose(iff);
    return 1;
}

/* VA 0x0048dee0, 10 bytes, [I] row, status draft */
void ztaginfSetDebugFlags(uint32_t flags)
{
    g_ztagDebugFlags = flags;
}

/* VA 0x0048def0, 460 bytes, [I] row, status draft */
void ztaginfDebugDrawNode(const ZtagNode *node, const RwRGBA *colour)
{
    /* the 12 edges of the node's box; corners combine boundA/boundB per axis */
    const float *a = node->boundA, *b = node->boundB;
    float c[8][3];
    int i;

    for (i = 0; i < 8; i++) {
        c[i][0] = (i & 4) ? b[0] : a[0];
        c[i][1] = (i & 2) ? b[1] : a[1];
        c[i][2] = (i & 1) ? b[2] : a[2];
    }
    if (!colour)
        return;
    /* face x = a, face x = b, then the four edges along x (same order as the exe) */
    g_pfnDbgDrawLine(c[0], c[1], colour, 0);
    g_pfnDbgDrawLine(c[1], c[3], colour, 0);
    g_pfnDbgDrawLine(c[2], c[3], colour, 0);
    g_pfnDbgDrawLine(c[2], c[0], colour, 0);
    g_pfnDbgDrawLine(c[4], c[5], colour, 0);
    g_pfnDbgDrawLine(c[5], c[7], colour, 0);
    g_pfnDbgDrawLine(c[6], c[7], colour, 0);
    g_pfnDbgDrawLine(c[6], c[4], colour, 0);
    g_pfnDbgDrawLine(c[0], c[4], colour, 0);
    g_pfnDbgDrawLine(c[1], c[5], colour, 0);
    g_pfnDbgDrawLine(c[2], c[6], colour, 0);
    g_pfnDbgDrawLine(c[3], c[7], colour, 0);
}

/* VA 0x0048e0c0, 484 bytes, [I] row, status draft */
void ztaginfDebugDrawFace(const ZtagFace *face, const RwRGBA *vertexColour, const RwRGBA *edgeColour,
                          const RwRGBA *faceColour)
{
    float p[3][3];
    int i;

    /* the face lifted 10 units along its normal */
    for (i = 0; i < 3; i++) {
        p[i][0] = face->v[i].x + face->normal.x * 10.0f;
        p[i][1] = face->v[i].y + face->normal.y * 10.0f;
        p[i][2] = face->v[i].z + face->normal.z * 10.0f;
    }
    if (faceColour) {
        if (face->faceTag)
            g_pfnDbgDrawTriangle(p[2], p[1], p[0], faceColour, 2);   /* tagged faces: both sides */
        g_pfnDbgDrawTriangle(p[0], p[1], p[2], faceColour, 2);
    }
    if (edgeColour) {
        if (face->edgeTag[0])
            g_pfnDbgDrawLine(p[0], p[1], edgeColour, 0);
        if (face->edgeTag[1])
            g_pfnDbgDrawLine(p[1], p[2], edgeColour, 0);
        if (face->edgeTag[2])
            g_pfnDbgDrawLine(p[2], p[0], edgeColour, 0);
    }
    if (vertexColour) {
        /* vertices one unit *below* the face (v - normal) */
        for (i = 0; i < 3; i++) {
            p[i][0] = face->v[i].x - face->normal.x;
            p[i][1] = face->v[i].y - face->normal.y;
            p[i][2] = face->v[i].z - face->normal.z;
        }
        for (i = 0; i < 3; i++) {
            if (face->vertexTag[i])
                g_pfnDbgDrawSphere(p[i], 0.5f, vertexColour, 0);
        }
    }
}

/* VA 0x0048e2b0, 146 bytes, [I] row, status draft */
void ztaginfDebugDrawEdge(const ZtagFace *face, int edge, const RwRGBA *colour)
{
    float a[3], b[3];
    const ZtagVec *v0 = &face->v[edge & 0xFF];
    const ZtagVec *v1 = &face->v[((edge & 0xFF) + 1) % 3];

    /* note: offset by the normal itself (x1), not x10 as in DebugDrawFace */
    a[0] = face->normal.x + v0->x; a[1] = v0->y + face->normal.y; a[2] = v0->z + face->normal.z;
    b[0] = face->normal.x + v1->x; b[1] = v1->y + face->normal.y; b[2] = v1->z + face->normal.z;
    if (colour && face->edgeTag[edge & 0xFF])
        g_pfnDbgDrawLine(a, b, colour, 2);
}

/* VA 0x0048e350, 170 bytes, [I] row, status draft */
int ztaginfDebugRender(void)
{
    RwRGBA faceColour = rgba(0x00, 0x00, 0xFF, 0x80);
    RwRGBA edgeColour = rgba(0xFF, 0x00, 0x00, 0xFF);
    RwRGBA vertexColour = rgba(0x00, 0xFF, 0x00, 0xFF);
    uint32_t i;
    ZtagFace *f;

    if (g_ztagDebugFlags) {
        for (i = 0, f = g_ztagFaces; i < g_ztagFaceCount; i++, f++)
            ztaginfDebugDrawFace(f, (g_ztagDebugFlags & 4) ? &vertexColour : 0,
                                 (g_ztagDebugFlags & 2) ? &edgeColour : 0,
                                 (g_ztagDebugFlags & 1) ? &faceColour : 0);
    }
    return 1;
}

/* VA 0x0048e400, 166 bytes, [I] row, status draft */
ZtagNode *ztaginfFindLeaf(const float *pos)
{
    /* LEVEL.md 6.3: x and z go low when below the split, y goes low when above it (ZGE y is down) */
    ZtagNode *node = &g_ztagRoot;

    while (node->kind != ZTAG_NODE_LEAF) {
        if (node->kind == ZTAG_NODE_X)
            node = (pos[0] < node->split) ? node->low : node->high;
        else if (node->kind == ZTAG_NODE_Y)
            node = (node->split < pos[1]) ? node->low : node->high;
        else if (node->kind == ZTAG_NODE_Z)
            node = (pos[2] < node->split) ? node->low : node->high;
        /* any other kind: stays on the same node and loops forever (not in the corpus) */
        if (!node)
            return 0;
    }
    if (g_ztagDebugTask) {
        RwRGBA red = rgba(0xFF, 0x00, 0x00, 0xFF);
        ztaginfDebugDrawNode(node, &red);
    }
    return node;
}

/* VA 0x0048e4b0, 407 bytes, [I] row, status draft */
int ztaginfPointInFace(const ZtagVec *a, const ZtagVec *b, const ZtagVec *c, const ZtagVec *normal,
                       const float *p)
{
    /* p is inside the triangle when the normals of (a,b,p), (b,c,p), (c,a,p) all equal the face
     * normal within 0.01 per component */
    const ZtagVec *e0[3], *e1[3];
    float n[3];
    int i;

    e0[0] = a; e1[0] = b;
    e0[1] = b; e1[1] = c;
    e0[2] = c; e1[2] = a;
    for (i = 0; i < 3; i++) {
        zmathTriangleNormal(n, e0[i], e1[i], p);
        zmathVecNormalize(n, n);
        if (fabs(n[0] - normal->x) >= ZTAG_EPS || fabs(n[1] - normal->y) >= ZTAG_EPS ||
            fabs(n[2] - normal->z) >= ZTAG_EPS)
            return 0;
    }
    return 1;
}

/* VA 0x0048e650, 77 bytes, [I] row, status draft */
int ztaginfProjectInFace(const ZtagFace *face, const float *pos, float dist)
{
    /* pos moved dist along -normal (onto the face plane when dist is the plane distance) */
    float d[4], p[4];

    zmathVecScale(d, &face->normal, dist);
    zmathVecSub(p, pos, d);
    return ztaginfPointInFace(&face->v[0], &face->v[1], &face->v[2], &face->normal, p);
}

/* VA 0x0048e6a0, 48 bytes, [I] row, status draft */
float ztaginfPlaneDistance(const ZtagFace *face, const float *pos)
{
    float d[4];

    zmathVecSub(d, pos, &face->v[0]);
    return (float)fabs(zmathVecDot(d, &face->normal));
}

/* VA 0x0048e6d0, 166 bytes, [I] row, status draft */
int ztaginfEdgeContains(const ZtagFace *face, const float *pos, int edge)
{
    /* cos of the angle between (pos - v[edge]) and the edge, both normalised; the test
     * 0 <= |cos| <= 1 is always true for finite input, so this returns 1 unless a vector is NaN */
    float d[4], e[4];
    float c;

    zmathVecSub(d, pos, &face->v[edge]);
    zmathVecSub(e, &face->v[(edge + 1) % 3], &face->v[edge]);
    zmathVecNormalize(e, e);
    zmathVecNormalize(d, d);
    c = zmathVecDot(d, e);
    return (0.0f <= fabs(c) && fabs(c) <= 1.0f);
}

/* VA 0x0048e780, 134 bytes, [I] row, status draft */
float ztaginfEdgeDistance(const ZtagFace *face, const float *pos, int edge)
{
    /* distance from pos to the infinite line through the edge */
    float d[4], e[4];
    float along;

    zmathVecSub(d, pos, &face->v[edge]);
    zmathVecSub(e, &face->v[(edge + 1) % 3], &face->v[edge]);
    zmathVecNormalize(e, e);
    along = zmathVecDot(d, e);
    return (float)sqrt(zmathVecLengthSq(d) - along * along);
}

/* VA 0x0048e810, 48 bytes, [I] row, status draft */
float ztaginfVertexDistance(const ZtagFace *face, const float *pos, int vertex)
{
    float d[4];

    zmathVecSub(d, pos, &face->v[vertex]);
    return (float)fabs(zmathVecLength(d));
}

/* VA 0x0048e840, 545 bytes, [I] row, status draft */
ZtagHit *ztaginfFindNearestFace(const float *pos, float maxDist)
{
    /*
     * The tagged face (faceTag != NULL) of pos's leaf whose plane is nearest to pos and whose
     * projection contains pos. Returns g_ztagFaceHit (kind 1, the 3 vertices, the face record) or
     * NULL if none is within maxDist. Untagged faces are ignored.
     */
    ZtagNode *leaf;
    ZtagFace *best = 0;
    float bestDist = 9999.9f;
    float dist;
    int i;

    g_ztagFaceHit.kind = 0;
    g_ztagFaceHit.tag = 0;
    leaf = ztaginfFindLeaf(pos);
    if (!leaf)
        return 0;
    if (g_ztagDebugTask) {
        RwRGBA leafColour = rgba(0xFF, 0x00, 0x00, 0x20);
        for (i = 0; i < (int)leaf->count; i++)
            ztaginfDebugDrawFace(leaf->faces[i], 0, 0, &leafColour);
    }
    for (i = 0; i < (int)leaf->count; i++) {
        ZtagFace *f = leaf->faces[i];
        float d[4];

        if (!f->faceTag)
            continue;
        zmathVecSub(d, pos, &f->v[0]);
        dist = (float)fabs(zmathVecDot(d, &f->normal));
        if (!_finite(dist)) {
            g_pfnDebugPrintf("FUCKED TAG INFO NORMAL\n");
            dist = 99999.0f;
        }
        if (dist < bestDist && ztaginfProjectInFace(f, pos, dist)) {   /* inlined in the exe */
            bestDist = dist;
            best = f;
        }
    }
    if (bestDist > maxDist)
        return 0;
    if (best && g_ztagDebugTask) {
        RwRGBA hitColour = rgba(0x00, 0x00, 0xFF, 0x20);
        ztaginfDebugDrawFace(best, 0, 0, &hitColour);
    }
    /* best is not NULL here: bestDist <= maxDist needs a hit (unless maxDist >= 9999.9) */
    g_ztagFaceHit.kind = ZTAG_HIT_FACE;
    g_ztagFaceHit.tag = best->faceTag;
    copyVec(&g_ztagFaceHit.v[0], &best->v[0]);
    copyVec(&g_ztagFaceHit.v[1], &best->v[1]);
    copyVec(&g_ztagFaceHit.v[2], &best->v[2]);
    return &g_ztagFaceHit;
}

/* VA 0x0048ea70, 689 bytes, [I] row, status draft */
ZtagHit *ztaginfFindNearestEdge(const float *pos, float maxDist)
{
    /* The tagged edge of pos's leaf nearest to pos (distance to the edge's line, EdgeContains and
     * EdgeDistance inlined). Returns g_ztagEdgeHit (kind 2, both ends, the edge record). */
    ZtagNode *leaf;
    ZtagFace *best = 0;
    int bestEdge = 0;
    float bestDist = 9999.9f;
    int i, e;

    g_ztagEdgeHit.kind = 0;
    g_ztagEdgeHit.tag = 0;
    leaf = ztaginfFindLeaf(pos);
    if (!leaf)
        return 0;
    for (i = 0; i < (int)leaf->count; i++) {
        for (e = 0; e < 3; e++) {
            ZtagFace *f = leaf->faces[i];
            float dist;

            if (!f->edgeTag[e])
                continue;
            dist = ztaginfEdgeDistance(f, pos, e);
            if (dist < bestDist && ztaginfEdgeContains(f, pos, e)) {
                best = f;
                bestEdge = e;
                bestDist = dist;
            }
        }
    }
    if (bestDist > maxDist)
        return 0;
    if (best && g_ztagDebugTask) {
        RwRGBA green = rgba(0x00, 0xFF, 0x00, 0x80);
        ztaginfDebugDrawEdge(best, bestEdge, &green);
    }
    g_ztagEdgeHit.kind = ZTAG_HIT_EDGE;
    g_ztagEdgeHit.tag = best->edgeTag[bestEdge];
    copyVec(&g_ztagEdgeHit.v[0], &best->v[bestEdge]);
    copyVec(&g_ztagEdgeHit.v[1], &best->v[(bestEdge + 1) % 3]);
    return &g_ztagEdgeHit;
}

/* VA 0x0048ed30, 319 bytes, [I] row, status draft */
ZtagHit *ztaginfFindNearestVertex(const float *pos, float maxDist)
{
    ZtagNode *leaf;
    ZtagFace *best = 0;
    int bestVertex = 0;
    float bestDist = 9999.9f;
    int i, v;

    g_ztagVertexHit.kind = 0;
    g_ztagVertexHit.tag = 0;
    leaf = ztaginfFindLeaf(pos);
    if (!leaf)
        return 0;
    for (i = 0; i < (int)leaf->count; i++) {
        for (v = 0; v < 3; v++) {
            ZtagFace *f = leaf->faces[i];
            float d[4], dist;

            if (!f->vertexTag[v])
                continue;
            zmathVecSub(d, pos, &f->v[v]);
            dist = (float)fabs(zmathVecLength(d));
            if (dist < bestDist) {
                bestDist = dist;
                best = f;
                bestVertex = v;
            }
        }
    }
    if (bestDist > maxDist)
        return 0;
    g_ztagVertexHit.kind = ZTAG_HIT_VERTEX;
    g_ztagVertexHit.tag = best->vertexTag[bestVertex];
    copyVec(&g_ztagVertexHit.v[0], &best->v[bestVertex]);
    return &g_ztagVertexHit;
}

/* appends a hit (callers check count < ZTAG_MAX_HITS); the exe always writes g_ztagHits[count],
 * whatever list->hits points to */
static ZtagHit *addHit(ZtagHitList *list, int kind, uint8_t *tag)
{
    ZtagHit *h = &g_ztagHits[list->count];
    h->kind = (uint8_t)kind;
    h->tag = tag;
    list->count++;
    return h;
}

/* VA 0x0048ee70, 1108 bytes, [I] row, status draft */
void ztaginfCollect(int kind, const ZtagNode *node, ZtagHitList *list, const float *pos, float radius)
{
    /*
     * Collects tagged faces/edges/vertices within radius of pos into g_ztagHits (at most 64).
     * Descends into both children where the split plane is closer than radius (low first by
     * recursion), else into the side pos is on - note the side test here is "split <= p -> high"
     * on every axis, also on y, unlike ztaginfFindLeaf. Faces in several leaves are added once
     * per leaf (no de-duplication).
     */
    float split = 0.0f;   /* the exe keeps this in the `list` argument slot (see kind 1 below);
                           * `list` itself lives on in a register */
    int i;

    while (node->kind != ZTAG_NODE_LEAF) {
        if (node->kind == ZTAG_NODE_X)
            split = pos[0];
        else if (node->kind == ZTAG_NODE_Y)
            split = pos[1];
        else if (node->kind == ZTAG_NODE_Z)
            split = pos[2];
        /* (`split` holds the position's coordinate on the node's axis) */
        if (fabs(split - node->split) < radius) {
            ztaginfCollect(kind, node->low, list, pos, radius);
            node = node->high;
        } else if (node->split <= split) {
            node = node->high;
        } else {
            node = node->low;
        }
    }
    for (i = 0; i < (int)node->count; i++) {
        ZtagFace *f = node->faces[i];
        int e;

        if (kind == ZTAG_HIT_FACE) {
            /*
             * [V] bug in the exe (0x0048F193): the "nearest vertex" distance is measured three
             * times to &f->v[n], where n is whatever the reused `list` stack slot holds - the bit
             * pattern of the last split coordinate, or the list pointer if the root is a leaf -
             * i.e. a wild pointer. ztaginfFindFaces, the only caller of this kind, has no caller.
             */
            if (f->faceTag) {
                float best = 9999.0f, d[4], dist;
                int n = *(int *)&split;
                for (e = 0; e < 3; e++) {
                    zmathVecSub(d, pos, &f->v[n]);
                    dist = (float)fabs(zmathVecLength(d));
                    if (dist < best)
                        best = dist;
                }
                if (best < radius && list->count < ZTAG_MAX_HITS) {
                    ZtagHit *h = addHit(list, ZTAG_HIT_FACE, f->faceTag);
                    copyVec(&h->v[0], &f->v[0]);
                    copyVec(&h->v[1], &f->v[1]);
                    copyVec(&h->v[2], &f->v[2]);
                }
            }
        } else if (kind == ZTAG_HIT_EDGE) {
            for (e = 0; e < 3; e++) {
                if (!f->edgeTag[e])
                    continue;
                if (ztaginfEdgeDistance(f, pos, e) < radius && list->count < ZTAG_MAX_HITS) {
                    ZtagHit *h;
                    if (g_ztagDebugTask) {
                        RwRGBA green = rgba(0x00, 0xFF, 0x00, 0xFF);
                        ztaginfDebugDrawEdge(f, e, &green);
                    }
                    h = addHit(list, ZTAG_HIT_EDGE, f->edgeTag[e]);
                    copyVec(&h->v[0], &f->v[e]);
                    copyVec(&h->v[1], &f->v[(e + 1) % 3]);
                }
            }
        } else if (kind == ZTAG_HIT_VERTEX) {
            for (e = 0; e < 3; e++) {
                float d[4];
                if (!f->vertexTag[e])
                    continue;
                zmathVecSub(d, pos, &f->v[e]);
                if (fabs(zmathVecLength(d)) < radius && list->count < ZTAG_MAX_HITS) {
                    ZtagHit *h = addHit(list, ZTAG_HIT_VERTEX, f->vertexTag[e]);
                    copyVec(&h->v[0], &f->v[e]);
                }
            }
        }
    }
}

/* VA 0x0048f2d0, 66 bytes, [I] row, status draft */
ZtagHitList *ztaginfFindFaces(const float *pos, float radius)
{
    g_ztagHitList.count = 0;
    g_ztagHitList.hits = g_ztagHits;
    ztaginfCollect(ZTAG_HIT_FACE, &g_ztagRoot, &g_ztagHitList, pos, radius);
    return g_ztagHitList.count ? &g_ztagHitList : 0;
}

/* VA 0x0048f320, 66 bytes, [I] row, status draft */
ZtagHitList *ztaginfFindEdges(const float *pos, float radius)
{
    /* zcolFindEdgesAlong: grind edges around the rider */
    g_ztagHitList.count = 0;
    g_ztagHitList.hits = g_ztagHits;
    ztaginfCollect(ZTAG_HIT_EDGE, &g_ztagRoot, &g_ztagHitList, pos, radius);
    return g_ztagHitList.count ? &g_ztagHitList : 0;
}

/* VA 0x0048f370, 66 bytes, [I] row, status draft */
ZtagHitList *ztaginfFindVertices(const float *pos, float radius)
{
    g_ztagHitList.count = 0;
    g_ztagHitList.hits = g_ztagHits;
    ztaginfCollect(ZTAG_HIT_VERTEX, &g_ztagRoot, &g_ztagHitList, pos, radius);
    return g_ztagHitList.count ? &g_ztagHitList : 0;
}

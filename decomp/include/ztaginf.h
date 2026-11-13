/*
 * ztaginf.h - NgEngine/Zge/ZgeMods/ztaginf.c: tagged collision faces (Tag._sd) and their kd-tree
 *. File format: docs/formats/LEVEL.md section 6.
 *
 * ztaginfLoad 0x0048DAE0 turns every TAGF record into a ZtagFace (vertices padded to 16 bytes, tag
 * offsets turned into pointers into the TAGI blob) and every TAGN record into a ZtagNode. The
 * queries (nearest face/edge/vertex, collect within a radius) walk the tree from the static root.
 * Offsets [V], names [I].
 */
#ifndef ZTAGINF_H
#define ZTAGINF_H
#include "dmtypes.h"

typedef struct ZtagVec {
    float x;                  /* +0x00 */
    float y;                  /* +0x04 */
    float z;                  /* +0x08 */
    float w;                  /* +0x0C never written (heap contents) */
} ZtagVec;                    /* 0x10 */

typedef struct ZtagFace {
    ZtagVec  v[3];            /* +0x00 vertices, ZGE units */
    ZtagVec  normal;          /* +0x30 */
    uint8_t *faceTag;         /* +0x40 12-byte face record in the TAGI blob, or NULL */
    uint8_t *edgeTag[3];      /* +0x44 4-byte records for v0->v1, v1->v2, v2->v0, or NULL */
    uint8_t *vertexTag[3];    /* +0x50 4-byte records, or NULL */
    uint32_t runtime[4];      /* +0x5C cleared by the loader, not used in ztaginf.c */
} ZtagFace;                   /* 0x6C */

#define ZTAG_NODE_LEAF 1
#define ZTAG_NODE_X    2      /* low child: p.x <  split */
#define ZTAG_NODE_Y    4      /* low child: p.y >  split (ZGE y points down) */
#define ZTAG_NODE_Z    8      /* low child: p.z <  split */

typedef struct ZtagNode {
    float            split;     /* +0x00 */
    float            boundB[3]; /* +0x04 TAGN +0x18 (RW maximum corner) */
    float            pad10;     /* +0x10 */
    float            boundA[3]; /* +0x14 TAGN +0x0C (RW minimum corner) */
    float            pad20;     /* +0x20 */
    uint8_t          kind;      /* +0x24 ZTAG_NODE_* */
    uint8_t          pad25;     /* +0x25 */
    uint16_t         count;     /* +0x26 faces in a leaf (low 16 bits of TAGN count) */
    struct ZtagNode *low;       /* +0x28 */
    struct ZtagNode *high;      /* +0x2C */
    ZtagFace       **faces;     /* +0x30 leaf face pointers */
} ZtagNode;                     /* 0x34, zmem pool of 256 (root is static) */

#define ZTAG_HIT_FACE   1
#define ZTAG_HIT_EDGE   2
#define ZTAG_HIT_VERTEX 3

typedef struct ZtagHit {
    uint8_t  kind;            /* +0x00 ZTAG_HIT_* (0 = none) */
    uint8_t  pad01[3];        /* +0x01 */
    ZtagVec  v[3];            /* +0x04 face: 3 vertices; edge: its 2 ends; vertex: 1 */
    uint8_t *tag;             /* +0x34 the tag record */
} ZtagHit;                    /* 0x38 */

#define ZTAG_MAX_HITS 64

typedef struct ZtagHitList {
    uint16_t count;           /* +0x00 */
    uint16_t pad02;           /* +0x02 */
    ZtagHit *hits;            /* +0x04 -> g_ztagHits */
} ZtagHitList;                /* 0x08 */

/* file records */
typedef struct ZtagTagdRecord {
    uint32_t size;            /* +0x00 8 ("Chunk sync error" otherwise) */
    uint32_t faceCount;       /* +0x04 */
} ZtagTagdRecord;             /* 0x08 */

typedef struct ZtagTagfRecord {
    uint32_t size;            /* +0x00 0x50 */
    float    v0[3];           /* +0x04 */
    float    v1[3];           /* +0x10 */
    float    v2[3];           /* +0x1C */
    float    normal[3];       /* +0x28 */
    int32_t  faceTag;         /* +0x34 byte offset into the TAGI blob, -1 = none */
    int32_t  edgeTag[3];      /* +0x38 */
    int32_t  vertexTag[3];    /* +0x44 */
} ZtagTagfRecord;             /* 0x50 */

typedef struct ZtagTagnRecord {
    uint32_t size;            /* +0x00 0x28 (not checked) */
    uint8_t  kind;            /* +0x04 */
    uint8_t  junk[3];         /* +0x05 */
    float    split;           /* +0x08 */
    float    boundA[3];       /* +0x0C */
    float    boundB[3];       /* +0x18 */
    uint32_t count;           /* +0x24 followed by count u32 face indices */
} ZtagTagnRecord;             /* 0x28 */

/* GHIDRA-SKIP-BEGIN */
typedef char ztaginf_h_size_check[(sizeof(ZtagVec) == 0x10 && sizeof(ZtagFace) == 0x6C &&
                                   sizeof(ZtagNode) == 0x34 && sizeof(ZtagHit) == 0x38 &&
                                   sizeof(ZtagHitList) == 0x08 && sizeof(ZtagTagdRecord) == 0x08 &&
                                   sizeof(ZtagTagfRecord) == 0x50 && sizeof(ZtagTagnRecord) == 0x28) ? 1 : -1];

int          ztaginfInit(void);
int          ztaginfShutdown(void);
void         ztaginfSetDebugDraw(int on, uint32_t flags);
void         ztaginfFreeNode(ZtagNode *node);
void         ztaginfUnload(void);
ZtagFace    *ztaginfGetFaces(void);
uint32_t     ztaginfGetFaceCount(void);
int          ztaginfLoadNode(ZtagNode *node, ZiffFile *iff);
int          ztaginfLoad(ZiffFile *iff);
void         ztaginfSetDebugFlags(uint32_t flags);
void         ztaginfDebugDrawNode(const ZtagNode *node, const RwRGBA *colour);
void         ztaginfDebugDrawFace(const ZtagFace *face, const RwRGBA *vertexColour, const RwRGBA *edgeColour,
                                  const RwRGBA *faceColour);
void         ztaginfDebugDrawEdge(const ZtagFace *face, int edge, const RwRGBA *colour);
int          ztaginfDebugRender(void);
ZtagNode    *ztaginfFindLeaf(const float *pos);
int          ztaginfPointInFace(const ZtagVec *a, const ZtagVec *b, const ZtagVec *c, const ZtagVec *normal,
                                const float *p);
int          ztaginfProjectInFace(const ZtagFace *face, const float *pos, float dist);
float        ztaginfPlaneDistance(const ZtagFace *face, const float *pos);
int          ztaginfEdgeContains(const ZtagFace *face, const float *pos, int edge);
float        ztaginfEdgeDistance(const ZtagFace *face, const float *pos, int edge);
float        ztaginfVertexDistance(const ZtagFace *face, const float *pos, int vertex);
ZtagHit     *ztaginfFindNearestFace(const float *pos, float maxDist);
ZtagHit     *ztaginfFindNearestEdge(const float *pos, float maxDist);
ZtagHit     *ztaginfFindNearestVertex(const float *pos, float maxDist);
void         ztaginfCollect(int kind, const ZtagNode *node, ZtagHitList *list, const float *pos, float radius);
ZtagHitList *ztaginfFindFaces(const float *pos, float radius);
ZtagHitList *ztaginfFindEdges(const float *pos, float radius);
ZtagHitList *ztaginfFindVertices(const float *pos, float radius);
/* GHIDRA-SKIP-END */

#endif /* ZTAGINF_H */

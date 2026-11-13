/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFzbsp.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFzbsp.c
 * Evidence: [V] path string 0x0055CB34: the asserts of zbspFindEdgesNear (0x91F, 0x940),
 *           zbspColSphere (0xDE9), zbspFindPropsByGroup (0xF21), zbspFindTaggedFaces (0xF61),
 *           zbspFindTaggedEdges (0xFF6) and the 113 "not implemented" stubs (lines 107 .. 4108,
 *           in address order, from 0x00465A50 'bsp init' on); [I] the rest by link order.
 * 137 functions, VA 0x00465a50-0x0046951f (the NgIF pass created the 113 stubs, zbspUnusedStub
 * 0x004665F0 and zbspPropColFree 0x00468D20, and took zbspPropSetRotation / AddRotation /
 * SetPosition from the NgIFzactor/NgIFzbsp gap).
 * Draft. Non-matching C (AD-8). Types: ngif.h, physics.h (NgColFace / NgColEdge),
 * bmxprop.h (NgPropCol). Description: docs/re/ngif.md, docs/re/physics.md.
 *
 * The PSX engine kept the level in its own BSP ("zbsp"); the PC port uses RenderWare's world and
 * collision instead, so most of the zbsp API is stubs that only print their name. What remains:
 *   - prop collision records (NgPropCol, 500 bytes, one per scene prop, made lazily) and the prop
 *     defs' collision shapes (NgPropShape, 0x38); the mode switches prop.c / physics.c use;
 *   - queries over the Tag._sd faces and edges: grind edges near a point, the n-th edge with a
 *     code, faces / edges / props with a tag id in a range, and the sphere query (RW triangles
 *     within a radius, tagged, with the closest point on each).
 */
#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "ngif.h"

#define NGIFZBSP_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFzbsp.c"

/* ------------------------------------------------------------------ other modules */
void  zobjSetRotation(void *obj, const Zquat *q);                        /* set the rotation */
void  zobjSetPosition(void *obj, const float *pos);                      /* set the position */
void  zobjSetActive(void *obj, int visible);
int   zrwColSphere(const RwV3d *pos, float radius, int flags, int arg, ZrwColTriList *out); /* RW sphere test */
extern void *(__cdecl *g_zglModelGetObj)(void *model);                 /* 0x00560EE0 -> zrwModel_00488BB0 */
extern void *(__cdecl *g_zglMatGetPluginData)(void *material);         /* 0x00561038 -> zrwMat_00489B30 */
extern zHeapList *g_zrollerEdges;                                      /* 0x00678494 the prop rails */
extern ZmemFreeList *g_zbspFacePool;                                   /* 0x005F34AC NgColFace records */
void  zcolPropRotated(NgPropCol *col, const int16_t *euler);           /* NgIFzcollide.c 0x0046AC40 */
void  zcolPropMoved(NgPropCol *col, const float *pos);                 /* 0x0046AC70 */
const NgPropShapeInfo *zcolGetPropShape(uint32_t defIndex);            /* 0x00469530 */

/* ------------------------------------------------------------------ globals (this file) */
static ZbspSphereHit s_sphereHits[256];    /* 0x005E63D8 */
static uint32_t   s_faceMaterial;          /* 0x005E7FD8 */
static NgColFace  s_propFaces[255];        /* 0x005E7FE0 zbspFindTaggedFaces: prop switch faces */
static NgColFace  s_sphereFaces[255];      /* 0x005E8420 */
static ZrwColTriList s_rwTris;             /* 0x005F0C20 */
static ZbspSphereResult s_sphere;          /* 0x005F0C28 (hits at +0x3C = 0x005F0C64) */
static void      *s_list[258];             /* 0x005F0C68 NULL-terminated result list (edges, faces, props) */
static NgColEdge  s_taggedEdge;            /* 0x005F1070 zbspGetTaggedEdge */
static int16_t    s_sphereVerts[255][9];   /* 0x005F10A8 integer copies of the sphere faces' corners */
ZmemFreeList     *g_zbspFacePool;          /* 0x005F34AC */
ZmemFreeList     *g_zbspEdgePool;          /* 0x005F34B0 NgColEdge records (0x38) */
static int        s_propFaceCount;         /* 0x005F34BC (never reset) */

#define LIST_END ((void **)&s_list[256])   /* 0x005F1068: "edge list overflow" past this */

/* the collision record of a scene prop, made on first use */
static NgPropCol *propCol(Zprop *z)
{
    if (z->info.user == NULL)
        zbspPropColCreate(z);
    return (NgPropCol *)z->info.user;
}

/* a grind edge (NgColEdge) from face vertex a to b with tag record e (code, kind) */
static void makeEdge(NgColEdge *edge, const ZtagVec *a, const ZtagVec *b, const uint8_t *e)
{
    RwV3d d, zero = { 0.0f, 0.0f, 0.0f }, up = { 0.0f, 1.0f, 0.0f }, n;

    edge->flags = 0;
    edge->kind = e[2];
    edge->v0[0] = (int16_t)a->x; edge->v0[1] = (int16_t)a->y; edge->v0[2] = (int16_t)a->z;
    edge->v1[0] = (int16_t)b->x; edge->v1[1] = (int16_t)b->y; edge->v1[2] = (int16_t)b->z;
    d.x = b->x; d.y = b->y; d.z = b->z;
    zmathVecSub(&d, &d, (const RwV3d *)a);
    edge->length = (int16_t)zmathVecLength(&d);
    edge->code = *(const uint16_t *)e;
    edge->owner = NULL;
    zmathVecNormalize(&d, &d);
    edge->dir[0] = d.x; edge->dir[1] = d.y; edge->dir[2] = d.z;
    zmathTriangleNormal(&n, &up, &zero, &d);           /* horizontal, across the edge */
    edge->normal[0] = n.x; edge->normal[1] = n.y; edge->normal[2] = n.z;
    edge->d = -zmathVecDot(&n, (const RwV3d *)a);
}

/* ------------------------------------------------------------------ functions */

/* VA 0x00465a50, 29 bytes, [V] direct, status draft */
int zbspInit(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 107, "bsp init");
    return 0;
}

/* VA 0x00465a80, 27 bytes, [V] direct, status draft */
void zbspUpdate(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 126, "bsp update");
}

/* VA 0x00465ab0, 32 bytes, [V] direct, status draft */
int zbspShutdown(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 142, "shutdown");
    return 0;
}

/* VA 0x00465ae0, 32 bytes, [V] direct, status draft */
int zbspInitializeScene(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 165, "initialize scene");
    return 0;
}

/* VA 0x00465b10, 32 bytes, [V] direct, status draft */
int zbspLoadScene(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 186, "load scene");
    return 0;
}

/* VA 0x00465b40, 30 bytes, [V] direct, status draft */
void zbspLoadAndProcessMap(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 204, "load and process map");
}

/* VA 0x00465b70, 32 bytes, [V] direct, status draft */
int zbspAllocAndLoadMap(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 221, "alloc and load map");
    return 0;
}

/* VA 0x00465ba0, 30 bytes, [V] direct, status draft */
void zbspFreeMap(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 238, "free map");
}

/* VA 0x00465bd0, 32 bytes, [V] direct, status draft */
int zbspSetTreeVisible(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 270, "set tree visible");
    return 0;
}

/* VA 0x00465c00, 34 bytes, [V] direct, status draft */
void zbspConvertTreeFromLoad(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 574, "convert tree from load");
}

/* VA 0x00465c40, 30 bytes, [V] direct, status draft */
void zbspRelativeLodZ(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 592, "relative lod z");
}

/* VA 0x00465c70, 30 bytes, [V] direct, status draft */
void zbspPreprocessScene(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 613, "preprocess scene");
}

/* VA 0x00465ca0, 30 bytes, [V] direct, status draft */
void zbspPreprocessTerrainNode(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 627, "preprocess terrain node");
}

/* VA 0x00465cd0, 30 bytes, [V] direct, status draft */
void zbspPreprocessFaceList(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 641, "preprocess face list");
}

/* VA 0x00465d00, 35 bytes, [V] direct, status draft */
int zbspViewerInPosSide(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 702, "viewer in pos side");
    return 1;
}

/* VA 0x00465d40, 30 bytes, [V] direct, status draft */
void zbspSndRegister(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 725, "snd register");
}

/* VA 0x00465d70, 30 bytes, [V] direct, status draft */
void zbspSndSetState(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 743, "snd set state");
}

/* VA 0x00465da0, 32 bytes, [V] direct, status draft */
int zbspSndGetHandle(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 760, "snd get handle");
    return 0;
}

/* VA 0x00465dd0, 32 bytes, [V] direct, status draft */
int zbspDvlitehandle(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 782, "dvlitehandle");
    return 0;
}

/* VA 0x00465e00, 35 bytes, [V] direct, status draft */
int zbspDvlitegetcolor(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 800, "dvlitegetcolor");
    return 1;
}

/* VA 0x00465e40, 30 bytes, [V] direct, status draft */
void zbspDvlitelite(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 819, "dvlitelite");
}

/* VA 0x00465e70, 30 bytes, [V] direct, status draft */
void zbspDvliteinitialize(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 836, "dvliteinitialize");
}

/* VA 0x00465ea0, 30 bytes, [V] direct, status draft */
void zbspDvliteAttachToProps(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 852, "dvlite attach to props");
}

/* VA 0x00465ed0, 30 bytes, [V] direct, status draft */
void zbspPropActivate(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 875, "prop activate");
}

/* VA 0x00465f00, 30 bytes, [V] direct, status draft */
void zbspPropDeactivate(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 890, "prop deactivate");
}

/* VA 0x00465f30, 30 bytes, [V] direct, status draft */
void zbspActivateTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 904, "activate tree");
}

/* VA 0x00465f60, 30 bytes, [V] direct, status draft */
void zbspDeactivateTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 918, "deactivate tree");
}

/* VA 0x00465f90, 32 bytes, [V] direct, status draft */
int zbspInitializeTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 937, "initialize tree ref");
    return 0;
}

/* VA 0x00465fc0, 33 bytes, [V] direct, status draft */
void zbspCreateTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 955, "create tree ref");
}

/* VA 0x00466000, 30 bytes, [V] direct, status draft */
void zbspDestroyTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 972, "destroy tree ref");
}

/* VA 0x00466030, 30 bytes, [V] direct, status draft */
void zbspPropAddTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 988, "prop add tree ref");
}

/* VA 0x00466060, 30 bytes, [V] direct, status draft */
void zbspRemoveTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1004, "remove tree ref");
}

/* VA 0x00466090, 30 bytes, [V] direct, status draft */
void zbspUpdateTreeRef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1020, "update tree ref");
}

/* VA 0x004660c0, 30 bytes, [V] direct, status draft */
void zbspAddToTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1038, "add to tree");
}

/* VA 0x004660f0, 30 bytes, [V] direct, status draft */
void zbspRemoveFromTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1055, "remove from tree");
}

/* VA 0x00466120, 35 bytes, [V] direct, status draft */
int zbspTotallyWithinTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1073, "totally within tree");
    return 1;
}

/* VA 0x00466160, 32 bytes, [V] direct, status draft */
int zbspOverlapsTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1092, "overlaps tree");
    return 0;
}

/* VA 0x00466190, 32 bytes, [V] direct, status draft */
int zbspFindTagged(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1114, "find tagged");
    return 0;
}

/* VA 0x004661c0, 79 bytes, [I] row, status draft */
void zbspPropSetRotation(NgPropCol *col, const int16_t *euler)
{
    /* turn the prop (prop.c movers); its rails follow unless it is moving and knocked */
    Zquat q;

    zquatFromEuler(&q, euler, 0);
    zobjSetRotation(col->prop->info.obj, &q);
    if (!(col->flags & NGPROPCOL_MOVING) || !(col->flags & NGPROPCOL_OFF))
        zcolPropRotated(col, euler);
}

/* VA 0x00466230, 82 bytes, [I] row, status draft */
void zbspPropAddRotation(NgPropCol *col, const int16_t *euler)
{
    int16_t e[3];

    zquatToEuler(e, (Zquat *)((uint8_t *)col->prop->info.obj + 0x14), 0);
    e[0] = (int16_t)(e[0] + euler[0]);
    e[1] = (int16_t)(e[1] + euler[1]);
    e[2] = (int16_t)(e[2] + euler[2]);
    zbspPropSetRotation(col, e);
}

/* VA 0x004662b0, 74 bytes, [I] row, status draft */
void zbspPropSetPosition(NgPropCol *col, const float *pos)
{
    col->pos[0] = (int16_t)pos[0];
    col->pos[1] = (int16_t)pos[1];
    col->pos[2] = (int16_t)pos[2];
    zobjSetPosition(col->prop->info.obj, pos);
    zcolPropMoved(col, pos);
}

/* VA 0x00466320, 30 bytes, [V] direct, status draft */
void zbspPropMove(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1229, "zbspPropMove");
}

/* VA 0x00466350, 35 bytes, [V] direct, status draft */
int zbspGetNumDefs(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1245, "get num defs");
    return 1;
}

/* VA 0x00466390, 206 bytes, [I] callers, status draft */
void zbspInitPropDefShape(ZpropDefInfo *def)
{
    /* The def's collision shape from its OBJI: byte 0x16 = file class 1..4 -> shape class 2, 3,
     * 1, 4 (LEVEL.md 5.4: 2 box with 4 top rails, 3 / 4 one rail, 1 not coded); a box of half
     * width extents.x * 50, height extents.y * 100, half depth extents.z * 50, raised by
     * (extents.y / 2 + extra[1] + extra[3]) * 100. */
    NgPropShape *s;

    if (def->obji == NULL)
        return;
    s = (NgPropShape *)zstringMemSet(zmemAlloc(sizeof(NgPropShape)), 0, sizeof(NgPropShape));
    def->collision = s;
    s->flags &= 0xFE00;
    s->surface = 0;
    s->unk_04 = 0;
    s->unk_06 = 0;
    s->unk_07 = 0;
    switch (def->obji[0x16]) {
    case 1:  s->info.cls = NGSHAPE_BOX;     break;
    case 2:  s->info.cls = NGSHAPE_RAIL;    break;
    case 3:  s->info.cls = NGSHAPE_1;       break;
    case 4:  s->info.cls = NGSHAPE_EDGE;    break;
    default: s->info.cls = NGSHAPE_NONE;    break;
    }
    if (s->info.cls == NGSHAPE_NONE) {
        s->info.size[0] = s->info.size[1] = s->info.size[2] = 0.0f;
        s->info.yOffset = 0.0f;
    } else if (s->info.cls < 5) {
        s->info.size[0] = def->extents[0] * 50.0f;
        s->info.size[1] = def->extents[1] * 100.0f;
        s->info.size[2] = def->extents[2] * 50.0f;
        s->info.yOffset = (def->extents[1] * 0.5f + def->extra[3] + def->extra[1]) * 100.0f;
    }
}

/* VA 0x004664b0, 32 bytes, [V] direct, status draft */
int zbspGetTypeFromHash(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1350, "get type from hash");
    return 0;
}

/* VA 0x004664e0, 70 bytes, [I] split, status draft */
int zbspGetPropDefIndex(const ZpropDefInfo *def)
{
    /* position of the def in zpropGetDefList (= NgPropCol.defIndex), -1 if absent; makes the
     * shapes of the defs it passes */
    zListNode *node;
    int i = 0;

    for (node = zpropGetDefList()->head; node != NULL; node = node->next, i++) {
        ZpropDef *d = (ZpropDef *)node;

        if (d->info.collision == NULL)
            zbspInitPropDefShape(&d->info);
        if (&d->info == def)
            return i;
    }
    return -1;
}

/* VA 0x00466550, 73 bytes, [I] between, status draft */
ZpropDefInfo *zbspGetPropDefInfo(int index)
{
    zListNode *node;
    int i = 0;

    for (node = zpropGetDefList()->head; node != NULL; node = node->next, i++) {
        ZpropDef *d = (ZpropDef *)node;

        if (d->info.collision == NULL)
            zbspInitPropDefShape(&d->info);
        if (i == index)
            return &d->info;
    }
    return NULL;
}

/* VA 0x004665c0, 32 bytes, [I] between, status draft */
NgPropShape *zbspGetPropShape(uint16_t index)
{
    ZpropDefInfo *def = zbspGetPropDefInfo(index);

    return def != NULL ? (NgPropShape *)def->collision : NULL;
}

/* VA 0x004665f0, 3 bytes, [I] between, status draft */
int zbspUnusedStub(void)
{
    return 0;   /* unreferenced, silent */
}

/* VA 0x00466610, 126 bytes, [I] between, status draft */
void zbspPropSetMode(NgPropCol *col, int mode)
{
    /* NGPROPCOL_SET_*: 0 knocked (physics: a hard crash into the prop), 1 (unused), 2 / 3 moving
     * (model flag 8 = not in the static lighting [I]), 4 / 5 hide / show */
    void *obj = col->prop->info.obj;

    switch (mode) {
    case NGPROPCOL_SET_OFF:
        col->flags |= NGPROPCOL_OFF;
        return;
    case NGPROPCOL_SET_1:
        col->flags |= NGPROPCOL_FLAG100;
        return;
    case NGPROPCOL_SET_MOVING:
        g_zrwModelSetFlags(*(void **)((uint8_t *)obj + 0x2C), 8, 0);
        col->flags |= NGPROPCOL_MOVING;
        return;
    case NGPROPCOL_SET_STATIC:
        g_zrwModelSetFlags(*(void **)((uint8_t *)obj + 0x2C), 0, 8);
        col->flags &= (uint16_t)~NGPROPCOL_MOVING;
        return;
    case NGPROPCOL_SET_HIDE:
        zobjSetActive(obj, 0);
        col->flags |= NGPROPCOL_HIDDEN;
        return;
    case NGPROPCOL_SET_SHOW:
        zobjSetActive(obj, 1);
        col->flags &= (uint16_t)~NGPROPCOL_HIDDEN;
        return;
    }
}

/* VA 0x004666e0, 30 bytes, [V] direct, status draft */
void zbspFxCreateTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1525, "fx create tree");
}

/* VA 0x00466710, 30 bytes, [V] direct, status draft */
void zbspDestroyTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1540, "destroy tree");
}

/* VA 0x00466740, 30 bytes, [V] direct, status draft */
void zbspDrawTerrain(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1562, "draw terrain");
}

/* VA 0x00466770, 30 bytes, [V] direct, status draft */
void zbspDrawTerrainNode(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1578, "draw terrain node");
}

/* VA 0x004667a0, 30 bytes, [V] direct, status draft */
void zbspDrawFaceList(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1592, "draw face list");
}

/* VA 0x004667d0, 30 bytes, [V] direct, status draft */
void zbspSetDebug(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1638, "set debug");
}

/* VA 0x00466800, 30 bytes, [V] direct, status draft */
void zbspDrawLineZ(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1667, "draw line z");
}

/* VA 0x00466830, 30 bytes, [V] direct, status draft */
void zbspDrawFaceNormal(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1680, "draw face normal");
}

/* VA 0x00466860, 30 bytes, [V] direct, status draft */
void zbspDrawFaceFlat(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1698, "draw face flat");
}

/* VA 0x00466890, 30 bytes, [V] direct, status draft */
void zbspShowBoundingBox(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1712, "show bounding box");
}

/* VA 0x004668c0, 30 bytes, [V] direct, status draft */
void zbspShowEdges(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1729, "show edges");
}

/* VA 0x004668f0, 30 bytes, [V] direct, status draft */
void zbspShowPropVolume(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1743, "show prop volume");
}

/* VA 0x00466920, 35 bytes, [V] direct, status draft */
int zbspGetFaceNum(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1750, "get face num");
    return 1;
}

/* VA 0x00466960, 30 bytes, [V] direct, status draft */
void zbspTrnDumpTree(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1764, "trn dump tree");
}

/* VA 0x00466990, 30 bytes, [V] direct, status draft */
void zbspShowPartition(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1778, "show partition");
}

/* VA 0x004669c0, 30 bytes, [V] direct, status draft */
void zbspShowPartition2(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1794, "show partition");
}

/* VA 0x004669f0, 30 bytes, [V] direct, status draft */
void zbspDrawPartition(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1808, "draw partition");
}

/* VA 0x00466a20, 32 bytes, [V] direct, status draft */
int zbspGetTerrainFaces(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1827, "get terrain faces");
    return 0;
}

/* VA 0x00466a50, 32 bytes, [V] direct, status draft */
int zbspGetTreeObject(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1844, "get tree object");
    return 0;
}

/* VA 0x00466a80, 32 bytes, [V] direct, status draft */
int zbspPointWithinTreeObject(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1862, "point within tree object");
    return 0;
}

/* VA 0x00466ab0, 32 bytes, [V] direct, status draft */
int zbspGetTreeObjects(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1883, "get tree objects");
    return 0;
}

/* VA 0x00466ae0, 30 bytes, [V] direct, status draft */
void zbspGetTerrainNode(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1903, "get terrain node");
}

/* VA 0x00466b10, 32 bytes, [V] direct, status draft */
int zbspGetMaterialInfo(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1920, "get material info");
    return 0;
}

/* VA 0x00466b40, 32 bytes, [V] direct, status draft */
int zbspGetMatRefData(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1937, "get mat ref data");
    return 0;
}

/* VA 0x00466b70, 35 bytes, [V] direct, status draft */
int zbspGetMatIdx(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1954, "get mat idx");
    return 1;
}

/* VA 0x00466bb0, 44 bytes, [I] between, status draft */
const uint32_t *zbspFaceMaterial(const NgColFace *face)
{
    /* the RW material flags of a sphere-query face (its triangle's material plugin data) */
    const ZrwColTri *tri = (const ZrwColTri *)face->rwTri;

    if (tri->material == NULL)
        return NULL;
    s_faceMaterial = *(const uint32_t *)g_zglMatGetPluginData(tri->material);
    return &s_faceMaterial;
}

/* VA 0x00466bf0, 32 bytes, [V] direct, status draft */
int zbspMatRefList(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 1996, "mat ref list");
    return 0;
}

/* VA 0x00466c20, 32 bytes, [V] direct, status draft */
int zbspFindTagged2(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2017, "find tagged");
    return 0;
}

/* VA 0x00466c50, 64 bytes, [I] between, status draft */
void zbspFaceCentroid(const NgColFace *face, RwV3d *out)
{
    zmathVecZero(out);
    zmathVecAdd(out, out, (const RwV3d *)face->v0);
    zmathVecAdd(out, out, (const RwV3d *)face->v1);
    zmathVecAdd(out, out, (const RwV3d *)face->v2);
    zmathVecScale(out, out, 0.33333334f);
}

/* VA 0x00466ca0, 32 bytes, [V] direct, status draft */
int zbspDoesVertExist(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2087, "does vert exist");
    return 0;
}

/* VA 0x00466cd0, 35 bytes, [V] direct, status draft */
int zbspFacesAtSamePos(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2107, "faces at same pos");
    return 1;
}

/* VA 0x00466d10, 32 bytes, [V] direct, status draft */
int zbspSceneSaveState(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2161, "scene save state");
    return 0;
}

/* VA 0x00466d40, 30 bytes, [V] direct, status draft */
void zbspSceneRestoreState(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2175, "scene restore state");
}

/* VA 0x00466d70, 261 bytes, [I] callers, status draft */
void zbspClosestPointOnSegment(const float *a, const float *b, const float *p, float *out)
{
    /* the point of segment a-b nearest to p */
    RwV3d d;
    float t;

    d.x = b[0] - a[0]; d.y = b[1] - a[1]; d.z = b[2] - a[2];
    zmathVecNormalize(&d, &d);
    if ((a[0] - p[0]) * d.x + (a[1] - p[1]) * d.y + (a[2] - p[2]) * d.z >= 0.0f) {
        out[0] = a[0]; out[1] = a[1]; out[2] = a[2];
        return;
    }
    t = (b[0] - p[0]) * d.x + (b[1] - p[1]) * d.y + (b[2] - p[2]) * d.z;
    if (t > 0.0f) {
        out[0] = b[0] - t * d.x; out[1] = b[1] - t * d.y; out[2] = b[2] - t * d.z;
        return;
    }
    out[0] = b[0]; out[1] = b[1]; out[2] = b[2];
}

/* VA 0x00466ed0, 1060 bytes, [V] direct, status draft */
NgColEdge **zbspFindEdgesNear(const float *pos, float radius)
{
    /* Grind edges within radius of pos: every Tag._sd edge with a kind (byte 2) != 0 (its record
     * cached in the face's runtime[edge]) and the rails of the props that are not moving,
     * knocked, flag 0x100 or hidden. NULL-terminated, static. */
    uint32_t n = ztaginfGetFaceCount();
    ZtagFace *f = ztaginfGetFaces();
    void **out = s_list;
    float q[3];
    int e;

    s_list[0] = NULL;
    if (n == 0)
        return (NgColEdge **)s_list;
    if (g_zbspEdgePool == NULL)
        g_zbspEdgePool = zmemFreeListCreate(sizeof(NgColEdge), 0x100, NULL);
    for (; n != 0; n--, f++) {
        for (e = 0; e < 3; e++) {
            const uint8_t *tag = f->edgeTag[e];
            const ZtagVec *a = &f->v[e], *b = &f->v[(e + 1) % 3];

            if (tag == NULL || tag[2] == 0)
                continue;
            zbspClosestPointOnSegment(&a->x, &b->x, pos, q);
            q[0] -= pos[0]; q[1] -= pos[1]; q[2] -= pos[2];
            if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] >= radius * radius)
                continue;
            if (f->runtime[e] == 0)
                f->runtime[e] = (uint32_t)(uint32_t)zmemFreeListAlloc(g_zbspEdgePool);
            makeEdge((NgColEdge *)(uint32_t)f->runtime[e], a, b, tag);
            if (out > LIST_END)
                zAssertFail("edge list overflow", NGIFZBSP_SRC, 0x91F, 0);
            *out++ = (void *)(uint32_t)f->runtime[e];
            *out = NULL;
        }
    }
    {
        NgColEdge *r;

        for (r = (NgColEdge *)zHeapListFirst(g_zrollerEdges); r != NULL;
             r = (NgColEdge *)zHeapListNext(g_zrollerEdges)) {
            uint16_t flags = ((NgPropCol *)r->owner)->flags;
            float a[3], b[3];

            if ((flags & (NGPROPCOL_MOVING | NGPROPCOL_OFF)) || (flags & (NGPROPCOL_FLAG100 | NGPROPCOL_HIDDEN)))
                continue;
            a[0] = r->v0[0]; a[1] = r->v0[1]; a[2] = r->v0[2];
            b[0] = r->v1[0]; b[1] = r->v1[1]; b[2] = r->v1[2];
            zbspClosestPointOnSegment(a, b, pos, q);
            q[0] -= pos[0]; q[1] -= pos[1]; q[2] -= pos[2];
            if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] >= radius * radius)
                continue;
            if (out > LIST_END)
                zAssertFail("edge list overflow", NGIFZBSP_SRC, 0x940, 0);
            *out++ = r;
            *out = NULL;
        }
    }
    return (NgColEdge **)s_list;
}

/* VA 0x00467410, 497 bytes, [I] between, status draft */
NgColEdge *zbspGetTaggedEdge(uint32_t ctx, int16_t code, int index)
{
    /* the index-th Tag._sd edge whose code is `code` (static record; physics' spine faces) */
    uint32_t n = ztaginfGetFaceCount(), i;
    ZtagFace *f = ztaginfGetFaces();
    int found = 0, e;

    (void)ctx;
    for (i = 0; i < n; i++, f++) {
        for (e = 0; e < 3; e++) {
            const uint8_t *tag = f->edgeTag[e];

            if (tag == NULL || *(const int16_t *)tag != code)
                continue;
            if (found == index) {
                makeEdge(&s_taggedEdge, &f->v[e], &f->v[(e + 1) % 3], tag);
                return &s_taggedEdge;
            }
            found++;
        }
    }
    return NULL;
}

/* VA 0x00467690, 23 bytes, [I] between, status draft */
int zbspEdgeOpenEnd(const NgColEdge *e)
{
    /* rail ends that may be ridden off: 1 low end, -1 high end, 0 none */
    if (e->flags & 1)
        return 1;
    return (e->flags & 2) ? -1 : 0;
}

/* VA 0x004676c0, 30 bytes, [V] direct, status draft */
void zbspGetNormalVector(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2507, "get normal vector");
}

/* VA 0x004676f0, 35 bytes, [V] direct, status draft */
int zbspGetSideVector(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2522, "get side vector");
    return 1;
}

/* VA 0x00467730, 32 bytes, [V] direct, status draft */
int zbspBspcollide(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2556, "bspcollide");
    return 0;
}

/* VA 0x00467760, 30 bytes, [V] direct, status draft */
void zbspCreateSpanPlanes(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2576, "create span planes");
}

/* VA 0x00467790, 30 bytes, [V] direct, status draft */
void zbspAddToFacelist(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2593, "add to facelist");
}

/* VA 0x004677c0, 30 bytes, [V] direct, status draft */
void zbspGetTerrainHeightinf(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2600, "get terrain heightinf");
}

/* VA 0x004677f0, 30 bytes, [V] direct, status draft */
void zbspTestRayCollision(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2609, "test ray collision");
}

/* VA 0x00467820, 30 bytes, [V] direct, status draft */
void zbspBsptest(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2624, "bsptest");
}

/* VA 0x00467850, 30 bytes, [V] direct, status draft */
void zbspTestresetfacecolors(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2638, "testresetfacecolors");
}

/* VA 0x00467880, 30 bytes, [V] direct, status draft */
void zbspHilightnodefaces(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2654, "hilightnodefaces");
}

/* VA 0x004678b0, 30 bytes, [V] direct, status draft */
void zbspSetFaceRgb(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2670, "set face rgb");
}

/* VA 0x004678e0, 30 bytes, [V] direct, status draft */
void zbspPrintFaceListInf(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2684, "print face list inf");
}

/* VA 0x00467910, 32 bytes, [V] direct, status draft */
int zbspTrntestraycollision(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2704, "trntestraycollision");
    return 0;
}

/* VA 0x00467940, 30 bytes, [V] direct, status draft */
void zbspCreatenodeshit(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2722, "createnodeshit");
}

/* VA 0x00467970, 30 bytes, [V] direct, status draft */
void zbspAddToNodeslist(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2740, "add to nodeslist");
}

/* VA 0x004679a0, 30 bytes, [V] direct, status draft */
void zbspAddToFacesList(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2758, "add to faces list");
}

/* VA 0x004679d0, 35 bytes, [V] direct, status draft */
int zbspLineToNodes(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2774, "line to nodes");
    return 1;
}

/* VA 0x00467a10, 35 bytes, [V] direct, status draft */
int zbspLineToFaces(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2792, "line to faces");
    return 1;
}

/* VA 0x00467a50, 35 bytes, [V] direct, status draft */
int zbspLineplaneintersection(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2809, "lineplaneintersection");
    return 1;
}

/* VA 0x00467a90, 519 bytes, [I] between, status draft */
int zbspFacePointInside(const NgColFace *face, float x, float y, float z)
{
    /* Is (x, y, z), in the face's plane, inside the face? 2-D edge tests on the plane of the
     * normal's dominant axis, against the integer corners (4 with flag bit 0, else 3). */
    int px = (int)x, py = (int)y, pz = (int)z;
    int axis, s, n, i;
    const int16_t *prev, *cur;

    if (fabsf(face->normal[0]) < fabsf(face->normal[1]))
        axis = fabsf(face->normal[2]) < fabsf(face->normal[1]) ? 1 : 2;
    else
        axis = fabsf(face->normal[2]) <= fabsf(face->normal[0]) ? 0 : 2;
    s = face->normal[axis] > 1e-05f ? 1 : (face->normal[axis] < -1e-05f ? -1 : 0);
    n = (face->flags & 1) ? 4 : 3;
    prev = face->vtx[0];
    for (i = n - 1; i >= 0; i--) {
        int c;

        cur = face->vtx[i];
        if (axis == 0)
            c = (py - cur[1]) * (prev[2] - cur[2]) - (pz - cur[2]) * (prev[1] - cur[1]);
        else if (axis == 1)
            c = (prev[0] - cur[0]) * (pz - cur[2]) - (px - cur[0]) * (prev[2] - cur[2]);
        else
            c = (px - cur[0]) * (prev[1] - cur[1]) - (prev[0] - cur[0]) * (py - cur[1]);
        if (c != 0 && (c ^ -s) < 0)
            return 0;
        prev = cur;
    }
    return 1;
}

/* VA 0x00467d30, 35 bytes, [V] direct, status draft */
int zbspClipRayToObject(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2938, "clip ray to object");
    return 1;
}

/* VA 0x00467d70, 35 bytes, [V] direct, status draft */
int zbspClipLineToProps(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 2999, "clip line to props");
    return 1;
}

/* VA 0x00467db0, 32 bytes, [V] direct, status draft */
int zbspCliplinetoprop(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3016, "Cliplinetoprop");
    return 0;
}

/* VA 0x00467de0, 30 bytes, [V] direct, status draft */
void zbspBuildrayboundingbox(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3034, "buildrayboundingbox");
}

/* VA 0x00467e10, 39 bytes, [V] direct, status draft */
void zbspAddpropfaces(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3051, "addpropfaces");
}

/* VA 0x00467e50, 30 bytes, [V] direct, status draft */
void zbspComputeFacePlaneEq(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3068, "zbspComputeFacePlaneEq");
}

/* VA 0x00467e80, 30 bytes, [V] direct, status draft */
void zbspTaginitialize(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3089, "taginitialize");
}

/* VA 0x00467eb0, 30 bytes, [V] direct, status draft */
void zbspTagregisterprofunc(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3104, "tagregisterprofunc");
}

/* VA 0x00467ee0, 32 bytes, [V] direct, status draft */
int zbspFacetagregister(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3120, "facetagregister");
    return 0;
}

/* VA 0x00467f10, 32 bytes, [V] direct, status draft */
int zbspFacetagpreprocess(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3138, "facetagpreprocess");
    return 0;
}

/* VA 0x00467f40, 32 bytes, [V] direct, status draft */
int zbspFacetagprocess(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3156, "facetagprocess");
    return 0;
}

/* VA 0x00467f70, 32 bytes, [V] direct, status draft */
int zbspFacetagfinddef(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3173, "facetagfinddef");
    return 0;
}

/* VA 0x00467fa0, 32 bytes, [V] direct, status draft */
int zbspFacetagregregister(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3192, "facetagregregister");
    return 0;
}

/* VA 0x00467fd0, 32 bytes, [V] direct, status draft */
int zbspFacetagpreattachasref(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3214, "facetagpreattachasref");
    return 0;
}

/* VA 0x00468000, 32 bytes, [V] direct, status draft */
int zbspFacetagpreattachsmposasref(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3229, "facetagpreattachsmposasref");
    return 0;
}

/* VA 0x00468030, 35 bytes, [V] direct, status draft */
int zbspFacetagproswapwindow(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3249, "facetagproswapwindow");
    return 1;
}

/* VA 0x00468070, 1624 bytes, [V] direct, status draft */
ZbspSphereResult *zbspColSphere(const float *pos, float radius, float maxNy)
{
    /* The RW triangles within radius of pos (wall probes: physics, camera, the IK). For each one
     * with |normal.y| <= maxNy, in front of the centre (or any distance for prop triangles) and
     * nearer than radius: an NgColFace (tag code / flags from the nearest Tag._sd face within 2,
     * as the segment test does) and a hit at the nearest point - the centre projected on the
     * plane if that is inside the face, else the first corner, else the first edge point within
     * the sphere's circle on the plane. Returns NULL if nothing is hit. */
    int count = 0, any = 0, k;
    ZbspSphereHit *hit = s_sphereHits;
    NgColFace *face = s_sphereFaces;
    int16_t (*verts)[9] = s_sphereVerts;

    if (zrwColSphere((const RwV3d *)pos, radius + 1.0f, 3, 0, &s_rwTris) == 0)
        return NULL;
    zstringMemSet(&s_sphere, 0, 0x40);
    if (s_rwTris.count == 0)
        return NULL;
    for (k = s_rwTris.count - 1; k >= 0; k--) {
        const ZrwColTri *a = &s_rwTris.list[k];
        NgPropCol *prop = NULL;
        ZtagHit *tag;
        RwV3d c;
        float dist, r2, px, py, pz, hx, hy, hz;
        uint16_t code = 0;
        int i;

        if (face >= s_sphereFaces + 255)
            continue;
        if (a->model != NULL) {
            void *obj = g_zglModelGetObj(a->model);
            zListNode *node;

            for (node = zpropGetList()->head; node != NULL; node = node->next) {
                NgPropCol *col = propCol((Zprop *)node);

                if (((Zprop *)node)->info.obj == obj) {
                    prop = col;
                    break;
                }
            }
        }
        face->flags = 0;
        face->kind = 0;
        face->code = 0;
        c.x = (a->v[0][0] + a->v[1][0] + a->v[2][0]) * 0.33333334f;
        c.y = (a->v[0][1] + a->v[1][1] + a->v[2][1]) * 0.33333334f;
        c.z = (a->v[0][2] + a->v[1][2] + a->v[2][2]) * 0.33333334f;
        tag = ztaginfFindNearestFace(&c.x, 2.0f);
        if (tag != NULL) {
            if (tag->tag != NULL) {
                uint8_t t = tag->tag[3];

                face->flags |= (uint8_t)((((t & 1) << 1 | (t & 0xC)) << 1 | (t & 2)) << 2);
                code = *(const uint16_t *)tag->tag;
                if (*(const uint32_t *)(tag->tag + 8) == 0 && (code & 0xF000) == 0x1000)
                    code &= 0xFFF;
            }
            face->code = code;
        }
        face->normal[0] = a->normal[0];
        face->normal[1] = a->normal[1];
        face->normal[2] = a->normal[2];
        face->d = -zmathVecDot((const RwV3d *)a->normal, (const RwV3d *)a->v[0]);
        face->rwTri = a;
        for (i = 0; i < 3; i++) {
            face->vtx[i] = &verts[0][i * 3];
            verts[0][i * 3 + 0] = (int16_t)a->v[i][0];
            verts[0][i * 3 + 1] = (int16_t)a->v[i][1];
            verts[0][i * 3 + 2] = (int16_t)a->v[i][2];
        }
        if (fabsf(face->normal[1]) > maxNy + 0.001f)
            continue;
        dist = face->normal[1] * pos[1] + pos[2] * face->normal[2] + pos[0] * face->normal[0] + face->d;
        if (!(dist >= 0.0f || prop == NULL) || !(dist < radius))
            continue;
        r2 = radius * radius - dist * dist;
        px = pos[0] - dist * a->normal[0];
        py = pos[1] - dist * a->normal[1];
        pz = pos[2] - dist * a->normal[2];
        if (zbspFacePointInside(face, px, py, pz)) {
            hx = px; hy = py; hz = pz;
        } else {
            int n = (face->flags & 1) ? 4 : 3, ix = (int)px, iy = (int)py, iz = (int)pz, found = 0;
            float q[3], pp[3];

            for (i = n - 1; i >= 0 && !found; i--) {
                const int16_t *v = face->vtx[i];
                int d2 = (v[2] - iz) * (v[2] - iz) + (v[1] - iy) * (v[1] - iy) + (v[0] - ix) * (v[0] - ix);

                if ((float)d2 < r2) {
                    hx = a->v[i][0]; hy = a->v[i][1]; hz = a->v[i][2];
                    found = 1;
                }
            }
            pp[0] = px; pp[1] = py; pp[2] = pz;
            for (i = n - 1; i >= 0 && !found; i--) {
                const int16_t *v0 = face->vtx[i], *v1 = face->vtx[(i + 1) % 3];
                float fa[3], fb[3];

                fa[0] = v0[0]; fa[1] = v0[1]; fa[2] = v0[2];
                fb[0] = v1[0]; fb[1] = v1[1]; fb[2] = v1[2];
                zbspClosestPointOnSegment(fa, fb, pp, q);
                q[0] -= px; q[1] -= py; q[2] -= pz;
                if ((float)(int)(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) < r2) {
                    hx = q[0] + px; hy = q[1] + py; hz = q[2] + pz;
                    found = 1;
                }
            }
            if (!found)
                continue;
        }
        hit->face = face;
        hit->prop = prop;
        hit->dist = dist;
        hit->point.x = hx;
        hit->point.y = hy;
        hit->point.z = hz;
        count++;
        any = 1;
        face++;
        verts++;
        hit++;
    }
    if (!any)
        return NULL;
    if (count > 0xFF)
        zAssertFail("Buffer overflow", NGIFZBSP_SRC, 0xDE9, 0);
    s_sphere.hits = s_sphereHits;
    s_sphereHits[count].face = NULL;
    return &s_sphere;
}

/* VA 0x00468870, 30 bytes, [V] direct, status draft */
void zbspColspherecreatenodeshit(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3582, "colspherecreatenodeshit");
}

/* VA 0x004688a0, 35 bytes, [V] direct, status draft */
int zbspHitnodeslist(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3596, "hitnodeslist");
    return 1;
}

/* VA 0x004688e0, 35 bytes, [V] direct, status draft */
int zbspTestwithfacelist(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3611, "testwithfacelist");
    return 1;
}

/* VA 0x00468920, 434 bytes, [I] between, status draft */
void zbspEdgeClosestPoint(const int16_t *v0, const int16_t *v1, const float *dir, const float *p, float *out)
{
    /* nearest point of edge v0-v1 (direction dir) to p, in integer steps (physics, the IK) */
    int dx = (int)((float)v0[0] - p[0]), dy = (int)((float)v0[1] - p[1]), dz = (int)((float)v0[2] - p[2]);
    float t;

    if ((float)dx * dir[0] + (float)dz * dir[2] + (float)dy * dir[1] >= 0.0f) {
        out[0] = (float)v0[0]; out[1] = (float)v0[1]; out[2] = (float)v0[2];
        return;
    }
    dx = (int)((float)v1[0] - p[0]);
    dy = (int)((float)v1[1] - p[1]);
    dz = (int)((float)v1[2] - p[2]);
    t = (float)dx * dir[0] + (float)dz * dir[2] + (float)dy * dir[1];
    if (t > 0.0f) {
        out[0] = (float)(v1[0] - (int)(t * dir[0]));
        out[1] = (float)(v1[1] - (int)(t * dir[1]));
        out[2] = (float)(v1[2] - (int)(t * dir[2]));
        return;
    }
    out[0] = (float)v1[0]; out[1] = (float)v1[1]; out[2] = (float)v1[2];
}

/* VA 0x00468b50, 34 bytes, [V] direct, status draft */
void zbspFaceboundingbox(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3711, "faceboundingbox");
}

/* VA 0x00468b90, 30 bytes, [V] direct, status draft */
void zbspColSphereDraw(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 3726, "zbspColSphereDraw");
}

/* VA 0x00468bc0, 272 bytes, [I] between, status draft */
NgPropCol *zbspPropColCreate(Zprop *z)
{
    /* The prop's collision record (zprop user data, freed by zbspPropColFree): position, rotation,
     * scale 1, the def index and the OBJI flags: byte 7 bits 0-4 = group, byte 6 bit 0 -> 0x400,
     * byte 4 bit 0 -> 0x800, byte 5 bit 0 -> 0x1000 (the last two never read). */
    const uint8_t *obji = z->info.obji;
    NgPropCol *col = (NgPropCol *)zstringMemSet(zmemAlloc(sizeof(NgPropCol)), 0, sizeof(NgPropCol));
    const float *pos = (const float *)((uint8_t *)z->info.obj + 0x08);

    z->info.user = col;
    z->info.userFree = (void (__cdecl *)(void *))zbspPropColFree;
    col->pos[0] = (int16_t)pos[0];
    col->pos[1] = (int16_t)pos[1];
    col->pos[2] = (int16_t)pos[2];
    col->scale[0] = col->scale[1] = col->scale[2] = 1.0f;
    if (obji == NULL) {
        col->flags &= 0xE3E0;
    } else {
        col->flags = (uint16_t)((col->flags & 0xE3E0) | (obji[7] & NGPROPCOL_GROUP) |
                                (obji[6] & 1) << 10 | (obji[4] & 1) << 11 | (obji[5] & 1) << 12);
    }
    col->rails = NULL;
    zquatToEuler(col->rot, (Zquat *)((uint8_t *)z->info.obj + 0x14), 0);
    col->prop = z;
    col->defIndex = (uint16_t)zbspGetPropDefIndex(z->info.def);
    return col;
}

/* VA 0x00468d20, 183 bytes, [I] between, status draft */
void zbspPropColFree(NgPropCol *col)
{
    /* the zprop user-data destructor: unlink the prop's rails from g_zrollerEdges, free */
    int n = 0, i;

    if (col == NULL)
        return;
    if (col->defIndex != 0xFFFF) {
        switch (zcolGetPropShape(col->defIndex)->cls) {
        case NGSHAPE_BOX: n = 4; break;
        case NGSHAPE_RAIL:
        case NGSHAPE_EDGE: n = 1; break;
        default: n = 0; break;
        }
    }
    if (col->rails != NULL && n != 0) {
        for (i = 0; i < n; i++) {
            void *e;

            for (e = zHeapListFirst(g_zrollerEdges); e != NULL; e = zHeapListNext(g_zrollerEdges)) {
                if (e == (void *)&((NgColEdge *)col->rails)[i]) {
                    zHeapListRemoveCurrent(g_zrollerEdges);
                    break;
                }
            }
        }
        zmemFree(col->rails);
    }
    zmemFree(col);
}

/* VA 0x00468e30, 137 bytes, [V] direct, status draft */
NgPropCol **zbspFindPropsByGroup(uint16_t lo, uint16_t hi)
{
    /* the collision records of the props whose OBJI byte 7 is in lo..hi (prop.c / game.c / physics);
     * NULL if none */
    zListNode *node;
    void **out = s_list;
    uint32_t n = 0;

    for (node = zpropGetList()->head; node != NULL; node = node->next) {
        Zprop *z = (Zprop *)node;
        NgPropCol *col = propCol(z);

        if (z->info.obji != NULL && z->info.obji[7] >= lo && z->info.obji[7] <= hi) {
            if (n > 0xFF)
                zAssertFail("prop list overflow", NGIFZBSP_SRC, 0xF21, 0);
            *out++ = col;
            n++;
            *out = NULL;
        }
    }
    return n != 0 ? (NgPropCol **)s_list : NULL;
}

/* VA 0x00468ef0, 319 bytes, [V] direct, status draft */
NgColFace **zbspFindTaggedFaces(int lo, int hi)
{
    /* Faces whose tag id (code & 0xFFF) is in lo..hi: Tag._sd faces (NgColFace cached in the
     * face's runtime[3]), then the props whose OBJI switch id (u16 at +8, & 0xFFF) is in range
     * (a fresh entry of s_propFaces per call, src NULL, prop = the record; the counter is never
     * reset). game.c: objective faces; physics.c: respawn faces. */
    uint32_t n = ztaginfGetFaceCount(), count = 0;
    ZtagFace *f = ztaginfGetFaces();
    void **out = s_list;
    zListNode *node;

    s_list[0] = NULL;
    if (n == 0)
        return (NgColFace **)s_list;
    for (; n != 0; n--, f++) {
        const uint16_t *tag = (const uint16_t *)f->faceTag;

        if (tag == NULL || (int)(*tag & 0xFFF) < lo || (int)(*tag & 0xFFF) > hi)
            continue;
        if (f->runtime[3] == 0)
            f->runtime[3] = (uint32_t)(uint32_t)zmemFreeListAlloc(g_zbspFacePool);
        count++;
        zbspMakeTagFace(f, tag, (NgColFace *)(uint32_t)f->runtime[3]);
        if (count > 0xFF)
            zAssertFail("prop list overflow", NGIFZBSP_SRC, 0xF61, 0);
        *out++ = (void *)(uint32_t)f->runtime[3];
        *out = NULL;
    }
    for (node = zpropGetList()->head; node != NULL; node = node->next) {
        Zprop *z = (Zprop *)node;
        NgPropCol *col = propCol(z);
        uint32_t id;

        if (z->info.obji == NULL)
            continue;
        id = *(const uint16_t *)(z->info.obji + 8) & 0xFFF;
        if ((int)id < lo || (int)id > hi)
            continue;
        s_propFaces[s_propFaceCount].prop = col;
        *out++ = &s_propFaces[s_propFaceCount++];
        *out = NULL;
    }
    return (NgColFace **)s_list;
}

/* VA 0x00469080, 252 bytes, [I] between, status draft */
void zbspMakeTagFace(const ZtagFace *f, const uint16_t *tag, NgColFace *face)
{
    /* an NgColFace from a Tag._sd face: unit normal, d = +dot(v0, n) (the segment test stores
     * -dot), corners as floats and integers */
    RwV3d n;
    int i;

    face->flags = 0;
    face->code = *tag;
    zmathTriangleNormal(&n, (const RwV3d *)&f->v[0], (const RwV3d *)&f->v[1], (const RwV3d *)&f->v[2]);
    zmathVecNormalize(&n, &n);
    face->normal[0] = n.x; face->normal[1] = n.y; face->normal[2] = n.z;
    face->d = zmathVecDot((const RwV3d *)&f->v[0], &n);
    face->v0[0] = f->v[0].x; face->v0[1] = f->v[0].y; face->v0[2] = f->v[0].z;
    face->v1[0] = f->v[1].x; face->v1[1] = f->v[1].y; face->v1[2] = f->v[1].z;
    face->v2[0] = f->v[2].x; face->v2[1] = f->v[2].y; face->v2[2] = f->v[2].z;
    for (i = 0; i < 3; i++) {
        face->vtx[i] = face->ivtx[i];
        face->ivtx[i][0] = (int16_t)(&f->v[i].x)[0];
        face->ivtx[i][1] = (int16_t)(&f->v[i].x)[1];
        face->ivtx[i][2] = (int16_t)(&f->v[i].x)[2];
    }
    face->tagFace = f;
    face->kind = 0;
    face->prop = NULL;
}

/* VA 0x004691c0, 655 bytes, [V] direct, status draft */
NgColEdge **zbspFindTaggedEdges(uint16_t lo, uint16_t hi)
{
    /* Tag._sd edges with a kind and a code in lo..hi (game.c: objective edges) */
    uint32_t n = ztaginfGetFaceCount();
    ZtagFace *f = ztaginfGetFaces();
    void **out = s_list;
    int e;

    s_list[0] = NULL;
    if (n == 0)
        return (NgColEdge **)s_list;
    if (g_zbspEdgePool == NULL)
        g_zbspEdgePool = zmemFreeListCreate(sizeof(NgColEdge), 0x100, NULL);
    for (; n != 0; n--, f++) {
        for (e = 0; e < 3; e++) {
            const uint8_t *tag = f->edgeTag[e];

            if (tag == NULL || tag[2] == 0 || *(const uint16_t *)tag < lo || *(const uint16_t *)tag > hi)
                continue;
            if (f->runtime[e] == 0)
                f->runtime[e] = (uint32_t)(uint32_t)zmemFreeListAlloc(g_zbspEdgePool);
            makeEdge((NgColEdge *)(uint32_t)f->runtime[e], &f->v[e], &f->v[(e + 1) % 3], tag);
            if (out > LIST_END)
                zAssertFail("edge list overflow", NGIFZBSP_SRC, 0xFF6, 0);
            *out++ = (void *)(uint32_t)f->runtime[e];
            *out = NULL;
        }
    }
    return (NgColEdge **)s_list;
}

/* VA 0x00469500, 32 bytes, [V] direct, status draft */
int zbspFindFaceByType(void)
{
    NGIF_MISSING(NGIFZBSP_SRC, 4108, "zbspFindFaceByType()");
    return 0;
}

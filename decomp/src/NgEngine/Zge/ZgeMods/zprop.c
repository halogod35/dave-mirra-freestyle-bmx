/*
 * NgEngine/Zge/ZgeMods/zprop.c
 * Original: C:\NgEngine\Zge\ZgeMods\zprop.c
 * Evidence: [V] path string at 0x00560cb0 (asserts in zpropLoadDef/zpropLoad); the accessors [I]
 * (link order, globals 0x006722B8/0x006722C4).
 * Draft. Non-matching C (AD-8). Types: decomp/include/zprop.h.
 * Formats: docs/formats/LEVEL.md 5.1 (PropDef._sd) and 5.2 (Prop._sd); Python reference:
 * tools/dmbmx/formats/level.py PropDefSd / PropSd (the code below agrees with it; notes inline).
 *
 * zsceneLoadComponent calls zpropLoadDef for every top-level PDBL form of PropDef._sd and zpropLoad
 * for every PRBL form of Prop._sd (handler tables in zScene.c). Each handler is entered with the
 * form as the current child of the IFF reader.
 *
 * zpropListsInit / zpropShutdown / zpropListsFree (0x00486910-0x00486A1C) were in zmem.c (the skeleton
 * attribution; the zmem pass drafted them there too): they only touch this file's two lists, so the zScene pass
 * moved them here.
 * 8 functions, VA 0x00486910-0x00486ed5.
 */
#include "decomp.h"

/* other modules (current names until they are decompiled) */
int    zrwZglGetValue(int what, void *out, int arg);       /* zgl getter; 1 = world scale (100.0) */
void  *zobjModelLoadIff(ZiffFile *iff, int flags);          /* 0x00489090 reads the MODL chunk */
void   zrwModelSetFlags(void *model, int what, int value); /* model render flag setter [I] */
void   zobjSetPosition(void *obj, const float *pos);          /* set position */
void   zobjSetRotation(void *obj, const float *quat);         /* set rotation */
void   zmathMatrixIdentity(float *m44);                     /* zmath.c 0x00481880 */
void   zmathMatrixToEuler(int16_t *euler, const float *m44, int mode); /* 0x00481FC0 (mode unused) */
void   zquatFromEuler(float *q, const int16_t *euler, int mode);  /* zquat.c 0x004832B0 */

#define ZPROP_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zprop.c"
#define ID_PDIN 0x4E494450u   /* 'PDIN' */
#define ID_MODL 0x4C444F4Du   /* 'MODL' */
#define ID_OBJI 0x494A424Fu   /* 'OBJI' */
#define ID_PRBL 0x4C425250u   /* 'PRBL' */
#define ZGL_WORLD_SCALE 1

zList g_zpropDefs;        /* 0x006722B8 ZpropDef list */
zList g_zprops;           /* 0x006722C4 Zprop list */

/* VA 0x00486910, 29 bytes, [I] row, status draft */
int zpropListsInit(void)
{
    /* zsceneInit */
    zListInit(&g_zpropDefs);
    zListInit(&g_zprops);
    return 1;
}

/* VA 0x00486930, 11 bytes, [I] row, status draft */
int zpropShutdown(void)
{
    /* zsceneShutdown; only reached by a direct call (no function in an older Ghidra DB) */
    zpropListsFree();
    return 1;
}

/* VA 0x00486940, 213 bytes, [I] row, status draft */
void zpropListsFree(void)
{
    /* zsceneClose: frees every prop, then every def; a node's attached blocks go to its *Free
     * callback when one is set, else to zmemFree. The def models are released separately as
     * scene templates (zsceneFreeObjects). */
    zListNode *node;

    while ((node = g_zprops.head) != 0) {
        Zprop *p = (Zprop *)node;
        zListRemove(&g_zprops, node);
        if (p->info.objiFree)
            p->info.objiFree(p->info.obji);
        else if (p->info.obji)
            zmemFree(p->info.obji);
        if (p->info.userFree)
            p->info.userFree(p->info.user);
        else if (p->info.user)
            zmemFree(p->info.user);
        zmemFree(p);
    }
    g_zprops.head = 0;
    while ((node = g_zpropDefs.head) != 0) {
        ZpropDef *d = (ZpropDef *)node;
        zListRemove(&g_zpropDefs, node);
        if (d->info.objiFree)
            d->info.objiFree(d->info.obji);
        else if (d->info.obji)
            zmemFree(d->info.obji);
        if (d->info.collisionFree)
            d->info.collisionFree(d->info.collision);
        else if (d->info.collision)
            zmemFree(d->info.collision);
        zmemFree(d);
    }
    g_zpropDefs.head = 0;
}

/* reads an optional OBJI chunk of the current form into a new heap block (NULL if absent) */
static uint8_t *zpropReadObji(ZiffFile *iff)
{
    ZobjiHeader hdr;
    uint8_t *data;

    if (!ziffChunkFindFirst(iff, ID_OBJI))
        return 0;
    ziffChunkReadData(iff, &hdr, sizeof(hdr));     /* hdr.hdrSize (8) is not checked */
    data = (uint8_t *)zmemAlloc(hdr.size);
    ziffChunkReadData(iff, data, hdr.size);
    return data;
}

/* VA 0x00486a20, 436 bytes, [I] row, status draft */
int zpropLoadDef(ZiffFile *iff)
{
    ZpropPdinRecord pdin;
    ZpropDef *def;

    if (!ziffChunkOpen(iff))
        return 1;
    def = (ZpropDef *)zstringMemSet(zmemAlloc(sizeof(ZpropDef)), 0, sizeof(ZpropDef));

    /* PDIN is searched from the start of the form, so chunk order inside PDBL does not matter */
    if (!ziffChunkFindFirst(iff, ID_PDIN)) {
        zAssertFail("zprop : Prop Def has no data!!!", ZPROP_FILE, 0x94, 0);
        return 0;                                  /* def is leaked and the form stays open */
    }
    ziffChunkReadData(iff, &pdin, sizeof(pdin));
    if (pdin.size != sizeof(pdin)) {
        zAssertFail("zprop : Prop Def header size invalid", ZPROP_FILE, 0x97, 0);
        return 0;
    }
    def->info.id = pdin.id;
    def->info.extents[0] = pdin.extents[0];
    def->info.extents[1] = pdin.extents[1];
    def->info.extents[2] = pdin.extents[2];
    def->info.extra[0] = pdin.extra[0];            /* LEVEL.md called these 16 bytes junk: they are */
    def->info.extra[1] = pdin.extra[1];            /* floats, read by zbspInitPropDefShape when the def */
    def->info.extra[2] = pdin.extra[2];            /* has an OBJI (box centre offset); stale memory */
    def->info.extra[3] = pdin.extra[3];            /* in defs without OBJI, which never read them */
    zstringCopy(def->info.name, pdin.name);        /* strcpy: stops at the NUL, junk after it is dropped */

    if (!ziffChunkFindFirst(iff, ID_MODL)) {
        zAssertFail("zprop : Prop Def has no geometry!!!", ZPROP_FILE, 0xA7, 0);
        return 0;
    }
    def->info.model = zobjModelLoadIff(iff, 0);
    zrwModelSetFlags(def->info.model, 2, 0);
    zrwModelSetFlags(def->info.model, 8, 0);

    def->info.obji = zpropReadObji(iff);
    def->info.collision = 0;
    def->info.collisionFree = 0;
    def->info.objiFree = 0;
    zListAddTail(&g_zpropDefs, &def->link);
    /* register the model as a scene template so zsceneCreateModel(name) finds it by id */
    zsceneAddObject(0, pdin.id, ZSCENE_OBJ_MODEL, 1, def->info.model);
    ziffChunkClose(iff);
    return 1;
}

/* VA 0x00486be0, 696 bytes, [I] row, status draft */
int zpropLoad(ZiffFile *iff)
{
    ZpropPrblRecord rec;
    zListNode *node;
    ZpropDefInfo *def;
    Zprop *prop;
    void *obj;
    float scale;
    float pos[3];
    float m[16];
    int16_t euler[3];
    float quat[4];

    if (!ziffChunkOpen(iff))
        return 1;
    /* the outer form and the inner record share the tag PRBL; FindFirst finds the inner one */
    if (!ziffChunkFindFirst(iff, ID_PRBL)) {
        zAssertFail("zprop : Prop has no data!!!", ZPROP_FILE, 0xDE, 0);
        return 0;
    }
    ziffChunkReadData(iff, &rec, sizeof(rec));
    if (rec.size != sizeof(rec)) {
        zAssertFail("zprop : Prop header size invalid", ZPROP_FILE, 0xE0, 0);
        return 0;
    }
    /* linear search of the def list (zpropFindDef inlined) */
    def = 0;
    for (node = g_zpropDefs.head; node; node = node->next) {
        if (((ZpropDef *)node)->info.id == rec.defId) {
            def = &((ZpropDef *)node)->info;
            break;
        }
    }
    if (!def) {
        zAssertFail("zprop : No prop def corresponds to the prop", ZPROP_FILE, 0xE4, 0);
        return 0;
    }

    prop = (Zprop *)zstringMemSet(zmemAlloc(sizeof(Zprop)), 0, sizeof(Zprop));
    zstringMemCopy(prop->info.name, rec.name, sizeof(rec.name));   /* all 16 bytes, junk included */
    prop->info.defId = rec.defId;
    prop->info.user = 0;
    prop->info.obji = 0;
    zListAddTail(&g_zprops, &prop->link);

    zrwZglGetValue(ZGL_WORLD_SCALE, &scale, 0);
    pos[0] = rec.pos[0] * scale;                   /* ZGE/100 -> ZGE units (LEVEL.md 2) */
    pos[1] = rec.pos[1] * scale;
    pos[2] = rec.pos[2] * scale;
    prop->info.def = def;
    /* an instance of the def's model, found through the template zpropLoadDef registered */
    obj = zsceneCreateModel(0, def->name);
    prop->info.obj = obj;
    if (!obj) {
        zAssertFail("zprop : Unable to create an obj based on the prop def model", ZPROP_FILE, 0xF9, 0);
    } else {
        zmathMatrixIdentity(m);
        zobjSetPosition(obj, pos);
        /*
         * The 3x3 rows are copied into the rows of a 4x4 (no transpose), converted to euler angles
         * by zmathMatrixToEuler (x = atan2(m[6], m[10]), y = atan2(-m[2], |m[0], m[1]|),
         * z = atan2(m[1], m[0])) and back to a quaternion by zquatFromEuler (q = qz * qy * qx).
         * zquatToMatrix of that quaternion reproduces the stored rows: the chain is the identity
         * up to the 16-bit angle quantisation (test_prop_rotation_chain_is_identity in tests/test_decomp_zscene.py).
         */
        m[0] = rec.matrix[0]; m[1] = rec.matrix[1]; m[2]  = rec.matrix[2];
        m[4] = rec.matrix[3]; m[5] = rec.matrix[4]; m[6]  = rec.matrix[5];
        m[8] = rec.matrix[6]; m[9] = rec.matrix[7]; m[10] = rec.matrix[8];
        zmathMatrixToEuler(euler, m, 0);
        zquatFromEuler(quat, euler, 0);
        zobjSetRotation(obj, quat);
    }
    prop->info.obji = zpropReadObji(iff);
    prop->info.userFree = 0;
    prop->info.objiFree = 0;
    ziffChunkClose(iff);
    return 1;
}

/* VA 0x00486ea0, 32 bytes, [I] row, status draft */
ZpropDefInfo *zpropFindDef(uint32_t id)
{
    zListNode *node;

    for (node = g_zpropDefs.head; node; node = node->next) {
        if (((ZpropDef *)node)->info.id == id)
            return &((ZpropDef *)node)->info;
    }
    return 0;
}

/* VA 0x00486ec0, 6 bytes, [I] row, status draft */
zList *zpropGetDefList(void)
{
    return &g_zpropDefs;
}

/* VA 0x00486ed0, 6 bytes, [I] row, status draft */
zList *zpropGetList(void)
{
    return &g_zprops;
}

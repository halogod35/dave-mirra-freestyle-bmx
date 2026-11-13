/*
 * NgEngine/Zge/zScene.c
 * Original: C:\NgEngine\Zge\zScene.c
 * Evidence: [V] path string referenced by 16 functions; the others [I] (link order, the globals
 * 0x00673F70-0x00673FFB, the def/entry pools). The zScene pass moved 0x00491D80-0x00491EE0 to zscnpts.c.
 * 39 functions, VA 0x00490180-0x00491d7f.
 * Draft. Non-matching C (AD-8). Types: decomp/include/zscene.h.
 * Level format: docs/formats/LEVEL.md (sections 1 and 3); Python reference tools/dmbmx/formats/level.py.
 *
 * Two halves:
 *  - the scene database: Scene.Txt (via the ztoken parser 0x004A1120..) -> ZsceneDef tree;
 *    zsceneOpen(name) loads a SCENE's ZSD and its components (zsceneLoadComponent);
 *  - the object registry: every model/anim/light/camera/emitter created through this module is a
 *    ZsceneEntry in g_zscene.entryPool; zsceneClose frees the scene-owned ones.
 */
#include "decomp.h"
#include "zanim.h"

/* other modules (current names until they are decompiled) */
int      zfontInit(void);                                /* 0x004A27A0 zFont.c */
int      zfontShutdown(void);                            /* 0x004A2800 zFont.c */
int      ztokenOpen(const char *path);                        /* 0x004A1120 */
int      ztokenClose(void);                                   /* 0x004A11E0 */
int      ztokenNext(char *buf, int size);                     /* 0x004A1220 */
int      ztokenReadInt(int *value);                           /* 0x004A2330 decimal or 0x hex */
int      ztokenReadString(char *buf, int size);               /* 0x004A2440 */
int      ztokenExpect(char *token);                           /* 0x004A2460 upper-cases token in place */
void     ztokenPrintLocation(void);                           /* 0x004A24F0 "File:%s, Line:%d" */
void    *zobjModelLoadFile(const char *path, int flags);      /* 0x00489070 */
void     zobjModelFree(void *model);                          /* 0x004890B0 */
void    *zobjModelCreateInstance(void *model);                /* 0x004890C0 */
void    *zobjModelCreate(void *model);                        /* 0x00488FB0 zobj for a model instance */
void     zobjDestroy(void *obj);                            /* destroy a zobj */
void    *zobjGetHandle(void *obj);                            /* the model of a model zobj */
void    *zobjCameraCreate(void);                           /* create a camera zobj */
void    *zobjLightCreate(int type, const RwRGBA *colour);  /* create a light zobj (0 directional, 1 ambient, ..) */
void    *zobjPEmitCreate(void);                               /* 0x00495C10 create a particle emitter zobj */
ZanimContents *zanimLoadIff(ZiffFile *iff);
uint32_t zanimGetId(ZanimContents *anim);                     /* the DEFI id (used as the motion id) */
void     zanimFreeContents(ZanimContents *c);
int      zpanoramaLoad(ZiffFile *iff);                        /* 0x004A10A0 zpanorama.c: the SDPA handler (used in the table below) */

#define ZSCENE_FILE "C:\\NgEngine\\Zge\\zScene.c"
#define ID_FORM 0x4D524F46u   /* 'FORM' */
#define ID_ANIM 0x4D494E41u   /* 'ANIM' */
#define ID_TAGB 0x42474154u
#define ID_SCPT 0x54504353u
#define ID_SDPA 0x41504453u
#define ID_PDBL 0x4C424450u
#define ID_PRBL 0x4C425250u
#define ZSCENE_TOKEN 0x80

/* flags of zsceneCreateModel / zsceneLoadAnim / zsceneCreate{Camera,Light,Emitter} */
#define ZSCENE_NO_ENTRY   1   /* do not register (CreateModel..Emitter); LoadAnim: keep after zsceneClose */
#define ZSCENE_NO_OBJECT  2   /* CreateModel: do not create a zobj (returns the name, see there) */

ZsceneGlobals g_zscene;       /* 0x00673F70 */

/* level component handlers (0x00561900..0x00561927), see LEVEL.md 1 */
const ZsceneChunkHandler zsceneTagHandlers[1]     = { { ID_TAGB, (ZsceneChunkFn)ztaginfLoad } };
const ZsceneChunkHandler zsceneSceneHandlers[2]   = { { ID_SCPT, (ZsceneChunkFn)zscnptsLoad },
                                                      { ID_SDPA, (ZsceneChunkFn)zpanoramaLoad } };
const ZsceneChunkHandler zscenePropDefHandlers[1] = { { ID_PDBL, (ZsceneChunkFn)zpropLoadDef } };
const ZsceneChunkHandler zscenePropHandlers[1]    = { { ID_PRBL, (ZsceneChunkFn)zpropLoad } };

/* model/anim file loaders by extension (0x005618A0, 0x005618B0) and the kind table (0x005618D0) */
static ZsceneLoader zsceneModelLoaders[2] = { { "ZMD", zsceneLoadZmd }, { 0, 0 } };
static ZsceneLoader zsceneAnimLoaders[4]  = { { "NAD", zsceneLoadAnimFile }, { "IFF", zsceneLoadAnimFile },
                                              { "ZAL", zsceneLoadAnimZal }, { 0, 0 } };
static ZsceneKindInfo zsceneKinds[3] = { { "SCENE", 0 }, { "MODEL", zsceneModelLoaders },
                                         { "ANIM", zsceneAnimLoaders } };

int   zscnptsInit(void);
int   zscnptsShutdown(void);
void  zscnptsUnload(void);
int   zpanoramaInit(void);
int   zpanoramaShutdown(void);
void  zpanoramaClose(void);
int   zpanoramaLoad(ZiffFile *iff);
extern const ZrwWorldApi g_zrwWorldApi;

/* ------------------------------------------------------------------ definitions (Scene.Txt) */

/* VA 0x00490180, 38 bytes, [I] row, status draft */
ZsceneDef *zsceneAllocDef(int kind)
{
    ZsceneDef *def = (ZsceneDef *)zmemFreeListAlloc(g_zscene.defPool);
    zstringMemSet(def, 0, sizeof(*def));
    def->kind = kind;
    return def;
}

/* VA 0x004901b0, 26 bytes, [I] row, status draft */
int zsceneFreeDef(ZsceneDef *def)
{
    /* also passed to zmemFreeListCreate as the def pool's callback */
    zmemFreeListFree(g_zscene.defPool, def);
    return 1;
}

/* VA 0x004901d0, 18 bytes, [I] row, status draft */
void zsceneDefAddSibling(ZsceneDef *def, ZsceneDef *sibling)
{
    sibling->next = def->next;
    def->next = sibling;
}

/* VA 0x004901f0, 18 bytes, [I] row, status draft */
void zsceneDefAddChild(ZsceneDef *def, ZsceneDef *child)
{
    child->next = def->child;
    def->child = child;
}

/* VA 0x00490210, 8 bytes, [I] row, status draft */
ZsceneDef *zsceneDefGetNext(ZsceneDef *def)
{
    return def->next;
}

/* VA 0x00490220, 8 bytes, [I] row, status draft */
ZsceneDef *zsceneDefGetChild(ZsceneDef *def)
{
    return def->child;
}

/* VA 0x00490230, 674 bytes, [V] direct, status draft */
int zsceneParseObject(ZsceneDef *def)
{
    /*
     * Parses "{ NAME <s> FILE <s> TYPE <n> SUBTYPE <n> MODEL {..} ANIM {..} }" into def. Nested
     * blocks become children (AllocDef + DefAddChild inlined); their result is ignored. Returns 1
     * at "}", 0 on an unknown token or end of file.
     */
    char tok[ZSCENE_TOKEN];
    int value;

    if (!ztokenExpect("{"))
        return 0;
    while (ztokenNext(tok, ZSCENE_TOKEN)) {
        if (!zstringCompare(tok, "NAME")) {
            if (ztokenReadString(tok, ZSCENE_TOKEN)) {
                if (zstringLength(tok) > 31)
                    zAssertFail("Object name is too long.", ZSCENE_FILE, 0x126, 0);
                zstringCopy(def->name, tok);           /* copies anyway: overflows into def->file */
                def->id = zHashName(def->name);
            }
        } else if (!zstringCompare(tok, "FILE")) {
            if (ztokenReadString(tok, ZSCENE_TOKEN)) {
                if (zstringLength(tok) > 63)
                    zAssertFail("File name is too long.", ZSCENE_FILE, 0x133, 0);
                zstringCopy(def->file, tok);
            }
        } else if (!zstringCompare(tok, "TYPE")) {
            if (ztokenReadInt(&value))
                def->type = value;
        } else if (!zstringCompare(tok, "SUBTYPE")) {
            if (ztokenReadInt(&value))
                def->subtype = value;
        } else if (!zstringCompare(tok, "ANIM") || !zstringCompare(tok, "MODEL")) {
            ZsceneDef *child = zsceneAllocDef(tok[0] == 'A' ? ZSCENE_DEF_ANIM : ZSCENE_DEF_MODEL);
            zsceneDefAddChild(def, child);
            zsceneParseObject(child);
        } else if (!zstringCompare(tok, "}")) {
            /* the exe tests `def == &g_zscene.common || def->file` - the array address is never
             * NULL, so "**ERROR**: Missing data" is unreachable (probably meant def->file[0]) */
            if (def == &g_zscene.common || def->file)
                return 1;
            g_pfnDebugPrintf("**ERROR**: Missing data\n");
            ztokenPrintLocation();
            return 0;
        } else {
            g_pfnDebugPrintf("**ERROR** Unknown token:'%s'\n", tok);
            ztokenPrintLocation();
            return 0;
        }
    }
    return 0;
}

/* VA 0x004904e0, 323 bytes, [V] direct, status draft */
int zsceneLoadDatabase(char *fileName)
{
    /* Scene.Txt: SCENE_COMMON {..} SCENE {..} ... END */
    char tok[ZSCENE_TOKEN];
    ZsceneDef *def;

    if (!ztokenOpen(fileName)) {
        zAssertFail(zstringFormat("Database not found:%s", fileName), ZSCENE_FILE, 0x1D2, 0);
        return 0;
    }
    if (ztokenExpect("SCENE_COMMON") && zsceneParseObject(&g_zscene.common) &&
        ztokenNext(tok, ZSCENE_TOKEN)) {
        while (!zstringCompare(tok, "SCENE")) {
            def = zsceneAllocDef(ZSCENE_DEF_SCENE);
            if (!g_zscene.scenes)
                g_zscene.scenes = def;
            else
                zsceneDefAddSibling(g_zscene.scenes, def);   /* inserted after the first scene */
            if (!zsceneParseObject(def) || !ztokenNext(tok, ZSCENE_TOKEN))
                goto fail;
        }
        if (!zstringCompare(tok, "END")) {
            ztokenClose();
            return 1;
        }
    }
fail:
    ztokenClose();
    return 0;
}

/* VA 0x00490630, 105 bytes, [I] row, status draft */
ZsceneDef *zsceneFindDefRecurse(ZsceneDef *def, ZsceneDefKey *key)
{
    /* Depth-first (self, children, siblings). Returns NULL once found (key->found set), else def.
     * key->id == 0 matches the first def of the kind and marks the key with id 0xFFFFFFFF. */
    if (def->kind == key->kind) {
        if (key->id == 0) {
            key->id = 0xFFFFFFFFu;
            key->found = def;
            return 0;
        }
        if (def->id == key->id) {
            key->found = def;
            return 0;
        }
    }
    if (def->child && !zsceneFindDefRecurse(def->child, key))
        return 0;
    if (def->next && !zsceneFindDefRecurse(def->next, key))
        return 0;
    return def;
}

/* the inlined search the callers use: def itself (id 0 = any, without marking the key), then its
 * children and siblings */
static ZsceneDef *zsceneSearchDef(ZsceneDef *def, ZsceneDefKey *key)
{
    if (def->kind == key->kind && (key->id == 0 || def->id == key->id))
        return def;
    if (def->child && !zsceneFindDefRecurse(def->child, key))
        return key->found;
    if (def->next)
        zsceneFindDefRecurse(def->next, key);
    return key->found;
}

/* VA 0x004906a0, 48 bytes, [I] row, status draft */
int zsceneMatchEntry(ZsceneEntry *entry, ZsceneEntryKey *key)
{
    if (entry->type == key->type && entry->id == key->id) {
        key->found = entry;
        return 0;              /* stop the pool walk */
    }
    return 1;
}

/* VA 0x004906d0, 56 bytes, [I] row, status draft */
ZsceneEntry *zsceneFindEntry(int type, uint32_t id)
{
    ZsceneEntryKey key;

    key.type = type;
    key.id = id;
    key.found = 0;
    zmemFreeListForEach(g_zscene.entryPool, (ZmemFreeListIterFn)zsceneMatchEntry, &key);
    return key.found;
}

/* VA 0x00490710, 114 bytes, [V] direct, status draft */
ZsceneEntry *zsceneAddObject(ZsceneDef *def, uint32_t id, int type, int isTemplate, void *obj)
{
    ZsceneEntry *e;

    if (!g_zscene.current) {
        zAssertFail("No scene is open", ZSCENE_FILE, 0x264, 0);
        return 0;
    }
    e = (ZsceneEntry *)zmemFreeListAlloc(g_zscene.entryPool);
    e->id = id;
    e->def = def;
    e->obj = obj;
    e->type = (int8_t)type;
    e->isTemplate = (uint8_t)isTemplate;
    e->sceneOwned = 1;
    return e;
}

/* ------------------------------------------------------------------ level components */

/* VA 0x00490790, 405 bytes, [V] direct, status draft */
int zsceneLoadComponent(void *zsd, int zsdSize, const char *dir, int type, int nHandlers,
                        const ZsceneChunkHandler *handlers)
{
    /*
     * Loads the component file of SCFI `type` and runs each handler on every top-level chunk with
     * its id (LEVEL.md 1). Returns 0 when the ZSD has no SCFI of that type, 1 when its size is 0
     * (component skipped). zsdSize is not used (FindScfi takes the length from the SCEN header).
     */
    ZsdScfiRecord *scfi;
    uint32_t size;
    char path[0x80];
    void *buf;
    zfileHandle *file;
    ZiffFile *iff;

    (void)zsdSize;
    scfi = g_zrwWorldApi.findScfi(zsd, type, &size);
    if (!scfi)
        return 0;
    if (scfi->size == 0)
        return 1;
    zstringPrintf(path, "%s%s", dir, scfi->name);   /* dir from zfileGetDirectory ends with '\' */
    /* exactly `size` bytes: a shorter file fails, a longer one is truncated (LEVEL.md 1) */
    buf = zfileLoad(path, (int)size);
    if (!buf) {
        zAssertFail(zstringFormat("Scene component file not found:%s", path), ZSCENE_FILE, 0x2EE, 0);
        return 0;
    }
    file = zfileOpenMemory(buf, (int)size);
    if (!file) {
        zAssertFail("Couldn't open ramdisk file", ZSCENE_FILE, 0x2F0, 0);
        return 0;                                /* buf leaks */
    }
    iff = ziffOpen(file, ZIFF_ANY_ID, size);     /* headerless: the root is the whole file */
    if (iff) {
        for (; nHandlers; nHandlers--, handlers++) {
            int more = ziffChunkFindFirst(iff, handlers->id);
            while (more) {
                if (!handlers->fn(iff)) {
                    ziffClose(iff);
                    zfileClose(file, 0);
                    zfileFreeLoaded(buf);
                    return 0;
                }
                more = ziffChunkFindNext(iff, handlers->id);
            }
        }
        ziffClose(iff);
    }
    zfileClose(file, 0);
    zfileFreeLoaded(buf);
    return 1;
}

/* VA 0x00490930, 160 bytes, [I] row, status draft */
int zsceneLoadComponents(void *zsd, int zsdSize, const char *dir)
{
    /* Tag._sd, Scene._sd, PropDef._sd, Prop._sd in this order (Mat and BSP: zrwWorldOpen).
     * zsceneOpen inlines the same sequence but ignores the result. No caller in the exe [V]. */
    if (!zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_TAG, 1, zsceneTagHandlers)) {
        g_pfnDebugPrintf("zrwWorld: Failed to load scene tagging info\n");
        return 0;
    }
    if (zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_SCENE, 2, zsceneSceneHandlers) &&
        zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_PROPDEF, 1, zscenePropDefHandlers) &&
        zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_PROP, 1, zscenePropHandlers))
        return 1;
    g_pfnDebugPrintf("zrwWorld: Failed to load scene point\n");   /* also for PropDef/Prop */
    return 0;
}

/* ------------------------------------------------------------------ object registry */

/* VA 0x004909d0, 384 bytes, [V] direct, status draft */
void zsceneFreeObjects(int all)
{
    /* all = 0 (zsceneClose): the scene-owned entries; all = 1 (zsceneShutdown): every entry */
    void *it = zmemFreeListFirst(g_zscene.entryPool);

    while (it) {
        void *next = zmemFreeListNext(it);
        ZsceneEntry *e = (ZsceneEntry *)zmemFreeListNodeData(it);
        void *obj = e->obj;

        it = next;
        if (!e->sceneOwned && !all)
            continue;
        switch (e->type) {
        case ZSCENE_OBJ_MODEL:
            if (!e->isTemplate) {
                /* zsceneDestroyModel inlined; the entry is found by obj and freed */
                if (((void **)obj)[0x2C / 4])             /* zobj +0x2C: its model */
                    zsceneRemoveObject(ZSCENE_OBJ_MODEL, 0, obj, 0);
                zobjDestroy(obj);
            } else {
                zobjModelFree(obj);
                zsceneRemoveObject(e->type, 1, obj, e);
            }
            break;
        case ZSCENE_OBJ_ANIM:
            zsceneFreeAnim(e);                         /* inlined */
            break;
        case ZSCENE_OBJ_LIGHT:
        case ZSCENE_OBJ_CAMERA:
        case ZSCENE_OBJ_EMITTER:
            zsceneRemoveObject(e->type, 0, obj, 0);
            zobjDestroy(obj);
            break;
        default:
            zAssertFail(zstringFormat("Unknown scene EzsceneObjType:%d", (int)e->type), ZSCENE_FILE, 0x395, 0);
            break;
        }
    }
}

/* VA 0x00490b70, 251 bytes, [V] direct, status draft */
int zsceneRemoveObject(int type, int isTemplate, void *obj, ZsceneEntry *entry)
{
    /* Frees `entry`, or else the first entry of `type` holding obj (for models also with the
     * same isTemplate). Returns 0 with a warning if there is none. */
    void *it;

    if (!entry) {
        for (it = zmemFreeListFirst(g_zscene.entryPool); it; it = zmemFreeListNext(it)) {
            ZsceneEntry *e = (ZsceneEntry *)zmemFreeListNodeData(it);
            if (e->type != type)
                continue;
            switch (type) {
            case ZSCENE_OBJ_MODEL:
                if ((int8_t)e->isTemplate != isTemplate)
                    continue;
                break;
            case ZSCENE_OBJ_ANIM:
            case ZSCENE_OBJ_LIGHT:
            case ZSCENE_OBJ_CAMERA:
            case ZSCENE_OBJ_EMITTER:
                break;
            default:
                zAssertFail("Uncoded", ZSCENE_FILE, 0x2AE, 0);
                return 0;
            }
            if (e->obj == obj) {
                entry = e;
                break;
            }
        }
        if (!entry) {
            g_pfnDebugPrintf("ZSCENE: Warning object was not in scene:Type:%d, idType:%d, ZID:0x%x\n",
                             type, isTemplate, obj);
            return 0;
        }
    }
    zmemFreeListFree(g_zscene.entryPool, entry);
    return 1;
}

/* VA 0x00490c80, 62 bytes, [I] row, status draft */
ZsceneLoadFn zsceneFindLoader(int kind, const char *ext)
{
    /* no caller in the exe [V]: CreateModel/LoadAnim inline it */
    ZsceneLoader *l = zsceneKinds[kind].loaders;

    if (l) {
        for (; l->ext; l++) {
            if (!zstringCompare(l->ext, ext))
                return l->load;      /* NULL load: "unknown extension" for the callers */
        }
    }
    return 0;
}

/* VA 0x00490cc0, 34 bytes, [I] row, status draft */
int zsceneLoadZmd(ZsceneLoadReq *req)
{
    req->model = zobjModelLoadFile(req->path, (int)req->aux);
    return req->model != 0;
}

/* VA 0x00490cf0, 153 bytes, [I] row, status draft */
ZsceneAnim *zsceneReadAnimIff(zfileHandle *file, ZsceneAnim *list)
{
    /* FORM { ANIM .. ANIM .. }: one ZsceneAnim per ANIM chunk, prepended to list */
    ZiffFile *iff = ziffOpen(file, ID_FORM, 0);
    int more;

    if (!iff)
        return list;
    more = ziffChunkFindFirst(iff, ID_ANIM);
    if (!more) {
        ziffClose(iff);
        return list;
    }
    do {
        ZanimContents *table = zanimLoadIff(iff);
        if (table) {
            ZsceneAnim *a = (ZsceneAnim *)zmemAlloc(sizeof(ZsceneAnim));
            a->table = table;
            a->motionId = zanimGetId(table);
            a->next = list;
            list = a;
        }
        more = ziffChunkFindNext(iff, ID_ANIM);
    } while (more);
    ziffClose(iff);
    return list;
}

/* VA 0x00490d90, 254 bytes, [V] direct, status draft */
int zsceneLoadAnimFile(ZsceneLoadReq *req)
{
    /* .NAD / .IFF: one FORM of ANIM chunks (zsceneReadAnimIff inlined) */
    int size;
    void *buf = zfileLoadAll(req->path, &size);
    zfileHandle *file;

    if (!buf) {
        zAssertFail(zstringFormat("AnimLib not found:%s", req->path), ZSCENE_FILE, 0x426, 0);
        return 0;
    }
    file = zfileOpenMemory(buf, size);
    req->aux = zsceneReadAnimIff(file, 0);
    zfileClose(file, 0);
    zfileFreeLoaded(buf);
    return 1;
}

/* VA 0x00490e90, 347 bytes, [V] direct, status draft */
int zsceneLoadAnimZal(ZsceneLoadReq *req)
{
    /* .ZAL: every asset group is one FORM of ANIM chunks (motion.zal) */
    ZsceneAnim *list = 0;
    ZassetLib *lib = zassetOpenLibrary(req->path);
    int n;

    if (!lib) {
        zAssertFail(zstringFormat("AnimLib not found:%s", req->path), ZSCENE_FILE, 0x448, 0);
        return 0;
    }
    for (n = zassetGetNumGroups(lib); n; n--) {
        uint32_t size = zassetGetAssetSize(lib);
        void *buf = zmemAlloc(size);
        if (zassetLoadAsset(lib, buf)) {
            zfileHandle *file = zfileOpenMemory(buf, (int)size);
            list = zsceneReadAnimIff(file, list);   /* inlined */
            zfileClose(file, 0);
        }
        zmemFree(buf);
        zassetNextGroup(lib);
    }
    zassetCloseLibrary(lib);
    req->aux = list;
    return 1;
}

/* ------------------------------------------------------------------ init / scenes */

/* VA 0x00490ff0, 190 bytes, [V] direct, status draft */
int zsceneInit(char *database)
{
    if (g_zscene.initialized)
        zAssertFail("Already initialized", ZSCENE_FILE, 0x47C, 0);
    zstringMemSet(&g_zscene, 0, sizeof(g_zscene));
    g_zscene.entryPool = zmemFreeListCreate(sizeof(ZsceneEntry), 0x100, 0);
    g_zscene.defPool = zmemFreeListCreate(sizeof(ZsceneDef), 0x100, (ZmemFreeListCallback)zsceneFreeDef);
    if (!database)
        database = "Scene.Txt";
    if (!zsceneLoadDatabase(database)) {
        g_pfnDebugPrintf("**ERROR** ZSCENE: Loading database\n");
        return 0;
    }
    g_zscene.initialized = 1;
    if (!zscnptsInit())
        return 0;
    if (!zpanoramaInit())
        return 0;
    if (!zpropListsInit())
        return 0;
    return zfontInit() != 0;
}

/* VA 0x004910b0, 160 bytes, [I] row, status draft */
int zsceneShutdown(void)
{
    if (!g_zscene.initialized)
        return 1;
    zsceneClose();                   /* inlined */
    zsceneFreeObjects(1);
    if (!zfontShutdown())
        return 0;
    if (!zpropShutdown())
        return 0;
    if (!zpanoramaShutdown())
        return 0;
    if (!zscnptsShutdown())
        return 0;
    zmemFreeListDestroy(g_zscene.entryPool);
    zmemFreeListDestroy(g_zscene.defPool);   /* the defs are not freed one by one */
    g_zscene.initialized = 0;
    return 1;
}

/* VA 0x00491150, 109 bytes, [I] row, status draft */
ZsceneDef *zsceneGetFirstScene(uint32_t unused)
{
    /* the first SCENE def (the list head is one, so this returns g_zscene.scenes); no caller [V] */
    ZsceneDefKey key;
    ZsceneDef *s = g_zscene.scenes;

    key.kind = ZSCENE_DEF_SCENE;
    key.id = 0;
    key.unused = unused;
    key.found = 0;
    if (s->kind == ZSCENE_DEF_SCENE)
        return s;
    if (!s->child || zsceneFindDefRecurse(s->child, &key)) {
        if (s->next)
            zsceneFindDefRecurse(s->next, &key);
    }
    return key.found;
}

/* VA 0x004911c0, 517 bytes, [V] direct, status draft */
int zsceneOpen(char *sceneName)
{
    /*
     * LEVEL.md 1: find the SCENE def by name (zHashName), load its FILE (the .ZSD), open the
     * world (Mat._sd + BSP._sd) through g_zrwWorldApi, then Tag, Scene, PropDef and Prop. A failed
     * component only prints a message: zsceneOpen still returns 1.
     */
    ZsceneDefKey key;
    ZsceneDef *scene;
    const char *path;
    void *zsd;
    int zsdSize;
    char dir[0x80];

    if (g_zscene.current) {
        zAssertFail("There is already a scene open", ZSCENE_FILE, 0x4F1, 0);
        return 0;
    }
    key.kind = ZSCENE_DEF_SCENE;
    key.id = zHashName(sceneName);
    key.unused = 0;
    key.found = 0;
    scene = zsceneSearchDef(g_zscene.scenes, &key);
    if (!scene) {
        zAssertFail(zstringFormat("Scene not found:%s\n", sceneName), ZSCENE_FILE, 0x4FC, 0);
        return 0;
    }
    path = scene->file;
    g_zscene.current = scene;        /* stays set if the loads below fail */
    zsd = zfileLoadAll(path, &zsdSize);
    if (!zsd) {
        zAssertFail(zstringFormat("ZSCENE: Could not load %s", path), ZSCENE_FILE, 0x509, 0);
        return 0;
    }
    zfileGetDirectory(path, dir);
    if (!g_zrwWorldApi.open(zsd, dir)) {
        zAssertFail(zstringFormat("Error loading:%s", path), ZSCENE_FILE, 0x512, 0);
        zfileFreeLoaded(zsd);
        return 0;
    }
    /* zsceneLoadComponents inlined */
    if (!zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_TAG, 1, zsceneTagHandlers))
        g_pfnDebugPrintf("zrwWorld: Failed to load scene tagging info\n");
    else if (!zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_SCENE, 2, zsceneSceneHandlers) ||
             !zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_PROPDEF, 1, zscenePropDefHandlers) ||
             !zsceneLoadComponent(zsd, zsdSize, dir, ZSD_TYPE_PROP, 1, zscenePropHandlers))
        g_pfnDebugPrintf("zrwWorld: Failed to load scene point\n");
    zfileFreeLoaded(zsd);
    return 1;
}

/* VA 0x004913d0, 61 bytes, [I] row, status draft */
int zsceneClose(void)
{
    if (g_zscene.current) {
        ztaginfUnload();
        zscnptsUnload();
        zpanoramaClose();
        zpropListsFree();
        zsceneFreeObjects(0);
        g_zrwWorldApi.close();
        g_zscene.current = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ models and animations */

/* finds the def of a MODEL/ANIM by name hash: in the open scene (or the first one), then in
 * SCENE_COMMON (inlined twice in the exe) */
static ZsceneDef *zsceneFindObjectDef(int kind, uint32_t id)
{
    ZsceneDefKey key;
    ZsceneDef *def;
    ZsceneDef *scene = g_zscene.current ? g_zscene.current : g_zscene.scenes;

    key.kind = kind;
    key.id = id;
    key.unused = 0;
    key.found = 0;
    if (scene->kind == kind && (id == 0 || scene->id == id))
        return scene;   /* (a SCENE def never has kind MODEL/ANIM) */
    if (!scene->child || zsceneFindDefRecurse(scene->child, &key)) {
        if (scene->next)
            zsceneFindDefRecurse(scene->next, &key);   /* note: also searches the other scenes */
    }
    if (key.found)
        return key.found;
    def = &g_zscene.common;
    if (def->kind == key.kind && (key.id == 0 || def->id == key.id))
        return def;
    if (!def->child || zsceneFindDefRecurse(def->child, &key)) {
        if (def->next)
            zsceneFindDefRecurse(def->next, &key);
    }
    return key.found;
}

/* the loader for a def's file extension (zsceneFindLoader inlined); ext receives the
 * upper-cased extension */
static ZsceneLoadFn zsceneLoaderFor(int kind, const char *file, char *ext)
{
    zfileGetExtension(file, ext);
    zstringUpper(ext);
    return zsceneFindLoader(kind, ext);
}

/* VA 0x00491410, 731 bytes, [V] direct, status draft */
void *zsceneCreateModel(int flags, const char *name)
{
    /*
     * A new instance of the model `name`: from a registered template (prop defs, panorama) or an
     * existing instance, else loaded from the MODEL def's FILE (ZMD). Registered as a scene-owned
     * ZSCENE_OBJ_MODEL entry unless ZSCENE_NO_ENTRY.
     */
    uint32_t id = zHashName(name);
    ZsceneEntry *e = zsceneFindEntry(ZSCENE_OBJ_MODEL, id);   /* MatchEntry walk inlined */
    ZsceneDef *def;
    void *model;
    void *obj;

    if (e) {
        model = e->isTemplate ? zobjModelCreateInstance(e->obj)
                              : zobjModelCreateInstance(zobjGetHandle(e->obj));
        def = e->def;
    } else {
        char ext[4];
        ZsceneLoadFn load;
        ZsceneLoadReq req;

        def = zsceneFindObjectDef(ZSCENE_DEF_MODEL, id);
        if (!def) {
            zAssertFail(zstringFormat("Model not in scene:%s", name), ZSCENE_FILE, 0x56C, 0);
            return 0;
        }
        load = zsceneLoaderFor(ZSCENE_DEF_MODEL, def->file, ext);
        if (!load) {
            zAssertFail(zstringFormat("Unknown model extension:%s", ext), ZSCENE_FILE, 0x575, 0);
            return 0;
        }
        req.path = def->file;
        req.aux = 0;
        req.model = 0;
        if (!load(&req)) {
            zAssertFail(zstringFormat("Model not found:%s", def->file), ZSCENE_FILE, 0x57E, 0);
            return 0;
        }
        model = req.model;     /* (the loaded model itself is not registered) */
    }
    if (!model)
        return 0;
    if (flags & ZSCENE_NO_OBJECT) {
        /* [V] the exe returns (and registers) the *name pointer* here, not the model:
         * 0x00491680 MOV ESI,[ESP+0x30] loads the name argument. No caller passes this flag. */
        obj = (void *)name;
    } else {
        obj = zobjModelCreate(model);
        if (!obj) {
            zAssertFail(zstringFormat("Failed to create object for:%s", name), ZSCENE_FILE, 0x58C, 0);
            return 0;
        }
    }
    if (!(flags & ZSCENE_NO_ENTRY))
        zsceneAddObject(def, id, ZSCENE_OBJ_MODEL, 0, obj);   /* inlined; asserts without a scene */
    return obj;
}

/* VA 0x004916f0, 43 bytes, [I] row, status draft */
int zsceneDestroyModel(void *obj)
{
    if (((void **)obj)[0x2C / 4])            /* zobj +0x2C: its model */
        zsceneRemoveObject(ZSCENE_OBJ_MODEL, 0, obj, 0);
    zobjDestroy(obj);
    return 1;
}

/* VA 0x00491720, 610 bytes, [I] row, status draft */
ZsceneEntry *zsceneLoadAnim(int flags, const char *name)
{
    /*
     * The animation library `name` (an ANIM def: .NAD/.IFF/.ZAL), loaded once: an existing entry is
     * returned as is. The entry (isTemplate = 2) is the "animLibID" of zsceneAnimFind/GetTable.
     * flags & 1 makes it survive zsceneClose.
     */
    uint32_t id = zHashName(name);
    ZsceneEntry *e = zsceneFindEntry(ZSCENE_OBJ_ANIM, id);
    ZsceneDef *def;
    ZsceneLoadFn load;
    ZsceneLoadReq req;
    char ext[4];

    if (e)
        return e;
    def = zsceneFindObjectDef(ZSCENE_DEF_ANIM, id);
    if (!def) {
        zAssertFail(zstringFormat("Anim not in scene:%s", name), ZSCENE_FILE, 0x5D1, 0);
        return 0;
    }
    load = zsceneLoaderFor(ZSCENE_DEF_ANIM, def->file, ext);
    if (!load) {
        zAssertFail("Unknown extension", ZSCENE_FILE, 0x5DA, 0);
        return 0;
    }
    req.path = def->file;
    req.aux = 0;
    if (!load(&req)) {
        zAssertFail(zstringFormat("Anim not found:%s", def->file), ZSCENE_FILE, 0x5E3, 0);
        return 0;
    }
    e = zsceneAddObject(def, id, ZSCENE_OBJ_ANIM, 2, req.aux);   /* inlined */
    if (flags & 1)
        e->sceneOwned = 0;          /* NULL dereference in the exe too when no scene is open */
    return e;
}

/* VA 0x00491990, 100 bytes, [V] direct, status draft */
int zsceneFreeAnim(ZsceneEntry *lib)
{
    ZsceneAnim *a, *next;

    if (!lib) {
        zAssertFail("NULL animLibID", ZSCENE_FILE, 0x603, 0);
        return 0;
    }
    for (a = (ZsceneAnim *)lib->obj; a; a = next) {
        next = a->next;
        zanimFreeContents((ZanimContents *)a->table);
        zmemFree(a);
    }
    zsceneRemoveObject(ZSCENE_OBJ_ANIM, 2, lib->obj, 0);
    return 1;
}

/* VA 0x00491a00, 118 bytes, [V] direct, status draft */
void *zsceneAnimFind(ZsceneEntry *lib, uint32_t motionId)
{
    ZsceneAnim *a;

    if (!lib) {
        zAssertFail("NULL animLibID", ZSCENE_FILE, 0x629, 0);
        return 0;
    }
    for (a = (ZsceneAnim *)lib->obj; a; a = a->next) {
        if (a->motionId == motionId)
            return a->table;
    }
    g_pfnDebugPrintf("**WARNING** ZSCENE: Motion (0x%x) not found in animation library", motionId);
    if (lib->def)
        g_pfnDebugPrintf(":%s", lib->def->name);
    g_pfnDebugPrintf("\n");
    return 0;
}

/* VA 0x00491a80, 123 bytes, [V] direct, status draft */
int zsceneAnimGetTable(ZsceneEntry *lib, void **table, int tableSize)
{
    /* Fills table with the library's motions in file order (the list is newest first) and pads
     * the rest with the first one. Returns the motion count. ngappLoadAnimTable (0x0045C690, unreferenced) is the caller. */
    ZsceneAnim *a;
    int n = 0, i;

    for (a = (ZsceneAnim *)lib->obj; a; a = a->next)
        n++;
    if (tableSize < n) {
        zAssertFail(zstringFormat("Table not big enough (TabSize:%d, NumMotions:%d", tableSize, n),
                    ZSCENE_FILE, 0x65A, 0);
        return 0;
    }
    i = n;
    for (a = (ZsceneAnim *)lib->obj; a; a = a->next)
        table[--i] = a->table;
    for (i = n; i < tableSize; i++)
        table[i] = table[0];
    return n;
}

/* ------------------------------------------------------------------ cameras, lights, emitters */

static void *zsceneRegister(void *obj, int flags, int type)
{
    /* the AddObject inlined by the three creators: no def, id 0 */
    if (obj && !(flags & ZSCENE_NO_ENTRY))
        zsceneAddObject(0, 0, type, 0, obj);
    return obj;
}

/* VA 0x00491b00, 109 bytes, [V] direct, status draft */
void *zsceneCreateCamera(int flags)
{
    return zsceneRegister(zobjCameraCreate(), flags, ZSCENE_OBJ_CAMERA);
}

/* VA 0x00491b70, 33 bytes, [I] row, status draft */
int zsceneDestroyCamera(void *obj)
{
    zsceneRemoveObject(ZSCENE_OBJ_CAMERA, 0, obj, 0);
    zobjDestroy(obj);
    return 1;
}

/* VA 0x00491ba0, 123 bytes, [V] direct, status draft */
void *zsceneCreateLight(int flags, int lightType, const RwRGBA *colour)
{
    /* ngappCreateLevelLights: one ambient light (type 1) and up to three directional lights (type 0)
     * whose colour is the 4-byte OBJI of the SpotLight scene points (r, g, b, a bytes / 255) */
    return zsceneRegister(zobjLightCreate(lightType, colour), flags, ZSCENE_OBJ_LIGHT);
}

/* VA 0x00491c20, 33 bytes, [I] row, status draft */
int zsceneDestroyLight(void *obj)
{
    zsceneRemoveObject(ZSCENE_OBJ_LIGHT, 0, obj, 0);
    zobjDestroy(obj);
    return 1;
}

/* VA 0x00491c50, 109 bytes, [V] direct, status draft */
void *zsceneCreateEmitter(int flags)
{
    return zsceneRegister(zobjPEmitCreate(), flags, ZSCENE_OBJ_EMITTER);
}

/* VA 0x00491cc0, 33 bytes, [I] row, status draft */
int zsceneDestroyEmitter(void *obj)
{
    zsceneRemoveObject(ZSCENE_OBJ_EMITTER, 0, obj, 0);
    zobjDestroy(obj);
    return 1;
}

/* VA 0x00491cf0, 144 bytes, [I] row, status draft */
void zsceneParseMotionName(const char *name, uint32_t *id, uint32_t *number)
{
    /* "<prefix>_<n>": *id = zHashName(prefix, at most 14 chars), *number = n (decimal).
     * Nothing is written when there is no '_'. With a longer prefix the number is read from
     * name + 15 instead of after the '_'. No caller in the exe [V]. */
    char prefix[16];
    uint32_t i = 0, n;

    if (zstringLength(name) == 0)
        return;
    while (name[i] != '_') {
        i = (i + 1) & 0xFFFF;
        if (i >= (uint32_t)zstringLength(name))
            return;
    }
    n = i > 14 ? 14 : i;
    zstringMemCopy(prefix, name, n);
    prefix[n] = 0;
    *id = zHashName(prefix);
    *number = zstringToUInt(name + n + 1, 10);
}

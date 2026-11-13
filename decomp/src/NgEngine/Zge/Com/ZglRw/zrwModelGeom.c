/*
 * NgEngine/Zge/Com/ZglRw/zrwModelGeom.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwModelGeom.c
 * Evidence: [V] path string 0x00562F74 (asserts of Init 0x4E, NotCoded 0x86, Lock 0xBA, Unlock 0xDC);
 *           model driver slots 32-38 (0x00560EF8-0x00560F10).
 * 9 functions, VA 0x0049DF90-0x0049E1EA (DM-059 created 8).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 2.3.
 *
 * Vertex access to a model's LOD-0 geometry (morph target 0) for deforming effects. Vertex numbers
 * are 1-based; positions convert between RW and ZGE units.
 */
#include "decomp.h"
#include "zglrw.h"

#define ZRWMG_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwModelGeom.c"

extern float g_zrwScale;       /* 0x00697720 */
extern float g_zrwInvScale;    /* 0x00697724 */

static int32_t s_zrwModelGeomInitialized;   /* 0x0067523C */

/* VA 0x0049df90, 42 bytes, [V] direct, status draft (no caller found) */
void zrwModelGeomInit(void)
{
    if (s_zrwModelGeomInitialized)
        zAssertFail("Already initialized", ZRWMG_SRC, 0x4E, 0);
    s_zrwModelGeomInitialized = 1;
}

/* VA 0x0049dfc0, 25 bytes, [I] DM-059 created, status draft (no caller found) */
int zrwModelGeomShutdown(void)
{
    if (s_zrwModelGeomInitialized)
        s_zrwModelGeomInitialized = 0;
    return 1;
}

/* VA 0x0049dfe0, 15 bytes, [I] DM-059 created, status draft (model slot 32) */
void zrwModelSetUpdateCallback(ZglModel *model, void (__cdecl *fn)(ZglModel *))
{
    model->updateCB = fn;
}

/* VA 0x0049dff0, 28 bytes, [V] direct, status draft (model slot 33: not implemented) */
int zrwModelGeomNotCoded(void)
{
    zAssertFail("Not coded", ZRWMG_SRC, 0x86, 0);
    return 0;
}

/* VA 0x0049e010, 6 bytes, [I] DM-059 created, status draft (model slot 34: index -> vertex number) */
int zrwModelGeomVertexNumber(int index)
{
    return index + 1;
}

/* VA 0x0049e020, 138 bytes, [V] direct, status draft (model slot 35; flags bit 0 = positions) */
int zrwModelGeomLock(ZglModel *model, uint32_t flags)
{
    if (flags & 1) {
        void *geom = model->def->geometry0;
        void *mt = RpGeometryGetMorphTarget(geom, 0);
        if (RpGeometryLock(geom, 2 /* rpGEOMETRYLOCKVERTICES */) == NULL) {
            zAssertFail(zstringFormat("Could not lock model geometry for '%s'", model->def->name), ZRWMG_SRC, 0xBA, 0);
            return 0;
        }
        model->def->lockedVerts = RpMorphTargetGetVertices(mt);
        model->flags |= 4;
    }
    return 1;
}

/* VA 0x0049e0b0, 92 bytes, [V] direct, status draft (model slot 36) */
int zrwModelGeomUnlock(ZglModel *model)
{
    if (RpGeometryUnlock(model->def->geometry0) == NULL) {
        zAssertFail(zstringFormat("Could not unlock model geometry for '%s'", model->def->name), ZRWMG_SRC, 0xDC, 0);
        return 0;
    }
    model->flags &= ~4u;
    return 1;
}

/* VA 0x0049e110, 104 bytes, [I] DM-059 created, status draft (model slot 37: vertex n in ZGE units) */
int zrwModelGeomGetVertex(ZglModel *model, int n, float *out)
{
    const RwV3d *v = &model->def->lockedVerts[n - 1];
    out[0] = v->x * g_zrwInvScale;
    out[1] = -(v->y * g_zrwInvScale);
    out[2] = -(v->z * g_zrwInvScale);
    return 1;
}

/* VA 0x0049e180, 104 bytes, [I] DM-059 created, status draft (model slot 38) */
int zrwModelGeomSetVertex(ZglModel *model, int n, const float *in)
{
    RwV3d *v = &model->def->lockedVerts[n - 1];
    v->x = g_zrwScale * in[0];
    v->y = -(in[1] * g_zrwScale);
    v->z = -(in[2] * g_zrwScale);
    return 1;
}

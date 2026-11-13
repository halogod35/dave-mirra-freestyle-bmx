/*
 * NgEngine/Zge/Com/ZglRw/zrwModelRender.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwModelRender.c
 * Evidence: [V] path string 0x00561824 (asserts of RenderShadowPass 0x312, Init 0x32F, Render 0x384,
 *           SetRenderMode 0x3C7); model driver slots 22-23; [I] the rest by link order, callers and
 *           the globals 0x00673E80-0x00673F6B. DM-059 moved here from ztaginf.c's hole the RW render
 *           callbacks 0x0048F3C0-0x0048FB50 (no ztaginf data; called from this file and zrwModel)
 *           and from gap_zrwModelRender__zobjLight the atomic render-callback helpers 0x0048FEC0-
 *           0x0048FF30.
 * 19 functions, VA 0x0048F3C0-0x0048FF4F (DM-059 created 10).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 2.4.
 *
 * Model rendering = RpClumpRender of the clump (the world renders the clumps it holds). Extra atomic
 * render callbacks (RpAtomicSetRenderCallBack is inlined: atomic +0x4C):
 *   mode 1  zrwModelRenderNop        (draw nothing)
 *   mode 2  zrwModelRenderFlatCB     (Im3D triangle list of the positions only, no texture)
 *   NgApp   zrwModelRenderFacingCB   (yaw towards the camera, model colour)
 *   NgIF    zrwModelRenderColouredCB (material colour = model colour while drawing)
 *   MDEF 0x10 models: zrwModelRenderShadowPass draws the "projected" version every frame (ZGL
 *   callback list 1), zrwModelRenderProjectedCB (far clip 300, textured, y lifted 2 units).
 */
#include "decomp.h"
#include "zmath.h"
#include "zglrw.h"

#define ZRWMR_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwModelRender.c"

/* the Im3D vertex RW 3.1 uses (RwIm3DVertex, 0x24) */
typedef struct ZrwIm3DVertex {
    RwV3d    pos;          /* +0x00 */
    RwV3d    normal;       /* +0x0C */
    RwRGBA   colour;       /* +0x18 */
    float    u, v;         /* +0x1C */
} ZrwIm3DVertex;           /* 0x24 */

extern void    *g_zrwCamera;          /* 0x006976F4 RwCamera * */
extern uint32_t g_zrwRenderState;     /* 0x00697778 zrwZglSetRenderState bits */
extern void    *g_zrwRenderRaster;    /* 0x0069777C current texture raster */

/* ------------------------------------------------------------------ globals */
static struct {
    int32_t   initialized;   /* 0x00673E80 */
    int32_t   mode;          /* 0x00673E84 zrwModelSetRenderMode */
    void   *(__cdecl *modeCB)(void *atomic); /* 0x00673E88 */
    void     *shadowCB;      /* 0x00673E8C ZGL callback node of RenderShadowPass */
    ZrwIm3DVertex *verts;    /* 0x00673E90 */
    void     *buf94;         /* 0x00673E94 */
    void     *buf98;         /* 0x00673E98 */
    uint16_t *indices;       /* 0x00673E9C */
    int32_t   maxVerts;      /* 0x00673EA0 */
    int32_t   maxTris;       /* 0x00673EA4 */
} s_zrwMr;                   /* 0x28 */
static ZrwIm3DVertex s_flatVerts[18];       /* 0x006737C0 */
static uint16_t      s_flatIndices[0x60];   /* 0x00673EA8 0,1,1,2,2,0,3,4,... line list */
static int32_t       s_flatIndicesDirty = 1; /* 0x00561820 */
static const RwRGBA  s_white[2] = { { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF } }; /* 0x00561818 */

/* VA 0x0048f3c0, 5 bytes, [I] DM-059 created, status draft (render mode 1) */
void *zrwModelRenderNop(void *atomic)
{
    return atomic;
}

/* VA 0x0048f3d0, 16 bytes, [I] DM-059 created, status draft (render mode 2) */
void *zrwModelRenderFlatCB(void *atomic)
{
    return zrwModelRenderImmediateCB(atomic, NULL);
}

/* VA 0x0048f3e0, 333 bytes, [I] DM-059 created, status draft (NgApp 0x0045AC40 installs it)
 * Billboard-style: the atomic's frame is rotated about y to face the camera's yaw, drawn with the
 * model colour and render state 0x2222 (no z-write [I]), then the colour is reset to white. */
void *zrwModelRenderFacingCB(void *atomic)
{
    ZglModel *model = zrwModelFromAtomic(atomic);
    RwMatrix *cam = RwFrameGetLTM(RwCameraGetFrame(g_zrwCamera));
    RwMatrix *m = RwFrameGetLTM(*(void **)((uint8_t *)atomic + 4));
    RwV3d axis, pos;
    uint32_t state;
    int16_t yaw;

    zrwModelApplyColour(model, &model->colour);
    zrwZglPushRenderState();
    state = (g_zrwRenderState & 0xFFFF22FF) | 0x2222;
    if (g_zrwRenderState != state || g_zrwRenderRaster != NULL)
        zrwZglSetRenderState(state, NULL);
    axis.x = 0.0f; axis.y = -1.0f; axis.z = 0.0f;
    pos = m->pos;
    yaw = zmathAtan2(cam->at.x, cam->at.z);
    m->right.x = 1.0f; m->right.y = 0.0f; m->right.z = 0.0f;
    m->up.x = 0.0f;    m->up.y = 1.0f;    m->up.z = 0.0f;
    m->at.x = 0.0f;    m->at.y = 0.0f;    m->at.z = 1.0f;
    m->pos.x = m->pos.y = m->pos.z = 0.0f;
    m->flags |= 0x20003;
    RwMatrixRotate(m, &axis, (float)(-yaw) * 0.005493164f /* 360/65536 */, 0);
    m->pos = pos;
    model->def->renderCB(atomic);
    zrwZglPopRenderState();
    zrwModelApplyColour(model, &s_white[0]);
    return atomic;
}

/* VA 0x0048f530, 59 bytes, [I] callers, status draft (zrwModelSetColour's render callback) */
void *zrwModelRenderColouredCB(void *atomic)
{
    ZglModel *model = zrwModelFromAtomic(atomic);
    zrwModelApplyColour(model, &model->colour);
    model->def->renderCB(atomic);
    zrwModelApplyColour(model, &s_white[1]);
    return atomic;
}

/* VA 0x0048f570, 420 bytes, [I] DM-059 created, status draft
 * Untextured immediate-mode version: every triangle copied to an Im3D batch (positions only, up
 * to 6 triangles) and drawn as an indexed list with render state 0x1220. */
void *zrwModelRenderImmediateCB(void *atomic, void *unused)
{
    void *geom = *(void **)((uint8_t *)atomic + 0x1C);
    const RwV3d *pos = RpMorphTargetGetVertices(RpGeometryGetMorphTarget(geom, 0));
    const uint16_t *tri = (const uint16_t *)RpGeometryGetTriangles(geom);
    RwMatrix *ltm = RwFrameGetLTM(*(void **)((uint8_t *)atomic + 4));
    int ntri = RpGeometryGetNumTriangles(geom);
    uint32_t state;
    int n = 0, k;
    (void)unused;

    if (s_flatIndicesDirty) {
        int16_t i, *p = (int16_t *)s_flatIndices;
        s_flatIndicesDirty = 0;
        for (i = 0; p < (int16_t *)s_flatIndices + 0x60; p += 6, i += 3) {
            p[0] = i;     p[1] = i + 1; p[2] = i + 1;
            p[3] = i + 2; p[4] = i + 2; p[5] = i;
        }
        zstringMemSet(s_flatIndices, 0, 2);
    }
    zrwZglPushRenderState();
    state = (g_zrwRenderState & 0xFFFF12F5) | 0x1220;
    if (g_zrwRenderState != state || g_zrwRenderRaster != NULL)
        zrwZglSetRenderState(state, NULL);
    for (; ntri > 0; ntri--, tri += 4) {
        for (k = 0; k < 3; k++)
            s_flatVerts[n + k].pos = pos[tri[k]];
        n += 3;
        if (n + 3 > 0x10) {
            if (RwIm3DTransform(s_flatVerts, n, ltm, 0)) {
                RwIm3DRenderIndexedPrimitive(1 /* line list */, s_flatIndices, n * 2);
                RwIm3DEnd();
            }
            n = 0;
        }
    }
    if (n > 0 && RwIm3DTransform(s_flatVerts, n, ltm, 0)) {
        RwIm3DRenderIndexedPrimitive(1, s_flatIndices, n * 2);
        RwIm3DEnd();
    }
    zrwZglPopRenderState();
    return atomic;
}

/* VA 0x0048f720, 869 bytes, [I] DM-059 created, status draft (an 0x2414-byte stack frame)
 * The flattened copy of an MDEF-0x10 model: textured Im3D triangles of the geometry with the
 * frame's LTM moved to the camera position and 2 units down, far clip 300, z test/write off
 * (render states 4/6 [I]), one indexed batch per material run. */
void *zrwModelRenderProjectedCB(void *atomic)
{
    void *geom = *(void **)((uint8_t *)atomic + 0x1C);
    RwMatrix *ltm = RwFrameGetLTM(*(void **)((uint8_t *)atomic + 4));
    int nv = RpGeometryGetNumVertices(geom);
    const RwV3d *pos = RpMorphTargetGetVertices(RpGeometryGetMorphTarget(geom, 0));
    const uint16_t *tri = (const uint16_t *)RpGeometryGetTriangles(geom);
    const float *uv = (const float *)RpGeometryGetVertexTexCoords(geom);
    int ntri = RpGeometryGetNumTriangles(geom);
    ZrwIm3DVertex verts[0x100];
    float oldFar;
    int i, n, curMat;
    uint16_t *idx;

    (void)RwRasterGetWidth(RwCameraGetRaster(g_zrwCamera));
    (void)RwRasterGetHeight(RwCameraGetRaster(g_zrwCamera));
    oldFar = RwCameraGetFarClipPlane(g_zrwCamera);
    RwCameraSetFarClipPlane(g_zrwCamera, 300.0f);
    if (geom == NULL)
        return atomic;
    if (s_zrwMr.maxVerts < nv) {
        s_zrwMr.verts = (ZrwIm3DVertex *)zmemResize(s_zrwMr.verts, nv * 0x20 + 0x10);
        s_zrwMr.buf94 = zmemResize(s_zrwMr.buf94, nv * 0xC);
        s_zrwMr.buf98 = zmemResize(s_zrwMr.buf98, nv);
        s_zrwMr.maxVerts = nv;
    }
    if (s_zrwMr.maxTris < ntri) {
        s_zrwMr.indices = (uint16_t *)zmemResize(s_zrwMr.indices, ntri * 6);
        s_zrwMr.maxTris = ntri;
    }
    RwMatrixTranslate(ltm, &RwFrameGetLTM(RwCameraGetFrame(g_zrwCamera))->pos, 0);
    ltm->pos.y -= 2.0f;
    for (i = 0; i < nv; i++) {
        verts[i].pos = pos[i];
        verts[i].colour.red = verts[i].colour.green = verts[i].colour.blue = verts[i].colour.alpha = 0xFF;
        verts[i].u = uv[2 * i];
        verts[i].v = uv[2 * i + 1];
    }
    if (g_zrwRenderState != ((g_zrwRenderState & 0xFFFF12F5) | 0x1220))
        zrwZglSetRenderState((g_zrwRenderState & 0xFFFF12F5) | 0x1220, g_zrwRenderRaster);
    RwRenderStateSet(4, 0);
    RwRenderStateSet(6, 0);
    if (RwIm3DTransform(verts, nv, ltm, 1)) {
        (void)RwCameraGetNearClipPlane(g_zrwCamera);   /* result unused [V] */
        idx = s_zrwMr.indices;
        n = 0;
        curMat = -1;
        for (; ntri > 0; ntri--, tri += 4) {
            if ((int16_t)tri[3] != curMat) {
                if (n != 0) {
                    void *raster = RwTextureGetRaster(RpMaterialGetTexture(RpGeometryGetMaterial(geom, curMat)));
                    if (raster != g_zrwRenderRaster)
                        zrwZglSetRenderState(g_zrwRenderState, raster);
                    RwIm3DRenderIndexedPrimitive(3 /* triangle list */, s_zrwMr.indices, n * 3);
                    n = 0;
                    idx = s_zrwMr.indices;
                }
                curMat = (int16_t)tri[3];
            }
            idx[0] = tri[0]; idx[1] = tri[1]; idx[2] = tri[2];
            idx += 3;
            n++;
        }
        if (n != 0) {
            void *raster = RwTextureGetRaster(RpMaterialGetTexture(RpGeometryGetMaterial(geom, curMat)));
            if (raster != g_zrwRenderRaster)
                zrwZglSetRenderState(g_zrwRenderState, raster);
            RwIm3DRenderIndexedPrimitive(3, s_zrwMr.indices, n * 3);
        }
    }
    RwIm3DEnd();
    RwCameraSetFarClipPlane(g_zrwCamera, oldFar);
    RwRenderStateSet(4, 1);
    RwRenderStateSet(6, 1);
    return atomic;
}

/* VA 0x0048fa90, 186 bytes, [I] DM-059 created, status draft
 * RwFrameForAllChildren callback: a white-to-red line from the parent's position to this frame. */
void *zrwModelDrawBoneCB(void *frame, RwV3d *parentPos)
{
    ZrwIm3DVertex v[2];
    RwV3d here = RwFrameGetLTM(frame)->pos;

    zstringMemSet(v, 0, sizeof(v));
    v[0].pos = *parentPos;
    v[0].colour.red = 0xFF; v[0].colour.alpha = 0xFF;
    v[1].pos = here;
    v[1].colour.red = 0xFF; v[1].colour.alpha = 0xFF;
    if (RwIm3DTransform(v, 2, NULL, 0)) {
        RwIm3DRenderLine(0, 1);
        RwIm3DEnd();
    }
    RwFrameForAllChildren(frame, (void *)zrwModelDrawBoneCB, &here);
    return frame;
}

/* VA 0x0048fb50, 29 bytes, [I] DM-059 created, status draft (no caller found) */
int zrwModelRenderFlat(ZglModel *model)
{
    RpClumpForAllAtomics(model->clump, (void *)zrwModelRenderImmediateCB, NULL);
    return 1;
}

/* VA 0x0048fb70, 75 bytes, [I] DM-059 created, status draft (model slot 23: debug skeleton) */
void zrwModelRenderSkeleton(ZglModel *model)
{
    void *root;
    RwV3d p;
    if (model->clump == NULL || (root = RpClumpGetFrame(model->clump)) == NULL)
        return;
    p = RwFrameGetLTM(root)->pos;
    RwFrameForAllChildren(root, (void *)zrwModelDrawBoneCB, &p);
}

/* VA 0x0048fbc0, 130 bytes, [V] direct, status draft (ZGL callback type 1 of MDEF-0x10 models)
 * Moves the model to the camera position and draws its atomics with zrwModelRenderProjectedCB. */
int zrwModelRenderShadowPass(ZglModel *model)
{
    void *clump = model->clump;
    if ((model->def->mdefFlags & 1) == 0) {
        zAssertFail("Not coded", ZRWMR_SRC, 0x312, 0);
        return 0;
    }
    RwMatrixTranslate(RwFrameGetMatrix(RpClumpGetFrame(clump)),
                      &RwFrameGetLTM(RwCameraGetFrame(g_zrwCamera))->pos, 0);
    RwFrameUpdateObjects(RpClumpGetFrame(clump));
    RpClumpForAllAtomics(model->clump, (void *)zrwModelRenderProjectedCB, NULL);
    return 1;
}

/* VA 0x0048fc50, 167 bytes, [V] direct, status draft (called by zrwModelInit) */
int zrwModelRenderInit(void)
{
    if (s_zrwMr.initialized)
        zAssertFail("Already initialized", ZRWMR_SRC, 0x32F, 0);
    zstringMemSet(&s_zrwMr, 0, sizeof(s_zrwMr));
    s_zrwMr.initialized = 1;
    s_zrwMr.maxVerts = 0x200;
    s_zrwMr.verts = (ZrwIm3DVertex *)zmemAlloc(0x4010);
    s_zrwMr.buf94 = zmemAlloc(0x1800);
    s_zrwMr.buf98 = zmemAlloc(0x200);
    s_zrwMr.maxTris = 0x100;
    s_zrwMr.indices = (uint16_t *)zmemAlloc(0x600);
    s_zrwMr.mode = 0;
    s_zrwMr.modeCB = NULL;
    return 1;
}

/* VA 0x0048fd00, 146 bytes, [I] callers, status draft */
int zrwModelRenderShutdown(void)
{
    if (s_zrwMr.initialized) {
        if (s_zrwMr.shadowCB != NULL) {
            zrwZglRemoveCallback(s_zrwMr.shadowCB);
            s_zrwMr.shadowCB = NULL;
        }
        if (s_zrwMr.verts) { zmemFree(s_zrwMr.verts); s_zrwMr.verts = NULL; }
        if (s_zrwMr.buf94) { zmemFree(s_zrwMr.buf94); s_zrwMr.buf94 = NULL; }
        if (s_zrwMr.buf98) { zmemFree(s_zrwMr.buf98); s_zrwMr.buf98 = NULL; }
        if (s_zrwMr.indices) { zmemFree(s_zrwMr.indices); s_zrwMr.indices = NULL; }
        s_zrwMr.initialized = 0;
    }
    return 1;
}

/* VA 0x0048fda0, 138 bytes, [V] direct, status draft (model slot 22)
 * Models with MDEF flag 1 only (others assert "Not coded"): root moved to the camera, skinned
 * bones updated, RpClumpRender. */
void zrwModelRender(ZglModel *model)
{
    void *clump = model->clump;
    if ((model->def->mdefFlags & 1) == 0) {
        zAssertFail("Not coded", ZRWMR_SRC, 900, 0);
        return;
    }
    RwMatrixTranslate(RwFrameGetMatrix(RpClumpGetFrame(clump)),
                      &RwFrameGetLTM(RwCameraGetFrame(g_zrwCamera))->pos, 0);
    RwFrameUpdateObjects(RpClumpGetFrame(clump));
    if (model->def->mdefFlags & 2)
        RpBoneClumpUpdate(clump);
    RpClumpRender(clump);
}

/* VA 0x0048fe30, 98 bytes, [I] DM-059 created, status draft (no caller found)
 * Global render mode for models loaded afterwards: 0 normal, 1 invisible, 2 flat. Returns the old. */
int32_t zrwModelSetRenderMode(int32_t mode)
{
    int32_t old = s_zrwMr.mode;
    s_zrwMr.mode = mode;
    if (mode == 0)
        s_zrwMr.modeCB = NULL;
    else if (mode == 1)
        s_zrwMr.modeCB = zrwModelRenderNop;
    else if (mode == 2)
        s_zrwMr.modeCB = zrwModelRenderFlatCB;
    else
        zAssertFail("Unkown", ZRWMR_SRC, 0x3C7, 0);
    return old;
}

/* VA 0x0048fea0, 20 bytes, [I] DM-059 created, status draft (RpAtomicSetRenderCallBack, inlined) */
void *zrwModelSetAtomicRenderCB(void *atomic, void *fn)
{
    *(void **)((uint8_t *)atomic + 0x4C) = fn;
    ((uint8_t *)atomic)[0x14]++;
    return atomic;
}

/* VA 0x0048fec0, 31 bytes, [I] row, status draft
 * DM-057's name kept: it sets the render callback of every atomic of the model (it does not walk
 * the materials). */
void zrwModelForEachMaterial(void *model, void *fn)
{
    if (fn != NULL)
        RpClumpForAllAtomics(((ZglModel *)model)->clump, (void *)zrwModelSetAtomicRenderCB, fn);
}

/* VA 0x0048fee0, 32 bytes, [I] gap, status draft (the global render mode, at load) */
void zrwModelApplyRenderMode(ZglModel *model)
{
    if (s_zrwMr.modeCB != NULL)
        RpClumpForAllAtomics(model->clump, (void *)zrwModelSetAtomicRenderCB, (void *)s_zrwMr.modeCB);
}

/* VA 0x0048ff00, 38 bytes, [I] gap, status draft (MDEF 0x10 models; one at a time) */
void zrwModelAddShadowCallback(ZglModel *model)
{
    if (s_zrwMr.shadowCB == NULL)
        s_zrwMr.shadowCB = zrwZglAddCallback(1, (void *)zrwModelRenderShadowPass, model, 0);
}

/* VA 0x0048ff30, 30 bytes, [I] gap, status draft */
void zrwModelRemoveShadowCallback(void)
{
    if (s_zrwMr.shadowCB != NULL) {
        zrwZglRemoveCallback(s_zrwMr.shadowCB);
        s_zrwMr.shadowCB = NULL;
    }
}

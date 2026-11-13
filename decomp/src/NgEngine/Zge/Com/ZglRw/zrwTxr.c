/*
 * NgEngine/Zge/Com/ZglRw/zrwTxr.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwTxr.c
 * Evidence: [V] path string 0x005625E4 (asserts of CreateTextureFromMemory, LoadPalettedImage,
 *           ChunkTBMP, Init, Release, SetFilterMode, the load buffer); the texture driver table
 *           0x005623F0 (15 slots, all in this file); [I] the rest by link order and globals
 *           0x00674290-0x006746BF.
 * 46 functions, VA 0x00497780-0x00498D71 (DM-059 created 17: the table slots reached only through
 * 0x005623F0, the paletted / procedural texture constructors and the RW texture plugin).
 * DM-059 (draft). Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 3,
 * docs/formats/TEXTURE.md 3.
 *
 * ZGL textures wrap RenderWare textures. Images arrive as complete .bmp / .png files (TBMP / TPNG
 * chunks of a MATI, or zrwMatCreateFromImage); they are read through RenderWare's image readers
 * from an in-memory file, colour-keyed (RGB 255,0,255 -> transparent) and made into an RwTexture
 * in the current texture dictionary.
 */
#include "decomp.h"
#include "zglrw.h"

#define ZRWTXR_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwTxr.c"

/* RenderWare raster formats (rwRASTERFORMAT*) */
#define RWFMT_DEFAULT  0x0000
#define RWFMT_1555     0x0100
#define RWFMT_565      0x0200
#define RWFMT_4444     0x0300
#define RWFMT_LUM8     0x0400
#define RWFMT_8888     0x0500
#define RWFMT_888      0x0600
#define RWFMT_16       0x0700
#define RWFMT_24       0x0800
#define RWFMT_32       0x0900
#define RWFMT_555      0x0A00
#define RWFMT_PIXMASK  0x0F00
#define RWFMT_PAL8     0x2000
#define RWFMT_PAL4     0x4000
#define RWFMT_MIPMAP   0x8000
#define RWFMT_MASK     0xFF00
#define RWRASTER_TEXTURE 4

#define RWFILTER_NEAREST         1
#define RWFILTER_LINEAR          2
#define RWFILTER_LINEARMIPLINEAR 6
#define RWADDRESS_WRAP   1
#define RWADDRESS_CLAMP  3

#define ZRWTXR_PLUGIN_ID 0xFF01    /* RwTextureRegisterPlugin id of the owner pointer */

/* ------------------------------------------------------------------ other modules */
/* the in-memory file the RW image readers read from (zrwRamDisk.c, DM-061) */
void  zrwRamDiskBegin(void);                                   /* 0x004A3500 install the memory file system */
void  zrwRamDiskAddFile(const void *buf, int len, const char *ext); /* 0x004A3580 add a memory file */
void  zrwRamDiskEnd(void);                                   /* 0x004A35C0 restore the RW file system */
extern uint32_t g_zrwZglFlags;                              /* 0x00697728 bit 0: clamp texture addressing */

/* ------------------------------------------------------------------ globals */
ZrwTxrGlobals g_zrwTxr;                                     /* 0x00674290 */
int32_t       g_zrwTxrPluginOffset = -1;                    /* 0x005623E8 */

/* ------------------------------------------------------------------ functions */

/* VA 0x00497780, 349 bytes, [I] callers, status draft
 * Debug dump of a raster: format name, width, height, stride, depth. */
void zrwTxrPrintRasterInfo(void *raster)
{
    uint32_t fmt;
    const char *name;

    if (raster == NULL)
        return;
    fmt = RwRasterGetFormat(raster);
    switch (fmt) {
    case RWFMT_DEFAULT: name = "rwRASTERFORMATDEFAULT"; break;
    case RWFMT_1555:    name = "rwRASTERFORMAT1555"; break;
    case RWFMT_565:     name = "rwRASTERFORMAT565"; break;
    case RWFMT_4444:    name = "rwRASTERFORMAT4444"; break;
    case RWFMT_LUM8:    name = "rwRASTERFORMATLUM8"; break;
    case RWFMT_8888:
    case RWFMT_888:     name = "rwRASTERFORMAT888"; break;  /* sic: 8888 prints 888 */
    case RWFMT_16:
    case RWFMT_24:      name = "rwRASTERFORMAT24"; break;   /* sic: 16 prints 24 */
    case RWFMT_32:      name = "rwRASTERFORMAT32"; break;
    case RWFMT_555:     name = "rwRASTERFORMAT555"; break;
    case RWFMT_PIXMASK: name = "rwRASTERFORMATPIXELFORMATMASK"; break;
    case RWFMT_PAL8:    name = "rwRASTERFORMATPAL8"; break;
    case RWFMT_PAL4:    name = "rwRASTERFORMATPAL4"; break;
    case RWFMT_MIPMAP:  name = "rwRASTERFORMATMIPMAP"; break;
    case RWFMT_MASK:    name = "rwRASTERFORMATMASK"; break;
    default:            name = "Unknown"; break;
    }
    g_pfnDebugPrintf("Format: %s\nWidth: %d\nHeight: %d\nStride: %d\nDepth: %d\n", name,
                     RwRasterGetWidth(raster), RwRasterGetHeight(raster),
                     RwRasterGetStride(raster), RwRasterGetDepth(raster));
}

/* VA 0x004978e0, 13 bytes, [I] callers, status draft */
void zrwTxr_SetColorKey(const RwRGBA *key)
{
    g_zrwTxr.colorKey = *key;
}

/* VA 0x004978f0, 233 bytes, [I] between, status draft
 * Colour key: in a 4/8-bit image every palette entry, in a 32-bit image every pixel equal to the
 * key RGB (alpha ignored) becomes 0,0,0,0. */
void zrwTxr_ApplyColorKey(RwImage *img)
{
    int depth = RwImageGetDepth(img);
    const RwRGBA key = g_zrwTxr.colorKey;

    if (depth == 4 || depth == 8) {
        RwRGBA *pal = RwImageGetPalette(img);
        int n;
        for (n = 1 << depth; n != 0; n--, pal++) {
            if (pal->red == key.red && pal->green == key.green && pal->blue == key.blue)
                pal->red = pal->green = pal->blue = pal->alpha = 0;
        }
    } else {
        uint8_t *row = RwImageGetPixels(img);
        int width = RwImageGetWidth(img);
        int y, x;
        for (y = RwImageGetHeight(img); y != 0; y--, row += img->stride) {
            RwRGBA *px = (RwRGBA *)row;
            for (x = width; x != 0; x--, px++) {
                if (px->red == key.red && px->green == key.green && px->blue == key.blue)
                    px->red = px->green = px->blue = px->alpha = 0;
            }
        }
    }
}

/* VA 0x004979e0, 66 bytes, [I] between, status draft
 * RwImageRegisterImageFormat("bmp") reader: RtBMPImageRead + colour key. */
RwImage *zrwTxr_ReadBmp(const char *name)
{
    RwImage *img = RtBMPImageRead(name);
    if (img != NULL) {
        zrwTxr_ApplyColorKey(img);
        g_zrwTxr.lastWidth = RwImageGetWidth(img);
        g_zrwTxr.lastHeight = RwImageGetHeight(img);
        g_zrwTxr.lastDepth = RwImageGetDepth(img);
    }
    return img;
}

/* VA 0x00497a30, 66 bytes, [I] between, status draft (libpng 1.0.5 behind RtPNGImageRead) */
RwImage *zrwTxr_ReadPng(const char *name)
{
    RwImage *img = RtPNGImageRead(name);
    if (img != NULL) {
        zrwTxr_ApplyColorKey(img);
        g_zrwTxr.lastWidth = RwImageGetWidth(img);
        g_zrwTxr.lastHeight = RwImageGetHeight(img);
        g_zrwTxr.lastDepth = RwImageGetDepth(img);
    }
    return img;
}

/* VA 0x00497a80, 3 bytes, [I] between, status draft (the "dds" reader is a stub) */
RwImage *zrwTxr_ReadDds(const char *name)
{
    (void)name;
    return NULL;
}

/* VA 0x00497a90, 22 bytes, [I] DM-059 created, status draft
 * RwTexture plugin constructor: owner = NULL. */
void *zrwTxrPluginConstructor(void *rwTexture, int offset)
{
    (void)offset;
    if (g_zrwTxrPluginOffset > 0)
        *(void **)((uint8_t *)rwTexture + g_zrwTxrPluginOffset) = NULL;
    return rwTexture;  /* [I] the code returns nothing useful; RW only tests non-NULL */
}

/* VA 0x00497ab0, 5 bytes, [I] DM-059 created, status draft (plugin destructor: nothing to free) */
void *zrwTxrPluginDestructor(void *rwTexture)
{
    return rwTexture;
}

/* VA 0x00497ac0, 29 bytes, [I] DM-059 created, status draft (no caller found) */
int zrwTxrSetOwnerOfRwTexture(void *rwTexture, void *owner)
{
    if (g_zrwTxrPluginOffset < 1)
        return 0;
    *(void **)((uint8_t *)rwTexture + g_zrwTxrPluginOffset) = owner;
    return 1;
}

/* VA 0x00497ae0, 20 bytes, [I] callers, status draft
 * The ZGL material that owns an RwTexture (zrwWorld.c maps world RpMaterials to ZGL materials). */
void *zrwTxrGetOwnerOfRwTexture(void *rwTexture)
{
    if (g_zrwTxrPluginOffset < 1)
        return NULL;
    return *(void **)((uint8_t *)rwTexture + g_zrwTxrPluginOffset);
}

static ZglTexture *zrwTxrNewObject(void)
{
    ZglTexture *tex = (ZglTexture *)zmemFreeListAlloc(g_zrwTxr.pool);
    zstringMemSet(tex, 0, sizeof(ZglTexture));
    tex->type = (tex->type & ~0xFFu) | ZGL_TYPE_TEXTURE;
    return tex;
}

/* VA 0x00497b00, 680 bytes, [V] direct, status draft
 * Texture driver slot 3. buf/len = a complete .bmp (type 0) or .png (type 1) file; 4 (dds) has no
 * reader. The buffer is mounted as an in-memory file with the extension "BMP"/"PNG" and
 * RwTextureRead(name) runs: it looks the name up in the current dictionary and otherwise reads the
 * image through the registered readers (zrwTxr_ReadBmp / ReadPng, so the colour key applies) and
 * zrwTxrReadCallBack (filtering, addressing). If that fails the image is read again as a BMP and the
 * texture built by hand: a 16-bit 565 texture raster, else a 32-bit default one, at the image's size. */
ZglTexture *zrwTxr_CreateTextureFromMemory(void *buf, int len, const char *name, int type, int unused)
{
    const char *ext;
    void *rwTex;
    RwImage *img;
    void *raster;
    uint32_t err[2];
    ZglTexture *tex;
    (void)unused;

    if (buf == NULL || len == 0) {
        zAssertFail("Error in parameters", ZRWTXR_SRC, 0x1ED, 0);
        return NULL;
    }
    if (type == ZGL_IMAGE_BMP)
        ext = "BMP";
    else if (type == ZGL_IMAGE_PNG)
        ext = "PNG";
    else if (type == ZGL_IMAGE_DDS)
        ext = NULL;
    else {
        zAssertFail("Bad EzglImageTypes", ZRWTXR_SRC, 0x1FD, 0);
        return NULL;
    }

    zrwRamDiskBegin();
    zrwRamDiskAddFile(buf, len, ext);
    zrwRamDiskAddFile(buf, len, ext);           /* registered twice [I: image + mask lookups] */
    rwTex = RwTextureRead(name, NULL);
    zrwRamDiskEnd();

    if (rwTex == NULL) {
        RwErrorGet(err);                   /* clear the RW error of the failed read */
        zrwRamDiskBegin();
        zrwRamDiskAddFile(buf, len, ext);
        img = RtBMPImageRead(name);
        if (img != NULL) {
            zrwTxr_ApplyColorKey(img);
            g_zrwTxr.lastWidth = RwImageGetWidth(img);
            g_zrwTxr.lastHeight = RwImageGetHeight(img);
            g_zrwTxr.lastDepth = RwImageGetDepth(img);
            raster = RwRasterCreate(RwImageGetWidth(img), RwImageGetHeight(img), 16,
                                    RWFMT_565 | RWRASTER_TEXTURE);
            if (raster == NULL)
                raster = RwRasterCreate(RwImageGetWidth(img), RwImageGetHeight(img), 32,
                                        RWFMT_DEFAULT | RWRASTER_TEXTURE);
            if (raster != NULL) {
                g_zrwTxr.lastWidth = RwRasterGetWidth(raster);
                g_zrwTxr.lastHeight = RwRasterGetHeight(raster);
                raster = RwRasterSetFromImage(raster, img);
                RwImageDestroy(img);
                if (raster != NULL) {
                    void *t = RwTextureCreate(raster);
                    if (t != NULL) {
                        RwTextureSetRaster(t, raster);
                        RwTextureSetName(t, name);
                        RwTextureSetFilterMode(t, g_zrwTxr.mipmapping ? RWFILTER_LINEARMIPLINEAR
                                                                      : RWFILTER_LINEAR);
                        RwTextureSetAddressing(t, (g_zrwZglFlags & 1) ? RWADDRESS_CLAMP
                                                                     : RWADDRESS_WRAP);
                        RwTexDictionaryAddTexture(g_zrwTxr.texDict, t);
                        rwTex = t;
                    }
                }
            }
        }
        zrwRamDiskEnd();
        RwErrorGet(err);
        if (rwTex == NULL) {
            zAssertFail("Failed to create texture", ZRWTXR_SRC, 0x277, 0);
            return NULL;
        }
    }

    tex = zrwTxrNewObject();
    g_zrwTxr.lastDepth = RwRasterGetDepth(RwTextureGetRaster(rwTex));
    tex->rwTexture = rwTex;
    tex->name = RwTextureGetName(rwTex);
    tex->nameHash = zHashName(tex->name);
    tex->width = g_zrwTxr.lastWidth;
    tex->refCount++;
    tex->height = g_zrwTxr.lastHeight;
    tex->depth = g_zrwTxr.lastDepth;
    return tex;
}

/* VA 0x00497db0, 616 bytes, [I] DM-059 created, status draft
 * An 8-bit paletted texture from raw pixels + a 256-entry RGBA palette (0x300 bytes are copied:
 * [I] the caller passes RGB triplets padded to RGBA?). If the driver has no PAL8 raster it falls back
 * to a 16-bit 1555 raster and converts every pixel, 0x7C1F (magenta) -> 0 (transparent), the
 * others get the alpha bit. Called by zrwMatCreatePaletteFromMemory. */
ZglTexture *zrwTxrLoadPalettedImageFromMemory(const uint8_t *pixels, int width, int height,
                                              const RwRGBA *palette, const char *name)
{
    void *raster;
    void *rwTex;
    ZglTexture *tex;
    uint32_t err[2];

    if (pixels == NULL || width == 0 || height == 0 || palette == NULL) {
        g_pfnDebugPrintf("zrwTxrLoadPalettedImageFromMemory: Error in parameters\n");
        return NULL;
    }
    raster = RwRasterCreate(width, height, 8, RWFMT_PAL8 | RWRASTER_TEXTURE);
    if (raster != NULL) {
        void *dst = RwRasterLock(raster, 0, 1);
        void *pal;
        if (dst == NULL) {
            zAssertFail("Unable to lock raster", ZRWTXR_SRC, 0x2B2, 0);
            return NULL;
        }
        zstringMemCopy(dst, pixels, width * height);
        RwRasterUnlock(raster);
        pal = RwRasterLockPalette(raster, 1);
        if (pal == NULL) {
            zAssertFail("Unable to lock raster palette", ZRWTXR_SRC, 0x2B9, 0);
            return NULL;
        }
        zstringMemCopy(pal, palette, 0x300);
        RwRasterUnlockPalette(raster);
    } else {
        uint16_t *px;
        int i;
        raster = RwRasterCreate(width, height, 16, RWFMT_1555 | RWRASTER_TEXTURE);
        if (raster == NULL)
            goto fail;
        RwErrorGet(err);
        px = (uint16_t *)RwRasterLock(raster, 0, 1);
        if (px == NULL) {
            zAssertFail("Unable to lock raster", ZRWTXR_SRC, 0x2CB, 0);
            return NULL;
        }
        for (i = 0; i < width * height; i++, px++) {
            const RwRGBA *c = &palette[pixels[i]];
            uint16_t v = (uint16_t)(((c->blue >> 3) << 10) | ((c->green >> 3) << 5) | (c->red >> 3));
            *px = (v == 0x7C1F) ? 0 : (uint16_t)(v | 0x8000);
        }
        RwRasterUnlock(raster);
    }
    rwTex = RwTextureCreate(raster);
    if (rwTex == NULL)
        goto fail;
    RwTextureSetName(rwTex, name);

    tex = zrwTxrNewObject();
    tex->rwTexture = rwTex;
    tex->name = RwTextureGetName(rwTex);
    tex->nameHash = zHashName(tex->name);
    tex->width = width;
    tex->depth = RwRasterGetDepth(raster);
    tex->refCount++;
    tex->height = height;
    return tex;
fail:
    g_pfnDebugPrintf("zrwTxrLoadPalettedImageFromMemory: Error creating texture\n");
    return NULL;
}

/* VA 0x00498020, 231 bytes, [I] DM-059 created, status draft
 * An empty texture raster of the given size and depth (zrwMatCreateProcedural). */
ZglTexture *zrwTxrCreateProceduralTexture(int width, int height, int depth, const char *name)
{
    void *raster;
    void *rwTex;
    ZglTexture *tex;

    if (width == 0 || height == 0 || depth == 0) {
        g_pfnDebugPrintf("zrwTxrCreateProceduralTexture: Error in parameters\n");
        return NULL;
    }
    raster = RwRasterCreate(width, height, depth, RWRASTER_TEXTURE);
    if (raster == NULL)
        rwTex = NULL;   /* [V] uninitialised in the original (the register still holds width) */
    else {
        rwTex = RwTextureCreate(raster);
        if (rwTex == NULL)
            goto fail;
        RwTextureSetName(rwTex, name);
    }
    if (rwTex != NULL) {
        tex = zrwTxrNewObject();
        tex->rwTexture = rwTex;
        tex->name = RwTextureGetName(rwTex);
        tex->nameHash = zHashName(tex->name);
        tex->width = width;
        tex->refCount++;
        tex->height = height;
        tex->depth = RwRasterGetDepth(raster);
        return tex;
    }
fail:
    g_pfnDebugPrintf("zrwTxrCreateProceduralTexture: Error creating texture\n");
    return NULL;
}

/* VA 0x00498110, 27 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwTxrLock(ZglTexture *tex)
{
    RwRasterLock(RwTextureGetRaster(tex->rwTexture), 0, 1);
}

/* VA 0x00498130, 23 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwTxrUnlock(ZglTexture *tex)
{
    RwRasterUnlock(RwTextureGetRaster(tex->rwTexture));
}

/* VA 0x00498150, 68 bytes, [I] DM-059 created, status draft (no caller found)
 * Fills a ZGL texture from an existing RwTexture. */
int zrwTxrWrapRwTexture(void *rwTexture, ZglTexture *tex, int width, int height, int depth)
{
    tex->rwTexture = rwTexture;
    tex->name = RwTextureGetName(rwTexture);
    tex->nameHash = zHashName(tex->name);
    tex->width = width;
    tex->height = height;
    tex->depth = depth;
    tex->refCount++;
    return 1;
}

/* VA 0x004981a0, 78 bytes, [I] DM-059 created, status draft
 * RwTextureSetReadCallBack hook: the old reader, then the engine's filtering and addressing. */
void *zrwTxrReadCallBack(const char *name, const char *mask)
{
    void *t = g_zrwTxr.oldReadCallBack(name, mask);
    if (t == NULL)
        return NULL;
    RwTextureSetFilterMode(t, g_zrwTxr.mipmapping ? RWFILTER_LINEARMIPLINEAR : RWFILTER_LINEAR);
    if (g_zrwZglFlags & 1)
        RwTextureSetAddressing(t, RWADDRESS_CLAMP);
    return t;
}

/* VA 0x004981f0, 174 bytes, [V] direct, status draft
 * IFF handler 'TBMP' (inside a MATI): the chunk is a complete .bmp file. The name is the MADF
 * name (zrwTxr_SetPendingName); the new texture becomes a material (zrwMat_CreateFromTexture). */
int zrwTxr_ChunkTBMP(void *iff)
{
    uint32_t size;
    void *dest;
    ZglTexture *tex;

    zrwMat_CurrentMadfFlag0();             /* result unused */
    size = ziffChunkGetSize((ZiffFile *)iff);
    if (g_zrwTxr.loadBuffer == NULL)
        dest = zmemAlloc(size);
    else {
        if (g_zrwTxr.loadBufferSize < size)
            zAssertFail("Load buffer is too small.", ZRWTXR_SRC, 0x396, 0);
        dest = g_zrwTxr.loadBuffer;
    }
    ziffChunkReadData((ZiffFile *)iff, dest, size);
    tex = zrwTxr_CreateTextureFromMemory(dest, size, g_zrwTxr.pendingName, ZGL_IMAGE_BMP, 0);
    if (tex == NULL)
        g_pfnDebugPrintf("zrwTxr: Error loading texture %s\n", g_zrwTxr.pendingName);
    else
        zrwMat_CreateFromTexture(tex);
    if (dest != g_zrwTxr.loadBuffer)
        zmemFree(dest);
    return 1;
}

/* VA 0x004982a0, 121 bytes, [V] direct, status draft (IFF handler 'TPNG'; no load buffer) */
int zrwTxr_ChunkTPNG(void *iff)
{
    uint32_t size = ziffChunkGetSize((ZiffFile *)iff);
    void *dest = zmemAlloc(size);
    ZglTexture *tex;

    ziffChunkReadData((ZiffFile *)iff, dest, size);
    tex = zrwTxr_CreateTextureFromMemory(dest, size, g_zrwTxr.pendingName, ZGL_IMAGE_PNG, 0);
    if (tex == NULL) {
        g_pfnDebugPrintf("zrwTxr: Error loading texture %s\n", g_zrwTxr.pendingName);
        zmemFree(dest);
        return 1;
    }
    zrwMat_CreateFromTexture(tex);
    zmemFree(dest);
    return 1;
}

/* VA 0x00498320, 6 bytes, [I] between, status draft (IFF handler 'TDDS': ignored) */
int zrwTxr_ChunkTDDS(void *iff)
{
    (void)iff;
    return 1;
}

/* VA 0x00498330, 184 bytes, [I] DM-059 created, status draft (no caller found)
 * Pixel / palette entry `index` of a locked raster buffer -> RwRGBA (RwRGBASetFromPixel). */
void zrwTxrPixelToRGBA(const void *pixels, uint32_t format, int index, RwRGBA *out)
{
    uint32_t v;
    uint32_t rgba;

    switch (format & RWFMT_PIXMASK) {
    case RWFMT_1555: case RWFMT_565: case RWFMT_4444: case RWFMT_16:
        v = ((const uint16_t *)pixels)[index]; break;
    case RWFMT_LUM8:
        v = ((const uint8_t *)pixels)[index]; break;
    case RWFMT_8888: case RWFMT_888: case RWFMT_24: case RWFMT_32:
        v = ((const uint32_t *)pixels)[index]; break;
    default:
        v = (uint32_t)(size_t)pixels; break;     /* sic [V]: the buffer address */
    }
    RwRGBASetFromPixel((RwRGBA *)&rgba, v, format);
    out[index] = *(RwRGBA *)&rgba;
}

/* VA 0x004983f0, 399 bytes, [I] callers, status draft
 * RwRGBA -> palette entry `index` in the raster's pixel format (zrwTxrSetPalette). */
void zrwTxrRGBAToPixel(void *pixels, uint32_t format, int index, const RwRGBA *in)
{
    const RwRGBA c = in[index];

    switch (format & RWFMT_PIXMASK) {
    case RWFMT_4444:
        ((uint16_t *)pixels)[index] = (uint16_t)(((c.alpha & 0xF0) << 8) | ((c.blue & 0xF0) << 4) |
                                                 (c.green & 0xF0) | (c.red >> 4));
        break;
    case RWFMT_1555:
        ((uint16_t *)pixels)[index] = (uint16_t)(((c.alpha & 0x80) << 8) | ((c.blue & 0xF8) << 7) |
                                                 ((c.green & 0xF8) << 2) | (c.red >> 3));
        break;
    case RWFMT_565:
        ((uint16_t *)pixels)[index] = (uint16_t)(((c.blue & 0xF8) << 8) | ((c.green & 0xFC) << 3) |
                                                 (c.red >> 3));
        break;
    case RWFMT_8888:
        ((uint32_t *)pixels)[index] = ((uint32_t)c.alpha << 24) | ((uint32_t)c.blue << 16) |
                                      ((uint32_t)c.green << 8) | c.red;
        break;
    case RWFMT_888:
        ((uint32_t *)pixels)[index] = ((uint32_t)c.blue << 16) | ((uint32_t)c.green << 8) | c.red;
        break;
    }
}

/* VA 0x00498580, 57 bytes, [I] callers, status draft (zmemFreeListForEach callback of PrintAll) */
int zrwTxrPrintCB(void *obj, void *unused)
{
    ZglTexture *tex = (ZglTexture *)obj;
    (void)unused;
    g_pfnDebugPrintf("Name:%s Width:%d Height:%d BPP:%d\n", tex->name, tex->width, tex->height,
                     tex->depth);
    zrwTxrPrintRasterInfo(RwTextureGetRaster(tex->rwTexture));
    return 1;
}

static void zrwTxrSetupDictionary(void)
{
    g_zrwTxr.texDict = RwTexDictionaryCreate();
    if (g_zrwTxr.texDict != NULL) {
        RwTexDictionarySetCurrent(g_zrwTxr.texDict);
        RwImageRegisterImageFormat("bmp", zrwTxr_ReadBmp, (void *)0x004E7EB0 /* RtBMPImageWrite */);
        RwImageRegisterImageFormat("png", zrwTxr_ReadPng, NULL);
        RwImageRegisterImageFormat("dds", zrwTxr_ReadDds, NULL);
        g_zrwTxr.mipmapping = 0;
        RwTextureSetMipmapping(0);
        RwTextureSetAutoMipmapping(0);
        g_zrwTxr.oldReadCallBack = RwTextureGetReadCallBack();
        RwTextureSetReadCallBack(zrwTxrReadCallBack);
    }
}

/* VA 0x004985c0, 122 bytes, [I] DM-059 created, status draft
 * A fresh texture dictionary + the image readers (App_Open, after RwEngineStart). */
void zrwTxrOpenDictionary(void)
{
    zrwTxrSetupDictionary();
}

/* VA 0x00498640, 32 bytes, [I] DM-059 created, status draft (App_Open's shutdown path) */
void zrwTxrCloseDictionary(void)
{
    RwTexDictionaryDestroy(g_zrwTxr.texDict);
    g_zrwTxr.texDict = NULL;
    RwTexDictionarySetCurrent(NULL);
}

/* VA 0x00498660, 295 bytes, [V] direct, status draft (texture driver slot 0) */
int zrwTxr_Init(void)
{
    if (g_zrwTxr.initialized)
        zAssertFail("Already initialized", ZRWTXR_SRC, 0x4F6, 0);
    zstringMemSet(&g_zrwTxr, 0, sizeof(g_zrwTxr));
    g_zrwTxr.initialized = 1;
    g_zrwTxr.pool = zmemFreeListCreate(sizeof(ZglTexture), 0x100,
                                       (ZmemFreeListCallback)zrwTxrRelease);
    g_zrwTxr.colorKey.red = 0xFF;
    g_zrwTxr.colorKey.green = 0;
    g_zrwTxr.colorKey.blue = 0xFF;
    g_zrwTxr.colorKey.alpha = 0;
    ziffRegisterHandler(0x504D4254 /* 'TBMP' */, (ZiffHandlerFn)zrwTxr_ChunkTBMP);
    ziffRegisterHandler(0x474E5054 /* 'TPNG' */, (ZiffHandlerFn)zrwTxr_ChunkTPNG);
    ziffRegisterHandler(0x53444454 /* 'TDDS' */, (ZiffHandlerFn)zrwTxr_ChunkTDDS);
    zrwTxrSetupDictionary();
    return 1;
}

/* VA 0x00498790, 68 bytes, [I] DM-059 created, status draft (texture driver slot 1) */
int zrwTxrShutdown(void)
{
    if (g_zrwTxr.initialized) {
        zmemFreeListDestroy(g_zrwTxr.pool);
        zrwTxrCloseDictionary();
        g_zrwTxr.initialized = 0;
    }
    return 1;
}

/* VA 0x004987e0, 41 bytes, [I] DM-059 created, status draft (texture driver slot 2: empty object) */
ZglTexture *zrwTxrCreate(void)
{
    return zrwTxrNewObject();
}

/* VA 0x00498810, 81 bytes, [V] direct, status draft (texture driver slot 4)
 * Drops one RW reference and one ZGL reference; frees the object at 0. */
int zrwTxrRelease(ZglTexture *tex)
{
    RwTextureDestroy(tex->rwTexture);
    if (--tex->refCount < 0)
        zAssertFail("Texture reference count has become negative", ZRWTXR_SRC, 0x551, 0);
    if (tex->refCount == 0)
        zmemFreeListFree(g_zrwTxr.pool, tex);
    return 1;
}

/* VA 0x00498870, 8 bytes, [I] callers, status draft (slot 5) */
int32_t zrwTxrGetWidth(ZglTexture *tex) { return tex->width; }

/* VA 0x00498880, 8 bytes, [I] callers, status draft (slot 6) */
int32_t zrwTxrGetHeight(ZglTexture *tex) { return tex->height; }

/* VA 0x00498890, 8 bytes, [I] DM-059 created, status draft (slot 7) */
int32_t zrwTxrGetDepth(ZglTexture *tex) { return tex->depth; }

/* VA 0x004988a0, 8 bytes, [I] callers, status draft (slot 8) */
const char *zrwTxrGetName(ZglTexture *tex) { return tex->name; }

/* VA 0x004988b0, 8 bytes, [I] callers, status draft (slot 9) */
uint32_t zrwTxrGetNameHash(ZglTexture *tex) { return tex->nameHash; }

/* VA 0x004988c0, 344 bytes, [I] DM-059 created, status draft (slot 10)
 * The palette of a 4/8-bit texture as RwRGBA[1 << depth] in g_zrwTxr.palette (NULL if not
 * paletted). */
RwRGBA *zrwTxrGetPalette(ZglTexture *tex)
{
    void *raster = RwTextureGetRaster(tex->rwTexture);
    uint32_t format = RwRasterGetFormat(raster);
    const uint8_t *pal;
    int i;

    if ((format & (RWFMT_PAL8 | RWFMT_PAL4)) == 0)
        return NULL;
    pal = (const uint8_t *)RwRasterLockPalette(raster, 2);
    if (pal == NULL)
        return NULL;
    for (i = 0; i < (1 << tex->depth); i++)
        zrwTxrPixelToRGBA(pal, format, i, g_zrwTxr.palette);
    RwRasterUnlockPalette(raster);
    return g_zrwTxr.palette;
}

/* VA 0x00498a20, 159 bytes, [I] callers, status draft (slot 11)
 * Writes g_zrwTxr.palette back into the raster palette (palette animation, zroller FAN materials). */
int zrwTxrSetPalette(ZglTexture *tex)
{
    void *raster = RwTextureGetRaster(tex->rwTexture);
    uint32_t format = RwRasterGetFormat(raster);
    void *pal;
    int i;

    if ((format & (RWFMT_PAL8 | RWFMT_PAL4)) == 0) {
        g_pfnDebugPrintf("Texture isn't paletted: %s\n", tex->name);
        return 0;
    }
    pal = RwRasterLockPalette(raster, 1);
    if (pal == NULL)
        return 0;
    for (i = 0; i < (1 << tex->depth); i++)
        zrwTxrRGBAToPixel(pal, format, i, g_zrwTxr.palette);
    RwRasterUnlockPalette(raster);
    return 1;
}

/* VA 0x00498ac0, 8 bytes, [I] callers, status draft */
void *zrwTxrGetRwTexture(ZglTexture *tex) { return tex->rwTexture; }

/* VA 0x00498ad0, 17 bytes, [I] callers, status draft */
void *zrwTxrGetRaster(ZglTexture *tex) { return RwTextureGetRaster(tex->rwTexture); }

/* VA 0x00498af0, 22 bytes, [I] callers, status draft (slot 12) */
void zrwTxrAddRef(ZglTexture *tex)
{
    tex->refCount++;
    RwTextureAddRef(tex->rwTexture);
}

/* VA 0x00498b10, 43 bytes, [I] callers, status draft (App_Open, before RwEngineStart) */
int zrwTxrRegisterPlugin(void)
{
    g_zrwTxrPluginOffset = RwTextureRegisterPlugin(sizeof(void *), ZRWTXR_PLUGIN_ID,
                                                   zrwTxrPluginConstructor, zrwTxrPluginDestructor,
                                                   NULL);
    return g_zrwTxrPluginOffset != -1;
}

/* VA 0x00498b40, 10 bytes, [I] callers, status draft (zrwMat_ChunkMADF) */
void zrwTxr_SetPendingName(const char *name)
{
    g_zrwTxr.pendingName = name;
}

/* VA 0x00498b50, 24 bytes, [I] callers, status draft (the material that wraps the texture) */
void zrwTxrSetOwner(ZglTexture *tex, void *owner)
{
    if (g_zrwTxrPluginOffset > 0)
        *(void **)((uint8_t *)tex->rwTexture + g_zrwTxrPluginOffset) = owner;
}

/* VA 0x00498b70, 28 bytes, [I] callers, status draft */
void zrwTxrSetMipmapping(int on)
{
    g_zrwTxr.mipmapping = on;
    RwTextureSetMipmapping(on);
    RwTextureSetAutoMipmapping(on);
}

/* VA 0x00498b90, 1 bytes, [I] callers, status draft (empty) */
void zrwTxrFlush(void)
{
}

/* VA 0x00498ba0, 46 bytes, [I] DM-059 created, status draft (no caller found)
 * ZGL filter mode 0..5 -> rwFILTERNEAREST..rwFILTERLINEARMIPLINEAR. */
void zrwTxrSetFilterMode(ZglTexture *tex, int mode)
{
    if (mode >= 0 && mode <= 5)
        RwTextureSetFilterMode(tex->rwTexture, mode + 1);
    else
        zAssertFail("Unknown mode", ZRWTXR_SRC, 0x6D6, 0);
}

/* VA 0x00498c70, 52 bytes, [I] DM-059 created, status draft (slot 13; follows the jump table of
 * zrwTxrSetFilterMode) */
int zrwTxrPrintInfo(ZglTexture *tex)
{
    g_pfnDebugPrintf("Name:%s Width:%d Height:%d BPP:%d\n", tex->name, tex->width, tex->height,
                     tex->depth);
    zrwTxrPrintRasterInfo(RwTextureGetRaster(tex->rwTexture));
    return 1;
}

/* VA 0x00498cb0, 62 bytes, [I] DM-059 created, status draft (slot 14) */
void zrwTxrPrintAll(void)
{
    g_pfnDebugPrintf("ZGLTEXTURE: Objects in use:%d\n", zmemFreeListGetUsedCount(g_zrwTxr.pool));
    g_pfnDebugIndent(1);
    zmemFreeListForEach(g_zrwTxr.pool, (ZmemFreeListIterFn)zrwTxrPrintCB, NULL);
    g_pfnDebugIndent(-1);
}

/* VA 0x00498cf0, 74 bytes, [V] direct, status draft (no caller found, DM-014) */
void zrwTxr_SetLoadBuffer(void *buf, uint32_t size)
{
    if (g_zrwTxr.loadBuffer != NULL)
        zAssertFail("Load buffer has already been preallocated.", ZRWTXR_SRC, 0x710, 0);
    g_zrwTxr.loadBuffer = buf;
    g_zrwTxr.loadBufferSize = size;
}

/* VA 0x00498d40, 50 bytes, [I] DM-059 created, status draft (no caller found) */
void zrwTxrClearLoadBuffer(void)
{
    if (g_zrwTxr.loadBuffer == NULL)
        zAssertFail("Load buffer has already been destroyed.", ZRWTXR_SRC, 0x71F, 0);
    g_zrwTxr.loadBuffer = NULL;
    g_zrwTxr.loadBufferSize = 0;
}

/* The texture driver table (slot 13 0x00498C70, slot 14 0x00498CB0) */
const ZglTextureApi g_texDriverVtbl = {                                      /* 0x005623F0 */
    zrwTxr_Init, zrwTxrShutdown, zrwTxrCreate, zrwTxr_CreateTextureFromMemory, zrwTxrRelease,
    zrwTxrGetWidth, zrwTxrGetHeight, zrwTxrGetDepth, zrwTxrGetName, zrwTxrGetNameHash,
    zrwTxrGetPalette, zrwTxrSetPalette, zrwTxrAddRef, zrwTxrPrintInfo, zrwTxrPrintAll
};

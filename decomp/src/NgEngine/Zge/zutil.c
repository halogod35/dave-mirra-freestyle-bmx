/*
 * NgEngine/Zge/zutil.c
 * Original: C:\NgEngine\Zge\zutil.c
 * Evidence: [V] zutilBBoxTransform (DM-061) asserts with the path string 0x005637E4; the
 *           literals right after it (the pool-map box strings 0x005637FC-0x0056385C) and the plane
 *           table 0x00563770 before it are used only by the code at 0x004A2F50-0x004A31F3, which
 *           sits between zFont.c and zrwRamDisk.c ("Error opening RAM disk file." 0x00563878 is
 *           used from 0x004A3200 on). "pBBoxOut==pBBoxIn" 0x005637D0 belongs to a compiled-out
 *           assert of this file.
 * 10 functions, VA 0x004a2c70-0x004a31f3.
 * Decompiled by DM-044 (non-matching C, AD-8); DM-061 created the five box helpers 0x004A2C70-
 * 0x004A2E10 in the hole after zFont.c (DM-044 thought the path string unreferenced).
 *
 * A bounding box is float[8]: min x,y,z at [0..2] and max x,y,z at [4..6] (RwBBox-like, 0x20).
 */
#include "decomp.h"
#include "ztools.h"

#include "zmath.h"

#define ZUTIL_FILE "C:\\NgEngine\\Zge\\zutil.c"

/* zmath helpers in the gap after zrwCol.c (names DM-044 [I]) */
void *zmathPlaneFromPointNormal(float *plane, const float *point, const float *normal);  /* 0x004956F0 */
int   zmathClipSegmentToPlane(float *p0, float *p1, const float *plane);                 /* 0x00495730 */
double __cdecl fabs(double x);                                                    /* CRT intrinsic (fabs) */

void zutilPrintPoolBlock(void *data, uint32_t size, int isFree, void *user);

/* 0x00563770: inward normals of the six box faces: +z, +y, +x (through min), -z, -y, -x (through max) */
const float g_zutilBBoxPlaneNormals[6][4] = {
    { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f, 0.0f },
};

/* VA 0x004a2c70, 36 bytes, [I] row, status draft */
float *zutilBBoxInit(float *box, const float *point)
{
    box[0] = box[4] = point[0];
    box[1] = box[5] = point[1];
    box[2] = box[6] = point[2];
    return box;
}

/* VA 0x004a2ca0, 119 bytes, [I] row, status draft */
float *zutilBBoxAddPoint(float *box, const float *point)
{
    if (point[0] < box[0]) box[0] = point[0];
    if (point[0] > box[4]) box[4] = point[0];
    if (point[1] < box[1]) box[1] = point[1];
    if (point[1] > box[5]) box[5] = point[1];
    if (point[2] < box[2]) box[2] = point[2];
    if (point[2] > box[6]) box[6] = point[2];
    return box;
}

/* VA 0x004a2d20, 161 bytes, [I] row, status draft */
float *zutilBBoxUnion(float *out, const float *a, const float *b)
{
    out[0] = a[0] < b[0] ? a[0] : b[0];
    out[4] = a[4] > b[4] ? a[4] : b[4];
    out[1] = a[1] < b[1] ? a[1] : b[1];
    out[5] = a[5] > b[5] ? a[5] : b[5];
    out[2] = a[2] < b[2] ? a[2] : b[2];
    out[6] = a[6] > b[6] ? a[6] : b[6];
    return out;
}

/* VA 0x004a2dd0, 52 bytes, [I] row, status draft */
void zutilBBoxHalfSize(float *out, const float *box)
{
    out[0] = (box[4] - box[0]) * 0.5f;
    out[1] = (box[5] - box[1]) * 0.5f;
    out[2] = (box[6] - box[2]) * 0.5f;
}

/* VA 0x004a2e10, 316 bytes, [I] row, status draft */
float *zutilBBoxTransform(float *out, const float *in, const RwMatrix *m)
{
    RwV3d corner;
    int i;

    if (out == in)
        zAssertFail("pBBoxOut==pBBoxIn", ZUTIL_FILE, 0xA1, 0);
    /* the box around the eight transformed corners (bit 0/1/2 of i picks min or max x/y/z) */
    for (i = 0; i < 8; i++) {
        corner.x = (i & 1) ? in[0] : in[4];
        corner.y = (i & 2) ? in[1] : in[5];
        corner.z = (i & 4) ? in[2] : in[6];
        zmathTransformPoint(&corner, m, &corner);
        if (i == 0)
            zutilBBoxInit(out, &corner.x);           /* inlined */
        else
            zutilBBoxAddPoint(out, &corner.x);       /* inlined */
    }
    return out;
}

/* VA 0x004a2f50, 92 bytes, [I] row, status draft */
int zutilBBoxContainsPoint(const float *bbox, const float *p)
{
    /* inclusive on both ends; called from 0x0049437E */
    return p[0] >= bbox[0] && p[0] <= bbox[4] &&
           p[1] >= bbox[1] && p[1] <= bbox[5] &&
           p[2] >= bbox[2] && p[2] <= bbox[6];
}

/* VA 0x004a2fb0, 87 bytes, [I] row, status draft */
int zutilBBoxOverlap(const float *a, const float *b)
{
    /* Probably meant as a box overlap test (|centre a - centre b| <= half a + half b per axis), but
     * [V] it compares only the half extents: x + y < |x - y| is never true for x, y >= 0, so it
     * returns 1 for any two valid boxes. Uncalled. */
    float ha, hb;
    int i;

    for (i = 0; i < 3; i++) {
        ha = (a[4 + i] - a[i]) * 0.5f;
        hb = (b[4 + i] - b[i]) * 0.5f;
        if (ha + hb < (float)fabs(ha - hb))
            return 0;
    }
    return 1;
}

/* VA 0x004a3010, 365 bytes, [I] row, status draft */
int zutilClipSegmentToBBox(float *p0, float *p1, const float *bbox)
{
    /* Clips the segment p0-p1 to the box in place. Returns 0 if it lies entirely outside (p0/p1
     * unchanged), else 1. Called from 0x00493E69. */
    float a[4], b[4], plane[5];
    int i;

    if (zutilBBoxContainsPoint(bbox, p0) && zutilBBoxContainsPoint(bbox, p1))   /* both inlined */
        return 1;
    a[0] = p0[0]; a[1] = p0[1]; a[2] = p0[2];
    b[0] = p1[0]; b[1] = p1[1]; b[2] = p1[2];
    for (i = 0; i < 6; i++) {
        zmathPlaneFromPointNormal(plane, i < 3 ? &bbox[0] : &bbox[4], g_zutilBBoxPlaneNormals[i]);
        if (!zmathClipSegmentToPlane(a, b, plane))
            return 0;
    }
    p0[0] = a[0]; p0[1] = a[1]; p0[2] = a[2];
    p1[0] = b[0]; p1[1] = b[1]; p1[2] = b[2];
    return 1;
}

/* VA 0x004a3180, 32 bytes, [I] row, status draft */
void zutilPrintPoolMap(ZmemPool *pool)
{
    /* Debug dump of a memory pool, one box per block. Uncalled. */
    zmemPoolWalk(pool, zutilPrintPoolBlock, NULL);
    g_pfnDebugPrintf("+--------------------+\n");
}

/* VA 0x004a31a0, 84 bytes, [I] row, status draft */
void zutilPrintPoolBlock(void *data, uint32_t size, int isFree, void *user)
{
    (void)data;
    (void)user;
    g_pfnDebugPrintf("+--------------------+\n");
    if (isFree) {
        g_pfnDebugPrintf("| %-7d            |\n", size);
        g_pfnDebugPrintf("|                    |\n");
    } else {
        g_pfnDebugPrintf("| %-7d XXXXXXXXXXX|\n", size);
        g_pfnDebugPrintf("|XXXXXXXXXXXXXXXXXXXX|\n");
    }
}

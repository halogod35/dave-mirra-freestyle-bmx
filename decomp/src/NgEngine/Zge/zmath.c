/*
 * NgEngine/Zge/zmath.c
 * Original: C:\NgEngine\Zge\zmath.c (name invented in the original style: no path string; DM-040)
 * Evidence: [I] link order (between zasset.c and zobjCamera.c, whose vtable 0x00560518 starts with
 * 0x00482090), the shared tables 0x0065E2BC-0x006702D3 and the zmath* call sites.
 * 59 functions, VA 0x00480c40-0x00482081.
 * DM-045 (draft). Non-matching C (AD-8). Types and conventions: decomp/include/zmath.h.
 *
 * Three groups:
 *  - trig: an 8192-entry-per-turn sine table (+ a quarter turn so cos = sin + 2048), and int16
 *    asin / atan tables indexed by trunc(4096 * x) (8193 entries, centred);
 *  - RwV3d helpers (add, scale, dot, cross, rotate by a binary angle, ...);
 *  - RwMatrix helpers (identity, rotate about X/Y/Z, multiply, rigid inverse, matrix -> Euler).
 * DM-045 found 12 functions Ghidra had not created (ApplySymbols creates them from decomp/symbols/symbols.csv):
 * zmathFreeTables and zmathShutdown (each called once) and ten with no callers in the exe: Floor,
 * Ceil, Atan, VecMul, VecMidpoint, TransformPoints, MatrixIdentityRotation, MatrixScaleUniform,
 * MatrixCopyTranslation, SeedRandom.
 * DM-061 created three more in the zasset/zmath hole (0x00480C40-0x00480D1A): the three table
 * builders of zmathInitTables as separate functions, none of them called.
 */
#include "decomp.h"
#include "zmath.h"

double sin(double x);
double asin(double x);
double atan(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double fabs(double x);

#define ZMATH_SIN_ENTRIES   8192                  /* per full turn */
#define ZMATH_TAB_HALF      4096                  /* asin/atan tables: index -4096..4096 */
#define ZMATH_RAD_PER_UNIT  9.58738019e-05f       /* 0x0054DCC4: 2 pi / 65536 */
#define ZMATH_UNITS_PER_RAD 10430.3779f           /* 0x0054DCC8: 65536 / (2 pi) */
#define ZMATH_SIN_INDEX(a)  (((unsigned)(a) & 0xFFFF) >> 3)

float    g_sinTable[ZMATH_SIN_ENTRIES + ZMATH_SIN_ENTRIES / 4];   /* 0x0065E2BC (10240 floats) */
float   *g_cosTablePtr;                                           /* 0x006682BC = &g_sinTable[2048] */
int16_t  g_zmathAsinTable[2 * ZMATH_TAB_HALF + 1];                /* 0x006682C0 */
int16_t *g_zmathAsinCentre;                                       /* 0x0066C2C4 = &g_zmathAsinTable[4096] */
int16_t  g_zmathAtanTable[2 * ZMATH_TAB_HALF + 1];                /* 0x0066C2C8 */
int16_t *g_zmathAtanCentre;                                       /* 0x006702CC = &g_zmathAtanTable[4096] */
uint32_t g_zmathRandomSeed;                                       /* 0x006702D0 */

/* ------------------------------------------------------------------ tables and trig */

/* VA 0x00480c40, 60 bytes, [I] row, status draft */
void zmathInitSinTable(void)
{
    int i;

    /* zmathInitTables' first loop on its own (no caller in the exe) */
    g_cosTablePtr = &g_sinTable[ZMATH_SIN_ENTRIES / 4];
    for (i = 0; i < ZMATH_SIN_ENTRIES + ZMATH_SIN_ENTRIES / 4; i++)
        g_sinTable[i] = (float)sin((int16_t)(i * 8) * ZMATH_RAD_PER_UNIT);
}

/* VA 0x00480c80, 75 bytes, [I] row, status draft */
void zmathInitAsinTable(void)
{
    int i;

    g_zmathAsinCentre = &g_zmathAsinTable[ZMATH_TAB_HALF];
    for (i = 0; i <= 2 * ZMATH_TAB_HALF; i++)
        g_zmathAsinTable[i] = (int16_t)(int)(asin((i - ZMATH_TAB_HALF) * (1.0 / 4096.0)) * ZMATH_UNITS_PER_RAD);
}

/* VA 0x00480cd0, 74 bytes, [I] row, status draft */
void zmathInitAtanTable(void)
{
    int i;

    g_zmathAtanCentre = &g_zmathAtanTable[ZMATH_TAB_HALF];
    for (i = 0; i <= 2 * ZMATH_TAB_HALF; i++)
        g_zmathAtanTable[i] = (int16_t)(int)(atan((i - ZMATH_TAB_HALF) * (1.0 / 4096.0)) * ZMATH_UNITS_PER_RAD);
}

/* VA 0x00480d20, 204 bytes, [I] row, status draft */
int zmathInitTables(void)
{
    int i;

    /* sin of every 8th binary angle; the angle is wrapped to int16 first, so the second half of
     * the turn (and the extra quarter used by cos) is computed from negative angles */
    g_cosTablePtr = &g_sinTable[ZMATH_SIN_ENTRIES / 4];
    for (i = 0; i < ZMATH_SIN_ENTRIES + ZMATH_SIN_ENTRIES / 4; i++)
        g_sinTable[i] = (float)sin((int16_t)(i * 8) * ZMATH_RAD_PER_UNIT);

    /* asin(x) and atan(x) for x = (i - 4096) / 4096, as binary angles (truncated by __ftol) */
    g_zmathAsinCentre = &g_zmathAsinTable[ZMATH_TAB_HALF];
    for (i = 0; i <= 2 * ZMATH_TAB_HALF; i++)
        g_zmathAsinTable[i] = (int16_t)(int)(asin((i - ZMATH_TAB_HALF) * (1.0 / 4096.0)) * ZMATH_UNITS_PER_RAD);
    g_zmathAtanCentre = &g_zmathAtanTable[ZMATH_TAB_HALF];
    for (i = 0; i <= 2 * ZMATH_TAB_HALF; i++)
        g_zmathAtanTable[i] = (int16_t)(int)(atan((i - ZMATH_TAB_HALF) * (1.0 / 4096.0)) * ZMATH_UNITS_PER_RAD);
    return 1;
}

/* VA 0x00480df0, 6 bytes, [I] row, status draft */
int zmathFreeTables(void)
{
    return 1;   /* the tables are static */
}

/* VA 0x00480e00, 19 bytes, [I] row, status draft */
float zmathFloor(float x)
{
    return (float)floor(x);
}

/* VA 0x00480e20, 19 bytes, [I] row, status draft */
float zmathCeil(float x)
{
    return (float)ceil(x);
}

/* VA 0x00480e40, 20 bytes, [I] row, status draft */
float zmathSin(int angle)
{
    return g_sinTable[ZMATH_SIN_INDEX(angle)];
}

/* Table lookup shared by the inverse functions: the exe computes i = __ftol(x * -4096.0f) and
 * reads centre[-i], i.e. entry trunc(4096 x). */
#define ZMATH_INV_LOOKUP(centre, x)  ((centre)[-(int)((x) * -4096.0f)])

/* VA 0x00480e60, 75 bytes, [I] row, status draft */
int16_t zmathAsin(float x)
{
    if (x >= 1.0f)
        x = 1.0f;
    else if (x <= -1.0f)
        x = -1.0f;
    return ZMATH_INV_LOOKUP(g_zmathAsinCentre, x);
}

/* VA 0x00480eb0, 22 bytes, [I] row, status draft */
float zmathCos(int angle)
{
    return g_cosTablePtr[ZMATH_SIN_INDEX(angle)];
}

/* VA 0x00480ed0, 87 bytes, [I] row, status draft */
int16_t zmathAcos(float x)
{
    if (x >= 1.0f)
        x = 1.0f;
    else if (x <= -1.0f)
        x = -1.0f;
    /* 0x3FFF, not 0x4000: acos(1) comes out as -1 */
    return (int16_t)(0x3FFF - ZMATH_INV_LOOKUP(g_zmathAsinCentre, x));
}

/* VA 0x00480f30, 57 bytes, [I] row, status draft */
float zmathTan(int angle)
{
    unsigned i = ZMATH_SIN_INDEX(angle);
    float c = g_cosTablePtr[i];

    if ((float)fabs(c) < 1e-05f)
        return 99999.99f;
    return g_sinTable[i] / c;
}

/* VA 0x00480f70, 116 bytes, [I] row, status draft */
int16_t zmathAtan(float x)
{
    /* the table covers |x| <= 1; beyond that atan(x) = +-90 degrees - atan(1/x) */
    if (x > 1.0f)
        return (int16_t)(0x4000 - ZMATH_INV_LOOKUP(g_zmathAtanCentre, 1.0f / x));
    if (x < -1.0f)
        return (int16_t)(-0x4000 - ZMATH_INV_LOOKUP(g_zmathAtanCentre, 1.0f / x));
    return ZMATH_INV_LOOKUP(g_zmathAtanCentre, x);
}

/* VA 0x00480ff0, 297 bytes, [I] row, status draft */
int16_t zmathAtan2(float y, float x)
{
    int negate = 0;
    int base = 0;
    int angle;

    if (y < 0.0f) {
        y = -y;
        negate = -1;
    }
    if (x < 0.0f) {
        x = -x;
        base = 0x8000;
        negate++;
    }
    /* first-quadrant angle from the smaller ratio (zmathAtan is inlined here in the exe);
     * y == x == 0 divides 0 by 0 */
    if (y > x)
        angle = 0x4000 - zmathAtan(x / y);
    else
        angle = zmathAtan(y / x);
    angle += base;
    if (negate)
        angle = -angle;
    return (int16_t)angle;
}

/* ------------------------------------------------------------------ vectors */

/* VA 0x00481120, 15 bytes, [I] row, status draft */
void zmathVecZero(RwV3d *v)
{
    v->z = 0.0f;
    v->y = 0.0f;
    v->x = 0.0f;
}

/* VA 0x00481130, 37 bytes, [I] row, status draft */
void zmathVecAdd(RwV3d *out, const RwV3d *a, const RwV3d *b)
{
    out->x = a->x + b->x;
    out->y = a->y + b->y;
    out->z = a->z + b->z;
}

/* VA 0x00481160, 37 bytes, [I] row, status draft */
void zmathVecSub(RwV3d *out, const RwV3d *a, const RwV3d *b)
{
    out->x = a->x - b->x;
    out->y = a->y - b->y;
    out->z = a->z - b->z;
}

/* VA 0x00481190, 37 bytes, [I] row, status draft */
void zmathVecMul(RwV3d *out, const RwV3d *a, const RwV3d *b)
{
    out->x = a->x * b->x;
    out->y = a->y * b->y;
    out->z = a->z * b->z;
}

/* VA 0x004811c0, 69 bytes, [I] row, status draft */
float zmathVecNormalize(RwV3d *out, const RwV3d *v)
{
    /* scales to 0.9999, not 1 (keeps dot products of unit vectors inside asin/acos's domain);
     * returns the old length; no zero check */
    float len = (float)sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    float k = 0.9999f / len;

    out->x = k * v->x;
    out->y = k * v->y;
    out->z = k * v->z;
    return len;
}

/* VA 0x00481210, 37 bytes, [I] row, status draft */
void zmathVecScale(RwV3d *out, const RwV3d *v, float s)
{
    out->x = s * v->x;
    out->y = s * v->y;
    out->z = s * v->z;
}

/* VA 0x00481240, 37 bytes, [I] row, status draft */
void zmathVecScaleXYZ(RwV3d *out, const RwV3d *v, float sx, float sy, float sz)
{
    out->x = sx * v->x;
    out->y = sy * v->y;
    out->z = sz * v->z;
}

/* VA 0x00481270, 71 bytes, [I] row, status draft */
void zmathVecMidpoint(RwV3d *out, const RwV3d *a, const RwV3d *b)
{
    out->x = a->x + b->x;
    out->y = a->y + b->y;
    out->z = a->z + b->z;
    out->x *= 0.5f;
    out->y *= 0.5f;
    out->z *= 0.5f;
}

/* VA 0x004812c0, 35 bytes, [I] row, status draft */
float zmathVecLengthSq(const RwV3d *v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

/* VA 0x004812f0, 31 bytes, [I] row, status draft */
void zmathVecNegate(RwV3d *out, const RwV3d *v)
{
    out->x = -v->x;
    out->y = -v->y;
    out->z = -v->z;
}

/* VA 0x00481310, 37 bytes, [I] row, status draft */
float zmathVecLength(const RwV3d *v)
{
    return (float)sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
}

/* VA 0x00481340, 75 bytes, [I] row, status draft */
void zmathVecRotateX(RwV3d *out, const RwV3d *v, int angle)
{
    /* writes y and z only: out->x keeps its value (callers pass out == v) */
    float y = v->y, z = v->z;
    float c = zmathCos(angle), s = zmathSin(angle);

    out->y = c * y - s * z;
    out->z = s * y + c * z;
}

/* VA 0x00481390, 73 bytes, [I] row, status draft */
void zmathVecRotateY(RwV3d *out, const RwV3d *v, int angle)
{
    /* x' = c x - s z, z' = s x + c z: the opposite sense to zmathMatrixRotateY (and to a
     * right-handed rotation about +Y). Writes x and z only. Used for the actor heading. */
    float x = v->x, z = v->z;
    float c = zmathCos(angle), s = zmathSin(angle);

    out->x = c * x - s * z;
    out->z = s * x + c * z;
}

/* VA 0x004813e0, 73 bytes, [I] row, status draft */
void zmathVecRotateZ(RwV3d *out, const RwV3d *v, int angle)
{
    float x = v->x, y = v->y;
    float c = zmathCos(angle), s = zmathSin(angle);

    out->x = c * x - s * y;
    out->y = s * x + c * y;
}

/* VA 0x00481430, 264 bytes, [I] row, status draft */
void zmathVecRotateXYZ(RwV3d *out, const RwV3d *v, const uint16_t *euler)
{
    /* X, then Y (zmathVecRotateY's sense), then Z. The x input is read from out, not v. */
    float x = out->x, y = v->y, z = v->z;
    float c, s;

    c = zmathCos(euler[0]); s = zmathSin(euler[0]);
    out->y = c * y - s * z;
    out->z = c * z + s * y;
    y = out->y;
    z = out->z;

    c = zmathCos(euler[1]); s = zmathSin(euler[1]);
    out->x = c * x - s * z;
    out->z = c * z + s * x;
    x = out->x;

    c = zmathCos(euler[2]); s = zmathSin(euler[2]);
    out->x = c * x - s * y;
    out->y = c * y + s * x;
}

/* VA 0x00481540, 29 bytes, [I] row, status draft */
float zmathVecDot(const RwV3d *a, const RwV3d *b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

/* VA 0x00481560, 75 bytes, [I] row, status draft */
void zmathVecCross(RwV3d *out, const RwV3d *a, const RwV3d *b)
{
    float x = a->y * b->z - a->z * b->y;
    float y = a->z * b->x - a->x * b->z;
    float z = a->x * b->y - a->y * b->x;

    out->x = x;
    out->z = z;
    out->y = y;
}

/* VA 0x004815b0, 133 bytes, [I] row, status draft */
void zmathTriangleNormal(RwV3d *out, const RwV3d *p0, const RwV3d *p1, const RwV3d *p2)
{
    /* (p1 - p0) x (p2 - p1), not normalised */
    RwV3d e1, e2;

    e1.x = p1->x - p0->x;  e1.y = p1->y - p0->y;  e1.z = p1->z - p0->z;
    e2.x = p2->x - p1->x;  e2.y = p2->y - p1->y;  e2.z = p2->z - p1->z;
    out->x = e1.y * e2.z - e1.z * e2.y;
    out->z = e1.x * e2.y - e1.y * e2.x;
    out->y = e1.z * e2.x - e1.x * e2.z;
}

/* ------------------------------------------------------------------ matrix * vector */

/* VA 0x00481640, 92 bytes, [I] row, status draft */
void zmathTransformPoint(RwV3d *out, const RwMatrix *m, const RwV3d *v)
{
    float x = v->x, y = v->y, z = v->z;

    out->x = x * m->right.x + y * m->up.x + z * m->at.x + m->pos.x;
    out->y = x * m->right.y + y * m->up.y + z * m->at.y + m->pos.y;
    out->z = x * m->right.z + y * m->up.z + z * m->at.z + m->pos.z;
}

/* VA 0x004816a0, 220 bytes, [I] row, status draft */
float *zmathTransformPoints(float *out, const RwMatrix *m, const float *in, int count)
{
    /* points are 16 bytes apart (x, y, z, w); w is neither read nor written */
    RwMatrix k = *m;
    float *o = out;

    for (; count > 0; count--, in += 4, o += 4) {
        float x = in[0], y = in[1], z = in[2];

        o[0] = x * k.right.x + k.pos.x + y * k.up.x + z * k.at.x;
        o[1] = x * k.right.y + k.pos.y + y * k.up.y + z * k.at.y;
        o[2] = x * k.right.z + k.pos.z + y * k.up.z + z * k.at.z;
    }
    return out;
}

/* VA 0x00481780, 83 bytes, [I] row, status draft */
void zmathTransformVector(RwV3d *out, const RwMatrix *m, const RwV3d *v)
{
    float x = v->x, y = v->y, z = v->z;

    out->x = x * m->right.x + y * m->up.x + z * m->at.x;
    out->y = x * m->right.y + y * m->up.y + z * m->at.y;
    out->z = x * m->right.z + y * m->up.z + z * m->at.z;
}

/* VA 0x004817e0, 57 bytes, [I] row, status draft */
void zmathVecLerp(RwV3d *out, const RwV3d *a, const RwV3d *b, float t)
{
    out->x = (b->x - a->x) * t + a->x;
    out->y = (b->y - a->y) * t + a->y;
    out->z = (b->z - a->z) * t + a->z;
}

/* VA 0x00481820, 37 bytes, [I] row, status draft */
void zmathVecAddScalar(RwV3d *out, const RwV3d *v, float s)
{
    out->x = s + v->x;
    out->y = s + v->y;
    out->z = s + v->z;
}

/* VA 0x00481850, 37 bytes, [I] row, status draft */
void zmathVecSubScalar(RwV3d *out, const RwV3d *v, float s)
{
    out->x = v->x - s;
    out->y = v->y - s;
    out->z = v->z - s;
}

/* ------------------------------------------------------------------ matrices */

/* VA 0x00481880, 47 bytes, [I] row, status draft */
void zmathMatrixIdentity(RwMatrix *m)
{
    /* rotation and position only; the flags and pad words are left alone */
    m->pos.x = m->pos.y = m->pos.z = 0.0f;
    m->right.x = 1.0f;  m->right.y = 0.0f;  m->right.z = 0.0f;
    m->up.x = 0.0f;     m->up.y = 1.0f;     m->up.z = 0.0f;
    m->at.x = 0.0f;     m->at.y = 0.0f;     m->at.z = 1.0f;
}

/* VA 0x004818b0, 16 bytes, [I] row, status draft */
void zmathMatrixClearTranslation(RwMatrix *m)
{
    m->pos.z = 0.0f;
    m->pos.y = 0.0f;
    m->pos.x = 0.0f;
}

/* VA 0x004818c0, 38 bytes, [I] row, status draft */
void zmathMatrixIdentityRotation(RwMatrix *m)
{
    m->right.x = 1.0f;  m->right.y = 0.0f;  m->right.z = 0.0f;
    m->up.x = 0.0f;     m->up.y = 1.0f;     m->up.z = 0.0f;
    m->at.x = 0.0f;     m->at.y = 0.0f;     m->at.z = 1.0f;
}

/* VA 0x004818f0, 59 bytes, [I] row, status draft */
RwMatrix *zmathMatrixTranslate(RwMatrix *dst, const RwMatrix *src, const RwV3d *t)
{
    if (dst != src)
        zmathMatrixCopyRotation(dst, src);
    dst->pos.x = src->pos.x + t->x;
    dst->pos.y = src->pos.y + t->y;
    dst->pos.z = src->pos.z + t->z;
    return dst;
}

/* The three rotations below copy src into dst (position and rotation, not the flags) when they
 * differ, then rotate every row (right, up, at and pos) about the world axis: in RenderWare terms
 * a post-concatenated rotation. */

/* VA 0x00481930, 199 bytes, [I] row, status draft */
RwMatrix *zmathMatrixRotateX(RwMatrix *dst, const RwMatrix *src, int angle)
{
    RwV3d *row[4];
    float c, s, y;
    int i;

    if (src != dst) {
        dst->pos = src->pos;
        zmathMatrixCopyRotation(dst, src);
    }
    c = zmathCos(angle);
    s = zmathSin(angle);
    row[0] = &dst->right; row[1] = &dst->up; row[2] = &dst->at; row[3] = &dst->pos;
    for (i = 0; i < 4; i++) {
        y = row[i]->y;
        row[i]->y = y * c - s * row[i]->z;
        row[i]->z = y * s + c * row[i]->z;
    }
    return dst;
}

/* VA 0x00481a00, 203 bytes, [I] row, status draft */
void zmathMatrixRotateY(RwMatrix *dst, const RwMatrix *src, int angle)
{
    RwV3d *row[4];
    float c, s, x;
    int i;

    if (src != dst) {
        dst->pos = src->pos;
        zmathMatrixCopyRotation(dst, src);
    }
    c = zmathCos(angle);
    s = zmathSin(angle);
    row[0] = &dst->right; row[1] = &dst->up; row[2] = &dst->at; row[3] = &dst->pos;
    for (i = 0; i < 4; i++) {
        x = row[i]->x;
        row[i]->x = x * c + s * row[i]->z;
        row[i]->z = c * row[i]->z - x * s;
    }
    /* no return value (eax is whatever was left in it) */
}

/* VA 0x00481ad0, 201 bytes, [I] row, status draft */
void zmathMatrixRotateZ(RwMatrix *dst, const RwMatrix *src, int angle)
{
    RwV3d *row[4];
    float c, s, x;
    int i;

    if (src != dst) {
        dst->pos = src->pos;
        zmathMatrixCopyRotation(dst, src);
    }
    c = zmathCos(angle);
    s = zmathSin(angle);
    row[0] = &dst->right; row[1] = &dst->up; row[2] = &dst->at; row[3] = &dst->pos;
    for (i = 0; i < 4; i++) {
        x = row[i]->x;
        row[i]->x = x * c - s * row[i]->y;
        row[i]->y = c * row[i]->y + x * s;
    }
}

/* VA 0x00481ba0, 57 bytes, [I] row, status draft */
RwMatrix *zmathMatrixRotateXYZ(RwMatrix *dst, const RwMatrix *src, const int16_t *euler)
{
    /* X first, then Y, then Z: the same order as zquatFromEuler (q = qz * qy * qx) */
    zmathMatrixRotateX(dst, src, (uint16_t)euler[0]);
    zmathMatrixRotateY(dst, dst, (uint16_t)euler[1]);
    zmathMatrixRotateZ(dst, dst, (uint16_t)euler[2]);
    return dst;
}

/* VA 0x00481be0, 75 bytes, [I] row, status draft */
RwMatrix *zmathMatrixScale(RwMatrix *dst, const RwMatrix *src, const RwV3d *s)
{
    /* scales the diagonal only (right.x, up.y, at.z): correct for axis-aligned matrices */
    if (src != dst) {
        dst->pos = src->pos;
        zmathMatrixCopyRotation(dst, src);
    }
    dst->right.x = src->right.x * s->x;
    dst->up.y = src->up.y * s->y;
    dst->at.z = src->at.z * s->z;
    return dst;
}

/* VA 0x00481c30, 37 bytes, [I] row, status draft */
RwMatrix *zmathMatrixScaleUniform(RwMatrix *dst, RwMatrix *src, float s)
{
    /* as written: scales src's diagonal in place and returns dst untouched (no copy) */
    src->right.x = s * src->right.x;
    src->up.y = s * src->up.y;
    src->at.z = s * src->at.z;
    return dst;
}

/* VA 0x00481c60, 41 bytes, [I] row, status draft */
RwMatrix *zmathMatrixCopy(RwMatrix *dst, const RwMatrix *src)
{
    dst->pos = src->pos;
    zmathMatrixCopyRotation(dst, src);
    return dst;
}

/* VA 0x00481c90, 27 bytes, [I] row, status draft */
void zmathMatrixCopyTranslation(RwMatrix *dst, const RwMatrix *src)
{
    dst->pos = src->pos;
}

/* VA 0x00481cb0, 61 bytes, [I] row, status draft */
void zmathMatrixCopyRotation(RwMatrix *dst, const RwMatrix *src)
{
    dst->right = src->right;
    dst->up = src->up;
    dst->at = src->at;
}

/* VA 0x00481cf0, 406 bytes, [I] row, status draft */
void zmathMatrixMultiply(RwMatrix *out, const RwMatrix *a, const RwMatrix *b)
{
    /* out = a * b in column-vector terms (b applied first): every row of b is transformed by a.
     * a is read into locals first, b is read while out is written, so out may alias a but not b. */
    RwMatrix k = *a;

    out->right.x = k.right.x * b->right.x + k.up.x * b->right.y + k.at.x * b->right.z;
    out->right.y = k.right.y * b->right.x + k.up.y * b->right.y + k.at.y * b->right.z;
    out->right.z = k.right.z * b->right.x + k.up.z * b->right.y + k.at.z * b->right.z;
    out->up.x = k.right.x * b->up.x + k.up.x * b->up.y + k.at.x * b->up.z;
    out->up.y = k.right.y * b->up.x + k.up.y * b->up.y + k.at.y * b->up.z;
    out->up.z = k.right.z * b->up.x + k.up.z * b->up.y + k.at.z * b->up.z;
    out->at.x = k.up.x * b->at.y + k.at.x * b->at.z + k.right.x * b->at.x;
    out->at.y = k.up.y * b->at.y + k.at.y * b->at.z + k.right.y * b->at.x;
    out->at.z = k.up.z * b->at.y + k.at.z * b->at.z + k.right.z * b->at.x;
    out->pos.x = k.right.x * b->pos.x + k.up.x * b->pos.y + k.at.x * b->pos.z + k.pos.x;
    out->pos.y = k.right.y * b->pos.x + k.up.y * b->pos.y + k.at.y * b->pos.z + k.pos.y;
    out->pos.z = k.right.z * b->pos.x + k.up.z * b->pos.y + k.at.z * b->pos.z + k.pos.z;
}

/* VA 0x00481e90, 103 bytes, [I] row, status draft */
void zmathMatrixTranspose(RwMatrix *dst, const RwMatrix *src)
{
    /* the 3x3 part only; works in place */
    float rx_y = src->right.y, rx_z = src->right.z, up_x = src->up.x;
    float up_z = src->up.z, at_x = src->at.x, at_y = src->at.y;

    if (dst != src) {
        dst->right.x = src->right.x;
        dst->up.y = src->up.y;
        dst->at.z = src->at.z;
    }
    dst->right.y = up_x;
    dst->right.z = at_x;
    dst->up.x = rx_y;
    dst->at.x = rx_z;
    dst->at.y = up_z;
    dst->up.z = at_y;
}

/* VA 0x00481f00, 73 bytes, [I] row, status draft */
RwMatrix *zmathMatrixInvertRigid(RwMatrix *dst, const RwMatrix *src)
{
    /* inverse of a rotation + translation: R^T and -R^T t */
    RwV3d t;

    zmathMatrixTranspose(dst, src);
    t.x = -src->pos.x;
    t.y = -src->pos.y;
    t.z = -src->pos.z;
    zmathTransformVector(&dst->pos, dst, &t);
    return dst;
}

/* ------------------------------------------------------------------ module */

/* VA 0x00481f50, 11 bytes, [I] row, status draft */
int zmathInit(void)
{
    zmathInitTables();
    return 1;
}

/* VA 0x00481f60, 11 bytes, [I] row, status draft */
int zmathShutdown(void)
{
    zmathFreeTables();
    return 1;
}

/* VA 0x00481f70, 10 bytes, [I] row, status draft */
void zmathSeedRandom(uint32_t seed)
{
    g_zmathRandomSeed = seed;
}

/* VA 0x00481f80, 51 bytes, [I] row, status draft */
uint32_t zmathRandom(void)
{
    uint32_t s = g_zmathRandomSeed;
    uint32_t r = ((s >> 16) | (s << 16)) + ((s >> 28) | (s << 4));

    r ^= 0x87654321u;
    g_zmathRandomSeed = (r >> 16) | (r << 16);
    return g_zmathRandomSeed;
}

/* VA 0x00481fc0, 194 bytes, [I] row, status draft */
void zmathMatrixToEuler(int16_t *euler, const RwMatrix *m)
{
    /* inverse of zmathMatrixRotateXYZ / zquatFromEuler (R = Rz Ry Rx); zquatToEuler passes a third
     * argument (its order, always 0) that is not used */
    float cy = (float)sqrt(m->right.x * m->right.x + m->right.y * m->right.y);

    if (cy > 0.001f) {
        euler[0] = zmathAtan2(m->up.z, m->at.z);
        euler[1] = zmathAtan2(-m->right.z, cy);
        euler[2] = zmathAtan2(m->right.y, m->right.x);
    } else {                                    /* gimbal lock: put all of the yaw into X */
        euler[0] = zmathAtan2(-m->at.y, m->up.y);
        euler[1] = zmathAtan2(-m->right.z, cy);
        euler[2] = 0;
    }
}

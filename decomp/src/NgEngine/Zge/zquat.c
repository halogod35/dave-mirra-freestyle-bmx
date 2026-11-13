/*
 * NgEngine/Zge/zquat.c
 * Original: C:\NgEngine\Zge\zquat.c
 * Evidence: [V] path string referenced by zquatFromEuler, zquatToEuler (line 0x208/0x231 "Not coded"),
 * zquatToMatrix, zquatNormalize and zquatInverse (line 0x287 "Division by 0": one inline normalise);
 * the others [I] (link order, between zstring.c's zfx registry and zfile.c).
 * 17 functions, VA 0x00483020-0x00483a5e.
 * Draft. Non-matching C (AD-8). Types and conventions: decomp/include/zmath.h.
 *
 * Quaternions are (x, y, z, w). zquatFromEuler builds q = qz * qy * qx from 16-bit binary angles
 * through the zmath sine table, zquatToMatrix gives the RwMatrix of R(q) (rows right/up/at).
 *
 * Note for the animation code (an open question from the legacy arm table): the rider's motion channels do NOT reach the
 * RenderWare frames through zquatToMatrix. zrwModelUpdate converts each node quaternion with
 * zrwModelQuatToRwMatrix 0x00487E70, which first maps (x, y, z, w) to (x, -y, -z, w), i.e. it
 * builds F R(q) F with F = diag(1, -1, -1) (the engine's Y-down axes -> RenderWare's). See
 * docs/formats/ANIM.md section 3.1.
 */
#include "decomp.h"
#include "zmath.h"

double sqrt(double x);

#define ZQUAT_FILE "C:\\NgEngine\\Zge\\zquat.c"
#define ZMATH_RAD_PER_UNIT  9.58738019e-05f       /* 0x0054DCC4: 2 pi / 65536 */
#define ZMATH_UNITS_PER_RAD 10430.3779f           /* 0x0054DCC8: 65536 / (2 pi) */
#define ZMATH_DEG_PER_UNIT  0.0054931640625f      /* 0x0054D15C: 360 / 65536 */
#define ZMATH_UNITS_PER_DEG 182.044449f           /* 0x0054D01C: 65536 / 360 */

/* VA 0x00483020, 22 bytes, [I] row, status draft */
void zquatIdentity(Zquat *q)
{
    q->x = 0.0f;
    q->y = 0.0f;
    q->z = 0.0f;
    q->w = 1.0f;
}

/* VA 0x00483040, 35 bytes, [I] row, status draft */
void zquatCopy(Zquat *dst, const Zquat *src)
{
    *dst = *src;
}

/* VA 0x00483070, 176 bytes, [I] row, status draft */
void zquatMul(Zquat *out, const Zquat *a, const Zquat *b)
{
    /* Hamilton product a * b; computed into locals, so out may alias a or b */
    Zquat r;

    r.w = b->w * a->w - a->x * b->x - b->y * a->y - b->z * a->z;
    r.x = b->w * a->x + b->x * a->w + b->z * a->y - b->y * a->z;
    r.y = b->y * a->w - b->z * a->x + a->y * b->w + a->z * b->x;
    r.z = b->z * a->w + b->y * a->x - a->y * b->x + a->z * b->w;
    *out = r;
}

/* VA 0x00483120, 73 bytes, [I] row, status draft */
void zquatFromAxisAngle(Zquat *q, const RwV3d *axis, int16_t angle)
{
    int half = angle / 2;
    float s = zmathSin(half);

    q->w = zmathCos(half);
    q->x = s * axis->x;
    q->y = s * axis->y;
    q->z = s * axis->z;
}

/* VA 0x00483170, 153 bytes, [I] row, status draft */
int16_t zquatToAxisAngle(RwV3d *axis, const Zquat *q)
{
    float len;
    int16_t half;

    axis->x = q->x;
    axis->y = q->y;
    axis->z = q->z;
    len = zmathVecLength(axis);
    if (len < 1e-05f) {
        axis->x = 1.0f;
        axis->y = 0.0f;
        axis->z = 0.0f;
        return 0;
    }
    axis->x = q->x / len;
    axis->y = q->y / len;
    axis->z = q->z / len;
    half = zmathAtan2(len, q->w);
    /* 2 * half, by way of degrees (truncated by __ftol) */
    return (int16_t)(int)(half * ZMATH_DEG_PER_UNIT * 2.0f * ZMATH_UNITS_PER_DEG);
}

/* VA 0x00483210, 160 bytes, [I] row, status draft */
void zquatToRotationVector(RwV3d *out, const Zquat *q)
{
    /* unit axis * (half angle / 180 degrees), pointing along the axis of positive rotation */
    int16_t half = zmathAcos(q->w);
    float k = half * ZMATH_DEG_PER_UNIT * (1.0f / 180.0f);

    out->x = q->x;
    out->y = q->y;
    out->z = q->z;
    zmathVecNormalize(out, out);
    if (zmathSin(half) < 0.0f) {
        out->x = -(k * out->x);
        out->y = -(k * out->y);
        out->z = -(k * out->z);
    } else {
        out->x = k * out->x;
        out->y = k * out->y;
        out->z = k * out->z;
    }
}

/* VA 0x004832b0, 357 bytes, [V] direct, status draft */
void zquatFromEuler(Zquat *q, const int16_t *euler, int order)
{
    /* q = qz * qy * qx (x applied first). The half angles go float -> binary angle (truncated by
     * __ftol; the two float constants multiply to 1 - 1.3e-8, so an exact half angle n comes out
     * as n - 1 when n > 0) -> table, so every factor is quantised to 8 units (0.044 degrees). */
    float hx, hy, hz;
    int16_t ax, ay, az;
    float cx, cy, cz, sx, sy, sz;

    if (order != 0)
        zAssertFail("Not coded", ZQUAT_FILE, 0x208, 0);
    hx = euler[0] * ZMATH_RAD_PER_UNIT * 0.5f;
    hy = euler[1] * ZMATH_RAD_PER_UNIT * 0.5f;
    hz = euler[2] * ZMATH_RAD_PER_UNIT * 0.5f;
    ax = (int16_t)(int)(hx * ZMATH_UNITS_PER_RAD);
    ay = (int16_t)(int)(hy * ZMATH_UNITS_PER_RAD);
    az = (int16_t)(int)(hz * ZMATH_UNITS_PER_RAD);
    cx = zmathCos(ax);  cy = zmathCos(ay);  cz = zmathCos(az);
    sx = zmathSin(ax);  sy = zmathSin(ay);  sz = zmathSin(az);

    q->x = sx * cz * cy - cx * sz * sy;
    q->y = cx * cz * sy + sx * sz * cy;
    q->z = cx * sz * cy - sx * cz * sy;
    q->w = sx * sz * sy + cx * cz * cy;
}

/* VA 0x00483420, 76 bytes, [V] direct, status draft */
void zquatToEuler(int16_t *euler, Zquat *q, int order)
{
    RwMatrix m;

    if (order != 0)
        zAssertFail("Not coded", ZQUAT_FILE, 0x231, 0);
    zquatToMatrix(&m, q);                  /* normalises q in place */
    zmathMatrixToEuler(euler, &m);         /* the exe also passes `order` */
}

/* VA 0x00483470, 409 bytes, [V] direct, status draft */
void zquatToMatrix(RwMatrix *m, Zquat *q)
{
    /* Normalises q IN PLACE first (zquatNormalize, inlined), then scales by 2 / |q| where the
     * textbook formula has 2 / |q|^2 (the same for unit quaternions). Writes the 3x3 part only. */
    float n = zquatNormalize(q);
    float s = (n > 0.0f) ? 2.0f / n : 0.0f;
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float xs = x * s, ys = y * s, zs = z * s;
    float wx = w * xs, wy = w * ys, wz = w * zs;
    float xx = x * xs, xy = x * ys, xz = x * zs;
    float yy = y * ys, yz = y * zs, zz = z * zs;

    m->right.x = 1.0f - (zz + yy);
    m->right.y = xy + wz;
    m->right.z = xz - wy;
    m->up.x = xy - wz;
    m->up.y = 1.0f - (zz + xx);
    m->up.z = yz + wx;
    m->at.x = xz + wy;
    m->at.y = yz - wx;
    m->at.z = 1.0f - (yy + xx);
}

/* VA 0x00483610, 48 bytes, [I] row, status draft */
Zquat *zquatFromMatrix(Zquat *q, const RwMatrix *m)
{
    int16_t euler[3];

    zmathMatrixToEuler(euler, m);          /* the exe passes a third argument 0 */
    zquatFromEuler(q, euler, 0);
    return q;
}

/* VA 0x00483640, 37 bytes, [I] row, status draft */
void zquatConjugate(Zquat *out, const Zquat *q)
{
    out->x = -q->x;
    out->y = -q->y;
    out->z = -q->z;
    out->w = q->w;
}

/* VA 0x00483670, 134 bytes, [V] direct, status draft */
float zquatNormalize(Zquat *q)
{
    float n = (float)sqrt(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);

    if (n <= 1e-05f)
        zAssertFail("Division by 0", ZQUAT_FILE, 0x287, 0);
    q->x /= n;
    q->y /= n;
    q->z /= n;
    q->w /= n;
    return n;
}

/* VA 0x00483700, 37 bytes, [I] row, status draft */
float zquatDot(const Zquat *a, const Zquat *b)
{
    return a->w * b->w + a->z * b->z + a->y * b->y + a->x * b->x;
}

/* VA 0x00483730, 198 bytes, [V] direct, status draft */
void zquatInverse(Zquat *out, Zquat *q)
{
    /* normalises q in place (inlined zquatNormalize), then conj(q) / |q_old|: right for unit
     * quaternions only (the inverse of q_unit is conj(q_unit)) */
    float n = zquatNormalize(q);

    out->x = -q->x / n;
    out->y = -q->y / n;
    out->z = -q->z / n;
    out->w = q->w / n;
}

/* VA 0x00483800, 319 bytes, [I] row, status draft */
void zquatSlerp(Zquat *out, const Zquat *a, const Zquat *b, float t)
{
    float d = a->x * b->x + a->z * b->z + a->y * b->y + b->w * a->w;
    int flip = 0;
    float s0, s1;

    if (d < 0.0f) {                        /* take the short way round */
        d = -d;
        flip = 1;
    }
    if (1.0f - d < 1e-05f) {               /* nearly equal: linear */
        s0 = 1.0f - t;
        s1 = t;
    } else {
        float theta = zmathAcos(d) * ZMATH_RAD_PER_UNIT;
        float st = zmathSin((int)(theta * ZMATH_UNITS_PER_RAD));

        s0 = zmathSin((int)((theta - t * theta) * ZMATH_UNITS_PER_RAD)) / st;
        s1 = zmathSin((int)(t * theta * ZMATH_UNITS_PER_RAD)) / st;
    }
    if (flip)
        s1 = -s1;
    out->x = s0 * a->x + s1 * b->x;
    out->y = s0 * a->y + s1 * b->y;
    out->z = s0 * a->z + s1 * b->z;
    out->w = s1 * b->w + s0 * a->w;
}

/* VA 0x00483940, 237 bytes, [I] row, status draft */
void zquatAddClosest(Zquat *out, const Zquat *a, const Zquat *b)
{
    /* a + b, or a - b when b is in the other hemisphere (dot <= 0) and neither is ~zero: the
     * accumulator of the animation blender (zanimEvaluate, zanimApplyKeys) */
    float d = b->w * a->w + a->z * b->z + a->y * b->y + a->x * b->x;

    if (d <= 0.0f
        && a->x * a->x + a->y * a->y + a->z * a->z + a->w * a->w >= 1e-05f
        && b->x * b->x + b->y * b->y + b->z * b->z + b->w * b->w >= 1e-05f) {
        out->x = a->x - b->x;
        out->y = a->y - b->y;
        out->z = a->z - b->z;
        out->w = a->w - b->w;
        return;
    }
    out->x = a->x + b->x;
    out->y = a->y + b->y;
    out->z = a->z + b->z;
    out->w = b->w + a->w;
}

/* VA 0x00483a30, 47 bytes, [I] row, status draft */
void zquatScale(Zquat *out, float s, const Zquat *q)
{
    out->x = s * q->x;
    out->y = s * q->y;
    out->z = s * q->z;
    out->w = s * q->w;
}

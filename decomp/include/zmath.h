/*
 * zmath.h - NgEngine maths: trig lookup tables, vectors, matrices (NgEngine/Zge/zmath.c) and
 * quaternions (NgEngine/Zge/zquat.c) (DM-045).
 *
 * Conventions [V] (from the code):
 *  - Angles are 16-bit binary angles: 65536 = 360 degrees (0x4000 = 90, 0x8000 = 180). The sine
 *    table has 8192 entries per turn, so zmathSin/zmathCos use (angle & 0xFFFF) >> 3: a resolution
 *    of 8 units (0.044 degrees). Inverse functions (zmathAsin/Acos/Atan/Atan2) return angles from
 *    two 8193-entry int16 tables indexed by trunc(4096 * x).
 *  - Vectors are RwV3d; matrices are RwMatrix (RenderWare layout: rows right/up/at/pos, a point is
 *    transformed as v' = v.x * right + v.y * up + v.z * at + pos). In column-vector terms the
 *    rows are the columns of the rotation, so the standard matrix R maps to right = R e_x.
 *  - Quaternions are (x, y, z, w); zquatMul is the Hamilton product a * b; zquatFromEuler builds
 *    q = qz * qy * qx (x applied first); zquatToMatrix gives the RwMatrix whose rotation is R(q).
 */
#ifndef ZMATH_H
#define ZMATH_H
#include "dmtypes.h"
#include "rwtypes.h"

typedef struct Zquat {
    float x;   /* +0x00 */
    float y;   /* +0x04 */
    float z;   /* +0x08 */
    float w;   /* +0x0C */
} Zquat;       /* 0x10 */

/* GHIDRA-SKIP-BEGIN */
typedef char zmath_h_size_check[(sizeof(Zquat) == 0x10) ? 1 : -1];
/* GHIDRA-SKIP-END */

#define ZMATH_ANGLE_90   0x4000
#define ZMATH_ANGLE_180  0x8000

/* zmath.c (VA 0x00480D20-0x00482082) */
int      zmathInitTables(void);                                                  /* 0x00480D20 */
int      zmathFreeTables(void);                                                  /* 0x00480DF0 */
float    zmathFloor(float x);                                                    /* 0x00480E00 */
float    zmathCeil(float x);                                                     /* 0x00480E20 */
float    zmathSin(int angle);                                                    /* 0x00480E40 */
int16_t  zmathAsin(float x);                                                     /* 0x00480E60 */
float    zmathCos(int angle);                                                    /* 0x00480EB0 */
int16_t  zmathAcos(float x);                                                     /* 0x00480ED0 */
float    zmathTan(int angle);                                                    /* 0x00480F30 */
int16_t  zmathAtan(float x);                                                     /* 0x00480F70 */
int16_t  zmathAtan2(float y, float x);                                           /* 0x00480FF0 */
void     zmathVecZero(RwV3d *v);                                                 /* 0x00481120 */
void     zmathVecAdd(RwV3d *out, const RwV3d *a, const RwV3d *b);                /* 0x00481130 */
void     zmathVecSub(RwV3d *out, const RwV3d *a, const RwV3d *b);                /* 0x00481160 */
void     zmathVecMul(RwV3d *out, const RwV3d *a, const RwV3d *b);                /* 0x00481190 */
float    zmathVecNormalize(RwV3d *out, const RwV3d *v);                          /* 0x004811C0 */
void     zmathVecScale(RwV3d *out, const RwV3d *v, float s);                     /* 0x00481210 */
void     zmathVecScaleXYZ(RwV3d *out, const RwV3d *v, float sx, float sy, float sz); /* 0x00481240 */
void     zmathVecMidpoint(RwV3d *out, const RwV3d *a, const RwV3d *b);           /* 0x00481270 */
float    zmathVecLengthSq(const RwV3d *v);                                       /* 0x004812C0 */
void     zmathVecNegate(RwV3d *out, const RwV3d *v);                             /* 0x004812F0 */
float    zmathVecLength(const RwV3d *v);                                         /* 0x00481310 */
void     zmathVecRotateX(RwV3d *out, const RwV3d *v, int angle);                 /* 0x00481340 */
void     zmathVecRotateY(RwV3d *out, const RwV3d *v, int angle);                 /* 0x00481390 */
void     zmathVecRotateZ(RwV3d *out, const RwV3d *v, int angle);                 /* 0x004813E0 */
void     zmathVecRotateXYZ(RwV3d *out, const RwV3d *v, const uint16_t *euler);   /* 0x00481430 */
float    zmathVecDot(const RwV3d *a, const RwV3d *b);                            /* 0x00481540 */
void     zmathVecCross(RwV3d *out, const RwV3d *a, const RwV3d *b);              /* 0x00481560 */
void     zmathTriangleNormal(RwV3d *out, const RwV3d *p0, const RwV3d *p1, const RwV3d *p2); /* 0x004815B0 */
void     zmathTransformPoint(RwV3d *out, const RwMatrix *m, const RwV3d *v);     /* 0x00481640 */
float   *zmathTransformPoints(float *out, const RwMatrix *m, const float *in, int count); /* 0x004816A0 */
void     zmathTransformVector(RwV3d *out, const RwMatrix *m, const RwV3d *v);    /* 0x00481780 */
void     zmathVecLerp(RwV3d *out, const RwV3d *a, const RwV3d *b, float t);      /* 0x004817E0 */
void     zmathVecAddScalar(RwV3d *out, const RwV3d *v, float s);                 /* 0x00481820 */
void     zmathVecSubScalar(RwV3d *out, const RwV3d *v, float s);                 /* 0x00481850 */
void     zmathMatrixIdentity(RwMatrix *m);                                       /* 0x00481880 */
void     zmathMatrixClearTranslation(RwMatrix *m);                               /* 0x004818B0 */
void     zmathMatrixIdentityRotation(RwMatrix *m);                               /* 0x004818C0 */
RwMatrix *zmathMatrixTranslate(RwMatrix *dst, const RwMatrix *src, const RwV3d *t); /* 0x004818F0 */
RwMatrix *zmathMatrixRotateX(RwMatrix *dst, const RwMatrix *src, int angle);     /* 0x00481930 */
void     zmathMatrixRotateY(RwMatrix *dst, const RwMatrix *src, int angle);      /* 0x00481A00 */
void     zmathMatrixRotateZ(RwMatrix *dst, const RwMatrix *src, int angle);      /* 0x00481AD0 */
RwMatrix *zmathMatrixRotateXYZ(RwMatrix *dst, const RwMatrix *src, const int16_t *euler); /* 0x00481BA0 */
RwMatrix *zmathMatrixScale(RwMatrix *dst, const RwMatrix *src, const RwV3d *s);  /* 0x00481BE0 */
RwMatrix *zmathMatrixScaleUniform(RwMatrix *dst, RwMatrix *src, float s);        /* 0x00481C30 */
RwMatrix *zmathMatrixCopy(RwMatrix *dst, const RwMatrix *src);                   /* 0x00481C60 */
void     zmathMatrixCopyTranslation(RwMatrix *dst, const RwMatrix *src);         /* 0x00481C90 */
void     zmathMatrixCopyRotation(RwMatrix *dst, const RwMatrix *src);            /* 0x00481CB0 */
void     zmathMatrixMultiply(RwMatrix *out, const RwMatrix *a, const RwMatrix *b); /* 0x00481CF0 */
void     zmathMatrixTranspose(RwMatrix *dst, const RwMatrix *src);               /* 0x00481E90 */
RwMatrix *zmathMatrixInvertRigid(RwMatrix *dst, const RwMatrix *src);            /* 0x00481F00 */
int      zmathInit(void);                                                        /* 0x00481F50 */
int      zmathShutdown(void);                                                    /* 0x00481F60 */
void     zmathSeedRandom(uint32_t seed);                                         /* 0x00481F70 */
uint32_t zmathRandom(void);                                                      /* 0x00481F80 */
void     zmathMatrixToEuler(int16_t *euler, const RwMatrix *m);                  /* 0x00481FC0 */

/* zquat.c (VA 0x00483020-0x00483A5E) */
void     zquatIdentity(Zquat *q);                                                /* 0x00483020 */
void     zquatCopy(Zquat *dst, const Zquat *src);                                /* 0x00483040 */
void     zquatMul(Zquat *out, const Zquat *a, const Zquat *b);                   /* 0x00483070 */
void     zquatFromAxisAngle(Zquat *q, const RwV3d *axis, int16_t angle);         /* 0x00483120 */
int16_t  zquatToAxisAngle(RwV3d *axis, const Zquat *q);                          /* 0x00483170 */
void     zquatToRotationVector(RwV3d *out, const Zquat *q);                      /* 0x00483210 */
void     zquatFromEuler(Zquat *q, const int16_t *euler, int order);              /* 0x004832B0 */
void     zquatToEuler(int16_t *euler, Zquat *q, int order);                      /* 0x00483420 */
void     zquatToMatrix(RwMatrix *m, Zquat *q);                                   /* 0x00483470 */
Zquat   *zquatFromMatrix(Zquat *q, const RwMatrix *m);                           /* 0x00483610 */
void     zquatConjugate(Zquat *out, const Zquat *q);                             /* 0x00483640 */
float    zquatNormalize(Zquat *q);                                               /* 0x00483670 */
float    zquatDot(const Zquat *a, const Zquat *b);                               /* 0x00483700 */
void     zquatInverse(Zquat *out, Zquat *q);                                     /* 0x00483730 */
void     zquatSlerp(Zquat *out, const Zquat *a, const Zquat *b, float t);        /* 0x00483800 */
void     zquatAddClosest(Zquat *out, const Zquat *a, const Zquat *b);            /* 0x00483940 */
void     zquatScale(Zquat *out, float s, const Zquat *q);                        /* 0x00483A30 */

#endif /* ZMATH_H */

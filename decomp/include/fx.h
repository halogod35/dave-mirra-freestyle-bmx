/*
 * fx.h - game particle effects (Bmx/Src/com/action/fx.c) (DM-052).
 *
 * On the PC the effects go through the zfx registry (zstring.h: zfxRegisterSpawner /
 * zfxCallSpawner): NgIFfx's init (0x0045F780) registers the particle types "lightning", "spark",
 * "shard", "splash", "dust", "poster", "ripple", the affectors "gravity" / "floorcoll" and one
 * spawner function per name (+ "skid"). fx.c fills a parameter block and calls the spawner by
 * name. Positions are relative to the fx system's frame origin (zfxGetSystem()->frame + 8).
 *
 * The other half of fx.c is the PlayStation path (zfxCreate with an effect script, the
 * zfxRegisterFuncTable callbacks): on the PC zfxCreate (NgIFfx.c) only prints "Unsupported effect"
 * and zfxRegisterFuncTable / zfxDestroyAll are MissingZGE.c stubs, so that code does nothing.
 * Offsets [V] (fx.c's stores); names and meanings [I]. Description: docs/re/props-replay-fx.md.
 */
#ifndef FX_H
#define FX_H
#include "dmtypes.h"
#include "rwtypes.h"

/* "spark" and "splash" spawner parameters (0x34) [V layout] */
typedef struct FxParticle {
    RwV3d    pos;        /* +0x00 relative to the fx system origin */
    uint32_t pad_0c;
    RwV3d    vel;        /* +0x10 per frame */
    uint32_t pad_1c;
    float    size;       /* +0x20 spark 2, splash 20 */
    float    size2;      /* +0x24 spark 2, splash 4 (end size / growth [I]) */
    uint32_t lifeMs;     /* +0x28 spark 2000, splash 1000 */
    RwRGBA   colour;     /* +0x2C (255, 255, 255, 64) */
    uint32_t flags;      /* +0x30 0 */
} FxParticle;            /* 0x34 */

/* "dust" spawner parameters (0x2C) */
typedef struct FxDust {
    RwV3d    pos;        /* +0x00 */
    uint32_t pad_0c;
    RwV3d    vel;        /* +0x10 */
    uint32_t pad_1c;
    float    size;       /* +0x20 20 */
    uint32_t lifeMs;     /* +0x24 1000 */
    uint32_t flags;      /* +0x28 0 */
} FxDust;                /* 0x2C */

/* "shard" spawner parameters (0x40) */
typedef struct FxShard {
    RwV3d    pos;        /* +0x00 */
    uint32_t pad_0c;
    RwV3d    vel;        /* +0x10 */
    uint32_t pad_1c;
    RwV3d    spin;       /* +0x20 0..999 per axis */
    uint32_t pad_2c;
    float    size;       /* +0x30 5..84 */
    uint32_t lifeMs;     /* +0x34 1000..2999 */
    RwRGBA   colour;     /* +0x38 (0x80, 0x80, 0xD0, 0x70) glass blue */
    uint32_t flags;      /* +0x3C 0 */
} FxShard;               /* 0x40 */

/* "skid" spawner parameters (0x4C): a quad on the ground */
typedef struct FxSkidCorner {
    RwV3d    p;
    uint32_t pad;
} FxSkidCorner;          /* 0x10 */
typedef struct FxSkidQuad {
    FxSkidCorner corner[4]; /* +0x00 */
    uint32_t lifeMs;     /* +0x40 5000 */
    RwRGBA   colour;     /* +0x44 (0x7F, 0x7F, 0x7F, 0x7F) */
    uint32_t flags;      /* +0x48 0 */
} FxSkidQuad;            /* 0x4C */

/* "ripple" spawner parameters (0x24) */
typedef struct FxRipple {
    RwV3d    pos;        /* +0x00 */
    uint32_t pad_0c;
    uint32_t lifeMs;     /* +0x10 1000 */
    float    size;       /* +0x14 30 */
    float    size2;      /* +0x18 250 */
    RwRGBA   colour;     /* +0x1C white */
    uint32_t flags;      /* +0x20 0 */
} FxRipple;              /* 0x24 */

/* zfxCreate parameter block of the PlayStation path (0x20) */
typedef struct FxPsxEffect {
    uint16_t kind;       /* +0x00 */
    uint16_t arg;        /* +0x02 0xFF */
    uint32_t target;     /* +0x04 */
    RwV3d    pos;        /* +0x08 */
    uint32_t unk_14;
    const void *script;  /* +0x18 g_fxPsxScripts[type] */
    uint32_t unk_1c;
} FxPsxEffect;           /* 0x20 */

/* zfxCreate parameter block of fxBodyBurst (0x40), built on the stack */
typedef struct FxPsxBurst {
    uint16_t kind;       /* +0x00 FxBodyPoint.kind */
    uint16_t arg;        /* +0x02 0xFF */
    uint32_t target;     /* +0x04 0 */
    RwV3d    pos;        /* +0x08 */
    uint32_t unk_14;
    void    *joint;      /* +0x18 joints[FxBodyPoint.joint] */
    RwV3d    vel;        /* +0x1C base velocity + random -16..15 per axis */
    uint32_t unk_28;
    uint32_t zero_2c[3]; /* +0x2C 0 */
    uint32_t unk_38;
    uint32_t zero_3c;    /* +0x3C 0 */
} FxPsxBurst;            /* 0x40 */

/* one entry of the 42-point body burst table (0x005547A0.., 8 bytes) [V layout] */
typedef struct FxBodyPoint {
    uint16_t joint;      /* +0x00 index into the joint-matrix array */
    uint16_t kind;       /* +0x02 */
    int16_t  x;          /* +0x04 offset / 2 (negated; z = -40) */
    int16_t  y;          /* +0x06 */
} FxBodyPoint;           /* 0x08 */

/* GHIDRA-SKIP-BEGIN */
typedef char fx_h_size_check[(sizeof(FxParticle) == 0x34 && sizeof(FxDust) == 0x2C &&
                              sizeof(FxShard) == 0x40 && sizeof(FxSkidCorner) == 0x10 &&
                              sizeof(FxSkidQuad) == 0x4C && sizeof(FxRipple) == 0x24 &&
                              sizeof(FxPsxEffect) == 0x20 && sizeof(FxPsxBurst) == 0x40 &&
                              sizeof(FxBodyPoint) == 0x08) ? 1 : -1];

struct BmxRider;
void  fxPsxFuncNop1(void);                                            /* 0x00408960 */
void  fxPsxFuncNop2(void);                                            /* 0x00408980 */
void  fxSetRider(struct BmxRider *r);                                 /* 0x004089A0 */
int   fxInit(void);                                                   /* 0x004089C0 */
void  fxShutdown(void);                                               /* 0x00408A10 */
void  fxDestroyAll(void);                                             /* 0x00408A40 */
void  fxDestroy(uint32_t which);                                      /* 0x00408A60 */
void  fxCollectEffect(void);                                          /* 0x00408A80 */
void  fxSpark(void *actor, int kind, const RwV3d *pos, uint32_t seed, float scale); /* 0x00408AA0 */
void  fxPuff(const RwV3d *pos);                                       /* 0x00408C50 */
void  fxDust(const RwV3d *pos, const RwV3d *vel);                     /* 0x00408C90 */
void  fxBodyBurst(void **joints, uint32_t effect, const RwV3d *pos, int yaw, const RwV3d *vel); /* 0x00408DB0 */
void  fxShards(const RwV3d *pos, const RwV3d *vel);                   /* 0x00408FD0 */
void  fxSkid(const RwV3d *from, const RwV3d *to, int kind, const RwV3d *normal); /* 0x00409250 */
void  fxPsxCreateAt(uint32_t target, uint16_t arg, const RwV3d *pos, int unused, uint32_t type); /* 0x004095A0 */
void  fxPsxCreate(uint32_t type, const RwV3d *pos, int arg);          /* 0x00409640 */
void  fxWaterSplash(void *actor, int kind, const RwV3d *offset);      /* 0x004096D0 */
void  fxPsxFlash(void *model, int alpha, const void *colour, int frames, float a, float b); /* 0x00409860 */
void  fxPsxFuncCreate(void *obj, uint32_t type);                      /* 0x00409880 */
int   fxPsxOnEvent(void *obj, int event);                             /* 0x004098B0 */
int   fxPsxOnSpawn(uint8_t *records, int count);                      /* 0x004098F0 */
/* GHIDRA-SKIP-END */

#endif /* FX_H */

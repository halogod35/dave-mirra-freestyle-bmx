/*
 * zaxis/Bmx/Src/com/action/fx.c
 * Original: C:\zaxis\Bmx\Src\com\action\fx.c
 * Evidence: [V] path string 0x00554C2C, referenced by the "type out of range" asserts of
 *           fxPsxCreateAt (line 0x42F) and fxPsxCreate (line 0x44C); [I] for the rest: the
 *           functions between contrler.c (ends 0x004088E6) and game.c (gameInit 0x004099C0) all use
 *           fx.c's data (0x00554718-0x00554C64: the PSX callback table, effect blocks, the spawner
 *           names "spark", "dust", "shard", "skid", "splash", "ripple" and fx.c's own path string),
 *           and line 0x42F needs about a thousand lines before fxPsxCreateAt.
 * 21 functions, 3074 bytes, VA 0x00408960-0x00409980.
 * DM-052 (draft). Non-matching C (AD-8). Types: fx.h. Description: docs/re/props-replay-fx.md.
 *
 * DM-052 took in the contrler/fx and fx/game gaps (11 functions) and created eight functions Ghidra
 * had missed (decomp/symbols/symbols.csv rows): fxPsxFuncNop1/2 and fxPsxFuncCreate (the zfxRegisterFuncTable
 * table 0x00554718), fxDestroyAll / fxDestroy / fxPuff (only ILT thunks, never called),
 * fxPsxOnEvent / fxPsxOnSpawn (hooks fxInit stores in 0x005F34B4 / 0x005F34B8).
 *
 * Two halves: the particle effects the PC build shows (sparks, dust, glass shards, skid marks,
 * water) go through the zfx spawner registry; the PlayStation effect scripts (zfxCreate) are dead on
 * the PC, where zfxCreate only prints "zfxCreate: Unsupported effect".
 */

#include <math.h>

#include "decomp.h"

#include "zmath.h"

#include "fx.h"

#define FX_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\fx.c"
#define FX_PSX_TYPES 6

/* ------------------------------------------------------------------ other modules (current names) */
int    zfxCreate(int effect, void *params, int arg);               /* 0x0045FCB0 NgIFfx.c: PC: prints only */
void  *zfxGetSystem(void *actor, int kind);                        /* 0x0045F480 -> the fx system (args unused) */
void   zfxInitTypes(void);                                         /* 0x0045F780 registers types / spawners */
void   zfxShutdownTypes(void);                                     /* 0x0045FC10 */
void   zfxFlash(const void *flash);                                /* 0x0045FDC0 screen flash */
void   zfxDestroyAll(uint32_t which);                        /* "zfxDestroyAll" (stub) */
int    zfxRegisterFuncTable(void *const *funcs, int count);         /* "zfxRegisterFuncTable" (stub) */

extern int (*g_zfxEventHook)(void *obj, int event);                /* 0x005F34B4 */
extern int (*g_zfxSpawnHook)(uint8_t *records, int count);         /* 0x005F34B8 */

/* ------------------------------------------------------------------ globals (this file) */
uint32_t   g_fxEnabled;                 /* 0x0056B6C4 */
uint32_t   g_fxCount;                   /* 0x0056B6C8 0 (fxInit), never read */
BmxRider  *g_fxRider;                   /* 0x0056B6CC rider whose actor anchors dust / skids (physics.c) */
RwV3d      g_fxSkidOrigin;              /* 0x0056B6D0 (0, 0, 0) */
void      *const g_fxPsxFuncs[3] = {    /* 0x00554718 zfxRegisterFuncTable */
    (void *)fxPsxFuncCreate, (void *)fxPsxFuncNop1, (void *)fxPsxFuncNop2
};
/* initialised data of the exe, not reproduced here (tests/test_decomp_dm052.py reads them back): */
extern uint8_t      g_fxCollectFlash[20];   /* 0x00554724 zfxFlash parameters (00 FB 00 04 00 FC 00 03 ...) */
extern uint8_t      g_fxPuffEffect[32];     /* 0x00554738 zfxCreate block, position at +0x0C */
extern const FxBodyPoint g_fxBodyPoints[42];/* 0x00554798 */
extern FxSkidCorner g_fxSkidCorners[4];     /* 0x00554964 last skid quad ((0, -100, 0), (0, 100, 0), ...) */
extern const void  *g_fxPsxScripts[FX_PSX_TYPES]; /* 0x00554A9C PSX effect scripts */
extern FxPsxEffect  g_fxPsxAt;              /* 0x00554AB8 */
extern FxPsxEffect  g_fxPsxPlain;           /* 0x00554AD8 */

/* ------------------------------------------------------------------ helpers */
/* the fx system's origin (spawner positions are relative to it) */
static const float *fxOrigin(void *sys)
{
    return (const float *)(*(uint8_t **)((uint8_t *)sys + 0x30) + 8);
}

static void fxRelative(RwV3d *out, const RwV3d *p, const float *origin)
{
    out->x = p->x - origin[0];
    out->y = p->y - origin[1];
    out->z = p->z - origin[2];
}

static void setRGBA(RwRGBA *c, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    c->red = r; c->green = g; c->blue = b; c->alpha = a;
}

/* ------------------------------------------------------------------ functions */

/* VA 0x00408960, 1 bytes, [I] row, status draft */
void fxPsxFuncNop1(void)
{
    /* zfxRegisterFuncTable entry 1: nothing */
}

/* VA 0x00408980, 1 bytes, [I] row, status draft */
void fxPsxFuncNop2(void)
{
    /* zfxRegisterFuncTable entry 2: nothing */
}

/* VA 0x004089a0, 10 bytes, [I] row, status draft */
void fxSetRider(BmxRider *r)
{
    /* physics.c, before moving a rider: dust and skid marks are spawned for this rider */
    g_fxRider = r;
}

/* VA 0x004089c0, 63 bytes, [I] row, status draft */
int fxInit(void)
{
    g_zfxEventHook = fxPsxOnEvent;
    g_zfxSpawnHook = fxPsxOnSpawn;
    zfxRegisterFuncTable(g_fxPsxFuncs, 3);
    g_fxCount = 0;
    g_fxEnabled = 1;
    zfxInitTypes();
    return 0;
}

/* VA 0x00408a10, 25 bytes, [I] row, status draft */
void fxShutdown(void)
{
    zfxDestroyAll(0);
    g_fxEnabled = 0;
    zfxShutdownTypes();
}

/* VA 0x00408a40, 9 bytes, [I] row, status draft */
void fxDestroyAll(void)
{
    /* never called */
    zfxDestroyAll(0);
}

/* VA 0x00408a60, 12 bytes, [I] row, status draft */
void fxDestroy(uint32_t which)
{
    /* never called */
    zfxDestroyAll(which);
}

/* VA 0x00408a80, 12 bytes, [I] row, status draft */
void fxCollectEffect(void)
{
    /* a goal prop collected / a traffic hit / riders colliding: a screen flash */
    zfxFlash(g_fxCollectFlash);
}

/* VA 0x00408aa0, 329 bytes, [I] row, status draft */
void fxSpark(void *actor, int kind, const RwV3d *pos, uint32_t seed, float scale)
{
    /* one spark at pos with a random velocity (grinding metal, physics.c); seed / scale unused */
    FxParticle p;
    void *sys;
    const float *o;

    (void)seed; (void)scale;
    zstringMemSet(&p, 0, sizeof(p));
    p.vel.y = -10.0f;
    p.size = 2.0f;
    p.size2 = 2.0f;
    p.lifeMs = 2000;
    setRGBA(&p.colour, 0xFF, 0xFF, 0xFF, 0x40);
    sys = zfxGetSystem(actor, kind);
    if (sys == NULL)
        return;
    o = fxOrigin(sys);
    fxRelative(&p.pos, pos, o);
    p.vel.x = (int16_t)(int)((double)(zmathRandom() & 0xFF) - 128.0) * 0.03125f;   /* -4 .. 4 */
    p.vel.y = (int16_t)(zmathRandom() & 0x7F) * -0.015625f;                          /* up to 2 up */
    p.vel.z = (int16_t)(int)((double)(zmathRandom() & 0xFF) - 128.0) * 0.03125f;
    zfxCallSpawner("spark", sys, &p);
}

/* VA 0x00408c50, 47 bytes, [I] row, status draft */
void fxPuff(const RwV3d *pos)
{
    /* never called: PSX effect 1 at pos */
    *(RwV3d *)(g_fxPuffEffect + 0x0C) = *pos;
    zfxCreate(1, g_fxPuffEffect, 0);
}

/* VA 0x00408c90, 224 bytes, [I] row, status draft */
void fxDust(const RwV3d *pos, const RwV3d *vel)
{
    /* dirt kicked up by the wheels / on landing (physics.c), half the given velocity */
    FxDust d;
    void *sys;

    zstringMemSet(&d, 0, sizeof(d));
    d.vel.y = -10.0f;
    d.size = 20.0f;
    d.lifeMs = 1000;
    sys = zfxGetSystem(g_fxRider->riderActor, 0);
    if (sys == NULL)
        return;
    fxRelative(&d.pos, pos, fxOrigin(sys));
    d.pos.y -= 20.0f;
    d.vel.x = vel->x * 0.5f;
    d.vel.y = vel->y * 0.5f;
    d.vel.z = vel->z * 0.5f;
    zfxCallSpawner("dust", sys, &d);
}

/* VA 0x00408db0, 425 bytes, [I] row, status draft */
void fxBodyBurst(void **joints, uint32_t effect, const RwV3d *pos, int yaw, const RwV3d *vel)
{
    /* never called: a PSX effect at each of the 42 body points (g_fxBodyPoints, last first) */
    FxPsxBurst b;
    int i;

    zstringMemSet(&b, 0, sizeof(b));
    b.arg = 0xFF;
    for (i = 41; i >= 0; i--) {
        const FxBodyPoint *pt = &g_fxBodyPoints[i];
        uint32_t rnd;
        RwV3d jitter;

        b.joint = joints[pt->joint];
        b.kind = pt->kind;
        b.pos.x = -(float)(int16_t)(pt->x * 2);
        b.pos.y = -(float)(int16_t)(pt->y * 2);
        b.pos.z = -40.0f;
        zmathVecRotateY(&b.pos, &b.pos, -yaw);
        zmathVecAdd(&b.pos, &b.pos, pos);
        rnd = zmathRandom();
        jitter.x = (float)(int16_t)((rnd & 0x1F) - 0x10);
        jitter.y = (float)(int16_t)(((int32_t)rnd >> 4 & 0x1F) - 0x10);
        jitter.z = (float)(int16_t)((rnd >> 8 & 0x1F) - 0x10);
        zmathVecAdd(&b.vel, &jitter, vel);
        zfxCreate((int)effect, &b, 0);
    }
}

/* VA 0x00408fd0, 500 bytes, [I] row, status draft */
void fxShards(const RwV3d *pos, const RwV3d *vel)
{
    /* 32 glass shards around pos (bmxpropHitTrigger: a window / fence broken), flying with vel */
    FxShard s;
    void *sys = zfxGetSystem(NULL, 0);
    const float *o;
    int n;

    if (sys == NULL)
        return;
    o = fxOrigin(sys);
    s.flags = 0;
    setRGBA(&s.colour, 0x80, 0x80, 0xD0, 0x70);
    for (n = 32; n != 0; n--) {
        s.pos.x = pos->x - o[0] + (int16_t)(zmathRandom() % 600 - 300);
        s.pos.y = pos->y - o[1] + (int16_t)(zmathRandom() % 600 - 300);
        s.pos.z = pos->z - o[2] + (int16_t)(zmathRandom() % 600 - 300);
        s.vel.x = vel->x - (int16_t)(zmathRandom() & 0x1F) * 0.005f;
        s.vel.y = vel->y - (int16_t)(zmathRandom() & 0x1F) * 0.005f;
        s.vel.z = vel->z - (int16_t)(zmathRandom() & 0x1F) * 0.005f;
        s.spin.x = (int16_t)(zmathRandom() % 1000);
        s.spin.y = (int16_t)(zmathRandom() % 1000);
        s.spin.z = (int16_t)(zmathRandom() % 1000);
        s.size = (int16_t)(zmathRandom() % 80 + 5);
        s.lifeMs = zmathRandom() % 2000 + 1000;
        zfxCallSpawner("shard", sys, &s);
    }
}

/* VA 0x00409250, 664 bytes, [I] row, status draft */
void fxSkid(const RwV3d *from, const RwV3d *to, int kind, const RwV3d *normal)
{
    /* a skid mark: a quad from `from` to `to` on the surface with that normal (physics.c: braking,
     * lock-on to a wall); 4 units long at least */
    RwV3d dir, n, side;
    FxSkidQuad q;
    void *sys;
    const float *o;
    int c;

    (void)kind;
    dir.x = from->x - to->x;
    dir.y = from->y - to->y;
    dir.z = from->z - to->z;
    if (fabsf(dir.x) + fabsf(dir.z) + fabsf(dir.y) < 4.0f)
        return;
    zmathVecNormalize(&dir, &dir);
    n = *normal;
    zmathTriangleNormal(&side, &g_fxSkidOrigin, &dir, &n);   /* across the skid */
    g_fxSkidCorners[0].p.x = side.x + from->x; g_fxSkidCorners[0].p.y = side.y + from->y;
    g_fxSkidCorners[0].p.z = side.z + from->z;
    g_fxSkidCorners[1].p.x = from->x - side.x; g_fxSkidCorners[1].p.y = from->y - side.y;
    g_fxSkidCorners[1].p.z = from->z - side.z;
    g_fxSkidCorners[2].p.x = to->x - side.x;   g_fxSkidCorners[2].p.y = to->y - side.y;
    g_fxSkidCorners[2].p.z = to->z - side.z;
    g_fxSkidCorners[3].p.x = side.x + to->x;   g_fxSkidCorners[3].p.y = side.y + to->y;
    g_fxSkidCorners[3].p.z = side.z + to->z;
    sys = zfxGetSystem(g_fxRider->riderActor, 0);
    if (sys == NULL)
        return;
    o = fxOrigin(sys);
    for (c = 0; c < 4; c++) {                                /* lifted half a unit off the surface */
        q.corner[c].p.x = n.x * 0.5f + g_fxSkidCorners[c].p.x - o[0];
        q.corner[c].p.y = n.y * 0.5f + g_fxSkidCorners[c].p.y - o[1];
        q.corner[c].p.z = n.z * 0.5f + g_fxSkidCorners[c].p.z - o[2];
    }
    q.flags = 0;
    setRGBA(&q.colour, 0x7F, 0x7F, 0x7F, 0x7F);
    q.lifeMs = 5000;
    zfxCallSpawner("skid", sys, &q);
}

/* VA 0x004095a0, 117 bytes, [V] direct, status draft */
void fxPsxCreateAt(uint32_t target, uint16_t arg, const RwV3d *pos, int unused, uint32_t type)
{
    (void)unused;
    if (type > FX_PSX_TYPES - 1)
        zAssertFail("type out of range", FX_SRC, 0x42F, NULL);
    g_fxPsxAt.script = g_fxPsxScripts[type];
    g_fxPsxAt.pos = *pos;
    g_fxPsxAt.target = target;
    g_fxPsxAt.arg = arg;
    zfxCreate(7, &g_fxPsxAt, 0);
}

/* VA 0x00409640, 98 bytes, [V] direct, status draft */
void fxPsxCreate(uint32_t type, const RwV3d *pos, int arg)
{
    if (type > FX_PSX_TYPES - 1)
        zAssertFail("type out of range", FX_SRC, 0x44C, NULL);
    g_fxPsxPlain.script = g_fxPsxScripts[type];
    g_fxPsxPlain.pos = *pos;
    zfxCreate(7, &g_fxPsxPlain, arg);
}

/* VA 0x004096d0, 312 bytes, [I] row, status draft */
void fxWaterSplash(void *actor, int kind, const RwV3d *offset)
{
    /* rider.c, crashing into water: a ripple and a splash at the actor + offset */
    FxRipple ripple;
    FxParticle splash;
    void *sys;
    const float *a = (const float *)((uint8_t *)actor + 0x0C);  /* the actor's position */

    (void)kind;
    zstringMemSet(&splash, 0, sizeof(splash));
    sys = zfxGetSystem(actor, 0);
    if (sys == NULL)
        return;
    ripple.pos.x = a[0] + offset->x - fxOrigin(sys)[0];
    ripple.pos.y = a[1] + offset->y - fxOrigin(sys)[1];
    ripple.pos.z = a[2] + offset->z - fxOrigin(sys)[2];
    ripple.pad_0c = 0;
    setRGBA(&ripple.colour, 0xFF, 0xFF, 0xFF, 0xFF);
    ripple.flags = 0;
    ripple.lifeMs = 1000;
    ripple.size = 30.0f;
    ripple.size2 = 250.0f;
    splash.pos = ripple.pos;
    splash.vel.y = -7.0f;
    splash.size = 20.0f;
    splash.size2 = 4.0f;
    splash.lifeMs = 1000;
    setRGBA(&splash.colour, 0xFF, 0xFF, 0xFF, 0x40);
    zfxCallSpawner("ripple", sys, &ripple);
    zfxCallSpawner("splash", sys, &splash);
}

/* VA 0x00409860, 1 bytes, [I] row, status draft */
void fxPsxFlash(void *model, int alpha, const void *colour, int frames, float a, float b)
{
    /* empty on the PC (fxPsxOnEvent calls it with the event object's model) */
    (void)model; (void)alpha; (void)colour; (void)frames; (void)a; (void)b;
}

/* VA 0x00409880, 31 bytes, [I] row, status draft */
void fxPsxFuncCreate(void *obj, uint32_t type)
{
    /* zfxRegisterFuncTable entry 0: a PSX effect at the object (+0x10 = its position) */
    fxPsxCreateAt(0, 0xFF, (const RwV3d *)((uint8_t *)obj + 0x10), 0, type);
}

/* VA 0x004098b0, 48 bytes, [I] row, status draft */
int fxPsxOnEvent(void *obj, int event)
{
    if (event == 1)
        fxPsxFlash(*(void **)((uint8_t *)obj + 0x1C), 0xFF, "", 16, 150.0f, 75.0f);
    return 0;
}

/* VA 0x004098f0, 145 bytes, [I] row, status draft */
int fxPsxOnSpawn(uint8_t *records, int count)
{
    /* effect records of 16 bytes: +4 s16 position[3], +0x0A kind (only 7 = PSX effect), +0x0B type,
     * +0x0C argument */
    for (; count != 0; count--, records += 16) {
        RwV3d pos;

        pos.x = (float)*(int16_t *)(records + 4);
        pos.y = (float)*(int16_t *)(records + 6);
        pos.z = (float)*(int16_t *)(records + 8);
        if (records[0x0A] != 7)
            g_pfnDebugPrintf("fx: Don't know how to create:%d\n", records[0x0A]);
        else
            fxPsxCreate(records[0x0B], &pos, (int)(records + 0x0C));
    }
    return 0;
}

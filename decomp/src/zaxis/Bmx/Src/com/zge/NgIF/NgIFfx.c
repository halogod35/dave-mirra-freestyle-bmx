/*
 * zaxis/Bmx/Src/com/zge/NgIF/NgIFfx.c
 * Original: C:\zaxis\Bmx\src\com\zge\NgIF\NgIFfx.c
 * Evidence: [V] path string 0x0055C634, zfxCreate's "Unsupported effect" message (line 0x682, so
 *           the file is ~1700 lines); [V] the type tables 0x0055C278..0x0055C3C8 point at the
 *           spawn / update / draw functions, zfxInitTypes registers them; [I] the file runs from
 *           the end of NgApp.c (0x0045C950) to zfxFlash (0x0045FDC0), by link order and data.
 * 31 functions, VA 0x0045c950-0x0045fdeb (DM-057 absorbed the NgApp/NgIFfx and NgIFfx/NgIFpsx
 * gaps and created 14 functions Ghidra had not made: the draw / update functions and four
 * spawners, only reached through the type tables and zfxRegisterSpawner).
 * DM-057 (draft). Non-matching C (AD-8). Types / prototypes: ngif.h. Description: docs/re/ngif.md.
 *
 * The PC particle effects behind fx.c: seven zfx particle types on the engine's particle emitter
 * (the zobjPEmit vtable, slots 0..5 / 7 inherited), each with a spawn function (fills ZfxParticle
 * records from a parameter block, fx.h) and a draw function (ZGL immediate-mode quads, lines or
 * triangles, fading out over the last 0.25-2 s). Spawners are registered by name ("spark",
 * "shard", "splash", "dust", "poster", "skid", "ripple", "lightning") and called by fx.c through
 * zfxCallSpawner. Also: the animated fan textures (props of the def "ANIM") and the camera shake
 * that fx.c calls "flash".
 */
#include <math.h>

#include "decomp.h"
#include "zmath.h"
#include "fx.h"
#include "ngif.h"

#define NGIFFX_SRC "C:\\zaxis\\Bmx\\src\\com\\zge\\NgIF\\NgIFfx.c"

/* ------------------------------------------------------------------ other modules */
uint32_t zpemitSpawn(void *emitter, const char *type, uint32_t count, void *arg, const void *params); /* 0x00495D50 */
void     zmathMatrixFromTo(RwMatrix *m, const RwV3d *from, const RwV3d *to);   /* 0x00495900 */
RwMatrix *zobjCameraGetMatrix(void *cam, int which);
void     zobjSetActive(void *obj, int visible);
extern ZfxType g_zfxTypeSpark, g_zfxTypeShard, g_zfxTypeSplash, g_zfxTypeDust,
               g_zfxTypePoster, g_zfxTypeRipple, g_zfxTypeLightning;   /* 0x0055C278 .. 0x0055C3C8 (0x38 apart) */
extern const void *g_zfxAffectorGravity[2];                             /* 0x005623D8 engine affector */
extern const void *g_zfxAffectorFloorColl[2];                           /* 0x005623E0 */
/* default parameter blocks (used when a spawner gets NULL) */
extern const float g_zfxSparkDefault[14];    /* 0x0055C128 vel random +-4, size 10, spread 10, 2000 ms */
extern const float g_zfxShardDefault[16];    /* 0x0055C160 size 8, 2000 ms */
extern const float g_zfxDustDefault[12];     /* 0x0055C1A0 vel.y -10, size 1, 2000 ms */
extern const float g_zfxPosterDefault[20];   /* 0x0055C1D0 5000 ms */
extern const float g_zfxRippleDefault[10];   /* 0x0055C220 3000 ms, size 10, growth 5 */
extern const float g_zfxLightningDefault[12];/* 0x0055C248 to (0, 100, 0), 8 segments, size 1, jitter 10 */
extern const float g_zfxSparkSpawnDefault[13];  /* 0x0055C400 */
extern const float g_zfxSplashSpawnDefault[13]; /* 0x0055C438 */
extern const float g_zfxShardGravity[4];     /* 0x0055C470 (0, 16, 0, 0) */
extern const char *const g_zfxTexAnimNames[17]; /* 0x0055C0D8: base "FAN1", then frames "FAN1", "FAN2", "FAN3" */
extern const char *const g_zfxTexAnimDef;    /* 0x0055C11C -> "ANIM" */
extern const uint32_t g_zfxTexAnimRate;      /* 0x0055C120 0x031E: 3 frames, 30 per second */

/* ------------------------------------------------------------------ globals (this file) */
uint32_t   g_zfxMatSpark;        /* 0x005AB4E0 "SPARK" */
uint32_t   g_zfxMatSkid;         /* 0x005AB4E4 "SKID" */
uint32_t   g_zfxMatRipple;       /* 0x005AB4E8 "RIPPLE" */
uint32_t   g_zfxMatSplash;       /* 0x005AB4EC "SPLASH" */
static const int8_t *s_shake;    /* 0x005AB4F0 zfxFlash: (x, y) byte pairs, -128 ends */
static void *s_sfxCube;          /* 0x005AB4F4 hidden "SFXCube" model (keeps the fx materials loaded) */
uint32_t   g_zfxMatDust[16];     /* 0x0068FC20 "DUST01".."DUST16" (dust fades through them) */
static int s_texAnimHandle;      /* 0x0068FCF8 */
static int s_shakeHandle;        /* 0x0068FCFC */
ZfxTexAnim g_zfxTexAnims[10];    /* 0x0068FD00 */
void      *g_zfxSystem;          /* 0x0068FF80 the emitter of all game effects (zfxGetSystem) */

/* ------------------------------------------------------------------ helpers */

/* (int16)((rand & 0xFF) - 128.0): -128..127 */
static float rnd128(void)
{
    return (float)(int16_t)((float)(zmathRandom() & 0xFF) - 128.0f);
}

static const RwV3d *emitterOrigin(void *emitter)
{
    /* the emitter's frame (+0x30): position at +8 */
    return (const RwV3d *)(*(uint8_t **)((uint8_t *)emitter + 0x30) + 8);
}

/* ms left of a particle's life (wraps to a huge value once it is over) */
static uint32_t timeLeft(const ZfxParticle *p, uint32_t now)
{
    return p->born + p->lifeMs - now;
}

static void cameraMatrix(RwMatrix *m)
{
    zmathMatrixCopy(m, zobjCameraGetMatrix(g_zglCameraGetObj(g_zglGetCurrentCamera(), 1), 0));
}

static void setUV(ZglVertex *v, float u, float t, RwRGBA c)
{
    v->u = u;
    v->v = t;
    v->colour = c;
}

static RwRGBA grey(uint8_t a)
{
    RwRGBA c;

    c.red = c.green = c.blue = c.alpha = a;
    return c;
}

/* a quad of half size s facing the camera at m.pos (offsets in camera axes) */
static void billboard(ZglVertex v[4], const RwMatrix *m, float s, RwRGBA c)
{
    v[0].pos.x = -s; v[0].pos.y = -s; v[0].pos.z = 0.0f;
    v[1].pos.x = -s; v[1].pos.y = s;  v[1].pos.z = 0.0f;
    v[2].pos.x = s;  v[2].pos.y = s;  v[2].pos.z = 0.0f;
    v[3].pos.x = s;  v[3].pos.y = -s; v[3].pos.z = 0.0f;
    zmathTransformPoint(&v[0].pos, m, &v[0].pos); setUV(&v[0], 0.0f, 0.0f, c);
    zmathTransformPoint(&v[1].pos, m, &v[1].pos); setUV(&v[1], 0.0f, 1.0f, c);
    zmathTransformPoint(&v[2].pos, m, &v[2].pos); setUV(&v[2], 1.0f, 1.0f, c);
    zmathTransformPoint(&v[3].pos, m, &v[3].pos); setUV(&v[3], 1.0f, 0.0f, c);
}

static void drawQuad(uint32_t flags, uint32_t material, ZglVertex v[4])
{
    ZglQuad q;

    q.material = material;
    q.v[0] = &v[0]; q.v[1] = &v[1]; q.v[2] = &v[2]; q.v[3] = &v[3];
    g_zglDrawQuads(flags, &q, 1, 0, 0);
}

/* a streak from prev to pos (camera-facing, width 2 x size along perp): spark and lightning */
static void drawStreak(const ZfxParticle *p, const RwMatrix *cam, const RwMatrix *camInv, RwRGBA c,
                       int lightning)
{
    RwMatrix m = *cam;
    RwV3d a, b, d, perp;
    ZglVertex v[4];
    ZglVertex *order[4];
    ZglQuad q;

    m.pos = p->prev;
    a = p->pos;
    b = p->prev;
    zmathTransformPoint(&a, camInv, &a);
    zmathTransformPoint(&b, camInv, &b);
    d.x = a.x - b.x;
    d.y = a.y - b.y;
    d.z = a.z - b.z;
    if (lightning) {
        perp.x = 0.0f; perp.y = p->size; perp.z = 0.0f;
    } else {
        perp.x = d.y; perp.y = -d.x; perp.z = 0.0f;
        zmathVecNormalize(&perp, &perp);
        perp.x *= p->size; perp.y *= p->size; perp.z *= p->size;
    }
    v[0].pos.x = lightning ? 0.0f : perp.x;  v[0].pos.y = lightning ? -p->size : perp.y; v[0].pos.z = lightning ? 0.0f : perp.z;
    v[1].pos.x = lightning ? 0.0f : -perp.x; v[1].pos.y = lightning ? p->size : -perp.y; v[1].pos.z = lightning ? 0.0f : -perp.z;
    if (lightning) {
        v[2].pos.x = d.x; v[2].pos.y = p->size + d.y; v[2].pos.z = d.z;
        v[3].pos.x = d.x; v[3].pos.y = d.y - p->size; v[3].pos.z = d.z;
    } else {
        v[2].pos.x = d.x - perp.x; v[2].pos.y = d.y - perp.y; v[2].pos.z = d.z - perp.z;
        v[3].pos.x = d.x + perp.x; v[3].pos.y = d.y + perp.y; v[3].pos.z = d.z + perp.z;
    }
    zmathTransformPoint(&v[0].pos, &m, &v[0].pos); setUV(&v[0], 0.0f, 0.0f, c);
    zmathTransformPoint(&v[1].pos, &m, &v[1].pos); setUV(&v[1], 0.0f, 1.0f, c);
    zmathTransformPoint(&v[2].pos, &m, &v[2].pos); setUV(&v[2], 1.0f, 1.0f, c);
    zmathTransformPoint(&v[3].pos, &m, &v[3].pos); setUV(&v[3], 1.0f, 0.0f, c);
    if (lightning && d.x <= 0.0f) {          /* keep the winding facing the camera */
        order[0] = &v[3]; order[1] = &v[2]; order[2] = &v[1]; order[3] = &v[0];
    } else {
        order[0] = &v[0]; order[1] = &v[1]; order[2] = &v[2]; order[3] = &v[3];
    }
    q.material = p->material;
    q.v[0] = order[0]; q.v[1] = order[1]; q.v[2] = order[2]; q.v[3] = order[3];
    g_zglDrawQuads(0x2202, &q, 1, 0, 0);
}

/* untextured particles: a line from the previous to the current position */
static void drawTrail(const ZfxParticle *p, RwRGBA c)
{
    ZglVertex v[2];
    ZglLine l;

    v[0].pos = p->prev; v[0].u = v[0].v = 0.0f; v[0].colour = c;
    v[1].pos = p->pos;  v[1].u = v[1].v = 0.0f; v[1].colour = c;
    l.material = 0;
    l.v[0] = &v[0];
    l.v[1] = &v[1];
    g_zglDrawLines(0x2202, &l, 1, 0, 0);
}

static ZfxParticle *newParticle(ZfxSystem *sys, uint32_t now, uint32_t flags)
{
    ZfxParticle *p = (ZfxParticle *)zmemFreeListAlloc(sys->pool);

    p->born = now;
    p->flags = flags;
    return p;
}

/* ------------------------------------------------------------------ the particle types */

/* VA 0x0045c950, 594 bytes, [I] between, status draft */
void zfxSparkSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "spark" and "splash" spawn (FxParticle): moving particles; velocity = base + spread x
     * random(-1..1) per axis (without parameters: random +-4) */
    uint32_t now = g_zglGetTime();
    const FxParticle *fp = (const FxParticle *)(p != NULL ? p : g_zfxSparkDefault);
    const RwV3d *o = emitterOrigin(emitter);
    uint32_t n;

    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    for (n = count & 0xFF; n != 0; n--) {
        ZfxParticle *q = newParticle(sys, now, ZFXP_MOVING);

        q->colour = fp->colour;
        q->pos.x = q->prev.x = o->x + fp->pos.x;
        q->pos.y = q->prev.y = o->y + fp->pos.y;
        q->pos.z = q->prev.z = o->z + fp->pos.z;
        if (p == NULL) {
            q->vel.x = rnd128() * 0.03125f;
            q->vel.y = rnd128() * 0.03125f;
            q->vel.z = rnd128() * 0.03125f;
        } else {
            q->vel.x = rnd128() * 0.0078125f * fp->size2 + fp->vel.x;
            q->vel.y = rnd128() * 0.0078125f * fp->size2 + fp->vel.y;
            q->vel.z = rnd128() * 0.0078125f * fp->size2 + fp->vel.z;
        }
        q->size = fp->size;
        q->lifeMs = fp->lifeMs;
        q->material = fp->flags;          /* +0x30: the material (zfxSpawnSpark / Splash set it) */
        zHeapListAdd(sys->particles, q);
    }
}

/* VA 0x0045cc50, 1221 bytes, [I] between, status draft */
int zfxSparkDraw(void *type, ZfxSystem *sys)
{
    /* sparks: a streak (or a line without a material), opaque until the last 0.5 s */
    uint32_t now = g_zglGetTime(), left;
    RwMatrix cam, inv;
    ZfxParticle *p;
    RwRGBA c;
    int n = 0;

    (void)type;
    cameraMatrix(&cam);
    zmathMatrixCopy(&inv, &cam);
    zmathMatrixTranspose(&inv, &inv);
    p = (ZfxParticle *)zHeapListFirst(sys->particles);
    if (p == NULL)
        return 0;
    do {
        inv.pos.x = inv.pos.y = inv.pos.z = 0.0f;
        left = timeLeft(p, now);
        c = p->colour;
        c.alpha = left < 0x200 ? (uint8_t)(left >> 1) : 0xFF;
        if (p->material == 0)
            drawTrail(p, c);
        else
            drawStreak(p, &cam, &inv, c, 0);
        p = (ZfxParticle *)zHeapListNext(sys->particles);
        n++;
    } while (p != NULL);
    return n;
}

/* VA 0x0045d260, 343 bytes, [I] between, status draft */
void zfxShardSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "shard" (FxShard): moving and spinning triangles; the spin axis is x or z, whichever is
     * closer to the velocity direction */
    uint32_t now = g_zglGetTime();
    const FxShard *fs = (const FxShard *)(p != NULL ? p : g_zfxShardDefault);
    const RwV3d *o = emitterOrigin(emitter);
    uint32_t n;

    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    for (n = count & 0xFF; n != 0; n--) {
        ZfxParticle *q = newParticle(sys, now, ZFXP_MOVING | ZFXP_SPINNING);

        q->colour = fs->colour;
        q->pos.x = q->prev.x = o->x + fs->pos.x;
        q->pos.y = q->prev.y = o->y + fs->pos.y;
        q->pos.z = q->prev.z = o->z + fs->pos.z;
        q->vel = fs->vel;
        q->axis.x = fs->vel.x;
        q->axis.y = 0.0f;
        q->axis.z = fs->vel.z;
        zmathVecNormalize(&q->axis, &q->axis);
        if (fabsf(q->axis.x) <= fabsf(q->axis.z)) {
            q->axis.x = 0.0f; q->axis.y = 0.0f; q->axis.z = 1.0f;
        } else {
            q->axis.x = 1.0f; q->axis.y = 0.0f; q->axis.z = 0.0f;
        }
        q->spin[0] = (int16_t)fs->spin.x;
        q->spin[1] = (int16_t)fs->spin.y;
        q->spin[2] = (int16_t)fs->spin.z;
        q->size = fs->size;
        q->lifeMs = fs->lifeMs;
        q->material = fs->flags;
        zHeapListAdd(sys->particles, q);
    }
}

/* VA 0x0045d420, 619 bytes, [I] between, status draft */
int zfxShardDraw(void *type, ZfxSystem *sys)
{
    /* shards: two triangles (front and back) in the plane rotated from +z to the spin axis;
     * alpha up to 0x80, fading over the last 0.25 s, gone after 40 s */
    static const RwV3d zAxis = { 0.0f, 0.0f, 1.0f };
    uint32_t now = g_zglGetTime(), left;
    RwMatrix m;
    ZglVertex v[3];
    ZglTri t[2];
    ZfxParticle *p;
    RwRGBA c;
    int n = 0, i;

    (void)type;
    t[0].v[0] = &v[0]; t[0].v[1] = &v[1]; t[0].v[2] = &v[2];
    t[1].v[0] = &v[2]; t[1].v[1] = &v[1]; t[1].v[2] = &v[0];
    p = (ZfxParticle *)zHeapListFirst(sys->particles);
    if (p == NULL)
        return 0;
    do {
        c = p->colour;
        zmathMatrixFromTo(&m, &zAxis, &p->axis);
        m.pos.x = m.pos.y = m.pos.z = 0.0f;
        left = timeLeft(p, now);
        c.alpha = left < 0x100 ? (uint8_t)(left >> 1) : 0x80;
        if (left > 40000)
            c.alpha = 0;
        v[0].pos.x = 0.0f;     v[0].pos.y = -p->size; v[0].pos.z = 0.0f; setUV(&v[0], 0.0f, 0.0f, c);
        v[1].pos.x = p->size;  v[1].pos.y = p->size;  v[1].pos.z = 0.0f; setUV(&v[1], 0.0f, 0.0f, c);
        v[2].pos.x = -p->size; v[2].pos.y = p->size;  v[2].pos.z = 0.0f; setUV(&v[2], 0.0f, 0.0f, c);
        for (i = 0; i < 3; i++) {
            zmathTransformPoint(&v[i].pos, &m, &v[i].pos);
            zmathVecAdd(&v[i].pos, &v[i].pos, &p->pos);
        }
        t[0].material = t[1].material = p->material;
        g_zglDrawTris(0x6502, t, 2, 0, 0);
        p = (ZfxParticle *)zHeapListNext(sys->particles);
        n++;
    } while (p != NULL);
    return n;
}

/* VA 0x0045d730, 619 bytes, [I] between, status draft */
int zfxSplashDraw(void *type, ZfxSystem *sys)
{
    /* splash drops: camera-facing squares, grey up to 0x80, fading over the last second */
    uint32_t now = g_zglGetTime(), left;
    RwMatrix cam;
    ZglVertex v[4];
    ZfxParticle *p;
    uint8_t a;
    int n = 0;

    (void)type;
    cameraMatrix(&cam);
    p = (ZfxParticle *)zHeapListFirst(sys->particles);
    if (p == NULL)
        return 0;
    do {
        cam.pos = p->pos;
        left = timeLeft(p, now);
        a = left > 30000 ? 0 : (left < 0x400 ? (uint8_t)(left >> 3) : 0x80);
        billboard(v, &cam, p->size, grey(a));
        drawQuad(0x2202, p->material, v);
        p = (ZfxParticle *)zHeapListNext(sys->particles);
        n++;
    } while (p != NULL);
    return n;
}

/* VA 0x0045da40, 171 bytes, [I] between, status draft */
void zfxDustSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "dust" (FxDust): one moving puff, grey 0x7F, starting with DUST01 */
    uint32_t now = g_zglGetTime();
    const FxDust *fd = (const FxDust *)(p != NULL ? p : g_zfxDustDefault);
    const RwV3d *o = emitterOrigin(emitter);
    ZfxParticle *q;

    (void)count;
    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    q = newParticle(sys, now, ZFXP_MOVING);
    q->colour = grey(0x7F);
    q->pos.x = q->prev.x = o->x + fd->pos.x;
    q->pos.y = q->prev.y = fd->pos.y + o->y;
    q->pos.z = q->prev.z = fd->pos.z + o->z;
    q->vel = fd->vel;
    q->size = fd->size;
    q->lifeMs = fd->lifeMs;
    q->material = g_zfxMatDust[0];
    zHeapListAdd(sys->particles, q);
}

/* VA 0x0045db20, 620 bytes, [I] between, status draft */
int zfxDustDraw(void *type, ZfxSystem *sys)
{
    /* dust: camera-facing squares; the texture runs through DUST01..DUST16 over the last second */
    uint32_t now = g_zglGetTime(), left;
    RwMatrix cam;
    ZglVertex v[4];
    ZfxParticle *p;
    int n = 0;

    (void)type;
    cameraMatrix(&cam);
    p = (ZfxParticle *)zHeapListFirst(sys->particles);
    if (p == NULL)
        return 0;
    do {
        cam.pos = p->pos;
        left = timeLeft(p, now);
        if (left > 30000)
            left = 0;
        else if (left > 1000)
            left = 1000;
        billboard(v, &cam, p->size, p->colour);
        drawQuad(0x2202, g_zfxMatDust[(15000 - left * 15) / 1000], v);
        p = (ZfxParticle *)zHeapListNext(sys->particles);
        n++;
    } while (p != NULL);
    return n;
}

/* VA 0x0045de30, 80 bytes, [I] between, status draft */
ZfxSystem *zfxPosterCreate(void *owner, uint32_t capacity)
{
    /* the poster type's own system constructor (the others use the engine's) */
    ZfxSystem *sys = (ZfxSystem *)zmemAlloc(sizeof(ZfxSystem));

    if (sys == NULL)
        return NULL;
    sys->particles = zHeapListCreate(capacity & 0xFFFF);
    sys->pool = zmemFreeListCreate(sizeof(ZfxParticle), capacity & 0xFFFF, NULL);
    sys->owner = owner;
    sys->lastTime = g_zglGetTime();
    return sys;
}

/* VA 0x0045dea0, 276 bytes, [I] between, status draft */
void zfxPosterSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "poster" / "skid" (FxSkidQuad): a static quad with four given corners */
    uint32_t now = g_zglGetTime();
    const FxSkidQuad *fq = (const FxSkidQuad *)(p != NULL ? p : g_zfxPosterDefault);
    const RwV3d *o = emitterOrigin(emitter);
    ZfxParticle *q;
    int i;

    (void)count;
    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    q = newParticle(sys, now, 0);
    q->colour = fq->colour;
    q->pos.x = o->x + fq->corner[0].p.x;
    q->pos.y = o->y + fq->corner[0].p.y;
    q->pos.z = o->z + fq->corner[0].p.z;
    q->vel.x = q->vel.y = q->vel.z = 0.0f;
    q->size = 0.0f;
    q->lifeMs = fq->lifeMs;
    q->material = fq->flags;
    for (i = 0; i < 4; i++) {
        q->corner[i].p.x = fq->corner[i].p.x + o->x;
        q->corner[i].p.y = fq->corner[i].p.y + o->y;
        q->corner[i].p.z = fq->corner[i].p.z + o->z;
    }
    zHeapListAdd(sys->particles, q);
}

/* VA 0x0045e010, 400 bytes, [I] between, status draft */
int zfxPosterDraw(void *type, ZfxSystem *sys)
{
    /* posters / skid marks: the quad, alpha fading over the last second (0 once expired) */
    uint32_t now = g_zglGetTime(), left;
    ZglVertex v[4];
    ZfxParticle *p;
    RwRGBA c;
    int n = 0, i;

    (void)type;
    for (p = (ZfxParticle *)zHeapListFirst(sys->particles); p != NULL;
         p = (ZfxParticle *)zHeapListNext(sys->particles), n++) {
        left = timeLeft(p, now);
        c = p->colour;
        if (left > 30000)
            c.alpha = 0;
        else
            c.alpha = left < 0x400 ? (uint8_t)(left >> 2) : 0xFF;
        for (i = 0; i < 4; i++)
            v[i].pos = p->corner[i].p;
        setUV(&v[0], 0.0f, 0.0f, c);
        setUV(&v[1], 0.0f, 1.0f, c);
        setUV(&v[2], 1.0f, 1.0f, c);
        setUV(&v[3], 1.0f, 0.0f, c);
        drawQuad(0x4102, p->material, v);
    }
    return n;
}

/* VA 0x0045e210, 143 bytes, [I] between, status draft */
void zfxRippleSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "ripple" (FxRipple): a ring that grows at size2 units per second (stored in vel.x) */
    uint32_t now = g_zglGetTime();
    const FxRipple *fr = (const FxRipple *)(p != NULL ? p : g_zfxRippleDefault);
    const RwV3d *o = emitterOrigin(emitter);
    ZfxParticle *q;

    (void)count;
    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    q = newParticle(sys, now, 0);
    q->colour = fr->colour;
    q->pos.x = o->x + fr->pos.x;
    q->pos.y = fr->pos.y + o->y;
    q->pos.z = fr->pos.z + o->z;
    q->vel.x = fr->size2;
    q->vel.y = 0.0f;
    q->vel.z = 0.0f;
    q->size = fr->size;
    q->lifeMs = fr->lifeMs;
    q->material = fr->flags;
    zHeapListAdd(sys->particles, q);
}

/* VA 0x0045e2d0, 415 bytes, [I] between, status draft */
int zfxRippleDraw(void *type, ZfxSystem *sys)
{
    /* ripples: a horizontal square of half size `size`, grey fading over the last 2 s */
    uint32_t now = g_zglGetTime(), left;
    ZglVertex v[4];
    ZfxParticle *p;
    uint8_t a;
    int n = 0;

    (void)type;
    for (p = (ZfxParticle *)zHeapListFirst(sys->particles); p != NULL;
         p = (ZfxParticle *)zHeapListNext(sys->particles), n++) {
        left = timeLeft(p, now);
        a = left > 30000 ? 0 : (left < 0x800 ? (uint8_t)(left >> 3) : 0xFF);
        v[0].pos.x = p->pos.x + p->size; v[0].pos.y = p->pos.y; v[0].pos.z = p->pos.z + p->size;
        v[1].pos.x = p->pos.x - p->size; v[1].pos.y = p->pos.y; v[1].pos.z = p->pos.z + p->size;
        v[2].pos.x = p->pos.x - p->size; v[2].pos.y = p->pos.y; v[2].pos.z = p->pos.z - p->size;
        v[3].pos.x = p->pos.x + p->size; v[3].pos.y = p->pos.y; v[3].pos.z = p->pos.z - p->size;
        setUV(&v[0], 0.0f, 0.0f, grey(a));
        setUV(&v[1], 0.0f, 1.0f, grey(a));
        setUV(&v[2], 1.0f, 1.0f, grey(a));
        setUV(&v[3], 1.0f, 0.0f, grey(a));
        drawQuad(0x2203, p->material, v);
    }
    return n;
}

/* VA 0x0045e4e0, 486 bytes, [I] between, status draft */
int zfxRippleUpdate(void *type, ZfxSystem *sys)
{
    /* The ripple type's update (a copy of the engine's with the size growth): integrate the moving
     * particles and keep their bounding box, grow every size by vel.x per second, spin, drop the
     * expired ones, then run the type's affectors. */
    uint32_t now = g_zglGetTime();
    uint32_t last = sys->lastTime;
    float dt;
    uint8_t *p;
    void **aff;
    int n = 0;

    if (sys != NULL && sys->particles != NULL) {
        sys->boundMin.x = sys->boundMin.y = sys->boundMin.z = 99999.0f;
        sys->boundMax.x = sys->boundMax.y = sys->boundMax.z = -99999.0f;
        p = (uint8_t *)zHeapListFirst(sys->particles);
        dt = (float)(now - last);
        while (p != NULL) {
            ZfxParticle *q = (ZfxParticle *)p;

            n++;
            if (q->flags & ZFXP_MOVING) {
                q->prev = q->pos;
                q->pos.x += dt * q->vel.x * 0.001f;
                q->pos.y += dt * q->vel.y * 0.001f;
                q->pos.z += dt * q->vel.z * 0.001f;
                zmathVecAdd(&q->pos, &q->pos, &q->vel);
                if (q->pos.x > sys->boundMax.x) sys->boundMax.x = q->pos.x;
                if (q->pos.y > sys->boundMax.y) sys->boundMax.y = q->pos.y;
                if (q->pos.z > sys->boundMax.z) sys->boundMax.z = q->pos.z;
                if (q->pos.x < sys->boundMin.x) sys->boundMin.x = q->pos.x;
                if (q->pos.y < sys->boundMin.y) sys->boundMin.y = q->pos.y;
                if (q->pos.z < sys->boundMin.z) sys->boundMin.z = q->pos.z;
            }
            q->size += dt * q->vel.x * 0.001f;
            if (q->flags & ZFXP_SPINNING)
                zmathVecRotateXYZ(&q->axis, &q->axis, (const uint16_t *)q->spin);
            if (now - q->born < q->lifeMs) {
                p = (uint8_t *)zHeapListNext(sys->particles);
            } else {
                zHeapListRemoveCurrent(sys->particles);
                zmemFreeListFree(sys->pool, q);
                p = (uint8_t *)zHeapListCurrent(sys->particles);
            }
        }
    }
    /* affector list at type +0x24: { next, ?, affector, arg } */
    for (aff = *(void ***)((uint8_t *)type + 0x24); aff != NULL; aff = (void **)aff[0]) {
        void **affector = (void **)aff[2];

        ((void (__cdecl *)(void *, ZfxSystem *, void *))affector[1])(affector, sys, aff[3]);
    }
    sys->lastTime = now;
    return n;
}

/* VA 0x0045e750, 668 bytes, [I] between, status draft */
void zfxLightningSpawn(void *type, void *emitter, ZfxSystem *sys, uint32_t count, void *unused, const float *p)
{
    /* "lightning": a bolt of `segments` streaks from `from` to `to`, each joint jittered by up to
     * +-jitter; blue-white (0x10, 0x10, 0xAF, 0xFF), segment i lives 300 + 10 i ms.
     * Parameters: from[4], to[4], u32 segments, size, jitter, material. */
    uint32_t now = g_zglGetTime();
    const float *lp = p != NULL ? p : g_zfxLightningDefault;
    uint32_t segments = *(const uint32_t *)&lp[8];
    const RwV3d *o = emitterOrigin(emitter);
    RwV3d cur, end;
    uint32_t n, i;

    (void)unused;
    if (type == NULL || sys == NULL)
        return;
    for (n = count & 0xFF; n != 0; n--) {
        cur.x = lp[0]; cur.y = lp[1]; cur.z = lp[2];
        for (i = 0; i < segments; i++) {
            ZfxParticle *q = (ZfxParticle *)zmemFreeListAlloc(sys->pool);

            if (i + 1 == segments) {
                end.x = lp[4]; end.y = lp[5]; end.z = lp[6];
            } else {
                end.x = (lp[4] - cur.x) / (float)(segments - i) + rnd128() * 0.0078125f * lp[10] + cur.x;
                end.y = (lp[5] - cur.y) / (float)(segments - i) + rnd128() * 0.0078125f * lp[10] + cur.y;
                end.z = (lp[6] - cur.z) / (float)(segments - i) + rnd128() * 0.0078125f * lp[10] + cur.z;
            }
            q->flags = 0;
            q->born = now;
            q->colour.red = 0x10; q->colour.green = 0x10; q->colour.blue = 0xAF; q->colour.alpha = 0xFF;
            q->prev.x = cur.x + o->x; q->prev.y = cur.y + o->y; q->prev.z = cur.z + o->z;
            q->pos.x = end.x + o->x;  q->pos.y = end.y + o->y;  q->pos.z = end.z + o->z;
            q->lifeMs = 300 + 10 * i;
            q->size = lp[9];
            q->material = *(const uint32_t *)&lp[11];
            zHeapListAdd(sys->particles, q);
            cur = end;
        }
    }
}

/* VA 0x0045eaa0, 1125 bytes, [I] between, status draft */
int zfxLightningDraw(void *type, ZfxSystem *sys)
{
    /* lightning: vertical-width streaks (or lines); the alpha is the ms left over the last 0.25 s
     * and 0 before that */
    uint32_t now = g_zglGetTime(), left;
    RwMatrix cam, inv;
    ZfxParticle *p;
    RwRGBA c;
    int n = 0;

    (void)type;
    cameraMatrix(&cam);
    zmathMatrixCopy(&inv, &cam);
    zmathMatrixTranspose(&inv, &inv);
    p = (ZfxParticle *)zHeapListFirst(sys->particles);
    if (p == NULL)
        return 0;
    do {
        inv.pos.x = inv.pos.y = inv.pos.z = 0.0f;
        left = timeLeft(p, now);
        c = p->colour;
        c.alpha = left < 0x100 ? (uint8_t)left : 0;
        if (p->material == 0)
            drawTrail(p, c);
        else
            drawStreak(p, &cam, &inv, c, 1);
        p = (ZfxParticle *)zHeapListNext(sys->particles);
        n++;
    } while (p != NULL);
    return n;
}

/* ------------------------------------------------------------------ the spawners (by name) */

/* VA 0x0045f030, 68 bytes, [I] between, status draft */
void zfxSpawnSpark(void *emitter, void *params)
{
    if (params == NULL) {
        zpemitSpawn(emitter, "spark", 2, NULL, g_zfxSparkSpawnDefault);
        return;
    }
    ((FxParticle *)params)->flags = g_zfxMatSpark;
    zpemitSpawn(emitter, "spark", 2, NULL, params);
}

/* VA 0x0045f0a0, 28 bytes, [I] between, status draft */
void zfxSpawnShard(void *emitter, void *params)
{
    zpemitSpawn(emitter, "shard", 1, NULL, params);
}

/* VA 0x0045f0d0, 68 bytes, [I] between, status draft */
void zfxSpawnSplash(void *emitter, void *params)
{
    if (params == NULL) {
        zpemitSpawn(emitter, "splash", 2, NULL, g_zfxSplashSpawnDefault);
        return;
    }
    ((FxParticle *)params)->flags = g_zfxMatSplash;
    zpemitSpawn(emitter, "splash", 0x20, NULL, params);
}

/* VA 0x0045f140, 59 bytes, [I] between, status draft */
void zfxSpawnDust(void *emitter, void *params)
{
    zpemitSpawn(emitter, "dust", 1, NULL, params != NULL ? params : (void *)g_zfxDustDefault);
}

/* VA 0x0045f190, 59 bytes, [I] between, status draft */
void zfxSpawnPoster(void *emitter, void *params)
{
    zpemitSpawn(emitter, "poster", 1, NULL, params != NULL ? params : (void *)g_zfxPosterDefault);
}

/* VA 0x0045f1e0, 192 bytes, [I] between, status draft */
void zfxSpawnRipple(void *emitter, void *params)
{
    /* three rings: 1.0 s 30 -> +250/s, 1.25 s 20 -> +200/s (5 higher), 1.5 s 10 -> +150/s (15 higher) */
    FxRipple *r = (FxRipple *)params;

    if (r == NULL) {
        zpemitSpawn(emitter, "ripple", 1, NULL, g_zfxRippleDefault);
        return;
    }
    r->flags = g_zfxMatRipple;
    r->lifeMs = 1000; r->size = 30.0f; r->size2 = 250.0f;
    zpemitSpawn(emitter, "ripple", 1, NULL, r);
    r->lifeMs = 1250; r->pos.y -= 5.0f; r->size = 20.0f; r->size2 = 200.0f;
    zpemitSpawn(emitter, "ripple", 1, NULL, r);
    r->lifeMs = 1500; r->pos.y -= 10.0f; r->size = 10.0f; r->size2 = 150.0f;
    zpemitSpawn(emitter, "ripple", 1, NULL, r);
}

/* VA 0x0045f2d0, 70 bytes, [I] between, status draft */
void zfxSpawnSkid(void *emitter, void *params)
{
    /* "skid": a poster whose flags 0 / 1 pick the SKID / RIPPLE material */
    FxSkidQuad *q = (FxSkidQuad *)params;

    if (q == NULL) {
        q = (FxSkidQuad *)g_zfxPosterDefault;
    } else {
        if (q->flags == 0)
            q->flags = g_zfxMatSkid;
        if (q->flags == 1)
            q->flags = g_zfxMatRipple;
    }
    zpemitSpawn(emitter, "poster", 1, NULL, q);
}

/* VA 0x0045f340, 241 bytes, [I] between, status draft */
void zfxSpawnLightning(void *emitter)
{
    /* a bolt from the origin to a random point within +-128, 8 segments, size 5, jitter 10 */
    float p[12];

    p[0] = p[1] = p[2] = p[3] = 0.0f;
    p[4] = rnd128();
    p[5] = rnd128();
    p[6] = rnd128();
    p[7] = 0.0f;
    *(uint32_t *)&p[8] = 8;
    p[9] = 5.0f;
    p[10] = 10.0f;
    p[11] = 0.0f;
    zpemitSpawn(emitter, "lightning", 1, NULL, p);
}

/* VA 0x0045f480, 6 bytes, [I] row, status draft */
void *zfxGetSystem(void *actor, int kind)
{
    (void)actor;
    (void)kind;
    return g_zfxSystem;
}

/* ------------------------------------------------------------------ animated textures */

/* VA 0x0045f4a0, 335 bytes, [I] between, status draft */
int zfxInitTexAnims(void)
{
    /* every prop def named "ANIM" gets a slot: the materials FAN1..FAN3 of its model (else of the
     * world) are cycled at 30 frames per second by zfxUpdateTexAnims */
    uint32_t now, anim;
    zListNode *node;
    ZfxTexAnim *t = g_zfxTexAnims;
    int count = 0, i;

    zHashName("ANIM");
    now = g_zglGetTime();
    for (i = 0; i < 10; i++)
        g_zfxTexAnims[i].def = NULL;
    anim = zHashName(g_zfxTexAnimDef);
    for (node = zpropGetDefList()->head; node != NULL; node = node->next) {
        ZpropDef *def = (ZpropDef *)node;

        if (def->info.id != anim)
            continue;
        t->def = def;
        t->frame = 0;
        t->fps = (uint8_t)g_zfxTexAnimRate;
        t->frames = (uint8_t)(g_zfxTexAnimRate >> 8);
        t->lastTime = now;
        t->base = (*g_zrwWorldApi.findMaterial)(zHashName(g_zfxTexAnimNames[0]));
        for (i = 0; i < (int)((g_zfxTexAnimRate >> 8) & 0xFF); i++) {
            void *mat = g_zglModelFindMaterial(def->info.model, zHashName(g_zfxTexAnimNames[1 + i]));

            if (mat == NULL)
                mat = (*g_zrwWorldApi.findMaterial)(zHashName(g_zfxTexAnimNames[1 + i]));
            t->mat[i] = mat;
        }
        count++;
        t++;
    }
    return count != 0;
}

/* VA 0x0045f650, 231 bytes, [I] between, status draft */
int zfxUpdateTexAnims(void *arg)
{
    /* frame callback: at each slot's rate, rotate the textures one material along */
    uint32_t now = g_zglGetTime();
    ZfxTexAnim *t;
    void *first;
    int i;

    (void)arg;
    for (t = g_zfxTexAnims; t < g_zfxTexAnims + 10; t++) {
        if (t->def == NULL)
            return 1;
        if (now - t->lastTime < (uint32_t)(1000 / t->fps))
            continue;
        t->lastTime = now;
        t->frame = (uint8_t)((t->frame + 1) % t->frames);
        first = g_zglMatGetTexture(t->mat[0], 0);
        for (i = 0; i < t->frames - 1; i++)
            g_zglMatSetTexture(t->mat[i], g_zglMatGetTexture(t->mat[i + 1], 0), 0);
        g_zglMatSetTexture(t->mat[t->frames - 1], first, 0);
    }
    return 1;
}

/* ------------------------------------------------------------------ setup */

/* VA 0x0045f780, 918 bytes, [I] row, status draft */
void zfxInitTypes(void)
{
    /* fxInit (fx.c): register the types, affectors and spawners, look the materials up, create the
     * effect emitter, start the texture animations */
    static const char *const dust[16] = { "DUST01", "DUST02", "DUST03", "DUST04", "DUST05", "DUST06",
                                          "DUST07", "DUST08", "DUST09", "DUST10", "DUST11", "DUST12",
                                          "DUST13", "DUST14", "DUST15", "DUST16" };
    int i;

    zfxRegistryInit();
    zfxRegisterType(&g_zfxTypeLightning, "lightning");
    zfxRegisterType(&g_zfxTypeSpark, "spark");
    zfxRegisterType(&g_zfxTypeShard, "shard");
    zfxRegisterType(&g_zfxTypeSplash, "splash");
    zfxRegisterType(&g_zfxTypeDust, "dust");
    zfxRegisterType(&g_zfxTypePoster, "poster");
    zfxRegisterType(&g_zfxTypeRipple, "ripple");
    zfxRegisterAffector((void *)g_zfxAffectorGravity, "gravity");
    zfxRegisterAffector((void *)g_zfxAffectorFloorColl, "floorcoll");
    zfxRegisterSpawner((void *)zfxSpawnSpark, "spark");
    zfxRegisterSpawner((void *)zfxSpawnShard, "shard");
    zfxRegisterSpawner((void *)zfxSpawnSplash, "splash");
    zfxRegisterSpawner((void *)zfxSpawnLightning, "lightning");
    zfxRegisterSpawner((void *)zfxSpawnDust, "dust");
    zfxRegisterSpawner((void *)zfxSpawnPoster, "poster");
    zfxRegisterSpawner((void *)zfxSpawnSkid, "skid");
    zfxRegisterSpawner((void *)zfxSpawnRipple, "ripple");
    g_zfxTypeSpark.addAffector(&g_zfxTypeSpark, "gravity", NULL);
    g_zfxTypeSpark.addAffector(&g_zfxTypeSplash, "gravity", NULL);
    g_zfxTypeSpark.addAffector(&g_zfxTypeShard, "gravity", g_zfxShardGravity);
    g_zfxTypeSpark.addAffector(&g_zfxTypeShard, "floorcoll", NULL);
    s_sfxCube = zsceneCreateModel(0, "SFXCube");
    zobjSetActive(s_sfxCube, 0);
    g_zfxMatSpark = g_zglMatFind(zHashName("SPARK"), 0);
    g_zfxMatSkid = g_zglMatFind(zHashName("SKID"), 0);
    g_zfxMatRipple = g_zglMatFind(zHashName("RIPPLE"), 0);
    g_zfxMatSplash = g_zglMatFind(zHashName("SPLASH"), 0);
    for (i = 0; i < 16; i++)
        g_zfxMatDust[i] = g_zglMatFind(zHashName(dust[i]), 0);
    g_zfxSystem = zsceneCreateEmitter(0);
    if (s_texAnimHandle == 0 && zfxInitTexAnims() == 1)
        s_texAnimHandle = g_zglAddFrameCallback(8, zfxUpdateTexAnims, NULL, 0);
}

/* VA 0x0045fc10, 114 bytes, [I] row, status draft */
void zfxShutdownTypes(void)
{
    if (s_texAnimHandle != 0) {
        g_zglRemoveFrameCallback(s_texAnimHandle);
        s_texAnimHandle = 0;
    }
    g_zfxTypeSpark.destroy(&g_zfxTypeSpark);
    g_zfxTypeDust.destroy(&g_zfxTypeDust);
    g_zfxTypePoster.destroy(&g_zfxTypePoster);
    g_zfxTypeLightning.destroy(&g_zfxTypeLightning);
    g_zfxTypeRipple.destroy(&g_zfxTypeRipple);
    g_zfxTypeSplash.destroy(&g_zfxTypeSplash);
    g_zfxTypeShard.destroy(&g_zfxTypeShard);
    zfxRegistryShutdown();
}

/* VA 0x0045fcb0, 40 bytes, [V] direct, status draft */
int zfxCreate(int effect, void *params, int arg)
{
    /* the PlayStation effect scripts are not supported on the PC */
    (void)params;
    (void)arg;
    if (effect != 0)
        NGIF_MISSING(NGIFFX_SRC, 0x682, "zfxCreate : Unsupported effect");
    return 0;
}

/* VA 0x0045fcf0, 154 bytes, [I] between, status draft */
int zfxShakeUpdate(void *arg)
{
    /* frame callback of zfxFlash: one (x, y) pair per frame, x 0.005 */
    void *cam = g_zglGetCurrentCamera();

    (void)arg;
    if (s_shake == NULL)
        return 1;
    if (*s_shake == -128) {
        g_zglRemoveFrameCallback(s_shakeHandle);
        g_zglCameraSetShake(cam, 0.0f, 0.0f);
        s_shake = NULL;
        s_shakeHandle = 0;
        return 1;
    }
    g_zglCameraSetShake(cam, (float)s_shake[0] * 0.005f, (float)s_shake[1] * 0.005f);
    s_shake += 2;
    return 1;
}

/* VA 0x0045fdc0, 44 bytes, [I] row, status draft */
void zfxFlash(const void *shake)
{
    /* fx.c's "flash" (fxCollectEffect): a camera shake sequence */
    s_shake = (const int8_t *)shake;
    if (s_shakeHandle == 0)
        s_shakeHandle = g_zglAddFrameCallback(1, zfxShakeUpdate, NULL, 0);
}

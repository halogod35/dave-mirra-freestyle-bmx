/*
 * zaxis/Bmx/Src/com/action/prop.c
 * Original: C:\zaxis\Bmx\Src\com\action\prop.c
 * Evidence: [V] path string 0x00558678, referenced by bmxpropInit's assert ("state info overflow",
 *           line 0x1EB); [V] the "bmxpropInit:" / "bmxpropGetPropByID:" messages; [I] for the rest:
 *           every function works on g_bmxprops (0x00694460) / g_bmxpropTable (0x00557D28), and they
 *           are contiguous between physics.c and replay.c.
 * 18 functions, 5854 bytes, VA 0x0042de70-0x0042fca1.
 * Draft. Non-matching C (AD-8). Types: bmxprop.h. Description: docs/re/props-replay-fx.md.
 *
 * The prop/replay/fx pass created bmxpropDraw 0x0042EB50 (a "return 0" draw callback that bmxpropInit registers;
 * Ghidra had not made it a function) with a decomp/symbols/symbols.csv row.
 *
 * Per level, up to 16 table slots (g_bmxpropTable[level][16]) name props to drive: movers (doors,
 * ramps, platforms, traffic that runs over the rider), goal props (switched by game.c when a goal
 * is done) and model pairs. Every prop with an OBJI u16 id is also a breakable "switch": touching
 * a class-9 trigger face with that code hides it and shows prop id + 1.
 */

#include <math.h>

#include "decomp.h"

#include "zmath.h"

#include "physics.h"

#include "bmxprop.h"

#define PROP_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\prop.c"
#define ANGLE_PER_DEGREE 182.04445f    /* 0x0054D01C = 0x10000 / 360 */
#define DEGREE_PER_ANGLE 0.0054931641f /* 0x0054D15C = 360 / 0x10000 */
#define SFX_PROP 0xFA3                 /* bmxsound command: a prop sound (mode 0 start, 1 stop, 2 move, 3 one-shot) */

/* ------------------------------------------------------------------ other modules (current names) */
uint32_t   bmxsoundEvent(uint32_t cmd, const void *pos, uint32_t mode, int id, int volume); /* bmxsound.c */
void       riderAddCrashDamage(BmxRider *r, int kind, float impact);             /* 0x00431E10 rider.c */
int        gameOnPropTouched(int player, void *prop);                            /* 0x0040CA60 game.c */
int        gameOnObjectHit(int player, void *face);                              /* 0x0040CBE0 game.c */
void       fxCollectEffect(void);                                                /* 0x00408A80 fx.c */
void       fxShards(const RwV3d *pos, const RwV3d *vel);                         /* 0x00408FD0 fx.c */
void       zrollerAddDrawCallback(int (*draw)(void));                              /* per-frame draw callback */
NgPropCol **zbspFindPropsByGroup(uint32_t lo, uint32_t hi);  /* collision records of the props whose OBJI
                                                             byte 7 is in lo..hi (NULL-terminated, static) */
void       zbspPropSetMode(NgPropCol *col, int mode);                          /* NGPROPCOL_SET_* */
const float *zcolGetPropShape(uint32_t defIndex);   /* the def's collision shape; [1..3] = half
                                                             width, height, half depth */
void       zbspPropSetPosition(NgPropCol *col, const float *pos);                /* 0x004662B0 */
void       zbspPropSetRotation(NgPropCol *col, const int16_t *euler);            /* 0x004661C0 */
void       zobjSetActive(void *obj, int visible);                                /* show / hide a scene object */
const float *zobjGetPosition(void *obj);                                           /* -> its position */
extern void (*g_zrwModelSetFlags)(void *model, uint32_t set, uint32_t clear);    /* 0x00560ED8 -> 0x004888C0 */

/* ------------------------------------------------------------------ globals (this file) */
BmxPropState g_bmxpropsStart;           /* 0x0056C4F8 state after bmxpropInit (bmxpropReset) */
BmxPropState g_bmxprops;                /* 0x00694460 */
/* g_bmxpropTable 0x00557D28: [14 levels][16] BmxPropDef, read back by tests/test_decomp_prop_replay_fx.py and
 * listed in docs/re/props-replay-fx.md; e.g. Greenville Dirt: 12 movers (traffic 20-25 runs from
 * the start), Woodward Dirt: gates 20/21 open the fence pairs 1090/1080. */
extern const BmxPropDef g_bmxpropTable[BMXPROP_LEVELS][BMXPROP_SLOTS];

/* ------------------------------------------------------------------ helpers */
static const BmxPropDef *propDef(const BmxProp *p)
{
    return &g_bmxpropTable[g_currentLevel][p->index];
}

static int isMotion(const BmxPropDef *d, int lo, int hi)
{
    return d->motion >= lo && d->motion <= hi;
}

/* the props named by link[] (movers 13..20) are switched when the move ends */
static void propSwitchLinks(BmxProp *p, int now)
{
    uint32_t i;
    BmxProp *other;

    for (i = 0; i < 2; i++) {
        if (p->link[i] == 0 || (other = bmxpropGetPropByID(p->link[i])) == NULL)
            continue;
        if (now)
            bmxpropActivateNow(p->player, other);
        else
            bmxpropActivate(p->player, other);
    }
}

static void propRiderUpdate(BmxProp *p, const BmxPropDef *d)
{
    /* a moving prop vs. the riders: traffic (motions 8/9) runs a rider over; a rider grinding or
     * stalling on the prop moves with it */
    const float *shape = zcolGetPropShape(p->col->defIndex);
    float halfW = p->col->scale[0] * shape[1];
    float height = p->col->scale[1] * shape[2];
    float halfD = p->col->scale[2] * shape[3];
    uint32_t n = g_numPlayers ? g_numPlayers : 1, s;

    for (s = 0; s < n; s++) {
        BmxRider *r = &g_riders[s];
        RwV3d rel, l;      /* was "d", which hid the BmxPropDef *d parameter */
        int dx, dy, dz;

        if (!(r->stateFlags & 1))
            continue;
        rel.x = r->pos[0] + r->physHipOffset[0] - p->pos[0];
        rel.y = r->pos[1] + r->physHipOffset[1] - p->pos[1];
        rel.z = r->pos[2] + r->physHipOffset[2] - p->pos[2];
        dx = (int)rel.x; dy = (int)rel.y; dz = (int)rel.z;
        if (dz * dz + dy * dy + dx * dx >= 1200 * 1200)
            continue;
        zmathTransformPoint(&l, &p->matrix, &rel);            /* hips in the prop's frame */
        if (fabsf(l.x) > halfW || fabsf(l.z) > halfD || l.y > 120.0f || l.y < -(height + 120.0f)) {
            if (isMotion(d, BMXMOVE_FALL, BMXMOVE_FALL2))
                contrlerRumble(r->slot, 1, 0x40);           /* near miss */
        } else if (isMotion(d, BMXMOVE_FALL, BMXMOVE_FALL2)) {
            float force = zmathVecLength((const RwV3d *)p->vel);
            uint8_t *dyn = (uint8_t *)r->animCtl;           /* rag doll: 4 bodies' velocities */
            RwV3d v;

            bmxphysSetState(r, BMXPHYS_CRASH);
            r->crashKind = 3;
            v.x = p->vel[0] * 1.05f;
            v.y = p->vel[1] * 1.05f - 15.0f;
            v.z = p->vel[2] * 1.05f;
            zmathVecRotateY(&v, &v, 0);
            *(RwV3d *)(dyn + 0x088) = v;
            *(RwV3d *)(dyn + 0x13C) = v;
            *(RwV3d *)(dyn + 0x1F0) = v;
            *(RwV3d *)(dyn + 0x9AC) = v;
            riderAddCrashDamage(r, 0, force);
            riderAddCrashDamage(r, 13, force);
            if (r->impactTimer == 0) {
                bmxsoundEvent(0xBBC, r->pos, 7, 0x7F, 0);
                bmxsoundEvent(0xBBD, r->pos, 1, 3, 0);
                bmxsoundEvent(0xBBE, r->pos, 1, 3, 0);
                contrlerRumble(r->slot, 1, 0xFF);
                fxCollectEffect();
                r->impactTimer = 60;
            }
        }
        if (((r->surface == BMXSURF_PROP && r->surfaceObj == (void *)p->col) ||
             (r->surface == BMXSURF_EDGE && ((NgColEdge *)r->surfaceObj)->owner == (void *)p->col)) &&
            (bmxphysGetState(r) == BMXPHYS_GRIND || bmxphysGetState(r) == BMXPHYS_STALL)) {
            zmathVecAdd((RwV3d *)r->pos, (const RwV3d *)r->pos, (const RwV3d *)p->vel);
            zmathVecAdd((RwV3d *)r->spawnPos, (const RwV3d *)r->spawnPos, (const RwV3d *)p->vel);
        }
    }
}

/* ------------------------------------------------------------------ functions */

/* VA 0x0042de70, 41 bytes, [I] row, status draft */
Zprop *bmxpropFindProp(uint32_t id)
{
    /* the first prop whose OBJI u16 at +8 equals id */
    Zprop *z;

    for (z = (Zprop *)zpropGetList()->head; z != NULL; z = (Zprop *)z->link.next)
        if (z->info.obji != NULL && *(uint16_t *)(z->info.obji + 8) == id)
            return z;
    return NULL;
}

/* VA 0x0042deb0, 761 bytes, [V] direct, status draft */
void bmxpropInit(void)
{
    /* Level start: bind the 16 table slots to their props, list the breakable props, start the
     * Greenville Dirt traffic, keep a copy for bmxpropReset and register the draw callback. */
    uint32_t i;
    BmxPropSwitch *sw;
    Zprop *z;

    for (i = 0; i < BMXPROP_SLOTS; i++) {
        BmxProp *p = &g_bmxprops.props[i];
        const BmxPropDef *d = &g_bmxpropTable[g_currentLevel][i];

        p->state = BMXPROPST_NONE;
        p->col = NULL;
        if (d->id == 0)
            continue;
        if (d->type < BMXPROP_SOUND || d->type > BMXPROP_PAIR) {
            NgPropCol **list = zbspFindPropsByGroup(d->id, d->id);
            NgPropCol *col;

            if (list == NULL) {
                g_pfnDebugPrintf("bmxpropInit: error finding prop with tag %d\n", d->id);
                continue;
            }
            col = list[0];
            p->index = (uint8_t)i;
            p->col = col;
            p->pos[0] = (float)col->pos[0];
            p->pos[1] = (float)col->pos[1];
            p->pos[2] = (float)col->pos[2];
            zmathVecZero((RwV3d *)p->vel);
            zmathMatrixIdentity(&p->matrix);
            zmathMatrixRotateXYZ(&p->matrix, &p->matrix, col->rot);
            p->spin[0] = p->spin[1] = p->spin[2] = 0;
            p->flags = 0;
            p->timer = 0;
            p->state = BMXPROPST_IDLE;
            p->toggled = 0;
            p->link[0] = d->link[0];
            p->link[1] = d->link[1];
            col->flags |= NGPROPCOL_DRIVEN;
            col->game = p;
            p->sound = -1;
            if (d->sfxLoop != -1)
                p->sound = (int32_t)bmxsoundEvent(SFX_PROP, p->pos, 0, d->sfxLoop, 0x7F);
            for (list++; *list != NULL; list++)          /* only one prop per tag is driven */
                g_pfnDebugPrintf("bmxpropInit: error: extra prop with tag %d (X: %d, Z: %d)\n", d->id,
                                 (*list)->pos[0], (*list)->pos[2]);
        } else {
            Zprop *zp = bmxpropFindProp(d->id);

            if (zp == NULL) {
                g_pfnDebugPrintf("bmxpropInit: error finding face with tag %d\n", d->id);
                continue;
            }
            p->index = (uint8_t)i;
            p->col = (NgPropCol *)zp->info.user;
            p->pos[0] = ((const float *)zp->info.obj)[1];  /* scene object +4: position */
            p->pos[1] = ((const float *)zp->info.obj)[2];
            p->pos[2] = ((const float *)zp->info.obj)[3];
            p->state = BMXPROPST_IDLE;
            p->sound = -1;
            if (d->sfxLoop != -1)
                p->sound = (int32_t)bmxsoundEvent(SFX_PROP, p->pos, 0, d->sfxLoop, 0x7F);
        }
    }

    zstringMemSet(g_bmxprops.switches, 0, sizeof(g_bmxprops.switches));
    sw = g_bmxprops.switches;
    for (z = (Zprop *)zpropGetList()->head; z != NULL; z = (Zprop *)z->link.next) {
        if (z->info.obji == NULL || *(uint16_t *)(z->info.obji + 8) == 0)
            continue;
        if (sw >= &g_bmxprops.switches[BMXPROP_SWITCHES])
            zAssertFail("state info overflow", PROP_SRC, 0x1EB, NULL);
        sw->unk_00 = 0;
        sw->prop = z;
        sw++;
    }

    if (g_currentLevel == 0) {                           /* Greenville Dirt: the traffic (20..25) runs */
        for (i = 0; i < BMXPROP_SLOTS; i++) {
            BmxProp *p = &g_bmxprops.props[i];
            uint16_t id = g_bmxpropTable[g_currentLevel][i].id;

            if (p->state != BMXPROPST_NONE && id > 19 && id < 26) {
                bmxpropTrigger(p, 0, 0);
                p->state = BMXPROPST_MOVING;
            }
        }
    }
    zstringMemCopy(&g_bmxpropsStart, &g_bmxprops, sizeof(BmxPropState));
    zrollerAddDrawCallback(bmxpropDraw);
}

/* VA 0x0042e270, 81 bytes, [I] row, status draft */
void bmxpropShutdown(void)
{
    /* stop the looping prop sounds */
    uint32_t i;

    for (i = 0; i < BMXPROP_SLOTS; i++) {
        BmxProp *p = &g_bmxprops.props[i];

        if (p->state != BMXPROPST_NONE && g_bmxpropTable[g_currentLevel][i].sfxLoop != -1)
            bmxsoundEvent(SFX_PROP, p->pos, 1, p->sound, 0);
    }
}

/* VA 0x0042e2f0, 1645 bytes, [I] row, status draft */
void bmxpropUpdate(void)
{
    /* Once per frame (action.c, after the riders): run the timers and moves of the 16 slots. */
    uint32_t i;

    for (i = 0; i < BMXPROP_SLOTS; i++) {
        BmxProp *p = &g_bmxprops.props[i];
        const BmxPropDef *d = &g_bmxpropTable[g_currentLevel][i];

        if (p->col == NULL)
            continue;
        switch (d->type) {
        case BMXPROP_MOVER:
            switch (p->state) {
            case BMXPROPST_IDLE:
                bmxpropIdle(p);
                break;
            case BMXPROPST_WAIT:
            case BMXPROPST_WAIT2:
                if (--p->timer == 0) {
                    bmxpropTrigger(p, p->player, 0);
                    p->state++;
                }
                break;
            case BMXPROPST_MOVING:
            case BMXPROPST_MOVING2:
                if (p->timer == -1 || p->timer-- != 0) {
                    zmathVecAdd((RwV3d *)p->pos, (const RwV3d *)p->pos, (const RwV3d *)p->vel);
                    zbspPropSetPosition(p->col, p->pos);
                    if (isMotion(d, BMXMOVE_KNOCK, BMXMOVE_TUMBLE)) {     /* knocked: slow down */
                        zmathVecScale((RwV3d *)p->vel, (const RwV3d *)p->vel, 0.94f);
                        p->spin[1] -= (int16_t)(p->spin[1] * DEGREE_PER_ANGLE * 0.06f * ANGLE_PER_DEGREE);
                    }
                    bmxpropSpin(p);
                    propRiderUpdate(p, d);
                    bmxpropStep(p);
                    if (d->sfxLoop != -1)
                        bmxsoundEvent(SFX_PROP, p->pos, 2, p->sound, 0x7F);
                } else {                                    /* the move is over */
                    p->state = BMXPROPST_IDLE;
                    if ((p->flags & BMXPROPF_MOVED) && p->toggled != 0) {
                        zbspPropSetMode(p->col, NGPROPCOL_SET_HIDE);
                        zbspPropSetMode(p->col, NGPROPCOL_SET_MOVING);
                        p->flags |= BMXPROPF_HIDDEN;
                    }
                    if (isMotion(d, BMXMOVE_SLIDE_L, BMXMOVE_DELAY4))
                        propSwitchLinks(p, 0);
                }
                break;
            }
            break;

        case BMXPROP_GOAL1:
        case BMXPROP_GOAL2:
        case BMXPROP_GOAL3:
            switch (p->state) {
            case BMXPROPST_WAIT:
            case BMXPROPST_WAIT2:
                if (--p->timer == 0) {
                    bmxpropTrigger(p, p->player, 0);
                    p->state++;
                    gameOnPropTouched(p->player, p);
                }
                break;
            case BMXPROPST_MOVING:
            case BMXPROPST_MOVING2:
                if (--p->timer == 0) {
                    if (d->type == BMXPROP_GOAL2 && p->state == BMXPROPST_MOVING) {
                        /* a timed gate: swing back after link[0] seconds */
                        p->state = BMXPROPST_WAIT2;
                        p->timer = (int32_t)p->link[0] * 30;
                        if (p->timer == 0)
                            p->timer = 1;
                    } else {
                        p->state = BMXPROPST_IDLE;
                    }
                } else {
                    if (d->motion == BMXMOVE_RISE) {
                        zmathVecAdd((RwV3d *)p->pos, (const RwV3d *)p->pos, (const RwV3d *)p->vel);
                        zbspPropSetPosition(p->col, p->pos);
                    }
                    bmxpropSpin(p);
                    if (d->sfxLoop != -1)
                        bmxsoundEvent(SFX_PROP, p->pos, 2, p->sound, 0x7F);
                }
                break;
            }
            break;

        case BMXPROP_SOUND:
        case BMXPROP_PAIR:
            if (d->sfxLoop != -1)
                bmxsoundEvent(SFX_PROP, p->pos, 2, p->sound, 0x7F);
            break;
        }
    }
}

/* VA 0x0042eb50, 3 bytes, [I] row, status draft */
int bmxpropDraw(void)
{
    /* per-frame draw callback (zrollerAddDrawCallback); empty in the release build */
    return 0;
}

/* VA 0x0042eb70, 300 bytes, [I] row, status draft */
void bmxpropSetState(const BmxPropState *state)
{
    /* Load a prop state (replay start / end, bmxpropReset) and make the world match it. */
    uint32_t i;
    BmxPropSwitch *sw;

    zstringMemCopy(&g_bmxprops, state, sizeof(BmxPropState));
    for (i = 0; i < BMXPROP_SLOTS; i++) {
        BmxProp *p = &g_bmxprops.props[i];
        uint8_t type = g_bmxpropTable[g_currentLevel][i].type;

        if (p->state == BMXPROPST_NONE || type == BMXPROP_SOUND)
            continue;
        if (type == BMXPROP_PAIR) {
            bmxpropShowPair(p, p->toggled);
            continue;
        }
        if (!(p->flags & BMXPROPF_HIDDEN)) {
            zbspPropSetMode(p->col, NGPROPCOL_SET_SHOW);
            zbspPropSetMode(p->col, NGPROPCOL_SET_STATIC);
        } else {
            zbspPropSetMode(p->col, NGPROPCOL_SET_HIDE);
            zbspPropSetMode(p->col, NGPROPCOL_SET_MOVING);
        }
        zbspPropSetPosition(p->col, p->pos);
        bmxpropRotate(p);
    }

    /* breakable props: an intact one shows its even-id model, a broken one the odd (id + 1) one */
    for (sw = g_bmxprops.switches; sw < &g_bmxprops.switches[BMXPROP_SWITCHES] && sw->prop != NULL; sw++) {
        const uint8_t *obji = sw->prop->info.obji;
        int odd = obji == NULL || (*(const uint16_t *)(obji + 8) & 1);
        int visible = (sw->flags & 1) ? odd : !odd;

        zobjSetActive(sw->prop->info.obj, visible);
    }
}

/* VA 0x0042ecf0, 12 bytes, [I] row, status draft */
void bmxpropReset(void)
{
    bmxpropSetState(&g_bmxpropsStart);
}

/* VA 0x0042ed10, 78 bytes, [I] row, status draft */
BmxProp *bmxpropGetPropByID(uint32_t id)
{
    /* the bound slot of the current level whose table id is `id` */
    uint32_t i;

    for (i = 0; i < BMXPROP_SLOTS; i++)
        if (g_bmxpropTable[g_currentLevel][i].id == id && g_bmxprops.props[i].state != BMXPROPST_NONE)
            return &g_bmxprops.props[i];
    g_pfnDebugPrintf("bmxpropGetPropByID: couldn't find prop %d\n", id);
    return NULL;
}

/* VA 0x0042ed80, 144 bytes, [I] row, status draft */
void bmxpropActivate(uint32_t player, BmxProp *prop)
{
    /* Trigger an idle prop, animated: a goal was completed (game.c), a linked mover finished, a
     * rider knocked it (bmxpropTouched). Goal-3 props and pairs only go one way. */
    const BmxPropDef *d = propDef(prop);

    if (prop->state != BMXPROPST_IDLE)
        return;
    if (d->type == BMXPROP_GOAL3) {
        if (prop->toggled != 0)
            return;
    } else if (d->type == BMXPROP_SOUND) {
        return;
    } else if (d->type == BMXPROP_PAIR) {
        if (prop->toggled != 0)
            return;
        bmxpropShowPair(prop, 1);
        if (d->sfxTrigger != -1)
            bmxsoundEvent(SFX_PROP, prop->pos, 3, d->sfxTrigger, 0x7F);
        return;
    }
    bmxpropTrigger(prop, player, 0);
    prop->player = (uint8_t)player;
    prop->state = BMXPROPST_MOVING;
}

/* VA 0x0042ee40, 91 bytes, [I] row, status draft */
void bmxpropActivateNow(uint32_t player, BmxProp *prop)
{
    /* Trigger a prop at once, without animating (game.c re-applies the props of goals already done
     * at the start of a run; finished movers switch their links this way when set instantly). */
    const BmxPropDef *d;

    if (prop->state == BMXPROPST_NONE)
        return;
    d = propDef(prop);
    if (d->type != BMXPROP_SOUND) {
        if (d->type == BMXPROP_PAIR)
            bmxpropShowPair(prop, 1);
        else
            bmxpropTrigger(prop, player, 1);
    }
    prop->player = (uint8_t)player;
    prop->state = BMXPROPST_IDLE;
}

/* VA 0x0042eec0, 113 bytes, [I] row, status draft */
int bmxpropTouched(uint32_t player, BmxProp *prop)
{
    /* physics.c: a rider touched the prop. Returns 0 when the rider goes through it (knock-over
     * movers 11/12), 1 when it is solid. */
    const BmxPropDef *d = propDef(prop);

    if (!gameOnPropTouched((int)player, prop)) {
        if (d->type != BMXPROP_MOVER)
            return 1;
        if (isMotion(d, BMXMOVE_KNOCK, BMXMOVE_TUMBLE))
            bmxpropActivate(player, prop);
    }
    if (d->type == BMXPROP_MOVER && isMotion(d, BMXMOVE_KNOCK, BMXMOVE_TUMBLE))
        return 0;
    return 1;
}

/* VA 0x0042ef60, 274 bytes, [I] row, status draft */
void bmxpropHitTrigger(uint32_t player, const NgColFace *face)
{
    /* physics.c: a wheel or the body went through a class-9 trigger face. Breaks the breakable
     * prop whose OBJI id equals the face code (window, fence): hide it, show id + 1, shards. */
    Zprop *whole = bmxpropFindProp(face->code);
    Zprop *broken = bmxpropFindProp(face->code + 1u);
    BmxPropSwitch *sw;
    RwV3d pos;

    if (whole == NULL)
        return;
    for (sw = g_bmxprops.switches; sw->prop != whole; sw++)
        if (sw + 1 >= &g_bmxprops.switches[BMXPROP_SWITCHES])
            return;
    if (sw->flags & 1)
        return;
    zobjSetActive(whole->info.obj, 0);
    if (broken != NULL) {
        zobjSetActive(broken->info.obj, 1);
        g_zrwModelSetFlags(*(void **)((uint8_t *)broken->info.obj + 0x2C), 8, 0);
    }
    pos = *(const RwV3d *)zobjGetPosition(whole->info.obj);
    fxShards(&pos, (const RwV3d *)g_riders[player].vel);
    bmxsoundEvent(0xFA2, &pos, 3, 3, 0x7F);
    gameOnObjectHit((int)player, (void *)face);
    sw->flags |= 1;
}

/* VA 0x0042f0d0, 1593 bytes, [I] row, status draft */
void bmxpropTrigger(BmxProp *prop, uint32_t player, int immediate)
{
    /* Start the slot's move (immediate == 0: set velocity / spin and a timer; bmxpropUpdate runs
     * it) or apply its end result at once (immediate != 0). `toggled` picks the direction. */
    const BmxPropDef *d = propDef(prop);
    RwV3d *right = &prop->matrix.right;
    int moving = 0;
    float dist;

    (void)player;
    if (d->type >= BMXPROP_GOAL1 && d->type <= BMXPROP_GOAL3) {
        switch (d->motion) {
        case BMXMOVE_RISE:                                  /* +-110 vertical over 60 frames */
            if (!immediate) {
                prop->timer = 60;
                prop->vel[1] = prop->toggled == 0 ? 1.8333334f : -1.8333334f;
            } else {
                prop->pos[1] += prop->toggled == 0 ? 110.0f : -110.0f;
            }
            prop->toggled ^= 1;
            break;
        case BMXMOVE_LIFT:                                  /* +-90 degrees about x over 60 frames */
            if (!immediate) {
                prop->timer = 60;
                prop->spin[0] = (int16_t)((prop->toggled ? -0x4000 : 0x4000) / 60);
            } else {
                prop->spin[0] = (int16_t)(prop->toggled ? -0x4000 : 0x4000);
            }
            prop->toggled ^= 1;
            break;
        case BMXMOVE_SWING_X:                               /* 30 degrees about y (0x5B x 60) */
            if (!immediate) {
                prop->spin[1] = 0x5B;
                prop->timer = 60;
            } else {
                prop->spin[1] = 0x1555;
            }
            prop->toggled++;
            break;
        }
        moving = d->type == BMXPROP_GOAL3;
    } else if (d->type == BMXPROP_MOVER) {
        switch (d->motion) {
        case BMXMOVE_RISE:
            zmathVecZero((RwV3d *)prop->vel);
            prop->flags |= BMXPROPF_MOVED;
            prop->toggled ^= 1;
            prop->timer = 1;
            break;
        case BMXMOVE_LIFT:                                  /* +-400 over 240 frames */
            if (!immediate) {
                prop->timer = 240;
                prop->vel[1] = prop->toggled == 0 ? -1.6666666f : 1.6666666f;
            } else {
                prop->pos[1] += prop->toggled == 0 ? -400.0f : 400.0f;
            }
            prop->toggled ^= 1;
            prop->flags |= BMXPROPF_MOVED;
            break;
        case BMXMOVE_SWING_X:
        case BMXMOVE_SWING_X2:
        case BMXMOVE_SWING_XN:                              /* +-90 degrees about x over 30 frames */
            if (!immediate) {
                prop->timer = 30;
                prop->spin[0] = (int16_t)((d->motion != BMXMOVE_SWING_XN ? 0x4000 : -0x4000) / 30);
            } else {
                prop->spin[0] = (int16_t)(d->motion != BMXMOVE_SWING_XN ? 0x4000 : -0x4000);
            }
            prop->toggled ^= 1;
            if (d->motion == BMXMOVE_SWING_X)
                prop->flags |= BMXPROPF_MOVED;
            break;
        case BMXMOVE_SWING_Z:
        case BMXMOVE_SWING_Z2:                              /* -90 degrees about z over 240 frames */
            /* the exe tests motion != 4 here, always true: both go the same way */
            if (!immediate) {
                prop->timer = 240;
                prop->spin[2] = (int16_t)(-0x4000 / 240);
            } else {
                prop->spin[2] = (int16_t)-0x4000;
            }
            prop->toggled ^= 1;
            break;
        case BMXMOVE_FALL:
        case BMXMOVE_FALL2:                                 /* runs along -z for ever */
            prop->vel[2] = -40.0f;
            prop->timer = -1;
            break;
        case BMXMOVE_BOB:                                   /* +-200 over 120 frames */
            if (!immediate) {
                prop->timer = 120;
                prop->vel[1] = prop->toggled == 0 ? -1.6666666f : 1.6666666f;
            } else {
                prop->pos[1] += prop->toggled == 0 ? -200.0f : 200.0f;
            }
            prop->toggled ^= 1;
            break;
        case BMXMOVE_KNOCK:
        case BMXMOVE_TUMBLE:                                /* pushed away by the rider */
            if (!immediate) {
                const RwV3d *up = &prop->matrix.up;
                const float *rv = g_riders[player].vel;
                float along = zmathVecDot((const RwV3d *)rv, up);

                prop->vel[0] = rv[0] - along * up->x;
                prop->vel[1] = rv[1] - along * up->y;
                prop->vel[2] = rv[2] - along * up->z;
                zmathVecScale((RwV3d *)prop->vel, (const RwV3d *)prop->vel, 1.25f);
                if (d->motion == BMXMOVE_KNOCK) {
                    prop->spin[1] = 0x5B0;
                    prop->timer = 60;
                } else {
                    prop->spin[0] = (int16_t)0xF99A;
                    prop->spin[1] = 0x444;
                    prop->spin[2] = 0x16C;
                    prop->timer = 15;
                }
            }
            prop->flags |= BMXPROPF_MOVED;
            prop->toggled ^= 1;
            moving = 1;
            break;
        case BMXMOVE_SLIDE_L:
        case BMXMOVE_SLIDE_S:                               /* along the right axis */
            dist = d->motion == BMXMOVE_SLIDE_L ? -1200.0f : -400.0f;
            if (prop->toggled != 0)
                dist = -dist;
            if (!immediate) {
                dist *= 1.0f / 150.0f;
                prop->timer = 150;
                prop->vel[0] = dist * right->x;
                prop->vel[1] = dist * right->y;
                prop->vel[2] = dist * right->z;
            } else {
                prop->pos[0] += dist * right->x;
                prop->pos[1] += dist * right->y;
                prop->pos[2] += dist * right->z;
            }
            prop->toggled ^= 1;
            break;
        case BMXMOVE_TRAVEL_L:
        case BMXMOVE_TRAVEL_3: {
            uint32_t next;

            dist = d->motion == BMXMOVE_TRAVEL_3 ? -1100.0f : -3200.0f;
            prop->timer = d->motion != BMXMOVE_TRAVEL_3 ? 360 : 90;
            if (prop->toggled != 0)
                dist = -dist;
            if (!immediate) {
                next = prop->toggled + 1;
                dist /= (float)(int16_t)prop->timer;
                prop->vel[0] = dist * right->x;
                prop->vel[1] = dist * right->y;
                prop->vel[2] = dist * right->z;
            } else {
                prop->pos[0] += dist * right->x;
                prop->pos[1] += dist * right->y;
                prop->pos[2] += dist * right->z;
                if (d->motion == BMXMOVE_TRAVEL_3) {        /* the turn and the second leg at once */
                    prop->spin[1] = 0x4000;
                    prop->toggled++;
                    bmxpropRotate(prop);
                    prop->spin[1] = 0;
                    prop->toggled++;
                    prop->pos[0] += right->x * -2400.0f;
                    prop->pos[1] += right->y * -2400.0f;
                    prop->pos[2] += right->z * -2400.0f;
                }
                next = prop->toggled + 1;
            }
            prop->toggled = next;
            break;
        }
        case BMXMOVE_DELAY1:
        case BMXMOVE_DELAY2:
        case BMXMOVE_DELAY3:
        case BMXMOVE_DELAY4:
            prop->timer = 60;
            prop->toggled ^= 1;
            break;
        }
    }

    /* every type: shown, collision on (one-way while knocked / goal 3), then the sound (animated)
     * or the end state (immediate) */
    zbspPropSetMode(prop->col, NGPROPCOL_SET_SHOW);
    zbspPropSetMode(prop->col, moving ? NGPROPCOL_SET_MOVING : NGPROPCOL_SET_STATIC);
    prop->flags &= ~BMXPROPF_HIDDEN;
    if (!immediate) {
        if (d->sfxTrigger != -1)
            bmxsoundEvent(SFX_PROP, prop->pos, 3, d->sfxTrigger, 0x7F);
        return;
    }
    zbspPropSetPosition(prop->col, prop->pos);
    bmxpropRotate(prop);
    if ((prop->flags & BMXPROPF_MOVED) && prop->toggled != 0) {
        zbspPropSetMode(prop->col, NGPROPCOL_SET_HIDE);
        zbspPropSetMode(prop->col, NGPROPCOL_SET_MOVING);
        prop->flags |= BMXPROPF_HIDDEN;
    }
    prop->timer = 0;
    if (isMotion(d, BMXMOVE_SLIDE_L, BMXMOVE_DELAY4))
        propSwitchLinks(prop, 1);
}

/* VA 0x0042f8f0, 33 bytes, [I] row, status draft */
void bmxpropSpin(BmxProp *prop)
{
    if (prop->spin[0] != 0 || prop->spin[1] != 0 || prop->spin[2] != 0)
        bmxpropRotate(prop);
}

/* VA 0x0042f930, 291 bytes, [I] row, status draft */
void bmxpropRotate(BmxProp *prop)
{
    /* apply one step of `spin` to the prop's matrix and hand the euler angles to its collision /
     * scene object */
    RwMatrix step, m;
    int16_t euler[3];
    float horiz;

    zmathMatrixIdentity(&step);
    zmathMatrixRotateXYZ(&step, &step, prop->spin);
    zmathMatrixMultiply(&prop->matrix, &prop->matrix, &step);
    zmathMatrixCopy(&m, &prop->matrix);
    horiz = (float)sqrt(m.right.x * m.right.x + m.right.y * m.right.y);
    if (horiz <= 0.001f) {                               /* gimbal lock */
        euler[0] = zmathAtan2(-m.at.y, m.up.y);
        euler[1] = zmathAtan2(-m.right.z, horiz);
        euler[2] = 0;
    } else {
        euler[0] = zmathAtan2(m.up.z, m.at.z);
        euler[1] = zmathAtan2(-m.right.z, horiz);
        euler[2] = zmathAtan2(m.right.y, m.right.x);
    }
    zbspPropSetRotation(prop->col, euler);
}

/* VA 0x0042fab0, 1 bytes, [I] row, status draft */
void bmxpropIdle(BmxProp *prop)
{
    /* idle movers: nothing (bmxpropUpdate calls it every frame) */
    (void)prop;
}

/* VA 0x0042fad0, 231 bytes, [I] row, status draft */
void bmxpropStep(BmxProp *prop)
{
    /* after each moving frame: wrap the traffic, run the stages of BMXMOVE_TRAVEL_3 */
    const BmxPropDef *d = propDef(prop);

    if (d->type != BMXPROP_MOVER || d->motion < BMXMOVE_FALL)
        return;
    if (d->motion < BMXMOVE_BOB) {
        if (prop->pos[2] < -8200.0f)
            prop->pos[2] += 12000.0f;
    } else if (d->motion == BMXMOVE_TRAVEL_3 && prop->timer == 0) {
        zmathVecZero((RwV3d *)prop->vel);
        prop->spin[0] = prop->spin[1] = prop->spin[2] = 0;
        if (prop->toggled == 1) {                        /* leg 1 done: turn about y for 90 frames */
            prop->timer = 90;
            prop->spin[1] = (int16_t)0xFF55;
            prop->toggled = 2;
        } else if (prop->toggled == 2) {                 /* turn done: 2400 along right in 270 frames */
            prop->timer = 270;
            prop->toggled = 3;
            prop->vel[0] = prop->matrix.right.x * 8.888889f;
            prop->vel[1] = prop->matrix.right.y * 8.888889f;
            prop->vel[2] = prop->matrix.right.z * 8.888889f;
        }
    }
}

/* VA 0x0042fc00, 162 bytes, [I] row, status draft */
void bmxpropShowPair(BmxProp *prop, uint32_t on)
{
    /* model pair: every prop with OBJI id == table id is shown when off, every one with id + 1 when on */
    uint16_t id = g_bmxpropTable[g_currentLevel][prop->index].id;
    Zprop *z;

    for (z = (Zprop *)zpropGetList()->head; z != NULL; z = (Zprop *)z->link.next) {
        uint16_t pid;

        if (z->info.obji == NULL)
            continue;
        pid = *(uint16_t *)(z->info.obji + 8);
        if (pid == id)
            zobjSetActive(z->info.obj, on == 0);
        else if (pid == id + 1u)
            zobjSetActive(z->info.obj, on != 0);
    }
    prop->toggled = on;
}

/*
 * zaxis/Bmx/Src/com/action/replay.c
 * Original: C:\zaxis\Bmx\Src\com\action\replay.c
 * Evidence: [V] path string 0x005587E4, referenced by the asserts of bmxreplayCreate, bmxreplayDestroy,
 *           bmxreplaySetMode, the four getters/setters and bmxreplayGetSaveArea (lines 0x14E-0x284);
 *           [V] the "bmxreplayUpdate:" / "bmxreplayCreate:" messages; [I] bmxreplayInit /
 *           bmxreplayShutdown (they only walk g_replays, between prop.c and bmxreplayUpdate);
 *           [I] bmxreplayEmpty1/2: two empty, unreferenced functions after the last assert of the
 *           file and before rider.c's first function (could as well be the start of rider.c).
 * 13 functions, 3381 bytes, VA 0x0042fce0-0x00430e50.
 * Draft. Non-matching C (AD-8). Types: replay.h. Format: docs/formats/REPLAY.md.
 *
 * The prop/replay/fx pass took in the prop/replay gap function (bmxreplayInit 0x0042FCE0) and created three
 * functions Ghidra had missed (decomp/symbols/symbols.csv rows): bmxreplayGetFrame 0x00430CF0 (reached only
 * through its ILT thunk, which nothing calls) and the two empty functions.
 *
 * A replay records one player's pad as an event stream (3 bytes per change: kind << 5 | frames
 * since the last event, then a u16) and keeps "start" and "live" snapshots of the rider, the props
 * and the goal progress. Playing it back restores the start snapshots and feeds the recorded pad
 * into the rider's BmxInput; stopping restores the live snapshots.
 */

#include "decomp.h"

#include "bmxprop.h"

#include "replay.h"

#define REPLAY_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\replay.c"
#define STICK_SCALE 127.0f              /* 0x0054D1E4 */
#define STICK_UNSCALE 0.007874016f      /* 0x0054D1E8 = 1 / 127 */
#define FRAMES_PER_SECOND 30

/* ------------------------------------------------------------------ other modules (current names) */
void      gameResetRider(int player, int newRun);                /* 0x0040C190 game.c */
/* zactorGetPos / SetPos / GetHeight / SetHeight / SetMatrix: zactor.h */

/* ------------------------------------------------------------------ globals (this file) */
BmxReplay *g_replays[REPLAY_MAX];       /* 0x0056D278 */

/* ------------------------------------------------------------------ helpers */
#define AT(rep, off) ((uint8_t *)(rep) + (off))

static BmxReplay *replayGet(uint8_t handle, int lineRange, int lineNull)
{
    BmxReplay *rep;

    if (handle >= REPLAY_MAX)
        zAssertFail("invalid replay handle", REPLAY_SRC, lineRange, NULL);
    rep = g_replays[handle];
    if (rep == NULL)
        zAssertFail("invalid handle passed or replay has been destroyed", REPLAY_SRC, lineNull, NULL);
    return rep;
}

/* the goal-info block is only kept in proquest, on the six challenge levels */
static int replayKeepsGoals(const BmxReplay *rep)
{
    return !(rep->flags & REPLAYF_NO_GOALS) && gameGetMode() == 0 && gameGetSubMode() == 0 &&
           g_currentLevel < 6;
}

/* room for one more 3-byte event? (the end test is "cursor < data end - 3") */
static int replayHasRoom(const BmxReplay *rep)
{
    return rep->cursor < AT(rep, rep->dataOffset + rep->dataSize - 3);
}

static void replayPutEvent(BmxReplay *rep, uint8_t head, uint16_t value)
{
    rep->cursor[0] = head;
    rep->cursor[1] = (uint8_t)value;
    rep->cursor[2] = (uint8_t)(value >> 8);
    rep->delta = 0;
    rep->cursor += 3;
}

static void replaySaveRider(BmxReplay *rep, uint32_t off, uint32_t slot, const BmxPad *pad)
{
    BmxRider *r = &g_riders[slot];
    BmxReplayRider *s = (BmxReplayRider *)AT(rep, off);
    const RwV3d *p;

    zstringMemCopy(s->rider, r, 0xF70);
    zstringMemCopy(s->riderActor, r->riderActor, 0x278);
    zstringMemCopy(s->actorData, *(void **)((uint8_t *)r->riderActor + 0x48), 0xD60);
    zstringMemCopy(s->dynamics, r->animCtl, 0xE88);
    p = zactorGetPos((ZActor *)r->bikeActor);           /* (the exe calls it once per component) */
    s->bikePos[0] = p->x;
    s->bikePos[1] = zactorGetPos((ZActor *)r->bikeActor)->y;
    s->bikePos[2] = zactorGetPos((ZActor *)r->bikeActor)->z;
    s->bikeHeight = zactorGetHeight((ZActor *)r->bikeActor);
    s->buttons = pad->input.buttons;
}

/* restore a rider snapshot. NB: the restore after playback (bmxreplaySetMode leaving 2/4) skips the
 * rag-doll block; only the restore of the start snapshot (entering playback) copies it back. */
static void replayLoadRider(BmxReplay *rep, uint32_t off, uint32_t slot, int withDynamics)
{
    BmxRider *r = &g_riders[slot];
    BmxReplayRider *s = (BmxReplayRider *)AT(rep, off);

    zstringMemCopy(r, s->rider, 0xF70);
    zstringMemCopy(r->riderActor, s->riderActor, 0x278);
    zstringMemCopy(*(void **)((uint8_t *)r->riderActor + 0x48), s->actorData, 0xD60);
    if (withDynamics)
        zstringMemCopy(r->animCtl, s->dynamics, 0xE88);
    zactorSetMatrix((ZActor *)r->bikeActor, (const RwMatrix *)r->bikeMatrix);
    zactorSetPos((ZActor *)r->bikeActor, (const RwV3d *)s->bikePos);
    zactorSetHeight((ZActor *)r->bikeActor, s->bikeHeight);
}

/* ------------------------------------------------------------------ functions */

/* VA 0x0042fce0, 28 bytes, [I] row, status draft */
void bmxreplayInit(void)
{
    uint32_t i;

    for (i = 0; i < REPLAY_MAX; i++)
        g_replays[i] = NULL;
}

/* VA 0x0042fd10, 38 bytes, [I] row, status draft */
void bmxreplayShutdown(void)
{
    uint32_t i;

    for (i = 0; i < REPLAY_MAX; i++)
        if (g_replays[i] != NULL)
            bmxreplayDestroy((uint8_t)i);
}

/* VA 0x0042fd50, 840 bytes, [I] row, status draft */
void bmxreplayUpdate(void)
{
    /* Once per simulated frame (action.c), for every replay: record the pad of its player, or play
     * the stream back into the rider's input. */
    uint32_t n, i;

    for (n = 0; n < REPLAY_MAX; n++) {
        BmxReplay *rep = g_replays[n];

        if (rep == NULL)
            continue;

        if (rep->mode == REPLAY_RECORD) {
            BmxPad *pad;

            rep->delta++;
            rep->frame++;
            rep->frames++;
            pad = contrlerGetPad(g_saveProfiles[rep->player].index);
            if (pad->flags & BMXPAD_DISCONNECTED)
                continue;

            /* buttons: an event when they change, or a keep-alive when the delta field is full */
            if (pad->input.buttons != rep->buttons || rep->delta == REPLAY_DELTA_MAX) {
                if (replayHasRoom(rep)) {
                    replayPutEvent(rep, (uint8_t)(REPLAY_EV_BUTTONS << 5 | rep->delta),
                                   (uint16_t)(rep->buttons ^ pad->input.buttons));
                } else {
                    g_pfnDebugPrintf("bmxreplayUpdate: reached end of replay buffer (replay %d, %d seconds)\n",
                                     n, rep->frames / FRAMES_PER_SECOND);
                    bmxreplaySetMode((uint8_t)n, REPLAY_OFF);
                }
                rep->buttons = pad->input.buttons;
            }

            /* sticks: x and y x 127 as s8; the u16 is (x << 8) | y */
            for (i = 0; i < 2; i++) {
                int8_t x = (int8_t)(int)(pad->input.stick[i][0] * STICK_SCALE);
                int8_t y = (int8_t)(int)(pad->input.stick[i][1] * STICK_SCALE);
                uint16_t v = (uint16_t)((uint8_t)x << 8 | (uint8_t)y);

                if (v == *(uint16_t *)rep->stick[i])
                    continue;
                if (replayHasRoom(rep)) {
                    replayPutEvent(rep, (uint8_t)((i + 1) << 5 | rep->delta), v);
                } else {
                    g_pfnDebugPrintf("bmxreplayUpdate: reached end of replay buffer (replay %d, %d seconds)\n",
                                     n, rep->frames / FRAMES_PER_SECOND);
                    bmxreplaySetMode((uint8_t)n, REPLAY_OFF);
                }
                *(uint16_t *)rep->stick[i] = v;
            }
        } else if (rep->mode == REPLAY_PLAY) {
            BmxInput *in = &g_riders[rep->player].input;
            uint32_t changed = 0;

            if (rep->frames == 0)
                continue;
            for (i = 0; i < 2; i++) {               /* last frame's sticks */
                in->prevStick[i][0] = rep->stick[i][1] * STICK_UNSCALE;
                in->prevStick[i][1] = rep->stick[i][0] * STICK_UNSCALE;
            }
            /* apply every event that falls on this frame (delta 0 chains events) */
            rep->delta--;
            while (rep->delta == 0 && replayHasRoom(rep)) {
                uint8_t kind = (uint8_t)(rep->cursor[0] >> 5);
                uint16_t v = (uint16_t)(rep->cursor[1] | rep->cursor[2] << 8);

                if (kind == REPLAY_EV_BUTTONS) {
                    changed = v;
                    rep->cursor += 3;
                } else if (kind <= REPLAY_EV_STICK1) {
                    *(uint16_t *)rep->stick[kind - 1] = v;
                    rep->cursor += 3;
                } else {
                    rep->cursor += 1;               /* kinds 3..7: not written by this recorder */
                }
                rep->delta = rep->cursor[0] & REPLAY_DELTA_MASK;
            }
            in->changed = changed;
            rep->buttons ^= changed;
            in->buttons = rep->buttons;
            for (i = 0; i < 2; i++) {
                in->stick[i][0] = rep->stick[i][1] * STICK_UNSCALE;
                in->stick[i][1] = rep->stick[i][0] * STICK_UNSCALE;
            }
            if (++rep->frame == rep->frames)
                bmxreplaySetMode((uint8_t)n, REPLAY_END);
        }
    }
}

/* VA 0x00430180, 391 bytes, [V] direct, status draft */
uint8_t bmxreplayCreate(const BmxReplayDesc *desc)
{
    /* Take a free handle and allocate header + stream + the snapshot blocks the flags keep. */
    uint8_t h;

    for (h = 0; h < REPLAY_MAX; h++) {
        int32_t mark;
        uint32_t size, data, off;
        BmxReplay *rep;

        if (g_replays[h] != NULL)
            continue;
        mark = zmemTempMark();
        size = REPLAY_HEADER;
        if (!(desc->flags & REPLAYF_NO_RIDER))
            size = REPLAY_HEADER + 2 * REPLAY_RIDER_SIZE;       /* 0x5C10 */
        if (!(desc->flags & REPLAYF_NO_PROPS))
            size += 2 * REPLAY_PROPS_SIZE;
        if (!(desc->flags & REPLAYF_NO_GOALS))
            size += 2 * REPLAY_GOALS_SIZE;
        data = desc->size == 0 ? REPLAY_DEFAULT_DATA : (desc->size + 3) & ~3u;
        rep = (BmxReplay *)zmemTempAlloc(size + data);
        if (rep == NULL) {
            zAssertFail("bmxreplayCreate: failed to allocate buffer\n", REPLAY_SRC, 0x14E, NULL);
            continue;                                           /* (assert returns: try the next handle) */
        }
        rep->tempMark = mark;
        rep->flags = desc->flags;
        rep->player = desc->player;
        rep->mode = REPLAY_OFF;
        rep->frames = 0;
        rep->dataSize = data;
        rep->dataOffset = REPLAY_HEADER;
        rep->riderStart = rep->riderLive = 0;
        rep->propsStart = rep->propsLive = 0;
        off = REPLAY_HEADER + data;
        if (!(rep->flags & REPLAYF_NO_RIDER)) {
            rep->riderStart = off;
            rep->riderLive = off + REPLAY_RIDER_SIZE;
            off += 2 * REPLAY_RIDER_SIZE;
        }
        if (!(rep->flags & REPLAYF_NO_PROPS)) {
            rep->propsStart = off;
            rep->propsLive = off + REPLAY_PROPS_SIZE;
            off += 2 * REPLAY_PROPS_SIZE;
            zstringMemCopy(AT(rep, rep->propsStart), &g_bmxprops, REPLAY_PROPS_SIZE);
        }
        if (!(rep->flags & REPLAYF_NO_GOALS)) {
            rep->goalsStart = off;
            rep->goalsLive = off + REPLAY_GOALS_SIZE;
            if (gameGetMode() == 0 && gameGetSubMode() == 0 && g_currentLevel < 6)
                zstringMemCopy(AT(rep, rep->goalsStart), comdataGetGoalInfo(rep->player, g_currentLevel),
                               REPLAY_GOALS_SIZE);
        }
        g_replays[h] = rep;
        return h;
    }
    return 0xFF;
}

/* VA 0x00430380, 106 bytes, [V] direct, status draft */
void bmxreplayDestroy(uint8_t handle)
{
    BmxReplay *rep = replayGet(handle, 0x15F, 0x161);

    zmemTempRelease(rep->tempMark);
    g_replays[handle] = NULL;
}

/* VA 0x00430410, 1501 bytes, [V] direct, status draft */
uint8_t bmxreplaySetMode(uint8_t handle, uint32_t mode)
{
    /* Switch between off / record / play / end, saving and restoring the snapshots:
     *   -> record:  start snapshots := now (props, goals, rider); stream rewound.
     *   -> play:    (coming from off / record) live snapshots := now; then load the start snapshots
     *               and rewind the stream. A replay without frames switches off instead.
     *   play/end -> off/record: load the live snapshots (back to where the run was). */
    BmxReplay *rep = replayGet(handle, 0x175, 0x177);
    uint32_t slot = rep->player;
    BmxPad *pad = contrlerGetPad(g_saveProfiles[slot].index);

    if (mode == rep->mode)
        return rep->mode;

    if (rep->mode == REPLAY_RECORD) {
        /* close the stream: one last event (mask 0) carrying the frames since the previous one */
        if (replayHasRoom(rep))
            replayPutEvent(rep, (uint8_t)(rep->delta + 1), 0);
    } else if ((rep->mode == REPLAY_PLAY || rep->mode == REPLAY_END) && mode < REPLAY_PLAY) {
        if (!(rep->flags & REPLAYF_NO_RIDER))
            replayLoadRider(rep, rep->riderLive, slot, 0);
        if (!(rep->flags & REPLAYF_NO_PROPS))
            bmxpropSetState((const BmxPropState *)AT(rep, rep->propsLive));
        if (replayKeepsGoals(rep))
            zstringMemCopy(comdataGetGoalInfo(rep->player, g_currentLevel), AT(rep, rep->goalsLive),
                           REPLAY_GOALS_SIZE);
    }

    if (mode == REPLAY_RECORD) {
        rep->controlConfig = g_riders[slot].profileByte2;
        if (!(rep->flags & REPLAYF_NO_PROPS))
            zstringMemCopy(AT(rep, rep->propsStart), &g_bmxprops, REPLAY_PROPS_SIZE);
        if (replayKeepsGoals(rep))
            zstringMemCopy(AT(rep, rep->goalsStart), comdataGetGoalInfo(rep->player, g_currentLevel),
                           REPLAY_GOALS_SIZE);
        if (!(rep->flags & REPLAYF_NO_RIDER)) {
            replaySaveRider(rep, rep->riderStart, slot, pad);
            rep->buttons = pad->input.buttons;
        } else {
            rep->buttons = 0;
        }
        *(uint32_t *)rep->stick = 0;
        rep->frame = 0;
        rep->cursor = AT(rep, rep->dataOffset);
        rep->frames = 0;
        rep->delta = 0;
    } else if (mode == REPLAY_PLAY) {
        if (rep->frames == 0) {
            rep->mode = REPLAY_OFF;
            return REPLAY_OFF;
        }
        g_riders[slot].profileByte2 = (uint8_t)rep->controlConfig;
        if (rep->mode < REPLAY_PLAY) {                  /* keep the live state for afterwards */
            if (!(rep->flags & REPLAYF_NO_PROPS))
                zstringMemCopy(AT(rep, rep->propsLive), &g_bmxprops, REPLAY_PROPS_SIZE);
            if (replayKeepsGoals(rep))
                zstringMemCopy(AT(rep, rep->goalsLive), comdataGetGoalInfo(rep->player, g_currentLevel),
                               REPLAY_GOALS_SIZE);
        }
        if (!(rep->flags & REPLAYF_NO_PROPS))
            bmxpropSetState((const BmxPropState *)AT(rep, rep->propsStart));
        if (replayKeepsGoals(rep))
            zstringMemCopy(comdataGetGoalInfo(rep->player, g_currentLevel), AT(rep, rep->goalsStart),
                           REPLAY_GOALS_SIZE);
        if (!(rep->flags & REPLAYF_NO_RIDER)) {
            if (rep->mode < REPLAY_PLAY)
                replaySaveRider(rep, rep->riderLive, slot, pad);
            replayLoadRider(rep, rep->riderStart, slot, 1);
            rep->buttons = ((BmxReplayRider *)AT(rep, rep->riderStart))->buttons;
        } else {
            gameResetRider(rep->player, 1);             /* no snapshot: start the rider afresh */
            rep->buttons = 0;
        }
        *(uint32_t *)rep->stick = 0;
        rep->frame = 0;
        rep->cursor = AT(rep, rep->dataOffset);
        rep->mode = REPLAY_PLAY;
        rep->delta = rep->cursor[0] & REPLAY_DELTA_MASK;
        return REPLAY_PLAY;
    }
    rep->mode = (uint8_t)mode;
    return rep->mode;
}

/* VA 0x00430b70, 83 bytes, [V] direct, status draft */
uint8_t bmxreplayGetMode(uint8_t handle)
{
    return replayGet(handle, 0x23E, 0x240)->mode;
}

/* VA 0x00430bf0, 90 bytes, [V] direct, status draft */
uint8_t bmxreplaySetPlayer(uint8_t handle, uint8_t player)
{
    replayGet(handle, 0x24E, 0x250)->player = player;
    return player;
}

/* VA 0x00430c70, 83 bytes, [V] direct, status draft */
uint8_t bmxreplayGetPlayer(uint8_t handle)
{
    return replayGet(handle, 0x25F, 0x261)->player;
}

/* VA 0x00430cf0, 81 bytes, [I] row, status draft */
uint32_t bmxreplayGetFrame(uint8_t handle)
{
    /* no caller in the exe (only its ILT thunk 0x00401686 exists) */
    return replayGet(handle, 0x26F, 0x271)->frame;
}

/* VA 0x00430d70, 138 bytes, [V] direct, status draft */
void bmxreplayGetSaveArea(uint8_t handle, void **start, uint32_t *size)
{
    /* The part of the buffer that is saved / loaded (action.c loads replay.zal records here): from
     * header +0x0C (controlConfig) to the end of the last block the flags keep. */
    BmxReplay *rep = replayGet(handle, 0x282, 0x284);

    *start = AT(rep, REPLAY_SAVE_START);
    *size = rep->dataSize + (REPLAY_HEADER - REPLAY_SAVE_START);
    if (!(rep->flags & REPLAYF_NO_RIDER))
        *size = rep->dataSize + (REPLAY_HEADER - REPLAY_SAVE_START) + 2 * REPLAY_RIDER_SIZE;  /* 0x5C04 */
    if (!(rep->flags & REPLAYF_NO_PROPS))
        *size += 2 * REPLAY_PROPS_SIZE;
    if (!(rep->flags & REPLAYF_NO_GOALS))
        *size += 2 * REPLAY_GOALS_SIZE;
}

/* VA 0x00430e30, 1 bytes, [I] row, status draft */
void bmxreplayEmpty1(void)
{
    /* empty; nothing references it (ILT thunk 0x004024DC only) */
}

/* VA 0x00430e50, 1 bytes, [I] row, status draft */
void bmxreplayEmpty2(void)
{
    /* empty; nothing references it (ILT thunk 0x0040181B only) */
}

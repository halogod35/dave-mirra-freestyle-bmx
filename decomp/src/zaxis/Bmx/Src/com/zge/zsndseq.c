/*
 * zaxis/Bmx/Src/com/zge/zsndseq.c
 * Original: C:\zaxis\Bmx\src\com\zge\zsndseq.c
 * Evidence: [V] path string 0x0055FEC8 (CRT assert()s "hTrack < kzsndSeqMaxTracks", "track",
 *           "Track[hTrack]", "noteEvent->flags & kzsndNoteEventFlagActive") in 13 functions;
 *           [I] the rest: between them, sharing the module's data 0x0064EB38-0x0064F9CB.
 *           zsndSeqInit (0x0047C4C0, the zactor/zprim pass's "probably zsndseq.c") clears exactly that data.
 * 28 functions, VA 0x0047C4C0-0x0047DE06. Draft. Non-matching C (AD-8).
 * Types: zsound.h. ZTRK byte code: docs/formats/AUDIO.md section 3.1; dmbmx.formats.audio has
 * a decoder/encoder (`dmbmx audio ztrk-dump`).
 *
 * The sequencer plays ZTRK tracks (table E of a bank): 32 tracks, 64 sounding notes.
 * zsndSeqUpdate runs once per game frame (g_zsndUpdateRate = 30 Hz): each track's clock advances
 * by 1000/30 ms (20.12 fixed), and every event whose delta time has passed is decoded and handled:
 *  - note (opcode 0x00 / 0x08): the track's voice at (note - 60) semitones, or the voice the
 *    keymap gives; volume (track volume x velocity x expression); the event's length field is the
 *    note's duration in ms, converted to frames, after which a key-off releases the channel;
 *  - control (0x10): 0 expression, 1 pan, 4 voice, 5 keymap;
 *  - marker (0x18): 1 = end of track (stop, or restart if looping); the track callback sees all;
 *  - jump (0x30): continue at start + u32.
 * The game uses it only for the level ambience (bmxsound: every track of SFXLEVnn.ZSD, started
 * with the level, paused with the game). The sound pass created zsndSeqStop, zsndSeqGroupMute,
 * zsndSeqGetState, zsndSeqSetMode, zsndSeqSetLoop, zsndSeqPlayTrackDef, zsndSeqSetCallback (ILT
 * thunks only) and the four event handlers (reached through ZsndSeqEvent.handler).
 */
#include "decomp.h"
#include "zsound.h"

#define ZSNDSEQ_FILE "C:\\zaxis\\Bmx\\src\\com\\zge\\zsndseq.c"
#define ASSERT_AT(cond, text, line) do { if (!(cond)) _assert((text), ZSNDSEQ_FILE, (line)); } while (0)
#define TRACK_CHECK(h, l)                                                 \
    ASSERT_AT((h) < ZSND_SEQ_MAX_TRACKS, "hTrack < kzsndSeqMaxTracks", (l))
#define TRACK_MODE(t)  ((t)->flags & ZSND_TRK_MODE_MASK)
#define NOTE_VOLUME(t, n) ((uint8_t)((((t)->volume * (n)->velocity >> 7) & 0xFF) * (t)->expression >> 7))

/* ------------------------------------------------------------------ globals */
ZsndSeqNote   g_zsndSeqNotes[ZSND_SEQ_MAX_NOTES];        /* 0x0064EB38 */
uint32_t      g_zsndSeqNextNote;                         /* 0x0064F038 */
int32_t       g_zsndSeqGroupMuteOn;                      /* 0x0064F03C */
uint32_t      g_zsndSeqNextTrack;                        /* 0x0064F040 */
ZsndSeqTrack  g_zsndSeqTrackPool[ZSND_SEQ_MAX_TRACKS];   /* 0x0064F048 */
ZsndSeqTrack *g_zsndSeqTracks[ZSND_SEQ_MAX_TRACKS];      /* 0x0064F948 "Track[]": active tracks */
int32_t       g_zsndSeqLock;                             /* 0x0064F9C8 > 0: zsndSeqUpdate skips */
extern uint32_t g_zsndUpdateRate;                        /* zsndsrc.c 0x0055FFC0 */

/* VA 0x0047c4c0, 77 bytes, [I] row, status draft */
int zsndSeqInit(void)
{
    int i;

    for (i = 0; i < ZSND_SEQ_MAX_TRACKS; i++)
        g_zsndSeqTracks[i] = 0;
    for (i = 0; i < ZSND_SEQ_MAX_TRACKS; i++)
        g_zsndSeqTrackPool[i].flags &= ~ZSND_TRK_ALLOC;
    g_zsndSeqNextTrack = 0;
    for (i = 0; i < ZSND_SEQ_MAX_NOTES; i++)
        g_zsndSeqNotes[i].flags = 0;
    g_zsndSeqNextNote = 0;
    g_zsndSeqGroupMuteOn = 0;
    return 0;
}

/* VA 0x0047c530, 52 bytes, [I] row, status draft */
int zsndSeqShutdown(void)
{
    uint8_t h;

    for (h = 0; h < ZSND_SEQ_MAX_TRACKS; h++)
        if (g_zsndSeqTracks[h])
            zsndSeqFreeTrack(h);
    return 0;
}

/* VA 0x0047c580, 235 bytes, [I] row, status draft
 * Once per game frame (bmxsoundUpdate). */
int zsndSeqUpdate(void)
{
    ZsndSeqEvent ev;            /* [V] .handler is left uninitialised for the opcodes without one
                                   (0x20, 0x28, 0x38, 0x40): a stale stack value gets called */
    ZsndSeqNote *n;
    uint32_t     left, wait;
    uint8_t      h;

    if (g_zsndSeqLock)
        return 0;
    for (h = 0; h < ZSND_SEQ_MAX_TRACKS; h++) {
        ZsndSeqTrack *t = g_zsndSeqTracks[h];
        if (!t)
            continue;
        /* notes: a delta list, only the head counts down */
        if (t->notes)
            t->notes->ticks--;
        for (n = t->notes; n && n->ticks == 0; n = n->next)
            zsndSeqNoteOff(n);
        t->notes = n;
        if (!zsndSeqIsRunning(t))
            continue;
        /* events */
        left = t->msPerTick;
        wait = t->wait;
        if (wait <= left) {
            do {
                left -= t->wait;
                zsndSeqDecodeEvent(t, &ev);
                if (ev.handler)
                    ev.handler(&ev);
                if (!zsndSeqIsRunning(t)) {
                    if (t->flags & ZSND_TRK_AUTOFREE)
                        zsndSeqFreeTrack(h);
                    break;
                }
                t->wait = zsndSeqPeekDelta(t) << 12;
            } while (t->wait <= left);
            wait = t->wait;
        }
        t->wait = wait - left;
    }
    return 0;
}

/* VA 0x0047c6b0, 339 bytes, [I] row, status draft
 * A stopped track for a track def; flags: ZSND_TRK_AUTOFREE etc. (the mode bits are ignored). */
uint8_t zsndSeqCreateTrack(ZsndTrackDef *def, uint16_t flags)
{
    ZsndSeqTrack *t;
    uint32_t      i;

    g_zsndSeqLock++;
    i = g_zsndSeqNextTrack;
    while (g_zsndSeqTrackPool[i].flags & ZSND_TRK_ALLOC) {
        if (++i == ZSND_SEQ_MAX_TRACKS)
            i = 0;
        if (i == g_zsndSeqNextTrack) {
            g_zsndSeqLock--;
            return 0xFF;
        }
    }
    t = &g_zsndSeqTrackPool[i];
    t->flags = ZSND_TRK_ALLOC;
    g_zsndSeqNextTrack = i + 1 == ZSND_SEQ_MAX_TRACKS ? 0 : i + 1;
    t->flags |= flags & 0xFFFC;
    t->state = ZSND_TRK_STOPPED;
    t->start = def->ztrk + 4;                         /* after the "ZTRK" tag */
    t->pc = def->ztrk + 4;
    t->msPerTick = 0x3E8000 / g_zsndUpdateRate;       /* (1000 << 12) / 30 */
    t->notes = 0;
    t->ticksPerMs = (g_zsndUpdateRate << 12) / 1000;
    t->velocity = 0x7F;
    t->volume = def->volume;
    t->expression = 0x7F;
    t->def = def;
    t->pan = def->pan;
    /* [V] the default voice / keymap are looked up in the bank of the slot's previous track:
     * t->bank is only set below (harmless: every track def in the game has 0xFF / 0xFF) */
    t->voice = def->voice == 0xFF ? 0 : zsndGetVoice(t->bank, def->voice);
    t->keymap = def->keymap == 0xFF ? 0 : zsndGetKeymap(t->bank, def->keymap);
    t->bank = def->bank;
    g_zsndSeqTracks[i] = t;
    g_zsndSeqLock--;
    return (uint8_t)i;
}

/* VA 0x0047c870, 156 bytes, [V] direct, status draft */
int zsndSeqFreeTrack(uint8_t hTrack)
{
    ZsndSeqTrack *t;
    ZsndSeqNote  *n;

    g_zsndSeqLock++;
    TRACK_CHECK(hTrack, 0x143);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x145);
    for (n = t->notes; n; n = n->next)
        zsndSeqNoteOff(n);
    g_zsndSeqNextTrack = hTrack;
    g_zsndSeqTracks[hTrack] = 0;
    g_zsndSeqTrackPool[hTrack].flags = 0;
    g_zsndSeqLock--;
    return 0;
}

static void stopNotes(ZsndSeqTrack *t)
{
    ZsndSeqNote *n;

    for (n = t->notes; n; n = n->next)
        zsndSeqNoteOff(n);
    t->notes = 0;
}

/* VA 0x0047c940, 195 bytes, [V] direct, status draft
 * Start from the top, or resume a paused track (its notes restart). */
int zsndSeqPlay(uint8_t hTrack)
{
    ZsndSeqTrack *t;
    ZsndSeqNote  *n;

    g_zsndSeqLock++;
    TRACK_CHECK(hTrack, 0x162);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x164);
    if (t->state == ZSND_TRK_PAUSED) {
        for (n = t->notes; n; n = n->next)
            if (!(n->flags & ZSND_NOTE_SILENT))
                zsndcKeyOn(n->channel);
    } else {
        stopNotes(t);
        t->pc = t->start;
        t->wait = zsndSeqPeekDelta(t) << 12;
    }
    t->state = ZSND_TRK_PLAYING;
    g_zsndSeqLock--;
    return 0;
}

/* VA 0x0047ca50, 172 bytes, [I] row, status draft
 * Mode 0: notes off now; mode 1: let them ring; 2/3: defer (ZSND_TRK_PENDING). */
int zsndSeqStop(uint8_t hTrack)
{
    ZsndSeqTrack *t;

    g_zsndSeqLock++;
    TRACK_CHECK(hTrack, 0x189);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x18B);
    if (zsndSeqIsRunning(t)) {
        if (TRACK_MODE(t) == 0)
            stopNotes(t);
        else if (TRACK_MODE(t) != 1)
            t->flags |= ZSND_TRK_PENDING;
    }
    t->state = ZSND_TRK_STOPPED;
    g_zsndSeqLock--;
    return 0;
}

/* VA 0x0047cb30, 179 bytes, [V] direct, status draft */
int zsndSeqPause(uint8_t hTrack)
{
    ZsndSeqTrack *t;
    ZsndSeqNote  *n;

    g_zsndSeqLock++;
    TRACK_CHECK(hTrack, 0x1AF);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x1B1);
    if (zsndSeqIsRunning(t)) {
        if (TRACK_MODE(t) == 0) {
            for (n = t->notes; n; n = n->next)
                if (!(n->flags & ZSND_NOTE_SILENT))
                    zsndcKeyOff(n->channel, 0);       /* release; zsndSeqPlay keys them on again */
        } else if (TRACK_MODE(t) != 1) {
            t->flags |= ZSND_TRK_PENDING;
        }
    }
    t->state = ZSND_TRK_PAUSED;
    g_zsndSeqLock--;
    return 0;
}

/* VA 0x0047cc20, 415 bytes, [V] direct, status draft */
int zsndSeqMute(uint8_t hTrack, int mute)
{
    ZsndSeqTrack *t;
    ZsndSeqNote  *n;

    g_zsndSeqLock++;
    TRACK_CHECK(hTrack, 0x1D5);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x1D7);
    if (!mute) {
        if (TRACK_MODE(t) < 2) {
            /* give the silent notes a channel again */
            for (n = t->notes; n; n = n->next) {
                if (!(n->flags & ZSND_NOTE_SILENT))
                    continue;
                n->channel = zsndcAlloc(n->voice, 0xFF);
                if (n->channel == 0xFF)
                    continue;
                zsndcSetPitch(n->channel, n->cents);
                zsndcSetVolume(n->channel, NOTE_VOLUME(t, n));
                zsndcSetPan(n->channel, t->pan);
                zsndcKeyOn(n->channel);
                n->flags &= ~ZSND_NOTE_SILENT;
            }
        } else {
            t->flags |= ZSND_TRK_PENDING;
        }
        t->flags &= ~ZSND_TRK_MUTED;
        g_zsndSeqLock--;
        return 0;
    }
    if (zsndSeqIsRunning(t)) {
        if (TRACK_MODE(t) == 0) {
            for (n = t->notes; n; n = n->next) {
                if (n->flags & ZSND_NOTE_SILENT)
                    continue;
                zsndcKeyOff(n->channel, 0);
                zsndcFree(n->channel);
                n->flags |= ZSND_NOTE_SILENT;
            }
        } else if (TRACK_MODE(t) != 1) {
            t->flags |= ZSND_TRK_PENDING;
        }
    }
    t->flags |= ZSND_TRK_MUTED;
    g_zsndSeqLock--;
    return 0;
}

/* VA 0x0047ce30, 176 bytes, [I] row, status draft
 * Mark a track as one that goes silent while the group mute is on (new notes only). */
int zsndSeqGroupMute(uint8_t hTrack, int mute)
{
    ZsndSeqTrack *t;

    TRACK_CHECK(hTrack, 0x214);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x216);
    if (!mute) {
        if (TRACK_MODE(t) > 1)
            t->flags |= ZSND_TRK_PENDING;
        t->flags &= ~ZSND_TRK_GROUPMUTE;
        g_zsndSeqGroupMuteOn = 0;
        return 0;
    }
    if (zsndSeqIsRunning(t) && TRACK_MODE(t) > 1)
        t->flags |= ZSND_TRK_PENDING;
    t->flags |= ZSND_TRK_GROUPMUTE;
    g_zsndSeqGroupMuteOn = 1;
    return 0;
}

/* VA 0x0047cf10, 145 bytes, [I] row, status draft
 * 1 stopped, 2 playing (or a deferred operation pending), 3 paused, 4 other. */
int zsndSeqGetState(uint8_t hTrack)
{
    TRACK_CHECK(hTrack, 0x23E);
    ASSERT_AT(g_zsndSeqTracks[hTrack], "Track[hTrack]", 0x23F);
    if (zsndSeqIsRunning(g_zsndSeqTracks[hTrack]))
        return 2;
    if (g_zsndSeqTracks[hTrack]->state == ZSND_TRK_STOPPED)
        return 1;
    if (g_zsndSeqTracks[hTrack]->state == ZSND_TRK_PAUSED)
        return 3;
    return 4;
}

/* VA 0x0047cfe0, 178 bytes, [I] row, status draft
 * Set the mode bits and replay a deferred stop / pause / mute with the new mode.
 * [V] ZSND_TRK_PENDING is never cleared, here or anywhere. */
int zsndSeqSetMode(uint8_t hTrack, uint8_t mode)
{
    ZsndSeqTrack *t;

    TRACK_CHECK(hTrack, 0x25A);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x25C);
    t->flags = (uint16_t)((t->flags & ~ZSND_TRK_MODE_MASK) | (mode & ZSND_TRK_MODE_MASK));
    if (t->flags & ZSND_TRK_PENDING) {
        if (t->state == ZSND_TRK_STOPPED)
            zsndSeqStop(hTrack);
        else if (t->state == ZSND_TRK_PAUSED)
            zsndSeqPause(hTrack);
        else if (t->flags & ZSND_TRK_MUTED)
            zsndSeqMute(hTrack, 1);
        else if (t->flags & ZSND_TRK_GROUPMUTE)
            zsndSeqGroupMute(hTrack, 1);
    }
    return 0;
}

/* VA 0x0047d0d0, 162 bytes, [V] direct, status draft
 * [V] both rates scale with the tempo, so a faster tempo also stretches note lengths (in frames). */
int zsndSeqSetTempo(uint8_t hTrack, float tempo)
{
    ZsndSeqTrack *t;

    TRACK_CHECK(hTrack, 0x27E);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x280);
    t->msPerTick = (uint32_t)(tempo * 4096000.0f / (int64_t)g_zsndUpdateRate);
    t->ticksPerMs = (uint32_t)((int64_t)g_zsndUpdateRate * tempo * 4.096f);
    return 0;
}

/* VA 0x0047d1b0, 173 bytes, [V] direct, status draft */
int zsndSeqSetVolume(uint8_t hTrack, uint8_t volume, int8_t pan)
{
    ZsndSeqTrack *t;
    ZsndSeqNote  *n;

    TRACK_CHECK(hTrack, 0x29A);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x29C);
    t->volume = volume;
    t->pan = pan;
    for (n = t->notes; n; n = n->next) {
        if (n->flags & ZSND_NOTE_SILENT)
            continue;
        zsndcSetVolume(n->channel, NOTE_VOLUME(t, n));
        zsndcSetPan(n->channel, t->pan);
    }
    return 0;
}

/* VA 0x0047d290, 98 bytes, [I] row, status draft */
int zsndSeqSetLoop(uint8_t hTrack, int loop)
{
    ZsndSeqTrack *t;

    TRACK_CHECK(hTrack, 0x2B3);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x2B5);
    if (loop)
        t->flags |= ZSND_TRK_LOOP;
    else
        t->flags &= ~ZSND_TRK_LOOP;
    return 0;
}

/* VA 0x0047d320, 82 bytes, [I] row, status draft
 * Fire and forget. [V] returns 0 (not the handle) on success, 0xFF on failure. */
uint8_t zsndSeqPlayTrackDef(ZsndTrackDef *def, uint8_t volume, int8_t pan)
{
    uint8_t h = zsndSeqCreateTrack(def, ZSND_TRK_AUTOFREE);

    if (h == 0xFF)
        return 0xFF;
    if (zsndSeqSetVolume(h, volume, pan) == 0 && zsndSeqPlay(h) == 0)
        return 0;
    zsndSeqFreeTrack(h);
    return 0xFF;
}

/* VA 0x0047d3a0, 91 bytes, [I] row, status draft */
int zsndSeqSetCallback(uint8_t hTrack, void *callback, void *user)
{
    ZsndSeqTrack *t;

    TRACK_CHECK(hTrack, 0x2E1);
    t = g_zsndSeqTracks[hTrack];
    ASSERT_AT(t, "track", 0x2E3);
    t->callback = (void (__cdecl *)(void *, int, int))callback;
    t->user = user;
    return 0;
}

/* VA 0x0047d420, 28 bytes, [I] row, status draft */
int zsndSeqIsRunning(const ZsndSeqTrack *t)
{
    if (t->state == ZSND_TRK_PLAYING)
        return 1;
    return (t->flags & ZSND_TRK_PENDING) >> 14;
}

/* VA 0x0047d450, 38 bytes, [I] row, status draft
 * The delta time (ms) stored in the next event: forms 3-5 have a u8, 6-7 a u32, 0-2 none. */
uint32_t zsndSeqPeekDelta(const ZsndSeqTrack *t)
{
    uint8_t form = t->pc[0] & 7;

    if (form < 3)
        return 0;
    if (form < 6)
        return t->pc[1];
    return zsndSeqReadU32(t->pc + 1);
}

/* VA 0x0047d490, 496 bytes, [I] row, status draft
 * Decode the event at t->pc into ev and advance. Returns 0 for an unknown opcode (pc stays).
 * Event: byte  (opcode & 0xF8) | form;  form = delta / length layout:
 *   0: -            1: len u8        2: len u32
 *   3: delta u8     4: delta u8, len u8     5: delta u8, len u32
 *   6: delta u32    7: delta u32, len u32
 * then the parameters (ZSND_OP_*). */
int zsndSeqDecodeEvent(ZsndSeqTrack *t, ZsndSeqEvent *ev)
{
    const uint8_t *p = t->pc;
    uint8_t        b = *p++, op = b & 0xF8;
    uint8_t        par[4];
    int            indirect, size = 0, i, ok = 1;

    ev->track = t;
    switch (b & 7) {
    case 0: ev->length = 0; break;
    case 1: ev->length = *p++; break;
    case 2: ev->length = zsndSeqReadU32(p); p += 4; break;
    case 3: ev->length = 0; p += 1; break;
    case 4: ev->length = p[1]; p += 2; break;
    case 5: ev->length = zsndSeqReadU32(p + 1); p += 5; break;
    case 6: ev->length = 0; p += 4; break;
    case 7: ev->length = zsndSeqReadU32(p + 4); p += 8; break;
    }
    if (op == ZSND_OP_INDIRECT) {
        /* opcode byte, then a source byte: 2 bits per parameter (1 literal byte, 2 "a byte 3
         * further on", 3 one of the track's reg[]). [V] the code masks with 2 instead of 3, so
         * only source 2 works; no bank uses this form. */
        uint8_t src;
        indirect = 1;
        op = p[0];
        src = p[1];
        p += 2;
        for (i = 0; i < 4; i++, src >>= 2) {
            switch (src & 2) {
            case 1:
                par[i] = *p++;
                break;
            case 2:
                par[i] = p[3];
                p += 3;
                break;
            case 3:
                switch (*p++) {
                case 0: par[i] = (uint8_t)t->reg[0]; break;
                case 1: par[i] = (uint8_t)t->reg[1]; break;
                case 2: par[i] = (uint8_t)t->reg[2]; break;
                case 3: par[i] = (uint8_t)t->reg[3]; break;
                }
                break;
            }
        }
    } else {
        indirect = 0;
        par[0] = p[0];
        par[1] = p[1];
    }
    switch (op) {
    case ZSND_OP_NOTE:
        ev->handler = zsndSeqEvNote;
        ev->p0 = par[0];
        ev->p1 = par[1];
        size = 2;
        break;
    case ZSND_OP_NOTE_RUN:
        ev->handler = zsndSeqEvNote;
        ev->p0 = par[0];
        ev->p1 = t->velocity;
        size = 1;
        break;
    case ZSND_OP_CONTROL:
        ev->handler = zsndSeqEvControl;
        ev->p0 = par[0];
        ev->p1 = par[1];
        size = 2;
        break;
    case ZSND_OP_MARKER:
        ev->handler = zsndSeqEvMarker;
        ev->p0 = par[0];
        ev->p1 = par[1];
        size = 2;
        break;
    case ZSND_OP_JUMP:
        ev->handler = zsndSeqEvJump;
        *(uint32_t *)&ev->p0 = zsndSeqReadU32(p);
        size = 4;
        break;
    case 0x20: case 0x28: case 0x38: case 0x40:
        break;                          /* no handler set (see zsndSeqUpdate) */
    default:
        ok = 0;
        break;
    }
    if (!indirect)
        p += size;
    t->pc = (uint8_t *)p;
    return ok;
}

/* VA 0x0047d7b0, 476 bytes, [I] row, status draft */
int zsndSeqEvNote(ZsndSeqEvent *ev)
{
    ZsndSeqTrack *t = ev->track;
    ZsndSeqNote  *n;
    ZsndVoice    *voice = 0;
    int16_t       cents;
    uint32_t      i, ticks;

    t->velocity = ev->p1;
    if (t->voice) {
        voice = t->voice;
        cents = (int16_t)((ev->p0 - 60) * 100);      /* note 60 = the voice's own pitch */
    } else {
        if (!t->keymap)
            return 0;
        zsndSeqKeymapLookup(t->keymap, ev->p0, &voice, &cents);
    }
    if (!voice)
        return 0;
    i = g_zsndSeqNextNote;
    while (g_zsndSeqNotes[i].flags & ZSND_NOTE_USED) {
        if (++i == ZSND_SEQ_MAX_NOTES)
            i = 0;
        if (i == g_zsndSeqNextNote)
            return 0;
    }
    n = &g_zsndSeqNotes[i];
    n->flags = ZSND_NOTE_USED;
    n->next = 0;
    g_zsndSeqNextNote = i + 1 == ZSND_SEQ_MAX_NOTES ? 0 : i + 1;
    n->voice = voice;
    n->velocity = ev->p1;
    n->cents = cents;
    ticks = t->ticksPerMs * ev->length;              /* ms -> frames, 20.12 */
    n->ticks = (ticks & 0xFFFFF000u) < 0x1000 ? 1 : ticks >> 12;
    if ((t->flags & ZSND_TRK_MUTED) || (g_zsndSeqGroupMuteOn && (t->flags & ZSND_TRK_GROUPMUTE))) {
        n->flags |= ZSND_NOTE_SILENT;                /* keeps time, makes no sound */
        zsndSeqInsertNote(t, n);
        return 1;
    }
    n->channel = zsndcAlloc(voice, 0xFF);
    if (n->channel != 0xFF) {
        zsndcSetPitch(n->channel, n->cents);
        zsndcSetVolume(n->channel, NOTE_VOLUME(t, n));
        zsndcSetPan(n->channel, t->pan);
        zsndcKeyOn(n->channel);
        zsndSeqInsertNote(t, n);
        return 1;
    }
    n->flags = 0;
    g_zsndSeqNextNote = i;
    return 0;
}

/* VA 0x0047da10, 274 bytes, [I] row, status draft */
int zsndSeqEvControl(ZsndSeqEvent *ev)
{
    ZsndSeqTrack *t = ev->track;
    ZsndSeqNote  *n;

    switch (ev->p0) {
    case ZSND_CTL_EXPRESSION:
        t->expression = ev->p1;
        for (n = t->notes; n; n = n->next)
            if (!(n->flags & ZSND_NOTE_SILENT))
                zsndcSetVolume(n->channel, NOTE_VOLUME(t, n));
        return 1;
    case ZSND_CTL_PAN:
        t->pan = (int8_t)ev->p1;
        for (n = t->notes; n; n = n->next)
            if (!(n->flags & ZSND_NOTE_SILENT))
                zsndcSetPan(n->channel, t->pan);
        return 1;
    case 2:
    case 3:
        return 1;
    case ZSND_CTL_VOICE:
        t->voice = ev->p1 == 0xFF ? 0 : zsndGetVoice(t->bank, (int8_t)ev->p1);
        return 1;
    case ZSND_CTL_KEYMAP:
        t->keymap = ev->p1 == 0xFF ? 0 : zsndGetKeymap(t->bank, (int8_t)ev->p1);
        return 1;
    }
    return 0;
}

/* VA 0x0047db90, 92 bytes, [I] row, status draft */
int zsndSeqEvMarker(ZsndSeqEvent *ev)
{
    ZsndSeqTrack *t = ev->track;

    if (ev->p0 == 1) {                   /* end of track */
        if (t->flags & ZSND_TRK_LOOP) {
            t->notes = 0;                /* [V] dropped, not keyed off: their channels and slots leak */
            t->pc = t->start;
            t->wait = zsndSeqPeekDelta(t) << 12;
        } else {
            t->state = ZSND_TRK_STOPPED;
        }
    }
    if (t->callback)
        t->callback(t->user, ev->p0, ev->p1);
    return 1;
}

/* VA 0x0047dc10, 40 bytes, [I] row, status draft */
int zsndSeqEvJump(ZsndSeqEvent *ev)
{
    ZsndSeqTrack *t = ev->track;

    t->pc = t->start + *(uint32_t *)&ev->p0;
    t->wait = zsndSeqPeekDelta(t) << 12;
    return 1;
}

/* VA 0x0047dc50, 89 bytes, [I] row, status draft
 * Insert into the track's delta list (each note's ticks count from the one before it). */
void zsndSeqInsertNote(ZsndSeqTrack *t, ZsndSeqNote *n)
{
    ZsndSeqNote *p = t->notes, *prev = 0;
    uint32_t     sum = 0;

    while (p) {
        if (n->ticks < sum + p->ticks)
            break;
        sum += p->ticks;
        prev = p;
        p = p->next;
    }
    if (prev) {
        prev->next = n;
        n->ticks -= sum;
    } else {
        t->notes = n;
    }
    n->next = p;
    if (p)
        p->ticks -= n->ticks;
}

/* VA 0x0047dcd0, 66 bytes, [V] direct, status draft */
void zsndSeqNoteOff(ZsndSeqNote *n)
{
    ASSERT_AT(n->flags & ZSND_NOTE_USED, "noteEvent->flags & kzsndNoteEventFlagActive", 0x46C);
    if (!(n->flags & ZSND_NOTE_SILENT)) {
        zsndcKeyOff(n->channel, 0);
        zsndcFree(n->channel);
    }
    n->flags = 0;
}

/* VA 0x0047dd40, 121 bytes, [I] row, status draft */
int zsndSeqKeymapLookup(const ZsndKeymap *km, uint8_t note, ZsndVoice **voice, int16_t *cents)
{
    uint32_t k;

    *voice = 0;
    if (note < km->firstNote || note >= km->firstNote + km->count)
        return 0;
    k = note - km->firstNote;
    if (km->key[k].voice == 0xFF)
        return 0;
    *voice = zsndGetVoice(km->bank, km->key[k].voice);
    *cents = (int16_t)(km->key[k].semitones * 100);
    return 1;
}

/* VA 0x0047dde0, 39 bytes, [I] row, status draft */
uint32_t zsndSeqReadU32(const uint8_t *p)
{
    return *(const uint32_t *)p;        /* unaligned little-endian read */
}

/*
 * NgEngine/Zge/zanim.c
 * Original: C:\NgEngine\Zge\zanim.c
 * Evidence: [V] path string referenced by zanimAllocTrack (inlined 6 times), the IFF chunk handlers,
 * zanimInit, zanimPlay and zanimMix; the rest [I] (link order 0x00498D80-0x0049A6CB, between
 * zrwTxr.c and ziff.c, and the globals 0x006746C0-0x006746C8).
 * 36 functions, VA 0x00498d80-0x0049a6cb. DM-042 wrote zanimLoadIff .. zanimFreeContents (then in
 * ziff.c); DM-045 completed the module. Non-matching C (AD-8). Types: decomp/include/zanim.h.
 *
 * Keyframe animation ("NAD" data, Scene.Txt ANIM entries *.NAD / *.IFF / *.ZAL):
 *  - loading: zanimLoadIff parses an ANIM form into a ZanimContents: DEFI (node names + hashes),
 *    AROT (rotation keys, one channel per node) and ATRA (translation keys);
 *  - playing: every model zobj gets a ZanimNodeTable (one ZanimNode per body part, sorted by name
 *    hash). zanimPlay / zanimCrossFade / zanimMix attach ZanimTracks (one per node and animation)
 *    with fade-in/out times and a weight; zanimEvaluate blends all tracks of a node at time t
 *    (slerp between keys, weighted shortest-path sum, normalise) into node->quat / trans / scale;
 *  - zrwModelUpdate turns node->quat into the RenderWare frame rotation through
 *    zrwModelQuatToRwMatrix (q -> (x, -y, -z, w)).
 * In NgBMX.exe only the loader, the node table (zanimCreateNodeTable / zanimDestroyNodeTable) and
 * zanimFindNode / zanimNodeSetQuat are reached: the rider and bike are posed by
 * NgIFzactor_ApplyMotionPose 0x004654B0 from motion.zal (decoder zactorDecodeMotion 0x00472B10), which writes
 * the node quaternions directly. The track/blend code below has no callers.
 */
#include "decomp.h"
#include "zmath.h"
#include "zanim.h"

#define ZANIM_FILE "C:\\NgEngine\\Zge\\zanim.c"
#define ID_ANIM 0x4D494E41u   /* 'ANIM' */
#define ID_DEFI 0x49464544u   /* 'DEFI' */
#define ID_AROT 0x544F5241u   /* 'AROT' */
#define ID_ATRA 0x41525441u   /* 'ATRA' */

#define ZANIM_MAX_TIME      32400.0f   /* zanimPlay asserts below this; zanimRebaseTime shifts down */
#define ZANIM_HOLD_TIME     10000.0f   /* a new track holds its last key this long after its end */

/* other modules (current names until their cards land) */
int  zobjModelGetNumParts(ZanimObj *obj);                          /* number of body parts (zrwModelGetNumParts) */
void zobjModelGetPartIds(ZanimObj *obj, uint32_t *hashes);        /* their name hashes (zrwModelGetPartIds) */
void zobjModelSetNodes(ZanimObj *obj, ZanimNodeTable *table);   /* obj->nodes = table; zrwModelBindParts links parts */

int            g_zanimInitialized;  /* 0x006746C0 */
ZanimTrack    *g_zanimTrackPool;    /* 0x006746C4: free tracks, linked through next */
ZanimContents *g_zanimContents;     /* 0x006746C8: target of the chunk handlers during zanimLoadIff */

/* ------------------------------------------------------------------ track pool */

/* VA 0x00498d80, 37 bytes, [I] row, status draft */
void zanimFlushTrackPool(void)
{
    ZanimTrack *t;

    while ((t = g_zanimTrackPool) != 0) {
        g_zanimTrackPool = t->next;
        zmemFree(t);
    }
}

/* VA 0x00498db0, 19 bytes, [I] row, status draft */
void zanimFreeTrack(ZanimTrack *track)
{
    track->next = g_zanimTrackPool;
    g_zanimTrackPool = track;
}

/* VA 0x00498dd0, 89 bytes, [I] row, status draft */
ZanimTrack *zanimAllocTrack(void)
{
    /* inlined in zanimPlay, zanimCrossFade and zanimMix (same assert line) */
    ZanimTrack *t = g_zanimTrackPool;

    if (t) {
        g_zanimTrackPool = t->next;
    } else {
        t = (ZanimTrack *)zmemAlloc(sizeof(ZanimTrack));
        if (!t)
            zAssertFail("Cannot allocate", ZANIM_FILE, 0x6F, 0);
    }
    zstringMemSet(t, 0, sizeof(ZanimTrack));
    return t;
}

/* VA 0x00498e30, 55 bytes, [I] row, status draft */
void zanimNodeUnlinkTrack(ZanimNode *node, ZanimTrack *track, ZanimTrack *prev)
{
    if (prev)
        prev->next = track->next;
    else
        node->tracks = track->next;
    zanimFreeTrack(track);
}

/* ------------------------------------------------------------------ key lookup (unused) */

/* The three samplers return the first key after t (no interpolation); past the last key they
 * read one key beyond the array. */

/* VA 0x00498e70, 65 bytes, [I] row, status draft */
void zanimSampleRotKey(const ZanimChannel *ch, float t, Zquat *out)
{
    uint32_t i = 0;

    while (i < ch->count && ch->keys[i].time <= t)
        i++;
    zquatCopy(out, (const Zquat *)ch->keys[i].v);
}

/* VA 0x00498ec0, 73 bytes, [I] row, status draft */
void zanimSampleTransKey(const ZanimChannel *ch, float t, RwV3d *out)
{
    uint32_t i = 0;

    while (i < ch->count && ch->keys[i].time <= t)
        i++;
    out->x = ch->keys[i].v[0];
    out->y = ch->keys[i].v[1];
    out->z = ch->keys[i].v[2];
}

/* VA 0x00498f10, 73 bytes, [I] row, status draft */
void zanimSampleScaleKey(const ZanimChannel *ch, float t, RwV3d *out)
{
    uint32_t i = 0;

    while (i < ch->count && ch->keys[i].time <= t)
        i++;
    out->x = ch->keys[i].v[0];
    out->y = ch->keys[i].v[1];
    out->z = ch->keys[i].v[2];
}

/* VA 0x00498f60, 53 bytes, [I] row, status draft */
void zanimTrackFadeOut(ZanimTrack *track, float t, float duration)
{
    /* inlined in zanimCrossFade */
    if (track->fadeInEnd > t)
        track->fadeInEnd = t;
    track->fading = 1;
    track->fadeOutStart = t;
    track->expire = track->fadeOutEnd = t + duration;
}

/* VA 0x00498fa0, 16 bytes, [I] row, status draft */
void zanimTrackFadeIn(ZanimTrack *track, float t, float duration)
{
    track->fadeInEnd = t + duration;
}

/* ------------------------------------------------------------------ IFF chunk handlers */

/* VA 0x00498fb0, 6 bytes, [I] row, status draft */
int zanimChunkANIM(ZiffFile *iff, void *user)
{
    return 1;                       /* the form itself: its children are parsed by ziffParse */
}

/* VA 0x00498fc0, 240 bytes, [V] direct, status draft */
int zanimChunkDEFI(ZiffFile *iff, void *user)
{
    /* u32 animation id, then 36-byte entries { char name[32]; u32 hash; } */
    ZanimNameList *list = (ZanimNameList *)zmemAlloc(sizeof(ZanimNameList));

    if (!list)
        zAssertFail("Can't allocate namelist", ZANIM_FILE, 0x224, 0);
    zstringMemSet(list, 0, sizeof(ZanimNameList));
    list->next = g_zanimContents->names;
    g_zanimContents->names = list;
    ziffChunkReadLong(iff, (int32_t *)&g_zanimContents->id);
    list->count = (ziffChunkGetSize(iff) - 4) / sizeof(ZanimDefName);
    if (list->count * sizeof(ZanimDefName) + 4 != ziffChunkGetSize(iff))
        zAssertFail("DEFI size mismatch", ZANIM_FILE, 0x22D, 0);
    list->names = (ZanimDefName *)zmemAlloc(list->count * sizeof(ZanimDefName));
    if (!list->names)
        zAssertFail("Can't allocate nadname", ZANIM_FILE, 0x230, 0);
    ziffChunkReadData(iff, list->names, ziffChunkGetSize(iff) - 4);
    return 1;
}

/* VA 0x004990b0, 322 bytes, [V] direct, status draft */
int zanimChunkAROT(ZiffFile *iff, void *user)
{
    /* u32 index into the latest DEFI list, then 20-byte keys { f32 time; f32 x, y, z, w; } */
    ZanimChannel *ch = (ZanimChannel *)zmemAlloc(sizeof(ZanimChannel));
    int32_t index;
    uint32_t i;

    if (!ch)
        zAssertFail("Can't allocate rot channel", ZANIM_FILE, 0x247, 0);
    zstringMemSet(ch, 0, sizeof(ZanimChannel));
    ch->next = g_zanimContents->rot;
    g_zanimContents->rot = ch;
    ziffChunkReadLong(iff, &index);
    ch->hash = g_zanimContents->names->names[index].hash;
    ch->count = (ziffChunkGetSize(iff) - 4) / 20;
    ch->keys = (ZanimKey *)zmemAlloc(ch->count * sizeof(ZanimKey));
    if (!ch->keys)
        zAssertFail("Can't Allocate NadRots", ZANIM_FILE, 0x255, 0);
    for (i = 0; i < ch->count; i++) {
        ziffChunkReadFloat(iff, &ch->keys[i].time);
        ziffChunkReadFloat(iff, &ch->keys[i].v[0]);
        ziffChunkReadFloat(iff, &ch->keys[i].v[1]);
        ziffChunkReadFloat(iff, &ch->keys[i].v[2]);
        ziffChunkReadFloat(iff, &ch->keys[i].v[3]);
    }
    return 1;
}

/* VA 0x00499200, 292 bytes, [V] direct, status draft */
int zanimChunkATRA(ZiffFile *iff, void *user)
{
    /* u32 DEFI index, then 16-byte keys { f32 time; f32 x, y, z; } (stored 20 bytes apart) */
    ZanimChannel *ch = (ZanimChannel *)zmemAlloc(sizeof(ZanimChannel));
    int32_t index;
    uint32_t i;

    if (!ch)
        zAssertFail("Can't allocate Trans channel", ZANIM_FILE, 0x272, 0);
    zstringMemSet(ch, 0, sizeof(ZanimChannel));
    ch->next = g_zanimContents->trans;
    g_zanimContents->trans = ch;
    ziffChunkReadLong(iff, &index);
    ch->hash = g_zanimContents->names->names[index].hash;
    ch->count = (ziffChunkGetSize(iff) - 4) >> 4;
    ch->keys = (ZanimKey *)zmemAlloc(ch->count * sizeof(ZanimKey));
    if (!ch->keys)
        zAssertFail("Can't Allocate NadTrans", ZANIM_FILE, 0x280, 0);
    for (i = 0; i < ch->count; i++) {
        ziffChunkReadFloat(iff, &ch->keys[i].time);
        ziffChunkReadFloat(iff, &ch->keys[i].v[0]);
        ziffChunkReadFloat(iff, &ch->keys[i].v[1]);
        ziffChunkReadFloat(iff, &ch->keys[i].v[2]);
    }
    return 1;
}

/* ------------------------------------------------------------------ module, node table */

/* VA 0x00499330, 122 bytes, [V] direct, status draft */
int zanimInit(void)
{
    if (g_zanimInitialized)
        zAssertFail("Already initialized", ZANIM_FILE, 0x29D, 0);
    g_zanimInitialized = 1;
    /* the exe passes a third argument (0, then the ANIM handler as the parent) that
     * ziffRegisterHandler does not read */
    ziffRegisterHandler(ID_ANIM, zanimChunkANIM);
    ziffRegisterHandler(ID_DEFI, zanimChunkDEFI);
    ziffRegisterHandler(ID_AROT, zanimChunkAROT);
    ziffRegisterHandler(ID_ATRA, zanimChunkATRA);
    return 1;
}

/* VA 0x004993b0, 61 bytes, [I] row, status draft */
int zanimShutdown(void)
{
    if (g_zanimInitialized) {
        zanimFlushTrackPool();      /* inlined */
        g_zanimInitialized = 0;
    }
    return 1;
}

/* VA 0x004993f0, 212 bytes, [I] row, status draft */
ZanimNodeTable *zanimCreateNodeTable(ZanimObj *obj)
{
    int count = zobjModelGetNumParts(obj);
    uint32_t size = sizeof(ZanimNodeTable) + count * sizeof(ZanimNode);
    ZanimNodeTable *table = (ZanimNodeTable *)zmemAlloc(size);
    uint32_t *hashes;
    uint32_t prev = 0;
    int i, j;

    zstringMemSet(table, 0, size);
    hashes = (uint32_t *)zmemAlloc(count * sizeof(uint32_t));
    zobjModelGetPartIds(obj, hashes);
    table->count = count;
    table->nodes = (ZanimNode *)(table + 1);
    /* selection sort by hash (ascending, duplicates collapse; leftover slots get 0xFFFFFFFF) */
    for (i = 0; i < count; i++) {
        ZanimNode *node = &table->nodes[i];
        uint32_t best = 0xFFFFFFFFu;

        for (j = 0; j < count; j++) {
            if (hashes[j] < best && hashes[j] > prev)
                best = hashes[j];
        }
        node->hash = best;
        prev = best;
        node->quat[0] = node->quat[1] = node->quat[2] = 0.0f;
        node->quat[3] = 1.0f;
        node->scale[0] = node->scale[1] = node->scale[2] = 1.0f;
    }
    zmemFree(hashes);
    zobjModelSetNodes(obj, table);
    return table;
}

/* VA 0x004994d0, 93 bytes, [I] row, status draft */
void zanimDestroyNodeTable(ZanimObj *obj)
{
    ZanimNodeTable *table = obj->nodes;
    ZanimTrack *t;
    uint32_t i;

    if (!table)
        return;
    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];

        while ((t = node->tracks) != 0) {
            node->tracks = t->next;
            zanimFreeTrack(t);
        }
    }
    zmemFree(obj->nodes);
}

/* ------------------------------------------------------------------ playback (no callers) */

/* VA 0x00499530, 107 bytes, [I] row, status draft */
void zanimRebaseTime(ZanimObj *obj, float dt)
{
    ZanimNodeTable *table = obj->nodes;
    ZanimTrack *t;
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        for (t = table->nodes[i].tracks; t; t = t->next) {
            t->end -= dt;
            t->expire -= dt;
            t->start -= dt;
            t->fadeInEnd -= dt;
            t->fadeOutEnd -= dt;
            t->fadeOutStart -= dt;
        }
    }
}

/* The channel lists of a ZanimContents and the node table are both sorted by hash, so the
 * playback functions walk them in step: advance a channel pointer while its hash is below the
 * node's, use it if equal. */
#define ZANIM_SEEK(ch, h)  while ((ch) && (ch)->hash < (h)) (ch) = (ch)->next

/* A track for `ch` that starts at t and ends with the channel's last key (inlined in the exe). */
static ZanimTrack *zanimNewTrack(const ZanimChannel *ch, float t)
{
    ZanimTrack *track = zanimAllocTrack();

    track->start = t;
    track->end = t + ch->keys[ch->count - 1].time;
    return track;
}

/* Full weight from t on, hold the end pose ZANIM_HOLD_TIME, then fade out over 1 ms. */
static void zanimTrackSetHold(ZanimTrack *track, float t)
{
    track->weight = 1.0f;
    track->fadeInEnd = t;
    track->expire = track->fadeOutStart = track->end + ZANIM_HOLD_TIME;
    track->fadeOutEnd = track->fadeOutStart + 1.0f;
}

/* VA 0x004995a0, 556 bytes, [I] row, status draft */
void zanimPlay(ZanimObj *obj, ZanimContents *anim, float t)
{
    /* replace whatever plays on every node by `anim`, starting at t */
    ZanimNodeTable *table = obj->nodes;
    ZanimChannel *rot = anim->rot, *trans = anim->trans, *scale = anim->scale;
    ZanimTrack *tr;
    uint32_t i;

    if (!(t < ZANIM_MAX_TIME))
        zAssertFail("Hey, Jacko!  Float time is too big, reset it", ZANIM_FILE, 0x366, 0);
    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];
        ZanimTrack *track = 0;

        while ((tr = node->tracks) != 0) {
            node->tracks = tr->next;
            zanimFreeTrack(tr);
        }
        ZANIM_SEEK(rot, node->hash);
        if (rot && rot->hash == node->hash) {
            track = zanimNewTrack(rot, t);
            track->rot = rot;
        }
        ZANIM_SEEK(trans, node->hash);
        if (trans && trans->hash == node->hash) {
            if (!track)
                track = zanimNewTrack(trans, t);
            track->trans = trans;
        }
        ZANIM_SEEK(scale, node->hash);          /* walked, but scale channels are never attached */
        if (track) {
            zanimTrackSetHold(track, t);
            node->tracks = track;
        }
    }
}

/* VA 0x004997d0, 571 bytes, [I] row, status draft */
void zanimCrossFade(ZanimObj *obj, ZanimContents *anim, float t, float duration)
{
    /* fade out what plays (over `duration`) and fade `anim` in over the same time */
    ZanimNodeTable *table = obj->nodes;
    ZanimChannel *rot = anim->rot, *trans = anim->trans, *scale = anim->scale;
    int nothingPlaying = 1;         /* set once for the whole table, not per node (sic) */
    ZanimTrack *tr;
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];
        ZanimTrack *track = 0;

        for (tr = node->tracks; tr && tr->fading != 1; tr = tr->next) {
            nothingPlaying = 0;
            zanimTrackFadeOut(tr, t, duration);     /* inlined */
        }
        ZANIM_SEEK(rot, node->hash);
        if (rot && rot->hash == node->hash) {
            track = zanimNewTrack(rot, t);
            track->rot = rot;
        }
        ZANIM_SEEK(trans, node->hash);
        if (trans && trans->hash == node->hash) {
            if (!track)
                track = zanimNewTrack(trans, t);
            track->trans = trans;
        }
        ZANIM_SEEK(scale, node->hash);
        if (track) {
            zanimTrackSetHold(track, t);
            track->next = node->tracks;
            node->tracks = track;
            if (!nothingPlaying)
                track->fadeInEnd = t + duration;
        }
    }
}

/* VA 0x00499a10, 670 bytes, [I] row, status draft */
void zanimMix(ZanimObj *obj, ZanimContents *anim, float t, const ZanimWeights *weights)
{
    /* layer `anim` over the (non-fading) animation playing on each node, with a per-node weight
     * (default 0.5); the tracks underneath are scaled by 1 - weight */
    ZanimNodeTable *table = obj->nodes;
    ZanimChannel *rot = anim->rot, *trans = anim->trans, *scale = anim->scale;
    uint32_t w = 0;                 /* cursor into weights->entries, kept across nodes */
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];
        /* sic: starts as the node's current head track, so a translation-only node attaches its
         * channel to the playing track and then links that track in front of itself */
        ZanimTrack *track = node->tracks;
        ZanimTrack *base, *tr;
        float weight, rest;

        ZANIM_SEEK(rot, node->hash);
        if (rot && rot->hash == node->hash) {
            track = zanimNewTrack(rot, t);
            track->rot = rot;
        }
        ZANIM_SEEK(trans, node->hash);
        if (trans && trans->hash == node->hash) {
            if (!track)
                track = zanimNewTrack(trans, t);
            track->trans = trans;
        }
        ZANIM_SEEK(scale, node->hash);
        if (!track)
            continue;
        track->next = node->tracks;
        node->tracks = track;
        if (!track->next)
            zAssertFail("Empty stream list", ZANIM_FILE, 0x4AA, 0);
        if (track->next->fading != 0)
            zAssertFail("Non nonblended animation", ZANIM_FILE, 0x4AB, 0);

        weight = 0.5f;
        if (w < weights->count - 1) {
            while (w < weights->count - 1 && weights->entries[w].hash < node->hash)
                w++;
        }
        if (weights->entries[w].hash == node->hash)
            weight = weights->entries[w].weight;

        /* same timing as the track underneath */
        base = track->next;
        track->weight = weight;
        track->fadeInEnd = base->fadeInEnd;
        track->expire = base->expire;
        track->fadeOutStart = base->fadeOutStart;
        track->fadeOutEnd = base->fadeOutEnd;
        track->end = base->end;
        rest = 1.0f - weight;
        for (tr = base; tr && tr->fading == 0; tr = tr->next)
            tr->weight *= rest;
    }
}

/* VA 0x00499cb0, 72 bytes, [I] row, status draft */
void zanimStop(ZanimObj *obj)
{
    ZanimNodeTable *table = obj->nodes;
    ZanimTrack *t;
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];

        while ((t = node->tracks) != 0) {
            node->tracks = t->next;
            zanimFreeTrack(t);
        }
    }
}

/* VA 0x00499d00, 82 bytes, [I] row, status draft */
int zanimIsDone(ZanimObj *obj, float t)
{
    /* decided by the first non-fading track found (in node order): done once t is past its end */
    ZanimNodeTable *table = obj->nodes;
    ZanimTrack *tr;
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        for (tr = table->nodes[i].tracks; tr; tr = tr->next) {
            if (tr->fading == 0)
                return t > tr->end;
        }
    }
    return 1;
}

/* Weight of a track at time t: 0 -> 1 over [start, fadeInEnd], 1 until fadeOutStart, 1 -> 0 by
 * fadeOutEnd, then 0; times the track weight (inlined three times in the exe). */
float zanimEvalScaleTrack(ZanimNode *node, ZanimTrack *track, float t);   /* 0x0049A180, below */

static float zanimTrackWeight(const ZanimTrack *tr, float t)
{
    float w;

    if (t < tr->fadeInEnd)
        w = (t - tr->start) / (tr->fadeInEnd - tr->start);
    else if (t < tr->fadeOutStart)
        w = 1.0f;
    else if (t < tr->fadeOutEnd)
        w = 1.0f - (t - tr->fadeOutStart) / (tr->fadeOutEnd - tr->fadeOutStart);
    else
        w = 0.0f;
    return w * tr->weight;
}

/* Key segment [k, k + 1] for local time lt, starting from the cursor k (inlined three times). */
static uint32_t zanimFindSegment(const ZanimChannel *ch, uint32_t k, float lt)
{
    if (lt < ch->keys[k].time) {
        /* sic: meant to step back, but counts up (k only grows until a key below lt is found);
         * evaluation times only move forward in practice, so this branch is rare */
        while (k > 0 && !(lt > ch->keys[k].time))
            k++;
        return k;
    }
    while (k < ch->count - 1 && !(lt < ch->keys[k].time))
        k++;
    return k - 1;
}

/* VA 0x00499d60, 1048 bytes, [I] row, status draft */
void zanimEvaluate(ZanimObj *obj, float t)
{
    ZanimNodeTable *table = obj->nodes;
    uint32_t i;

    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];
        ZanimTrack *tr, *prev = 0;
        float rotWeight = 0.0f, transWeight = 0.0f, scaleWeight = 0.0f;

        /* drop the tracks that have expired */
        for (tr = node->tracks; tr; ) {
            if (t > tr->expire) {
                if (prev)
                    prev->next = tr->next;
                else
                    node->tracks = tr->next;
                zanimFreeTrack(tr);
                tr = prev ? prev->next : node->tracks;
            } else {
                prev = tr;
                tr = tr->next;
            }
        }
        node->flags &= 0xFFF8;

        for (tr = node->tracks; tr; tr = tr->next) {
            float tc = (t > tr->end) ? tr->end : t;     /* hold the last key */
            float lt = tc - tr->start;
            float w, u;
            uint32_t k;

            if (tr->rot) {
                w = zanimTrackWeight(tr, t);
                if (w < 1e-05f) {
                    w = 0.0f;
                } else {
                    const ZanimKey *keys = tr->rot->keys;
                    Zquat q, qw;

                    if (!(node->flags & ZANIM_NODE_QUAT)) {
                        node->flags |= ZANIM_NODE_QUAT;
                        node->quat[0] = node->quat[1] = node->quat[2] = node->quat[3] = 0.0f;
                    }
                    k = tr->rotKey = zanimFindSegment(tr->rot, tr->rotKey, lt);
                    u = (lt - keys[k].time) / (keys[k + 1].time - keys[k].time);
                    zquatSlerp(&q, (const Zquat *)keys[k].v, (const Zquat *)keys[k + 1].v, u);
                    zquatScale(&qw, w, &q);
                    zquatAddClosest((Zquat *)node->quat, &qw, (Zquat *)node->quat);
                }
                rotWeight += w;
            }
            if (tr->trans) {
                w = zanimTrackWeight(tr, t);
                if (w < 1e-05f) {
                    w = 0.0f;
                } else {
                    const ZanimKey *keys = tr->trans->keys;
                    RwV3d v, vw;

                    if (!(node->flags & ZANIM_NODE_TRANS)) {
                        node->flags |= ZANIM_NODE_TRANS;
                        node->trans[0] = node->trans[1] = node->trans[2] = 0.0f;
                    }
                    k = tr->transKey = zanimFindSegment(tr->trans, tr->transKey, lt);
                    u = (lt - keys[k].time) / (keys[k + 1].time - keys[k].time);
                    zmathVecLerp(&v, (const RwV3d *)keys[k].v, (const RwV3d *)keys[k + 1].v, u);
                    zmathVecScale(&vw, &v, w);
                    zmathVecAdd((RwV3d *)node->trans, &vw, (RwV3d *)node->trans);
                }
                transWeight += w;
            }
            if (tr->scale)
                scaleWeight += zanimEvalScaleTrack(node, tr, t);
        }

        if (node->flags & ZANIM_NODE_QUAT)
            zquatNormalize((Zquat *)node->quat);
        if (node->flags & ZANIM_NODE_TRANS)
            zmathVecScale((RwV3d *)node->trans, (RwV3d *)node->trans, 1.0f / transWeight);
        if (node->flags & ZANIM_NODE_SCALE)
            zmathVecScale((RwV3d *)node->scale, (RwV3d *)node->scale, 1.0f / scaleWeight);
        (void)rotWeight;            /* summed but unused: the quaternion is normalised instead */
    }
}

/* VA 0x0049a180, 371 bytes, [I] row, status draft */
float zanimEvalScaleTrack(ZanimNode *node, ZanimTrack *track, float t)
{
    float tc = (t > track->end) ? track->end : t;
    float w = zanimTrackWeight(track, t);
    const ZanimKey *keys = track->scale->keys;
    RwV3d v, vw;
    float lt, u;
    uint32_t k;

    if (w < 1e-05f)
        return 0.0f;
    if (!(node->flags & ZANIM_NODE_SCALE)) {
        node->flags |= ZANIM_NODE_SCALE;
        node->scale[0] = node->scale[1] = node->scale[2] = 0.0f;
    }
    lt = tc - track->start;
    k = track->scaleKey = zanimFindSegment(track->scale, track->scaleKey, lt);
    u = (lt - keys[k].time) / (keys[k + 1].time - keys[k].time);
    zmathVecLerp(&v, (const RwV3d *)keys[k].v, (const RwV3d *)keys[k + 1].v, u);
    zmathVecScale(&vw, &v, w);
    zmathVecAdd((RwV3d *)node->scale, &vw, (RwV3d *)node->scale);
    return w;
}

/* VA 0x0049a300, 478 bytes, [I] row, status draft */
void zanimApplyKeys(ZanimObj *obj, ZanimContents *anim, float t, float blend)
{
    /* blend the node pose towards the first key after t (no interpolation, no tracks):
     * node = node * (1 - blend) + key * blend, quaternions by the shortest path, renormalised */
    ZanimNodeTable *table = obj->nodes;
    ZanimChannel *rot = anim->rot, *trans = anim->trans, *scale = anim->scale;
    uint32_t i, k;

    for (i = 0; i < (uint32_t)table->count; i++) {
        ZanimNode *node = &table->nodes[i];

        ZANIM_SEEK(rot, node->hash);
        if (rot && rot->hash == node->hash) {
            Zquat q, qw;

            for (k = 0; k < rot->count && rot->keys[k].time <= t; k++)
                ;
            zquatCopy(&q, (const Zquat *)rot->keys[k].v);
            zquatScale(&qw, blend, &q);
            zquatScale((Zquat *)node->quat, 1.0f - blend, (Zquat *)node->quat);
            zquatAddClosest((Zquat *)node->quat, &qw, (Zquat *)node->quat);
            zquatNormalize((Zquat *)node->quat);
        }
        ZANIM_SEEK(trans, node->hash);
        if (trans && trans->hash == node->hash) {
            RwV3d v;

            for (k = 0; k < trans->count && trans->keys[k].time <= t; k++)
                ;
            v.x = trans->keys[k].v[0];
            v.y = trans->keys[k].v[1];
            v.z = trans->keys[k].v[2];
            zmathVecScale(&v, &v, blend);
            zmathVecScale((RwV3d *)node->trans, (RwV3d *)node->trans, 1.0f - blend);
            zmathVecAdd((RwV3d *)node->trans, (RwV3d *)node->trans, &v);
        }
        ZANIM_SEEK(scale, node->hash);
    }
}

/* ------------------------------------------------------------------ loader and node access */

/* VA 0x0049a4e0, 73 bytes, [I] row, status draft */
ZanimContents *zanimLoadIff(ZiffFile *iff)
{
    /* Parse every child of the next chunk into a fresh contents block (filled by the handlers). */
    if (!ziffChunkOpen(iff))
        return 0;
    g_zanimContents = (ZanimContents *)zmemAlloc(sizeof(ZanimContents));
    zstringMemSet(g_zanimContents, 0, sizeof(ZanimContents));
    ziffParse(iff, 0);
    ziffChunkClose(iff);
    return g_zanimContents;
}

/* VA 0x0049a530, 7 bytes, [I] row, status draft */
uint32_t zanimGetId(ZanimContents *anim)
{
    return anim->id;                /* zScene uses it as the motion id (ZsceneAnim.motionId) */
}

/* VA 0x0049a540, 40 bytes, [I] row, status draft */
ZanimNode *zanimFindNode(ZanimNodeTable *table, uint32_t hash)
{
    ZanimNode *node = table->nodes;
    int n;

    for (n = table->count; n != 0; n--, node++) {
        if (node->hash == hash)
            return node;
    }
    return 0;
}

/* VA 0x0049a570, 39 bytes, [I] row, status draft */
void zanimNodeSetQuat(ZanimNode *node, const Zquat *q)
{
    node->quat[0] = q->x;
    node->quat[1] = q->y;
    node->quat[2] = q->z;
    node->quat[3] = q->w;
    node->flags |= ZANIM_NODE_QUAT;
}

/* VA 0x0049a5a0, 8 bytes, [I] row, status draft */
float *zanimNodeGetQuat(ZanimNode *node)
{
    return node->quat;
}

/* VA 0x0049a5b0, 33 bytes, [I] row, status draft */
void zanimNodeSetTrans(ZanimNode *node, const RwV3d *v)
{
    node->trans[0] = v->x;
    node->trans[1] = v->y;
    node->trans[2] = v->z;
    node->flags |= ZANIM_NODE_TRANS;
}

/* VA 0x0049a5e0, 8 bytes, [I] row, status draft */
float *zanimNodeGetTrans(ZanimNode *node)
{
    return node->trans;
}

/* VA 0x0049a5f0, 33 bytes, [I] row, status draft */
void zanimNodeSetScale(ZanimNode *node, const RwV3d *v)
{
    node->scale[0] = v->x;
    node->scale[1] = v->y;
    node->scale[2] = v->z;
    node->flags |= ZANIM_NODE_SCALE;
}

/* VA 0x0049a620, 8 bytes, [I] row, status draft */
float *zanimNodeGetScale(ZanimNode *node)
{
    return node->scale;
}

/* VA 0x0049a630, 156 bytes, [I] row, status draft */
void zanimFreeContents(ZanimContents *anim)
{
    ZanimChannel *ch, *next;
    ZanimNameList *l, *nextl;

    for (ch = anim->trans; ch; ch = next) {
        next = ch->next;
        zmemFree(ch->keys);
        zmemFree(ch);
    }
    for (ch = anim->rot; ch; ch = next) {
        next = ch->next;
        zmemFree(ch->keys);
        zmemFree(ch);
    }
    for (ch = anim->scale; ch; ch = next) {
        next = ch->next;
        zmemFree(ch->keys);
        zmemFree(ch);
    }
    for (l = anim->names; l; l = nextl) {
        nextl = l->next;
        zmemFree(l->names);
        zmemFree(l);
    }
    zmemFree(anim);
}

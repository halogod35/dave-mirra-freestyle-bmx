/*
 * zaxis/Bmx/Src/Pc/zge/sound/winLoad.c
 * Original: C:\zaxis\Bmx\Src\Pc\zge\sound\winLoad.c
 * Evidence: [V] path string 0x0055EFE8, pushed by every function here except zsndUnloadAsset /
 *           zsndIsAssetLoaded ([I]: between them, and they use g_zsndAssets). The module map
 *           counts 11 [V]: the getters DM-055 created have no Ghidra references yet (-noanalysis).
 * 17 functions, VA 0x0046FB20-0x00470A47. DM-055 (draft). Non-matching C (AD-8).
 * Types: zsound.h. Bank format: docs/formats/AUDIO.md. Description: docs/re/sound.md.
 *
 * The sound-bank ("audio asset") manager: up to 16 loaded ZSND/WIN banks. A *resident* bank is
 * read whole (sfx banks: the mixer plays from memory); a streamed bank (music, speech) only has
 * its header area (tables + ZTRK code, bytes [0, headerSize)) in memory, and zsndGetSampleFile
 * hands the stream code the file name, offset and size of one sample file.
 * Loading turns the table offsets into pointers and stamps every voice / sample / track def /
 * keymap with the bank handle (the runtime fields of the file format, 0 on disk).
 * DM-055 created six getters Ghidra had missed (zsndIsAssetLoaded, zsndGetNumSamples,
 * zsndGetNumPhrases, zsndGetNumKeymaps, zsndGetPhrase; only reached through ILT thunks).
 * There is no checksum: the loader compares "ZSND" and "WIN " and nothing else.
 */
#include <string.h>
#include <windows.h>

#include "decomp.h"
#include "zsound.h"

#define WINLOAD_FILE "C:\\zaxis\\Bmx\\src\\Pc\\zge\\sound\\winLoad.c"

/* ------------------------------------------------------------------ globals */
ZsndFileDesc g_zsndFileDesc;                     /* 0x00600D88 zsndGetSampleFile's result */
ZsndAsset    g_zsndAssets[ZSND_MAX_ASSETS];      /* 0x00600D98 */
uint32_t     g_zsndNumVoices;                    /* 0x00601218 statistics over the loaded banks */
uint32_t     g_zsndNumSamples;                   /* 0x0060121C */
uint32_t     g_zsndNumResidentFiles;             /* 0x00601220 */
uint32_t     g_zsndNumStreamFiles;               /* 0x00601224 */
uint32_t     g_zsndNumPhrases;                   /* 0x00601228 */
uint32_t     g_zsndNumTrackDefs;                 /* 0x0060122C */
uint32_t     g_zsndNumKeymaps;                   /* 0x00601230 */

void zsndcStopVoice(ZsndVoice *voice);           /* winChan.c */

/* the checks every accessor starts with */
#define CHECK_HANDLE(h, l1, l2)                                                     \
    do {                                                                            \
        if ((h) >= ZSND_MAX_ASSETS)                                                 \
            zAssertFail("invalid file handle", WINLOAD_FILE, (l1), 0);              \
        if (!(g_zsndAssets[(h)].flags & ZSND_ASSET_LOADED))                         \
            zAssertFail("file not loaded", WINLOAD_FILE, (l2), 0);                  \
    } while (0)

/* VA 0x0046fb20, 382 bytes, [V] direct, status draft */
int zsndLoadAsset(const char *path, int resident, uint16_t *handle)
{
    HANDLE   f;
    DWORD    size, got;
    ZsndBankHeader header;
    uint8_t *data;
    int      err;

    *handle = 0xFFFF;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    if (!resident) {
        /* a streamed bank: only the tables and the ZTRK code stay in memory */
        ReadFile(f, &header, sizeof(header), &got, NULL);
        size = header.headerSize;
    } else {
        size = SetFilePointer(f, 0, NULL, FILE_END);
    }
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    data = (uint8_t *)zmemAlloc(size);
    if (!data) {
        CloseHandle(f);
        return 3;
    }
    ReadFile(f, data, size, &got, NULL);
    if (got != size) {
        err = 2;
    } else {
        err = zsndRegisterAsset((ZsndBankHeader *)data, resident, handle);
        if (err == 0) {
            ZsndAsset *a = &g_zsndAssets[*handle];
            a->flags |= ZSND_ASSET_OWNED;
            if (!resident) {
                /* [V] the length check looks at the (just cleared) stored name, not at `path`,
                 * so it never fires; a longer path would overrun the 64-byte field */
                if (strlen(a->path) > 63)
                    zAssertFail("file name too long", WINLOAD_FILE, 0x8F, 0);
                strcpy(a->path, path);
            }
            CloseHandle(f);
            return 0;
        }
    }
    zmemFree(data);
    CloseHandle(f);
    return err;
}

/* VA 0x0046fd00, 737 bytes, [V] direct, status draft */
int zsndRegisterAsset(ZsndBankHeader *bank, int resident, uint16_t *handle)
{
    ZsndAsset *a = 0;
    uint8_t   *base = (uint8_t *)bank;
    uint32_t   i, k;
    int        h;

    for (h = 0; h < ZSND_MAX_ASSETS; h++) {
        if (!(g_zsndAssets[h].flags & ZSND_ASSET_LOADED)) {
            a = &g_zsndAssets[h];
            a->path[0] = 0;
            a->flags = ZSND_ASSET_LOADED | (resident ? ZSND_ASSET_RESIDENT : 0);
            *handle = (uint16_t)h;
            break;
        }
    }
    if (!a)
        zAssertFail("too many audio asset files", WINLOAD_FILE, 0xCB, 0);   /* then writes through NULL */
    a->bank = bank;
    if (strncmp(bank->magic, "ZSND", 4) != 0)
        zAssertFail("invalid audio asset file header", WINLOAD_FILE, 0xCE, 0);
    if (strncmp(bank->platform, "WIN ", 4) != 0)
        zAssertFail("invalid audio asset file platform", WINLOAD_FILE, 0xCF, 0);

    /* table offsets -> pointers (0x38/0x3C are left alone) */
    bank->voices    = (ZsndVoice *)(base + (uint32_t)bank->voices);
    bank->samples   = (ZsndSample *)(base + (uint32_t)bank->samples);
    bank->trackDefs = (ZsndTrackDef *)(base + (uint32_t)bank->trackDefs);
    bank->files     = (ZsndSampleFile *)(base + (uint32_t)bank->files);
    bank->phrases   = (uint8_t **)(base + (uint32_t)bank->phrases);
    bank->keymaps   = (ZsndKeymap **)(base + (uint32_t)bank->keymaps);

    for (i = 0; i < bank->numVoices; i++)
        bank->voices[i].bank = *handle;
    g_zsndNumVoices += bank->numVoices;
    for (i = 0; i < bank->numSamples; i++)
        bank->samples[i].bank = *handle;
    g_zsndNumSamples += bank->numSamples;
    for (i = 0; i < bank->numPhrases; i++) {
        uint8_t *ph;
        bank->phrases[i] = (uint8_t *)((uint32_t)bank->phrases[i] + (uint32_t)a->bank);
        ph = bank->phrases[i];
        if ((ph[0] & 1) == 1)                    /* a phrase with sub-entries: relocate them */
            for (k = 0; k < ph[1]; k++)
                *(uint32_t *)(ph + 0x10 + k * 8) += (uint32_t)a->bank;
    }
    g_zsndNumPhrases += bank->numPhrases;
    for (i = 0; i < bank->numTrackDefs; i++) {
        bank->trackDefs[i].ztrk = (uint8_t *)((uint32_t)bank->trackDefs[i].ztrk + (uint32_t)a->bank);
        bank->trackDefs[i].bank = *handle;
    }
    g_zsndNumTrackDefs += bank->numTrackDefs;
    for (i = 0; i < bank->numKeymaps; i++) {
        bank->keymaps[i] = (ZsndKeymap *)((uint32_t)bank->keymaps[i] + (uint32_t)a->bank);
        bank->keymaps[i]->bank = *handle;
    }
    g_zsndNumKeymaps += bank->numKeymaps;

    if (!resident) {
        g_zsndNumStreamFiles += bank->numFiles;
        return 0;
    }
    /* resident: every sample points at its PCM data */
    for (i = 0; i < bank->numSamples; i++) {
        ZsndSample *s = &bank->samples[i];
        if (s->file == 0xFFFF)
            continue;
        if (s->file >= bank->numFiles)
            zAssertFail("invalid sample file number", WINLOAD_FILE, 0xFE, 0);
        s->data = (uint8_t *)a->bank + bank->files[s->file].offset;
    }
    g_zsndNumResidentFiles += bank->numFiles;
    return 0;
}

/* VA 0x004700b0, 58 bytes, [I] row, status draft */
void zsndUnloadAsset(uint16_t handle)
{
    int h;

    if (handle != ZSND_ASSET_ALL) {
        zsndFreeAsset(handle);
        return;
    }
    for (h = 0; h < ZSND_MAX_ASSETS; h++)
        if (g_zsndAssets[h].flags & ZSND_ASSET_LOADED)
            zsndFreeAsset((uint16_t)h);
}

/* VA 0x00470100, 35 bytes, [I] row, status draft */
int zsndIsAssetLoaded(uint16_t handle)
{
    return handle < ZSND_MAX_ASSETS && (g_zsndAssets[handle].flags & ZSND_ASSET_LOADED) ? 1 : 0;
}

/* VA 0x00470140, 104 bytes, [V] direct, status draft */
uint32_t zsndGetNumVoices(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x15D, 0x15F);
    return g_zsndAssets[handle].bank->numVoices;
}

/* VA 0x004701d0, 104 bytes, [I] row, status draft */
uint32_t zsndGetNumSamples(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x16F, 0x171);
    return g_zsndAssets[handle].bank->numSamples;
}

/* VA 0x00470260, 104 bytes, [V] direct, status draft */
uint32_t zsndGetNumSampleFiles(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x181, 0x183);
    return g_zsndAssets[handle].bank->numFiles;
}

/* VA 0x004702f0, 104 bytes, [I] row, status draft */
uint32_t zsndGetNumPhrases(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x193, 0x195);
    return g_zsndAssets[handle].bank->numPhrases;
}

/* VA 0x00470380, 104 bytes, [V] direct, status draft */
uint32_t zsndGetNumTrackDefs(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x1A5, 0x1A7);
    return g_zsndAssets[handle].bank->numTrackDefs;
}

/* VA 0x00470410, 104 bytes, [I] row, status draft */
uint32_t zsndGetNumKeymaps(uint16_t handle)
{
    CHECK_HANDLE(handle, 0x1B7, 0x1B9);
    return g_zsndAssets[handle].bank->numKeymaps;
}

/* VA 0x004704a0, 138 bytes, [V] direct, status draft */
ZsndVoice *zsndGetVoice(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x1D6, 0x1D8);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numVoices)
        zAssertFail("invalid voice number", WINLOAD_FILE, 0x1DA, 0);
    return &b->voices[index];
}

/* VA 0x00470560, 138 bytes, [V] direct, status draft */
ZsndSample *zsndGetSample(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x1EC, 0x1EE);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numSamples)
        zAssertFail("sample number out of range", WINLOAD_FILE, 0x1F0, 0);
    return &b->samples[index];
}

/* VA 0x00470620, 168 bytes, [V] direct, status draft */
ZsndFileDesc *zsndGetSampleFile(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x203, 0x205);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numFiles)
        zAssertFail("sample file number out of range", WINLOAD_FILE, 0x207, 0);
    g_zsndFileDesc.path   = g_zsndAssets[handle].path;
    g_zsndFileDesc.offset = b->files[index].offset;
    g_zsndFileDesc.size   = b->files[index].size;
    return &g_zsndFileDesc;
}

/* VA 0x00470700, 144 bytes, [I] row, status draft */
uint8_t *zsndGetPhrase(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x21B, 0x21D);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numPhrases)
        zAssertFail("phrase number out of range", WINLOAD_FILE, 0x21F, 0);
    return b->phrases[index];
}

/* VA 0x004707c0, 137 bytes, [V] direct, status draft */
ZsndTrackDef *zsndGetTrackDef(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x230, 0x232);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numTrackDefs)
        zAssertFail("track def number out of range", WINLOAD_FILE, 0x234, 0);
    return &b->trackDefs[index];
}

/* VA 0x00470880, 144 bytes, [V] direct, status draft */
ZsndKeymap *zsndGetKeymap(uint16_t handle, uint32_t index)
{
    ZsndBankHeader *b;

    CHECK_HANDLE(handle, 0x245, 0x247);
    b = g_zsndAssets[handle].bank;
    if (index >= b->numKeymaps)
        zAssertFail("keymap number out of range", WINLOAD_FILE, 0x249, 0);
    return b->keymaps[index];
}

/* VA 0x00470940, 264 bytes, [V] direct, status draft */
void zsndFreeAsset(uint16_t handle)
{
    ZsndAsset      *a = &g_zsndAssets[handle];
    ZsndBankHeader *b = a->bank;
    int             i;

    if (!(a->flags & ZSND_ASSET_LOADED))
        zAssertFail("file not loaded", WINLOAD_FILE, 0x25C, 0);
    for (i = (int)b->numVoices - 1; i >= 0; i--)       /* silence whatever still plays them */
        zsndcStopVoice(&b->voices[i]);
    g_zsndNumVoices  -= b->numVoices;
    g_zsndNumSamples -= b->numSamples;
    if (a->flags & ZSND_ASSET_RESIDENT)
        g_zsndNumResidentFiles -= b->numFiles;
    else
        g_zsndNumStreamFiles -= b->numFiles;
    g_zsndNumPhrases   -= b->numPhrases;
    g_zsndNumTrackDefs -= b->numTrackDefs;
    g_zsndNumKeymaps   -= b->numKeymaps;
    if (a->flags & ZSND_ASSET_OWNED)
        zmemFree(a->bank);
    a->flags = 0;
}

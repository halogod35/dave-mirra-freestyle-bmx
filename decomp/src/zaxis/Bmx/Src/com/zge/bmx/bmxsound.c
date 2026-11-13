/*
 * zaxis/Bmx/Src/com/zge/bmx/bmxsound.c
 * Original: C:\zaxis\Bmx\src\com\zge\bmx\bmxsound.c
 * Evidence: [V] path string 0x0055B724 (zAssertFail: "sound already open", "failure loading ...
 *           asset file", "prop/level sound type out of range", ...) in bmxsoundOpen, bmxsoundClose
 *           and bmxsoundEvent; [I] the rest: between them / called only from here, and they use the
 *           module's data (0x0055B530-0x0055B55B, 0x00574668-0x005746B3).
 *           The sound pass moved the three functions the skeleton had put first (0x0044DC30-0x0044DD23) to
 *           frontend.c: "APP: Shutdown MENU" (0x0055B518) follows frontend.c's literals and
 *           pairs with frontendInitApp's "APP: Init MENU"; they call the GUI, not the sound code.
 * 10 functions, VA 0x0044DD40-0x00450043. Draft. Non-matching C (AD-8).
 * Types: zsound.h. The event map: docs/re/sound.md section 6 (and dmbmx.formats.audio.EVENTS).
 *
 * The game's sound layer. bmxsoundOpen starts the mixer (22050 Hz stereo), the sequencer
 * (30 updates/s), the 3-D sources (range 2500, life 100 ms) and loads SFXCOM.ZSD (resident) and
 * the headers of MUSIC.ZSD / SPEECH.ZSD (streamed); a 16 ms multimedia timer runs the mixer.
 * Everything else is one entry point, bmxsoundEvent(event, a, b, c, d):
 *   1000-1005  mode / music / volumes (the mode switch loads and unloads the banks:
 *              menu = SFXMENnn (alternating 00/01, two looping voices = the menu music);
 *              game = SFXGAME + SFXLEVnn (+ SFXGAME2 and SFXLEV12 instead of the level's bank when
 *              d != 0) + SFXCHRnn (c) + a 256 KB music stream + 2x2 rolling sources + the level's
 *              ZTRK ambience tracks)
 *   2000-2009  SFXCOM menu sounds        3000-3009  rider / bike one-shots (positional)
 *   4000-4003  continuous positional     5000-5011  scoring / HUD             6000 pause
 * Music: one stream over MUSIC.ZSD's 10 IMA-ADPCM tracks; bmxsoundUpdate starts the level's track
 * (g_bmxsoundLevelTrack) and, when one ends, the next (g_bmxsoundMusicCycle).
 * The PSX reverb requests (zsndcSetEffect type 2) are refused by the PC mixer.
 * The sound pass created bmxsoundSetUpdateRate / bmxsoundProbe / bmxsoundStub (ILT thunks only) and
 * bmxsoundTimerProc (timeSetEvent's callback).
 */
#include <math.h>
#include <windows.h>
#include <mmsystem.h>

#include "decomp.h"
#include "zmath.h"
#include "zsound.h"

#define BMXSOUND_FILE "C:\\zaxis\\Bmx\\src\\com\\zge\\bmx\\bmxsound.c"
#define FAIL(msg, line) zAssertFail((msg), BMXSOUND_FILE, (line), 0)

/* ------------------------------------------------------------------ data */
int32_t     g_bmxsoundMode = -1;          /* 0x0055B530 -1 closed, 0 idle, 1 menu, 2 game, 3 paused */
int32_t     g_bmxsoundMusicMode = -1;     /* 0x0055B534 0-2 music on, 3 off, 4 fading out */
uint8_t     g_bmxsoundVolume[3] = { 0x7F, 0x7F, 0x7F };  /* 0x0055B538 sfx, music, (unused) */
uint16_t    g_bmxsoundCommonBank = 0xFFFF;               /* 0x0055B53C SFXCOM.ZSD */
uint16_t    g_bmxsoundMenuBank = 0xFFFF;                 /* 0x0055B53E SFXMENnn.ZSD */
uint16_t    g_bmxsoundGameBank = 0xFFFF;                 /* 0x0055B540 SFXGAME.ZSD */
uint16_t    g_bmxsoundGame2Bank = 0xFFFF;                /* 0x0055B542 SFXGAME2.ZSD ("gun SFX") */
uint16_t    g_bmxsoundRiderBank = 0xFFFF;                /* 0x0055B544 SFXCHRnn.ZSD */
uint16_t    g_bmxsoundLevelBank = 0xFFFF;                /* 0x0055B546 SFXLEVnn.ZSD */
uint16_t    g_bmxsoundMusicBank = 0xFFFF;                /* 0x0055B548 MUSIC.ZSD (streamed) */
uint16_t    g_bmxsoundSpeechBank = 0xFFFF;               /* 0x0055B54A SPEECH.ZSD (streamed, unused) */
int8_t      g_bmxsoundNextTrack = -1;     /* 0x0055B54C requested music track */
uint8_t     g_bmxsoundMusicCycle = 1;     /* 0x0055B54D at the end of a track go on to the next */
/* 0x0055B550: each level's first music track (MUSIC.ZSD sample file) */
uint8_t     g_bmxsoundLevelTrack[12] = { 7, 1, 5, 2, 3, 4, 8, 6, 0, 9, 7, 6 };
int8_t      g_bmxsoundTrack;              /* 0x00574668 playing music track, -1 none */
uint16_t    g_bmxsoundRollSrc2[2];        /* 0x0057466C per player: second rolling source */
uint16_t    g_bmxsoundRollSrc[2];         /* 0x00574670 per player: tyre source */
uint32_t    g_bmxsoundTimer;              /* 0x00574674 timeSetEvent id */
uint8_t     g_bmxsoundAmbience[4];        /* 0x00574678 zsndseq handles of the level's tracks */
ZsndcEffect g_bmxsoundReverb;             /* 0x00574680 */
uint32_t    g_bmxsoundTimerPeriod;        /* 0x00574694 */
uint8_t     g_bmxsoundLoopChan;           /* 0x00574698 event 3007's looping channel */
uint8_t     g_bmxsoundMenuChan[2];        /* 0x0057469C the two menu-music channels */
uint8_t     g_bmxsoundLevel;              /* 0x0057469E 0..11 */
uint8_t     g_bmxsoundCharSet;            /* 0x0057469F SFXCHRnn: 0/1 */
uint8_t     g_bmxsoundAmbienceSwap;       /* 0x005746A0 level 4: which ambience track is quiet */
ZsndStream *g_bmxsoundMusic;              /* 0x005746A4 */
int32_t     g_bmxsoundMusicDelay;         /* 0x005746A8 frames before the music may change again */
uint8_t     g_bmxsoundMenuMusic;          /* 0x005746AC alternates SFXMEN00 / SFXMEN01 */
uint32_t    g_bmxsoundLastTick;           /* 0x005746B0 */

#define SFX_VOL   g_bmxsoundVolume[0]
#define MUSIC_VOL g_bmxsoundVolume[1]
#define PRIO_GAME 0x20                    /* positional one-shots */
#define PRIO_UI   0x21                    /* menu / HUD sounds (outrank the game's) */
#define PRIO_MENU 0x22                    /* the menu music */

extern void *g_zrwHwndQuery;              /* zrwZgl 0x00561488: function pointer, query 0x15 = HWND */

/* VA 0x0044dd40, 12 bytes, [I] row, status draft */
void bmxsoundSetUpdateRate(uint32_t hz)
{
    zsndSetUpdateRate(hz);
}

/* VA 0x0044dd60, 14 bytes, [I] row, status draft */
int bmxsoundProbe(void *hwnd)
{
    return zsndcProbe(hwnd);
}

/* VA 0x0044dd80, 14 bytes, [I] row, status draft */
int bmxsoundStub(int unused)
{
    return zsndStub();
}

/* VA 0x0044dda0, 591 bytes, [V] direct, status draft */
int bmxsoundOpen(void)
{
    ZsndcInitParams init;
    TIMECAPS        caps;
    void          **hwnd;
    UINT            period;

    if (g_bmxsoundMode != -1)
        FAIL("sound already open", 0xFC);
    zstringMemSet(&init, 0, sizeof(init));
    ((void (__cdecl *)(int, void ***, int))g_zrwHwndQuery)(0x15, &hwnd, 0);
    init.hwnd = *hwnd;
    init.flags = 0;
    init.rate = 22050;
    if (zsndcInit(&init) != 0)
        FAIL("failure initializing channel manager", 0x111);
    g_zsndUpdateRate = 30;
    if (zsndSeqInit() != 0)
        FAIL("failure initializing sequenced audio manager", 0x114);
    if (zsndSrcInit(2500, 100) != 0)
        FAIL("failure initializing 3-d sound source manager", 0x116);
    if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", "SFXCOM.ZSD"), 1, &g_bmxsoundCommonBank) != 0)
        FAIL("failure loading common SFX asset file", 0x11A);
    if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", "MUSIC.ZSD"), 0, &g_bmxsoundMusicBank) != 0)
        FAIL("failure loading music asset file", 0x11D);
    if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", "SPEECH.ZSD"), 0, &g_bmxsoundSpeechBank) != 0)
        FAIL("failure loading speech asset file", 0x120);
    g_bmxsoundMode = 0;
    if (timeGetDevCaps(&caps, sizeof(caps)) != 0) {
        FAIL("error getting timer device capabilities", 0x130);
        return 0;
    }
    period = caps.wPeriodMin < 2 ? 1 : caps.wPeriodMin;
    if (period >= caps.wPeriodMax)
        caps.wPeriodMin = caps.wPeriodMax;
    else if (caps.wPeriodMin < 2)
        caps.wPeriodMin = 1;
    g_bmxsoundTimerPeriod = caps.wPeriodMin;
    timeBeginPeriod(g_bmxsoundTimerPeriod);
    g_bmxsoundTimer = timeSetEvent(16, g_bmxsoundTimerPeriod, (LPTIMECALLBACK)bmxsoundTimerProc, 0, TIME_PERIODIC);
    if (g_bmxsoundTimer == 0)
        FAIL("error creating timer event", 300);
    return timeEndPeriod(g_bmxsoundTimerPeriod);
}

/* VA 0x0044e090, 98 bytes, [V] direct, status draft */
void bmxsoundClose(void)
{
    if (g_bmxsoundMode == -1)
        FAIL("sound already closed", 0x13E);
    timeKillEvent(g_bmxsoundTimer);
    timeEndPeriod(g_bmxsoundTimerPeriod);
    zsndUnloadAsset(ZSND_ASSET_ALL);
    zsndSrcShutdown();
    zsndSeqShutdown();
    zsndcShutdown();
    g_bmxsoundMode = -1;
}

/* VA 0x0044e120, 338 bytes, [I] row, status draft
 * Once per game frame: start / change the music, then the sequencer, the sources, the fades. */
void bmxsoundUpdate(void)
{
    uint32_t state;

    if (g_bmxsoundMusicDelay)
        g_bmxsoundMusicDelay--;
    if (g_bmxsoundMode == -1)
        return;
    if (g_bmxsoundMode == 2 && g_bmxsoundMusicMode >= 0 && g_bmxsoundMusicMode < 3 &&
        (zsndStreamGetState(g_bmxsoundMusic, &state), state == ZSND_STREAM_STOPPED || g_bmxsoundNextTrack != -1) &&
        g_bmxsoundMusicDelay == 0) {
        if (g_bmxsoundNextTrack == -1) {
            if (g_bmxsoundTrack == -1) {
                g_bmxsoundTrack = (int8_t)g_bmxsoundLevelTrack[g_bmxsoundLevel];
            } else if (g_bmxsoundMusicCycle) {
                g_bmxsoundTrack++;
                if (g_bmxsoundTrack >= (int)zsndGetNumSampleFiles(g_bmxsoundMusicBank))
                    g_bmxsoundTrack = 0;
            }
            g_bmxsoundNextTrack = g_bmxsoundTrack;
        }
        /* the music: IMA-ADPCM mono at 44100 Hz (flags 0x40) */
        zsndStreamSetFile(g_bmxsoundMusic, zsndGetSampleFile(g_bmxsoundMusicBank, g_bmxsoundNextTrack), ZSND_STRM_44K);
        zsndStreamSetVolume(g_bmxsoundMusic, (uint8_t)(MUSIC_VOL * 15 >> 4), 0);
        zsndStreamPlay(g_bmxsoundMusic);
        g_bmxsoundTrack = g_bmxsoundNextTrack;
        g_bmxsoundNextTrack = -1;
        g_bmxsoundMusicDelay = 60;
    }
    zsndSeqUpdate();
    zsndSrcUpdate();
    zsndStreamUpdate();
}

/* VA 0x0044e2e0, 15 bytes, [I] row, status draft */
void bmxsoundService(void)
{
    if (g_bmxsoundMode == -1)
        return;
    zsndStreamService();
}

static void ambienceVolumes(void)
{
    uint32_t i;
    uint8_t  vol;

    for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++) {
        vol = SFX_VOL;
        if (g_bmxsoundLevel == 4 && (uint32_t)(i == 0) == g_bmxsoundAmbienceSwap)
            vol = (uint8_t)(SFX_VOL * 20 >> 7);          /* the other room's track, nearly off */
        zsndSeqSetVolume(g_bmxsoundAmbience[i], vol, 0);
    }
}

static void setReverb(int8_t mode, int8_t depth)
{
    g_bmxsoundReverb.type = 2;                           /* PSX parameters: refused (0x10D) */
    g_bmxsoundReverb.level = mode;
    g_bmxsoundReverb.psxDepth = depth;
    zsndcSetEffect(&g_bmxsoundReverb);
}

/* volume of a rolling source from a speed-like value */
static uint8_t rollVolume(uint32_t v)
{
    if (v == 0)
        return 0;
    return v < 32 ? (uint8_t)(v * 4 * SFX_VOL >> 7) : SFX_VOL;
}

/* events 1000 (BMXSND_SET_MODE): a = new mode; for 2: b = level, c = SFXCHR set, d = gun bank */
static int setMode(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    char     name[32];
    RwV3d    farPos;      /* not "far": <windows.h> defines it away */
    uint32_t i;

    if ((uint32_t)g_bmxsoundMode == a)
        return 0;
    if (g_bmxsoundMode == 1) {                           /* leaving the menu */
        zsndcFree(g_bmxsoundMenuChan[0]);
        zsndcFree(g_bmxsoundMenuChan[1]);
        zsndUnloadAsset(g_bmxsoundMenuBank);
        g_bmxsoundMenuBank = 0xFFFF;
    } else if (g_bmxsoundMode > 1 && g_bmxsoundMode < 4) {
        if (a < 2) {                                     /* leaving the game */
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++) {
                zsndSeqFreeTrack(g_bmxsoundAmbience[i]);
                g_bmxsoundAmbience[i] = 0xFF;
            }
            zsndStreamStop(g_bmxsoundMusic);
            for (i = 0; i < 2; i++) {
                zsndSrcFree(g_bmxsoundRollSrc[i]);
                g_bmxsoundRollSrc[i] = 0xFFFF;
                zsndSrcFree(g_bmxsoundRollSrc2[i]);
                g_bmxsoundRollSrc2[i] = 0xFFFF;
            }
            if (g_bmxsoundLoopChan != 0xFF) {
                zsndcFree(g_bmxsoundLoopChan);
                g_bmxsoundLoopChan = 0xFF;
            }
            zsndStreamDestroy(g_bmxsoundMusic);
            g_bmxsoundMusic = 0;
            zsndUnloadAsset(g_bmxsoundRiderBank);
            g_bmxsoundRiderBank = 0xFFFF;
            zsndUnloadAsset(g_bmxsoundLevelBank);
            g_bmxsoundLevelBank = 0xFFFF;
            if (g_bmxsoundGame2Bank != 0xFFFF) {
                zsndUnloadAsset(g_bmxsoundGame2Bank);
                g_bmxsoundGame2Bank = 0xFFFF;
            }
            zsndUnloadAsset(g_bmxsoundGameBank);
            g_bmxsoundGameBank = 0xFFFF;
            zsndStreamShutdown();
            setReverb(0, 0);
        } else if (a == 3) {                             /* pause */
            zsndcPlayVoice(zsndGetVoice(g_bmxsoundGameBank, 110), 0, SFX_VOL, 0, PRIO_UI);
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++)
                zsndSeqPause(g_bmxsoundAmbience[i]);
            if (g_bmxsoundMusicMode >= 0 && g_bmxsoundMusicMode < 3 && g_bmxsoundTrack != -1)
                zsndStreamPause(g_bmxsoundMusic, 1);
        }
    }

    if (a == 1) {                                        /* the menu: its music is two loops */
        zstringPrintf(name, "SFXMEN%02d.ZSD", g_bmxsoundMenuMusic);
        if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", name), 1, &g_bmxsoundMenuBank) != 0)
            FAIL("failure loading menu SFX asset file", 0x281);
        g_bmxsoundMenuChan[0] = zsndcAlloc(zsndGetVoice(g_bmxsoundMenuBank, 0), PRIO_MENU);
        g_bmxsoundMenuChan[1] = zsndcAlloc(zsndGetVoice(g_bmxsoundMenuBank, 1), PRIO_MENU);
        zsndcSetVolume(g_bmxsoundMenuChan[0], MUSIC_VOL);
        zsndcSetVolume(g_bmxsoundMenuChan[1], MUSIC_VOL);
        zsndcKeyOn(g_bmxsoundMenuChan[0]);
        zsndcKeyOn(g_bmxsoundMenuChan[1]);
        g_bmxsoundMenuMusic = (uint8_t)((g_bmxsoundMenuMusic - 1) & 1);   /* the other one next time */
    } else if (a == 2 && g_bmxsoundMode >= 0) {
        if (g_bmxsoundMode < 2) {                        /* entering the game */
            g_bmxsoundMusicMode = 0;
            g_bmxsoundLevel = (uint8_t)(b < 12 ? b : 0);
            g_bmxsoundCharSet = (uint8_t)(c < 2 ? c : 0);
            if (g_bmxsoundLevel == 4) {
                setReverb(2, 0x46);
                g_bmxsoundAmbienceSwap = 0;
            } else {
                setReverb(1, 0x11);
                g_bmxsoundAmbienceSwap = 1;
            }
            zsndStreamInit();
            if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", "SFXGAME.ZSD"), 1, &g_bmxsoundGameBank) != 0)
                FAIL("failure loading game SFX asset file", 0x2C8);
            if (d == 0) {
                zstringPrintf(name, "SFXLEV%02d.ZSD", g_bmxsoundLevel);
            } else {
                if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", "SFXGAME2.ZSD"), 1, &g_bmxsoundGame2Bank) != 0)
                    FAIL("failure loading gun SFX asset file", 0x2D1);
                zstringPrintf(name, "SFXLEV%02d.ZSD", 12);   /* the gun mode's own level bank */
            }
            g_bmxsoundLoopChan = 0xFF;
            if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", name), 1, &g_bmxsoundLevelBank) != 0)
                FAIL("failure loading level SFX asset file", 0x2D7);
            zstringPrintf(name, "SFXCHR%02d.ZSD", g_bmxsoundCharSet);
            if (zsndLoadAsset(zfileBuildPath("bin\\audio\\", name), 1, &g_bmxsoundRiderBank) != 0)
                FAIL("failure loading character SFX asset file", 0x2DE);
            /* the music stream: 256 KB buffer, IMA-ADPCM mono 44100 Hz, starts with track 0 */
            if (zsndStreamCreate(&g_bmxsoundMusic, zsndGetSampleFile(g_bmxsoundMusicBank, 0), 0, 0x40000,
                                 ZSND_STRM_44K | 0x04) != 0)
                FAIL("failure creating music stream (probably out of SPU RAM)", 0x2F1);
            g_bmxsoundTrack = -1;
            g_bmxsoundNextTrack = -1;
            zsndStreamSetVolume(g_bmxsoundMusic, (uint8_t)(MUSIC_VOL * 15 >> 4), 0);
            /* the rolling sources start far away (silent) */
            farPos.x = 0.0f;
            farPos.y = 20000.0f;
            farPos.z = 0.0f;
            for (i = 0; i < 2; i++) {
                g_bmxsoundRollSrc[i] = zsndSrcCreate(zsndGetVoice(g_bmxsoundGameBank, 0), &farPos);
                zsndSrcSetVolume(g_bmxsoundRollSrc[i], SFX_VOL);
                g_bmxsoundRollSrc2[i] = zsndSrcCreate(zsndGetVoice(g_bmxsoundGameBank, 9), &farPos);
                zsndSrcSetVolume(g_bmxsoundRollSrc2[i], SFX_VOL);
            }
            /* the level ambience: every ZTRK track of the level bank */
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++) {
                uint8_t vol = SFX_VOL;
                g_bmxsoundAmbience[i] = zsndSeqCreateTrack(zsndGetTrackDef(g_bmxsoundLevelBank, i), 0);
                if (g_bmxsoundLevel == 4 && (uint32_t)(i == 0) == g_bmxsoundAmbienceSwap)
                    vol = (uint8_t)(SFX_VOL * 20 >> 7);
                zsndSeqSetVolume(g_bmxsoundAmbience[i], vol, 0);
                zsndSeqPlay(g_bmxsoundAmbience[i]);
            }
        } else if (g_bmxsoundMode == 3) {                /* unpause */
            if (g_bmxsoundMusicMode >= 0 && g_bmxsoundMusicMode < 3) {
                if (g_bmxsoundTrack != -1)
                    zsndStreamPlay(g_bmxsoundMusic);
            } else if (g_bmxsoundMusicMode == 4) {
                zsndStreamStop(g_bmxsoundMusic);
            }
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++)
                zsndSeqPlay(g_bmxsoundAmbience[i]);
        }
    }
    g_bmxsoundMode = (int32_t)a;
    return 0;
}

/* VA 0x0044e300, 5747 bytes, [V] direct, status draft
 * The game's only way to make a sound. Returns 0 (handled), 1 (unknown event), or a value. */
int bmxsoundEvent(uint32_t event, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint16_t bank;
    uint32_t voice, rnd, i, player;
    int16_t  cents = 0;
    uint8_t  vol, prio, level;
    int8_t   pan = 0;
    RwV3d    pos;

    if (g_bmxsoundMode == -1)
        return 0;
    rnd = zmathRandom();
    if (g_bmxsoundMode == 2) {
        if (g_bmxsoundMusicMode == 2 && event > 4999 && event < 6000)
            return 0;                         /* music mode 2 mutes the scoring sounds */
    } else if (g_bmxsoundMode == 3 && event > 3000 && event < 4001) {
        return 0;                             /* paused: no rider sounds */
    }

    switch (event) {
    /* ---------------------------------------------------------------- control */
    case BMXSND_SET_MODE:
        return setMode(a, b, c, d);
    case BMXSND_SET_MUSIC:
        if ((uint32_t)g_bmxsoundMusicMode == a)
            return 0;
        if (g_bmxsoundMode == 2 && g_bmxsoundMusicMode >= 0 && g_bmxsoundMusicMode < 3) {
            if (a != 3) {
                zsndStreamFade(g_bmxsoundMusic, 750, 0, 2);   /* fade out, then stop */
                g_bmxsoundMusicMode = (int32_t)a;
                g_bmxsoundMusicDelay = 30;
                return 0;
            }
            zsndStreamStop(g_bmxsoundMusic);
        }
        g_bmxsoundMusicMode = (int32_t)a;
        return 0;
    case BMXSND_SET_VOLUMES:
        g_bmxsoundVolume[a] = (uint8_t)b;
        if (a == 0) {
            if (g_bmxsoundMode >= 2 && g_bmxsoundMode <= 3)
                ambienceVolumes();
            return 0;
        }
        if (a != 1)
            return a == 2 ? 0 : 2;
        if (g_bmxsoundMode == 1) {
            zsndcSetVolume(g_bmxsoundMenuChan[0], MUSIC_VOL);
            zsndcSetVolume(g_bmxsoundMenuChan[1], MUSIC_VOL);
        } else if (g_bmxsoundMode >= 2 && g_bmxsoundMode < 4) {
            zsndStreamSetVolume(g_bmxsoundMusic, (uint8_t)(MUSIC_VOL * 15 >> 4), 0);
        }
        return 0;
    case BMXSND_SET_REVERB:
        setReverb((int8_t)a, (int8_t)b);
        return 0;
    case BMXSND_GET_TRACK:
        return g_bmxsoundNextTrack == -1 ? g_bmxsoundTrack : g_bmxsoundNextTrack;
    case BMXSND_SET_TRACK:
        if (a >= zsndGetNumSampleFiles(g_bmxsoundMusicBank) - 1)
            a = zsndGetNumSampleFiles(g_bmxsoundMusicBank) - 1;
        if ((int32_t)a == g_bmxsoundTrack || g_bmxsoundMusicMode < 0)
            return 0;
        if (g_bmxsoundMusicMode < 2)
            g_bmxsoundNextTrack = (int8_t)a;
        return 0;

    /* ---------------------------------------------------------------- menu (SFXCOM) */
    case 2000: case 2001: case 2002: case 2004: case 2005:
        bank = g_bmxsoundCommonBank; voice = event - 2000; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 2003:
        bank = g_bmxsoundCommonBank; voice = g_bmxsoundMode == 1 ? 5 : 3; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 2008: case 2009:
        return 0;                             /* silent on the PC */

    /* ---------------------------------------------------------------- rider / bike, a = position */
    case 3000:                                /* any SFXCOM voice b (unused) */
        if (a != 0 || b >= zsndGetNumVoices(g_bmxsoundCommonBank))
            return 0;
        bank = g_bmxsoundCommonBank; voice = b; vol = SFX_VOL; prio = PRIO_GAME;
        break;
    case 3001:                                /* b = kind, c = strength */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        bank = g_bmxsoundGameBank; voice = (b & 0xFF) + 0x15; vol = (uint8_t)(level * c >> 7); prio = PRIO_GAME;
        break;
    case 3002:                                /* b = surface << 8 | kind, c = strength */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        switch ((b >> 8) & 0xFF) {
        case 2:  voice = (b & 0xFF) + 0x31; break;
        case 3:  voice = 0x3A; break;
        case 4:  voice = 0x3C; break;
        case 5:  voice = 0x3B; break;
        default: voice = (b & 0xFF) + 0x28; break;
        }
        bank = g_bmxsoundGameBank; vol = (uint8_t)(level * c >> 7); prio = PRIO_GAME;
        break;
    case 3003:
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        bank = g_bmxsoundGameBank; voice = (b & 0xFF) + 0x1F; vol = (uint8_t)(level * c >> 7); prio = PRIO_GAME;
        break;
    case 3004:                                /* impacts: b >> 8 = material, c = force (3 grades) */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        voice = (c & ~0x1Fu) <= 0x40 ? c >> 5 : 2;
        voice += ((b >> 8) & 0xFF) * 3 + 0x3D;
        bank = g_bmxsoundGameBank; vol = (uint8_t)(level * c >> 7); prio = PRIO_GAME;
        break;
    case 3005:                                /* body hits, b = 0..3 */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        bank = g_bmxsoundGameBank; voice = b + 0x58; vol = (uint8_t)((c * 0x20 + 0x1F) * level >> 7); prio = PRIO_GAME;
        break;
    case 3006:                                /* rider voice: one of SFXCHRnn's four at random */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        bank = g_bmxsoundRiderBank; voice = rnd & 3; cents = (int16_t)(c << 6);
        vol = (uint8_t)((c * 0x10 + 0x4F) * level >> 7); prio = PRIO_GAME;
        break;
    case 3007:                                /* SFXGAME2 loop (no caller in this build) */
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        if (g_bmxsoundLoopChan == 0xFF) {
            g_bmxsoundLoopChan = zsndcAlloc(zsndGetVoice(g_bmxsoundGame2Bank, b), PRIO_UI);
            if (g_bmxsoundLoopChan == 0xFF)
                return 0;
        }
        zsndcSetVolume(g_bmxsoundLoopChan, level);
        zsndcSetPan(g_bmxsoundLoopChan, pan);
        zsndcKeyOn(g_bmxsoundLoopChan);
        return 0;
    case 3008:                                /* SFXGAME2 4+b / 10+b (no caller) */
    case 3009:
        bmxsoundPlace((RwV3d *)a, SFX_VOL, &level, &pan);
        bank = g_bmxsoundGame2Bank; voice = b + (event == 3008 ? 4 : 10); vol = level; prio = PRIO_GAME;
        break;

    /* ---------------------------------------------------------------- continuous positional */
    case 4000:  /* rolling: a = position, b = player << 28 | kind bits, c / d = the two speeds */
        pos = *(RwV3d *)a;
        player = b >> 28;
        voice = b & 0xFF;
        vol = rollVolume(c);
        cents = c ? (int16_t)(c * 8 - 416) : 0;
        if (((b >> 8) & 0xFF) == 1) {
            if ((b >> 24) & 0xF)
                voice = 11;
            zsndSrcSetVoice(g_bmxsoundRollSrc[player], zsndGetVoice(g_bmxsoundGameBank, voice));
        } else if (((b >> 8) & 0xFF) == 2) {
            zsndSrcSetVoice(g_bmxsoundRollSrc[player], zsndGetVoice(g_bmxsoundGameBank, voice + 12));
        }
        zsndSrcSetVolume(g_bmxsoundRollSrc[player], vol);
        zsndSrcSetPitch(g_bmxsoundRollSrc[player], cents);
        vol = rollVolume(d);
        cents = d ? (int16_t)(d * 8 - 416) : 0;
        zsndSrcSetVoice(g_bmxsoundRollSrc2[player], zsndGetVoice(g_bmxsoundGameBank, ((b >> 16) & 0xFF) + 9));
        zsndSrcSetVolume(g_bmxsoundRollSrc2[player], vol);
        zsndSrcSetPitch(g_bmxsoundRollSrc2[player], cents);
        zsndSrcSetPosition(g_bmxsoundRollSrc[player], &pos);
        zsndSrcSetPosition(g_bmxsoundRollSrc2[player], &pos);
        return 0;
    case 4001:  /* camera position: level 4 has an inside and an outside ambience */
        if (!a)
            FAIL("NULL pointer passed as positional SFX argument", 0x4C7);
        pos = *(RwV3d *)a;
        if (g_bmxsoundLevel != 4)
            return 0;
        if (pos.x <= 0.0f) {
            if (g_bmxsoundAmbienceSwap)
                return 0;
            setReverb(2, 1);
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++)
                zsndSeqSetVolume(g_bmxsoundAmbience[i], i == 0 ? (uint8_t)(SFX_VOL * 20 >> 7) : SFX_VOL, 0);
            g_bmxsoundAmbienceSwap = 1;
            return 0;
        }
        if (g_bmxsoundAmbienceSwap) {
            setReverb(2, 0x46);
            for (i = 0; i < zsndGetNumTrackDefs(g_bmxsoundLevelBank); i++)
                zsndSeqSetVolume(g_bmxsoundAmbience[i], i != 0 ? (uint8_t)(SFX_VOL * 20 >> 7) : SFX_VOL, 0);
            g_bmxsoundAmbienceSwap = 0;
        }
        if (zsndGetNumTrackDefs(g_bmxsoundLevelBank) == 0)
            return 0;
        {   /* track 0 fades with the height difference to y = -2000 (never below 50) */
            float k = (float)fabs(pos.y - -2000.0f) * 0.042333335f;
            level = 127 - (int32_t)k < 50 ? 50 : (uint8_t)(127 - (int32_t)k);
        }
        zsndSeqSetVolume(g_bmxsoundAmbience[0], (uint8_t)(SFX_VOL * level >> 7), 0);
        return 0;
    case 4002:  /* props: a = position, b = op, c = kind 0..3 (op 0) or source handle, d = volume */
    case 4003:
        if (!a)
            FAIL("NULL pointer passed as positional SFX argument", 0x46C);
        pos = *(RwV3d *)a;
        voice = event != 4002 ? 8 : 0;
        switch (b) {
        case 0: {                                        /* create a looping source */
            uint16_t h;
            if (c > 3)
                FAIL("prop/level sound type out of range", 0x472);
            h = zsndSrcCreate(zsndGetVoice(g_bmxsoundLevelBank, voice + 4 + c), &pos);
            if (h != 0xFFFF)
                zsndSrcSetVolume(h, (uint8_t)(SFX_VOL * d >> 7));
            return h;
        }
        case 1:                                          /* free it */
            if ((uint16_t)c != 0xFFFF)
                zsndSrcFree((uint16_t)c);
            return 0;
        case 2:                                          /* move it */
            if ((uint16_t)c == 0xFFFF)
                return 0;
            zsndSrcSetVolume((uint16_t)c, (uint8_t)(SFX_VOL * d >> 7));
            zsndSrcSetPosition((uint16_t)c, &pos);
            return 0;
        case 3:                                          /* a one-shot */
            bmxsoundPlace((RwV3d *)a, (uint8_t)(SFX_VOL * d >> 7), &level, &pan);
            if (c > 3)
                FAIL("prop/level sound type out of range", 0x485);
            bank = g_bmxsoundLevelBank; voice += c; vol = level; prio = PRIO_GAME;
            break;
        default:
            return 0;
        }
        break;

    /* ---------------------------------------------------------------- scoring / HUD (SFXGAME) */
    case 5000:                                /* timer: a = seconds left */
        if (a > 10)
            return 0;
        bank = g_bmxsoundGameBank; voice = a == 0 ? 0x5D : 0x5C; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 5001:                                /* trick in a combo: rising pitch */
        bank = g_bmxsoundGameBank; voice = 0x5E; cents = (int16_t)(a * 33 > 499 ? 500 : a * 33);
        vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 5002:
        return 0;
    case 5003:                                /* crowd: a = excitement 0..7, b = big */
        level = (uint8_t)(a * 16 + 0x1F);
        cents = (int16_t)(a * 30 - (rnd & 0x1FF) + 196);
        voice = (b ? 0x11 : 0x13) - (a < 4);
        bank = g_bmxsoundLevelBank; vol = (uint8_t)(SFX_VOL * level >> 7); prio = PRIO_GAME;
        break;
    case 5004:
        bank = g_bmxsoundGameBank; voice = a == 0 ? 0x60 : a == 1 ? 0x5F : 0x61; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 5005:
        bank = g_bmxsoundGameBank; voice = a + 0x61; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 5006:
        bank = g_bmxsoundGameBank; voice = a + 100; vol = SFX_VOL; prio = PRIO_UI;
        break;
    case 5007: case 5008: case 5009: case 5010:
        bank = g_bmxsoundGameBank; voice = event - 0x1327; vol = SFX_VOL; prio = PRIO_UI;   /* 104..107 */
        break;
    case 5011:                                /* two at once */
        zsndcPlayVoice(zsndGetVoice(g_bmxsoundGameBank, 0x6C), 0, SFX_VOL, 0, PRIO_GAME);
        bank = g_bmxsoundGameBank; voice = 0x6D; vol = SFX_VOL; prio = PRIO_GAME;
        break;
    case BMXSND_PAUSE:
        bank = g_bmxsoundGameBank; voice = 0x6F; vol = SFX_VOL; prio = PRIO_UI;
        break;
    default:
        return 1;
    }
    zsndcPlayVoice(zsndGetVoice(bank, voice), cents, vol, pan, prio);
    return 0;
}

/* VA 0x0044ffb0, 75 bytes, [I] row, status draft
 * Level and pan of a one-shot at pos (no position: full level, centre). */
int bmxsoundPlace(const RwV3d *pos, uint8_t volume, uint8_t *outVolume, int8_t *outPan)
{
    int rc;

    if (!pos) {
        *outVolume = volume;
        *outPan = 0;
        return 1;
    }
    rc = zsndSrcGetLevelPan(pos, outVolume, outPan);
    *outVolume = (uint8_t)(*outVolume * volume >> 7);
    return rc;
}

/* VA 0x00450020, 36 bytes, [I] row, status draft
 * The 16 ms multimedia timer: run the mixer (at most every 4 ms). */
void __stdcall bmxsoundTimerProc(uint32_t id, uint32_t msg, uint32_t user, uint32_t dw1, uint32_t dw2)
{
    if (timeGetTime() - g_bmxsoundLastTick > 3) {
        zsndcUpdate();
        g_bmxsoundLastTick = timeGetTime();
    }
}

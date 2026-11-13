/*
 * zaxis/Bmx/Src/com/action/action.c
 * Original: C:\zaxis\Bmx\Src\com\action\action.c
 * Evidence: [V] path string 0x00551A64, referenced by the asserts of actionStartup (line 0xBF),
 *           actionLoad (0x1F2) and actionSetMode (0x6BC-0x6C2); [I] for actionPickDemoLevel,
 *           actionUpdate, actionShutdown ("APP: Shutdown ACTION") and actionGetMode (link order,
 *           the action globals 0x00551A30-0x00551A43 and 0x0056B3E0-0x0056B3FB).
 * 8 functions, 5072 bytes, VA 0x00403c30-0x004056aa (the clean-up created actionServiceSound 0x004040C0 and documented
 * the thunk-only code at 0x00403CC0).
 * Draft. Non-matching C (AD-8). Description: docs/re/input-camera.md.
 *
 * The camera pass moved the skeleton's action_004056c0 / action_00405780 / action_00407470 to camera.c
 * (cameraInit, cameraUpdate, cameraApply): they only touch the camera globals.
 *
 * The "action" layer is the in-level game loop. actionStartup runs once (motion library, save
 * defaults); actionLoad builds a level (pads, physics, riders, overlays, camera, motion table,
 * props, game rules); actionUpdate is the per-frame entry (NgApp): it reads the pads, then
 * depending on the action mode simulates the riders (riderReadInput -> riderUpdateAll ->
 * bmxphysUpdate -> props -> gameUpdate), runs the pause menu, the replay viewer or the attract
 * demo, and finally updates the camera. actionSetMode switches between those modes.
 */

#include "decomp.h"

#include "physics.h"

#include "contrler.h"

#include "camera.h"

#define ACTION_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\action.c"

void zactorCloseMotionLibrary(void);   /* 0x00475000 zactor.c */
void bmxsoundService(void);            /* 0x0044E2E0 bmxsound.c */

/* g_actionMode (actionGetMode). [V] values and the actionUpdate / actionSetMode switches; names [I]. */
enum {
    ACTION_PLAY    = 0,   /* riding: the riders are simulated */
    ACTION_PAUSED  = 1,   /* pause menu (its replay screens 9-11 / 25 run the replay behind it) */
    ACTION_REPLAY  = 2,   /* "view replay" of the run (gameGetViewReplay) */
    ACTION_REPLAY3 = 3,   /* replay of g_actionInstantReplay [I]: nothing in the exe writes that handle */
    ACTION_DEMO    = 4,   /* attract mode: no players, a recorded run from replay.zal */
    ACTION_NONE    = 0xFF /* actionSetMode(-1) (overlay.c, leaving the level) */
};

/* g_actionReplayState: the replay viewer (modes 2/3) and the demo (mode 4). [V] values. */
enum {
    REPLAY_FADE_IN  = 0,  /* 30 frames */
    REPLAY_PLAYING  = 1,
    REPLAY_FADE_OUT = 2,  /* 30 frames, then leave */
    REPLAY_PAUSED   = 3,  /* Square: frozen */
    REPLAY_RESTART  = 4,  /* Triangle: started over; 30 frames, then playing */
    REPLAY_ORBIT    = 5,  /* Circle while paused: frozen, the camera circles the rider */
    REPLAY_ENDED    = 6,  /* reached the end; 30 frames, then fade out */
    REPLAY_CRASH    = 7   /* 240 frames of crash physics, then fade out (set by no code in this file) */
};

/* HUD items (overlayShowItem, 0..16) that a mode switch leaves alone, as bit masks */
#define HUD_KEEP_PLAY    ((1u << 0) | (1u << 9) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16))
#define HUD_KEEP_PAUSE   ((1u << 0) | (1u << 9) | (1u << 13) | (1u << 14) | (1u << 16))
#define HUD_KEEP_REPLAY  (HUD_KEEP_PAUSE | (1u << 1) | (1u << 2) | (1u << 10) | (1u << 15))

/* ------------------------------------------------------------------ other modules (current names) */
int      zrollerStartup(void);
int      zrollerInit(void);                                 /* level resources: 1 = failed */
void     zrollerSetLocal(int);
void     zrollerLoadCodeModule(int);
void     zrollerInitializeLoadingScreen(int);
void     zrollerSetMap(uint8_t group);                     /* level resource group */
int      zrollerLoadAssets(void);                              /* load the level assets, 0 = ok */
void     zrollerShutdownLoadingScreen(void);
int      zrollerRunDrawCallbacks(void);                                      /* run the frame callbacks 0x005FB6C8[8] */
void     ngappEmptyHook(void);                                      /* empty */
int      zrollerShutdown(void);                                 /* level resources: free */
void     zsystemSetMenuMode(void);                               /* zsystemSetMenuMode: back to the menus */
void     zconsoleLog(const char *msg);                    /* debug message */
void     overlayLoadFonts(void);
void     overlayLoadStrings(uint8_t language);
void     overlayInit(void);
void     overlayShutdown(void);
int      overlayUpdate(void);                                  /* the open overlay screen */
void     overlayShowItem(int item, uint32_t on);                 /* show / hide a HUD item */
void     overlaySetItem(int item, const void *data);            /* set a HUD item */
uint8_t  overlayGetScreen(void);                                  /* current overlay screen */
void     overlaySetScreen(uint32_t screen);                       /* open an overlay screen */
uint32_t bmxsoundEvent(int cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
BmxRider *riderCreate(int slot, int rider, int style, uint32_t bike);
void     riderReadInput(void);
void     riderUpdateAll(void);
void     gameInit(void);
void     gameShutdown(void);
void     gameUpdate(void);
uint8_t  gameStart(uint8_t mode);
uint8_t  gameSetSubMode(uint8_t sub);
uint8_t  gameGetViewReplay(void);
void     gameResetRider(int player, int newRun);

extern ZassetLib *g_replayZal;                                    /* 0x0068FFA8 */
extern void      *g_motionTable[239];                             /* 0x006962FC, [1..238] */
extern const uint8_t g_levelResGroup[12];                         /* 0x00551FB8 */
extern uint8_t    g_selectedSubMode;                              /* 0x0056B616 [I] frontend's game sub-mode */
extern uint32_t   g_appQuit;                                      /* 0x00551FD4 [I] set by Start+Select */
extern uint32_t   g_actionUnk3e4;                                 /* 0x0056B3E4 cleared only */
extern uint8_t    g_actionUnk3e8;                                 /* 0x0056B3E8 cleared only */
extern uint32_t   g_actionUnk3f0;                                 /* 0x0056B3F0 cleared only */

/* this file */
int      actionStartup(void);
uint32_t actionPickDemoLevel(void);
int      actionLoad(void);
int      actionUpdate(void);
void     actionShutdown(void);
uint32_t actionSetMode(uint32_t mode);
uint32_t actionGetMode(void);

/* ------------------------------------------------------------------ globals (this file) */
uint8_t  g_actionMode;                    /* 0x00551A30 ACTION_* */
const uint8_t g_actionDemoRuns[3][2] = {  /* 0x00551A34 {rider, level}: Dave Mirra / Woodward Trails, */
    { 0, 4 }, { 1, 0 }, { 1, 1 }          /*   Ryan Nyquist / Greenville Park, Ryan Nyquist / Vail Trails */
};
int32_t  g_actionFrameSkip = 1;           /* 0x00551A3C update every n-th call; -1 = debug: hold R2 on pad 0 */
int32_t  g_actionFrameCount = 1;          /* 0x00551A40 */
const uint8_t g_levelResGroup2P[12] = { 18, 20, 22, 19, 21, 23, 13, 15, 17, 12, 14, 16 };  /* 0x00551FC4 */
uint8_t  g_actionReplayState;             /* 0x0056B3E0 REPLAY_* */
uint8_t  g_actionDemoReplay;              /* 0x0056B3E1 replay handle of the demo */
uint8_t  g_actionInstantReplay;           /* 0x0056B3E2 */
uint8_t  g_actionSavedShot;               /* 0x0056B3E9 camera shot before Circle (orbit) */
uint32_t g_actionTimer;                   /* 0x0056B3EC frames in the current replay state */
uint32_t g_actionOverlaysOn;              /* 0x0056B3F4 HUD shown (Select toggles it in replays) */
uint8_t  g_actionDemoCount;               /* 0x0056B3F8 demos shown (selects g_actionDemoRuns[n % 3]) */
uint16_t g_actionQuitFrames;              /* 0x0056B3FA frames Start + Select have been held */
uint8_t  g_actionPausePlayer;             /* 0x006962F5 only this player controls menus / replays (0xFF any) */

static void actionShowOverlays(uint32_t on, uint32_t keep)
{
    uint32_t i;

    for (i = 0; i < 17; i++)
        if (!(keep & (1u << i)))
            overlayShowItem(i, on);
}

/* simulate one frame of the riders and the level (live or replayed input) */
static void actionSimulate(int withGame)
{
    riderUpdateAll();
    bmxphysUpdate();
    bmxpropUpdate();
    if (withGame)
        gameUpdate();
}

static int actionPlayerControls(uint32_t p)
{
    return g_actionPausePlayer == 0xFF || g_actionPausePlayer == (uint8_t)p;
}

/* overlay 0x10 fade: {in, r, g, b, frames} */
static void actionFade(uint8_t fadeIn)
{
    struct { uint8_t in, r, g, b; uint16_t frames; } f;

    f.in = fadeIn;
    f.r = f.g = f.b = 0xFF;
    f.frames = 30;
    overlaySetItem(0x10, &f);
}

static void actionReplayIcon(uint8_t icon)
{
    overlaySetItem(9, &icon);                      /* 1 "R" record, 2 "P" play, 3 "p" pause, 4 none */
}

/* VA 0x00403c30, 99 bytes, [V] direct, status draft */
int actionStartup(void)
{
    if (zrollerStartup() != 0)
        return 1;
    zrollerSetLocal(0);
    if (zactorOpenMotionLibrary("Assets\\MOTION.ZAL") == 0) {
        comdataInitDefaults();
        zsystemSetMenuMode();
        return 0;
    }
    g_pfnDebugPrintf("ZARTVIEW: Failed to open motions %s\n", "Assets\\MOTION.ZAL");
    zAssertFail("Failed to open motions", ACTION_SRC, 0xBF, NULL);
    return 1;
}

/* Code at 0x00403CC0 (5 bytes): a tail jump to zactorCloseMotionLibrary, the undo of
 * actionStartup [I name]; no caller, only its /INCREMENTAL thunk 0x00401A0A. Ghidra keeps the lone
 * jmp as a thunk function (actionCloseMotions -> 0x00475000), so it has no decomp/functions.csv row. */
void actionCloseMotions(void)
{
    zactorCloseMotionLibrary();
}

/* VA 0x00403ce0, 32 bytes, [I] between, status draft */
uint32_t actionPickDemoLevel(void)
{
    /* The level of the next attract demo; returns how many full rounds of the three were shown. */
    g_currentLevel = g_actionDemoRuns[g_actionDemoCount % 3][1];
    return g_actionDemoCount / 3u;
}

/* VA 0x00403d10, 751 bytes, [V] direct, status draft */
int actionLoad(void)
{
    /* Build the level. 0 = ok, 1 = failed. With no players this is the attract demo: the next
     * of g_actionDemoRuns, costume 5, first bike. */
    BmxRider *r;
    ZactorBank *bank;
    ZactorBankEntry *e;
    uint32_t p, n;

    contrlerInit();
    zinputGetDevice(0);
    zrollerLoadCodeModule(1);
    zrollerInitializeLoadingScreen(0);
    if (zrollerInit() == 1)
        return 1;
    bmxphysInit();
    fxInit();
    zactorSetHeading(-0x4000, 1);

    if (g_numPlayers == 0) {
        uint32_t k = g_actionDemoCount % 3u;

        g_saveProfiles[0].style = 4;
        g_saveProfiles[0].bike = 0;
        g_saveProfiles[0].rider = g_actionDemoRuns[k][0];
        g_currentLevel = g_actionDemoRuns[k][1];
        g_actionDemoCount++;
        zrollerSetMap(g_levelResGroup[g_currentLevel]);
        r = riderCreate(0, g_saveProfiles[0].rider, g_saveProfiles[0].style, g_saveProfiles[0].bike);
        if (r == NULL) {
            g_pfnDebugPrintf("Failed to create demo rider\n");
            return 1;
        }
    } else {
        zrollerSetMap(g_numPlayers < 2 ? g_levelResGroup[g_currentLevel] : g_levelResGroup2P[g_currentLevel]);
        for (p = 0; p < g_numPlayers; p++) {
            const SaveProfile *pr = &g_saveProfiles[p];

            if (riderCreate((int)p, pr->rider, pr->style, pr->bike) == NULL) {
                g_pfnDebugPrintf("Failed to create rider %ld\n", (long)p);
                return 1;
            }
        }
    }

    overlayLoadFonts();
    if (zrollerLoadAssets() != 0) {
        g_pfnDebugPrintf("Failed to load assets\n");
        return 1;
    }
    overlayLoadStrings(g_saveBlockA.language);
    zoveriiInit();
    overlayInit();
    overlayShowItem(0x10, 1);
    cameraInit();

    /* motion table: g_motionTable[1..n] = the clips of bank 0 in order */
    bank = zactorLoadMotionBank(0);
    if (bank == NULL) {
        g_pfnDebugPrintf("ZARTVIEW: Failed to open motion bank %d\n", 0);
        return 1;
    }
    n = 0;
    while ((e = zactorNextMotion(bank)) != NULL)
        g_motionTable[++n] = (void *)e->data;
    if (n > 238)
        zAssertFail("Code does not match motion asset file.", ACTION_SRC, 0x1F2, NULL);

    bmxreplayInit();
    bmxsoundEvent(1000, 2, g_currentLevel, (g_saveBlockA.cheats_active & 0x20) >> 5, 0);  /* "silly grunt" */
    bmxpropInit();
    g_actionMode = ACTION_PLAY;
    g_actionReplayState = REPLAY_FADE_IN;
    g_actionUnk3e8 = 0;
    g_actionPausePlayer = 0xFF;
    g_currentPlayer = 0;
    gameInit();
    cameraSetPlayer(0);
    cameraSetMode(0, CAM_MODE_FLYBY);

    if (g_numPlayers == 0) {
        actionSetMode(ACTION_DEMO);
    } else if (g_numPlayers == 1) {
        gameSetSubMode(g_selectedSubMode);
        gameStart(0);
    } else {
        overlaySetScreen(0x16);
    }
    zrollerShutdownLoadingScreen();
    return 0;
}

/* VA 0x004040c0, 8 bytes, [I] row, status draft */
int actionServiceSound(void)
{
    /* new function: no caller, thunk 0x00401FCD */
    bmxsoundService();
    return 0;
}

/* VA 0x004040e0, 2668 bytes, [I] between, status draft */
int actionUpdate(void)
{
    ZInputDevice *dev0 = contrlerGetPad(0)->device;
    uint32_t events = contrlerUpdate();
    uint32_t p;
    uint8_t rep = 0;

    /* Start + Select held for two seconds: quit */
    if (contrlerTestButtons(g_currentPlayer, PAD_START, CONTRLER_TEST_HELD_ALL) &&
        contrlerTestButtons(g_currentPlayer, PAD_SELECT, CONTRLER_TEST_HELD_ALL)) {
        if (++g_actionQuitFrames > 60) {
            g_appQuit = 1;
            g_actionQuitFrames = 0;
            zsystemSetMenuMode();
            return 0;
        }
    } else {
        g_actionQuitFrames = 0;
    }

    if (--g_actionFrameCount != 0)
        goto done;
    g_actionFrameCount = g_actionFrameSkip;

    /* a pad was unplugged while riding: pause */
    if ((events & 1) && actionGetMode() == ACTION_PLAY) {
        bmxsoundEvent(1000, 3, 0, 0, 0);
        overlaySetScreen(1);
    }

    switch (actionGetMode()) {
    case ACTION_PLAY:
        if (contrlerTestButtons(g_currentPlayer, PAD_PAUSE, CONTRLER_TEST_PRESSED_ANY)) {
            bmxsoundEvent(1000, 3, 0, 0, 0);
            overlaySetScreen(1);                     /* the pause menu (it calls actionSetMode(1)) */
        }
        if (actionGetMode() == ACTION_PLAY) {
            bmxreplayUpdate();
            riderReadInput();
            actionSimulate(1);
        }
        cameraUpdate();
        break;

    case ACTION_PAUSED:
        /* menu auto-repeat: a held d-pad direction repeats after 20 frames, then every 3 frames
         * (every frame with L1 / R1 held) */
        for (p = 0; p < 2; p++) {
            BmxPad *pad = contrlerGetPad(p);

            if ((pad->input.changed & ~(uint32_t)(PAD_LTRIG1 | PAD_RTRIG1)) == 0) {
                if (pad->repeat == 0) {
                    pad->input.changed = pad->input.buttons & PAD_DPAD;
                    pad->repeat = (pad->input.buttons & (PAD_LTRIG1 | PAD_RTRIG1)) ? 0 : 3;
                } else {
                    pad->repeat--;
                }
            } else {
                pad->repeat = 20;
                g_actionTimer = 0;
            }
        }
        overlayUpdate();
        if (actionGetMode() != ACTION_PAUSED)
            return 0;
        p = overlayGetScreen();
        if (p > 8 && (p < 12 || p == 25)) {          /* replay screens of the pause menu */
            uint8_t st;

            bmxreplayUpdate();
            rep = gameGetViewReplay();
            st = bmxreplayGetMode(rep);
            if (st == 2) {
                actionSimulate(1);
            } else if (st == 4) {                    /* at the end: play it again from the start */
                bmxreplaySetMode(rep, 2);
                cameraSetMode(bmxreplayGetPlayer(rep), CAM_MODE_REPLAY);
                cameraSetShot(bmxreplayGetPlayer(rep), 8);
            }
        }
        cameraUpdate();
        break;

    case ACTION_REPLAY:
    case ACTION_REPLAY3:
        rep = actionGetMode() == ACTION_REPLAY3 ? g_actionInstantReplay : gameGetViewReplay();
        switch (g_actionReplayState) {
        case REPLAY_FADE_IN:
            if (++g_actionTimer == 1) {
                actionFade(1);
                cameraUpdate();
            } else if (g_actionTimer == 30) {
                g_actionReplayState = REPLAY_PLAYING;
                g_actionTimer = 0;
            }
            break;

        case REPLAY_PLAYING:
        case REPLAY_RESTART:
        case REPLAY_ENDED:
        case REPLAY_CRASH:
            /* buttons: Start/Esc stop, Square pause, Circle orbit, Cross next shot, Select HUD */
            for (p = 0; p < g_numPlayers; p++) {
                if (!actionPlayerControls(p))
                    continue;
                if (contrlerTestButtons(p, PAD_PAUSE, CONTRLER_TEST_PRESSED_ANY)) {
                    g_actionReplayState = REPLAY_FADE_OUT;
                    g_actionTimer = 0;
                    actionReplayIcon(4);
                    break;
                }
                if (contrlerTestButtons(p, PAD_FIRE2, CONTRLER_TEST_PRESSED_ANY)) {
                    g_actionReplayState = REPLAY_PAUSED;
                    g_actionTimer = 0;
                    if (g_actionOverlaysOn)
                        actionShowOverlays(0, HUD_KEEP_PAUSE);
                    actionReplayIcon(3);
                    break;
                }
                if (contrlerTestButtons(p, PAD_FIRE3, CONTRLER_TEST_PRESSED_ANY)) {
                    g_actionSavedShot = cameraGetShot(bmxreplayGetPlayer(rep));
                    cameraSetShot(bmxreplayGetPlayer(rep), 0);
                    g_actionReplayState = REPLAY_ORBIT;
                    g_actionTimer = 0;
                    actionReplayIcon(3);
                    break;
                }
                if (contrlerTestButtons(p, PAD_FIRE4, CONTRLER_TEST_PRESSED_ANY)) {
                    uint8_t shot = cameraGetShot(bmxreplayGetPlayer(rep));

                    cameraSetShot(bmxreplayGetPlayer(rep), shot < 8 ? shot + 1u : 0u);
                }
                if (contrlerTestButtons(p, PAD_SELECT, CONTRLER_TEST_PRESSED_ANY)) {
                    g_actionOverlaysOn ^= 1;
                    actionShowOverlays(g_actionOverlaysOn, HUD_KEEP_REPLAY);
                }
            }

            /* second dispatch on the (possibly new) state: jump table 0x00404BB8 */
            switch (g_actionReplayState) {
            case REPLAY_PLAYING:
                for (p = 0; p < g_numPlayers; p++) {
                    if (actionPlayerControls(p) &&
                        contrlerTestButtons(p, PAD_FIRE1, CONTRLER_TEST_PRESSED_ANY)) {
                        bmxreplaySetMode(rep, 4);      /* Triangle: from the start */
                        bmxreplaySetMode(rep, 2);
                        if (actionGetMode() == ACTION_REPLAY)
                            cameraSetMode(bmxreplayGetPlayer(rep), CAM_MODE_REPLAY);
                        else
                            cameraSetShot(bmxreplayGetPlayer(rep), cameraGetShot(bmxreplayGetPlayer(rep)));
                        cameraUpdate();
                        g_actionReplayState = REPLAY_RESTART;
                        g_actionTimer = 0;
                        break;
                    }
                }
                /* fall through */
            case REPLAY_CRASH:
                if (g_actionReplayState == REPLAY_PLAYING) {
                    uint8_t st;

                    bmxreplayUpdate();
                    st = bmxreplayGetMode(rep);
                    if (st == 2) {
                        actionSimulate(1);
                    } else if (st == 4) {
                        if (actionGetMode() == ACTION_REPLAY3) {      /* loops */
                            bmxreplaySetMode(rep, 2);
                            cameraSetShot(bmxreplayGetPlayer(rep), cameraGetShot(bmxreplayGetPlayer(rep)));
                            g_actionReplayState = REPLAY_RESTART;
                        } else {
                            g_actionReplayState = REPLAY_ENDED;
                        }
                        g_actionTimer = 0;
                        break;                       /* no camera update this frame */
                    }
                    if (g_actionReplayState == REPLAY_PLAYING)
                        cameraUpdate();
                } else if (g_actionReplayState == REPLAY_CRASH) {
                    if (++g_actionTimer < 240) {
                        BmxRider *r = &g_riders[bmxreplayGetPlayer(rep)];

                        if (r->impactTimer != 0)
                            r->impactTimer--;
                        bmxphysUpdate();
                        bmxpropUpdate();
                        cameraUpdate();
                    } else {
                        g_actionReplayState = REPLAY_FADE_OUT;
                        g_actionTimer = 0;
                    }
                }
                break;
            case REPLAY_RESTART:
            case REPLAY_ENDED:
                if (++g_actionTimer == 30) {
                    g_actionReplayState = g_actionReplayState == REPLAY_RESTART ? REPLAY_PLAYING : REPLAY_FADE_OUT;
                    g_actionTimer = 0;
                }
                cameraUpdate();
                break;
            default:                                 /* 2, 3, 5: nothing more this frame */
                break;
            }
            break;

        case REPLAY_FADE_OUT:
            if (g_actionTimer++ == 0) {
                actionFade(0);
                if (gameGetMode() == 0 && actionGetMode() != ACTION_REPLAY3)
                    bmxsoundEvent(0x3E9, 4, 0, 0, 0);
            } else if (g_actionTimer == 30) {
                if (actionGetMode() == ACTION_REPLAY)
                    overlaySetScreen(0x1A);          /* back to the run-over screen */
                else
                    actionSetMode(ACTION_PLAY);
                cameraUpdate();
            }
            break;

        case REPLAY_PAUSED:
        case REPLAY_ORBIT:
            for (p = 0; p < g_numPlayers; p++) {
                if (!actionPlayerControls(p))
                    continue;
                if (contrlerTestButtons(p, PAD_PAUSE, CONTRLER_TEST_PRESSED_ANY)) {
                    if (g_actionReplayState == REPLAY_ORBIT)
                        cameraSetShot(bmxreplayGetPlayer(rep), g_actionSavedShot);
                    g_actionReplayState = REPLAY_FADE_OUT;
                    g_actionTimer = 0;
                    actionReplayIcon(4);
                    break;
                }
                if (g_actionReplayState == REPLAY_PAUSED &&
                    contrlerTestButtons(p, PAD_FIRE3, CONTRLER_TEST_PRESSED_ANY)) {
                    g_actionSavedShot = cameraGetShot(bmxreplayGetPlayer(rep));
                    cameraSetShot(bmxreplayGetPlayer(rep), 0);
                    g_actionReplayState = REPLAY_ORBIT;
                    g_actionTimer = 0;
                    break;
                }
                if (contrlerTestButtons(p, PAD_FACE, CONTRLER_TEST_PRESSED_ANY)) {   /* any face button: play */
                    if (g_actionReplayState == REPLAY_ORBIT)
                        cameraSetShot(bmxreplayGetPlayer(rep), g_actionSavedShot);
                    g_actionReplayState = REPLAY_PLAYING;
                    g_actionTimer = 0;
                    if (g_actionOverlaysOn)
                        actionShowOverlays(1, HUD_KEEP_REPLAY);
                    actionReplayIcon(2);
                    break;
                }
            }
            if (g_actionReplayState == REPLAY_PAUSED) {
                if (++g_actionTimer == 1)
                    bmxsoundEvent(6000, 0, 0, 0, 0);
            } else if (g_actionReplayState == REPLAY_ORBIT) {
                cameraUpdate();
            }
            break;
        }
        break;

    case ACTION_DEMO:
        rep = g_actionDemoReplay;
        switch (g_actionReplayState) {
        case REPLAY_FADE_IN:
            if (++g_actionTimer == 1) {
                actionFade(1);
                cameraUpdate();
            } else if (g_actionTimer == 30) {
                g_actionReplayState = REPLAY_PLAYING;
                g_actionTimer = 0;
            }
            break;
        case REPLAY_PLAYING:
            for (p = 0; p < 2; p++) {                /* any button on a connected pad ends the demo */
                BmxPad *pad = contrlerGetPad(p);

                if (!(pad->flags & BMXPAD_DISCONNECTED) && (pad->input.buttons & pad->input.changed) != 0) {
                    g_actionReplayState = REPLAY_FADE_OUT;
                    g_actionTimer = 0;
                    break;
                }
            }
            if (g_actionReplayState == REPLAY_PLAYING) {
                uint8_t st;
                int over;

                bmxreplayUpdate();
                st = bmxreplayGetMode(rep);
                if (st == 2) {
                    actionSimulate(0);
                    if (++g_actionTimer == 30 || g_actionTimer == 3450) {
                        uint8_t snd = (uint8_t)bmxsoundEvent(0x3EC, 0, 0, 0, 0);

                        overlaySetItem(0xD, &snd);   /* the current music track title (OVL_ITEM_TRACK) */
                        cameraUpdate();
                        break;
                    }
                    over = g_actionTimer == 3900;     /* 130 s */
                } else {
                    over = st == 4;
                }
                if (over) {
                    g_actionReplayState = REPLAY_PAUSED;   /* 3: fade out and leave, below */
                    g_actionTimer = 0;
                }
            }
            cameraUpdate();
            break;
        case REPLAY_FADE_OUT:
        case REPLAY_PAUSED:
            if (++g_actionTimer == 1) {
                actionFade(0);
                bmxsoundEvent(0x3E9, 4, 0, 0, 0);
            } else if (g_actionTimer == 30) {
                zsystemSetMenuMode();               /* back to the title menu */
                return 0;
            }
            break;
        }
        break;
    }

    zrollerRunDrawCallbacks();
    if (g_actionFrameSkip == -1) {                   /* debug build switch: fake R2 on pad 0 */
        dev0->buttons |= PAD_RTRIG2;
        dev0->changed |= PAD_RTRIG2;
    }
done:
    ngappEmptyHook();
    return 0;
}

/* VA 0x00404ed0, 71 bytes, [I] strings, status draft */
void actionShutdown(void)
{
    zconsoleLog("APP: Shutdown ACTION\n");
    bmxpropShutdown();
    overlayShutdown();
    gameShutdown();
    bmxsoundEvent(1000, 0, 0, 0, 0);
    bmxreplayShutdown();
    contrlerShutdown();
    fxShutdown();
    bmxphysShutdown();
    zrollerShutdown();
}

/* VA 0x00404f40, 1432 bytes, [V] direct, status draft */
uint32_t actionSetMode(uint32_t mode)
{
    uint8_t rep;
    uint8_t item;
    uint32_t i, menu;

    g_actionPausePlayer = 0xFF;
    g_actionTimer = 0;
    g_actionUnk3f0 = 0;

    /* leave the current mode */
    switch (g_actionMode) {
    case ACTION_PLAY:
        if (mode != ACTION_PAUSED) {
            for (i = 0; i < 17; i++)
                if (i != 16)
                    overlayShowItem(i, 0);
            g_actionOverlaysOn = 0;
        }
        break;
    case ACTION_PAUSED:
        menu = overlayGetScreen();
        if (menu > 8 && (menu < 11 || menu == 25)) { /* a replay was running behind the menu */
            rep = gameGetViewReplay();
            bmxreplaySetMode(rep, 0);
            gameResetRider(bmxreplayGetPlayer(rep), 0);
            cameraSetMode(g_currentPlayer, CAM_MODE_FLYBY);
            cameraSetPlayer(g_currentPlayer);
        }
        break;
    case ACTION_REPLAY:
        rep = gameGetViewReplay();
        bmxreplaySetMode(rep, 0);
        gameResetRider(bmxreplayGetPlayer(rep), 0);
        if (gameGetMode() == 3)
            gameResetRider(g_currentPlayer, 1);
        /* fall through */
    case ACTION_DEMO:
        if (g_actionMode == ACTION_DEMO) {
            gameResetRider(bmxreplayGetPlayer(g_actionDemoReplay), 0);
            bmxreplayDestroy(g_actionDemoReplay);
        }
        item = 0;
        overlaySetItem(9, &item);
        overlayShowItem(9, 0);
        overlayShowItem(14, 0);
        overlayShowItem(15, 0);
        item = 0xFF;
        overlaySetItem(13, &item);
        overlayShowItem(13, 0);
        overlayShowItem(0, 0);
        cameraSetMode(g_currentPlayer, CAM_MODE_FLYBY);
        cameraSetPlayer(g_currentPlayer);
        if (gameGetMode() == 0)
            bmxsoundEvent(0x3E9, 4, 0, 0, 0);
        break;
    case ACTION_REPLAY3:
        bmxreplaySetMode(g_actionInstantReplay, 0);
        item = 0;
        overlaySetItem(9, &item);
        overlayShowItem(9, 0);
        overlayShowItem(14, 0);
        overlayShowItem(15, 0);
        cameraSetMode(g_currentPlayer, CAM_MODE_FOLLOW);
        g_actionUnk3e4 = 0;
        break;
    }

    /* enter the new one */
    switch (mode) {
    case ACTION_PLAY:
        actionShowOverlays(1, HUD_KEEP_PLAY);
        g_actionOverlaysOn = 1;
        contrlerSetPaused(0);
        g_actionUnk3e4 = 0;
        break;
    case ACTION_PAUSED:
        menu = overlayGetScreen();
        if (menu > 8 && (menu < 11 || menu == 25)) {
            for (i = 0; i < g_numPlayers; i++)
                gameResetRider((int)i, 0);
            rep = gameGetViewReplay();
            cameraSetPlayer(bmxreplayGetPlayer(rep));
            bmxreplaySetMode(rep, 2);
            cameraSetMode(bmxreplayGetPlayer(rep), CAM_MODE_REPLAY);
            cameraSetShot(bmxreplayGetPlayer(rep), 8);
        }
        contrlerSetPaused(1);
        break;
    case ACTION_REPLAY:
    case ACTION_DEMO:
        for (i = 0; i < g_numPlayers; i++)
            gameResetRider((int)i, 0);
        if (mode == ACTION_DEMO) {
            BmxReplayDesc desc;
            void *buf;
            uint32_t size;

            g_currentPlayer = 0;
            desc.flags = 7;
            desc.player = 0;
            desc.size = 0x2800;
            g_actionDemoReplay = (uint8_t)bmxreplayCreate(&desc);
            if (g_actionDemoReplay == 0xFF)
                zAssertFail("Failed to create replay (probably out of RAM).", ACTION_SRC, 0x6BC, NULL);
            bmxreplayGetSaveArea(g_actionDemoReplay, &buf, &size);
            /* replay.zal group = rider + level * 2 */
            if (!zassetFindAsset(g_replayZal, g_saveProfiles[0].rider + g_currentLevel * 2u, 0)) {
                zAssertFail("replay data not found in asset library", ACTION_SRC, 0x6C2, NULL);
            } else {
                if (size < zassetGetAssetSize(g_replayZal))
                    zAssertFail("replay data too big for demo replay buffer", ACTION_SRC, 0x6BF, NULL);
                zassetLoadAsset(g_replayZal, buf);
            }
            rep = g_actionDemoReplay;
        } else {
            rep = gameGetViewReplay();
        }
        cameraSetPlayer(bmxreplayGetPlayer(rep));
        bmxreplaySetMode(rep, 2);
        item = 2;
        overlaySetItem(9, &item);
        item = 0xFF;
        overlaySetItem(10, &item);
        cameraSetMode(bmxreplayGetPlayer(rep), CAM_MODE_REPLAY);
        if (mode == ACTION_DEMO) {
            for (i = 0; i < 17; i++)
                if (i != 16)
                    overlayShowItem(i, 0);
            g_actionOverlaysOn = 0;
            overlayShowItem(13, 1);
            overlayShowItem(0, 1);
            overlayShowItem(14, 1);
            overlayShowItem(15, 1);
        } else {
            overlayShowItem(9, 1);
            overlayShowItem(2, 0);
            overlayShowItem(1, 0);
            overlayShowItem(10, 0);
            overlayShowItem(14, 0);
            overlayShowItem(15, 0);
        }
        if (gameGetMode() == 0)
            bmxsoundEvent(0x3E9, 2, 0, 0, 0);
        break;
    case ACTION_REPLAY3:
        bmxreplaySetMode(g_actionInstantReplay, 2);
        item = 2;
        overlaySetItem(9, &item);
        overlayShowItem(9, 1);
        overlayShowItem(14, 0);
        overlayShowItem(15, 0);
        cameraSetMode(bmxreplayGetPlayer(g_actionInstantReplay), CAM_MODE_REPLAY);
        cameraSetShot(bmxreplayGetPlayer(g_actionInstantReplay), 1);
        break;
    }
    g_actionReplayState = REPLAY_FADE_IN;
    g_actionMode = (uint8_t)mode;
    return mode & 0xFF;
}

/* VA 0x004056a0, 11 bytes, [I] split, status draft */
uint32_t actionGetMode(void)
{
    return g_actionMode;
}

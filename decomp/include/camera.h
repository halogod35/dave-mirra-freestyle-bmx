/*
 * camera.h - the game camera (Bmx/Src/com/action/camera.c) (DM-051).
 *
 * One BmxCamera per player (g_cameras[2] 0x0056B448, stride 0xB8); only the camera of
 * g_cameraPlayer is updated and handed to the engine camera each frame. Offsets [V] (camera.c and
 * the callers in action.c / game.c / rider.c); names and meanings [I] unless noted.
 * Units as in physics.h: ZGE world units (cm), y points DOWN, per-frame values, 16-bit angles.
 * Full description: docs/re/input-camera.md.
 */
#ifndef CAMERA_H
#define CAMERA_H
#include "dmtypes.h"
#include "rwtypes.h"

/* What drives the camera (BmxCamera.mode, cameraSetMode). [V] values and the cameraUpdate switch;
 * the uses [V] from the callers (game.c / action.c). */
enum {
    CAM_MODE_FOLLOW = 0,   /* in play: follows the rider in the player's view (CAM_VIEW_*); crash: high
                              wide shot looking down at the rider */
    CAM_MODE_REPLAY = 1,   /* replays and the attract demo: "TV" shots 0..10 (cameraSetShot) */
    CAM_MODE_STILL  = 2,   /* nothing moves the target (no caller sets it; its smoothing set exists) */
    CAM_MODE_ORBIT  = 3,   /* run intro: circles the rider at 500 units, 700 up, +300 angle / frame */
    CAM_MODE_FLYBY  = 4,   /* level start / standings / menus: a random spot of the level, slowly
                              circling (-50 / frame) a point 500 away */
    CAM_MODES = 5,
    CAM_MODE_NONE   = 0xFF /* cameraInit, before the first cameraSetMode */
};

/* Player camera types: SaveProfile.camera (save +6), pause menu "camera type" (texts 0x1087+v). */
enum {
    CAM_VIEW_BEHIND = 0,        /* 275 behind the hips along the motion, 160 up; FOV 50 */
    CAM_VIEW_WIDE   = 1,        /* same placement, FOV 60 and slower look-at smoothing */
    CAM_VIEW_ABOVE  = 2,        /* 800 above, 150 behind, looking down at the rider; FOV 60 */
    CAM_VIEW_FIRST_PERSON = 3,  /* the rider's head (hips + up * 100), looking 500 ahead along the rider's
                                   forward axis with the rider's matrix; rider + bike hidden, horizon off;
                                   FOV 70 */
    CAM_VIEWS = 4
};

/* BmxCamera.flags */
#define CAMF_NO_MIN_DIST   0x01u   /* skip the "at least 250 from the rider" clamp (orbit, flyby, 1st person) */
#define CAMF_NO_WALL_PUSH  0x02u   /* skip pushing the camera out of nearby walls (same modes) */
#define CAMF_AUTO_SHOT     0x04u   /* replay: cut to the next shot by itself when a shot is done */
#define CAMF_SHOT_DONE     0x08u   /* replay: the current shot is finished (cut at the next update) */
#define CAMF_FLASH_A       0x10u   /* replay cut: flash the screen (zrollerStartCrossFade(0x80, 0, -16)) */
#define CAMF_FLASH_B       0x20u   /* the same, -8 */

/* Six smoothing parameters, one set per mode / view / replay shot. [V] tables 0x00551D04 (shots),
 * 0x00551E0C (crash), 0x00551E24 (modes 2-4), 0x00551E6C (views); meanings [I] from cameraUpdate. */
typedef struct BmxCamParams {
    float spring;      /* +0x00 velocity gained per frame towards the smoothed target (x distance) */
    float damping;     /* +0x04 velocity lost per frame (fraction) */
    float posFollow;   /* +0x08 smoothed target position += (target - smoothed) * posFollow */
    float lookFollow;  /* +0x0C look-at point += (wanted - look-at) * lookFollow */
    float fovFollow;   /* +0x10 FOV += (wanted - FOV) * fovFollow */
    float fov;         /* +0x14 wanted field of view, degrees */
} BmxCamParams;        /* 0x18 */

typedef struct BmxCamera {
    uint8_t  mode;            /* +0x00 CAM_MODE_* */
    uint8_t  shotPhase;       /* +0x01 replay shots 1-6, 10: 0 settle, 1 wait for the rider, 2 leave */
    uint8_t  view;            /* +0x02 CAM_VIEW_* (cameraSetView) */
    uint8_t  shot;            /* +0x03 replay shot 0..10 (cameraSetShot) */
    uint32_t timer;           /* +0x04 frames since the last mode / shot / view change */
    uint16_t orbitAngle;      /* +0x08 binary angle of the orbit / flyby / replay shot 0 */
    uint8_t  snap;            /* +0x0A 1: jump to the target at the next update (no smoothing) */
    uint8_t  flags;           /* +0x0B CAMF_* */
    uint16_t blocked;         /* +0x0C frames the line of sight to the rider has been blocked */
    uint8_t  pad_0e[2];
    RwV3d    pos;             /* +0x10 camera position */
    float    pad_1c;
    RwV3d    wantPos;         /* +0x20 where the camera wants to be */
    float    pad_2c;
    RwV3d    smoothPos;       /* +0x30 wantPos smoothed (posFollow); the spring pulls pos towards it */
    float    pad_3c;
    RwV3d    look;            /* +0x40 look-at point */
    float    pad_4c;
    RwV3d    wantLook;        /* +0x50 wanted look-at point (usually the hips) */
    float    pad_5c;
    RwV3d    vel;             /* +0x60 camera velocity (per frame) */
    float    pad_6c;
    float    fov;             /* +0x70 field of view, degrees */
    float    wantFov;         /* +0x74 */
    RwMatrix matrix;          /* +0x78 right = view direction, up, at = right x up; pos unused */
} BmxCamera;                  /* 0xB8 */

/* Engine camera request (cameraApply; built on the stack by cameraUpdate). */
typedef struct BmxCamView {
    void    *obj;             /* +0x00 the scene camera zobj (0x0068FF98) */
    int16_t  fov;             /* +0x04 degrees */
    uint8_t  pad_06[2];
    RwV3d    pos;             /* +0x08 */
    float    pad_14;
    RwV3d    look;            /* +0x18 */
    float    pad_24;
    RwMatrix matrix;          /* +0x28 */
} BmxCamView;                 /* 0x68 */

/* GHIDRA-SKIP-BEGIN */
typedef char camera_h_size_check[(sizeof(BmxCamParams) == 0x18 && sizeof(BmxCamera) == 0xB8 &&
                                  sizeof(BmxCamView) == 0x68) ? 1 : -1];

extern BmxCamera    g_cameras[2];                     /* 0x0056B448 */
extern uint8_t      g_cameraPlayer;                   /* 0x0056B408 */
extern int32_t      g_cameraNightVisionSize[2];       /* 0x0056B410 {640, 480} */
extern uint32_t     g_cameraShake;                    /* 0x0056B5B8 [I] cleared on every cut; shot 9 zooms in while set */
extern const float  g_camShotOffset[11][4];           /* 0x00551C54 camera offset in the rider frame, shots 2/4/6 */
extern const BmxCamParams g_camShotParams[11];        /* 0x00551D04 */
extern const BmxCamParams g_camCrashParams;           /* 0x00551E0C */
extern const BmxCamParams g_camModeParams[3];         /* 0x00551E24 modes 2..4 */
extern const BmxCamParams g_camViewParams[CAM_VIEWS]; /* 0x00551E6C */
extern const float  g_camLead;                        /* 0x00551ECC 5: look-ahead, frames of rider velocity */
extern const float  g_camMinDist;                     /* 0x00551ED0 250: minimum horizontal distance to the hips */
extern const float  g_camWallRadius;                  /* 0x00551ED4 170: walls closer than this push the camera */
extern const float  g_camFollowDist;                  /* 0x00551ED8 275: behind / wide distance */
extern const float  g_camFollowHeight;                /* 0x00551EDC 160: behind / wide height (rolling) */
extern const float  g_camAirHeight;                   /* 0x00551EE0 125: height step in the air (x2) / locking on */

void     cameraInit(void);
void     cameraUpdate(void);
void     cameraApply(const BmxCamView *v);
int      cameraDrawNightVision(void);
void     cameraSetPlayer(uint32_t player);
uint32_t cameraGetPlayer(void);
void     cameraSetView(uint32_t player, uint32_t view);
void     cameraSetMode(uint32_t player, int mode);
void     cameraSetShot(uint32_t player, uint32_t shot);
uint8_t  cameraGetShot(uint32_t player);
/* GHIDRA-SKIP-END */

#endif /* CAMERA_H */

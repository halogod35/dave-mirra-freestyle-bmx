/*
 * zaxis/Bmx/Src/com/action/camera.c
 * Original: C:\zaxis\Bmx\Src\com\action\camera.c
 * Evidence: [V] path string 0x00551F88, referenced by the assert of cameraSetPlayer (line 0x49F,
 *           "player number out of range"); [I] for the rest: every function here only works on the
 *           camera globals 0x0056B408-0x0056B5BB (g_cameraPlayer, g_cameras[2], g_cameraShake), they
 *           are contiguous in link order (action.c's last function actionGetMode ends at 0x004056AA,
 *           comdata.c starts at 0x00407BA0), and the assert line 1183 needs a long file in front of
 *           cameraSetPlayer: cameraUpdate (5.7 KB) is that code.
 * 13 functions, 7160 bytes, VA 0x004056c0-0x00407b85 (the clean-up created 0x00405760, 0x00407730, 0x00407890).
 * Draft. Non-matching C (AD-8). Types: camera.h. Description: docs/re/input-camera.md.
 *
 * The camera pass moved cameraInit, cameraUpdate and cameraApply here from action.c (the skeleton "split" /
 * "callers"), took in the five camera-gap functions (0x00407600-0x00407B85) and found
 * cameraDrawNightVision 0x004074C0, which Ghidra had not created (it is reached only through the
 * draw-callback pointer cameraInit registers; a decomp/symbols/symbols.csv row creates it).
 *
 * Each player has a camera; only the one of g_cameraPlayer runs. A camera is a damped spring:
 * the mode (follow / replay shots / orbit / flyby) computes a wanted position and look-at point,
 * the wanted position is smoothed, the camera accelerates towards it, is pushed out of nearby
 * walls, kept at least 250 from the rider, and the look-at and the field of view are smoothed; the
 * six factors come from one BmxCamParams set per view / shot / mode.
 */

#include <math.h>

#include "decomp.h"

#include "zmath.h"

#include "physics.h"

#include "camera.h"

#define CAMERA_SRC "C:\\zaxis\\Bmx\\Src\\com\\action\\camera.c"
#define ANGLE_PER_DEGREE 182.04445f    /* 0x10000 / 360 */

/* One wall candidate of zbspColSphere (list at result + 0x3C, ends with normal == NULL). */
typedef struct NgBspNearFace {
    const float *normal;      /* +0x00 face normal */
    uint32_t unk_04;
    float    point[3];        /* +0x08 nearest point of the face */
    uint32_t unk_14;
    uint32_t passable;        /* +0x18 non-zero: ignore */
} NgBspNearFace;              /* 0x1C */

/* ------------------------------------------------------------------ other modules (current names) */
void     zglEnableHorizon(void);                              /* "Enable Horizon" (stub, prints only) */
void     zglDisableHorizon(void);                              /* "zgl disable horizon" (stub) */
void     zrollerStartCrossFade(int a, int b, int c);              /* screen flash (camera cut) [I] */
void     zrollerAddDrawCallback(int (*draw)(void));                /* add a per-frame draw callback */
float   *zrollerGetWorldBounds(void);                             /* level bounding box min[4], max[4] */
void     psxDrawRect(uint32_t rgb, int x, int y, uint32_t flags, const int32_t *size);  /* 2D rect */
void     zbspFaceCentroid(const NgColFace *ground, RwV3d *pos); /* snap pos onto that ground */
uint8_t *zbspColSphere(const RwV3d *pos, float radius, float cosLimit);  /* walls near pos */
int      zcolSegmentTest(const RwV3d *from, const RwV3d *to, NgColHit *hit);  /* segment test */
void     zobjCameraSetFov(void *cam, int16_t fov);            /* set the field of view */
void     zobjCameraLookAt(void *cam, const RwV3d *pos, const RwV3d *look, int roll);
void     zsndSrcResetDoppler(void);                                 /* camera cut: reset sound sources [I] */
void     bmxsoundEvent(int cmd, const void *pos, uint32_t a, uint32_t b, uint32_t c);
uint32_t actionGetMode(void);

extern void  *g_sceneCamera;                                     /* 0x0068FF98 the scene's camera zobj [I] */
extern RwV3d  g_zmathOrigin;                                     /* 0x006702D8 (0, 0, 0) */

#define RIDER_FLAG_ACTIVE 0x00000001u
#define CHEAT_NIGHT_VISION 0x0002u     /* SaveBlockA.cheats_active bits = cheat index (CHEAT_NAMES) */
#define CHEAT_GHOST_RIDER  0x0040u

/* ------------------------------------------------------------------ helpers */
static void hips(const BmxRider *r, RwV3d *out)
{
    out->x = r->physHipOffset[0] + r->pos[0];
    out->y = r->physHipOffset[1] + r->pos[1];
    out->z = r->physHipOffset[2] + r->pos[2];
}

static int inAirOrLocking(int state)
{
    return state == BMXPHYS_AIR || state == BMXPHYS_GRIND_LOCK || state == BMXPHYS_STALL_LOCK;
}

/* crash shot: 300 out from the rider (on the camera's side), 700 up, looking at the rider */
static void cameraFrameCrash(BmxCamera *cam, const BmxRider *r, int lookToo)
{
    RwV3d d;

    if (lookToo) {
        cam->wantLook.x = r->pos[0];
        cam->wantLook.y = r->pos[1] - 25.0f;
        cam->wantLook.z = r->pos[2];
    }
    zmathVecNegate(&d, (const RwV3d *)r->pos);
    zmathVecAdd(&d, &d, &cam->wantPos);
    d.y = 0.0f;
    if (d.x == 0.0f || d.z == 0.0f) {
        d.x = 500.0f;                                /* (z kept as it is) */
    } else {
        zmathVecNormalize(&d, &d);
        zmathVecScale(&d, &d, 300.0f);
    }
    d.y = -700.0f;
    zmathVecAdd(&cam->wantPos, &d, (const RwV3d *)r->pos);
}

/* horizontal distance of the wanted position from the hips, squared, in whole units */
static uint32_t wantDist2(const BmxCamera *cam, const BmxRider *r)
{
    int dx = (int)(cam->wantPos.x - (r->physHipOffset[0] + r->pos[0]));
    int dz = (int)(cam->wantPos.z - (r->physHipOffset[2] + r->pos[2]));

    return (uint32_t)(dz * dz + dx * dx);
}

/* ------------------------------------------------------------------ functions */

/* VA 0x004056c0, 121 bytes, [I] row, status draft */
void cameraInit(void)
{
    /* actionLoad: one camera per player, in the player's saved view (save profile +6), following. */
    uint32_t p, n = g_numPlayers ? g_numPlayers : 1;

    for (p = 0; p < n; p++) {
        BmxCamera *cam = &g_cameras[p];

        cam->view = 0xFF;                            /* force cameraSetView to apply it */
        cameraSetView(p, g_saveProfiles[p].camera);
        cam->mode = CAM_MODE_NONE;
        cameraSetMode(p, CAM_MODE_FOLLOW);
        cam->shot = 0;
        cam->snap = 1;
        cam->flags = 0;
    }
    g_cameraPlayer = 0;
    zrollerAddDrawCallback(cameraDrawNightVision);
}

/* VA 0x00405760, 1 bytes, [I] row, status draft */
void cameraStub(void)
{
    /* new function: a bare ret between cameraInit and cameraUpdate; no caller, thunk 0x00402013 */
}

/* VA 0x00405780, 5772 bytes, [I] row, status draft */
void cameraUpdate(void)
{
    uint32_t p;

    for (p = 0; p < (g_numPlayers ? g_numPlayers : 1u); p++) {
        BmxRider *r = &g_riders[p];
        BmxCamera *cam = &g_cameras[p];
        BmxCamParams prm;
        RwV3d d, h, np;
        int state, riderMatrix = 0;

        if (p != g_cameraPlayer)
            continue;
        state = bmxphysGetState(r);

        /* replay: the shot is over and the camera cycles by itself -> next shot (0..8) */
        if ((cam->flags & CAMF_AUTO_SHOT) && (cam->flags & CAMF_SHOT_DONE)) {
            cameraSetShot(p, cam->shot < 8 ? cam->shot + 1u : 0u);
            cam->flags |= CAMF_AUTO_SHOT;
        }

        if (cam->mode == CAM_MODE_FOLLOW)
            prm = (state == BMXPHYS_CRASH && cam->view != CAM_VIEW_FIRST_PERSON) ? g_camCrashParams
                                                                                  : g_camViewParams[cam->view];
        else if (cam->mode == CAM_MODE_REPLAY)
            prm = g_camShotParams[cam->shot];
        else
            prm = g_camModeParams[cam->mode - 2];
        cam->wantFov = prm.fov;

        switch (cam->mode) {
        case CAM_MODE_FOLLOW:
            if (state == BMXPHYS_CRASH && cam->view != CAM_VIEW_FIRST_PERSON) {
                cameraFrameCrash(cam, r, 1);
                break;
            }
            /* look at the hips, a third of the way towards the bike once moving */
            hips(r, &cam->wantLook);
            if (bmxphysGetState(r) != BMXPHYS_ROLL || r->frameDist != 0.0f) {
                cam->wantLook.x += (r->spawnPos[0] - (r->physHipOffset[0] + r->pos[0])) * 0.33f;
                cam->wantLook.y += (r->spawnPos[1] - (r->physHipOffset[1] + r->pos[1])) * 0.33f;
                cam->wantLook.z += (r->spawnPos[2] - (r->physHipOffset[2] + r->pos[2])) * 0.33f;
            }
            switch (cam->view) {
            case CAM_VIEW_BEHIND:
            case CAM_VIEW_WIDE: {                    /* lead the rider by 5 frames of motion */
                int s = bmxphysGetState(r);

                d.x = r->vel[0];
                d.y = r->vel[1];
                d.z = r->vel[2];
                if (inAirOrLocking(s) && r->vel[1] > 15.0f)
                    d.y -= 15.0f;                    /* falling fast: look a little lower */
                else
                    d.y = 0.0f;
                zmathVecScale(&d, &d, g_camLead);
                zmathVecAdd(&cam->wantLook, &cam->wantLook, &d);
                break;
            }
            case CAM_VIEW_ABOVE:
                if (r->frameDist == 0.0f) {
                    d.x = r->matrix[0];
                    d.z = r->matrix[2];
                } else {
                    d.x = r->vel[0];
                    d.z = r->vel[2];
                }
                d.y = 0.0f;
                zmathVecScale(&d, &d, g_camLead + g_camLead);
                zmathVecAdd(&cam->wantLook, &cam->wantLook, &d);
                break;
            case CAM_VIEW_FIRST_PERSON:              /* 500 ahead, 80 up; the rider's own matrix */
                cam->wantLook.x += r->matrix[0] * 500.0f + r->matrix[4] * 80.0f;
                cam->wantLook.y += r->matrix[1] * 500.0f + r->matrix[5] * 80.0f;
                cam->wantLook.z += r->matrix[2] * 500.0f + r->matrix[6] * 80.0f;
                zmathMatrixCopy(&cam->matrix, (const RwMatrix *)r->matrix);
                riderMatrix = 1;
                break;
            }

            hips(r, &h);
            if (cam->view == CAM_VIEW_ABOVE) {       /* 800 up, 150 back */
                if (r->frameDist == 0.0f) {
                    d.x = r->matrix[0];
                    d.z = r->matrix[2];
                } else {
                    d.x = r->vel[0];
                    d.z = r->vel[2];
                }
                d.y = 0.0f;
                zmathVecNormalize(&d, &d);
                cam->wantPos.x = h.x - d.x * 150.0f;
                cam->wantPos.y = h.y - 800.0f;
                cam->wantPos.z = h.z - d.z * 150.0f;
            } else if (cam->view == CAM_VIEW_FIRST_PERSON) {   /* the head: 100 up from the hips */
                cam->wantPos.x = r->matrix[4] * 100.0f + h.x;
                cam->wantPos.y = r->matrix[5] * 100.0f + h.y;
                cam->wantPos.z = r->matrix[6] * 100.0f + h.z;
                zactorHide(r->riderActor);
                zactorHide(r->bikeActor);
            } else {                                 /* behind / wide: 275 back along the motion */
                if (r->frameDist == 0.0f) {
                    d.x = r->matrix[0];
                    d.y = r->matrix[1];
                    d.z = r->matrix[2];
                } else {
                    d.x = r->vel[0];
                    d.y = r->vel[1];
                    d.z = r->vel[2];
                }
                if (inAirOrLocking(state))
                    d.y = 0.0f;
                if (fabsf(d.x) >= 1e-05f || fabsf(d.y) >= 1e-05f || fabsf(d.z) >= 1e-05f)
                    zmathVecNormalize(&d, &d);
                np.x = h.x - g_camFollowDist * d.x;
                np.y = h.y - g_camFollowDist * d.y;
                np.z = h.z - g_camFollowDist * d.z;
                switch (bmxphysGetState(r)) {
                case BMXPHYS_ROLL:
                    if (r->frameDist != 0.0f)
                        np.y -= g_camFollowHeight;
                    else
                        np.y -= 70.0f;               /* standing still: low */
                    break;
                case BMXPHYS_AIR:
                    if ((r->surface == BMXSURF_QUARTER || r->surface == BMXSURF_VERT) && r->vel[1] < 0.0f) {
                        prm.lookFollow = 0.75f;      /* going up a ramp wall: track the rider closely */
                        np.y = np.y - 150.0f - g_camAirHeight;
                    } else {
                        np.y = np.y - g_camAirHeight - g_camAirHeight;
                    }
                    break;
                case BMXPHYS_GRIND_LOCK:
                case BMXPHYS_STALL_LOCK:
                    np.y -= g_camAirHeight;
                    break;
                default:
                    np.y -= g_camFollowHeight;
                    break;
                }
                cam->wantPos = np;
            }
            break;

        case CAM_MODE_REPLAY:
            /* look 20 above the hips, a third towards the bike unless crashed */
            cam->wantLook.x = r->matrix[4] * 20.0f + r->physHipOffset[0] + r->pos[0];
            cam->wantLook.y = r->matrix[5] * 20.0f + r->physHipOffset[1] + r->pos[1];
            cam->wantLook.z = r->matrix[6] * 20.0f + r->physHipOffset[2] + r->pos[2];
            if (bmxphysGetState(r) != BMXPHYS_CRASH) {
                cam->wantLook.x += (r->spawnPos[0] - cam->wantLook.x) * 0.33f;
                cam->wantLook.y += (r->spawnPos[1] - cam->wantLook.y) * 0.33f;
                cam->wantLook.z += (r->spawnPos[2] - cam->wantLook.z) * 0.33f;
            }
            switch (cam->shot) {
            case 0:                                  /* circling the rider, 500 out, 300 up */
                d.x = r->vel[0];
                d.y = r->vel[1];
                d.z = r->vel[2];
                zmathVecScale(&d, &d, 0.25f);
                zmathVecAdd(&cam->wantLook, &cam->wantLook, &d);
                cam->orbitAngle = (uint16_t)(cam->orbitAngle + 300);
                cam->wantPos.x = 500.0f;
                cam->wantPos.y = -300.0f;
                cam->wantPos.z = 0.0f;
                zmathVecRotateY(&cam->wantPos, &cam->wantPos, cam->orbitAngle);
                zmathVecAdd(&cam->wantPos, &cam->wantPos, (const RwV3d *)r->pos);
                if (cam->timer > 180)
                    cam->flags |= CAMF_SHOT_DONE;
                break;

            case 1: case 3: case 5: case 7: case 10:
                /* fixed point near the ground (cameraSetShot): wait, zoom in while the rider passes,
                 * then leave once the rider is far (or after a short hold) */
                d.x = r->vel[0];
                d.y = r->vel[1];
                d.z = r->vel[2];
                zmathVecScale(&d, &d, 1.5f);
                zmathVecAdd(&cam->wantLook, &cam->wantLook, &d);
                if (cam->shotPhase == 0) {
                    if (cam->timer > 4)
                        cam->shotPhase = 1;
                } else if (cam->shotPhase == 1) {
                    if (cam->timer > 60) {
                        uint32_t lim = cam->shot != 1 ? 2000u : 1300u;

                        if (wantDist2(cam, r) < lim * lim) {
                            switch (cam->shot) {
                            case 1:  cam->wantFov = 60.0f; break;
                            case 3:  cam->wantFov = 25.0f; break;
                            case 5:
                            case 10: cam->wantFov = 30.0f; break;
                            case 7:
                                if (cam->timer > 300) {
                                    cam->timer = 0;
                                    cam->shotPhase = 2;
                                }
                                break;
                            }
                        } else {
                            cam->timer = 0;
                            cam->shotPhase = 2;
                        }
                    }
                } else if (cam->shotPhase == 2) {
                    uint8_t f = (uint8_t)(cam->flags & ~CAMF_SHOT_DONE);

                    cam->flags = f;
                    if (cam->shot == 1 ? cam->timer > 29 : (cam->shot != 5 || cam->timer > 9))
                        cam->flags = (uint8_t)(f | CAMF_SHOT_DONE);
                    if ((cam->flags & CAMF_SHOT_DONE) && !(cam->flags & CAMF_AUTO_SHOT))
                        cameraSetShot(p, cam->shot);  /* same shot from a new spot */
                }
                if (cam->shot == 7)
                    cam->wantPos.y -= 5.0f;          /* crane: rises 5 per frame */
                break;

            case 2: case 4: case 6:                  /* riding along: offset in the rider's frame */
                if (bmxphysGetState(r) == BMXPHYS_CRASH) {
                    cameraFrameCrash(cam, r, 0);
                } else {
                    RwMatrix m;

                    d.x = r->vel[0];
                    d.y = r->vel[1];
                    d.z = r->vel[2];
                    zmathVecScale(&d, &d, cam->shot == 4 ? 7.5f : 1.75f);
                    zmathVecAdd(&cam->wantLook, &cam->wantLook, &d);
                    zmathMatrixCopy(&m, (const RwMatrix *)r->matrix);
                    if (r->frameDist != 0.0f) {      /* forward = direction of motion */
                        m.right.x = r->vel[0];
                        m.right.y = r->vel[1];
                        m.right.z = r->vel[2];
                        zmathVecNormalize(&m.right, &m.right);
                        zmathTriangleNormal(&m.at, &m.right, &g_zmathOrigin, &m.up);
                    }
                    zmathVecZero(&m.pos);
                    zmathTransformPoint(&cam->wantPos, &m, (const RwV3d *)g_camShotOffset[cam->shot]);
                    zmathVecAdd(&cam->wantPos, &cam->wantPos, (const RwV3d *)r->pos);
                    zmathVecAdd(&cam->wantPos, &cam->wantPos, (const RwV3d *)r->physHipOffset);
                }
                if (cam->timer > 150)
                    cam->flags |= CAMF_SHOT_DONE;
                break;

            case 8: case 9:                          /* far fixed points: move on when 15 m / 30 m away */
                if (cam->timer > 60) {
                    uint32_t lim = cam->shot != 8 ? 3000u : 1500u;

                    if (wantDist2(cam, r) >= lim * lim) {
                        if (!(cam->flags & CAMF_AUTO_SHOT))
                            cameraSetShot(p, cam->shot);
                        else
                            cam->flags |= CAMF_SHOT_DONE;
                    }
                }
                if (cam->shot == 9 && g_cameraShake != 0 && cam->timer != 0)
                    cam->wantFov = 15.0f;
                break;
            }
            break;

        case CAM_MODE_ORBIT:                         /* run intro: 500 out, 700 up, around the rider */
            hips(r, &cam->wantLook);
            cam->orbitAngle = (uint16_t)(cam->orbitAngle + 300);
            cam->wantPos.x = 500.0f;
            cam->wantPos.y = -700.0f;
            cam->wantPos.z = 0.0f;
            zmathVecRotateY(&cam->wantPos, &cam->wantPos, cam->orbitAngle);
            zmathVecAdd(&cam->wantPos, &cam->wantPos, (const RwV3d *)r->pos);
            break;

        case CAM_MODE_FLYBY:
            /* every 256 frames a new random spot: 1000 inside the level box, on a 400 grid, on a
             * plain ground face (no prop, no flag 0x10, face class 0 or a ramp 4-8, no hazard
             * material), 500 above it; the look-at point circles it at 500. The arithmetic is
             * unsigned like the exe's (shr / mul by the 1/400 constant). */
            if ((uint8_t)cam->timer == 0) {
                const float *box = zrollerGetWorldBounds();
                BmxProbe probe;
                uint32_t cls;

                for (;;) {
                    int lo = (int)box[0];
                    uint32_t x = (((zmathRandom() & 0xFF) * (uint32_t)((int)box[4] - lo - 2000)) >> 8) + 1000 + lo;
                    uint32_t z;

                    lo = (int)box[2];
                    z = (((zmathRandom() & 0xFF) * (uint32_t)((int)box[6] - lo - 2000)) >> 8) + 1000 + lo;
                    d.y = 0.0f;
                    d.x = (float)(int16_t)(x / 400 * 400 + 1000);
                    d.z = (float)(int16_t)(z / 400 * 400 + 1000);
                    if (!bmxphysProbeGround(&d, &probe))
                        continue;
                    if (probe.flags & 1)
                        continue;
                    if (probe.face->flags & 0x10)
                        continue;
                    cls = probe.face->code >> 12;
                    if (cls != 0 && (cls < 4 || cls > 8))
                        continue;
                    if ((*probe.material & 0xF) == 6)
                        continue;
                    break;
                }
                cam->wantPos.x = probe.pos[0];
                cam->wantPos.z = probe.pos[2];
                cam->snap = 1;
                cam->wantPos.y = probe.pos[1] - 500.0f;
            }
            cam->orbitAngle = (uint16_t)(cam->orbitAngle - 50);
            cam->wantLook.x = 500.0f;
            cam->wantLook.y = 200.0f;
            cam->wantLook.z = 0.0f;
            zmathVecRotateY(&cam->wantLook, &cam->wantLook, cam->orbitAngle);
            zmathVecAdd(&cam->wantLook, &cam->wantLook, &cam->wantPos);
            break;
        }

        if (cam->snap == 0) {
            float len;

            /* the spring */
            cam->vel.x -= prm.damping * cam->vel.x;
            cam->vel.y -= prm.damping * cam->vel.y;
            cam->vel.z -= prm.damping * cam->vel.z;
            cam->look.x += (cam->wantLook.x - cam->look.x) * prm.lookFollow;
            cam->look.y += (cam->wantLook.y - cam->look.y) * prm.lookFollow;
            cam->look.z += (cam->wantLook.z - cam->look.z) * prm.lookFollow;
            cam->smoothPos.x += (cam->wantPos.x - cam->smoothPos.x) * prm.posFollow;
            cam->smoothPos.y += (cam->wantPos.y - cam->smoothPos.y) * prm.posFollow;
            cam->smoothPos.z += (cam->wantPos.z - cam->smoothPos.z) * prm.posFollow;
            d.x = cam->smoothPos.x - cam->pos.x;
            d.y = cam->smoothPos.y - cam->pos.y;
            d.z = cam->smoothPos.z - cam->pos.z;
            len = zmathVecLength(&d) * prm.spring;
            if (len > 0.0f) {
                zmathVecNormalize(&d, &d);
                cam->vel.x += len * d.x;
                cam->vel.y += len * d.y;
                cam->vel.z += len * d.z;
            }
            np.x = cam->pos.x + cam->vel.x;
            np.y = cam->vel.y + cam->pos.y;
            np.z = cam->pos.z + cam->vel.z;

            /* walls within 170 that face the camera (the look direction meets them at 0..45 deg
             * from grazing): push the camera out to 170 */
            if (!(cam->flags & CAMF_NO_WALL_PUSH)) {
                const uint8_t *near = zbspColSphere(&np, g_camWallRadius, zmathCos(0x2AAA));

                if (near != NULL) {
                    const NgBspNearFace *f;

                    for (f = *(const NgBspNearFace *const *)(near + 0x3C); f->normal != NULL; f++) {
                        int ix, iy, iz, ir;

                        d.x = f->point[0] - np.x;
                        d.y = f->point[1] - np.y;
                        d.z = f->point[2] - np.z;
                        iz = (int)d.z;
                        iy = (int)d.y;
                        ix = (int)d.x;
                        ir = (int)g_camWallRadius;
                        if ((uint32_t)(ix * ix + iy * iy + iz * iz) < (uint32_t)(ir * ir)) {
                            RwV3d toLook, n;
                            float dot;

                            toLook.x = cam->look.x - np.x;
                            toLook.y = cam->look.y - np.y;
                            toLook.z = cam->look.z - np.z;
                            n.x = f->normal[0];
                            n.y = f->normal[1];
                            n.z = f->normal[2];
                            zmathVecNormalize(&toLook, &toLook);
                            dot = zmathVecDot(&toLook, &n);
                            if (dot < 0.0f && dot > -0.7f && f->passable == 0) {
                                float l = zmathVecLength(&d), push;

                                zmathVecScale(&d, &d, 1.0f / l);
                                push = g_camWallRadius - l;
                                np.x -= push * d.x;
                                np.y -= push * d.y;
                                np.z -= push * d.z;
                            }
                        }
                    }
                }
            }

            /* at least 250 from the hips horizontally (the height difference is kept, in whole units) */
            if (!(cam->flags & CAMF_NO_MIN_DIST)) {
                int ix, iz, ir;
                int16_t iy;
                uint32_t d2;

                hips(r, &h);
                d.x = np.x - h.x;
                d.y = np.y - h.y;
                d.z = np.z - h.z;
                ix = (int)d.x;
                iy = (int16_t)(int)d.y;
                iz = (int)d.z;
                d2 = (uint32_t)(iz * iz + ix * ix);
                ir = (int)g_camMinDist;
                if (d2 < (uint32_t)(ir * ir)) {
                    d.y = 0.0f;
                    if (d2 == 0) {
                        d.x = r->matrix[0];
                        d.z = r->matrix[2];
                        d.y = 0.0f;
                    } else {
                        zmathVecNormalize(&d, &d);
                    }
                    zmathVecScale(&d, &d, g_camMinDist);
                    d.y = (float)iy;
                    np.x = h.x + d.x;
                    np.y = h.y + d.y;
                    np.z = d.z + h.z;
                }
            }

            cam->vel.x = np.x - cam->pos.x;
            cam->pos.x = np.x;
            cam->vel.y = np.y - cam->pos.y;
            cam->pos.y = np.y;
            cam->vel.z = np.z - cam->pos.z;
            cam->pos.z = np.z;

            /* line of sight blocked for more than 20 frames (not counted down again): cut */
            if (cam->mode != CAM_MODE_FLYBY) {
                NgColHit hit;

                hips(r, &h);
                if (zcolSegmentTest(&cam->pos, &h, &hit) && ++cam->blocked > 20) {
                    if (cam->mode == CAM_MODE_FOLLOW) {
                        if (r->frameDist != 0.0f && bmxphysGetState(r) != BMXPHYS_CRASH)
                            cam->snap = 1;
                    } else if (cam->mode == CAM_MODE_REPLAY) {
                        if (!(cam->flags & CAMF_AUTO_SHOT))
                            cameraSetShot(p, cam->shot);
                        else
                            cam->flags |= CAMF_SHOT_DONE;
                    }
                }
            }

            if (cam->fov != cam->wantFov) {
                float df = cam->wantFov - cam->fov;

                if (fabsf(df) <= 0.1f)
                    cam->fov = cam->wantFov;
                else
                    cam->fov += df * prm.fovFollow;
            }
        } else if (cam->snap == 1) {                 /* cut: jump straight to the target */
            cam->look = cam->wantLook;
            cam->smoothPos = cam->wantPos;
            cam->pos = cam->wantPos;
            zmathVecZero(&cam->vel);
            cam->fov = cam->wantFov;
            cam->snap = 0;
            cam->blocked = 0;
            zsndSrcResetDoppler();
        }

        if (cam->flags & CAMF_FLASH_A) {
            zrollerStartCrossFade(0x80, 0, -16);
            cam->flags &= (uint8_t)~CAMF_FLASH_A;
        } else if (cam->flags & CAMF_FLASH_B) {
            zrollerStartCrossFade(0x80, 0, -8);
            cam->flags &= (uint8_t)~CAMF_FLASH_B;
        }

        if (p == g_cameraPlayer) {
            BmxCamView v;

            if ((g_saveBlockA.cheats_active & CHEAT_GHOST_RIDER) && g_numPlayers != 0)
                zactorHide(r->riderActor);      /* "ghost rider": no rider, just the bike */
            if (!riderMatrix) {
                /* right = view direction; up = world up (y down) unless looking straight up/down */
                RwMatrix *m = &cam->matrix;
                float dot;

                m->right.x = cam->look.x - cam->pos.x;
                m->right.y = cam->look.y - cam->pos.y;
                m->right.z = cam->look.z - cam->pos.z;
                zmathVecNormalize(&m->right, &m->right);
                d.x = 0.0f;
                d.y = -1.0f;
                d.z = 0.0f;
                dot = zmathVecDot(&m->right, &d);
                if (fabsf(dot) < 0.999f) {
                    m->up.x = 0.0f;
                    m->up.y = -1.0f;
                    m->up.z = 0.0f;
                } else if (fabsf(zmathVecDot(&m->right, &m->up)) > 0.999f) {
                    m->up.x = 1.0f;
                    m->up.y = 0.0f;
                    m->up.z = 0.0f;
                }
                zmathTriangleNormal(&m->at, &m->right, &g_zmathOrigin, &m->up);
                zmathVecNormalize(&m->at, &m->at);
                zmathTriangleNormal(&m->up, &m->at, &g_zmathOrigin, &m->right);
            }
            v.obj = g_sceneCamera;
            v.fov = (int16_t)(int)(cam->fov * ANGLE_PER_DEGREE);
            v.pos = cam->pos;
            v.look = cam->look;
            zmathMatrixCopy(&v.matrix, &cam->matrix);
            cameraApply(&v);
        }
        cam->timer++;
    }
}

/* VA 0x00407470, 60 bytes, [I] row, status draft */
void cameraApply(const BmxCamView *v)
{
    zobjCameraSetFov(v->obj, v->fov);
    zobjCameraLookAt(v->obj, &v->pos, &v->look, 0);
    bmxsoundEvent(0xFA1, &v->pos, 0, 0, 0);       /* the listener follows the camera */
}

/* VA 0x004074c0, 68 bytes, [I] row, status draft */
int cameraDrawNightVision(void)
{
    /* Draw callback (cameraInit): the "night vision" cheat (cheats_active bit 1) lays a
     * semi-transparent green 640 x 480 rectangle over the screen. */
    if ((g_saveBlockA.cheats_active & CHEAT_NIGHT_VISION) && g_numPlayers != 0) {
        g_cameraNightVisionSize[0] = 640;
        g_cameraNightVisionSize[1] = 480;
        psxDrawRect(0x70007000u, 0, 0, 0x200, g_cameraNightVisionSize);
    }
    return 0;
}

/* VA 0x00407530, 147 bytes, [V] direct, status draft */
void cameraSetPlayer(uint32_t player)
{
    /* Whose camera is shown. A pending cut flash moves along with it. */
    BmxCamera *old;

    if (player > 1)
        zAssertFail("player number out of range", CAMERA_SRC, 0x49F, NULL);
    if (player == g_cameraPlayer)
        return;
    old = &g_cameras[g_cameraPlayer];
    if (old->flags & CAMF_FLASH_A) {
        g_cameras[player].flags |= CAMF_FLASH_A;
        old->flags &= (uint8_t)~CAMF_FLASH_A;
    } else if (old->flags & CAMF_FLASH_B) {
        g_cameras[player].flags |= CAMF_FLASH_B;
        old->flags &= (uint8_t)~CAMF_FLASH_B;
    }
    g_cameraPlayer = (uint8_t)player;
}

/* VA 0x00407600, 11 bytes, [I] row, status draft */
uint32_t cameraGetPlayer(void)
{
    return g_cameraPlayer;
}

/* VA 0x00407620, 205 bytes, [I] row, status draft */
void cameraSetView(uint32_t player, uint32_t view)
{
    /* The player's camera type (cameraInit from the save profile, the pause menu "camera type").
     * Leaving / entering first person while following shows / hides the rider and the bike and
     * switches the horizon. */
    BmxCamera *cam = &g_cameras[player];

    if (cam->view == view)
        return;
    cam->snap = 0;
    if (cam->view == CAM_VIEW_FIRST_PERSON && cam->mode == CAM_MODE_FOLLOW) {
        cam->snap = 1;
        cam->flags &= (uint8_t)~(CAMF_NO_MIN_DIST | CAMF_NO_WALL_PUSH);
        if (g_riders[player].stateFlags & RIDER_FLAG_ACTIVE) {
            zactorShow(g_riders[player].riderActor);
            zactorShow(g_riders[player].bikeActor);
        }
        zglEnableHorizon();
    }
    if (view == CAM_VIEW_FIRST_PERSON && cam->mode == CAM_MODE_FOLLOW) {
        cam->snap = 1;
        cam->flags |= CAMF_NO_MIN_DIST | CAMF_NO_WALL_PUSH;
        zactorHide(g_riders[player].riderActor);
        zactorHide(g_riders[player].bikeActor);
        zglDisableHorizon();
    }
    cam->view = (uint8_t)view;
    if (cam->mode == CAM_MODE_FOLLOW)
        cam->timer = 0;
}

/* VA 0x00407730, 22 bytes, [I] row, status draft */
uint8_t cameraGetView(uint32_t player)
{
    /* new function: no caller, thunk 0x0040112C */
    return g_cameras[player].view;
}

/* VA 0x00407760, 190 bytes, [I] row, status draft */
void cameraSetMode(uint32_t player, int mode)
{
    BmxCamera *cam = &g_cameras[player];

    cam->timer = 0;
    cam->snap = 0;
    cam->flags &= (uint8_t)~(CAMF_NO_MIN_DIST | CAMF_NO_WALL_PUSH);
    g_cameraShake = 0;

    /* leaving */
    switch (cam->mode) {
    case CAM_MODE_FOLLOW:
        if (cam->view == CAM_VIEW_FIRST_PERSON) {
            cam->snap = 1;
            zglEnableHorizon();                   /* horizon back on */
        }
        break;
    case CAM_MODE_REPLAY:
    case CAM_MODE_ORBIT:
    case CAM_MODE_FLYBY:
        cam->snap = 1;
        break;
    }

    /* entering */
    switch (mode) {
    case CAM_MODE_FOLLOW:
        if (cam->view == CAM_VIEW_FIRST_PERSON) {
            cam->snap = 1;
            zglDisableHorizon();                   /* horizon off */
        }
        break;
    case CAM_MODE_REPLAY:                            /* starts on shot 6 and cycles by itself */
        cam->snap = 1;
        cameraSetShot(player, 6);
        cam->mode = (uint8_t)mode;
        cam->flags = (uint8_t)((cam->flags & ~CAMF_SHOT_DONE) | CAMF_AUTO_SHOT);
        return;
    case CAM_MODE_ORBIT:
    case CAM_MODE_FLYBY:
        cam->snap = 1;
        cam->flags |= CAMF_NO_MIN_DIST | CAMF_NO_WALL_PUSH;
        break;
    }
    cam->mode = (uint8_t)mode;
}

/* VA 0x00407890, 22 bytes, [I] row, status draft */
uint8_t cameraGetMode(uint32_t player)
{
    /* new function: no caller, thunk 0x00401B8B */
    return g_cameras[player].mode;
}

/* VA 0x004078c0, 519 bytes, [I] row, status draft */
void cameraSetShot(uint32_t player, uint32_t shot)
{
    /* Replay shot 0..10 (jump table 0x00407AC8/0x00407AD0). The fixed-point shots 1, 3, 5 and 7-10
     * put the camera over the ground a little ahead of the rider: 2.5 m ahead along the facing (or
     * 25 frames of motion), snapped to a 400 grid (+200), onto the nearest respawn-capable ground
     * face, then raised 300 (shot 1), 120 (7), 800 (10) or 500. Outside the pause menu a cut to such
     * a shot flashes the screen half of the time when the rider is airborne, locking on or grinding
     * (physics states 1, 3-5; not for shots 9 and 10). Every call clears CAMF_AUTO_SHOT and CAMF_SHOT_DONE. */
    BmxCamera *cam = &g_cameras[player];
    const BmxRider *r = &g_riders[player];

    cam->timer = 0;
    cam->shotPhase = 0;
    cam->snap = 1;
    g_cameraShake = 0;

    switch (shot) {
    case 1: case 3: case 5: case 7: case 8: case 9: case 10: {
        RwV3d spot;
        int x, z, state;
        uint32_t rnd;

        if (r->frameDist == 0.0f) {
            x = (int)r->pos[0] - (int)(r->matrix[0] * -250.0f);
            z = (int)r->pos[2] - (int)(r->matrix[2] * -250.0f);
        } else {
            x = (int)r->pos[0] - (int)(r->vel[0] * -25.0f);
            z = (int)r->pos[2] - (int)(r->vel[2] * -25.0f);
        }
        spot.y = r->pos[1];
        spot.x = (float)(int16_t)((int16_t)(x / 400) * 400 + 200);
        spot.z = (float)(int16_t)((int16_t)(z / 400) * 400 + 200);
        zbspFaceCentroid(bmxphysFindRespawnFace(&spot, 2), &cam->wantPos);
        if (shot == 1)
            cam->wantPos.y -= 300.0f;
        else if (shot == 7)
            cam->wantPos.y -= 120.0f;
        else if (shot == 10)
            cam->wantPos.y -= 800.0f;
        else
            cam->wantPos.y -= 500.0f;

        if (actionGetMode() == 1) {                  /* pause menu: no flash */
            cam->flags &= (uint8_t)~CAMF_FLASH_A;
        } else if (shot < 9 || shot > 10) {
            state = bmxphysGetState(r);
            if (state == BMXPHYS_AIR || (state >= BMXPHYS_GRIND_LOCK && state <= BMXPHYS_STALL_LOCK)) {
                rnd = zmathRandom() & 3;
                if (rnd < 2)
                    cam->flags |= (uint8_t)(CAMF_FLASH_A << rnd);
            }
        }
        break;
    }
    default:                                         /* 0, 2, 4, 6: positioned by cameraUpdate */
        break;
    }
    cam->shot = (uint8_t)shot;
    cam->flags &= (uint8_t)~(CAMF_AUTO_SHOT | CAMF_SHOT_DONE);
}

/* VA 0x00407b70, 22 bytes, [I] row, status draft */
uint8_t cameraGetShot(uint32_t player)
{
    return g_cameras[player].shot;
}

/*
 * NgEngine/Zge/ZgeMods/zobjLight.c
 * Original: C:\NgEngine\Zge\ZgeMods\zobjLight.c
 * Evidence: [V] path string referenced by 2 function(s); the others [I]: the vtable 0x00561858
 * (destroy, set position, set rotation), the global 0x00673F68, and the zrwLight function table
 * 0x00563428 that every one of them calls. Absorbs the zobjLight/zScene gap (the NgIF pass's three
 * setters); the ZGE object pass created 0x0048FF50/70/90, 0x0048FFF0, 0x00490140 and 0x00490160.
 * 11 functions, VA 0x0048ff50-0x00490176.
 * Draft. Non-matching C (AD-8). Types: zobj.h. Description: docs/re/zge.md 2.4.
 */
#include "decomp.h"
#include "zobj.h"

#define ZOBJLIGHT_FILE "C:\\NgEngine\\Zge\\ZgeMods\\zobjLight.c"

static int g_zobjLightInitialised;   /* 0x00673F68 */

const ZobjVtbl g_zobjLightVtbl = {    /* 0x00561858 */
    zobjLightDestroy, 0, zobjLightSetPosition, zobjLightSetRotation, 0, 0, 0, 0, 0
};

/* VA 0x0048ff50, 23 bytes, [I] row, status draft */
int zobjLightDestroy(ZObj *light)
{
    g_zrwLightDestroy(light->handle);
    return 1;
}

/* VA 0x0048ff70, 28 bytes, [I] row, status draft */
int zobjLightSetPosition(ZObj *light, const RwV3d *pos)
{
    g_zrwLightSetPosition(light->handle, pos);
    return 1;
}

/* VA 0x0048ff90, 46 bytes, [I] row, status draft */
int zobjLightSetRotation(ZObj *light, const Zquat *rot)
{
    RwV3d axis;

    zquatToAxisAngle(&axis, rot);             /* the light points along the rotation axis */
    g_zrwLightSetDirection(light->handle, &axis);
    return 1;
}

/* VA 0x0048ffc0, 45 bytes, [V] direct, status draft */
int zobjLightInit(void)
{
    if (g_zobjLightInitialised)
        zAssertFail("Already initialized", ZOBJLIGHT_FILE, 0x99, 0);
    g_zobjLightInitialised = 1;
    return 1;
}

/* VA 0x0048fff0, 25 bytes, [I] row, status draft */
int zobjLightShutdown(void)
{
    if (g_zobjLightInitialised)
        g_zobjLightInitialised = 0;
    return 1;
}

/* VA 0x00490010, 169 bytes, [V] direct, status draft */
ZObj *zobjLightCreate(int type, const RwRGBA *colour)
{
    ZObj *light = zobjAlloc();

    if (light == 0)
        return 0;
    zobjConstruct(light, ZOBJ_TYPE_LIGHT);
    light->vtbl = &g_zobjLightVtbl;
    switch (type) {                 /* four identical cases in the exe */
    case ZOBJLIGHT_DIRECTIONAL:
    case ZOBJLIGHT_AMBIENT:
    case ZOBJLIGHT_POINT:
    case ZOBJLIGHT_SPOT:
        light->handle = g_zrwLightCreate(type, colour);
        break;
    default:
        zAssertFail("Unknown type", ZOBJLIGHT_FILE, 0xDA, 0);
        break;
    }
    return light;
}

/* VA 0x004900d0, 23 bytes, [I] row, status draft */
void zobjLightSetColour(ZObj *light, const RwRGBA *colour)
{
    g_zrwLightSetColour(light->handle, colour);
}

/* VA 0x004900f0, 42 bytes, [I] row, status draft */
void zobjLightSetFlags(ZObj *light, uint32_t flags)
{
    uint8_t rw = (flags & 1) ? 1 : 0;          /* rpLIGHTLIGHTATOMICS */

    if (flags & 2)
        rw |= 2;                                /* rpLIGHTLIGHTWORLD */
    g_zrwLightSetFlags(light->handle, rw);
}

/* VA 0x00490120, 23 bytes, [I] row, status draft */
void zobjLightSetDirection(ZObj *light, const RwV3d *dir)
{
    g_zrwLightSetDirection(light->handle, dir);
}

/* VA 0x00490140, 23 bytes, [I] row, status draft */
void zobjLightSetConeAngle(ZObj *light, int angle)
{
    g_zrwLightSetConeAngle(light->handle, (int16_t)angle);
}

/* VA 0x00490160, 23 bytes, [I] row, status draft */
void zobjLightSetRadius(ZObj *light, float radius)
{
    g_zrwLightSetRadius(light->handle, radius);
}

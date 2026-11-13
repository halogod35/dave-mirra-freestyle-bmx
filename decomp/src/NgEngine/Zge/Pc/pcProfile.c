/*
 * NgEngine/Zge/Pc/pcProfile.c
 * Original: C:\NgEngine\Zge\Pc\pcProfile.c
 * Evidence: [V] path string referenced by pcProfileInit; the others [I]: they use the counter
 * frequency 0x006764D8 or QueryPerformanceCounter (zprofile.c's callers). The ZGE object pass created
 * pcProfileShutdown / Start and moved TicksToUnits / Elapsed in from the pcProfile/end gap.
 * 5 functions, VA 0x004a45e0-0x004a46bb.
 * Draft. Non-matching C (AD-8). Description: docs/re/zge.md 7.
 */
#include "decomp.h"
#include "ztools.h"

#define PCPROFILE_FILE "C:\\NgEngine\\Zge\\Pc\\pcProfile.c"

int __stdcall QueryPerformanceFrequency(uint32_t *freq);   /* KERNEL32, LARGE_INTEGER */
int __stdcall QueryPerformanceCounter(uint32_t *count);

static int      g_pcProfileInitialised;  /* 0x006764D0 */
static uint32_t g_pcProfileFrequency[2]; /* 0x006764D8 counts per second (LARGE_INTEGER); Init clears 0x10 bytes */

/* VA 0x004a45e0, 91 bytes, [V] direct, status draft */
int pcProfileInit(void)
{
    if (g_pcProfileInitialised)
        zAssertFail("Already initialized", PCPROFILE_FILE, 0x4A, 0);
    g_pcProfileInitialised = 0;
    g_pcProfileFrequency[0] = g_pcProfileFrequency[1] = 0;
    if (!QueryPerformanceFrequency(g_pcProfileFrequency)) {
        g_pfnDebugPrintf("PROFILE: No hardware counter available\n");
        return 0;
    }
    g_pcProfileInitialised = 1;
    return 1;
}

/* VA 0x004a4640, 25 bytes, [I] row, status draft */
int pcProfileShutdown(void)
{
    if (g_pcProfileInitialised)
        g_pcProfileInitialised = 0;
    return 1;
}

/* VA 0x004a4660, 43 bytes, [I] row, status draft */
float pcProfileTicksToUnits(uint32_t ticks, float unitsPerSecond)
{
    /* ticks / (counts per unit); the frequency is loaded as a 64-bit integer (fild qword) */
    double freq = (double)g_pcProfileFrequency[0] + (double)g_pcProfileFrequency[1] * 4294967296.0;

    return (float)((double)ticks / (freq / unitsPerSecond));
}

/* VA 0x004a4690, 12 bytes, [I] row, status draft */
void pcProfileStart(uint32_t *start)
{
    QueryPerformanceCounter(start);
}

/* VA 0x004a46a0, 28 bytes, [I] row, status draft */
uint32_t pcProfileElapsed(const uint32_t *start)
{
    uint32_t now[2];

    QueryPerformanceCounter(now);
    return now[0] - start[0];           /* low words only */
}

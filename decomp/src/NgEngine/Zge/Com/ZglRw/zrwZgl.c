/*
 * NgEngine/Zge/Com/ZglRw/zrwZgl.c
 * Original: C:\NgEngine\Zge\Com\ZglRw\zrwZgl.c
 * Evidence: [V] path string 0x005614C8 (asserts of zrwZglSetValue 0x106, GetValue 0x13E/0x171,
 *           zrwZglShutdown 0x243, App_Start 0x38C, zrwZglRender 0x45F, the callback lists
 *           0x4ED-0x506, NotTested 0x633); the system driver table 0x00561468 (13 slots); [I] the
 *           rest by link order and the globals 0x006724D0-0x0067290F, 0x006976C0-0x006977CF.
 * 49 functions, VA 0x00489CC0-0x0048C10E (the zmem pass wrote the first three; the ZglRw pass the rest and
 * created 13: the dialog procedure, the memory hooks, App_CloseEngine / App_Reopen / App_Stop and
 * the small table slots).
 * Draft. Non-matching C (AD-8). Types: zglrw.h. Description: docs/re/zrw.md 1, 10.
 *
 * The RenderWare "skeleton" of the game: WinMain and the window, the device / video-mode dialog,
 * App_Open (RenderWare + plugins), App_Start (zge + the game), App_Frame (the game frame),
 * zrwZglRender (the frame render: cameras, world, the nine ZGL callback lists), the ZGL values
 * (scale, fog, clear colour ...), the render-state cache and the movie player front end.
 */
#include "decomp.h"
#include "zglrw.h"

/* USER32 / KERNEL32 imports */
int  __stdcall MessageBoxA(HWND hWnd, LPCSTR text, LPCSTR caption, UINT type);
void __stdcall OutputDebugStringA(LPCSTR text);
void __stdcall DebugBreak(void);
void __stdcall PostQuitMessage(int exitCode);
#define MB_YESNOCANCEL    0x00000003u
#define MB_ICONHAND       0x00000010u
#define MB_SETFOREGROUND  0x00010000u
#define MB_TOPMOST        0x00040000u
#define IDCANCEL          2
#define IDYES             6

char *g_zrwZglAppTitle = "RenderWare(tm) Application Error";   /* 0x005611D8 -> 0x00561204 */
char  g_zassertBuf[256];        /* 0x006723D0, real size unknown (256 assumed) */
ZassertHandler g_assertHandler = zAssertMessageBox;             /* 0x0056147C, used by zAssertFail */

/* VA 0x00489cc0, 25 bytes, [I] row, status draft */
void zrwZglErrorBox(const char *msg)
{
    /* called 15 times by App_Open/App_Start for "... failed" messages */
    MessageBoxA(NULL, msg, g_zrwZglAppTitle, MB_TOPMOST | MB_SETFOREGROUND | MB_ICONHAND);
}

/* VA 0x00489ce0, 12 bytes, [I] row, status draft */
void zrwZglDebugString(const char *msg)
{
    OutputDebugStringA(msg);
}

/* VA 0x00489cf0, 127 bytes, [I] row, status draft */
void zAssertMessageBox(const char *why, const char *file, int line, const char *func)
{
    /* The assert handler (docs/re/integrity-checks.md #8). Yes = DebugBreak (without a debugger the
     * game crashes), No = continue after the failed check, Cancel = PostQuitMessage(0). Every
     * caller in the exe passes func = NULL, so the second format is the one seen. */
    int answer;

    if (func == NULL)
        zstringPrintf(g_zassertBuf,
                      "***ASSERT***\n\nFile:%s\nLine:%d\nWhy:%s\n\nDo you wish to debug the application?\n"
                      "('Cancel' will terminate the application, 'No' will continue)",
                      file, line, why);
    else
        zstringPrintf(g_zassertBuf,
                      "***ASSERT***\n\nFunc:%s\nFile:%s\nLine:%d\nWhy:%s\n\nDo you wish to debug the application?\n"
                      "('Cancel' will terminate the application, 'No' will continue)",
                      func, file, line, why);
    /* the formatted text is not length-checked [V] */
    answer = MessageBoxA(NULL, g_zassertBuf, g_zrwZglAppTitle,
                         MB_TOPMOST | MB_SETFOREGROUND | MB_ICONHAND | MB_YESNOCANCEL);
    if (answer == IDYES)
        DebugBreak();
    else if (answer == IDCANCEL)
        PostQuitMessage(0);
}


/* ------------------------------------------------------------------ ZGL driver over RenderWare */

#define ZRWZGL_SRC "C:\\NgEngine\\Zge\\Com\\ZglRw\\zrwZgl.c"

/* USER32 / GDI / ADVAPI32 / WINMM (opaque here) */
typedef struct ZWndClass { UINT style; void *wndProc; int clsExtra, wndExtra; HINSTANCE inst; void *icon, *cursor,
                           *brush; LPCSTR menu, cls; } ZWndClass;
typedef struct ZRect { long left, top, right, bottom; } ZRect;
typedef struct ZMsg { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD_PTR time; long x, y; } ZMsg;
int   __stdcall RegisterClassA(const ZWndClass *c);
int   __stdcall AdjustWindowRect(ZRect *r, unsigned long style, int menu);
HWND  __stdcall CreateWindowExA(unsigned long ex, LPCSTR cls, LPCSTR title, unsigned long style, int x, int y,
                                int w, int h, HWND parent, void *menu, HINSTANCE inst, void *param);
int   __stdcall DestroyWindow(HWND w);
long  __stdcall SetWindowLongA(HWND w, int index, long value);
int   __stdcall SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags);
int   __stdcall ShowWindow(HWND w, int cmd);
int   __stdcall UpdateWindow(HWND w);
int   __stdcall PeekMessageA(ZMsg *m, HWND w, UINT a, UINT b, UINT remove);
int   __stdcall TranslateMessage(const ZMsg *m);
LRESULT __stdcall DispatchMessageA(const ZMsg *m);
LRESULT __stdcall DefWindowProcA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
LRESULT __stdcall SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
HWND  __stdcall GetDlgItem(HWND dlg, int id);
int   __stdcall EndDialog(HWND dlg, int result);
int   __stdcall DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, void *proc, LPARAM param);
HWND  __stdcall SetFocus(HWND w);
int   __stdcall ShowCursor(int show);
int   __stdcall ClipCursor(const ZRect *r);
void *__stdcall LoadCursorA(HINSTANCE inst, LPCSTR name);
void  __stdcall Sleep(unsigned long ms);
unsigned long __stdcall timeGetTime(void);
long  __stdcall RegOpenKeyExA(void *key, LPCSTR sub, unsigned long opts, unsigned long sam, void **out);
long  __stdcall RegQueryValueExA(void *key, LPCSTR name, unsigned long *res, unsigned long *type, uint8_t *data,
                                 unsigned long *size);
long  __stdcall RegCloseKey(void *key);
void  __cdecl   _exit(int code);
int   __cdecl   atoi(const char *s);                          /* CRT 0x004A5BB3 */
FILE *__cdecl   fopen(const char *name, const char *mode);    /* CRT 0x004A5C98 */
int   __cdecl   fclose(FILE *f);                              /* CRT 0x004A5CAB */
#define CB_ADDSTRING 0x143
#define CB_SETITEMDATA 0x151
#define CB_SETCURSEL 0x14E
#define CB_GETCURSEL 0x147
#define CB_GETITEMDATA 0x150
#define CB_RESETCONTENT 0x14B
#define WM_ACTIVATE 6
#define WM_DESTROY 2
#define WM_MOVE 3
#define WM_CREATE 1
#define WM_GETMINMAXINFO 0x24
#define WM_KEYDOWN 0x100
#define WM_QUIT 0x12
#define WM_INITDIALOG 0x110
#define WM_COMMAND 0x111
#define WM_CLOSE 0x10
#define MM_MCINOTIFY_AUDIO 0x3BD    /* the AVI player's audio tick message [I] */

/* the RW video mode record (RwVideoMode) */
typedef struct ZrwVideoMode { int32_t width, height, depth; uint32_t flags; } ZrwVideoMode;

/* other modules */
int   zrwWorldInit(void);
int   zrwWorldShutdown(void);
int   zrwWorldRender(void);
int   zgeInit(const char *cmdLine);               /* zge.c: engine start-up */
void  zgeShutdown(void);                              /* zge.c: engine shutdown */
int   zramdiskShutdown(void);                          /* 0x0049F490 */
ZInputDevice *zinputGetDevice(int index);              /* zinput.c: device by index */
int   pcinputPollDevices(void);                        /* pcinput.c gap 0x00493910 */
int   pcinputAcquireAll(void);                         /* 0x00493830 */
int   pcinputUnacquireAll(void);                       /* 0x00493890 */
void  pcmovieInit(void *player);                  /* pcmovie.c: init */
int   pcmovieOpen(void *player, const char *path, HWND w);   /* open */
void  pcmoviePlay(void *player, HWND w, int fullscreen, int a, int b);   /* play */
void  pcmovieUpdate(void *player, int a, int b);    /* update */
void  pcmovieAudioDone(void *player);                  /* audio tick */
void  pcmovieShutdown(void *player);                  /* close */
int   pcmovieIsPlaying(void *player);                      /* pcmovie.c: still playing */
extern void *g_zmemPlatformAlloc, *g_zmemPlatformFree, *g_zmemPlatformRealloc;   /* zmem.c */
extern const uint32_t g_zrwDosKeyCodes[256];           /* 0x0054DCE8 scan code -> key, 0x80 = Escape [I] */
extern void *g_zrwPrimaryDDraw;                        /* 0x0069730C IDirectDraw * (RW's) */

/* ------------------------------------------------------------------ globals */
/* the application (0x00697780, memset 0x34 by WinMain) */
HWND        g_zrwHwnd;              /* 0x00697780 */
HINSTANCE   g_zrwHinstance;         /* 0x00697784 */
int32_t     g_zrwFullscreen;        /* 0x00697788 */
const char *g_zrwWindowTitle;       /* 0x00697794 "RenderWare Application" */
int32_t     g_zrwWindowW = 640;     /* 0x00697798 */
int32_t     g_zrwWindowH = 480;     /* 0x0069779C */
int32_t     g_zrwCmdModeW, g_zrwCmdModeH, g_zrwCmdModeDepth;   /* 0x006977A0.. from "-rW.H.D" */
int32_t     g_zrwQuit;              /* 0x006977AC */
int32_t     g_zrwNoEscapeQuit;      /* 0x006977B0 set value 0x14 */
void       *g_zrwDDSurface;         /* 0x006977CC */
void       *g_zrwPlatformCalloc;    /* 0x005614A8 RW calloc (zmem.c reads it) */
void       *g_zrwPlatformMemCheck;  /* 0x005614AC = zrwZglNop */
/* the renderer (0x006976C0, memset 0x88 by App_Start) */
void       *g_zrwFrameBuffer;       /* 0x006976C0 */
void       *g_zrwZBuffer;           /* 0x006976C4 */
void       *g_zrwWorld;             /* 0x006976C8 */
float       g_zrwWorldOrigin[3];    /* 0x006976CC */
float       g_zrwWorldOriginScaled[3]; /* 0x006976D8 */
int32_t     g_zrwPvsEnabled;        /* 0x006976E8 */
int32_t     g_zrwPvsVisible, g_zrwPvsTotal;   /* 0x006976EC / 0x006976F0 */
void       *g_zrwCamera;            /* 0x006976F4 */
int32_t     g_zrwInUpdate;          /* 0x006976F8 */
float       g_zrwWorldBox[2][4];    /* 0x006976FC (set value 2) */
float       g_zrwTargetFps;         /* 0x0069771C (set value 0; 0 = unlimited) */
float       g_zrwScale = 1.0f;      /* 0x00697720 ZGE -> RW = 1 / value 1 */
float       g_zrwInvScale = 1.0f;   /* 0x00697724 */
uint32_t    g_zrwZglFlags;          /* 0x00697728 1 clamp textures, 2 mirrored quaternions, 4 (value 0xD) */
int32_t     g_zrwClearMode = 1;     /* 0x0069772C */
RwRGBA      g_zrwClearColour;       /* 0x00697730 0x00DDAE7F (127, 174, 221) */
RwRGBA      g_zrwTextColour;        /* 0x00697734 white */
uint32_t    g_zrwLastTime;          /* 0x00697738 */
uint32_t    g_zrwSecondStart;       /* 0x0069773C */
int32_t     g_zrwFps;               /* 0x00697740 */
uint32_t    g_zrwFrameCount;        /* 0x00697744 frames this second */
int32_t     g_zrwScreenW, g_zrwScreenH;   /* 0x00697770 / 0x00697774 */
uint32_t    g_zrwRenderState;       /* 0x00697778 */
void       *g_zrwRenderRaster;      /* 0x0069777C */
uint8_t     g_zrwMoviePlayer[0x400];/* 0x006973A0 pcmovie player (size [I]) */
/* this file's state (0x00672760, memset 0xA8 by App_Start) */
static struct {
    int32_t   initialized;          /* 0x00672760 */
    const char *cmdArg;             /* 0x00672764 first word of the command line */
    zList     lists[9];             /* 0x00672768 ZGL callback lists 0..8 */
    struct { uint32_t state; void *raster; } stack[4];   /* 0x006727D4 */
    int32_t   depth;                /* 0x006727F4 */
    ZmemFreeList *nodes;            /* 0x006727F8 callback nodes (0x14) */
    int32_t   reopening;            /* 0x006727FC */
    HWND      openParams;           /* 0x00672800 RwEngineOpen parameters (the window) */
    void     *memFuncs;             /* 0x00672804 */
} s_zgl;
static char s_cmdArg[0x100];        /* 0x00672808 */
static char s_subSystems[8][0x50];  /* 0x006724D0 RwSubSystemInfo names */
static int32_t s_numSubSystems;     /* 0x00672750 */
static int32_t s_subSystem;         /* 0x00672754 */
static int32_t s_videoMode;         /* 0x00672758 */
static uint32_t s_frameTime;        /* 0x0067275C */
static int32_t s_movieDisabled;     /* 0x0067288C */

/* ZGL callback node (0x14): list link, list, function, argument */
typedef struct ZglCallback {
    zListNode node;                 /* +0x00 */
    zList    *list;                 /* +0x08 */
    int (__cdecl *fn)(void *arg);   /* +0x0C */
    void     *arg;                  /* +0x10 */
} ZglCallback;

static int zrwZglRunList(zList *list)
{
    ZglCallback *c = (ZglCallback *)list->head, *next;
    for (; c != NULL; c = next) {
        next = (ZglCallback *)c->node.next;
        if (!c->fn(c->arg)) {
            g_pfnDebugPrintf("Failed in call to 0x%x\n", c->fn);
            return 0;
        }
    }
    return 1;
}

static int zrwZglIsListedMode(const ZrwVideoMode *m)
{
    if (m->depth != 16 && m->depth != 32)
        return 0;
    return (m->height == 480 && m->width == 640) || (m->height == 600 && m->width == 800) ||
           (m->height == 768 && m->width == 1024) || (m->height == 1024 && m->width == 1280) ||
           (m->height == 1200 && m->width == 1600);
}

/* VA 0x00489d70, 372 bytes, [I] between, status draft
 * The RW subsystem (display adapter) and video mode: "-rW.H.D" on the command line picks a
 * fullscreen mode directly, otherwise the device dialog (resource 0x68) asks. A fullscreen mode
 * sets the window size and hides the cursor. */
int zrwZglSelectDevice(void)
{
    HINSTANCE inst = g_zrwHinstance;
    HWND parent = g_zrwHwnd;
    ZrwVideoMode m;
    int i, n;

    s_numSubSystems = RwEngineGetNumSubSystems();
    if (s_numSubSystems == 0)
        return 0;
    if (s_numSubSystems > 8)
        s_numSubSystems = 8;
    for (i = 0; i < s_numSubSystems; i++)
        RwEngineGetSubSystemInfo(s_subSystems[i], i);
    s_subSystem = RwEngineGetCurrentSubSystem();
    if (g_zrwCmdModeW == 0 || g_zrwCmdModeH == 0 || g_zrwCmdModeDepth == 0) {
        if (DialogBoxParamA(inst, (LPCSTR)0x68, parent, (void *)zrwZglDeviceDlgProc, 0) == 0)
            return 0;
    } else {
        n = RwEngineGetNumVideoModes();
        for (i = 0; i < n; i++) {
            RwEngineGetVideoModeInfo(&m, i);
            if (m.depth == g_zrwCmdModeDepth && m.width == g_zrwCmdModeW && m.height == g_zrwCmdModeH &&
                (m.flags & 1))
                s_videoMode = i;
        }
    }
    return zrwZglReselectDevice();
}

/* VA 0x00489ef0, 123 bytes, [I] between, status draft (also App_Reopen) */
int zrwZglReselectDevice(void)
{
    ZrwVideoMode m;
    if (!RwEngineSetSubSystem(s_subSystem))
        return 0;
    if (!RwEngineSetVideoMode(s_videoMode))
        return 0;
    RwEngineGetVideoModeInfo(&m, s_videoMode);
    if (m.flags & 1) {               /* rwVIDEOMODEEXCLUSIVE */
        g_zrwWindowW = m.width;
        g_zrwWindowH = m.height;
        g_zrwFullscreen = 1;
        ShowCursor(0);
    }
    return 1;
}

static void zrwZglFillModes(HWND list, int *selItem, int *selMode)
{
    ZrwVideoMode m;
    char text[100];
    int i, n = RwEngineGetNumVideoModes();
    for (i = 0; i < n; i++) {
        RwEngineGetVideoModeInfo(&m, i);
        if (zrwZglIsListedMode(&m)) {
            LRESULT item;
            zstringPrintf(text, "%lu x %lu x %lu %s", m.width, m.height, m.depth,
                          (m.flags & 1) ? "(Fullscreen)" : "");
            item = SendMessageA(list, CB_ADDSTRING, 0, (LPARAM)text);
            SendMessageA(list, CB_SETITEMDATA, (WPARAM)item, i);
            if (selItem != NULL && m.depth == 16 && m.height == 480 && m.width == 640) {
                *selItem = (int)item;
                *selMode = i;
            }
        }
    }
}

/* VA 0x00489f70, 548 bytes, [I] between, status draft (WM_INITDIALOG)
 * Lists the subsystems (not the ones ending in "MMX" [I: 0x00561378]) and the video modes 640x480,
 * 800x600, 1024x768, 1280x1024, 1600x1200 at 16 or 32 bits; preselects 640x480x16. */
void zrwZglDeviceDlgInit(HWND dlg)
{
    HWND sub = GetDlgItem(dlg, 1000), modes = GetDlgItem(dlg, 1001);
    LRESULT item = 0, cur = 0;
    int i, selItem = 0, selMode = 0;

    for (i = 0; i < s_numSubSystems; i++) {
        const char *name = s_subSystems[i];
        if (zstringCompare(name + zstringLength(name) - 3, (const char *)0x00561378) != 0) {
            item = SendMessageA(sub, CB_ADDSTRING, 0, (LPARAM)name);
            SendMessageA(sub, CB_SETITEMDATA, (WPARAM)item, i);
        }
        if (i == s_subSystem)
            cur = item;
    }
    SendMessageA(sub, CB_SETCURSEL, (WPARAM)cur, 0);
    selItem = (int)RwEngineGetCurrentVideoMode();
    zrwZglFillModes(modes, &selItem, &selMode);
    s_videoMode = selMode;
    SendMessageA(modes, CB_SETCURSEL, (WPARAM)selItem, 0);
    SetFocus(sub);
}

/* VA 0x0048a1a0, 389 bytes, [I] between, status draft (subsystem changed: refill the modes) */
void zrwZglDeviceDlgChangeSubSystem(HWND dlg)
{
    HWND sub = GetDlgItem(dlg, 1000);
    LRESULT sel = SendMessageA(sub, CB_GETITEMDATA, (WPARAM)SendMessageA(sub, CB_GETCURSEL, 0, 0), 0);
    if (sel != s_subSystem) {
        HWND modes;
        RwEngineSetSubSystem(s_subSystem);   /* sic: the old index [V] */
        modes = GetDlgItem(dlg, 1001);
        SendMessageA(modes, CB_RESETCONTENT, 0, 0);
        zrwZglFillModes(modes, NULL, NULL);
        s_videoMode = (int32_t)RwEngineGetCurrentVideoMode();
        SendMessageA(modes, CB_SETCURSEL, (WPARAM)s_videoMode, 0);
    }
}

/* VA 0x0048a330, 304 bytes, [I] new function, status draft (the device dialog procedure) */
int __stdcall zrwZglDeviceDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_CLOSE) {
        EndDialog(dlg, 0);
        return 1;
    }
    if (msg == WM_INITDIALOG) {
        zrwZglDeviceDlgInit(dlg);
        return 0;
    }
    if (msg != WM_COMMAND)
        return 0;
    switch (wp & 0xFFFF) {
    case 1001:                         /* mode list: selection changed */
        if ((wp >> 16) == 1) {
            HWND modes = GetDlgItem(dlg, 1001);
            s_videoMode = (int32_t)SendMessageA(modes, CB_GETITEMDATA,
                                                (WPARAM)SendMessageA(modes, CB_GETCURSEL, 0, 0), 0);
        }
        return 1;
    case 1000:
        zrwZglDeviceDlgChangeSubSystem(dlg);
        return 1;
    case 1:                            /* OK */
        if ((wp >> 16) == 0)
            EndDialog(dlg, 1);
        return 1;
    case 1002:                         /* Cancel */
        if ((wp >> 16) == 0)
            EndDialog(dlg, 0);
        return 1;
    }
    return 0;
}

/* VA 0x0048a460, 915 bytes, [V] direct, status draft
 * "-rW.H.D" picks a video mode; window class "RwAppClass", 640x480 client area; App_Open,
 * App_Start, then the message loop: App_Frame + zrwZglRender per frame, throttled to the target
 * fps when one is set (ZGL value 0). Ends with _exit(-1). */
int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, char *lpCmdLine, int nShowCmd)
{
    ZWndClass wc;
    ZRect r;
    ZMsg msg;
    int i;

    if (hPrevInstance != NULL) {
        MessageBoxA(NULL, "A RenderWare(tm) Application is already running...", g_zrwZglAppTitle, 0x10);
        return 0;
    }
    zstringMemSet(&g_zrwHwnd, 0, 0x34);
    g_zrwWindowTitle = "RenderWare Application";
    g_zrwWindowW = 640;
    g_zrwWindowH = 480;
    g_zrwCmdModeW = g_zrwCmdModeH = g_zrwCmdModeDepth = 0;
    g_zrwQuit = 0;
    g_zrwFullscreen = 0;
    if (lpCmdLine != NULL)
        for (i = 0; lpCmdLine[i] != '\0'; i++)
            if (lpCmdLine[i] == '-' && lpCmdLine[i + 1] == 'r') {
                char *p = lpCmdLine + i + 2;
                int field = 0, k;
                for (k = 0; p[k] != '\0' && field < 3; k++)
                    if (p[k] == '.' || p[k] == ' ') {
                        p[k] = '\0';
                        if (field == 0) g_zrwCmdModeW = atoi(p);
                        else if (field == 1) g_zrwCmdModeH = atoi(p);
                        else g_zrwCmdModeDepth = atoi(p);
                        field++;
                        p += k + 1;
                        k = -1;
                    }
                if (field == 2)
                    g_zrwCmdModeDepth = atoi(p);
            }
    wc.style = 0x2000;
    wc.wndProc = (void *)MainWndProc;
    wc.clsExtra = wc.wndExtra = 0;
    wc.inst = hInstance;
    wc.icon = NULL;
    wc.cursor = LoadCursorA(NULL, (LPCSTR)0x7F00);
    wc.brush = NULL;
    wc.menu = NULL;
    wc.cls = "RwAppClass";
    if (!RegisterClassA(&wc))
        g_zrwHwnd = NULL;
    else {
        r.left = r.top = 0;
        r.right = g_zrwWindowW;
        r.bottom = g_zrwWindowH;
        AdjustWindowRect(&r, 0xCF0000, 0);
        g_zrwHwnd = CreateWindowExA(0, "RwAppClass", g_zrwWindowTitle, 0xCF0000, (int)0x80000000,
                                    (int)0x80000000, r.right - r.left, r.bottom - r.top, NULL, NULL,
                                    hInstance, NULL);
    }
    if (g_zrwHwnd == NULL)
        return 0;
    g_zrwHinstance = hInstance;
    if (!App_Open(g_zrwHwnd)) {
        DestroyWindow(g_zrwHwnd);
        return 0;
    }
    if (g_zrwFullscreen) {
        SetWindowLongA(g_zrwHwnd, -16 /* GWL_STYLE */, (long)0x80000000 /* WS_POPUP */);
        SetWindowPos(g_zrwHwnd, NULL, 0, 0, 0, 0, 0x27);
    }
    ShowWindow(g_zrwHwnd, nShowCmd);
    UpdateWindow(g_zrwHwnd);
    if (App_Start(lpCmdLine)) {
        while (!g_zrwQuit) {
            if (PeekMessageA(&msg, NULL, 0, 0, 1)) {
                if (msg.message == WM_QUIT)
                    break;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                continue;
            }
            if (g_zrwTargetFps != 0.0f) {
                if (s_frameTime == 0)
                    s_frameTime = timeGetTime();
                else {
                    unsigned long dt = timeGetTime() - s_frameTime;
                    if ((float)dt < 1000.0f / g_zrwTargetFps)
                        continue;
                    s_frameTime += dt;
                }
            }
            if (!App_Frame())
                break;
            zrwZglRender();
        }
    }
    _exit(-1);
    return 0;
}

/* VA 0x0048a800, 315 bytes, [I] row, status draft
 * Activation (re)acquires the DirectInput devices; Escape (scan code table 0x0054DCE8 value 0x80)
 * quits unless ZGL value 0x14 was set; WM_GETMINMAXINFO pins the window size. */
LRESULT __stdcall MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_ACTIVATE:
        if (wParam == 1)
            pcinputAcquireAll();
        else
            pcinputUnacquireAll();
        return 0;
    case WM_CREATE:
    case WM_MOVE:
        return 0;
    case WM_DESTROY:
        ClipCursor(NULL);
        PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        ZRect r;
        int32_t *mm = (int32_t *)lParam;
        int w, h;
        r.left = r.top = 0;
        r.right = g_zrwWindowW;
        r.bottom = g_zrwWindowH;
        AdjustWindowRect(&r, 0xCF0000, 0);
        w = r.right - r.left;
        h = r.bottom - r.top;
        mm[2] = w; mm[3] = h;           /* ptMaxSize */
        mm[8] = w; mm[9] = h;           /* ptMaxTrackSize */
        if (g_zrwFullscreen) {
            mm[6] = w; mm[7] = h;       /* ptMinTrackSize */
        }
        return 0;
    }
    case WM_KEYDOWN: {
        uint8_t scan = (uint8_t)(lParam >> 16);
        if (lParam & 0x1000000)
            scan |= 0x80;               /* extended key */
        if (!g_zrwNoEscapeQuit && g_zrwDosKeyCodes[scan] == 0x80)
            g_zrwQuit = 1;
        return 0;
    }
    }
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

/* VA 0x0048a940, 58 bytes, [V] direct, status draft (zfile.c: the PC file devices) */
int zfileGetPlatformDevices(zfileDeviceList *list)
{
    if (zrwFileInit() == 1) {
        if (zramdiskInit() != 1) {
            zrwFileShutdown();
            return 0;
        }
        list->count = 2;
        list->devices[0] = &zrwFileDevice;
        list->devices[1] = &zfileDevice2;
    }
    return 1;
}

/* VA 0x0048a980, 29 bytes, [I] row, status draft */
void zfileReleasePlatformDevices(void)
{
    zrwFileShutdown();
    zramdiskShutdown();
}

/* VA 0x0048a9a0, 3 bytes, [I] callers, status draft (RwEngineInit's memory functions: none) */
void *zrwZglGetMemoryFunctions(void)
{
    return NULL;
}

static void zrwZglFlipToGdi(void)
{
    if (g_zrwFullscreen && g_zrwPrimaryDDraw != NULL) {
        /* IDirectDraw::GetGDISurface (+0x90) then IDirectDrawSurface::Flip to it (+0x28) [I] */
        ((int (__stdcall *)(void *, void **))(*(void ***)g_zrwPrimaryDDraw)[36])(g_zrwPrimaryDDraw, &g_zrwDDSurface);
        ((int (__stdcall *)(void *))(*(void ***)g_zrwDDSurface)[10])(g_zrwDDSurface);
    }
}

/* VA 0x0048a9b0, 81 bytes, [I] callers, status draft (frontend.c: the quit question) */
int zrwZglConfirmQuit(void)
{
    zrwZglFlipToGdi();
    return MessageBoxA(g_zrwHwnd, "Do you want to quit the game?", "Dave Mirra Freestyle BMX", 0x10124) == 6;
}

/* VA 0x0048aa10, 255 bytes, [I] row, status draft (integrity check #9)
 * The CD check: HKLM\Software\Acclaim Entertainment\Dave Mirra Freestyle BMX\1.00.000\SRCDISK +
 * "\bin\movie\acclaim.avi" must open; otherwise "Please Insert Game CD" (Retry / Cancel = exit). */
int app_CheckGameCD(void)
{
    void *key;
    uint8_t path[0x200];
    unsigned long type, size = sizeof(path);
    FILE *f;

    if (RegOpenKeyExA((void *)0x80000002, "Software\\Acclaim Entertainment\\Dave Mirra Freestyle BMX\\1.00.000\\",
                      0, 0x20019, &key) != 0)
        return 0;
    RegQueryValueExA(key, "SRCDISK", NULL, &type, path, &size);
    RegCloseKey(key);
    zstringCat((char *)path, "\\bin\\movie\\acclaim.avi");
    for (f = fopen((char *)path, "rb"); f == NULL; f = fopen((char *)path, "rb")) {
        zrwZglFlipToGdi();
        if (MessageBoxA(g_zrwHwnd, "Please Insert Game CD", "Dave Mirra Freestyle BMX", 0x31) == IDCANCEL)
            _exit(-1);
    }
    return 1;   /* the file is left open [V] */
}

/* VA 0x0048ab10, 434 bytes, [V] direct, status draft (system slot 7: ZGL values) */
int zrwZglSetValue(int what, void *value, void *arg)
{
    const float *f = (const float *)value;
    switch (what) {
    case 0:  g_zrwTargetFps = *f; return 1;
    case 1:  g_zrwScale = 1.0f / *f; g_zrwInvScale = *f; return 1;     /* world units per RW unit */
    case 2:  zstringMemCopy(g_zrwWorldBox, value, 0x20); return 1;
    case 3:  zrwCamSetClearMode((ZglCamera *)value, (int32_t)(size_t)arg); return 1;
    case 4:  zrwCamSetClearColour((ZglCamera *)value, (const RwRGBA *)arg); return 1;
    case 5:  zrwCamSetClipPlanes((ZglCamera *)value, 0.0f, *(const float *)arg); return 1;
    case 7:  zrwCamSetFog((ZglCamera *)value, (uint32_t)(size_t)arg); return 1;
    case 8:  zrwCamSetFogColour((ZglCamera *)value, (const RwRGBA *)arg); return 1;
    case 9:  zrwCamSetFogDistance((ZglCamera *)value, *(const float *)arg); return 1;
    case 0xC:  g_zrwZglFlags = (g_zrwZglFlags & ~1u) | ((uint32_t)(size_t)value & 1); return 1;
    case 0xD:  g_zrwZglFlags = (g_zrwZglFlags & ~4u) | (((uint32_t)(size_t)value & 1) << 2); return 1;
    case 0xE:  zrwTxrSetMipmapping((int)(size_t)value); return 1;
    case 0xF:  zrwTxrFlush(); return 1;
    case 0x10: g_zrwZglFlags = (g_zrwZglFlags & ~2u) | (((uint32_t)(size_t)value & 1) << 1); return 1;
    case 0x14: g_zrwNoEscapeQuit = 1; return 1;
    }
    zAssertFail(zstringFormat("Unknown parameter %d", what), ZRWZGL_SRC, 0x106, 0);
    return 0;
}

/* VA 0x0048ad20, 446 bytes, [V] direct, status draft (system slot 8) */
int zrwZglGetValue(int what, void *out, void *arg)
{
    float *o = (float *)out;
    const float *a = (const float *)arg;
    switch (what) {
    case 0:  *o = g_zrwTargetFps; return 1;
    case 1:  *o = g_zrwInvScale; return 1;
    case 2:  zstringMemCopy(out, g_zrwWorldBox, 0x20); return 1;
    case 3:  *(int32_t *)o = g_zrwClearMode; return 1;
    case 4:  *(RwRGBA *)o = g_zrwClearColour; return 1;
    case 5:  *o = 0.0f; return 0;
    case 6:
        if (arg == NULL)
            zAssertFail("Invalid camera specified.", ZRWZGL_SRC, 0x13E, 0);
        *o = RwIm2DGetNearScreenZ();
        return 1;
    case 7: case 8: case 9:
        return 1;
    case 10: *o = (float)g_zrwScreenW; return 1;
    case 11: *o = (float)g_zrwScreenH; return 1;
    case 0x11:                           /* RW vector -> ZGE */
        o[0] = g_zrwInvScale * a[0];
        o[1] = -(a[1] * g_zrwInvScale);
        o[2] = -(a[2] * g_zrwInvScale);
        return 1;
    case 0x12: *o = g_zrwScale * *a; return 1;
    case 0x13: *o = g_zrwInvScale * *a; return 1;
    case 0x15: *(void **)o = &g_zrwHwnd; return 1;
    }
    zAssertFail(zstringFormat("Unknown parameter %d", what), ZRWZGL_SRC, 0x171, 0);
    return 0;
}

static void zrwZglHookMemoryFns(void)
{
    void **m = (void **)RwOsGetMemoryInterface();
    g_zmemPlatformAlloc = m[0];
    g_zmemPlatformFree = ((void **)RwOsGetMemoryInterface())[1];
    g_zmemPlatformRealloc = ((void **)RwOsGetMemoryInterface())[2];
    g_zrwPlatformCalloc = ((void **)RwOsGetMemoryInterface())[3];
    g_zrwPlatformMemCheck = (void *)zrwZglNop;
}

/* VA 0x0048af40, 64 bytes, [I] new function, status draft (no caller found)
 * zmem's platform heap = RenderWare's allocator (malloc / free / realloc / calloc). */
void zrwZglHookMemory(void)
{
    zrwZglHookMemoryFns();
}

/* VA 0x0048af80, 1 bytes, [I] new function, status draft (an empty memory-check hook) */
void zrwZglNop(void)
{
}

/* VA 0x0048af90, 69 bytes, [I] new function, status draft (no caller found) */
int zrwZglHookMemoryRet(void)
{
    zrwZglHookMemoryFns();
    return 1;
}

/* VA 0x0048afe0, 297 bytes, [I] callers, status draft (system slot 0; zge.c installs the drivers)
 * Defaults (scale 1, clamp textures, clear colour 127,174,221, white text), then the drivers in
 * order texture, material, model, light, collision, primitives, then the world. */
int zrwZglInit(void)
{
    zstringMemSet(&g_zrwScreenW, 0, 0x10);
    g_zrwScreenH = g_zrwWindowH;
    g_zrwScreenW = g_zrwWindowW;
    g_zrwZglFlags = (g_zrwZglFlags & ~4u) | 1;
    g_zrwTargetFps = 0.0f;
    g_zrwClearMode = 1;
    *(uint32_t *)&g_zrwTextColour = 0xFFFFFFFF;
    *(uint32_t *)&g_zrwClearColour = 0x00DDAE7F;
    g_zrwScale = 1.0f;
    g_zrwInvScale = 1.0f;
    if (!g_texDriverVtbl.init())
        return 0;
    if (!g_zglMaterialApi.init())
        return 0;
    if (!g_zglModelApi.init())
        return 0;
    if (!g_zglLightApi.init())
        return 0;
    if (!g_zglColApi.init())
        return 0;
    if (!g_zglPrimApi.init())
        return 0;
    if (!zrwWorldInit()) {
        zrwZglErrorBox("Cannot create world.");
        return 0;
    }
    s_zgl.nodes = zmemFreeListCreate(sizeof(ZglCallback), 0x20, NULL);
    return 1;
}

/* VA 0x0048b110, 140 bytes, [I] new function, status draft (system slot 1) */
int zrwZglShutdown(void)
{
    if (!s_zgl.initialized)
        zAssertFail("Not initialized", ZRWZGL_SRC, 0x243, 0);
    s_zgl.initialized = 0;
    if (!zrwWorldShutdown() || !g_zglPrimApi.shutdown() || !g_zglColApi.shutdown() ||
        !g_zglLightApi.shutdown() || !g_zglModelApi.shutdown() || !g_zglMaterialApi.shutdown() ||
        !g_texDriverVtbl.shutdown())
        return 0;
    zmemFreeListDestroy(s_zgl.nodes);
    return 1;
}

static int zrwZglOpenEngine(HWND hWnd)
{
    if (!s_zgl.reopening)
        s_zgl.memFuncs = zrwZglGetMemoryFunctions();
    RwEngineInit(s_zgl.memFuncs, 0);
    if (!RpWorldPluginAttach()) { zrwZglErrorBox("RpWorldPluginAttach failed"); return 0; }
    if (!RpAnimPluginAttach()) { zrwZglErrorBox("RpAnimPluginAttach failed"); return 0; }
    if (!RpBonePluginAttach()) { zrwZglErrorBox("RpBonePluginAttach failed"); return 0; }
    if (!RpLODAtomicPluginAttach()) { zrwZglErrorBox("RpLODAtomicPluginAttach failed"); return 0; }
    if (!RpPVSPluginAttach()) { zrwZglErrorBox("RpPVSPluginAttach failed"); return 0; }
    if (!zrwTxrRegisterPlugin()) { zrwZglErrorBox("rwMatRegisterPlugin failed"); return 0; }
    if (!zrwModelRegisterPlugin()) { zrwZglErrorBox("zrwMdlRegisterPlugin failed"); return 0; }
    s_zgl.openParams = hWnd;
    RwEngineOpen(&s_zgl.openParams);
    if (!(s_zgl.reopening ? zrwZglReselectDevice() : zrwZglSelectDevice()))
        return 0;
    return RwEngineStart() != 0;
}

/* VA 0x0048b1a0, 274 bytes, [I] row, status draft
 * RenderWare start-up: RwEngineInit, the plugins (world, Anim, Bone, LOD atomic, PVS, the zrwTxr
 * texture plugin, the zrwModel clump plugin), RwEngineOpen on the window, the device, RwEngineStart. */
int App_Open(HWND hWnd)
{
    return zrwZglOpenEngine(hWnd);
}

/* VA 0x0048b2c0, 21 bytes, [I] new function, status draft (no caller found) */
int App_CloseEngine(void)
{
    RwEngineStop();
    RwEngineClose();
    RwEngineTerm();
    return 1;
}

/* VA 0x0048b2e0, 357 bytes, [I] new function, status draft (no caller found)
 * Device switch: everything RenderWare-owned is released, the engine reopened on the selected
 * device and the resources rebuilt (frame buffers, texture dictionary, debug charset, model scratch). */
int App_Reopen(void)
{
    s_zgl.reopening = 1;
    zrwDbgDestroyCharset();
    zrwTxrCloseDictionary();
    zrwCamDestroyFrameBuffers();
    zrwModelClose();
    RwEngineStop();
    RwEngineClose();
    RwEngineTerm();
    zrwZglOpenEngine(s_zgl.openParams);
    RwEngineSetSomething(0x200000);
    zrwCamCreateFrameBuffers();
    zrwTxrOpenDictionary();
    zrwDbgCreateCharset();
    zrwModelOpen();
    return 1;
}

/* VA 0x0048b450, 271 bytes, [I] row, status draft
 * Clears the renderer state, hooks zmem's platform heap to RW's allocator, takes the first word of
 * the command line (the scene / level argument for zge), starts the engine (zge) and the game
 * (ngappInit). */
int App_Start(char *cmdLine)
{
    if (s_zgl.initialized)
        zAssertFail("Already initialized", ZRWZGL_SRC, 0x38C, 0);
    zstringMemSet(&g_zrwFrameBuffer, 0, 0x88);
    zstringMemSet(&s_zgl, 0, 0xA8);
    s_zgl.initialized = 1;
    zrwZglHookMemoryFns();
    if (cmdLine != NULL)
        while (*cmdLine != '\0') {
            char *d = s_cmdArg;
            while (*cmdLine != '\0' && *cmdLine != ' ')
                *d++ = *cmdLine++;
            *d = '\0';
            cmdLine++;
            if (s_cmdArg[0] != '\0') {
                s_zgl.cmdArg = s_cmdArg;
                break;
            }
        }
    if (!zgeInit(s_zgl.cmdArg))
        return 0;
    if (!ngappInit())
        return 0;
    g_zrwSecondStart = timeGetTime();
    g_zrwLastTime = g_zrwSecondStart;
    return 1;
}

/* VA 0x0048b560, 16 bytes, [I] new function, status draft (no caller found) */
int App_Stop(void)
{
    ngappShutdown();
    zgeShutdown();
    return 1;
}

/* VA 0x0048b570, 274 bytes, [I] row, status draft
 * One game frame: dt in seconds, the fps counter, callback list 7, ngappFrame(dt) (0 = quit), list 8. */
int App_Frame(void)
{
    unsigned long now = timeGetTime();
    float dt = (now < g_zrwLastTime) ? 0.0f : (float)(now - g_zrwLastTime) * 0.001f;
    uint32_t frames = g_zrwFrameCount + 1;

    g_zrwLastTime = now;
    g_zrwFrameCount = frames;
    if (g_zrwSecondStart + 1000 <= now) {
        g_zrwFrameCount = 0;
        g_zrwSecondStart = now;
        g_zrwFps = (int32_t)frames;
    }
    zrwZglRunList(&s_zgl.lists[7]);
    if (!ngappFrame(dt)) {
        g_zrwQuit = 1;
        return 0;
    }
    zrwZglRunList(&s_zgl.lists[8]);
    return 1;
}

/* VA 0x0048b690, 693 bytes, [I] callers, status draft (the frame render)
 * Clear every camera that owns rasters, list 0, then per camera: begin update (fog, render state),
 * PVS view point, list 1, the world, PVS statistics, list 2, list 5, list 4, end update (a pending RW
 * error asserts); then list 3 and RwRasterShowRaster. */
int zrwZglRender(void)
{
    ZglCamera *cam;
    uint32_t err[2];

    if (g_zrwFrameBuffer == NULL)
        return 1;
    if (g_zrwClearMode)
        for (cam = zrwCamGetFirst(); cam != NULL; cam = zrwCamGetNext(cam))
            if (cam->flags & 4)
                RwCameraClear(zrwCamGetRwCamera(cam), &g_zrwClearColour, 3);
    zrwZglRunList(&s_zgl.lists[0]);
    for (cam = zrwCamGetFirst(); cam != NULL; cam = zrwCamGetNext(cam)) {
        if ((cam->flags & 4) == 0)
            continue;
        RwErrorGet(err);
        if (!zrwCamBeginUpdate(cam))
            continue;
        if (g_zrwPvsEnabled) {
            RpPVSHook(g_zrwWorld);
            RpPVSSetViewPosition(g_zrwWorld,
                                 &RwFrameGetMatrix(RwCameraGetFrame(zrwCamGetRwCamera(cam)))->pos);
        }
        zrwZglRunList(&s_zgl.lists[1]);
        if (!zrwWorldRender())
            return 0;
        if (g_zrwPvsEnabled) {
            RpPVSStatisticsGet(g_zrwWorld, &g_zrwPvsVisible, &g_zrwPvsTotal);
            RpPVSUnhook(g_zrwWorld);
        }
        zrwZglResetRenderState();
        zrwZglRunList(&s_zgl.lists[2]);
        zrwZglResetRenderState();
        zrwZglRunList(&s_zgl.lists[5]);
        zrwZglRunList(&s_zgl.lists[4]);
        if (!zrwCamEndUpdate())
            return 0;
        RwErrorGet(err);
        if (err[1] != 0x80000000) {
            zAssertFail("There is a pending RenderWare error", ZRWZGL_SRC, 0x45F, 0);
            return 0;
        }
    }
    zrwZglRunList(&s_zgl.lists[3]);
    RwRasterShowRaster(g_zrwFrameBuffer, g_zrwHwnd, 1);
    return 1;
}

/* VA 0x0048b950, 35 bytes, [I] callers, status draft (zrwWorld: show the frame without the loop) */
void zrwZglShowFrame(int vsync)
{
    RwRasterShowRaster(g_zrwFrameBuffer, g_zrwHwnd, vsync != 0);
}

/* VA 0x0048b980, 11 bytes, [I] new function, status draft (no caller found) */
void zrwZglRequestQuit(void)
{
    g_zrwQuit = 1;
}

/* VA 0x0048b990, 77 bytes, [I] callers, status draft (zrwDbg: a callback in a private list) */
void *zrwZglAddCallbackToList(zList *list, void *fn, void *arg, int tail)
{
    ZglCallback *c = (ZglCallback *)zmemFreeListAlloc(s_zgl.nodes);
    if (c == NULL)
        return NULL;
    if (tail == 0)
        zListAddHead(list, &c->node);
    else
        zListAddTail(list, &c->node);
    c->list = list;
    c->fn = (int (__cdecl *)(void *))fn;
    c->arg = arg;
    return c;
}

/* VA 0x0048b9e0, 65 bytes, [I] callers, status draft */
int zrwZglRunCallbackList(zList *list)
{
    return zrwZglRunList(list);
}

/* VA 0x0048ba30, 153 bytes, [V] direct, status draft (system slot 2; list 0..8; tail = append) */
void *zrwZglAddCallback(int list, void *fn, void *arg, int tail)
{
    if (!s_zgl.initialized)
        zAssertFail("Not initialized", ZRWZGL_SRC, 0x4ED, 0);
    if (list > 8)
        zAssertFail("Invalid type", ZRWZGL_SRC, 0x4F0, 0);
    return zrwZglAddCallbackToList(&s_zgl.lists[list], fn, arg, tail);
}

/* VA 0x0048bad0, 70 bytes, [V] direct, status draft (system slot 3) */
int zrwZglRemoveCallback(void *node)
{
    ZglCallback *c = (ZglCallback *)node;
    if (c == NULL) {
        zAssertFail((const char *)0x005603D8, ZRWZGL_SRC, 0x506, 0);
        return 0;
    }
    zListRemove(c->list, &c->node);
    zmemFreeListFree(s_zgl.nodes, c);
    return 1;
}

/* VA 0x0048bb20, 66 bytes, [I] callers, status draft (system slot 4) */
void zrwZglRunCallbacks(int list)
{
    zrwZglRunList(&s_zgl.lists[list]);
}

static void zrwZglApplyBlend(uint32_t state)
{
    RwRenderStateSet(8 /* src blend */, (state >> 8) & 0xF);
    RwRenderStateSet(9 /* dest blend */, (state >> 12) & 0xF);
}

/* VA 0x0048bb70, 229 bytes, [I] callers, status draft (system slot 9: state 0x6502, no texture) */
void zrwZglResetRenderState(void)
{
    int address;
    g_zrwRenderRaster = NULL;
    g_zrwRenderState = 0x6502;
    RwRenderStateSet(1, 0);
    address = ((g_zrwRenderState >> 4) & 2) + 1;
    RwRenderStateSet(2, address);
    RwRenderStateSet(0x12, address);
    RwRenderStateSet(0x13, address);
    RwRenderStateSet(3, 1);
    RwRenderStateSet(4, 1);
    RwRenderStateSet(5, 2);
    RwRenderStateSet(6, 1);
    RwRenderStateSet(7, 2);
    RwRenderStateSet(10, (~g_zrwRenderState >> 3) & 1);
    RwRenderStateSet(0xB, 0xFFFFFFFF);
    RwRenderStateSet(0xC, 0);
    RwRenderStateSet(0xD, 0xFFFFFFFF);
    RwRenderStateSet(0xE, 1);
    RwRenderStateSet(0x11, 1);
    zrwZglApplyBlend(g_zrwRenderState);
}

/* VA 0x0048bc60, 288 bytes, [I] callers, status draft (system slot 10)
 * ZGL render state bits: 1 no z (test + write off), 2 blending on (else src/dest forced to 8/1 [I]),
 * 4 flat shading, 8 no vertex alpha, 0x20 clamp addressing, 0x0F00 src blend, 0xF000 dest blend;
 * only changed bits reach RenderWare. raster = the texture. */
int zrwZglSetRenderState(uint32_t state, void *raster)
{
    uint32_t diff = g_zrwRenderState ^ state;
    if (diff != 0) {
        if (diff & 1) {
            RwRenderStateSet(4, (state & 1) == 0);
            RwRenderStateSet(6, (state & 1) == 0);
        }
        if (diff & 4)
            RwRenderStateSet(7, (state & 4) ? 1 : 2);
        if (diff & 0x20) {
            int a = ((state >> 4) & 2) + 1;
            RwRenderStateSet(2, a);
            RwRenderStateSet(0x12, a);
            RwRenderStateSet(0x13, a);
        }
        if (diff & 0xFF02) {
            if ((state & 2) == 0) {
                state = (state & 0xFFFF12FF) | 0x1208;
                diff = g_zrwRenderState ^ state;
            }
            if (diff & 0xFF00)
                zrwZglApplyBlend(state);
        }
        if (diff & 8)
            RwRenderStateSet(10, (state & 8) == 0);
        g_zrwRenderState = state;
    }
    if (raster != g_zrwRenderRaster) {
        RwRenderStateSet(1, (uint32_t)(size_t)raster);
        g_zrwRenderRaster = raster;
    }
    return 1;
}

/* VA 0x0048bd80, 75 bytes, [I] callers, status draft (system slot 11, depth 4) */
int zrwZglPushRenderState(void)
{
    if (s_zgl.depth == 4) {
        g_pfnDebugPrintf("Render state stack is full");
        return 0;
    }
    s_zgl.stack[s_zgl.depth].state = g_zrwRenderState;
    s_zgl.stack[s_zgl.depth].raster = g_zrwRenderRaster;
    s_zgl.depth++;
    return 1;
}

/* VA 0x0048bdd0, 94 bytes, [I] callers, status draft (system slot 12) */
int zrwZglPopRenderState(void)
{
    if (s_zgl.depth == 0) {
        g_pfnDebugPrintf("Render state stack is empty");
        return 0;
    }
    s_zgl.depth--;
    if (s_zgl.stack[s_zgl.depth].state != g_zrwRenderState ||
        s_zgl.stack[s_zgl.depth].raster != g_zrwRenderRaster)
        g_zglSystemApi.setRenderState(s_zgl.stack[s_zgl.depth].state, s_zgl.stack[s_zgl.depth].raster);
    return 1;
}

/* VA 0x0048be30, 127 bytes, [I] callers, status draft (an RW matrix -> ZGE axes and units) */
void zrwZglMatrixFromRw(float *out, const RwMatrix *m)
{
    out[0] = m->right.x;   out[1] = -m->right.y;  out[2] = -m->right.z;
    out[4] = -m->up.x;     out[5] = m->up.y;      out[6] = m->up.z;
    out[8] = -m->at.x;     out[9] = m->at.y;      out[10] = m->at.z;
    out[12] = (m->pos.x - g_zrwWorldOrigin[0]) * g_zrwInvScale;
    out[13] = -((m->pos.y - g_zrwWorldOrigin[1]) * g_zrwInvScale);
    out[14] = -((m->pos.z - g_zrwWorldOrigin[2]) * g_zrwInvScale);
}

/* VA 0x0048beb0, 28 bytes, [I] new function, status draft (no caller found) */
int zrwZglNotTested(void)
{
    zAssertFail("Not tested", ZRWZGL_SRC, 0x633, 0);
    return 0;
}

/* VA 0x0048bed0, 575 bytes, [I] callers, status draft (NgApp: intro movies)
 * Plays an AVI through pcmovie.c unless the file is missing (returns 1 = carry on): polls input every
 * 40 ms, any newly pressed button aborts; then half a second of input polling. Returns 0 if the user
 * quit meanwhile. */
int zrwZglPlayMovie(const char *path)
{
    char name[0x200];
    FILE *f;
    ZMsg msg;
    unsigned long t;

    zstringCopy(name, path);
    f = fopen(name, "rb");
    if (f == NULL)
        return 1;
    fclose(f);
    if (s_movieDisabled)
        return g_zrwQuit == 0;
    pcmovieInit(g_zrwMoviePlayer);
    if (!pcmovieOpen(g_zrwMoviePlayer, name, g_zrwHwnd))
        return 1;
    pcmoviePlay(g_zrwMoviePlayer, g_zrwHwnd, g_zrwFullscreen, 0, 0);
    t = g_zglSystemApi.getTime();
    for (;;) {
        int abort = 0, i;
        if (!PeekMessageA(&msg, NULL, 0, 0, 1)) {
            if (g_zglSystemApi.getTime() - t > 0x27) {
                t = g_zglSystemApi.getTime();
                pcmovieUpdate(g_zrwMoviePlayer, 0, 0);
                pcinputPollDevices();
                for (i = 0; i < 2; i++) {
                    ZInputDevice *d = zinputGetDevice(i);
                    if (d != NULL && d->buttons != 0 && (d->changed & d->buttons) != 0)
                        abort = 1;
                }
            }
            Sleep(1);
        } else if (msg.message == MM_MCINOTIFY_AUDIO) {
            g_pfnDebugPrintf("Movie : Audio Tick\n");
            pcmovieAudioDone(g_zrwMoviePlayer);
        } else {
            if (msg.message == WM_QUIT)
                _exit(-1);
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (abort)
            g_pfnDebugPrintf("Movie : Key Abort\n");
        if (abort || !pcmovieIsPlaying(g_zrwMoviePlayer) || g_zrwQuit)
            break;
    }
    g_pfnDebugPrintf("Movie : Done Playing\n");
    pcmovieShutdown(g_zrwMoviePlayer);
    t = g_zglSystemApi.getTime();
    while (g_zglSystemApi.getTime() - t < 500) {
        Sleep(0x14);
        pcinputPollDevices();
        if (PeekMessageA(&msg, NULL, 0, 0, 1) && msg.message != MM_MCINOTIFY_AUDIO) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return g_zrwQuit == 0;
}

/* The system driver table 0x00561468 (zge.c) */
const ZglSystemApi g_zglSystemApi = {
    zrwZglInit, zrwZglShutdown, zrwZglAddCallback, zrwZglRemoveCallback, zrwZglRunCallbacks,
    zAssertMessageBox, (uint32_t (__cdecl *)(void))timeGetTime, zrwZglSetValue, zrwZglGetValue,
    zrwZglResetRenderState, zrwZglSetRenderState, zrwZglPushRenderState, zrwZglPopRenderState
};

/*
 * ztools.h - the small engine modules between zpanorama.c and the C runtime (DM-061):
 *   ztoken.c     the script tokenizer (Scene.Txt, ZSD build files): #INCLUDE, #DEFINE, $symbols
 *   zFont.c      ZFNT glyph-set fonts
 *   zutil.c      the bounding-box helpers DM-044's zutil.c lacked
 *   zrwRamDisk.c a RenderWare file interface reading from registered memory blocks
 *   zprofile.c   the on-screen CPU profiler bars
 *   ztimer.c     timed callbacks on a binary heap
 *   pcProfile.c  QueryPerformanceCounter timing
 *   zheap.c      the binary heap (priority queue)
 * Offsets [V] (field use in these files); names [I]. Description: docs/re/zge.md 3-6.
 */
#ifndef ZTOOLS_H
#define ZTOOLS_H
#include "dmtypes.h"
#include "zlist.h"
#include "rwtypes.h"

/* ------------------------------------------------------------------ ztoken.c */
#define ZTOKEN_MAX_DEPTH   4       /* the main file + 3 nested includes */
#define ZTOKEN_SYMBOL_SPACE 0x400  /* "$name@value" pairs */

/* One open script file (g_ztokenFiles[4] 0x00675380; an include copies its parent's record). */
typedef struct ZtokenFile {
    void    *file;                 /* +0x000 zfileHandle */
    char     path[0x100];          /* +0x004 */
    char     line[0x100];          /* +0x104 the current line (zfileGets, 0xFF chars) */
    uint16_t pos;                  /* +0x204 read position in line */
    uint16_t pad206;               /* +0x206 */
    int      lineNo;               /* +0x208 */
    char     includePath[0x100];   /* +0x20C prefix for #INCLUDE / $$$ (#INCLUDE_PATH) */
} ZtokenFile;                      /* 0x30C */

/* ------------------------------------------------------------------ zFont.c */
/* zFont.c is C++: a Zfont and a font's glyph array are allocated with new[] (a count word in front
 * of the block). A glyph is a vector shape: points and strokes (point index lists). */
typedef struct ZfontPoint {
    float    x;                    /* +0x00 from the file (16 bytes per point: x, y, u, v) [I] */
    float    y;                    /* +0x04 */
    uint32_t unk08;                /* +0x08 cleared */
    uint32_t unk0C;                /* +0x0C not written */
    float    u;                    /* +0x10 */
    float    v;                    /* +0x14 */
    uint32_t unk18;                /* +0x18 not written */
} ZfontPoint;                      /* 0x1C */

typedef struct ZfontStroke {
    uint32_t     unk00;            /* +0x00 cleared */
    ZfontPoint **points;           /* +0x04 */
    uint32_t     count;            /* +0x08 */
} ZfontStroke;                     /* 0x0C */

typedef struct ZfontGlyph {
    uint16_t     numPoints;        /* +0x00 0 = no glyph for this character */
    uint16_t     numStrokes;       /* +0x02 */
    ZfontPoint  *points;           /* +0x04 */
    ZfontStroke *strokes;          /* +0x08 */
} ZfontGlyph;                      /* 0x0C */

typedef struct Zfont {
    zListNode   node;              /* +0x00 in g_zfontList */
    uint16_t    numGlyphs;         /* +0x08 */
    uint16_t    pad0A;             /* +0x0A */
    ZfontGlyph *glyphs;            /* +0x0C new ZfontGlyph[numGlyphs] */
} Zfont;                           /* 0x10 */

/* ------------------------------------------------------------------ zrwRamDisk.c */
typedef struct ZrwRamDiskFile {
    void       *address;           /* +0x00 */
    uint32_t    size;              /* +0x04 */
    const char *extension;         /* +0x08 upper case, NULL = any; rwfexist accepts only it */
} ZrwRamDiskFile;                  /* 0x0C */

/* ------------------------------------------------------------------ zprofile.c */
#define ZPROFILE_BAR_USER   0
#define ZPROFILE_BAR_APP    1      /* "App": frame callbacks 7 (start) .. 8 (stop) */
#define ZPROFILE_BAR_ENGINE 2      /* "EngCPU": frame callback 0 (start) */

typedef struct ZprofileBar {
    uint32_t    flags;             /* +0x00 bit 0: timing */
    int         type;              /* +0x04 ZPROFILE_BAR_* */
    const char *name;              /* +0x08 drawn left of the bar */
    uint32_t    ticks;             /* +0x0C accumulated this frame (performance counter) */
    uint32_t    peak;              /* +0x10 */
    float       scale;             /* +0x14 frames (1/60 s) for the full width */
    uint32_t    colour;            /* +0x18 RwRGBA */
    uint32_t    start[2];          /* +0x1C LARGE_INTEGER */
} ZprofileBar;                     /* 0x24 */

typedef struct ZprofileLayout {    /* zprofileSetLayout copies 12 words */
    uint32_t margin;               /* +0x00 0x20 */
    uint32_t offsetX;              /* +0x04 0 */
    uint32_t offsetY;              /* +0x08 0 */
    uint32_t barHeight;            /* +0x0C 8 */
    uint32_t fillHeight;           /* +0x10 6 */
    uint32_t barGap;               /* +0x14 2 */
    uint32_t tickHeight;           /* +0x18 4 */
    uint32_t tickWidth;            /* +0x1C 8 */
    uint32_t tickLength;           /* +0x20 4 */
    uint32_t defaultScale;         /* +0x24 2 (frames) */
    uint32_t peakPeriod;           /* +0x28 0x78 frames */
    uint32_t fps;                  /* +0x2C 0x3C */
} ZprofileLayout;                  /* 0x30 */

typedef struct ZprofileGlobals {   /* 0x00676438, cleared by zprofileInit */
    int            initialised;    /* +0x00 */
    void          *pool;           /* +0x04 ZmemFreeList of ZprofileBar */
    int            drawCb;         /* +0x08 ZGL frame callback (priority 4) */
    int            peakDelay;      /* +0x0C frames before peaks are recorded (10) */
    int            peakCountdown;  /* +0x10 */
    int            peakPeriod;     /* +0x14 0 = never reset the peaks */
    float          x;              /* +0x18 */
    float          y;              /* +0x1C */
    float          defaultScale;   /* +0x20 */
    ZprofileBar   *appBar;         /* +0x24 */
    int            appStartCb;     /* +0x28 */
    int            appStopCb;      /* +0x2C */
    ZprofileBar   *engineBar;      /* +0x30 */
    int            engineCb;       /* +0x34 */
    ZprofileLayout layout;         /* +0x38 */
} ZprofileGlobals;                 /* 0x68 */

/* ------------------------------------------------------------------ zheap.c / ztimer.c */
typedef struct ZheapItem {
    uint32_t key;                  /* +0x00 */
    void    *value;                /* +0x04 */
} ZheapItem;                       /* 0x08 */

typedef struct Zheap {             /* 1-based array, items[0] is a sentinel */
    int        capacity;           /* +0x00 at least 10 */
    int        count;              /* +0x04 */
    int        maxHeap;            /* +0x08 0: smallest key on top */
    ZheapItem *items;              /* +0x0C capacity + 1 */
} Zheap;                           /* 0x10 */

#define ZTIMER_CALLBACK  1         /* fn(now, arg1, arg2) */
#define ZTIMER_EVENT     2         /* prints "Event type timer for obj %d" (fn = object id) */
#define ZTIMER_SCRIPT    3         /* prints "Script type timer" */
#define ZTIMER_COUNTDOWN 4         /* polled with ztimerPoll */
typedef void (__cdecl *ZtimerFn)(uint32_t now, void *arg1, void *arg2);
typedef struct ZtimerEvent {
    zListNode node;                /* +0x00 in the free list */
    uint32_t  due;                 /* +0x08 timeGetTime ms */
    uint32_t  interval;            /* +0x0C */
    int       type;                /* +0x10 ZTIMER_* */
    void     *arg1;                /* +0x14 */
    void     *arg2;                /* +0x18 */
    ZtimerFn  fn;                  /* +0x1C */
} ZtimerEvent;                     /* 0x20 */

/* GHIDRA-SKIP-BEGIN */
typedef char ztools_h_size_check[(sizeof(ZtokenFile) == 0x30C && sizeof(ZfontPoint) == 0x1C &&
                                  sizeof(ZfontStroke) == 0x0C && sizeof(ZfontGlyph) == 0x0C &&
                                  sizeof(Zfont) == 0x10 && sizeof(ZrwRamDiskFile) == 0x0C &&
                                  sizeof(ZprofileBar) == 0x24 && sizeof(ZprofileLayout) == 0x30 &&
                                  sizeof(ZprofileGlobals) == 0x68 && sizeof(ZheapItem) == 0x08 &&
                                  sizeof(Zheap) == 0x10 && sizeof(ZtimerEvent) == 0x20) ? 1 : -1];

/* ztoken.c */
int       ztokenOpen(char *path);                                  /* 0x004A1120 */
int       ztokenClose(void);                                       /* 0x004A11E0 */
int       ztokenNext(char *buf, int size);                         /* 0x004A1220 */
int       ztokenDefine(const char *name, const char *value);       /* 0x004A20C0 */
int       ztokenLookup(char *name);                                /* 0x004A22A0 */
int       ztokenReadInt(int *value);                               /* 0x004A2330 */
int       ztokenReadFloat(float *value);                           /* 0x004A23F0 */
int       ztokenReadString(char *buf, int size);                   /* 0x004A2440 */
int       ztokenExpect(char *token);                               /* 0x004A2460 */
int       ztokenGetLineNumber(void);                               /* 0x004A24E0 */
void      ztokenPrintLocation(void);                               /* 0x004A24F0 */
char     *ztokenGetLineRest(void);                                 /* 0x004A2550 */
void      ztokenSkipLine(void);                                    /* 0x004A2570 */
void      ztokenSetVerbose(int on);                                /* 0x004A2590 */
void      ztokenSetCapitalize(int on);                             /* 0x004A25A0 */
void      ztokenSetHashComments(int on);                           /* 0x004A25F0 */
void      ztokenSetPreprocess(int on);                             /* 0x004A2600 */
int       ztokenGetPreprocess(void);                               /* 0x004A2610 */
extern int g_ztokenQuoted;                                         /* 0x006763CC bit 0: last token was "quoted" */

/* zFont.c */
void      zfontGlyphConstruct(ZfontGlyph *g);                      /* 0x004A2620 ZfontGlyph::ZfontGlyph */
void      zfontGlyphDestruct(ZfontGlyph *g);                       /* 0x004A2640 ZfontGlyph::~ZfontGlyph */
void      zfontConstruct(Zfont *font);                             /* 0x004A26A0 Zfont::Zfont */
void      zfontDestruct(Zfont *font);                              /* 0x004A26B0 Zfont::~Zfont */
int       zfontChunkZFNT(void *iff);                               /* 0x004A2760 */
int       zfontInit(void);                                         /* 0x004A27A0 */
int       zfontShutdown(void);                                     /* 0x004A2800 */
void      zfontDrawTest(void);                                     /* 0x004A2820 */
int       zfontLoadFile(const char *path);                         /* 0x004A2950 */
int       zfontLoad(const void *data, uint32_t size);              /* 0x004A2990 */
int       zfontLoadBody(void);                                     /* 0x004A29D4 (tail of zfontLoad) */

/* zutil.c (DM-044's zutil.c has the others) */
float    *zutilBBoxInit(float *box, const float *point);           /* 0x004A2C70 */
float    *zutilBBoxAddPoint(float *box, const float *point);       /* 0x004A2CA0 */
float    *zutilBBoxUnion(float *out, const float *a, const float *b); /* 0x004A2D20 */
void      zutilBBoxHalfSize(float *out, const float *box);         /* 0x004A2DD0 */
float    *zutilBBoxTransform(float *out, const float *in, const RwMatrix *m); /* 0x004A2E10 */

/* zrwRamDisk.c */
int       zrwRamDiskExists(const char *name);                      /* 0x004A3200 */
void     *zrwRamDiskOpen(const char *name, const char *access);    /* 0x004A3260 */
int       zrwRamDiskClose(void *fp);                               /* 0x004A33C0 */
unsigned  zrwRamDiskRead(void *addr, unsigned size, unsigned count, void *fp); /* 0x004A33F0 */
unsigned  zrwRamDiskWrite(const void *addr, unsigned size, unsigned count, void *fp); /* 0x004A3420 */
char     *zrwRamDiskGets(char *buf, int maxLen, void *fp);         /* 0x004A3450 */
int       zrwRamDiskFlush(void *fp);                               /* 0x004A3470 */
int       zrwRamDiskPuts(const char *buf, void *fp);               /* 0x004A3480 */
int       zrwRamDiskEof(void *fp);                                 /* 0x004A3490 */
int       zrwRamDiskSeek(void *fp, long offset, int origin);       /* 0x004A34E0 */
void      zrwRamDiskBegin(void);                                   /* 0x004A3500 */
void      zrwRamDiskAddFile(void *address, uint32_t size, const char *extension); /* 0x004A3580 */
void      zrwRamDiskEnd(void);                                     /* 0x004A35C0 */

/* zprofile.c */
int       zprofileDraw(void *arg);                                 /* 0x004A35F0 */
int       zprofileInit(void);                                      /* 0x004A3B60 */
int       zprofileShutdown(void);                                  /* 0x004A3CB0 */
void      zprofileSetLayout(const ZprofileLayout *layout);         /* 0x004A3D30 */
ZprofileBar *zprofileCreateBar(int unused, int type, const uint32_t *colour, const char *name); /* 0x004A3DD0 */
ZprofileBar *zprofileCreateAppBar(void);                           /* 0x004A3EA0 */
ZprofileBar *zprofileCreateEngineBar(void);                        /* 0x004A3FA0 */
void      zprofileFreeBar(void *bar);                              /* 0x004A4080 */
void      zprofileEnable(int on);                                  /* 0x004A40A0 */
int       zprofileBarStart(ZprofileBar *bar);                      /* 0x004A40F0 */
int       zprofileBarStop(ZprofileBar *bar);                       /* 0x004A4110 */
void      zprofileSetPeakPeriod(int frames);                       /* 0x004A4150 */
void      zprofileBarSetScale(ZprofileBar *bar, float frames);     /* 0x004A41E0 */
void      zprofileSetPosition(int x, int y);                       /* 0x004A41F0 */

/* ztimer.c */
int       ztimerInit(int maxEvents);                               /* 0x004A4230 */
int       ztimerUpdate(void *arg);                                 /* 0x004A4280 */
int       ztimerShutdown(void);                                    /* 0x004A4340 */
int       ztimerAddCallback(uint32_t ms, ZtimerFn fn, void *arg1, void *arg2); /* 0x004A43E0 */
void      ztimerAlloc(void);                                       /* 0x004A4440 */
int       ztimerAddEvent(uint32_t ms, ZtimerFn objId, void *arg1, void *arg2); /* 0x004A44B0 */
ZtimerEvent *ztimerCreateCallback(uint32_t ms, ZtimerFn fn, void *arg1, void *arg2); /* 0x004A4510 */
ZtimerEvent *ztimerCreateCountdown(uint32_t ms);                   /* 0x004A4560 */
int       ztimerPoll(ZtimerEvent *ev);                             /* 0x004A45B0 */

/* pcProfile.c */
int       pcProfileInit(void);                                     /* 0x004A45E0 */
int       pcProfileShutdown(void);                                 /* 0x004A4640 */
float     pcProfileTicksToUnits(uint32_t ticks, float unitsPerSecond); /* 0x004A4660 */
void      pcProfileStart(uint32_t *start);                         /* 0x004A4690 */
uint32_t  pcProfileElapsed(const uint32_t *start);                 /* 0x004A46A0 */

/* zheap.c */
void      zheapInit(Zheap *h, int maxHeap, int capacity);          /* 0x004A46C0 */
void      zheapClear(Zheap *h);                                    /* 0x004A4710 */
int       zheapInsert(uint32_t key, void *value, Zheap *h);        /* 0x004A4720 */
ZheapItem *zheapPop(Zheap *h);                                     /* 0x004A47D0 */
ZheapItem *zheapPeek(Zheap *h);                                    /* 0x004A48C0 */
int       zheapIsEmpty(const Zheap *h);                            /* 0x004A48E0 */
int       zheapIsFull(const Zheap *h);                             /* 0x004A48F0 */
void      zheapFree(Zheap *h);                                     /* 0x004A4910 */
int       zheapRemove(void *value, Zheap *h);                      /* 0x004A4920 */
/* GHIDRA-SKIP-END */

#endif /* ZTOOLS_H */

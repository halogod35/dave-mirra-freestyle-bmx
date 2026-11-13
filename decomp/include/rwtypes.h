/*
 * rwtypes.h - the few RenderWare 3.1 public types that game/engine prototypes use (DM-040).
 *
 * Layouts follow the RenderWare 3.x SDK headers (rwcore.h) [I: not verified against this exe
 * beyond the field use in zrwTxr.c]. RenderWare itself is library code and is not decompiled.
 * Also the short integer names other cards use in decomp/symbols/symbols.csv (u8, u16, u32, s8, s16, s32).
 */
#ifndef RWTYPES_H
#define RWTYPES_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;
typedef short          s16;
typedef int            s32;

typedef struct RwRGBA {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    unsigned char alpha;
} RwRGBA;                 /* 4 */

typedef struct RwImage {
    int            flags;
    int            width;
    int            height;
    int            depth;
    int            stride;
    unsigned char *cpPixels;
    RwRGBA        *palette;
} RwImage;                /* 0x1C */

/* DM-043: RwV3d / RwMatrix (rwcore.h layout). [V] zscnptsLoad 0x00491F50 fills the rows at
 * +0x00/+0x10/+0x20 and the position at +0x30 of a 0x40-byte matrix inside its scene point. */
typedef struct RwV3d {
    float x;
    float y;
    float z;
} RwV3d;                  /* 0x0C */

typedef struct RwMatrix {
    RwV3d        right;   /* +0x00 */
    unsigned int flags;   /* +0x0C */
    RwV3d        up;      /* +0x10 */
    unsigned int pad1;    /* +0x1C */
    RwV3d        at;      /* +0x20 */
    unsigned int pad2;    /* +0x2C */
    RwV3d        pos;     /* +0x30 */
    unsigned int pad3;    /* +0x3C */
} RwMatrix;               /* 0x40 */

/* RwOsGetFileInterface() table (RenderWare 3.x RwFileFunctions). [V] zrwFileInit 0x0049EA10
 * copies these 10 pointers to 0x006752C0; zrwFile.c uses exist/open/close/read/write/seek. */
typedef struct RwFileFunctions {
    int   (__cdecl *rwfexist)(const char *name);
    void *(__cdecl *rwfopen)(const char *name, const char *access);
    int   (__cdecl *rwfclose)(void *fp);
    unsigned int (__cdecl *rwfread)(void *addr, unsigned int size, unsigned int count, void *fp);
    unsigned int (__cdecl *rwfwrite)(const void *addr, unsigned int size, unsigned int count, void *fp);
    char *(__cdecl *rwfgets)(char *buffer, int maxLen, void *fp);
    int   (__cdecl *rwfputs)(const char *buffer, void *fp);
    int   (__cdecl *rwfeof)(void *fp);
    int   (__cdecl *rwfseek)(void *fp, long offset, int origin);
    int   (__cdecl *rwfflush)(void *fp);
} RwFileFunctions;        /* 0x28 */

/* GHIDRA-SKIP-BEGIN */
RwFileFunctions *RwOsGetFileInterface(void);   /* 0x004E8280 (RenderWare) */
/* GHIDRA-SKIP-END */

#endif /* RWTYPES_H */

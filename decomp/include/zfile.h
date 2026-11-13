/*
 * zfile.h - NgEngine file system: devices, handles, callbacks (NgEngine/Zge/zfile.c).
 * DM-040 (first layout), DM-041 (slot names, callbacks, API).
 *
 * The file system is a thin layer over "devices". A path may start with a 4-character device
 * name and a colon ("rw00:assets\x.zal", "rd00:<addr hex>:<size hex>"); without a prefix the
 * default device is used. Two devices exist on PC [V] (zfileGetPlatformDevices 0x0048A940):
 *   zrwFileDevice 0x00562FF8 "rw00"  stdio through RenderWare's file interface (zrwFile.c)
 *   zfileDevice2  0x00563240 "rd00"  RAM disk: a memory block read/written as a file (zramdisk.c)
 *
 * Every device operation can report through an optional callback (an asynchronous design from
 * the PSX memory-card code): cb->fn is called with { cb->user, subject, ok, value } before the
 * operation returns. All callers on PC pass NULL.
 *
 * Slot meanings: [V] = shown by the zfile.c wrapper / both implementations; [I] = name inferred
 * from the argument shape and the two implementations (the PC devices make them trivial).
 */
#ifndef ZFILE_H
#define ZFILE_H
#include "dmtypes.h"

typedef struct zfileDevice zfileDevice;

typedef struct zfileHandle {
    void        *devFile;   /* +0x00 device-private file (FILE * / zramdiskFile *) */
    zfileDevice *device;    /* +0x04 */
} zfileHandle;              /* 0x08, from a pool of 8 (zfileInit) */

/* What a device passes to a callback. */
typedef struct zfileCallbackInfo {
    void *user;             /* +0x00 zfileCallback.user */
    void *subject;          /* +0x04 the device or handle the operation was on */
    int   ok;               /* +0x08 1 = success (read/write: all bytes transferred) */
    int   value;            /* +0x0C result: bytes transferred, new position, size, flag */
} zfileCallbackInfo;        /* 0x10 */

typedef struct zfileCallback {
    void (__cdecl *fn)(zfileCallbackInfo *info);
    void *user;
} zfileCallback;            /* 0x08 */

/* open modes (index into the RW mode table "rb","rb","wb","r+b" 0x00563038) */
#define ZFILE_MODE_READ   1
#define ZFILE_MODE_WRITE  2
#define ZFILE_MODE_RW     3
/* seek origins (same values as SEEK_SET/SEEK_CUR/SEEK_END, table 0x0054E130) */
#define ZFILE_SEEK_SET    0
#define ZFILE_SEEK_CUR    1
#define ZFILE_SEEK_END    2

struct zfileDevice {
    /* +0x00 [V] the 4-character device name ("rw00", "rd00") */
    const char *(__cdecl *GetName)(zfileDevice *dev);
    /* +0x04 [V] build a device file name from attributes: rw00 {"PATH"} -> path,
     *            rd00 {"ADDRESS","SIZE"} -> "rd00:%10x:%10x"; returns a static buffer or NULL */
    char *(__cdecl *MakeFileName)(zfileDevice *dev, int count, const char **names, void **values);
    /* +0x08 [I] is the device present; *result = 1 on both PC devices */
    int  (__cdecl *CheckDevice)(zfileDevice *dev, int *result, zfileCallback *cb);
    /* +0x0C [I] format the medium; "not permitted"/"not supported" on PC, returns 0 */
    int  (__cdecl *Format)(zfileDevice *dev, zfileCallback *cb);
    /* +0x10 [I] is the medium formatted; *result = 1 on both PC devices */
    int  (__cdecl *CheckFormatted)(zfileDevice *dev, int *result, zfileCallback *cb);
    /* +0x14 [I] free-space style query; rw00: not implemented, rd00: *result = 1 */
    int  (__cdecl *GetFreeSpace)(zfileDevice *dev, int arg, int *result, zfileCallback *cb);
    /* +0x18 [V] does the file exist (rw00 -> rwfexist; rd00: always 1) */
    int  (__cdecl *Exists)(zfileDevice *dev, const char *path, int *exists, zfileCallback *cb);
    /* +0x1C [I] delete a file; not implemented on either PC device */
    int  (__cdecl *Delete)(zfileDevice *dev, const char *path, zfileCallback *cb);
    /* +0x20 [V] open; h->devFile receives the device file; mode ZFILE_MODE_* */
    int  (__cdecl *Open)(zfileHandle *h, const char *path, int mode, zfileCallback *cb);
    /* +0x24 [V] */
    int  (__cdecl *Close)(zfileHandle *h, zfileCallback *cb);
    /* +0x28 [V] returns bytes read */
    int  (__cdecl *Read)(zfileHandle *h, void *buf, int n, zfileCallback *cb);
    /* +0x2C [V] returns bytes written */
    int  (__cdecl *Write)(zfileHandle *h, const void *buf, int n, zfileCallback *cb);
    /* +0x30 [V] origin ZFILE_SEEK_* */
    int  (__cdecl *Seek)(zfileHandle *h, int offset, int origin, zfileCallback *cb);
    /* +0x34 [V] rd00 returns the position; rw00: not implemented */
    int  (__cdecl *Tell)(zfileHandle *h, int *pos, zfileCallback *cb);
    /* +0x38 [V] */
    int  (__cdecl *GetSize)(zfileHandle *h, int *size, zfileCallback *cb);
    /* +0x3C NULL in both instances */
    void *reserved3C;
};                          /* 0x40 */

/* Registered devices (filled by zfileGetPlatformDevices). */
typedef struct zfileDeviceList {
    uint32_t     count;        /* +0x00 2 on PC */
    zfileDevice *devices[16];  /* +0x04 [I] capacity: the next global follows at +0x44 */
} zfileDeviceList;             /* 0x44 */

/* RAM-disk file (zramdisk.c), from a pool of 16; h->devFile points to it. */
#define ZRAMDISK_WRITE 0x1   /* opened with ZFILE_MODE_WRITE bit */
#define ZRAMDISK_READ  0x2   /* opened with ZFILE_MODE_READ bit */
typedef struct zramdiskFile {
    uint32_t pos;     /* +0x00 */
    uint32_t size;    /* +0x04 */
    uint32_t flags;   /* +0x08 ZRAMDISK_* */
    uint8_t *base;    /* +0x0C the memory block; NULL = closed */
} zramdiskFile;       /* 0x10 */

/* GHIDRA-SKIP-BEGIN */
typedef char zramdisk_size_check[(sizeof(zramdiskFile) == 0x10) ? 1 : -1];
typedef char zfile_h_size_check[(sizeof(zfileDevice) == 0x40 && sizeof(zfileHandle) == 8 &&
    sizeof(zfileCallbackInfo) == 0x10 && sizeof(zfileCallback) == 8 && sizeof(zfileDeviceList) == 0x44) ? 1 : -1];

/* globals (zfile.c) */
extern char             g_zfileErrorBuf[0x40];      /* 0x00670528 */
extern zfileDevice     *g_zfileDefaultDevice;       /* 0x00670568 */
extern char             g_zfileLastError[0x40];     /* 0x0067056C */
extern char             g_zfilePathBuf[0x80];       /* 0x006705AC */
extern zfileDeviceList  g_zfileDevices;             /* 0x00670630 */
extern int              g_zfileInitialized;         /* 0x00670674 */
extern void            *g_zfileHandlePool;          /* 0x00670678 */
extern void            *g_zfileHeap;                /* 0x0067067C load heap, 0 = general zmem */
extern zfileDevice     *g_zfileRamDiskDevice;       /* 0x0056062C, -1 until found */
extern zfileDevice      zrwFileDevice;              /* 0x00562FF8 */
extern zfileDevice      zfileDevice2;               /* 0x00563240 (RAM disk) */

/* zfile.c API */
void         zfileRegisterErrorMessage(const char *msg);
int          zfileInit(void);
zfileDeviceList *zfileGetDeviceList(void);
const char  *zfileGetDeviceName(zfileDevice *dev);
char        *zfileMakeFileName(zfileDevice *dev, int count, const char **names, void **values);
zfileDevice *zfileFindDevice(const char *path);
zfileDevice *zfileGetDefaultDevice(void);
int          zfileSetDefaultDevice(zfileDevice *dev);
char        *zfileGetLastError(void);
int          zfileCheckDevice(zfileDevice *dev, int *result, zfileCallback *cb);
int          zfileFormatDevice(zfileDevice *dev, zfileCallback *cb);
int          zfileCheckFormatted(zfileDevice *dev, int *result, zfileCallback *cb);
int          zfileGetFreeSpace(zfileDevice *dev, int arg, int *result, zfileCallback *cb);
int          zfileExists(const char *path, int *exists, zfileCallback *cb);
int          zfileDelete(const char *path, zfileCallback *cb);
zfileHandle *zfileOpen(const char *path, int mode, zfileCallback *cb);
int          zfileClose(zfileHandle *h, zfileCallback *cb);
int          zfileRead(zfileHandle *h, void *buf, int n, zfileCallback *cb);
int          zfileWrite(zfileHandle *h, const void *buf, int n, zfileCallback *cb);
int          zfileSeek(zfileHandle *h, int offset, int origin, zfileCallback *cb);
int          zfileTell(zfileHandle *h, int *pos, zfileCallback *cb);
int          zfileGetSize(zfileHandle *h, int *size, zfileCallback *cb);
void        *zfileLoadAll(const char *path, int *outSize);
void        *zfileLoad(const char *path, int size);
void         zfileFreeLoaded(void *buf);
char        *zfileGets(zfileHandle *h, char *buf, int size);
char        *zfileBuildPath(const char *dir, const char *name);
char        *zfileGetDirectory(const char *path, char *out);
char        *zfileGetExtension(const char *path, char *out);
zfileHandle *zfileOpenMemory(void *address, int size);
int          zfileGetPlatformDevices(zfileDeviceList *list);   /* 0x0048A940, zrwZgl.c */
void         zfileReleasePlatformDevices(void);                /* 0x0048A980, zrwZgl.c */
int          zfileHandlePoolClose(zfileHandle *h);
int          zfileShutdown(void);
/* device modules */
int          zrwFileInit(void);
int          zrwFileShutdown(void);
int          zramdiskInit(void);
/* GHIDRA-SKIP-END */

#endif /* ZFILE_H */

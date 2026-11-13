/*
 * dmtypes.h - base types for the NgBMX.exe reconstruction (DM-040).
 *
 * The decomp is non-matching C (AD-8). Stubs keep Ghidra's signatures, so this header also maps
 * Ghidra's pseudo-types (undefined4, uint, ...) and the few Win32 types used in prototypes.
 * Target: 32-bit x86, MSVC 6 data model (int/long/pointer = 4 bytes).
 *
 * Not imported into Ghidra (the program already has these types); see ghidra_types.txt.
 */
#ifndef DMTYPES_H
#define DMTYPES_H

#if !defined(_MSC_VER)
#define __int64 long long
#endif

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef __int64            int64_t;
typedef unsigned __int64   uint64_t;

/* Ghidra pseudo-types used in the stub signatures */
typedef unsigned char      undefined;
typedef unsigned char      undefined1;
typedef unsigned short     undefined2;
typedef unsigned int       undefined3;   /* 3-byte value, widened */
typedef unsigned int       undefined4;
typedef unsigned __int64   undefined8;
typedef unsigned char      byte;
typedef unsigned short     ushort;
typedef unsigned int       uint;
typedef unsigned long      ulong;
typedef long double        float10;      /* x87 80-bit */
typedef void               code;         /* used as code * (function pointer) */
#ifndef __cplusplus
typedef int                bool;
#define true  1
#define false 0
#endif

/* Win32 types that appear in prototypes (avoid windows.h). The sound files include <windows.h>
 * first; then its own definitions win (DM-064: _WINDEF_ is windef.h's guard, MSVC and MinGW). */
#if !defined(_WINDEF_) && !defined(_WINDEF_H)
typedef void *             HWND;
typedef void *             HINSTANCE;
typedef unsigned int       UINT;
typedef unsigned int       WPARAM;
typedef long               LPARAM;
typedef long               LRESULT;
typedef const char *       LPCSTR;
typedef void *             HDC;
typedef unsigned long      DWORD_PTR;
#endif
typedef unsigned int       size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
typedef struct _iobuf      FILE;    /* CRT FILE, opaque */
long __cdecl ftell(FILE *fp);       /* CRT 0x004A6239 */
int  __cdecl fseek(FILE *fp, long offset, int origin);  /* CRT 0x004A61AD */

#if !defined(_MSC_VER)
#define __cdecl
#define __stdcall
#define __fastcall
#define __thiscall
#endif

#endif /* DMTYPES_H */

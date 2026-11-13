# Statically linked libraries in NgBMX.exe

DM-002. Exe = the 2000-11-22 build in the install (image base 0x00400000). VAs are `0x00xxxxxx`.
[V] = verified with Ghidra 12.2 / pefile on the exe; [I] = inferred.
Library code is **identified and tagged, not decompiled** (AD-8). In Ghidra every library function
has a function tag `LIB_<name>` and `decomp/symbols/functions.csv` has the name in its `library` column. The
ranges below are the `LIB_RANGES` table in `tools/ghidra/build_symbols.py`, which does the tagging.

## Layout of `.text` (0x00401000-0x0054ac1b) and `_rwcseg`

The exe was linked with **`/INCREMENTAL`** [V]: `.text` starts with an incremental-link thunk table
(ILT, 633 five-byte `jmp` stubs), nearly every call goes through it, and `.text` ends in about 170 KB
of `0xCC` padding. `decomp/symbols/functions.csv` resolves callers/callees through the thunks (column `thunk_of`
marks the thunks themselves).

| VA range | What | Functions | Tag | Evidence |
|---|---|---:|---|---|
| 0x00401005-0x00402616 | ILT `jmp` thunks (linker) | 633 (+1 at 0x0040249b whose target 0x0046c360 is not a function start, so Ghidra did not mark it a thunk) | (thunk) | 5-byte `jmp rel32` stubs, `thunk_of` column [V] |
| 0x00402617-0x00403c2f | `0xCC` padding | — | — | [V] |
| 0x00403c30-0x0047ec3f | Game code `C:\zaxis\Bmx\Src\...` (first function: `action.c` 0x00403c30) | see module-map | — | path strings [V] |
| 0x0047ec40-0x004a4a7f | Engine `C:\NgEngine\Zge\...` (+ RW skeleton `WinMain`) | see module-map | — | path strings [V] |
| 0x004a4a20-0x004af9cf | **MSVC C runtime** (static LIBC), part 1; starts with fpinit.c (`_fpmath` 0x004a4a20, `_cfltcvt_init` 0x004a4a38, `_set_new_mode` 0x004a4a70; DM-061, was 0x004a4a80) | 293 (both parts) | `LIB_msvcrt` | `entry` = `WinMainCRTStartup` 0x004a51f9; FID hits `__ftol`, `_malloc`, `_strncpy`, `__global_unwind2`, `__allmul`...; "Microsoft Visual C++ Runtime Library", `R6009` messages, `__MSVCRT_HEAP_SELECT` [V] |
| 0x004af9d0-0x004afc5d | Import-library `jmp [IAT]` stubs: AVIFIL32/MSVFW32 (vfw32.lib), `RtlUnwind`, `DirectInputCreateEx` (dinput.lib) | 24 | (thunk) | thunk to external [V] |
| 0x004afc60-0x004b0f1f | **dinput.lib data** in `.text`: `c_dfDIKeyboard`/`c_dfDIJoystick`-style `DIOBJECTDATAFORMAT` tables (16-byte records: GUID ptr into `.rdata` 0x0054f3xx, offset, type, flags) | — | — | record scan [V], table names [I] |
| 0x004b0f20-0x00506e3f | **RenderWare 3.1** (core, world, D3D7/DDraw driver, image + toolkits, `rtpng` glue `rtpng_ImageRead` 0x005068a0) | 653 (all RW parts) | `LIB_renderware` | strings "RenderWare(tm) Application...", `Software\Criterion\RenderWare`, "Failed to create Direct Draw4", "Unable to enumerate D3D Texture formats", "Invalid combination type", "Invalid projection type specified"; error funcs 0x004ded80/0x004dedd0 have 146 callers in this range [V] |
| 0x00506e40-0x0051ce0f | **libpng 1.0.5** interleaved with **zlib 1.1.3** (both shipped inside RW's PNG toolkit, so their objects are mixed) | 106 + 22 | `LIB_libpng`, `LIB_zlib` | "libpng version 1.0.5 - October 15, 1999", "Incompatible libpng version in application and library", "inflate 1.1.3 Copyright 1995-1998 Mark Adler", zlib error strings [V] |
| 0x0051ce10-0x0051d2ef | RenderWare (late-linked object) | (in 653) | `LIB_renderware` | only caller 0x004fbf60 (RW), refs `.data1` [V] |
| 0x0051d2f0-0x0052095f | **MSVC C runtime**, part 2 (objects pulled in late: `_strcmpi`, `_strcmp`, `_setjmp3`/`_longjmp` for libpng, time/locale, `_mbsnbicoll`) | (in 289) | `LIB_msvcrt` | FID hits; `GetTimeZoneInformation`, `CompareStringA` imports [V] |
| 0x00520960-0x0054ac1b | RenderWare (late-linked object 0x00520960-0x0052130a), then `0xCC` padding | (in 653) | `LIB_renderware` | calls RW error funcs 0x004ded80/0x004dedd0 [V] |
| 0x0054b000-0x0054b158 | `_rwcseg` section: RenderWare CPU-specific code | 2 | `LIB_renderware` | section name, refs only `_rwdseg` [V] |

Data side [V]: game/engine string literals and globals live in `.data` 0x00551000-0x00563a5f in the
same object order as the code. CRT data follows (0x00563a60+). RenderWare, libpng and zlib keep their
strings in **`.data1`** (0x0069b000-0x0069f085) and RW also uses `_rwdseg` (0x006a0000).

## Per library

### MSVC C runtime (`LIB_msvcrt`, 289 functions)
- Static single-threaded LIBC of MSVC 6 [I: link layout, `__MSVCRT_HEAP_SELECT`, no `msvcrt.dll` import].
- Ghidra's Function ID named 33 of them (FID names kept). The rest keep `FUN_` names. They are tagged
  but not named one by one (out of scope; FID databases for VC6 would name more).
- `WinMainCRTStartup` 0x004a51f9 [V: PE entry point] calls `WinMain` 0x0048a460.
- The game prints through a CRT `printf`-like function pointer `g_pfnDebugPrintf` [0x00560074] [I].

### RenderWare 3.1 (`LIB_renderware`, 653 functions)
- Version: assets carry RW stream version stamp `0x00000310` (= 3.1.0) in their chunk headers [V,
  e.g. `assets\bike.zmd` MDFF → clump id 0x10, version 0x310]. The exe has no version string.
- Mostly in 0x004b0f20-0x00506e3f, plus late objects at 0x0051ce10 and 0x00520960 and `_rwcseg`.
- Includes the D3D7/DirectDraw driver: the only users of `DDRAW.DLL` (`DirectDrawCreate`,
  `DirectDrawEnumerateA`) and of `ADVAPI32` (registry, `Software\Criterion\RenderWare`) are RW functions [V].
- Function 0x004b0f20 had no xrefs and was not found by auto-analysis; DM-002 created it.
- The engine wraps RW in `NgEngine/Zge/Com/ZglRw/zrw*.c` (game side, not library).
- The `WinMain`/window code at 0x0048a460-0x0048a99f uses the RW "skeleton" class name `RwAppClass`,
  so it is probably the RW SDK skeleton (`win.c`) compiled into the app [I]. It is counted as engine
  code (unattributed), not as library.

### libpng 1.0.5 (`LIB_libpng`, 106 functions) and zlib 1.1.3 (`LIB_zlib`, 22 functions)
- They are only reached from RW's PNG reader (`rtpng_ImageRead` 0x005068a0 → `png_create_read_struct`
  0x005070d0 ...) [V]. The game does not call them directly. They are used for `TPNG` textures (DM-014).
- Split between the two: zlib = the closure of the zlib entry points (`inflateInit_`, `inflateReset`,
  `inflateEnd`, `inflate`, `crc32`) inside the shared range; the rest is libpng.
- Named [V by unique error strings]: `inflate` 0x005114b0, `inflateInit2_` 0x00511b90 ("1.1.3"),
  `inflate_blocks` 0x00518db0, `inflate_trees_bits` 0x0051c130, `inflate_trees_dynamic` 0x0051c200,
  `png_create_read_struct`, `png_read_info`, `png_read_row`, `png_read_end`, `png_read_data`,
  `png_set_read_fn`, `png_do_read_transformations`, `png_crc_finish`, every `png_handle_<chunk>`
  0x00511e90-0x00517560, `png_check_chunk_name`, `png_read_filter_row`, `png_read_finish_row`.
  [I by call structure]: `inflateInit_`, `inflateReset`, `inflateEnd`, `inflate_codes`, `inflate_fast`,
  `huft_build`, `inflate_flush`, `inflate_blocks_new/reset/free`, `inflate_codes_new`,
  `inflate_trees_fixed`, `png_error`, `png_warning`, `png_chunk_error`, `png_chunk_warning`, `png_malloc`,
  `png_set_IHDR`. DM-012 named `crc32` 0x00518b20, `get_crc_table` 0x00518b10, `png_reset_crc`,
  `png_calculate_crc` and the CRC table 0x005677b0.
- **Both "CRC error" strings in the exe are libpng's** (0x0069d3d0, used by `png_crc_finish` and the
  chunk handlers; 0x0069cfdf has no xrefs). No game code computes a CRC (see DM-012,
  `docs/re/integrity-checks.md`).

### DirectX and other Windows APIs (who calls them) [V]

| DLL | Imports | Called from |
|---|---|---|
| DDRAW | `DirectDrawCreate`, `DirectDrawEnumerateA` | RenderWare driver only |
| DINPUT | `DirectInputCreateEx` | `NgEngine/Zge/Pc/pcinput.c` (0x00493160) |
| DSOUND | **not in the import table**. Loaded at run time: `"DSOUND.DLL"` 0x0055eef0 + `"DirectSoundCreate"` 0x0055eed8 | `Bmx/Src/Pc/zge/sound/winChan.c` (0x0046d940) |
| WINMM | `waveOut*`, `time*`, `sndPlaySoundA` | `WinMain`/app timer (0x0048a460, 0x0048b570), `bmxsound.c`, `NgApp.c`, the movie player 0x0049fb30-0x004a0db0 |
| AVIFIL32, MSVFW32 | `AVIFile*`/`AVIStream*`, `DrawDib*` | movie player 0x0049fb30-0x004a0db0 (unattributed engine code between `zge.c` and `zFont.c`; plays `bin\movie\*.AVI`) |
| USER32 | window/message APIs | `WinMain`, window helpers 0x00489cc0-0x0048a1a0, `NgApp.c`, `pcinput.c` |
| ADVAPI32 | `RegOpenKeyExA`, ... | RenderWare only |

Correction to an earlier project note: DSOUND is not imported; it is loaded dynamically by `winChan.c`.

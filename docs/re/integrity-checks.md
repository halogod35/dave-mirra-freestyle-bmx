# Integrity checks in NgBMX.exe: inventory

Owner: DM-012 (save) and DM-030 (everything else), SK-SAVE. Exe = the 2000-11-22 PC build, SHA-256 `97aa9077…0d09a2`.
[V] = verified by disassembly/decompilation (capstone, plus a private Ghidra 12.2 project) and/or code run over the corpus; [I] = inferred.
Not verified in-game until play-tests DM-012-PT1 / DM-030-PT3 pass.

## Verdict

**[V] The PC build computes no CRC, checksum or hash over any game file.** The claim that "some files are CRC-checked" holds only for **PNG images**, through the statically linked **libpng 1.0.5**: each PNG chunk carries a standard CRC-32, and zlib checks the adler32 of the compressed image data. That is library behaviour that every PNG writer satisfies automatically. It can be broken only by hand-editing PNG bytes, and `dmbmx integrity fix-png` repairs it.

All other load-time checks are **magic / version / size** checks. The table lists every one found.

## Inventory

| # | File / data | Check | VA | On failure | Pass / re-sign | Tag |
|---|---|---|---|---|---|---|
| 1 | `save\SAVE.DAT` | read of 0x235C bytes succeeds | 0x004121EC `cmp eax,0x235C` / `jne` 0x004121F1 | load returns 9; the save is ignored | file ≥ 0x235C bytes (extra bytes ignored) | V |
| 2 | `save\SAVE.DAT` | `u32 @4 == 0x235C` | 0x004121F8 / `jne` 0x004121FD | returns 8, prints "memory card data is invalid (probably old version)", nothing copied; menu text 0x261 "LOAD FAILED." | `dmbmx save fix-crc` (or `set`/`unlock-all`, which always re-sign); patch `save-size-check` | V |
| 3 | `save\SAVE.DAT` | magic `1C 51 81 2A` | none (written, never read) | n/a | n/a | V |
| 4 | PNG in `TPNG` chunks (bikes `*bike*.zmd`, `sfx.zmd`, two `Mat._sd`) | libpng chunk CRC-32 (`png_crc_finish` inlined in each `png_handle_*`, "CRC error" 0x0069D3D0; `png_calculate_crc` 0x00508DE0 → zlib `crc32` 0x00518B20) | RenderWare PNG reader `RtPNGImageRead` 0x005068A0 creates the read struct at 0x005068DE with the default CRC policy (**no `png_set_crc_action` anywhere**) | **critical** chunk (IHDR/PLTE/IDAT/IEND): `png_error` → longjmp → reader returns NULL → `zrwTxr_CreateTextureFromMemory` 0x00497B00 asserts **"Failed to create texture"** (#8) → zrwTxr prints "Error loading texture" and continues **without the texture**. **Ancillary** chunk: warning, the chunk is dropped, the texture loads | write PNGs with any encoder, or `dmbmx integrity fix-png` (recomputes every chunk CRC in place) | V (code), I (in-game; PT3 part B) |
| 5 | same PNG IDAT data | zlib adler32 of the zlib stream ("incorrect data check" 0x0069D1D0, compare at 0x005119C6) | `inflate` 0x005114B0, reached **only** from libpng (callers 0x00507B42, 0x005171EF, 0x00518614; `inflateInit_` only from 0x005071E6, 0x00508BBE) | libpng "Decompression error" → same as #4 | any zlib compressor | V |
| 6 | ZAL archives | magic, read byte counts, decompressed size == table size ("ERROR - Size mismatch in decompressed data") | `zasset_ReadAssetData` 0x00480930 (assert at 0x00480B87) | assert (#8); the asset is used as decoded | write the table sizes correctly (DM-023 packer); see `docs/formats/ZAL.md` §2.4: **no checksums** | V |
| 7 | ZAL compression | method 1 = LZRW3-A (0x0049BD10), method 2 = LZO1X (0x0049C0AC) in `zcompress_Decompress` 0x0049C080 | n/a | "ZCOMPRES: Decompression failed" print | neither format carries a checksum | V |
| 8 | (all `zAssertFail` checks) | `zAssertFail` 0x0047ED10 → handler pointer 0x0056147C = 0x00489CF0 | asserts are **live in the release build** | MessageBox "RenderWare(tm) Application Error / \*\*\*ASSERT\*\*\* File/Line/Why … Do you wish to debug?" (Yes = `DebugBreak` → crash, No = continue, Cancel = `PostQuitMessage`) | patch `assert-continue` (always "No") | V |
| 9 | Level `.ZSD` SCFI entries (type u8 @+0, **size u32 @+4**, name @+8; 0x28 bytes) | component loaded with `zfileLoad(name, size)` 0x00485070: allocate `size`, read exactly `size` | zScene.c loader (assert string 0x00561A54), `zrwWorldFindScfi` 0x004962E0 | file **shorter** than size → read fails → assert "Scene component file not found" → level not loaded. **Longer** → silently truncated to size | `dmbmx integrity resign-zsd LEVEL.ZSD -o OUT` sets every size to the real file length. [V] all 72 corpus entries match their files | V |
| 10 | IFF containers (ziff) | root chunk ID equals the expected ID | `ziffOpen` 0x0049A790 | prints "ZIFF: Non matching chunkID", asserts "ZIFF: Bad IFF file" | keep the 4CCs | V |
| 11 | Format header sizes | `chunk size == sizeof(struct)`: zprop "Prop Def header size invalid" (0x38, 0x00486A9B), "Prop header size invalid" (0x48, 0x00486C4B), zscnpts "Point data size mismatch" (0x4C, 0x00491FAE), ztaginf "face structure mismatch" (0x50, 0x0048DCA5), zrwMat "MADF Mismatch" (0x48, 0x004893F5), zanim "DEFI size mismatch" (0x00499044) | as listed | assert (#8) | keep the struct sizes (they are format constants, not data-dependent) | V |
| 12 | Font | version word == 0x101 ("Bad font version") | 0x004A29AB | assert | keep the version | V |
| 13 | Audio banks (`bin\audio\*.zsd`) | 4-byte magic + 4-byte platform tag (`strncmp`) | `zsndRegisterAsset` 0x0046FD00 (winLoad.c) | assert "invalid audio asset file header/platform" | keep the tags | V |
| 14 | RenderWare streams (`BSP._sd`, DFF in MDFF) | RW chunk library-version handling | RenderWare 3.x internals | RW error | keep RW versions (library behaviour, not surveyed in detail) | I |
| 15 | CD check | `app_CheckGameCD` 0x0048AA10: registry `HKLM\Software\Acclaim Entertainment\Dave Mirra Freestyle BMX\1.00.000\SRCDISK` + `fopen(SRCDISK + "\bin\movie\acclaim.avi")`, loops on MessageBox "Please Insert Game CD" (Cancel → `exit(-1)`) | **dead code**: no call, jump, conditional jump or stored pointer reaches 0x0048AA10 anywhere in the image | never runs | nothing to do | V (static) |

## Evidence that nothing else exists

1. **CRC-32:** the only CRC-32 table (0x005677B0) is read only by zlib `crc32` 0x00518B20 (via `get_crc_table` 0x00518B10). Its only callers are `png_reset_crc` 0x00508DC0 and `png_calculate_crc` 0x00508E24. A scan of `.text` and `_rwcseg` for the table-driven idiom `xor reg, [reg*4 + table]` finds only that table (9 sites, all inside `crc32`). [V]
2. **adler32** 0x0051A1F0 (constants 0xFFF1 / 0x15B0): called only by `inflate` (0x005119C1), and its pointer is stored only by `inflateInit2_` (0x00511C41). `inflate` is called only by libpng. [V]
3. **Hand-rolled sums:** a capstone scan over `.text` looked for short loops (≤ 0x60 bytes) that accumulate memory with add/xor/adc/rol/ror/sub/imul and are followed within 12 instructions by a `cmp` of the accumulator against memory. It found 5 candidates, all false positives: ziff chunk navigation 0x0049AE15 and CRT code 0x004A4B55 / 0x004A79B4 / 0x004A7CEE. [V]
4. **Loaders:** I decompiled the 152 functions referencing the source paths of zasset, zfile, zprop, zobj, zrwModel, zobjModel, zrwMat, ztaginf, zScene, zscnpts, zrwWorld, zrwTxr, zanim, ziff, zrwModelCreate, zrwModelGeom, zrwFile, zramdisk, zrwRamDisk, zFont, NgIFzbsp, NgIFzres, winLoad, zrwShadow, zobjLight, zobjPEmit, zrwCol and zrwZgl. Every assert/print string in them is a structural, bounds, version or size message (listed above); none mentions or computes a checksum. File sizes (`zfileGetSize` 0x00484CD0) are used only to size buffers (callers 0x004808F9, 0x00480A8F, 0x0049A7C4, 0x004A34C1). [V]
5. **Strings:** "checksum", "crc" (apart from libpng's two), "hash mismatch", "signature" and "verify" do not occur. "Missing hash function" (0x0055CB18) is a hash-table registry message; asset IDs are name hashes for lookup (`docs/formats/ZAL.md`), not integrity hashes. [V]
6. **Registry / copy protection:** `RegQueryValueExA` is called only by the dead CD check (#15) and by Direct3D driver-config code ("RefRast", 0x004D3FC0, 0x004DB6xx). The `Reg*` jump stubs at 0x004AFC10-0x004AFC1C are unreferenced. [V]
7. **PE checksum:** `OptionalHeader.CheckSum` = 0 (not used by Windows for an exe), so a patched copy needs no PE checksum update. [V]

## Re-signing (AD-7 route 1): `dmbmx`

| Command | Fixes |
|---|---|
| `dmbmx save fix-crc IN OUT` (and every `save set` / `unlock-all`) | #1-#3: magic, size field, length |
| `dmbmx integrity fix-png IN (-o OUT \| --in-place)` | #4: recomputes every chunk CRC of every embedded PNG; length unchanged |
| `dmbmx integrity resign-zsd LEVEL.ZSD (-o OUT \| --in-place)` | #9: SCFI sizes = the real component sizes |
| `dmbmx integrity check PATH...` | reports #1-#2 (SAVE.DAT), #9 (.ZSD) and #4 (PNG CRCs, critical vs ancillary) for files or folders; exit code 1 on a failure. [V] whole install: 63 files checked, all OK |

## Patching (AD-7 route 2): `dmbmx patch`

`tools/dmbmx/patch/patches.json`. The patcher verifies the input's SHA-256 against the pristine exe and checks each patch's original bytes. It always writes a new file, and refuses the input path, `_pristine`, and the install's `NgBMX.exe`.

| id | VA | file offset | orig → new | effect | groups |
|---|---|---|---|---|---|
| `save-size-check` | 0x004121FD | 0x121FD | `75 42` → `90 90` | the save size field (#2) is no longer checked | `nocrc`, `all` |
| `assert-continue` | 0x0047ED10 | 0x7ED10 | `8B 44 24 10` → `C3 90 90 90` | every assert returns as if "No" was pressed: no dialog; the failing operation still fails | `all` |

`dmbmx patch nocrc NgBMX.exe NgBMX_nocrc.exe` applies `nocrc` (2 bytes differ, [V]). There is no CRC to remove, so that group contains only the save size check. `dmbmx patch apply IN OUT --group all | --id X`.

## Open

- In-game confirmation: DM-030-PT3 (`tools/testing/playtest/make_playtest_dm030.py`). Part A: the stock exe rejects a bad-size save and the `nocrc` exe accepts it. Part B: a bike PNG with a bad IDAT CRC gives the "Failed to create texture" assert and a missing texture.
- Question for the user (via the Director): which files did the Reddit post see fail, and how (message text or symptom)? A PNG hand-edited without fixing its CRC (#4), a `_sd` file that grew or shrank without its SCFI size updated (#9), or a ZAL entry whose sizes weren't updated (#6) all look like a "CRC check" to a modder.
- RenderWare stream version checks (#14) are not surveyed in detail.

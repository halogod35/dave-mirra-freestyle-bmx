# Module map of NgBMX.exe (original source file → code)

This is the base for the decomp plan. Exe = the 2000-11-22 build; VAs `0x00xxxxxx`.
[V] = verified on the exe (Ghidra 12.2 project + `decomp/symbols/functions.csv`), [I] = inferred.
Library code is in [libraries.md](libraries.md).

## How the map is made

1. **Path strings** [V]. The binary embeds 77 distinct original source paths (`__FILE__` in
   assert/error macros, e.g. `C:\zaxis\Bmx\Src\com\action\memcard.c`). Every function that
   references one belongs to that file: **312 functions, 72 files**.
2. **Link order** [V]. The exe is MSVC with `/INCREMENTAL`; each object file's code is contiguous
   and the objects appear in the same order in `.text` as their literals in `.data`. Checked: the
   first VAs of the 72 files (lowest [V] function each) are in exactly the same order as their path
   strings in `.data` (0x00551a64 `action.c` ... 0x00563a38 `pcProfile.c`), with no exception.
3. **Basic inference passes** [I], repeated until nothing changes (`tools/ghidra/build_symbols.py`):
   - *strings*: the function references a literal inside a file's `.data` literal window (the span
     of literals used by that file's [V] functions, clipped by the neighbouring files' path strings),
     and that file is the previous or next attributed file in VA order;
   - *between*: the function lies between two functions of the same file;
   - *callers*: all its callers are in one file and the function right next to it is in that file.
4. **Extended passes** [I] (same script):
   - *seed* (`STRING_SEEDS`): the function references a message that names its module or itself
     (`bmxphys...`, `bmxprop...`, `bmxreplay...`, `GUIMAIN:`, `ZPRIM:`, `ZCOMPRES:`, `Movie : ` in
     the player's literal slot, `RwMat:`, `RwCam:`, `zrwModelCreate...`, `frontend...`);
   - *range* (`RANGE_FILES`): whole VA ranges with clear evidence: the trig-table/vector block →
     `zmath.c`, the `zcompres_*` block → `zcompres.c`;
   - *orphan*: a literal inside the `.data` slot of a path string that no code references
     (`ztaginf.c`, `zutil.c`);
   - *split*: in each remaining gap between files A and B, every function votes A or B by its call
     edges; the split point that agrees with most votes assigns the voters. Functions with no vote
     stay in the gap.
   After the passes, every file is **one contiguous run** and the files are in the same order as
   the path strings in `.data` (checked [V]).
5. Everything else stays **unattributed** and is listed as a gap below. A gap is code of one of its
   two neighbours or of a file whose path string is never used (see "Files with no code xref").

Coverage: the basic passes placed 616 of 1366 game/engine functions; with the extended passes **1128 of 1399** (81%;
the 26 file-device functions created by symbols.csv rows and 8 more created earlier are included), in 78 files (72 with path xrefs, 2 orphan
path strings, 4 new files: `guimain.c`, `zmath.c`, `zcompres.c`, `pcmovie.c`).

Rules a reader can rely on: a [V] row is certain; an [I] row is right in the large majority of
cases but can be off at file boundaries. `split` and `range` are the weakest methods. Re-running
the pipeline regenerates the tables below.

## Conventions

- `source_file` in `decomp/symbols/symbols.csv` = the original path without drive and without the leading
  `zaxis\`: `Bmx/Src/com/action/memcard.c`, `NgEngine/Zge/zfile.c` (the convention the ZAL and save readers
  already used). The binary mixes `Bmx\Src` and `Bmx\src`; we write `Src` for both.
  Library code: `lib:msvcrt`, `lib:renderware`, `lib:libpng`, `lib:zlib`.
- For the decomp tree (AD-8): `Bmx/...` → `decomp/src/zaxis/Bmx/...`, `NgEngine/...` →
  `decomp/src/NgEngine/...`.
- Generated names: a function attributed by the pipeline and not named by anyone is called
  `<file>_<va>` (e.g. `game_0040a850`). Hand-written rows always win (the generator only regenerates its own
  `<file>_<va>` rows and the unnamed `lib:` rows, and never adds one where a hand-written row exists).
- New files (no path string) get names in the original style (`guimain.c` after its `GUIMAIN:`
  messages, `zcompres.c` after `ZCOMPRES:`, `zmath.c`, `pcmovie.c`); they are marked * below.
- Types: `struct` rows (`0x00000000,struct,<Type>,<header>,...`) list the recovered structs; the
  structs themselves live in `decomp/include/*.h` and are imported into Ghidra by
  `tools/ghidra/ImportTypes.java` (category `/dmbmx`).

## Top-level layout [V]

| VA range | Contents |
|---|---|
| 0x00401005-0x00402616 | `/INCREMENTAL` thunk table (634 `jmp` stubs), then `0xCC` padding |
| 0x00403c30-0x0047ebff | **Game**: `Bmx/Src/com/action/*` (action ... rider), `com/menu/*`, `com/zge/bmx/*`, `com/zge/NgIF/*` (NgEngine interface layer), `Pc/zge/sound/*`, `com/zge/*` (zactor, zprim, zsndseq, zsndsrc) |
| 0x0047ec40-0x004a4a7f | **Engine** `NgEngine/Zge/*` (zdebug, zasset, zfile, zmem, zobj*, ziff, zScene, ...) and the RenderWare glue `Com/ZglRw/zrw*.c`; `WinMain` (RW skeleton style) 0x0048a460 |
| 0x004a4a20- | libraries (from `_fpmath`): CRT, import stubs, RenderWare, libpng+zlib, CRT, RenderWare ([libraries.md](libraries.md)) |

## Named entry points

| VA | Name | Source file | Conf | Evidence |
|---|---|---|---|---|
| 0x004a51f9 | `WinMainCRTStartup` | lib:msvcrt | V | PE entry point; calls `WinMain` |
| 0x0048a460 | `WinMain` | (unattributed, RW skeleton) | V | called by the CRT entry with (hInstance, 0, cmdline, nShow); parses `-r` resolution switch, `RegisterClassA("RwAppClass")`, `CreateWindowExA`, then the **main loop**: `PeekMessageA` loop, frame limiter on `timeGetTime`, calls `App_Frame` each iteration |
| 0x0048a800 | `MainWndProc` | (unattributed) | I | `lpfnWndProc` of the window class (function created by hand) |
| 0x0048b1a0 | `App_Open` | `NgEngine/Zge/Com/ZglRw/zrwZgl.c` [I] | I | called by `WinMain` with the HWND; failure → `DestroyWindow` |
| 0x0048b450 | `App_Start` | `zrwZgl.c` [I] | I | called by `WinMain` with the command line before the loop |
| 0x0048b570 | `App_Frame` | `zrwZgl.c` [I] | I | called once per main-loop iteration; frame timing (`timeGetTime`, fps counter) |
| 0x0047ed10 | `zAssertFail` | `NgEngine/Zge/zdebug.c` [I] | I | `(msg, __FILE__, __LINE__, 0)`, 252 callers; calls handler pointer `g_pfnAssertHandler` [0x0056147c] |
| 0x00560074 | `g_pfnDebugPrintf` (data) | | I | printf-like pointer used for the save error messages |
| 0x00483a60 | `zfileRegisterErrorMessage` | `NgEngine/Zge/zfile.c` | V | own name in its format string |
| 0x00483aa0 | `zfileInit` | `zfile.c` | I | "File system should not be initialized", creates the file-handle allocator |
| 0x00484700 | `zfileOpen` | `zfile.c` | V | `(path, mode 1=read/2=write, 0)`; used by the save code |
| 0x00484a60 | `zfileClose` | `zfile.c` | V | |
| 0x00484b20 | `zfileRead` | `zfile.c` | V | `(h, buf, n, 0)` → device vtable +0x28 |
| 0x00484b90 | `zfileWrite` | `zfile.c` | V | |
| 0x00484c00, 0x00484c70, 0x00484cd0 | `zfileSeek`, `zfileTell`, `zfileGetSize` | `zfile.c` | V/I/V | device slots +0x30/+0x34/+0x38; zasset uses Seek(f, off, 0, 0) and GetSize(f, &size, 0) |
| 0x0049e1f0-0x0049e950, 0x0049eaa0-0x0049f3d0 | `zrwFile_*`, `zramdisk_*` (the skeleton, 26 functions created) | `zrwFile.c`, `zramdisk.c` | V/I | the two `zfileDevice` tables (0x00562ff8, 0x00563240); see `decomp/include/zfile.h` |
| 0x00485460 | `zfileBuildPath` | `zfile.c` | I | "Memory corruption when building path name" |
| 0x004804d0 ... 0x00480930 | `zasset_OpenLibrary`, `zasset_CloseLibrary`, `zasset_FindAsset` ("Failed to find asset Group=%d, AssetNum=%d"), `zasset_OpenRefFile`, `zasset_LoadAsset` | `NgEngine/Zge/zasset.c` | V/I | asset lookup by group/number; ZAL library open/close |
| 0x00485d80 / 0x00485e10 / 0x00485e40 | `zmemTempAlloc` / `zmemTempRelease` / `zmemTempMark` (first named by the save reader, renamed by the zmem pass) | `NgEngine/Zge/zmem.c` | I | stack-style temporary allocator ("Memory allocation failed - Out of ...") |
| 0x00412130 | `memcard_CheckSaveExists` | `Bmx/Src/com/action/memcard.c` | V | opens `.\save\SAVE.DAT`; 6 = exists, 5 = not |
| 0x00412170 | `memcard_Stub0` | `memcard.c` | I | `return 0` |
| 0x00412190 | `memcard_LoadSave` | `memcard.c` | V | reads 0x235C bytes, checks only `u32 @4 == 0x235C`, copies 3 blocks; **no CRC** |
| 0x00412340 | `memcard_WriteSave` | `memcard.c` | V | builds header `1C 51 81 2A`, `0x235C`, copies 3 blocks, writes 0x235C bytes |

The save details are in `docs/formats/SAVE.md` and `docs/re/integrity-checks.md`.

The skeleton also named, from messages that carry the original function name [V]: `bmxphysUpdatePositionAir`
0x0042dad0, `bmxpropInit` 0x0042deb0, `bmxpropGetPropByID` 0x0042ed10, `bmxreplayUpdate` 0x0042fd50,
`bmxreplayCreate` 0x00430180, `frontendInitApp` 0x0044d990, `zfxCreate` 0x0045fcb0,
`zrollerGetPlayerStartPos` 0x0046cf30, `zrwMatCreateFromImage` 0x004896d0, `zrwWorldHookRpMaterials`
0x00496ff0, `zrwModelSetRwInfo` 0x0049c880, `zrwModelCreateInstance` 0x0049ce90; and [I]
`zdebugPrint` 0x0047f940, `zmathInitTables`/`zmathSin`/`zmathCos` 0x00480d20/0x00480e40/0x00480eb0,
`zrwZglPlayMovie` 0x0048bed0, `zmemAlloc`/`zmemFree`/`zmemPoolCreate`/`zmemPoolAlloc`/`zmemPoolFree`.
The zmem pass renamed the zmem allocators after the exe's own messages: the fixed-size allocator at
0x00485e80-0x00486170 is a *free list* (`zmemFreeListCreate/Destroy/Alloc/Free`, "Free list pool has
been exhausted."), and `zmemPool*` now means the first-fit *memory pools* (`zmemPoolCreate` 0x00486410,
`zmemPoolAlloc` 0x004865b0, `zmemPoolFree` 0x00486820: "zmemPoolFree(): Memory has been previously
freed."). `zMemTemp*` became `zmemTemp*`. Full list: the zmem*/zList*/zstring* rows of `decomp/symbols/symbols.csv`.
The original style is `<module><Verb>` without underscore (`bmxprop`, `zroller`, `zrwModel`).
The game.c pass renamed the game-flow layer the same way: `game_*` -> `game<Verb>` (46 functions of game.c,
e.g. `gameApplyReward`, `gameAwardLevelProgress`, `gameSetState`), `comdata_GetGoalInfo` ->
`comdataGetGoalInfo`, and moved the save reader's `save_InitDefaults` 0x00407BA0 / `save_GetRecordPtr`
0x00407D20 from the camera gap into `comdata.c` as `comdataInitDefaults` / `comdataGetRecordPtr`
(row anchors: they sit right before `comdataGetGoalInfo` and touch only the saved globals). It also
named two callees in the gaps without attributing them: `cameraSetMode` 0x00407760 and
`fxCollectEffect` 0x00408A80 [I].
The camera pass (action / contrler / camera): `camera.c` is 0x004056C0-0x00407B85. The skeleton had put
`action_004056c0` / `action_00405780` / `action_00407470` in action.c ("split" / "callers"); they are
`cameraInit` / `cameraUpdate` / `cameraApply` and, with the five camera-gap functions and the new
`cameraDrawNightVision` 0x004074C0 (a draw callback Ghidra had not created; a `decomp/symbols/symbols.csv` row creates
it), only touch the camera globals 0x0056B408-0x0056B5BB; camera.c's assert at line 1183 also needs
cameraUpdate's ~1100 lines in front of it. The 27 functions of the three files are named `action*`,
`camera*`, `contrler*` (`decomp/symbols/symbols.csv` rows, which carry the source file);
`gap_camera__comdata.c` is gone. Details: `docs/re/input-camera.md`.
The overlay pass (overlays): `overlymp.c` is 0x004124C0-0x004147AD (it takes in the gap function 0x00414580,
`overlympDrawLetters`, called only from there), `overlay.c` 0x004147E0-0x0041FA0C (it ends right before
physics.c), `zoverii.c` 0x00457B00-0x0045A130. 90 functions in these ranges were not in the Ghidra
project (85 in overlay.c, 5 in overlymp.c): they are reached only through the HUD item table 0x00556F00,
the overlay screen records 0x00557098-0x005571B7 or two ILT thunks; `decomp/symbols/symbols.csv` rows
create them. The 71-function gap between bmxsound.c and zoverii.c (no strings, called through tables)
is not overlay code: nothing in it is called from the overlay files. Details: `docs/re/hud.md`.
The front-end pass (front end): `guiinput.c` 0x00436C20-0x0043744E, `guimain.c` 0x00437460-0x004399E6 (it takes in the old
gap function 0x004375A0 and the line element before it), the new `guisfile.c`* (0x00439A00, the screen-file loader:
its "GUISFILE:" literals lie between guimain.c's and guitoggl.c's), `guitoggl.c` 0x00439F80-0x0043BE61 (the
generic element handler 0x00439F80 joins it by the split pass; 0x00439FA1 is a bogus function inside it),
`frontend.c` 0x0043BE90-0x0044DD23. 95 functions in these ranges were not in the Ghidra project: they are
reached only through /INCREMENTAL thunks (screen handlers, per-frame tasks, element handlers);
`decomp/symbols/symbols.csv` rows create them. The gap before guiinput.c (0x00436520-0x00436C05: the button
element, text markup, the help-system stubs) stays a gap file. Details: `docs/re/frontend.md`.

## Files with no code xref [V]

Five path strings are never referenced by an instruction (their asserts may be compiled out or
referenced through data): `NgEngine/Zge/ZgeMods/zprop.c` 0x00560cb0,
`NgEngine/Zge/ZgeMods/ztaginf.c` 0x005617dc, `NgEngine/Zge/ZgeMods/zscnpts.c` 0x00561d64,
`NgEngine/Zge/zutil.c` 0x005637e4, `NgEngine/Zge/Com/ZglRw/zrwRamDisk.c` 0x005638b8. Their code
is in the gaps next to their link-order neighbours (by `.data` order: zprop.c after zmem.c, ztaginf.c
after zrwPrim.c, zscnpts.c after zScene.c, zutil.c after zFont.c, zrwRamDisk.c after zutil.c) [I].
The skeleton found code for two of them through their literals (`ztaginf.c` 8 functions, `zutil.c` 1).
`zprop.c`, `zscnpts.c` and `zrwRamDisk.c` still have none.

## The large gaps: what the extended passes did [I]

| Gap after the basic passes | Funcs | Result of the extended passes |
|---|---:|---|
| 0x004375a0-0x0043a66a (after `guiinput`) | 65 | new `guimain.c` (62; "GUIMAIN:" and "GUISFILE:" messages, `intart.zal`); 3 left |
| 0x0043f730-0x0044da86 (before `frontend`) | 48 | all 49 to `frontend.c` (call-graph split + "FRONTEND:" messages; frontend.c now 73 functions) |
| 0x00431e10-0x004371ea (after `rider`) | 46 | `rider.c` 22 (trick/animation handlers calling rider + contrler), `guiinput.c` 15; 9 left |
| 0x00480d20-0x004820e5 (after `zasset`) | 46 | new `zmath.c` (trig lookup tables `sin`/`atan` + vector/matrix helpers) |
| 0x004200b0-0x0042de98 (after `physics`) | 31 | `physics.c` 30 (`bmxphysUpdatePositionAir`, respawn warnings), `prop.c` 1 |
| 0x00475300-0x00477ca1 (after `zactor`) | 29 | `zactor.c` 25, `zprim.c` 4 ("ZPRIM:" messages) |
| 0x0049fb30-0x004a2797 (after `zge`) | 26 | new `pcmovie.c` (13; "Movie : ..." messages, AVIFile/DrawDib), `zFont.c` 1; 12 left |
| 0x0048ccf0-0x0048f361 (after `zrwPrim`) | 18 | orphan `ztaginf.c` 8; 10 left |
| 0x00450060-0x00457a88 (after `bmxsound`) | 71 | `bmx/dyn.c` (new, [I] name), the crash rag doll + limb IK (74 functions with 4 created); 0x004578F0 = zoverii.c `zoveriiInit` |

## Rebuilding

```bash
# once: import + auto-analysis into the canonical project (<repo>/.ghidra/project, gitignored; ~2 min)
mkdir -p .ghidra/project
tools/ghidra/ghidra.sh headless "<repo>\.ghidra\project" NgBMX -import "F:\Projects\Dave Mirra freestyle BMX\NgBMX.exe"
# once, on a fresh project: the zrwWorldPrintInfo fix-up (idempotent; see below)
tools/ghidra/ghidra.sh headless "<repo>\.ghidra\project" NgBMX -process NgBMX.exe -noanalysis \
    -scriptPath "<repo>\tools\ghidra" -postScript FixupZrwWorldPrintInfo.java
tools/ghidra/apply_symbols.sh --no-apply                # export decomp/symbols/functions.csv
tools/testing/py.sh tools/ghidra/build_symbols.py       # module map -> symbols.csv + tables below
tools/ghidra/apply_symbols.sh                           # types (ImportTypes) + names/tags into Ghidra, re-export functions.csv
tools/ghidra/apply_symbols.sh --no-apply --no-export --decompile --decompile-libs   # C of every function -> out/ghidra_c/
tools/testing/py.sh tools/decomp/make_skeleton.py --signatures out/ghidra_c       # decomp/functions.csv, stubs, PROGRESS.md
```

Only the owner of the Ghidra project runs `apply_symbols.sh` (AD-6). Everyone else adds rows to
`decomp/symbols/symbols.csv` (header `address,kind,name,type,source_file,confidence`, `0x%08x` lowercase,
sorted by address; duplicate addresses allowed, the last row wins).

`FixupZrwWorldPrintInfo.java` re-disassembles `zrwWorldPrintInfo` 0x00495F60, which auto-analysis decodes from the
wrong offset, so that `ApplySymbols` can create it. `SetFtolSignature.java` gives the CRT's `__ftol`
0x004A4A80 its x87 signature so the decompiler shows float-to-int arguments (optional, for readable
Ghidra C; run it the same way with `-postScript SetFtolSignature.java`). Both scripts describe themselves
in their header comments.

## Tables (generated)

<!-- BEGIN GENERATED by tools/ghidra/build_symbols.py; do not edit by hand -->

### Totals [V]

| What | Functions |
|---|---:|
| All functions in Ghidra (incl. thunks) | 4202 |
| jmp thunks (ILT 0x00401005-0x00402616: 635; elsewhere: 29) | 664 |
| Game + engine code (below 0x004a4a20, non-thunk) | 2458 |
| ... attributed to a source file | 2429 (98%) |
| ... of which [V] direct path reference | 359 |
| ... unattributed | 29 |
| Library `lib:libpng` | 106 |
| Library `lib:msvcrt` | 293 |
| Library `lib:renderware` | 658 |
| Library `lib:zlib` | 23 |

### Source file -> VA ranges

Sorted by first VA (= link order). `V` = functions that reference the path string themselves;
`I` = inferred, by method: `s` string in the file's .data string window, `b` between two functions of the file,
`c` all callers in the file; `e` seed string naming the file/function, `r` manual range, `o` literal
in an orphan path string's window, `x` call-graph split of a gap, `w` a hand-written symbols.csv row names the file. Files marked * have no path string
in the exe; their name is invented in the original style.
Confidence: **high** = one contiguous run with >= 1 V function and at most 1/4 split/range; **medium** = has V, seed or orphan evidence; **low** = only placed by range/split/adjacency.

| # | Source file | VA range(s) | Funcs | V | I (s/b/c/e/r/o/x/w) | Bytes | Conf |
|---:|---|---|---:|---:|---|---:|---|
| 1 | `Bmx/Src/com/action/action.c` | 0x00403c30-0x004056aa | 8 | 3 | 0/0/0/0/0/0/0/5 | 5072 | high |
| 2 | `Bmx/Src/com/action/camera.c` | 0x004056c0-0x00407b85 | 13 | 1 | 0/0/0/0/0/0/0/12 | 7160 | high |
| 3 | `Bmx/Src/com/action/comdata.c` | 0x00407ba0-0x0040800d | 3 | 1 | 0/0/0/0/0/0/0/2 | 847 | high |
| 4 | `Bmx/Src/com/action/contrler.c` | 0x00408030-0x004088c5 | 11 | 5 | 0/0/0/0/0/0/0/6 | 1702 | high |
| 5 | `Bmx/Src/com/action/fx.c` | 0x00408960-0x00409980 | 21 | 2 | 0/0/0/0/0/0/0/19 | 3074 | high |
| 6 | `Bmx/Src/com/action/game.c` | 0x004099c0-0x00412011 | 47 | 10 | 0/0/0/0/0/0/0/37 | 26247 | high |
| 7 | `Bmx/Src/com/action/memcard.c` | 0x00412130-0x0041246e | 4 | 1 | 0/0/0/0/0/0/0/3 | 678 | high |
| 8 | `Bmx/Src/com/action/overlymp.c` | 0x004124c0-0x004147ac | 11 | 2 | 0/0/0/0/0/0/0/9 | 6914 | high |
| 9 | `Bmx/Src/com/action/overlay.c` | 0x004147e0-0x0041fa06 | 122 | 5 | 0/0/0/0/0/0/0/117 | 33470 | high |
| 10 | `Bmx/Src/com/action/physics.c` | 0x0041fa20-0x0042de22 | 50 | 3 | 0/0/0/0/0/0/0/47 | 45590 | high |
| 11 | `Bmx/Src/com/action/prop.c` | 0x0042de70-0x0042fca1 | 18 | 1 | 0/0/0/0/0/0/0/17 | 5854 | high |
| 12 | `Bmx/Src/com/action/replay.c` | 0x0042fce0-0x00430e50 | 13 | 7 | 0/0/0/0/0/0/0/6 | 3381 | high |
| 13 | `Bmx/Src/com/action/rider.c` | 0x00430e70-0x004364fc | 34 | 3 | 0/0/0/0/0/0/0/31 | 17120 | high |
| 14 | `Bmx/Src/com/menu/gui/guiinput.c` | 0x00436c20-0x0043744e | 20 | 2 | 0/0/0/0/0/0/0/18 | 1455 | high |
| 15 | `Bmx/Src/com/menu/gui/guimain.c`* | 0x00437460-0x004399e6 | 86 | 0 | 0/0/0/0/0/0/0/86 | 6543 | low |
| 16 | `Bmx/Src/com/menu/gui/guisfile.c`* | 0x00439a00 | 1 | 0 | 0/0/0/0/0/0/0/1 | 1116 | low |
| 17 | `Bmx/Src/com/menu/gui/guitoggl.c` | 0x00439f80-0x0043be61 | 14 | 10 | 0/0/0/0/0/0/0/4 | 4819 | high |
| 18 | `Bmx/Src/com/menu/frontend.c` | 0x0043be90-0x0044dd23 | 136 | 1 | 0/0/0/0/0/0/0/135 | 54822 | high |
| 19 | `Bmx/Src/com/zge/bmx/bmxsound.c` | 0x0044dd40-0x00450043 | 10 | 3 | 0/0/0/0/0/0/0/7 | 6940 | high |
| 20 | `Bmx/Src/com/zge/bmx/dyn.c`* | 0x00450060-0x00457896 | 74 | 0 | 0/0/0/0/0/0/0/74 | 23732 | low |
| 21 | `Bmx/Src/com/zge/bmx/zoverii.c` | 0x004578f0-0x0045a130 | 19 | 13 | 0/0/0/0/0/0/0/6 | 8083 | high |
| 22 | `Bmx/Src/com/zge/NgIF/MissingZGE.c` | 0x0045a1a0-0x0045a322 | 8 | 7 | 0/0/0/0/0/0/0/1 | 212 | high |
| 23 | `Bmx/Src/com/zge/NgIF/NgApp.c` | 0x0045a340-0x0045c93c | 28 | 2 | 0/0/0/0/0/0/0/26 | 7384 | high |
| 24 | `Bmx/Src/com/zge/NgIF/NgIFfx.c` | 0x0045c950-0x0045fdeb | 31 | 1 | 0/0/0/0/0/0/0/30 | 10407 | high |
| 25 | `Bmx/Src/com/zge/NgIF/NgIFpsx.c` | 0x0045fe00-0x004646d5 | 39 | 5 | 0/0/0/0/0/0/0/34 | 13991 | high |
| 26 | `Bmx/Src/com/zge/NgIF/NgIFzactor.c` | 0x004646f0-0x00465a23 | 12 | 4 | 0/0/0/0/0/0/0/8 | 3735 | high |
| 27 | `Bmx/Src/com/zge/NgIF/NgIFzbsp.c` | 0x00465a50-0x0046951f | 137 | 5 | 0/0/0/0/0/0/0/132 | 10639 | high |
| 28 | `Bmx/Src/com/zge/NgIF/NgIFzcollide.c` | 0x00469530-0x0046ac85 | 17 | 3 | 0/0/0/0/0/0/0/14 | 4564 | high |
| 29 | `Bmx/Src/com/zge/NgIF/NgIfzmemDMM.c` | 0x0046aca0-0x0046b792 | 11 | 8 | 0/0/0/0/0/0/0/3 | 2119 | high |
| 30 | `Bmx/Src/com/zge/NgIF/NgIFzres.c` | 0x0046b7c0-0x0046c206 | 19 | 6 | 0/0/0/0/0/0/0/13 | 1894 | high |
| 31 | `Bmx/Src/com/zge/NgIF/NgIFzroller.c` | 0x0046c220-0x0046d922 | 92 | 12 | 0/0/0/0/0/0/0/80 | 3608 | high |
| 32 | `Bmx/Src/Pc/zge/sound/winChan.c` | 0x0046d940-0x0046fa0a | 21 | 12 | 0/0/0/0/0/0/0/9 | 6689 | high |
| 33 | `Bmx/Src/Pc/zge/sound/winLoad.c` | 0x0046fb20-0x00470a47 | 17 | 11 | 0/0/0/0/0/0/0/6 | 2969 | high |
| 34 | `Bmx/Src/Pc/zge/sound/winStrm.c` | 0x00470aa0-0x004725d1 | 27 | 15 | 0/0/0/0/0/0/0/12 | 5368 | high |
| 35 | `Bmx/Src/com/zge/zactor.c` | 0x00472630-0x0047785c | 85 | 2 | 0/0/0/0/0/0/0/83 | 15865 | high |
| 36 | `Bmx/Src/com/zge/zprim.c` | 0x004779e0-0x0047c3c2 | 73 | 6 | 0/0/0/0/0/0/0/67 | 14085 | high |
| 37 | `Bmx/Src/com/zge/zsndseq.c` | 0x0047c4c0-0x0047de06 | 28 | 7 | 0/0/0/0/0/0/0/21 | 4684 | high |
| 38 | `Bmx/Src/com/zge/zsndsrc.c` | 0x0047de20-0x0047ebc2 | 26 | 9 | 0/0/0/0/0/0/0/17 | 2479 | high |
| 39 | `NgEngine/Zge/zdebug.c` | 0x0047ec40-0x0047edfd | 7 | 1 | 0/0/0/0/0/0/0/6 | 402 | high |
| 40 | `NgEngine/Zge/Com/ZglRw/zrwDbg.c` | 0x0047ee00-0x004802ec | 32 | 2 | 0/0/0/0/0/0/0/30 | 5161 | high |
| 41 | `NgEngine/Zge/zinput.c` | 0x004802f0-0x0048044d | 6 | 1 | 0/0/0/0/0/0/0/5 | 296 | high |
| 42 | `NgEngine/Zge/zasset.c` | 0x00480450-0x00480c36 | 11 | 4 | 0/0/0/0/0/0/0/7 | 1959 | high |
| 43 | `NgEngine/Zge/zmath.c`* | 0x00480c40-0x00482081 | 59 | 0 | 0/0/0/0/0/0/0/59 | 4742 | low |
| 44 | `NgEngine/Zge/ZgeMods/zobjCamera.c` | 0x00482090-0x00482405 | 11 | 2 | 0/0/0/0/0/0/0/9 | 800 | high |
| 45 | `NgEngine/Zge/zstring.c` | 0x00482420-0x00483014 | 39 | 3 | 0/0/0/0/0/0/0/36 | 2802 | high |
| 46 | `NgEngine/Zge/zquat.c` | 0x00483020-0x00483a5e | 17 | 4 | 0/0/0/0/0/0/0/13 | 2518 | high |
| 47 | `NgEngine/Zge/zfile.c` | 0x00483a60-0x004859a7 | 35 | 19 | 0/0/0/0/0/0/0/16 | 7747 | high |
| 48 | `NgEngine/Zge/zmem.c` | 0x004859b0-0x00486903 | 41 | 12 | 0/0/0/0/0/0/0/29 | 3555 | high |
| 49 | `NgEngine/Zge/ZgeMods/zprop.c` | 0x00486910-0x00486ed5 | 8 | 0 | 0/0/0/0/0/0/0/8 | 1429 | low |
| 50 | `NgEngine/Zge/zobj.c` | 0x00486ee0-0x00487acd | 33 | 3 | 0/0/0/0/0/0/0/30 | 2768 | high |
| 51 | `NgEngine/Zge/Com/ZglRw/zrwModel.c` | 0x00487ad0-0x00488d2b | 47 | 2 | 0/0/0/0/0/0/0/45 | 4364 | high |
| 52 | `NgEngine/Zge/ZgeMods/zobjModel.c` | 0x00488d30-0x004892af | 35 | 1 | 0/0/0/0/0/0/0/34 | 1111 | high |
| 53 | `NgEngine/Zge/Com/ZglRw/zrwMat.c` | 0x004892b0-0x00489caa | 25 | 9 | 0/0/0/0/0/0/0/16 | 2378 | high |
| 54 | `NgEngine/Zge/Com/ZglRw/zrwZgl.c` | 0x00489cc0-0x0048c10e | 43 | 6 | 0/0/0/0/0/0/0/37 | 8806 | high |
| 55 | `NgEngine/Zge/zList.c` | 0x0048c110-0x0048c5b4 | 20 | 1 | 0/0/0/0/0/0/0/19 | 1028 | high |
| 56 | `NgEngine/Zge/Com/ZglRw/zrwPrim.c` | 0x0048c5c0-0x0048d6e6 | 12 | 1 | 0/0/0/0/0/0/0/11 | 4330 | high |
| 57 | `NgEngine/Zge/ZgeMods/ztaginf.c` | 0x0048d6f0-0x0048f3b1 | 28 | 0 | 0/0/0/0/0/0/0/28 | 7141 | low |
| 58 | `NgEngine/Zge/Com/ZglRw/zrwModelRender.c` | 0x0048f3c0-0x0048ff4d | 19 | 3 | 0/0/0/0/0/0/0/16 | 2822 | high |
| 59 | `NgEngine/Zge/ZgeMods/zobjLight.c` | 0x0048ff50-0x00490176 | 11 | 2 | 0/0/0/0/0/0/0/9 | 470 | high |
| 60 | `NgEngine/Zge/zScene.c` | 0x00490180-0x00491d7f | 39 | 12 | 0/0/0/0/0/0/0/27 | 6832 | high |
| 61 | `NgEngine/Zge/ZgeMods/zscnpts.c` | 0x00491d80-0x004921af | 11 | 0 | 0/0/0/0/0/0/0/11 | 997 | low |
| 62 | `NgEngine/Zge/Com/ZglRw/zrwCam.c` | 0x004921b0-0x00493158 | 33 | 1 | 0/0/0/0/0/0/0/32 | 3784 | high |
| 63 | `NgEngine/Zge/Pc/pcinput.c` | 0x00493160-0x00493cde | 11 | 3 | 0/0/0/0/0/0/0/8 | 2898 | high |
| 64 | `NgEngine/Zge/Com/ZglRw/zrwCol.c` | 0x00493ce0-0x00495366 | 21 | 2 | 0/0/0/0/0/0/0/19 | 5623 | high |
| 65 | `NgEngine/Zge/ZgeMods/zobjPEmit.c` | 0x00495a10-0x00495de5 | 12 | 1 | 0/0/0/0/0/0/0/11 | 890 | high |
| 66 | `NgEngine/Zge/Com/ZglRw/zrwWorld.c` | 0x00495df0-0x00497771 | 33 | 3 | 0/0/0/0/0/0/0/30 | 6311 | high |
| 67 | `NgEngine/Zge/Com/ZglRw/zrwTxr.c` | 0x00497780-0x00498d71 | 49 | 4 | 0/0/0/0/0/0/0/45 | 5089 | high |
| 68 | `NgEngine/Zge/zanim.c` | 0x00498d80-0x0049a6cb | 36 | 4 | 0/0/0/0/0/0/0/32 | 6188 | high |
| 69 | `NgEngine/Zge/ziff.c` | 0x0049a6d0-0x0049b328 | 22 | 13 | 0/0/0/0/0/0/0/9 | 3011 | high |
| 70 | `NgEngine/Zge/Com/ZglRw/zrwShadow.c` | 0x0049b330-0x0049b872 | 5 | 1 | 0/0/0/0/0/0/0/4 | 1314 | high |
| 71 | `NgEngine/Zge/zcompres.c`* | 0x0049b880-0x0049c435 | 6 | 0 | 0/0/0/0/0/0/0/6 | 2954 | low |
| 72 | `NgEngine/Zge/Com/ZglRw/zrwModelCreate.c` | 0x0049c440-0x0049df87 | 51 | 7 | 0/0/0/0/0/0/0/44 | 6622 | high |
| 73 | `NgEngine/Zge/Com/ZglRw/zrwModelGeom.c` | 0x0049df90-0x0049e1e7 | 9 | 3 | 0/0/0/0/0/0/0/6 | 554 | high |
| 74 | `NgEngine/Zge/Com/ZglRw/zrwFile.c` | 0x0049e1f0-0x0049ea91 | 17 | 17 | 0/0/0/0/0/0/0/0 | 2088 | high |
| 75 | `NgEngine/Zge/ZgeMods/zramdisk.c` | 0x0049eaa0-0x0049f4cf | 17 | 16 | 0/0/0/0/0/0/0/1 | 2473 | high |
| 76 | `NgEngine/Zge/Com/ZglRw/zrwLight.c` | 0x0049f4d0-0x0049f8f2 | 10 | 1 | 0/0/0/0/0/0/0/9 | 995 | high |
| 77 | `NgEngine/Zge/zge.c` | 0x0049f900-0x0049fb29 | 5 | 1 | 0/0/0/0/0/0/0/4 | 546 | high |
| 78 | `NgEngine/Zge/Pc/pcmovie.c`* | 0x0049fb30-0x004a105a | 22 | 0 | 0/0/0/0/0/0/0/22 | 5260 | low |
| 79 | `NgEngine/Zge/ZgeMods/zpanorama.c`* | 0x004a1060-0x004a1111 | 5 | 0 | 0/0/0/0/0/0/0/5 | 143 | low |
| 80 | `NgEngine/Zge/ztoken.c`* | 0x004a1120-0x004a2615 | 18 | 0 | 0/0/0/0/0/0/0/18 | 5219 | low |
| 81 | `NgEngine/Zge/ZgeMods/zFont.c` | 0x004a2620-0x004a2c62 | 11 | 3 | 0/0/0/0/0/0/0/8 | 1533 | high |
| 82 | `NgEngine/Zge/zutil.c` | 0x004a2c70-0x004a31f3 | 10 | 0 | 0/0/0/0/0/0/0/10 | 1344 | low |
| 83 | `NgEngine/Zge/Com/ZglRw/zrwRamDisk.c` | 0x004a3200-0x004a35e6 | 13 | 1 | 0/0/0/0/0/0/0/12 | 881 | high |
| 84 | `NgEngine/Zge/zprofile.c` | 0x004a35f0-0x004a4225 | 15 | 1 | 0/0/0/0/0/0/0/14 | 3032 | high |
| 85 | `NgEngine/Zge/ztimer.c`* | 0x004a4230-0x004a45dc | 9 | 0 | 0/0/0/0/0/0/0/9 | 891 | low |
| 86 | `NgEngine/Zge/Pc/pcProfile.c` | 0x004a45e0-0x004a46bb | 5 | 1 | 0/0/0/0/0/0/0/4 | 199 | high |
| 87 | `NgEngine/Zge/zheap.c`* | 0x004a46c0-0x004a4a1a | 9 | 0 | 0/0/0/0/0/0/0/9 | 796 | low |

### Unattributed game/engine code (gaps between modules)

Each gap lies between the last function of the previous file and the first of the next one;
its functions belong to one of the two neighbours or to a file with no path string.

| Gap VA range | Funcs | Bytes | Previous file | Next file |
|---|---:|---:|---|---|
| 0x00436520-0x00436c05 | 15 | 962 | `rider` | `guiinput` |
| 0x00495370-0x00495a07 | 14 | 1583 | `zrwCol` | `zobjPEmit` |

<!-- END GENERATED -->

# The ZGE engine objects and the small engine modules (DM-061)

Sources: `decomp/src/NgEngine/Zge/` `zobj.c`, `ZgeMods/zobjCamera.c`, `zobjModel.c`, `zobjLight.c`,
`zobjPEmit.c`, `zge.c`, `zinput.c`, `ztoken.c`, `ZgeMods/zFont.c`, `zprofile.c`, `ztimer.c`, `zheap.c`,
`Pc/pcinput.c`, `Pc/pcmovie.c`, `Pc/pcProfile.c`, `Com/ZglRw/zrwRamDisk.c`; types `decomp/include/zobj.h`,
`zpc.h`, `ztools.h`; test `tests/test_decomp_dm061.py`. Evidence tags as in CONTRIBUTING.md AD-5: **[V]** read
from the code or the exe (the test reads the tables and call sites back), **[I]** inferred. Nothing
here is verified in game. Input devices: [input-camera.md](input-camera.md) 2.4.

## 1. Start-up order (`zge.c`) [V]

`zgeInit(sceneFile)` 0x0049F900 (called from App_Open via NgApp): zmem, zmath, zfile, zstring, ziff,
ZGL (`zrwZgl` 0x0048AFE0), **zinput**, zdebug, **zobj** (`zobjInit(0x32)`: the argument is only
asserted non-zero), zrwCol, zScene (`Scene.Txt`), **ztimer** (64 events), **zprofile**. `zgeShutdown`
0x0049F9C0 runs the same list backwards (App_Start's exit path). `zgeScalarFromRw` / `zgeVecFromRw`
convert RenderWare units back to ZGE units (zglGet 0x13 / 0x11: divide by the world scale, negate y
and z).

## 2. The object model (`zobj.c`) [V offsets, I names]

A **zobj** (`ZObj`, 0x3C, from one free list of 0x100 per block) is the engine's scene node:

| Offset | Field | |
|---|---|---|
| +0x00 | flags | bits 0-2 type (0 model, 1 light, 2 particle emitter, 3 camera), 0x08 active, 0x10 (set by `zobjSetFlag10`, never read), 0x20 attached |
| +0x04 | pos | local position (+0x10 unused pad) |
| +0x14 | rot | local rotation quaternion |
| +0x24 | link | `ZobjLink` (parent, parent model frame, next sibling, first child), allocated on demand |
| +0x28 | vtbl | `ZobjVtbl`, 9 slots: destroy, update, setPosition, setRotation, setScale, setActive, getId, getRwObject, getBounds |
| +0x2C | handle | the renderer object (zrwModel / zrwLight / zrwCam / emitter) |
| +0x30 | data | per type (camera matrices, model node table, emitter state) |
| +0x34 | user | `zobjSetUserData` |

The `zobjSet*` functions store the local value and forward it through the vtable. Every frame the
ZGL callback `zobjUpdateAll` (priority 8) walks all **active root** objects: `update` pushes the
transform to the renderer; each active child gets `world = parent matrix (or the parent model's
frame matrix) x local` pushed through setPosition / setRotation, then `update`, recursively
(`zobjUpdateChild`, `zobjForEachChild`). `zobjAttach(obj, parent, frame)` pushes the object at the
front of the parent's child list and sets ATTACHED; `zobjFind(id)` matches `vtbl->getId` (a model's
name hash).

**[V] original bug in `zobjDetach`** 0x00487830: when the object is not its parent's first child,
the code unlinks it but then clears the flags / link and re-applies the transform of the *previous
sibling* instead of the object itself (it uses the wrong register); the detached object keeps its
ATTACHED flag and a stale parent pointer. Harmless in the shipped game: `zobjAttach` has no caller
[V, thunk-aware xref scan], so no hierarchy is ever built and `zobjDetach` (called only by
`zobjDestroy`) always takes the no-link path.

### 2.1 Types

| Type | vtable | notes |
|---|---|---|
| model | 0x00560FD0 static, 0x00560FA8 animated (models with 2+ frames; slot 0 0x00488D30 also frees the zanim node table) | every slot forwards to the zrwModel function table 0x00560E94; setActive / Hide / Show toggle zrwModel flag 2 (hidden); `zobjModelRenderAll` draws every model object |
| light | 0x00561858 | types 0-3 index the RenderWare light type table 0x00563408; setRotation turns the quaternion into an axis and sets it as the light direction; flags 1/2 = light atomics / world |
| camera | 0x00560518 | data = four matrices (view, its inverse, rotation only, yaw only) rebuilt on demand (`zobjCameraGetMatrix`), invalidated by setPosition / setRotation; created at (0, 0, -100) looking at the origin with a 55-degree field of view (0x271C) |
| particle emitter | 0x00562098 | data `ZobjPEmitData` (0x3C): position, rotation, scale, active; destroy / update / create are in DM-059's gap files |

## 3. The script tokenizer (`ztoken.c`) [V]

Reads `Scene.Txt` for zScene (`zsceneLoadDatabase`, `zsceneParseObject`). Tokens are separated by
space, tab, CR, LF and **comma**; tokens are upper-cased unless quoted. `;` or `//` comments to the
end of the line; `/*` and `*/` (as separate tokens) nest. Preprocessor: `#INCLUDE file`,
`#INCLUDE_PATH dir`, `#DEFINE name value`, `$$name value`, `$$$file`, `$$~` (forget all symbols);
`#ifndef name` / `#endif` are read and **ignored** (no conditional compilation). `$name` expands to its
value; `$* $+ $- $/ a b` evaluate integers (decimal, `0x..` or `$..` hex). Includes nest three deep;
the symbol table is 0x400 bytes of `$NAME@value`. Quirks [V]: a successful `$$$file` include returns
"no token"; `#INCLUDE` / `$$$` failures print the include stack and end the stream. The DM-061 port in
the test tokenizes Scene.Txt exactly like DM-043's stand-in (case aside).

## 4. Fonts (`zFont.c`) [V layout, I meaning]

C++ code (`new[]` cookies). A `ZFNT` IFF chunk (or file) is a vector font: u16 version 0x0101, u16
glyph count, then per glyph u16 points (0 = empty), u16 strokes, 16 bytes per point, and per stroke a
u16 count plus that many u16 point indices. Fonts stay loaded; the only drawer is `zfontDrawTest`, a
debug spinner (first font, one glyph every 30 frames, one degree per frame, at (200, 300)). No ZFNT
chunk exists in the shipped data [V: no file under assets, bin or save contains the bytes `ZFNT`].

## 5. Movies (`pcmovie.c`) [V code; files checked by the test]

`PcMovie` (0x28C) plays one AVI with AVIFile: up to 8 streams; the first video stream is drawn with
DrawDib at the time since `pcmoviePlay` (GetTickCount), the first audio stream goes through waveOut
in up to 16 buffers of 32 KB (`WHDR` + data), refilled from the window procedure (MM_WOM_DONE ->
`pcmovieAudioDone`). Audio needs `0 < dwSampleSize <= 0x8000`. In full screen the DirectDraw primary
surface is flipped to GDI before drawing. Looping restarts the clock (audio loops in `FillAudio`).

The seven files of `bin\movie` (added 2026-11-13: acclaim, amish, credits, intro, mirra, nyquist,
zaxis) all fit: stream 0 is **Cinepak** (`cvid`) 320x240, 24-bit, 15 fps; stream 1 PCM 16-bit, mono
37 800 or 38 700 Hz (sampleSize 2), credits stereo 44 100 Hz (sampleSize 4). Cinepak is decoded by the
system's `iccvid` codec through `AVIStreamGetFrameOpen`; without it no frame is drawn (the audio still
plays). Which movie is played when is zrwZglPlayMovie's and the front end's business (DM-059,
[frontend.md](frontend.md)).

## 6. The RAM disk file interface (`zrwRamDisk.c`) [V]

`zrwRamDiskBegin` swaps RenderWare's file interface (`RwOsGetFileInterface`) for one that reads
registered memory blocks (`zrwRamDiskAddFile(address, size, extension)`, two slots, no bound check):
each `rwfopen` ignores the name and opens the **next** block through the zfile `rd00` device
(name built from ADDRESS / SIZE); `rwfexist` only checks the extension; `rwfread` returns zfileRead's
result. `zrwRamDiskEnd` restores RenderWare's interface. Nothing in the shipped exe calls Begin /
AddFile / End.

## 7. Profiler, timer, heap (`zprofile.c`, `ztimer.c`, `zheap.c`, `pcProfile.c`) [V]

- `pcProfile`: QueryPerformanceCounter; elapsed ticks use the low 32 bits only.
- `zprofile`: CPU bars (`ZprofileBar`), drawn by a ZGL callback (priority 4) as a grey peak quad, a
  coloured value quad and tick marks every 1 / 0.1 / 0.01 / 0.001 frame (60 Hz frames; white / red /
  green / blue). "App" spans frame callbacks 7..8, "EngCPU" starts at 0. Layout defaults: margin 32,
  bar 8 / fill 6 / gap 2 pixels, 2 frames full width, peak reset every 120 frames, 10 frames before
  peaks count. No code creates a bar, so only the empty draw callback runs.
- `ztimer`: timed callbacks on a `zheap` min-heap keyed by `timeGetTime`. **Never works [V]:** every
  Add / Create function takes the free-list head *before* calling the lazy allocator, so they all
  return 0 (and a fired event is re-added to a free list it was never removed from). Nothing calls them.
- `zheap`: a 1-based binary heap with a sentinel, min or max; `zheapRemove` never finds the last item.

## 8. Discovered and open

- The C runtime starts at `_fpmath` 0x004A4A20, not 0x004A4A80 (build_symbols `LIB_RANGES` moved).
- Holes left for other cards: zfile.c 0x004853A0 / 0x004854C0 / 0x004859B0, ziff.c 0x0049A740
  (ziffShutdown, called by zgeShutdown) and four more, zScene.c 0x00490B50 / 0x00490C6C, zrwCol.c
  0x00494C40 (called by zgeShutdown), ztaginf.c / zrwModelRender 0x0048F3C0 (2 KB), zscnpts/zrwCam
  0x004921B0, zrwShadow/zcompres 0x0049B3D0, 0x0049C440: code that is not yet a Ghidra function.

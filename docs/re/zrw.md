# The ZglRw layer: ZGL on RenderWare 3.1 (zrw*.c)

DM-059. C: `decomp/src/NgEngine/Zge/Com/ZglRw/zrw*.c` (zrwTxr 49, zrwMat 25, zrwModel 47, zrwModelCreate 44,
zrwModelGeom 9, zrwModelRender 19, zrwLight 10, zrwCam 33, zrwCol 21, zrwPrim 12, zrwShadow 5, zrwDbg 32,
zrwZgl 43 functions; zrwWorld.c is DM-043's, zrwFile.c DM-041's, zrwRamDisk.c DM-061's) and the gap files
around them; types, driver tables and the RenderWare prototypes in `decomp/include/zglrw.h`; checks in
`tests/test_decomp_dm059.py`. Tags as in CONTRIBUTING.md AD-5: [V] = checked against the exe or the corpus
(mostly by that test), [I] = inferred.

DM-059 drafted 401 functions (395 in these files and the gaps, 4 zfx affectors in zrwWorld.c, 2 wrappers in
zcompres.c); 172 of them were not functions in the Ghidra project: they are reached only through the
driver tables, RenderWare callbacks or jump tables. DM-059's rows in `decomp/symbols/symbols.csv` create them
(172 game functions + 4 RenderWare ones). 0x0049D895 is a Ghidra artefact: the tail of the MODL handler,
kept as `zrwModelChunkMODLBody`.

## 1. Architecture

The engine core (zge, zobj*, zScene, NgIF) never calls RenderWare. Every `zrw*.c` module implements
one ZGL "driver": a const table of function pointers in `.data`, which the core calls through [V: table
contents vs the C, test_driver_tables_match_the_exe]:

| Table | VA | Slots | Module | Bound by |
|---|---|---|---|---|
| texture | 0x005623F0 | 15 | zrwTxr.c | `g_texDriverVtbl` (zrwZglInit calls slot 0) |
| material | 0x00561018 | 15 | zrwMat.c | zrwZglInit |
| model | 0x00560E78 | 41 | zrwModel.c, zrwModelCreate.c, zrwModelGeom.c, zrwModelRender.c | zrwZglInit, zobjModel.c |
| light | 0x00563420 | 10 | zrwLight.c | zrwZglInit, zobjLight.c |
| collision | 0x00562060 | 4 | zrwCol.c | zrwZglInit, NgIF |
| primitives | 0x00561720 | 7 | zrwPrim.c | zrwZglInit, NgIF, zrwDbg, zrwShadow |
| camera | 0x00561D88 | 15 | zrwCam.c | zobjCamera.c |
| system | 0x00561468 | 13 | zrwZgl.c | zge.c (`zgeInit`) |
| debug | 0x00560030 | 20 | zrwDbg.c | zdebug.c (`g_pfnDebug*` slots) |

[V] `zrwZglInit` (0x0048AFE0) initialises the drivers in the order texture, material, model, light,
collision, primitives, then the world (zrwWorld.c). Every ZGL object starts with a word whose low byte is
its type: model 0, camera 1, light 2, material 3, texture 4; all are reference counted and come from
fixed free lists (256 textures, 256 materials, 256 models, 2 cameras, 2 lights).

**Coordinates** [V: zrwModelUpdate, zrwColLineTest, zrwLightSetPosition, zrwCamSetMatrix]. The engine
works in ZGE units with y and z the other way round. With `s = g_zrwScale` (0x00697720 = 1 / ZGL value 1)
and `o` the world origin (0x006976CC = `RpWorldGetOrigin`, i.e. `-invWorldOrigin` of BSP._sd):

    rw = (zge.x * s + o.x,  o.y - zge.y * s,  o.z - zge.z * s)
    zge = ((rw.x - o.x) / s,  -(rw.y - o.y) / s,  -(rw.z - o.z) / s)

Quaternions flip y and z the same way (`zrwModelQuatToRwMatrix`; ZGL value 0x10 selects a mirrored
variant). Angles are 16-bit (0x10000 = 360 degrees).

## 2. Models: ZMD -> RpClump (zrwModelCreate.c)

`zrwModelCreateInit` registers the IFF handlers 'MODL', 'MLOD' and 'MCOL' [V]. A ZMD is read by
`zrwModelLoadFile` (zfileLoadAll, "Not a MODL" unless it starts with MODL) or `zrwModelLoadMemory`; the
MODL handler (`zrwModelChunkMODL` 0x0049D7E0) then does, in this order:

1. **MDEF** (`zrwModelReadMDEF`, "Missing model def block"): reads the 0x2C-byte header: `kind`
   becomes the model-def flags, `name[16]` and `name_id` are kept, the four `params` floats and the
   u16 at +0x2A are **not read** [V]. Then `count` entries of name[16] + u32 id; byte 15 of every name
   is forced to 0, so a 16-character MDEF name loses its last letter in the engine [V: the corpus has
   such names, e.g. `_RightShoulder01`].
2. **MATI** chunks (all of them, through zrwMat with a creation callback that adds each new material to
   the model's material list). The model's materials are matched to the clump's RpMaterials afterwards
   by RwTexture identity (`zrwModelSetRwInfo`; "Failed to match RpMaterial" otherwise).
3. **MLOD** chunks (`zrwModelChunkMLOD` 0x0049D350), at most 4 ("Ignoring extra LOD in %s"):
   - **MLDF**: 8 bytes read; **word 0 (the "flags" 7 / 3 / 0x1B) is ignored**, word 1 is the LOD
     switch distance as a float; 0 means the defaults 0, 50, 100, 150 (0x00562CC4) [V]. Every corpus
     ZMD has word 1 = 0 [V].
   - **MDFF**: the payload is read into an RW-allocated buffer and parsed with
     `RwStreamOpen(rwSTREAMMEMORY)` + `RwStreamFindChunk(rwID_CLUMP)` + `RpClumpStreamRead`
     (`zrwModelLoadClump` 0x0049D120). The plugins attached in App_Open decide what is understood:
     **RpWorld, RpAnim** (the frame extension 0x108 = the bone id, read with `RpAnimFrameGetID`),
     **RpBone** (the 0x10E skin ranges), **RpLODAtomic**, **RpPVS**, plus the game's texture plugin
     (0xFF01, the owning ZGL material) and clump plugin (0xFF02, the owning ZglModel).
   - LOD 0's clump is the model; its first atomic, geometry and default render callback are kept in
     the def. Every LOD gets a **part table**: one `ZglModelPart` per frame (bone id, frame, bind
     position, parent / sibling / child links) that the animation system binds to its nodes by id
     (`zrwModelBindParts`).
   - [V] Nothing in the ZglRw code writes the current-LOD field (+0x48) and only LOD 0's clump is ever
     added to the world, so **the PC build always draws LOD 0**; the distances are stored but unused
     [I: no other code reads them]. This answers ZMD.md open question 1.
4. **MCOL** (only with MDEF flag 0x80; no corpus file has it): a separate collision clump, never
   rendered, always in the world.

After the chunks: MDEF flag 2 runs `RpBoneClumpInitialize` (skinned models); the global render mode is
applied; MDEF flag 0x10 models get the "projected" pass (section 2.4), the others are added to the world
unless flag 4 is set.

**MDEF flags** (the ZMD.md "kind" word) as the engine uses them [V: code; I: names]:

| Bit | Meaning |
|---|---|
| 0x01 | has a custom render (zrwModelRender / RenderShadowPass assert "Not coded" without it) |
| 0x02 | skinned (RpBone: initialise at load, update every frame; `createInstance` reloads the file instead of cloning) [V: set exactly for the corpus models with Bone plugins] |
| 0x04 | not added to the world at load |
| 0x08 | unlit: rpGEOMETRYLIGHT cleared |
| 0x10 | flattened / projected render pass every frame (one model at a time) |
| 0x40 | no collision flag at instancing |
| 0x80 | has an MCOL clump |
| 0xC0 | no collision test on the atomics |

The corpus uses only 0 and 2 [V].

### 2.1 Instances, transforms and parts

`zrwModelCreateInstance` shares the def and the materials and clones the clump with `RpClumpClone`.
The clone loses the frames' RpAnim ids, so they are parked in each frame matrix (at.x = low 16 bits,
at.y = high 16 bits) and restored on the clone; harmless because every bound frame matrix is rebuilt by
the animation each update [V code; I harmless]. `zrwModelUpdate`: root = scale (if set) x rotation(quat)
at the RW position of the ZGE position; each bound part's frame = its node's rotation at bind position +
node translation x scale; skinned models then update their bones; then the model's update callback.

### 2.2 Model flags (`zrwModelSetFlags`, slot 24)

1 = def flag bit 0 + atomic flag 0x80 (set for one-part models by zobjModelCreate) [I: meaning];
2 = in the world; 4 = MCOL clump rendered; 8 = collision (atomic flag rpATOMICCOLLISIONTEST, or the MCOL
clump in / out of the world).

### 2.3 Geometry access (zrwModelGeom.c)

Lock / unlock LOD 0's geometry (morph target 0) and get / set vertex n (1-based) in ZGE units.

### 2.4 Rendering (zrwModelRender.c)

Models are drawn by the world (`RpWorldRender`). Extra atomic render callbacks: a nop (render mode 1),
a flat untextured Im3D line drawing (mode 2), a camera-facing billboard (NgApp), a tinted pass that
applies the model colour (`zrwModelSetColour`), and for MDEF-0x10 models a pass that moves the model to
the camera and draws it textured with far clip 300 and z test/write off.

## 3. Textures (zrwTxr.c) -> TEXTURE.md 3

- [V] MATI = MADF + TBMP / TPNG (TDDS is accepted and ignored). The MADF handler reads the payload
  (< 0x90 bytes, nameOffset 0x48) and passes the name on; the image handler loads the texture under that
  name and wraps it in a material; the MATI handler copies the six MADF words (TEXTURE.md 2.1).
- [V] `zrwTxr_CreateTextureFromMemory`: type 0 = .bmp, 1 = .png. The buffer is mounted as an in-memory
  file (zrwRamDisk.c) with the extension "BMP" / "PNG" and `RwTextureRead(name, NULL)` runs: a texture
  of that name already in the **current texture dictionary is reused**, otherwise the registered image
  reader for the extension (`zrwTxr_ReadBmp` = RtBMPImageRead, `zrwTxr_ReadPng` = RtPNGImageRead /
  libpng 1.0.5) decodes it, applies the colour key and RenderWare picks the raster format. If that
  fails, the image is read again as a BMP and put into a 16-bit 565 texture raster, else a 32-bit one.
- [V] Colour key RGB 255,0,255 -> RGBA 0,0,0,0: on the palette for 4/8-bit images (so paletted
  textures stay paletted), on the pixels otherwise. The port in the test agrees with
  `dmbmx.formats.texture.apply_color_key` on every TBMP of the corpus.
- [V] Filtering: linear (linear-mip-linear when mipmapping is on: ZGL value 0xE; off by default,
  `RwTextureSetMipmapping(FALSE)`); addressing clamp when g_zrwZglFlags bit 0 is set (the default after
  zrwZglInit), else wrap.
- **Names** [V]: the texture name is the MADF name; the ZGL texture keeps `zHashName(name)`; materials
  are found by the name hash of their first texture (`zrwMatFindByTextureHash`, `zrwModelFindMaterial`).
  Because RwTextureRead looks the dictionary up first, a later MATI with the same name gets the first
  one's texture [I: consequence].
- Palette access (`zrwTxrGetPalette` / `zrwTxrSetPalette`) converts the raster palette to and from RGBA
  for palette animation (zroller's animated materials).
- `zrwTxrLoadPalettedImageFromMemory` (PAL8 raster, else 1555 with 0x7C1F transparent) and
  `zrwTxrCreateProceduralTexture` exist but have no caller.

## 4. Materials (zrwMat.c)

`ZglMaterial` (0x74): MADF flags / words, the surface word (+0x14 = MADF +0x38: surface type and
animation bits, returned by `zrwMatGetSurface`), up to 6 texture passes, the RpMaterial once linked.
Materials of a level (Mat._sd) are collected by zrwWorld.c, those of a ZMD by the model loader, both
through the creation callback. `zrwMatSetColour` sets the RpMaterial colour (model tints).

## 5. Collision queries (zrwCol.c)

Two queries, ZGE coordinates in and out [V code]:

- `zrwColLineTest(a, b, mode, exclude, hit)` (slot 2): the world sectors through
  `RpCollisionWorldForAllIntersections` (which walks each sector's 384-byte collision tree: RW
  0x004BE560, ported in `dmbmx.formats.rwcoll.line_test`) and / or the atomics of the world's clumps
  (`RpWorldForAllAtomicIntersections` + `RpAtomicForAllIntersections`); **the nearest triangle (smallest
  t) wins**. Result `ZrwColHit`: normal, corners, t, the owning ZglModel (NULL for the world), the ZGL
  material (through the RwTexture owner plugin: its surface word gives the surface type) and the point.
  [V] The port in the test (ZGE -> RW conversion, per-sector walk, nearest hit) finds the same triangle
  and t as brute force over WoodwardDirt.
- `zrwColSphereTest(centre, radius, mode, exclude, list)` (slot 3): every triangle touching the sphere,
  in a growable list (0x400, +16).
- mode bits: 1 world sectors, 2 / 4 atomics with / without flag 0x80 (both = all), 8 skip the clumps in
  the NULL-terminated `exclude` list, 0x20 clip the line to the world box (ZGL value 2).
- `zrwColGetHitColour`: the prelit colour at the last hit (sector triangles only), for the rider's
  ground tint (NgApp).

## 6. Shadows (zrwShadow.c)

A blob shadow: a 6 x 6 vertex grid (size = 2 x ZGL value 1), oriented by the owner's shadow direction,
each vertex dropped onto the world with the line test and lifted size/10 along the hit normal; quads with
a missing corner are skipped. Drawn with the "ShadowMap" material (`assets\shadowMap.Bmp`) through zrwPrim
(flags 0x4103) every frame [V strings, code].

## 7. Cameras (zrwCam.c)

One frame buffer + z buffer at the screen resolution (default 640 x 480; `-rW.H.D` on the command line
selects a fullscreen mode), and per camera sub-rasters for its window. Defaults [V]: fov 0x271C (55
degrees), view window tan(fov)/2 by 3/4 of that, near 0.5, far 75; fog (flag 2) uses a colour and a
distance before the far plane (ZGL values 7-9). `zrwCamSetMatrix` turns a ZGE camera matrix and position
into the RW frame (axes re-signed, position scaled from the world origin).

## 8. Immediate mode (zrwPrim.c)

Lines, triangles, quads and polygons of `ZglVertex` through Im3D (world or camera space) or Im2D (screen
space; flag 0x02000000), render state = flags & 0xFFFFFF. 2-D quads of a windowed camera are clipped to
its window (Sutherland-Hodgman).

## 9. Debug driver (zrwDbg.c)

`g_pfnDebugPrintf` = OutputDebugString with an indent (0..32); `zdebugPrint(x, y, fmt, ...)` queues
on-screen text (RtCharset; y < 0 = rows below the previous line); up to 512 queued shapes (axes, line,
triangle, sphere, box) per frame; an fps / RW metrics line. All drawn by ZGL callback list 4.

## 10. The application (zrwZgl.c)

- **WinMain**: window class "RwAppClass", 640 x 480 client area, `-rW.H.D` command-line video mode,
  App_Open, App_Start, then the loop: App_Frame + zrwZglRender per frame, throttled to ZGL value 0 (fps;
  0 = unlimited). Escape quits unless ZGL value 0x14 is set; window (de)activation (un)acquires the
  DirectInput devices.
- **App_Open**: RwEngineInit, the plugins (RpWorld, RpAnim, RpBone, RpLODAtomic, RpPVS, the two game
  plugins), RwEngineOpen, the device dialog (resource 0x68: subsystems except "MMX" ones, modes
  640x480 .. 1600x1200 at 16 or 32 bits) or the -r mode, RwEngineStart.
- **App_Start**: zmem's platform heap = RW's allocator, the first command-line word -> `zgeInit`, then
  `ngappInit`. **App_Frame**: dt, the fps counter, callback list 7, `ngappFrame(dt)`, list 8.
- **zrwZglRender**: clear the cameras, list 0, then per camera: begin update (fog, render state), PVS,
  list 1, the world, list 2, list 5, list 4 (debug), end update (a pending RW error asserts); list 3;
  `RwRasterShowRaster`.
- **ZGL values** (system slots 7 / 8): 0 fps, 1 world units per RW unit, 2 world box, 3 / 4 clear mode /
  colour (127, 174, 221 by default), 5 far clip, 6 near screen z, 7-9 fog, 10 / 11 screen size, 0xC
  clamp textures, 0xD flag 4, 0xE mipmapping, 0x10 mirrored quaternions, 0x11-0x13 RW <-> ZGE scaling,
  0x14 no Escape quit, 0x15 the window record.
- **Render state** (`zrwZglSetRenderState`): bit 1 no z test / write, 2 blending (else src/dest forced
  to 8/1 [I]), 4 flat shading, 8 no vertex alpha, 0x20 clamp, 0x0F00 / 0xF000 source / destination blend;
  only changed bits reach RenderWare; a 4-deep push / pop stack.
- **The CD check** `app_CheckGameCD` (DM-030 #9): `HKLM\Software\Acclaim Entertainment\Dave Mirra
  Freestyle BMX\1.00.000\SRCDISK` + `\bin\movie\acclaim.avi` must open, else "Please Insert Game CD".
- **Movies** (`zrwZglPlayMovie`): a missing file is skipped; any newly pressed button aborts.

## 11. Open questions

1. Atomic flag 0x80 (model flag 1): set for one-part (static) models; its effect in RenderWare 3.1 is
   not identified.
2. MDEF flags 0x10 / 0x40 / 0x80 are coded but unused by the corpus; what the 0x10 "projected" pass
   was for (a reflection or sky object?) is unknown.
3. The RenderWare names in zglrw.h are [I]: from the RW 3.x SDK and their use; RW 3.1's render-state
   numbering (states 4 / 6 as z test / write) is assumed.
4. `zrwColGetHitColour` is simplified in the C (interpolation along the corners' height).

# Dave Mirra Freestyle BMX (PC, 2000): file format specification

One self-contained description of every file format of the PC game (Z-Axis / Acclaim, `NgBMX.exe` built
2000-11-22), written so that you can implement a parser from this document alone. The reference
implementation is `tools/3dsmax/parser/dmparse.py` (stdlib Python); the 3ds Max importers in `tools/3dsmax/` use it.

Condensed from the project's detailed format notes (`docs/formats/*.md`, `docs/re/*.md`), which carry the
full evidence trail.
- **[V]** = verified by running code over every shipped file, or read in the executable.
- **[I]** = inferred.
- Addresses are virtual addresses in `NgBMX.exe` (image base 0x00400000).

All integers are **little-endian**. "u32" = unsigned 32-bit, "i32" signed, "f32" IEEE float, "char[n]"
a NUL-terminated string in an n-byte field (bytes after the NUL are padding or junk, keep them verbatim
when rewriting).

This edition covers every shipped format, the level files included.

---

## 1. Where things are

| Path (install) | Format | Section |
|---|---|---|
| `assets\*.zmd` (117; 2 are 0 bytes) | model: rider, bike, shadow, sfx | 3 |
| `assets\<Level>\<Level>.ZSD` (12 levels) | level include file (`SCEN`) | 5 |
| `assets\<Level>\Mat._sd` | level textures (`MATI` sequence) | 4.4 |
| `assets\<Level>\BSP._sd` | RenderWare World (`0x0B`): the level geometry | 5.4 |
| `assets\<Level>\PropDef._sd`, `Prop._sd`, `Scene._sd` | prop models, placed props, scene points + panorama | 5.5 |
| `assets\<Level>\Tag._sd` | tagged collision faces | 5.6 |
| `assets\<Level>\temp.dff` (some levels) | bare RenderWare Clump | 2.2 |
| `assets\*.zal` (6) | asset archive: Intwrap, material, miscdata, motion, replay, tables | 6 |
| `bin\audio\*.zsd` (22) | sound bank (`ZSND`) | 9 |
| `save\SAVE.DAT` | save game | 10 |

**Identify a file by its first bytes**:

| First bytes | Format |
|---|---|
| `MODL` | ZMD model |
| `SCEN` | level ZSD |
| `PDBL` / `PRBL` / `SCPT` / `TAGB` | PropDef / Prop / Scene / Tag `_sd` |
| `MATI` | `Mat._sd` |
| `FORM` | IFF table |
| `STRS` | string table |
| `ZSND` | sound bank |
| `1C 51 81 2A` | ZAL archive or SAVE.DAT: SAVE.DAT is exactly 9,052 bytes and has u32 0x235C at offset 4 |
| u32 0x0B / 0x10 | RenderWare World / Clump |

> Name clash: `bin\audio\*.zsd` are sound banks (`ZSND`); `assets\<Level>\*.ZSD` are level include files (`SCEN`).

## 2. Common layers

### 2.1 Z-Axis 4CC chunks

```
chunk := char tag[4]; u32 size; u8 payload[size]          size EXCLUDES the 8-byte header
```

- [V] There is no alignment between chunks. Container chunks hold only child chunks: `MODL`, `MLOD`,
  `MATI`, `FORM`, `SCEN`.
- Every other chunk holds raw data.
- A child always ends inside its parent.

### 2.2 RenderWare chunks

```
rwchunk := u32 type; u32 size; u32 version; u8 payload[size]    size EXCLUDES the 12-byte header
```

[V] Every chunk inside one stream carries the same version word: **0x00000301** (most files) or
**0x00000303** (`Amishbike01`, `secretbike01`, `TroyMcMurray1v1`). That is the RenderWare 3.0 / early
3.x encoding: no packed build number, and Geometry inline in the Atomic (no GeometryList). The task
called this "RenderWare 3.1". The version words say 3.0.x, and the layout is the pre-3.1 one.

| Type | Name | Container | Used in |
|---|---|---|---|
| 0x01 | Struct | no | everywhere: the fixed fields of the parent |
| 0x02 | String | no | texture names (NUL + junk padding to 4) |
| 0x03 | Extension | yes | plugin list of the parent |
| 0x06 | Texture | yes | Struct(u32 filter/addressing = 0x106), String name, String mask, Extension |
| 0x07 | Material | yes | §3.6 |
| 0x08 | MaterialList | yes | §3.6 |
| 0x09 | AtomicSect (world sector) | yes | `BSP._sd`, §5.4 |
| 0x0A | PlaneSect | yes | `BSP._sd`, §5.4 |
| 0x0B | World | yes | `BSP._sd`, §5.4 |
| 0x0E | FrameList | yes | §3.3 |
| 0x0F | Geometry | yes | §3.4 |
| 0x10 | Clump | yes | `MDFF`, `temp.dff` |
| 0x14 | Atomic | yes | §3.2 |
| 0x105 | Morph plugin | no | payload u32 0 (kept raw) |
| 0x108 | Animation plugin (frame ext.) | no | u32 bone id, u32 0 |
| 0x10E | Bone plugin (geometry ext.) | no | §3.7 |
| 0x50E | BinMesh plugin (geometry ext.) | no | §3.5 |

Containers in this game: 0x03, 0x06, 0x07, 0x08, 0x09 (AtomicSector), 0x0A (PlaneSector), 0x0B, 0x0E,
0x0F, 0x10, 0x14.

### 2.3 Tag index

| Tag | Where | § |
|---|---|---|
| `MODL` `MLOD` `MLDF` `MDFF` `MDEF` | ZMD | 3 |
| `MATI` `MADF` `TBMP` `TPNG` `TDDS` | ZMD, `Mat._sd` | 4 |
| `SCEN` `SCDF` `SCFI` | level ZSD | 5 |
| `PDBL{PDIN, OBJI, MODL}` `PRBL{PRBL, OBJI}` `SCPT{SDPT, OBJI}` `SDPA{MODL}` | level `_sd` forms | 5.5 |
| `TAGB{TAGD, TAGN, TAGF, TAGI}` | `Tag._sd` | 5.6 |
| `FORM` `PLHD` `PLYR` | miscdata.zal id 1 | 7.1 |
| `FORM` `ZMHD` `ZMAP` | tables.zal id 0 | 7.2 |
| `STRS` | miscdata.zal | 7.3 |
| ZAL magic `0x2A81511C` | `*.zal` | 6 |
| motion bank (headerless ZAL) | motion.zal id 0 | 8 |
| `ZSND` `WIN ` `ZTRK` | sound banks | 9 |
| `1C 51 81 2A` + `5C 23 00 00` | SAVE.DAT | 10 |

### 2.4 Name hash (zhash)

[V] `zHashName` 0x004828C0. It produces bone ids, MDEF/MADF ids, and (in principle) ZAL asset ids.
Upper-case the name with the C-locale `toupper`, and treat the bytes as **signed** chars `c[1..n]`. Then,
mod 2^32:

```
zhash(s) = sum_{i=1..n} i * c[i]  +  (c[n] << 16)              zhash("") = 0
```

Examples: `Hips` = 0x00530316, `Head` = 0x004402A5, `LeftFoot` = 0x00540AF9, `DAVEMIRRA` = 0x00410D2A.

### 2.5 Coordinate system

- [V] Game/RenderWare space is **right-handed, Y up, +Z forward, +X = the model's left**. Evidence: bike
  wheels at y ≈ −0.3, seat at +0.5, front wheel at +Z, `LeftGrip`/`LeftHand` at +X. This is the same
  convention as glTF.
- [V] RenderWare uses **row vectors**: `p' = p · [right; up; at] + pos`, and a frame's world matrix is
  `local · parent`. 3ds Max uses the same convention (`Matrix3` rows), so a frame's four vectors are
  exactly a Max `matrix3`.
- [V] **The in-game riders' bind pose lies along +Z.** Head at z = +0.65, feet at −0.75, forward is −Y.
  The `Hips` frame (first child of the root `Skin` frame) rotates the Y-up skeleton to model +Z. To stand
  a rider up in Y-up space, apply `Rx(−90°)`: (x, y, z) → (x, z, −y). The rule is: for a skinned model,
  if the first child of the root frame has its world local-Y axis along ±Z, rotate ∓90° about X. Bikes,
  sfx, ShadowVolume and the test rider `DaveMirra.ZMD` are already Y-up.
  - [I] In game, the motion's Hips channel replaces that frame and stands the rider up (§8.4).
- **To 3ds Max / Blender (Z up):** apply `Rx(+90°)`: (x, y, z) → (x, −z, y). In row-vector form the rows
  are (1,0,0), (0,0,1), (0,−1,0).
  - For riders, the two rotations cancel, so model space is already Max/Blender space.
  - Front then faces −Y, which is the Front view.
- Units: a rider is about 1.95 units tall. The importers scale by 100 by default.
- UVs have their origin at the **top-left**. Max/OBJ use bottom-left, so write `v' = 1 − v`.

### 2.6 Level coordinates

[V] Levels use three spaces:

| Data | Stored in | To RW world space |
|---|---|---|
| World (`BSP._sd`) vertices, PropDef/Panorama models | RW world space (Y up, 1 unit ≈ 1 m) | as is |
| Tag faces, tag-tree bounds | engine (ZGE) units | `o + (x, −y, −z) / 100` |
| Prop and scene-point positions | ZGE / 100 | `o + (x, −y, −z)` |
| Prop rotations (9 f32, rows right/up/at) | – | **as is**: the stored rows are the RW rotation (`prop_matrix_to_rw`) |
| Scene-point rotations (9 f32, rows right/up/at) | ZGE axes | `F · M · F` with F = diag(1, −1, −1) (`matrix_to_rw`) |

- The origin `o = −invWorldOrigin` comes from the World struct (§5.4). The engine's ZGE space is
  `100 × (x − o.x, −(y − o.y), −(z − o.z))`.
- F is a 180° rotation about X, so it is a proper rotation: winding is preserved.
- [V] Props: the game turns the rows into a quaternion and renders the model with its *mirrored*
  quaternion converter (ngappInit sets ZGL value 0x10), which is D·(F·M·F)·D with D = diag(−1, 1, 1), i.e.
  M itself. `F · M · F` gives the opposite yaw sign (the earlier importers did that). The game's own
  prop collision boxes line up with the models only with M (docs/formats/LEVEL.md 2.1).
- A placed model's world position is `p_world = p_model · R + t` (row vectors), with R and t from the table.
- The importers re-centre levels on `o` (they subtract `o`), so the level sits at the Max origin. This is
  presentation only.

## 3. ZMD (models)

```
ZMD   := MODL                                                    0-byte files exist (TroyMcMurray1v2/1v3)
MODL  := "MODL" u32 size { MLOD+ MDEF MATI* }                    child order always MLOD..., MDEF, MATI...  [V]
MLOD  := "MLOD" u32 size { MLDF MDFF }                           one per LOD (only DaveMirra_shadow has 2)
MLDF  := "MLDF" u32 8 { u32 flags; u32 zero }                    flags 7 (in game), 3 (older assets), 0x1B (shadow LODs) [I meaning]
MDFF  := "MDFF" u32 size { one RW Clump stream }
MDEF  := "MDEF" u32 size { header + name table }                 §3.8
MATI  := "MATI" u32 size { MADF (TBMP|TPNG) }                    one per texture, §4
```

### 3.1 Clump

```
Clump(0x10)
  Struct          u32 numAtomics                     1 for riders and shadows, 7 for bikes
  FrameList(0x0E) §3.3
  Atomic(0x14) x numAtomics
  Extension       (empty)
```

### 3.2 Atomic

```
Atomic(0x14) { Struct{ u32 frameIndex; u32 flags (=5); u32 unused (=0) }, Geometry(0x0F), Extension(empty) }
```

### 3.3 FrameList and frames

```
FrameList { Struct{ u32 n; Frame[n] }, Extension x n }     each Extension holds Animation(0x108){u32 boneId; u32 0}
Frame (56 bytes):
  0x00 f32[3] right    0x0C f32[3] up    0x18 f32[3] at    0x24 f32[3] pos
  0x30 i32    parent (-1 = root; parents always come first)
  0x34 u32    flags (0)
```

- [V] `boneId = zhash(bone name)`. It links frames, MDEF names and the Bone plugin.
- 0xFFFFFFFF means "no bone" (bike frame 5, a ShadowVolume helper).
- **Naming a frame:**
  - the MDEF entry with that id (if several match, the **last** one wins: `bike_hires` has `wheel2` and `wheel1` with one id);
  - else a known name whose zhash matches;
  - else `bone_0x%08X`;
  - frames without an id are `frame_<i>`.
- Known names: Skin, Hips, _Abdomen, Chest, Chestx, Shoulders, _LeftShoulder, _RightShoulder, Head,
  Left/Right UpArm, LowArm, Hand, UpLeg, LowLeg, Foot; Hipsdeck, BackWheel, FrontWheel, Sprocket,
  Handlebars, SeatGrip, Left/RightGrip, Left/RightPedal, SFXCube, Cone01, wheel.

### 3.4 Geometry

```
Geometry(0x0F) { Struct, MaterialList(0x08), Extension{ BinMesh(0x50E), [Bone(0x10E)], [Morph(0x105)] } }
Struct:
  u32 flags                   0x01 tristrip, 0x02 positions, 0x04 textured, 0x08 prelit, 0x10 normals,
                              0x20 light, 0x80 two UV sets. Seen: 0x35 (most), 0x34, 0x3D, 0x39, 0x3C, 0x10
  u32 numTriangles, u32 numVertices, u32 numMorphTargets (=1)
  f32 ambient, specular, diffuse (1, 1, 1)
  if flags & 0x08: u8 rgba[numVertices][4]                    prelit vertex colours (shadows)
  if flags & 0x04: f32 uv[numVertices][2]   (0x80: two sets)
  u16 tri[numTriangles][4] = { v0, v1, materialIndex, v2 }
  per morph target: f32 sphere[4] (x, y, z, r); u32 hasVertices; u32 hasNormals;
                    f32 pos[numVertices][3] if hasVertices; f32 normal[numVertices][3] if hasNormals
```

- [V] **Winding:** the counter-clockwise triangle is **(v1, v0, v2)**. It agrees with the vertex normals
  (the sum of a face's three normals is on its CCW side for 99.6% of all model faces; 6 models have some
  that are not, DaveMirra.ZMD the most at 77% agreeing).
  All indices are < numVertices, and the material index is < the MaterialList count.
- [V] Normals are unit length. There are no unreferenced vertices in the corpus.
- [V] The bounding sphere is centred on the bounding-box centre and encloses every vertex.
- **Where vertices live:**
  - Skinned meshes (riders): **model space**.
  - Rigid parts (bikes): local to the atomic's frame. Transform by the frame's world matrix.

### 3.5 BinMesh (0x50E)

```
u32 flags (1 = triangle strips, 0 = lists); u32 numMeshes; u32 totalIndices;
per mesh: u32 numIndices; u32 materialIndex; u32 index[numIndices]
```

- [V] `numMeshes` equals the material count, and flags bit 0 matches the geometry's 0x01 flag.
- Strip triangle k is (s[k], s[k+1], s[k+2]) for even k and (s[k+1], s[k], s[k+2]) for odd k.
  Degenerate triangles are skipped.
- [V] The strips draw exactly the triangle list's triangles, with the same winding. A reader can ignore
  BinMesh and use the triangle list; a writer must regenerate it.

### 3.6 Materials

```
MaterialList { Struct{ u32 n; i32 index[n] }, Material x (count of -1) }   -1 = new material, k = reuse slot k
Material(0x07) { Struct{ u32 flags; u8 rgba[4]; u32 unused; u32 isTextured; [f32 ambient,specular,diffuse only in 0x303 files] },
                 [Texture(0x06)], Extension }
```

The texture name, e.g. `Name_917_0` or `tire`, matches a MATI name **case-insensitively** in the same file.
[V] Every texture resolves.

### 3.7 Skinning: the Bone plugin (0x10E)

```
entry[size/12] := { u32 firstVertex; u32 vertexCount; u32 boneId }
```

- [V] Each vertex belongs to exactly **one** bone (rigid skinning, weight 1.0).
- The used ranges are contiguous from 0 and cover all vertices.
- Entries come in authoring order, not frame order. Unused entries are all zero.
- `MikeLaird5` has 29 entries for 39 frames.
- **Bind pose:** the frame hierarchy as stored. The vertices are in model space, so the inverse bind matrix
  is `inverse(world(frame))`.

### 3.8 MDEF

```
0x00 u32 kind (2 riders, 0 others)   0x04 char[16] name   0x14 u32 nameId = zhash(name)
0x18 f32[4] params [I unknown]       0x28 u16 count       0x2A u16 unk
0x2C entry[count] = { char name[16]; u32 id }        (bones of riders / parts of bikes)
```

### 3.9 Shapes in the corpus [V]

| Model | Frames | Atomics | Notes |
|---|---|---|---|
| In-game riders `*1..5` | 20 | 1 skinned | MikeLaird5 has 39 frames |
| `DaveMirra.ZMD` | 17 | 1 | test asset, Y-up, lists |
| Bikes | 10-11 | 7 rigid | BackWheel, FrontWheel, Sprocket, Handlebars, 2 pedals, frame on the root `Hipsdeck` |
| `*_shadow` | | | prelit, untextured |
| `ShadowVolume.ZMD` | | | normals only (flags 0x10) |
| `sfx.zmd` | | | 20 textures |

Totals: 116 LODs, 410 geometries, 106,584 vertices, 96,018 triangles, 324 embedded textures.

## 4. Textures and materials: MATI

```
MATI := "MATI" u32 size { MADF, TBMP | TPNG | TDDS }
MADF (80-104 bytes, < 0x90):
  0x00 u32  nameOffset = 0x48 (the game asserts "MADF Mismatch" otherwise)
  0x04 u32  flags (0)   0x08 u32 (0)   0x0C u32 (0)            -> engine material +0x08/+0x0C/+0x10
  0x10 f32[3] 0.9 x3    0x1C f32[3] 0.5 x3    0x28 f32[3] 0.1 x3    0x34 f32 0.25    [V] never read by the game
  0x38 u32  surface word: bits 0-3 surface type [I]; bits 4-14 animated-material flags [I]
  0x3C u32  anim1   0x40 u32 anim2         (FAN1/2/3 animated fan textures only)
  0x44 u32  nameId = zhash(name)
  0x48 char name[] NUL, zero-padded to 4 (upper-case)
TBMP := "TBMP" u32 size { complete .bmp file, zero-padded to 4 }   image length = BMP bfSize
TPNG := "TPNG" u32 size { complete .png file, zero-padded to 4 }   image ends after IEND + CRC
TDDS := .dds (registered by the engine, never shipped)
```

### 4.1 Surface types [I]

Inferred from level texture names:

| Value | Surface | Value | Surface |
|---|---|---|---|
| 0 | default | 8 | rubber |
| 1 | metal | 9 | sand |
| 2 | wood | 10 | dirt |
| 3 | wall | 11 | water |
| 4 | brick | 12 | chain-link |
| 5 | grass | 13 | fence |
| 6 | ocean | 14 | foam |
| 7 | foliage | | |

ZMD textures are all 0.

### 4.2 Image rules [V]

- Images are decoded by RenderWare's BMP/PNG readers. PNG goes through libpng 1.0.5, which checks chunk CRCs.
- Any 4/8/24-bit uncompressed BMP loads in any BMP slot.
- **Colour key:** after decoding, every palette entry (4/8-bit) or pixel (truecolour) that equals RGB
  **(255, 0, 255)** becomes fully transparent.
- The game never resizes images. Every ZMD and level texture is square, from 2² to 512².

### 4.3 Formats in ZMDs [V]

324 textures in total:
- 212 BMP 24-bit
- 1 BMP 8-bit 64²
- 1 BMP 8-bit 32² (160 colours)
- 1 BMP 4-bit
- 109 PNG, all 8-bit RGBA, non-interlaced

### 4.4 Mat._sd

A level's `Mat._sd` is a flat sequence of `MATI` chunks with no padding and no other chunks. [V] It holds
872 MATI over 12 levels:
- 640 BMP 8-bit
- 229 BMP 4-bit
- 1 BMP 24-bit
- 2 PNG

`material.zal` holds 1,262 plain BMP files (no MATI): HUD and effect art, and low-colour copies of bike and
rider art.

## 5. Levels

### 5.1 The level ZSD (include file)

```
SCEN := "SCEN" u32 size { SCDF, SCFI x N }
SCDF := "SCDF" u32 16 { u8 0; u8 junk[15] }                    junk = uninitialised memory, keep it
SCFI := "SCFI" u32 40 {
          0x00 u8  load order: 1 Mat, 2 PropDef, 3 Tag, 4 BSP, 5 Prop, 6 Scene
          0x01 u8  junk[3]   (cc cc cc / ec 9a 00)
          0x04 u32 file size of the component
          0x08 char name[32]   e.g. "BSP._sd", NUL then junk/0xCC
        }
```

- [V] SCDF's size covers only its own 16 bytes, so the SCFI blocks are SCDF's **siblings** inside SCEN.
  There is no count field: N comes from SCEN's size.
- [V] All 12 levels list 6 components.
- **Integrity:** the game allocates and reads exactly `size` bytes of each component.
  - A component shorter than `size` → "Scene component file not found", and the level fails to load.
  - A longer one is silently truncated.
  - Update `size` after editing a component. [V] It equals the real size in all 72 entries.

### 5.2 Level folders [V]

Each level folder holds:
- `<name>.ZSD`
- `Mat._sd`, `PropDef._sd`, `Tag._sd`, `BSP._sd`, `Prop._sd`, `Scene._sd`
- sometimes `temp.dff`

The 12 level folders are four parks × three styles: Acclaim, Greenville, ProTour and Woodward, each with
Dirt, Street and Vert (e.g. `AcclaimDirt`, `WoodwardVert`).

The level list with display names and per-level ZAL names is in `tables.zal` (§7.2).

### 5.3 How a level loads [V]

`zsceneOpen` (0x004911C0) loads the components in a fixed order:
1. `Mat._sd` (textures);
2. `BSP._sd` (the RW World);
3. every RpMaterial is hooked to its MATI by texture name;
4. `Tag._sd`, `Scene._sd`, `PropDef._sd`, `Prop._sd`, through per-tag chunk handlers.

The handlers find sub-chunks **by tag** inside each form, and the order inside a form does not matter to
the engine. Fixed records start with a u32 equal to their own size; the loaders assert on it. Every chunk
payload must be a multiple of 4 bytes ("Data length not word size").

Corpus totals over the 12 levels:

| Data | Count |
|---|---|
| World sectors | 1,111 |
| World vertices / triangles | 136,864 / 62,094 |
| World materials | 670 |
| Prop definitions / props | 508 / 927 |
| Scene points / panoramas | 72 / 12 |
| Tag faces | 22,610 |

### 5.4 BSP._sd: the RenderWare World

```
World(0x0B) { Struct(52), MaterialList(0x08), <root sector>, Extension }
World struct: u32 rootIsWorldSector (0: the root is a PlaneSect); f32 invWorldOrigin[3]; f32 ambient, specular, diffuse;
              u32 numTriangles, numVertices, numPlaneSectors, numWorldSectors; u32 colSectorSize; u32 format
              format 0x4000003C: 0x04 textured, 0x08 prelit, 0x10 normals, 0x20 light (0x80 = 2 UV sets)
PlaneSect(0x0A) { Struct{ u32 axis (0 x, 4 y, 8 z); f32 value; u32 leftIsWorldSector; u32 rightIsWorldSector;
                          f32 leftValue; f32 rightValue }, <left sector>, <right sector> }
AtomicSect(0x09) { Struct, Extension{ BinMesh(0x50E) } }
AtomicSect struct:
  u32 matListWindowBase (0; 0xCDCD junk in empty sectors); u32 numTriangles; u32 numVertices (<= 256)
  f32 inf[3]; f32 sup[3] (sector box); u32 collSectorPresent; u32 unused
  f32 vertices[nv][3]                   absolute RW world space
  s8  normals[nv][4] (x, y, z /127, pad 0xCD)        if format & 0x10
  u8  prelit[nv][4]  RGBA baked lighting             if format & 0x08
  f32 uv[nv][2]      origin top-left                 if format & 0x04 (twice if 0x80)
  u8  triangles[nt][4] = { material, a, b, c }       material is relative to matListWindowBase; (a, b, c) is CCW
  u8  collision[384]                                 if collSectorPresent (RW 3.0 collision block, not decoded)
```

- [V] Sector normals (all 12 levels): every level has the normals flag, so every vertex has one
  (the sectors with none are the empty ones). They are s8/127, 0.996-1.019 long; 14 are (0, 0, 0)
  (AcclaimStreet 3, GreenvilleVert 3, ProTourStreet 5, WoodwardVert 3). With the CCW triangles they agree
  on 91-100% of the faces, except ProTourDirt and WoodwardStreet, where most floor normals point down
  (15% and 30% of the faces agree). The game lights the world with the prelit colours.
- Sectors are visited depth-first, left before right (the file order). The tree is up to 14 planes deep,
  so a reader needs to allow more than 16 levels of RW nesting.
- The world's MaterialList is the same structure as a model's (§3.6). Each slot's texture name matches a
  `Mat._sd` MATI case-insensitively, except one shipped data bug: AcclaimStreet `trees11_2` has no MATI.
- BinMesh holds the same triangles grouped per material, in the optimiser's order. RW renders from it, so
  a geometry editor must rebuild it.
- [V] 185 triangles repeat another triangle's vertex set (double-sided pieces).

### 5.5 PropDef._sd, Prop._sd, Scene._sd

Each file is a flat list of forms. A form's payload is a list of 4CC chunks.

```
PropDef._sd: PDBL{ PDIN, [OBJI], MODL }
  PDIN (0x38): u32 0x38; u32 id = zhash(name); char name[20]; f32 extents[3] (non-zero iff OBJI) [I box]; u8 junk[16]
  MODL: a complete model (§3) WITHOUT MATI; its textures come from Mat._sd by name. 1 atomic in all 508
Prop._sd: PRBL{ PRBL, [OBJI] }          (the outer form and the inner record share the tag PRBL)
  PRBL (0x48): u32 0x48; u32 defId (= a PDIN id); char name[16]; f32 m[9] (rows right, up, at; ZGE axes);
               f32 pos[3] (ZGE/100)
Scene._sd: SCPT{ SDPT, [OBJI] } x 6, SDPA{ MODL }
  SDPT (0x4C): u32 0x4C; u32 id = zhash(name); u32 type (1, 2, 3); char name[16]; f32 m[9]; f32 pos[3] (ZGE/100)
  SDPA: the sky panorama model (registered as zhash("Panorama")); placed at the world origin [I]
OBJI: u32 8; u32 n; u8 data[n]; pad to 4 -- object attributes, layout per class (props 10 bytes, defs 23/2,
      lights 4, Globals 5) [I]
```

- Prop placement: `R = prop_matrix_to_rw(m)` = the stored rows, `t = pos_to_rw(pos)` (§2.6), and each def vertex goes to
  `p · R + t`. [V] All 927 rotations are orthonormal with det +1; 923 are pure yaw.
- Scene points:
  - `Globals` (level settings in OBJI);
  - `Player` and `PlayerT` (rider start points) [I];
  - three `SpotLight`s (types 1-3).

  The spelling varies (`PLAYER`, `GLOBALS`). [V] The engine stores the matrix as −row0, row1, −row2.

### 5.6 Tag._sd: tagged collision faces

```
TAGB{ TAGD, TAGN x M, TAGF x faceCount, TAGI }
TAGD: u32 8; u32 faceCount
TAGF (0x50): u32 0x50; f32 v0[3], v1[3], v2[3] (ZGE, CCW against the normal); f32 normal[3];
             i32 faceTag; i32 edgeTag[3]; i32 vertexTag[3]           byte offsets into the TAGI blob, -1 = none
TAGI: u32 8; u32 dataSize; records in face order (per face: face, edges 0-2, vertices 0-2)
  face record (12): u16 code (bits 12-15 class, 0-11 id); u8 kind; u8 flags; u32 param; u32 extra (0)
  edge/vertex record (4): u16 code; u8 kind; u8 pad (0xCD)
TAGN (kd-tree node, pre-order, low subtree first): u32 0x28; u8 kind (1 leaf, 2 x, 4 y, 8 z); u8 junk[3];
     f32 split; f32 boundA[3]; f32 boundB[3]; u32 count; u32 faceIndex[count]
```

- [V] Tag faces are a collision copy of most of the world mesh: 92-100 % of their vertices lie on world
  vertices, and props are not included. The root box equals the world bounding box.
- [V] 4 faces in the corpus have **NaN normals** (degenerate triangles). Compare floats bitwise.
- [I] The code classes encode surfaces and trick/gap zones; the game code reads `code` and `flags` (NgIFzbsp, NgIFzcollide).

### 5.7 temp.dff

A bare RW Clump left behind by the exporter, in 8 of 12 folders. The game never loads it.

## 6. ZAL archives

```
ZAL     := Header GroupTable AssetData* Tail
Header  := u32 magic = 0x2A81511C; u8 reserved[12] (0); u16 flags; u16 count          (0x14 bytes)
flags   : bits 0-12 assets per group (always 1); bit 13 ref mode (unused); bit 15 compressed (a)
count   : bits 0-13 number of groups; bit 14 codec (0 = method 1, 1 = method 2 = LZO1X); bit 15 compressed (b)
comp    = (flags >> 15) * 2 + (count >> 15)          asset record = 3 words if comp != 0, else 2
Group   := u32 id; Asset[assets per group]
Asset   := u32 offset (relative to byte 0x10); u32 size (low 24 bits = uncompressed size); [u32 storedSize]
```

- [V] Data follows the table in table order. Each asset starts 4-byte aligned; the 0-3 alignment bytes
  are junk (`CD` …), so keep them when rewriting.
- **Headerless form** (the motion bank, §8): no magic or reserved bytes, `u16 flags, u16 count` at
  offset 0, and offsets relative to byte 0.
- [V] There is no checksum. The loader checks only the magic, the read sizes, and the decoded length
  against `size`.
- Codec method 1 is unused by the game files.

**LZO1X decompression** (method 2, standard LZO1X as in minilzo). A stream is a sequence of
instructions; `t` is the next byte.
- **First byte > 17:** copy `t − 17` literals. Then, if `t − 17` < 4, continue with a match; else with
  the "first literal run" state.
- **`t < 16` in the main loop:** a literal run of `t + 3` bytes. If `t == 0` the length extends: add 255
  per zero byte, then 15 + the next byte.
- **`t < 16` right after a literal run:** a 3-byte match at distance `1 + 0x800 + (t >> 2) + (next << 2)`.
- **`t ≥ 64`:** length `(t >> 5) + 1`, distance `1 + ((t >> 2) & 7) + (next << 3)`.
- **`t ≥ 32`:** length `(t & 31) + 2`, extended by zero bytes with base 31. Distance `1 + (b0 >> 2) + (b1 << 6)`.
- **`t ≥ 16`:** length `(t & 7) + 2`, extended with base 7. Distance `((t & 8) << 11) + (b0 >> 2) + (b1 << 6)`.
  A distance of 0 marks **end of stream**; otherwise add 0x4000.
- **`t < 16` after a match:** a 2-byte match at distance `1 + (t >> 2) + (next << 2)`.
- **After every match:** copy `ip[−2] & 3` literals. If that count is 0, read a new instruction;
  otherwise the next byte is a match instruction.

Matches may overlap their output (copy byte by byte).

| Archive | Groups | Contents [V] |
|---|---|---|
| Intwrap.zal | 5 | group 0: a nested compressed ZAL of 35 GUI resources (`SCRN`/`STRS` records); groups 1-4: 512² 8-bit BMP splash screens |
| material.zal | 1262 | BMP files (4/8-bit) |
| miscdata.zal | 21 | id 1: player table FORM (§7.1); 0x7531-0x7535: small binary tables (not decoded); 0x7919-0x791C and 0x7D01-0x7D0C: STRS string tables, in 4 language variants (§7.3) |
| motion.zal | 1 | motion bank (§8) |
| replay.zal | 24 | demo replay records, 2 per level (0x40-byte header + recorded input stream; not decoded) |
| tables.zal | 1 | level table FORM (§7.2) |

## 7. FORM tables and strings

```
FORM := "FORM" u32 size { chunk* }          4CC chunks of §2.1. No form-type word (unlike EA IFF).
```

### 7.1 Player table: FORM{PLHD, PLYR} (miscdata.zal group 1)

Decoded here. Loader: `NgIFzres_0046ba10` (NgIFzres.c, line 0x117).

```
PLHD := "PLHD" u32 8 { u16 count (60); u16 entrySize (24); u16 0; u16 6 }
        [V] the game asserts "Out of sync player data" unless the chunk is 8 bytes and entrySize == 0x18
PLYR := "PLYR" u32 count*24 { entry[count] }
entry (24 bytes):
  0x00 u16 refOffset   } a reference into an unshipped resource: refOffset[k+1] = refOffset[k] + refLength[k]
  0x02 u8  refLength   } [V] contiguous for all distinct entries; [I] target unknown
  0x03 u8  refFlags    (0x80 in all entries)
  0x04 u32 id          0x2C, 0x2D, ... consecutive per distinct entry; [I] meaning unknown
  0x08 char name[16]   rider's full name, e.g. "Dave Mirra"
```

- [V] **Entry index = rider × 5 + style.** `NgIFzres_0046bb00(index)` returns one entry and asserts
  "Invalid player" when `index ≥ count`. The overlay code (scoreboard) prints the name of entry
  `rider × 5`.
- Riders in order: Dave Mirra, Ryan Nyquist, Troy McMurray, Mike Laird, Chad Kagy, Tim Mirra, Kenan
  Harkin, Shaun Butler, Leigh Ramsdell, Joey Garcia, Slim Jim Guy, Amish Boy.
- The one-style riders (Slim Jim Guy, Amish Boy) repeat one entry five times.
- [V] Only `name` is read by the PC code. `ref*` and `id` are unused in this build.

### 7.2 Level table: FORM{ZMHD, ZMAP} (tables.zal group 0)

```
ZMHD := "ZMHD" u32 8 { u32 count (24); u32 recordSize (100) }
ZMAP := "ZMAP" u32 count*100 { record[count] }
record: 0x00 u16[4] (0, a, a, b) [I]; 0x08 u32[13] (1000, 10000, 20000, 5000, 0, 10000, 3, 15000, 4, 20000, 5, 25000, 6) [I score/goal thresholds];
        0x3C char name[20] ("ACCLAIM DIRT"); 0x50 char zal[20] ("ACCLDIRT.ZAL")
```

[V] The record is read by level number (`NgIFzres_0046b990`, 100 bytes). The per-level ZAL files named
here are not shipped with the PC build.

### 7.3 String tables: STRS

```
"STRS"; u16 baseTextId; u16 count; u16 offset[count] (from the start of the chunk); NUL-terminated latin-1 strings
```

- [V] Text id = baseTextId + index. Text ids ≥ 0x1000 select table `id >> 12`.
- Example: table 0x7919 (base 0) has 627 strings, with string 0x25E = "LOAD COMPLETE.".
- Each set of four groups holds the language variants: two English, one French, one German.

## 8. Animations: the motion bank (motion.zal)

### 8.1 Bank

[V] motion.zal holds 1 group (id 0), LZO1X-compressed, 202,116 bytes. Its payload is a **headerless ZAL**
(§6) with 238 groups, ids 1..238 = the motion numbers the game code uses. Each group is one clip. There
are no motion names in the files.

### 8.2 Clip

```
0x00 u16 frames | 0x8000 (100-byte block follows the sections; never set)
0x02 u16 flags: bits 4-7 number of sections; bit 15 = 16-bit angles (else 8-bit); all clips 0x9823
0x04 u32 frame time in ms (34)       0x08 u32 extra (0)
0x0C section[n] = { u8 channels; u8 flags (0x80; bit 1 = no init record); u16 kind (0 rider, else bike) }
then: one init record per section, delta records in time order, u32 0, u32 end time (= frames * 34)
```

Records start 4-byte aligned and are zero-padded to 4.

| Field | Present when | Notes |
|---|---|---|
| `u32 mask` | always | bits 28-31 = section; bit 0 = long header; bit 1 = translation; bit 2+i = channel i present |
| `u16 time` | always | ms |
| `u16 h; f32 value` | mask bit 0 | meaning open |
| `s16 translation[3]` | mask bit 1 | |
| `s16[3]` per present channel | | 8-bit mode: `s8[3]` for deltas, `u8[3]` for init values |

- **Rider init record** (fixed layout): `u32 mask, u16 time, u16 h, f32 value, s16 trans[3]`, then
  **every** channel.
- **Other init records** use the generic layout; mask bit 2 means "all channels".

### 8.3 Sampling (the game's decoder, `zactor_00472b10`) [V]

- **Init values are absolute.** Angles are binary s16 (65536 = 360°; 8-bit: `u8 << 8`). Translation is × 1/8.
- **Each frame k > 0:**
  - the records with `time == k × 34` replace their channels' **delta** (s16, or `s8 << 7`);
  - translation deltas are × 1/256;
  - then every channel adds its current delta.
- **Deltas persist** until a later record replaces them. An explicit (0, 0, 0) is how a rotation stops.
- **Translation:** the rider's x/z are per-frame root displacements and its y accumulates; the bike's
  translation accumulates. [I] These are world units, Y-down; the importers ignore root motion.
- **Euler → quaternion:** q = qz · qy · qx, with half angles `a × π / 65536`, layout (x, y, z, w) and the
  Hamilton product.

### 8.4 Channels → frames [V]

| Section | Channels, in order |
|---|---|
| Rider | Hips, Chest, Head, LeftUpArm, LeftLowArm, LeftHand, RightUpArm, RightLowArm, RightHand, LeftUpLeg, LeftLowLeg, LeftFoot, RightUpLeg, RightLowLeg, RightFoot |
| Bike | Hipsdeck, Handlebars, Sprocket, RightPedal, LeftPedal, BackWheel, FrontWheel, LeftGrip, RightGrip, SeatGrip (clips use the first 1-5) |

- A channel finds its frame by `zhash(name)` = the frame's Animation bone id.
- The channel **replaces** the frame's local rotation; the bind translation stays.
- The Hips channel stands the rider up in Y-up space, so **drop the rider upright correction (§2.5) when
  animating**.
- **Arm corrections** (NgIFzactor, literal), with Cz(±90°) = 0x4000 about Z:

  | Channel | q' |
  |---|---|
  | LeftUpArm | q · Cz(+90) |
  | RightUpArm | q · Cz(−90) |
  | LeftLowArm, LeftHand | Cz(−90) · q · Cz(+90) |
  | RightLowArm, RightHand | Cz(+90) · q · Cz(−90) |
  | all others (bike channels too) | q |

- **Engine → model axes** (every channel, rider and bike): the frame's local rotation is the quaternion
  **(x', −y', −z', w')**, i.e. F·R(q')·F with F = diag(1, −1, −1). [V] `zrwModelQuatToRwMatrix`
  0x00487E70 does this when the model update writes the frames. With it the hands land on the
  handlebars (median hand to bar end 0.16, feet to pedals 0.10); without it 0.69. Older tools
  used an empirical Rx(180) before the upper arms instead, with no F: do not combine the two.
- **Mirroring** (motion flag 8) swaps left and right channels and negates y/z.

## 9. Sound banks: ZSND (`bin\audio\*.zsd`)

```
0x00 "ZSND"  0x04 "WIN "  0x08 u32 file size  0x0C u32 end of header area (samples start at align4)
0x10..0x47: six (u32 count, u32 offset) table pairs  A voices(20 B), B sample defs(24 B), C sample files(8 B: u32 offset, u32 size),
            D (empty), E ZTRK tracks(16 B), [0x38/0x3C: 0, end of E], F (empty)
B sample def: u16 sampleFile (0xFFFF none); u16 flags (bit 0 loop); u32 rate Hz; u32 runtime ptr (0);
              u32 loopStart; u32 loopEnd (frames, inclusive; 0 = whole); u16 0; u16 runtime handle (0)
```

- [V] **Samples** follow back to back in table C order. sfx banks hold **signed 16-bit mono PCM** at the
  def's rate.
- `music.zsd` (no sample defs) holds **IMA-ADPCM mono 16-bit 44,100 Hz** streams: low nibble first,
  predictor 0, step index 0, no block headers.
- There are no checksums.

## 10. SAVE.DAT

[V] The file is fixed-size, 0x235C (9,052) bytes: a raw dump of 3 memory blocks. There is no CRC. The game
checks only `u32 @4 == 0x235C`.

| Offset | Size | Content |
|---|---|---|
| 0x0000 | 4 | magic `0x2A81511C` (not checked) |
| 0x0004 | 4 | size `0x235C` (checked) |
| 0x0008 | 0xC3C | **options**: u16 cheats unlocked, u16 cheats active, u8 vol, u8 vol, u8 music track, u8 riders unlocked |
| | | **12 rider careers × 0x104** at 0x10: u32 flags, u8 levels/styles/bikes unlocked, goal info, competition results, 88-bit trick set |
| | | u8 language at 0xC40 |
| 0x0C44 | 0x1680 | **records:** 12 levels × 24 × 20-byte entries {u32 value; u8 rider; u8; u8; char name[13]} |
| 0x22C4 | 0x98 | **2 profiles × 0x4C:** u8 flags, u8 index, u8, u8 rider, u8 style, u8 bike, u8 camera, char name[29], u32 controls[7] |

The full field list is in `docs/formats/SAVE.md`.

## 11. Open questions

1. MLDF flags (7 / 3 / 0x1B); MDEF params f32[4] and unk u16.
2. MADF surface/animation bit layout; the surface enum in the exe.
3. PLYR `ref*` (which pool the offset/length point into) and `id`.
4. ZMAP's u16/u32 header words.
5. miscdata.zal tables 0x7531-0x7535; replay.zal input stream; Intwrap SCRN records.
6. Motion record `h`/`value`; root-motion units.
7. Level details: the 384-byte RW collision block per sector, `colSectorSize`, OBJI layouts, tag code
   meanings, PDIN extents, `PlayerT`/SpotLight semantics, and how the panorama is placed (see docs/formats/LEVEL.md).

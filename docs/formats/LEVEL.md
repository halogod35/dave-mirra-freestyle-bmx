# LEVEL: level folders (`<name>.ZSD`, `BSP/Mat/PropDef/Prop/Scene/Tag._sd`, `temp.dff`)

Owner: DM-013 (Agent C2). Code: `tools/dmbmx/formats/level.py` (ZSD, PropDef, Prop, Scene, Tag, temp.dff, `Level`),
`tools/dmbmx/formats/rwworld.py` (BSP world), `tools/dmbmx/formats/mati.py` (Mat._sd, DM-014), `tools/dmbmx/formats/zmd.py`
(embedded `MODL` models, RW materials), `tools/dmbmx/convert/level_export.py` (OBJ/glTF), CLI `dmbmx level`.
Tests: `tests/test_level_*.py`, `tests/test_cli_level.py`. Blender check: `tools/blender/scripts/level_check.py`.
Play-test generator: `tools/testing/playtest/make_playtest_dm013.py`.

Evidence tags (CONTRIBUTING.md AD-5): **[V]** verified by code over all 12 level folders and/or by the disassembly
(VA given, 2000-11-22 exe), **[I]** inferred. All integers are little-endian. "ZGE" = the engine's own
coordinate space (section 2).

## 0. Summary for importer writers (DM-060)

- [V] A level is **one RW world** (`BSP._sd`, static geometry + textures) + **placed props** (`Prop._sd`
  instances of `PropDef._sd` models) + **tagged collision faces** (`Tag._sd`) + **scene points**
  (`Scene._sd`: player start, lights, globals) + **textures** (`Mat._sd`, MATI, see TEXTURE.md).
- [V] Every file round-trips byte-identically through the decoded object model (12/12 folders, 80 files).
- [V] Units: 1 RW unit ≈ 1 m (riders are ~1.95 units tall). RW space is Y-up. The engine works in
  ZGE units = 100 × RW with Y and Z negated around the world origin (section 2).
- [V] Writing any `_sd` requires updating its size in the `.ZSD` (`Level.files()` does it).
- [V] Every `_sd` chunk payload must be a multiple of 4 bytes (the engine's IFF reader asserts).

| File | SCFI type | Content | Reader | Count / sizes (12 levels) |
|---|---|---|---|---|
| `<name>.ZSD` | – | component index | `level.Zsd` | 12 files, 320 bytes each |
| `Mat._sd` | 1 | flat list of MATI (texture + material def) | `mati.MatSd` | 872 MATI |
| `PropDef._sd` | 2 | prop definitions: `PDBL{PDIN, OBJI?, MODL}` | `level.PropDefSd` | 508 defs, 57–222 KB |
| `Tag._sd` | 3 | collision faces + tags + kd-tree: `TAGB{…}` | `level.TagSd` | 22,610 faces, 3,032 nodes |
| `BSP._sd` | 4 | RenderWare 3.0 World (RW 0x0B) | `rwworld.World` | 1,111 sectors, 136,864 verts, 62,094 tris, 670 materials |
| `Prop._sd` | 5 | prop instances: `PRBL{PRBL, OBJI?}` | `level.PropSd` | 927 props |
| `Scene._sd` | 6 | `SCPT{SDPT, OBJI?}` × 6 + `SDPA{MODL}` | `level.SceneSd` | 72 points, 12 panoramas |
| `temp.dff` | – | bare RW Clump, **not loaded** | `level.TempDff` | 8 of 12 folders |

## 1. Load sequence in the exe [V]

```
zsceneOpen(name)                       0x004911C0  "There is already a scene open", "Scene not found %s"
  zfile load <dir>/<name>.ZSD          (the SCENE entry's FILE from Scene.Txt: e.g. "AcclaimD.ZSD")
  zrwWorldOpen(zsd, dir)               0x00496FF0   (g_zrwWorldApi.open; DM-040 called it zrwWorldHookRpMaterials)
    zrwWorldLoadMaterials (inlined)    0x004963F0   Mat._sd: zrwWorldFindScfi(zsd, 1, &size) 0x004962E0,
                                                    ziffOpen(file, -1, size); MATI handlers (zrwMat)
    zrwWorldLoad(zsd, dir)             0x004966E0   BSP._sd: FindScfi(zsd, 4, NULL) (size NOT used),
                                                    RwStreamOpen(file) -> RwStreamFindChunk(0x0B) -> RpWorldStreamRead
    zrwWorldHookRpMaterials (inlined)  0x004964F0   hook every RpMaterial to its MATI by RwTexture;
                                                    "zrwWorldHookRpMaterials: Failed to match RpMaterial:%s" if missing
  zsceneLoadComponent(zsd, size, dir, type, nHandlers, handlers)   0x00490790
    type 3 Tag._sd      handlers 0x00561900  { 'TAGB' -> ztaginfLoad   0x0048DAE0 }
    type 6 Scene._sd    handlers 0x00561908  { 'SCPT' -> zscnptsLoad   0x00491F50, 'SDPA' -> zpanoramaLoad 0x004A10A0 }
    type 2 PropDef._sd  handlers 0x00561918  { 'PDBL' -> zpropLoadDef  0x00486A20 }
    type 5 Prop._sd     handlers 0x00561920  { 'PRBL' -> zpropLoad     0x00486BE0 }
```

- [V] `zsceneLoadComponent` finds the SCFI of the given type, skips it if its size is 0, builds
  `<dir>\<name>` and calls `zfileLoad(path, size)` 0x00485070: allocates `size` bytes and reads exactly
  `size` bytes. A shorter file → NULL → assert **"Scene component file not found %s"** (zScene.c line 0x2EE);
  a longer file is silently truncated. It then opens the buffer as a RAM file and an IFF reader (`ziffOpen`),
  and for each handler walks every top-level chunk with that tag (`ziff` find/next), calling the handler.
- [V] Each handler re-enters the form (`ziff` descend 0x0049B130), finds its sub-chunks **by tag**
  (search from the start of the form, 0x0049AD90; order inside a form does not matter to the engine),
  reads fixed-size records and asserts on the first u32 (record size).
- [V] Chunk sizes must be multiples of 4: ziff asserts **"Data length not word size for header %c%c%c%c"**
  (0x0049ADF9 / 0x0049B003) otherwise.
- [V] Mat._sd: the file is opened with `zfileOpen` and its SCFI size is passed to `ziffOpen` as the IFF
  length ([I] bytes past it are not read; a size larger than the file runs the reader off the end). A size
  of 0 skips Mat._sd. BSP._sd: FindScfi is called with a NULL size pointer, so its size is **not used** (RW
  opens the file by name) [V].
- [V] (DM-043) The ZSD comes from `Scene.Txt`: zsceneInit 0x00490FF0 parses it (zsceneLoadDatabase
  0x004904E0) into `SCENE_COMMON` (rider/bike MODELs, the `MainBank` ANIM = `Assets\Motion.ZAL`) and 14
  `SCENE { NAME "<Park> <Dirt|Street|Vert>" FILE "Assets\<folder>\<name>.Zsd" }` blocks. The game calls
  `zsceneOpen(levelNames[g_currentLevel])` with the 14-entry name table at 0x0055BC44 (Greenville Dirt
  ... Acclaim Vert, then "Test A"/"Test B", which both reuse GrenVert.Zsd); names match by zHashName.
  So a Dirt/Street/Vert variant is simply another SCENE name (tests/test_decomp_dm043.py ports the
  parser and resolves all 14 names to the 12 ZSDs).
- [V] (DM-043) A failing Tag/Scene/PropDef/Prop component only prints "zrwWorld: Failed to load scene
  tagging info" / "... scene point" (the latter also for PropDef and Prop); zsceneOpen still returns 1.
  A missing ZSD or a failing world open asserts and returns 0. C: decomp/src/NgEngine/Zge/zScene.c,
  Com/ZglRw/zrwWorld.c.

## 2. Coordinate systems [V]

```
RW world space (BSP._sd vertices, PropDef MODL vertices):  Y up, 1 unit ~ 1 m
origin o = -World.invWorldOrigin                              (World struct, section 4.1)
ZGE (engine) = 100 * (x - o.x, -(y - o.y), -(z - o.z))        scale 100.0 set by NgApp via zrwZgl(1, 100.0)
```

| Data | Stored in | To RW world space | Evidence |
|---|---|---|---|
| Tag faces, tag-tree bounds/splits | ZGE units | `o + (x, -y, -z) / 100` (`zge_to_rw`) | [V] tag root box == world bbox (12/12); 92.2–100 % of tag vertices coincide with world vertices |
| Prop / scene-point position | ZGE / 100 | `o + (x, -y, -z)` (`pos_to_rw`) | [V] zpropLoad 0x00486BE0 and zscnptsLoad 0x00491F50 multiply it by the world scale |
| **Prop** rotation (9 f32 rows) | – | **the stored rows unchanged** (`prop_matrix_to_rw`, DM-065) | [V] (DM-043) the stored matrix → euler → quaternion chain is the identity: zpropLoad copies the rows into a 4x4 without transposing, zmathMatrixToEuler 0x00481FC0 extracts x = atan2(m6, m10), y = atan2(−m2, ‖m0,m1‖), z = atan2(m1, m0), zquatFromEuler builds q = qz·qy·qx, and zquatToMatrix of it is the same row matrix. [V] (DM-065) the model's RW frame is then built by **zrwModelQuatToRwMatrixMirror** 0x00487FC0, not 0x00487E70, because ngappInit 0x0045B270 sets ZGL value 0x10 = 1 (g_zrwZglFlags bit 1, tested by zrwModelUpdate 0x00488650). Mirror = D·(F·R(q)·F)·D with D = diag(−1, 1, 1); D·F = −I, so the RW rotation is the stored M. See 2.1 |
| Scene-point rotation (9 f32 rows) | ZGE axes | `F·M·F`, F = diag(1, -1, -1) (`matrix_to_rw`) | [I] the rows are only used as ZGE direction vectors (5.3), which map to RW by F; scene points are never rendered |

### 2.1 Prop rotation: the evidence (DM-065)

Before DM-065 props used `F·M·F` like scene points, which gives the opposite yaw sign. A tester saw it in
3ds Max ("+80 shows as −80; flipping the sign fixes it"). The fix is pinned by `tests/test_3dsmax_dm065.py`:

- [V] exe: ngappInit pushes `(0x10, 1, 0)` and calls ZGL slot 7 (`[0x00561484]` = zrwZglSetValue
  0x0048AB10), whose case 0x10 sets g_zrwZglFlags (0x00697728) bit 1. zrwModelUpdate tests that bit and calls
  the mirror converter for the model's root frame. The mirror converter writes the same nine terms as
  zrwModelQuatToRwMatrix with right.y, right.z, up.x and at.x negated (`fchs` before exactly those stores).
  The animation parts (zrwModelPoseParts) always use the plain converter, so motion is unaffected.
- [V] Ported chain (zmathMatrixToEuler → zquatFromEuler → mirror converter): every one of the 927 props gets
  its stored rows back (1e-5); 300+ of them differ from `F·M·F` by more than 0.05.
- [V] Data cross-check, independent of the render path: zcolBuildPropRails 0x0046A4D0 builds each prop's
  collision box in ZGE space from the def's extents/extra and the prop's euler angles (zmathVecRotateXYZ,
  whose Y rotation has the opposite sense to zquatFromEuler's). On the 96 props with a box and a yaw that is
  not a multiple of 90°, the box corners land on the placed model's vertices (median 1.7 cm horizontally)
  with the stored rows, and metres away with `F·M·F` (median 1.36 m); 94 of 96 favour the stored rows.
- Consequence for editors: a prop's PRBL matrix is written as the RW rotation rows directly. `F·M·F` stays
  correct for vectors (tag faces, scene-point directions).

- [V] `World.bbox()` of AcclaimDirt = (124.0, 58.9, 168.9)…(248.0, 117.8, 337.8), origin (168, 55.545, 193.84);
  the tag root box (−4400, −335.7, 2492)/(8000, −6226, −14400) maps exactly onto it.
- [V] Matrices are stored as rows **right, up, at** (RW row-vector convention: `p' = p·M + t`). As a
  column-major 4x4 (glTF) the columns are right, up, at, position.
- [V] F is a proper rotation (180° about X), so winding/handedness is preserved; the RW world is
  handled as right-handed Y-up (as in ZMD.md / the ZMD exporter), and the level renders un-mirrored in
  Blender ("WOODWARD" signage reads correctly).

## 3. ZSD: `<name>.ZSD` [V]

```
SCEN  u32 size = 16+8 + N*(0x28+8)   (= 312 for N = 6; file = 320 bytes, nothing after SCEN)
  SCDF  u32 16    payload[16]
  SCFI  u32 0x28  payload[0x28]      x N   (siblings of SCDF inside SCEN)
```

Logically the SCFI records belong to SCDF ("definition"), but SCDF's size covers only its own 16 bytes
and only SCEN's size covers the SCFI chunks. There is no count field; N = the number of SCFI chunks
inside SCEN (6 in every level).

**SCDF payload (16 bytes)**

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u8 | flag | 0 in all 12 [V] |
| 0x01 | u8[15] | junk | `0xCC` fill or stale memory (5 variants) [V]; **never read** by the exe (no "SCDF" constant anywhere in the image [V]; DM-043: zrwWorldFindScfi walks only the SCFI children of SCEN) |

**SCFI payload (0x28 bytes)**: [V] `zrwWorldFindScfi` 0x004962E0 asserts **"Chunk out of sync"** unless the size is 0x28

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u8 | type | component type: 1 Mat, 2 PropDef, 3 Tag, 4 BSP, 5 Prop, 6 Scene [V]. The only field matched by FindScfi |
| 0x01 | u8[3] | junk | uninitialised (`cc cc cc`, `ec 9a 00`, `00 00 00`, …) [V]; not read; preserve |
| 0x04 | u32 | size | byte size of the component file [V 72/72 equal the real sizes]; see section 1 for how it is used |
| 0x08 | char[32] | name | file name relative to the ZSD folder, NUL-terminated, junk after the NUL (preserve) [V] |

File order of the SCFI records varies per level (e.g. AcclaimDirt: BSP, Mat, Tag, Prop, PropDef, Scene);
the load order is fixed by the exe (section 1), not by the file.

## 4. BSP._sd: RenderWare 3.0 World [V]

RW chunk = `u32 type; u32 size; u32 version` (12-byte header, size excludes it). Version 0x00000301 on every
chunk. No GeometryList, no native data; standard RW 3.0 world stream.

```
World(0x0B)
  Struct(0x01)          52 bytes, 4.1
  MaterialList(0x08)
    Struct              u32 n; i32 index[n]           (all -1: one Material per slot)
    Material(0x07) x n  same layout as in ZMD (ZMD.md 3.3): Struct(16) [+ Texture(0x06){Struct(u32 0x106), String name, String mask, Extension}] + Extension
  PlaneSect(0x0A) | AtomicSect(0x09)                  root sector (World.rootIsWorldSector picks; always PlaneSect)
  Extension(0x03)       empty
PlaneSect(0x0A)
  Struct                24 bytes, 4.2
  <left sector> <right sector>                        PlaneSect or AtomicSect each, as the struct's flags say
AtomicSect(0x09)
  Struct                44-byte header + arrays, 4.3
  Extension(0x03)
    BinMesh(0x50E)      always (also in empty sectors: flags 0, 0 meshes)
```

### 4.1 World struct (52 bytes)

| Off | Type | Name | Corpus |
|---|---|---|---|
| 0x00 | u32 | rootIsWorldSector | 0 (root is a PlaneSect) |
| 0x04 | f32[3] | invWorldOrigin | −origin; e.g. (−168, −55.545, −193.84) |
| 0x10 | f32[3] | surface ambient, specular, diffuse | (0.6, 0.0, 0.2) |
| 0x1C | u32 | numTriangles | = Σ sector triangles [V] (recomputed on write) |
| 0x20 | u32 | numVertices | = Σ sector vertices [V] |
| 0x24 | u32 | numPlaneSectors | = PlaneSect count [V] |
| 0x28 | u32 | numWorldSectors | = AtomicSect count [V] (= planes + 1) |
| 0x2C | u32 | colSectorSize | 5516–9696 = Σ over sectors of the collision tree's used bytes (3 × highest reachable node + 3) rounded up to 4 [V 12/12; RW writer 0x004C3AE0]. The world reader 0x004C3E60 allocates exactly this much for the trees, so it is **recomputed on write** (DM-049, `rwcoll.col_sector_size`) |
| 0x30 | u32 | format | 0x4000003C: 0x04 textured, 0x08 prelit, 0x10 normals, 0x20 light; 0x40000000 is ORed in by RW 3.1's world writer 0x004C3AE0 [V] (kept). The reader honours 0x04/0x08/0x10/0x80 |

### 4.2 PlaneSect struct (24 bytes)

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | type | split axis as a byte offset into a V3d: 0 = x, 4 = y, 8 = z [V] |
| 0x04 | f32 | value | split plane (RW units) |
| 0x08 | u32 | leftIsWorldSector | 1 → left child is an AtomicSect [V] |
| 0x0C | u32 | rightIsWorldSector | 1 → right child is an AtomicSect |
| 0x10 | f32 | leftValue | extent of the left child along the axis: ≥ every left vertex [V 1099/1099]. RW's world walk 0x004BD400 visits the left side only where a segment reaches ≤ leftValue (it never reads `value`) [V]; the writer raises it when an edited sector's vertices go further (DM-049) |
| 0x14 | f32 | rightValue | extent of the right child: ≤ every right vertex [V]; visited where a segment reaches ≥ rightValue; lowered by the writer like leftValue |

[V] Tree depth ≤ 14; children follow the struct in the order left, right.

### 4.3 AtomicSect (world sector) struct

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | matListWindowBase | 0 in all non-empty sectors; `0x0000CDCD` (heap fill) in the 327 empty ones [V] |
| 0x04 | u32 | numTriangles | nt |
| 0x08 | u32 | numVertices | nv ≤ 255 in the corpus; the format allows ≤ 256 (u8 indices) |
| 0x0C | f32[3] | inf | sector box minimum (not tight) [V: contains all vertices] |
| 0x18 | f32[3] | sup | sector box maximum |
| 0x24 | u32 | collSectorPresent | 1 in all 784 non-empty sectors, 0 in empty ones [V] |
| 0x28 | u32 | unused | 0 |
| 0x2C | f32[nv][3] | vertices | RW world space (absolute) |
| … | s8[nv][4] | normals | x, y, z / 127 + 1 pad byte (`0xCD` everywhere) — if format & 0x10 |
| … | u8[nv][4] | prelit | RGBA vertex colours (baked lighting) — if format & 0x08 |
| … | f32[nv][2] | uv | texture coordinates, origin top-left — if format & 0x04 |
| … | u8[nt][4] | triangles | `material, a, b, c`: material is relative to matListWindowBase; (a, b, c) is **counter-clockwise** against the vertex normals [V 10,279 vs 18 in two levels] |
| … | u8[384] | collision | RW collision tree, exactly 384 bytes when collSectorPresent [V 784/784]: 128 nodes x 3 bytes, implicit binary tree (root 1, children 2i / 2i+1); byte0 bit 7 split, bits 5-6 axis, bits 0-4 triangle count; byte1 plane vertex; byte2 first triangle [V] DM-048 (RW 0x004BE560 / 0x004BED60, all 784 sectors consistent: `dmbmx.formats.rwcoll`, docs/re/physics.md 3). Only the first 3 × (highest reachable node) + 3 bytes are loaded (RW 0x004BB760); the rest is junk in 783/784 sectors [V]. Kept raw; **rebuilt on write when the sector's geometry changed** (DM-049, physics.md 3.1) |

**BinMesh (0x50E)** [V]: `u32 flags (0 = triangle lists); u32 numMeshes; u32 totalIndices;` then per mesh
`u32 n; u32 material (absolute world material index); u32 index[n]`. It contains exactly the sector's
triangles grouped by material, but in an optimiser's order (not reproducible from the triangle list).
RW renders from the BinMesh, so a geometry edit must rebuild it: `AtomicSector.rebuild_bin_mesh()`.
[V] DM-048: **the game collides against this block** (every NgIFzcollide segment test goes through RW's
per-sector tree; Tag._sd only supplies the tag of the face hit). The tree references triangles by index,
so a geometry edit that adds, removes or re-orders triangles must rebuild the block (and sort the
triangles so each node's are contiguous); `dmbmx.formats.rwcoll.check(sector)` reports a stale tree.
[V] DM-049: the dmbmx writer does that automatically (`rwcoll.build`, section 8); the shipped trees
cannot be reproduced byte for byte (0/784: the vertex arrays were renumbered after RW built them, and
the build-time coordinates differed slightly), but the rebuilt trees answer line and sphere queries
exactly like the shipped ones (docs/re/physics.md 3.1).

**Vertex order** [V 784/784]: a sector's vertices are grouped by material and, within a material, in
first-use order of the stored triangles.

- [V] **Duplicate faces:** 185 triangles in the corpus repeat another triangle's vertex set in the same
  sector and material (double-sided pieces); Blender's importer merges them.
- [V] **Textures:** each world material's texture name matches a MATI in the level's `Mat._sd`
  case-insensitively — except one shipped data bug: AcclaimStreet's `trees11_2` has no MATI.

## 5. PropDef._sd / Prop._sd / Scene._sd: Z-Axis IFF forms [V]

4CC chunk = `char tag[4]; u32 size; payload[size]` (size excludes the header, multiple of 4). Each file is a
flat list of forms; a form's payload is a list of chunks. Fixed-size records start with a u32 equal to
their own size (the loaders assert on it). Names are fixed-size NUL-terminated fields with junk after the
NUL (preserved by `name_raw`).

### 5.1 PropDef._sd: `PDBL{ PDIN, [OBJI], MODL }` (zpropLoadDef 0x00486A20)

Order in the corpus: `PDIN, MODL` (365) or `PDIN, OBJI, MODL` (143). The handler asserts
"Prop Def has no data" (no PDIN), "Prop Def header size invalid" (≠ 0x38), "Prop Def has no geometry" (no MODL).

**PDIN (0x38 bytes)**

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | size | 0x38 |
| 0x04 | u32 | id | `zhash(name)` [V 508/508]; props reference it; the def is registered as a scene model with this id (zsceneAddObject 0x00490710) |
| 0x08 | char[20] | name | e.g. `Railbase`, `palmtrnk`, `Door`; junk after the NUL |
| 0x1C | f32[3] | extents | non-zero exactly when the def has an OBJI [V 508/508]; collision box size in RW units (x width, y height, z depth): [V] zbspInitPropDefShape builds the def's collision record as (x·50, y·100, z·50) = half-width, height, half-depth in ZGE units |
| 0x28 | f32[4] | extra | (was "junk") [V] DM-043: four floats, copied to ZpropDefInfo.extra; zbspInitPropDefShape reads [1] and [3] for the box's vertical offset ((y·0.5 + extra[1] + extra[3])·100). With OBJI they are a box-centre offset in ZGE axes (e.g. palmtrnk_B (0.05, −4.0, −0.03, 0) for height 8); without OBJI they are stale pointers and never read (the collision record is only built for defs with an OBJI) |

**MODL**: a complete ZMD model (`MODL{ MLOD{MLDF, MDFF{RW Clump}}, MDEF }`, ZMD.md) **without MATI**:
textures come from `Mat._sd` by name. [V] 508/508 have one atomic. Read with `zmd.Zmd.read`.
The MDEF name differs from the PDIN name in 143 defs [V] (harmless; the PDIN name is used).

### 5.2 Prop._sd: `PRBL{ PRBL, [OBJI] }` (zpropLoad 0x00486BE0)

The outer form and the inner record share the tag `PRBL`. 367 props without OBJI, 560 with.
Asserts: "Prop has no data", "Prop header size invalid" (≠ 0x48), "No prop def corresponds to prop" (unknown id).

**Inner PRBL (0x48 bytes)**

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | size | 0x48 |
| 0x04 | u32 | def_id | PDIN id [V 927/927 resolve] |
| 0x08 | char[16] | name | always the def's name [V]; junk after the NUL |
| 0x18 | f32[9] | matrix | rows right, up, at; **the RW rotation as is** (2.1, DM-065); orthonormal, det +1 [V 927/927]; 923 are pure yaw |
| 0x3C | f32[3] | pos | ZGE/100 units (section 2) |

[V] The handler multiplies `pos` by the world scale, converts the matrix to euler angles (0x00481FC0) and a
quaternion (zquatFromEuler) and places a new scene object created from the def's model (zScene 0x00491410).

### 5.3 Scene._sd: `SCPT{ SDPT, [OBJI] }` × 6 + `SDPA{ MODL }` (zscnptsLoad 0x00491F50, zpanoramaLoad 0x004A10A0)

Every level has 6 points: `Globals`, `Player`, `PlayerT` (type 1) and three `SpotLight` (types 1, 2, 3),
plus one `SDPA` [V]. Spelling varies (one level `GLOBALS`, two `PLAYER`/`PLAYERT`).

**SDPT (0x4C bytes)**: asserts "Point has no data", "Point data size mismatch" (≠ 0x4C)

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | size | 0x4C |
| 0x04 | u32 | id | `zhash(name)` [V 72/72] |
| 0x08 | u32 | type | 1, 2, 3 (SpotLight number) [V] |
| 0x0C | char[16] | name | junk after the NUL (`ff`, …) |
| 0x1C | f32[9] | matrix | rows; [V] stored by the engine as −row0, row1, −row2 |
| 0x40 | f32[3] | pos | ZGE/100 units; [V] multiplied by the world scale |

[V] (DM-043) How the game uses the points (zscnptsFind(zhash(name), type) 0x00492130):
- `Player` (type = player number + 1) = rider start position; `PlayerT` (same type) = its **target**:
  zrollerGetPlayerStartPos 0x0046CF30 sets the start heading to the yaw from Player to PlayerT (x/z
  plane). Without a PlayerT the heading comes from the Player matrix. Without a Player it asserts
  "Player start point not found" and uses a hard-coded start.
- `SpotLight` type 1..3 = the three directional light slots (ngappCreateLevelLights); its 4-byte OBJI is the
  light colour (RGBA bytes / 255, zrwLight 0x0049F550). An optional `SpotLightT` of the same type is the
  target: direction = normalize(T − S); without it the direction is the stored matrix's (negated) right
  row. A SpotLight without OBJI makes no light. There is also one ambient light with a constant colour.
- `Globals` type 1: the first 4 bytes of its OBJI are the camera clear (background) colour RwRGBA
  (zrwZgl setting 4 -> 0x00697730, used by RwCameraClear); the 5th byte is not read there.
- The engine stores the matrix as −row0, row1, −row2 (diag(−1, 1, −1)·M, not F·M·F) and pos·100;
  the name is not kept.

**SDPA**: one `MODL` (a ZMD model without MATI): [V] loaded by zpanoramaLoad 0x004A10A0 and registered
as a scene *template* with id `zhash("Panorama")` (0x00410A3B); there is no placement data. [V] DM-043:
nothing in the exe reads it afterwards (zpanoramaGet 0x004A1090 has no caller, pointer or thunk; the
hash constant does not occur in the image; "Panorama" is the only such literal), so [I] the shipped game
never instantiates or draws the panorama (unverified in game). Its textures are in `Mat._sd`.

### 5.4 OBJI: object attribute block (all three files)

```
OBJI  u32 size = 8 + n + pad
  u32 hdr  = 8          [V] all 751 (560 prop, 143 prop def, 48 scene point)
  u32 n                 data length
  u8  data[n]
  u8  pad[(-n) & 3]     0 in the corpus
```

[V] The handlers copy `data` to a heap block (prop +0x24, prop def +0x28, scene point +0x48). The
layout is per object class (game code); observed:

| Owner | n | Observation |
|---|---|---|
| Prop | 10 | [V] DM-052 (docs/re/props-replay-fx.md 2): bytes 0–3 = RGBA **tint** (ngappApplyPropTints: alpha forced to 0xFF, set on the model's materials via 0x00488CA0; skipped if all 0); bytes 4/5/6 bit 0 -> collision flags 0x800/0x1000/0x400 (0 in all 560); **byte 7 = group id** (bits 0-4: goal prop groups; whole byte: prop.c table slots of types 0..3); **u16 at +8 = switch id** (model pairs / breakables: `id` shown before, `id + 1` after; bmxpropSetState / bmxpropShowPair) |
| PropDef | 23 (141), 2 (`ANIM`) | **byte 0 bit 0 = grind rails** [V] DM-052: zrollerLoadAssets builds rail edges from the collision box (engine class 2: 4 top edges, 3/4: one; engine class 1 would assert "Not coded", never set with file class 3 in the corpus); **byte 22 = collision class** [V] zbspInitPropDefShape: 1→2, 2→3, 3→1, 4→4 (engine class), 0 = no collision box; corpus 45×1, 71×2, 1×3, 11×4, 13×0 |
| Scene `SpotLight` | 4 | [V] DM-043: RwRGBA light colour (r, g, b, a bytes / 255), e.g. `4d 4d 43 bf` |
| Scene `Globals` | 5 | [V] DM-043: bytes 0–3 = camera clear colour RwRGBA (e.g. `be b2 8e bf`); byte 4 not read by NgApp |

## 6. Tag._sd: tagged collision faces [V]

```
TAGB
  TAGD  u32 8; u32 faceCount                                     "Tag data not found" / "Chunk sync error"
  TAGN  x M    kd-tree nodes, pre-order (node, low subtree, high subtree)
  TAGF  x faceCount  0x50 bytes each                              "Tagging info face structure mismatch"
  TAGI  u32 8; u32 dataSize; records[]                            tag records (face/edge/vertex)
```

ztaginfLoad 0x0048DAE0 reads TAGD, then TAGI (only if faceCount ≠ 0, as a blob), allocates
faceCount × 0x6C bytes, reads **every** TAGF chunk into it (no bound check against faceCount: more TAGF
chunks than faceCount overflow the heap block — the writer always writes the real count), then the tree
(ztaginfLoadNode 0x0048D9A0) and asserts "Extra nodes in tree" if TAGN chunks remain. The engine uses the
tree to find the nearest tagged face / edges around the rider (ztaginfFindNearestFace 0x0048E840,
ztaginfFindEdges 0x0048F320; callers NgIFzbsp/NgIFzcollide).

[V] Tag faces are a collision copy of (most of) the world mesh: 92.2–100 % of their vertices coincide with
BSP vertices; props are not in it.

### 6.1 TAGF (0x50 bytes)

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | size | 0x50 |
| 0x04 | f32[3] | v0 | ZGE units |
| 0x10 | f32[3] | v1 | |
| 0x1C | f32[3] | v2 | (v0, v1, v2) is counter-clockwise against `normal` [V 1628/1628] |
| 0x28 | f32[3] | normal | unit, ZGE axes |
| 0x34 | i32 | faceTag | byte offset of the face's 12-byte record in the TAGI blob, −1 = none |
| 0x38 | i32[3] | edgeTag | 4-byte records for the edges v0→v1, v1→v2, v2→v0; −1 = none |
| 0x44 | i32[3] | vertexTag | 4-byte records for v0, v1, v2; −1 = none (**none in the corpus**) |

### 6.2 TAGI records

The blob (after the 8-byte header) is exactly the referenced records, **in face order** and, per face, in
field order (face, edges 0–2, vertices 0–2) [V 12/12]; every record is referenced once. The writer
regenerates the blob and the offsets from the per-face records.

**Face record (12 bytes)**: 20,865 faces have one

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u16 | code | bits 12–15 class, 0–11 id. Classes (faces): 0 (3,256, incl. 0x0FA1–0x0FA4), 1 (13,458; 200 distinct ids 0–3002), 2 (1,591), 3 (106), 4 (400), 5 (351), 6 (491), 7 (688), 8 (490), 10 (34). [V] NgIFzbsp 0x00468070 and NgIFzcollide 0x00469B00 read it and, when class == 1 and `extra` == 0, keep only the id |
| 0x02 | u8 | kind | 0 (20,751) or 1 (114) [I] |
| 0x03 | u8 | flags | 0 (18,489), 1, 2, 3, 5, 8, 10, 12, 14, 16; [V] bits 0–3 are remapped into the contact flags (`((b&1)<<1 \| b&0xC)<<1 \| b&2)<<2`) by the same two functions |
| 0x04 | u32 | param | 0–5, 7 (non-zero in 936) [I] |
| 0x08 | u32 | extra | 0 [V] |

**Meaning to the physics [V code paths, DM-048, docs/re/physics.md 2]:** class 0 ground (ids 0xFA1-0xFA4 =
respawn faces); class 1 reaches the physics as class 0 (the collision layer strips it when `extra` == 0, i.e.
always) and its id is read by game.c [I: gap / transfer zones]; **2 spine, 3 vert wall** (the id is the edge-tag
code of the coping edge [V 1,591/1,591 and 106/106]); **4-8 launch ramps** (take-off speed cap 11/15/21/23/35);
9 trigger (not solid, reported to prop.c); **10 launch pad** (id = launch speed along the normal, ids 30-50). Flags: bit 0 = not a respawn / wall-plant face, bit 2 =
not a wall-plant face. `param` is not read by the physics.

**Edge / vertex record (4 bytes)**: 3,550 edge tags, 0 vertex tags

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u16 | code | 0–0x0FA4 [V] read by NgIFzcollide 0x00469560 |
| 0x02 | u8 | kind | 0–3 (595 / 1,139 / 933 / 883) [V] read (byte +2) by NgIFzcollide 0x00469560; DM-048: grind sound / sparks, 1 metal, 2 wood (no sparks), 0/3 default |
| 0x03 | u8 | pad | `0xCD` (heap fill) [V]; the last record of AcclaimDirt has 0 |

**TAGI dataSize** [V 12/12] = blob length, minus 1 when the last record is a 4-byte record whose pad
byte is 0 (the exporter wrote 3 bytes, the chunk was zero-padded to 4).

### 6.3 TAGN (0x28 bytes + 4 × count)

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | size | 0x28 (record size, not the chunk size) |
| 0x04 | u8 | kind | 1 leaf, 2 split on x, 4 on y, 8 on z |
| 0x05 | u8[3] | junk | 0 or stale bytes (687 nodes) |
| 0x08 | f32 | split | split coordinate (ZGE); 0 in leaves |
| 0x0C | f32[3] | bound_a | box corner that maps to the **RW minimum** [V 3032/3032] (ZGE: x min, y max, z max) |
| 0x18 | f32[3] | bound_b | box corner that maps to the **RW maximum** |
| 0x24 | u32 | count | number of face indices (the engine reads the low 16 bits); 0 in split nodes |
| 0x28 | u32[count] | faces | indices into the TAGF list (leaves; a face may be in several leaves) |

Children: a split node is followed by its **low** subtree then its **high** subtree. [V] Engine
(ztaginfFindLeaf 0x0048E400): x: `p.x < split` → low; y: `p.y > split` → low; z: `p.z < split` → low (ZGE
values; the data agrees: the low child's bound equals the split on that side, 1510/1510).
[V] Every face is in at least one leaf; the root box equals the world bbox.

## 7. temp.dff [V]

A bare RW `Clump(0x10)` (version 0x301): one frame, one atomic, 33–192 vertices, textures such as
`WWSpanhires_01_01-02` / `warehouse1a`. It is an exporter leftover: the string `temp.dff` does not occur in
NgBMX.exe and no SCFI names it. Decoded with `zmd.Clump`; round-trips 8/8.

## 8. Object model (`dmbmx.formats.level`, `dmbmx.formats.rwworld`)

```
Level.read_dir(dir, components=None) -> Level      .files() -> {name: bytes} (ZSD re-signed)   .write_dir(out)
  .zsd: Zsd(scdf, entries[ScfiEntry(type, junk, size, name, name_raw)], extra, trailing)
  .world: rwworld.World(inv_origin, surface, flags, col_sector_size, materials[zmd.Material],
          material_indices, root: PlaneSector(axis, value, left, right, left_value, right_value) | AtomicSector,
          ext) ; .origin .sectors() .meshes() .bbox() .texture_names() .update_counts()
    AtomicSector(mat_base, inf, sup, coll_present, unused, vertices, normals(4 raw bytes), prelit, uvs,
          uvs2, triangles[(mat, a, b, c)], coll(bytes), ext, bin_mesh) ; .rebuild_bin_mesh() .normal_vectors()
          .geometry_changed() .rebuild_collision()
    World: .rebuild_collision(force=False, split=True) .split_sector(s) .line_hits(start, end)
          .write(keep_counts=False, rebuild_collision=True)
  rwcoll: decode check used_size col_sector_size side tree build line_test sphere_test segment_hits sphere_hits
  .materials: mati.MatSd                      .texture(name) -> Mati
  .prop_defs: PropDefSd.defs[PropDef(id, name, extents, junk, obji, model: zmd.Zmd, order)] ; .by_id()
  .props: PropSd.props[Prop(def_id, name, matrix[9], pos[3], obji)]
  .scene: SceneSd.points[ScenePoint(id, type, name, matrix, pos, obji)], .panorama: Panorama(model)
  .tags: TagSd(faces[TagFace(verts, normal, tag: FaceTag|None, edge_tags[3], vertex_tags[3])],
          root: TagNode(kind, junk, split, bound_a, bound_b, faces, low, high)) ; .leaves()
  .temp_dff: TempDff(clump: zmd.Clump)
  .prop_instances() -> [(Prop, PropDef, rw_pos, rw_rotation_rows)]      rotation = prop_matrix_to_rw
zge_to_rw(p, origin), pos_to_rw(p, origin), matrix_to_rw(m9) (scene points), prop_matrix_to_rw(m9) (props),
read_component(name, data)
```

Unknown chunks are kept as raw `Chunk` objects (`extra`); every reader raises `FormatError` subclasses
on malformed input (fuzzed: `tests/test_level_fuzz.py`). Writers raise `ValueError` for unrepresentable
data (chunk payload not a multiple of 4, > 256 vertices in a sector, > 65535 faces in a tag leaf).

**What an editor must keep consistent**: SCFI sizes (automatic in `Level.files()`); World header
counts and `colSectorSize` (automatic unless `keep_counts`); BinMesh after a sector edit
(`rebuild_bin_mesh`); the collision tree, the sector's `inf`/`sup` and the PlaneSect left/right values above
an edited sector (automatic on write, `World.rebuild_collision`; a sector too big for one tree is split in
two; PlaneSect `value` is not used by the game's walk and stays as it is); tag tree leaves after adding tag
faces (manual); prop `def_id` must exist in PropDef._sd.

## 9. CLI

```
dmbmx level info <dir> [--props] [--points]        ZSD sizes check, world/prop/scene/tag stats
dmbmx level dump-obj <dir> <out.obj> [--no-props] [--tags]      world + placed props (+ tag faces), MTL + PNGs
dmbmx level export <dir> <out.glb> [--no-props] [--no-tags] [--no-points] [--panorama]
dmbmx level rewrite <dir> <outdir>                 re-encode every file, re-sign the ZSD
```

glTF layout: root `<Level>` (extras: origin, ZGE scale, SCFI sizes) → `World` (one mesh, a primitive per
texture, COLOR_0 = prelit), `Props` (a node per instance with its matrix; meshes shared per def; extras:
def id, stored ZGE matrix/pos, OBJI hex), `Tags` (a node per face-tag code with the faces as a mesh;
extras: code, class, id, count), `ScenePoints` (empties; extras: name, type, id, OBJI hex), optional
`Panorama`. Textures get the game's colour key (RGB 255,0,255 → transparent). [V] Blender 5.3 imports
all 12 levels with matching triangle/prop/tag/point counts (`tools/blender/scripts/level_check.py`).

## 10. Invariants checked by tests

- Byte-identical round trip of every ZSD, BSP, Prop, PropDef, Scene, Tag `_sd` and temp.dff, and of whole
  folders via `Level.files()` (pinned counts: 12 levels, 12 of each `_sd`, 8 temp.dff).
- ZSD: 6 entries with types 1–6, names match the type, sizes equal the files.
- World header counts; collision blocks are 384 bytes; textures resolve (one known exception).
- `colSectorSize` = Σ used tree bytes rounded to 4; vertex order = material groups in first-use order; every
  sector's tree rebuilt by `rwcoll.build` passes `check()` and answers line and sphere queries like the shipped
  tree (`tests/test_rwcoll_build.py`); an edited sector is rebuilt on write, unedited sectors keep their bytes.
- Tag root box == world bbox; ≥ 90 % of tag vertices on world vertices; every face in a leaf.
- PDIN id == zhash(name); extents ≠ 0 ⇔ OBJI; props resolve to a def with the same name; rotations det +1;
  scene point ids == zhash(name).
- Editing props/tags writes a folder whose ZSD passes `dmbmx.integrity.zsd.check` and re-reads equal.

## 11. Open questions

1. ~~The 384-byte RW collision block per sector~~ **Resolved (DM-048)**: 4.3; it is used for all world
   collision, and geometry edits must rebuild it.
2. ~~`World.colSectorSize` and format bit 0x40000000~~ **Resolved (DM-049)**: 4.1. Still open: RW's own split
   choice for the collision tree (docs/re/physics.md 3.1).
3. ~~Face-tag `code` classes / ids and `param`, edge-tag `kind`~~ **Resolved for the physics (DM-048)**: 6.2 and
   docs/re/physics.md 2. Still open: the class-1 ids that are not coping edges (gap / trick names, game.c)
   and face flags 0x08 / 0x40.
4. ~~OBJI layouts per class~~: DM-043 resolved prop-def byte 22, the SpotLight colour and the Globals clear
   colour; DM-052 the prop tint, flags, group id and switch id and prop-def byte 0 (5.4). Still open: the
   meaning of the collision flags from prop bytes 4..6 (always 0).
5. ~~PDIN `extents` and the 16 junk bytes~~ **Resolved (DM-043)**: 5.1.
6. ~~Whether the euler conversion (0x00481FC0) transposes~~ **Resolved (DM-043)**: no; the chain is the
   identity (section 2). **DM-065**: but the render converter is the mirrored one, so props are rotated
   by their stored rows, not `F·M·F` (2.1).
7. ~~`PlayerT`, the SpotLight types, the Panorama~~ **Resolved (DM-043)**: 5.3 (PlayerT = start-heading
   target; SpotLight type = light slot, OBJI = colour; the panorama is never placed [I]).
8. ~~Which `Scene.Txt` entry names each ZSD~~ **Resolved (DM-043)**: section 1.
9. (DM-043) Tag codes: ztaginf.c never interprets a code; it turns the TAGI offsets into pointers and
   returns the records of the nearest tagged face / the tagged edges within a radius. The meanings stay
   in the game code (zbspColSphere, zcolFindEdgesAlong/00469B00; see open question 3).

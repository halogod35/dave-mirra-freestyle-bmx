# ZMD: model file (riders, bikes, shadows, sfx)

Owner: DM-011 (Agent C). Reader/writer: `tools/dmbmx/formats/zmd.py`. Chunk layer: `tools/dmbmx/core/chunks.py`.
Survey: `tools/research/survey_zmd.py`. Tests: `tests/test_zmd_*.py`, `tests/test_cli_zmd.py`.

Evidence tags: **[V]** verified by code over all 117 corpus files (115 non-empty), **[I]** inferred.

## 1. Overview

- [V] 117 `.zmd` files in `assets/` (mixed `.ZMD`/`.zmd`). Two are **0 bytes**: `TroyMcMurray1v2.ZMD`, `TroyMcMurray1v3.ZMD`. The reader returns an empty model for them (`Zmd.empty`) and writes back 0 bytes.
- [V] A ZMD is a **Z-Axis 4CC chunk tree**. The geometry inside is a **RenderWare 3.0 binary stream** (one `Clump`), and the **textures are embedded** as complete `.bmp`/`.png` files.
- [V] All 117 files round-trip **byte-identically through the decoded object model**: `write()` re-encodes every struct and does not pass bytes through. Corpus totals are 116 LODs, 410 geometries, 106,584 vertices, 96,018 triangles, 324 embedded textures and 1,853 frames.

All integers are little-endian.

## 2. Grammar

```
ZMD   := MODL
MODL  := "MODL" u32 size { MLOD+ MDEF MATI* }             size = file size - 8 (no padding)  [V]
MLOD  := "MLOD" u32 size { MLDF MDFF }                     one per LOD                         [V]
MLDF  := "MLDF" u32 8    { u32 flags; u32 zero }            flags: 7 (108x), 3 (6x), 0x1B (2x) [V]
MDFF  := "MDFF" u32 size { RW Clump stream }                exactly one RW chunk (Clump 0x10)   [V]
MDEF  := "MDEF" u32 size { model header + name table }      exactly one per file               [V]
MATI  := "MATI" u32 size { MADF (TBMP | TPNG) }             one per texture                     [V]
MADF  := "MADF" u32 80|84 { material def }                                                       [V]
TBMP  := "TBMP" u32 size { BMP file, zero-padded to 4 }                                          [V]
TPNG  := "TPNG" u32 size { PNG file, zero-padded to 4 }                                          [V]
```

- [V] 4CC chunk = `char tag[4]; u32 size; u8 payload[size]`. `size` **excludes** the 8-byte header. There is no alignment between chunks and no trailing bytes at any level, in any file.
- [V] Child order inside MODL is always `MLOD…, MDEF, MATI…`. 114 files have one MLOD. `DaveMirra_shadow.zmd` has two (MLDF flags 7 and 0x1B).
- [V] MATI counts: riders have 1 (TimMirra* and DaveMirra.ZMD have 2), bikes 3 to 5, `sfx.zmd` 20, and shadows / `ShadowVolume.ZMD` 0.
- [I] MLDF.flags is a per-LOD render/LOD flag word. Its meaning is unknown: 7 is the in-game default, 3 appears on older or test assets (`DaveMirra.ZMD`, `bike.zmd`, `bike_hires.zmd`, `TMbike02`, `AmishBoy2`, `sfx`), and 0x1B on shadow LODs.

## 3. RenderWare stream inside MDFF

[V] RW chunk = `u32 type; u32 size; u32 version; payload[size]` (12-byte header, `size` excludes it). Every chunk inside one MDFF carries the same version: **0x00000301** (113 files) or **0x00000303** (`Amishbike01.ZMD`, `secretbike01.ZMD`, `TroyMcMurray1v1.ZMD`). This is the RW 3.0-era encoding, with no packed build number. There is **no GeometryList**: each Atomic contains its Geometry inline (pre-3.1 layout).

```
Clump(0x10)
  Struct(0x01)            u32 numAtomics                               (1 for riders, 7 for bikes)
  FrameList(0x0E)
    Struct                u32 numFrames; Frame[numFrames]  (56 bytes each)
    Extension(0x03) x numFrames
      Animation(0x108)    8 bytes: u32 boneId; u32 zero               (every frame, 1853/1853)
  Atomic(0x14) x numAtomics
    Struct                u32 frameIndex; u32 flags (=5); u32 unused (=0)
    Geometry(0x0F)
      Struct              geometry data (§3.2)
      MaterialList(0x08)
        Struct            u32 n; i32 index[n]   (all -1 = "new material" in corpus)
        Material(0x07) x (count of -1)
          Struct          §3.3
          Texture(0x06)?  Struct(u32 filter/addressing = 0x106), String name, String mask, Extension(empty)
          Extension       (empty)
      Extension
        BinMesh(0x50E)    always                                      §3.4
        Bone(0x10E)       riders only (65 geometries)                  §3.5
        Morph(0x105)      402 geometries; payload u32 0 (kept raw)
    Extension             (empty)
  Extension               (empty)
```

### 3.1 Frame (56 bytes) [V]

| Off | Type | Name |
|---|---|---|
| 0 | f32[3] | right (rotation row 0) |
| 12 | f32[3] | up (row 1) |
| 24 | f32[3] | at (row 2) |
| 36 | f32[3] | position |
| 48 | i32 | parent frame index (-1 = root) |
| 52 | u32 | flags (always 0) |

- [V] Parents always precede their children. RW uses row vectors (`p' = p·R + pos`). `zmd.Frame.matrix()` returns the column-vector 4x4 (columns = right, up, at, pos).
- [V] The frame extension `Animation(0x108)` holds `u32 boneId` = `name_hash(bone name)` (§6) and `u32 0`. This is the link between frames, MDEF names and skin ranges.

### 3.2 Geometry struct [V]

```
u32 flags            0x35 (109 files), 0x34, 0x3D, 0x39, 0x3C, 0x10; numTexSets byte is 0
u32 numTriangles
u32 numVertices
u32 numMorphTargets  always 1
f32 ambient, specular, diffuse    (surface properties; always 1,1,1)
if flags & 0x08 PRELIT:   u8 rgba[numVertices][4]
if flags & 0x04 TEXTURED: f32 uv[numVertices][2]        (0x80 TEXTURED2 -> two sets; unused)
u16 tri[numTriangles][4]  = { b, a, materialIndex, c }
per morph target:
  f32 sphere[4] (x, y, z, radius)
  u32 hasVertices, hasNormals
  f32 vertices[numVertices][3]   if hasVertices
  f32 normals[numVertices][3]    if hasNormals
```

- [V] The computed size equals the chunk size for all 410 geometries. All triangle indices are `< numVertices`, and all material indices are `< MaterialList.n`.
- [V] **Winding:** `(a, b, c)` = `(tri[1], tri[0], tri[3])` is counter-clockwise against the vertex normals (bike: 407/408 faces agree; DaveMirra: 1184/1540, where smoothing on thin parts explains the rest). `meshes()` yields this CCW order.
- [V] UV origin is top-left (the OBJ dump writes `1 - v`, and the textures render correctly in Blender).
- [I] Flags: 0x01 tristrip (BinMesh is strips), 0x02 positions, 0x04 textured, 0x08 prelit, 0x10 normals, 0x20 light. Shadows (`*_shadow`) are prelit; `ShadowVolume.ZMD` is normals-only (0x10).

### 3.3 Material struct [V]

| Off | Type | Name |
|---|---|---|
| 0 | u32 | flags (0) |
| 4 | u8[4] | colour RGBA (ff ff ff ff; one `80 80 80 ff`) |
| 8 | u32 | unused (stale pointer garbage, kept verbatim) |
| 12 | u32 | isTextured (1, or 0 for the 9 untextured materials in shadow files) |
| 16 | f32[3] | ambient, specular, diffuse (**only in 0x303 files**: struct is 28 bytes, else 16) |

[V] Texture `String` chunks hold a NUL-terminated name padded to 4 bytes **with garbage** (e.g. `Name_917_0\0\xfd`). The writer keeps the raw bytes unless the name changes. The mask name is always empty.

### 3.4 BinMesh (0x50E) [V]

`u32 flags (1 = tristrips, 0 = lists); u32 numMeshes; u32 totalIndices;` then per mesh `u32 numIndices; u32 materialIndex; u32 indices[numIndices]`. `numMeshes == MaterialList.n` in all 410 geometries. With flags 0, `totalIndices == 3 * numTriangles`. The plain triangle list in §3.2 is always present, so `meshes()` uses that and ignores the strips. **An importer must regenerate BinMesh** (strips or lists) when it changes topology.

### 3.5 Skinning: Bone (0x10E) plugin [V]

Riders are a **single mesh with rigid skinning**: each vertex belongs to exactly one bone. Vertices are sorted by bone.

```
struct { u32 firstVertex; u32 vertexCount; u32 boneId; } entry[size / 12];
```

- [V] Used ranges are contiguous from 0 and cover exactly `numVertices` in 65/65 geometries. One entry per frame, with unused entries (the root `Skin`) all zero. Exception: `MikeLaird5.ZMD` has 29 entries for 39 frames.
- [V] `boneId` matches the frame's `Animation(0x108)` id, which gives a vertex → frame mapping. `meshes()` returns `joints = [[frameIndex]]`, `weights = [[1.0]]`.
- [V] **Bind pose = the frame hierarchy as stored, and vertices are in model space** (not bone-local): each bone's vertex centroid sits next to its joint's world position, e.g. Head vertices around y≈0.8 with the Head joint at y=0.686. `skeleton()` therefore gives `inverse_bind = inverse(world)`.
- [V] DaveMirra.ZMD: 17 bones (`Skin, Hips, LeftUpLeg, Chest, RightUpLeg, RightLowLeg, RightFoot, Shoulders, Head, LeftUpArm, RightUpArm, RightLowArm, RightHand, LeftLowArm, LeftHand, LeftLowLeg, LeftFoot`). The in-game riders `*1..5` have 20 bones, and MikeLaird5 has 39. T-pose, about 1.95 units tall, Y up.
- [I] There is no RpSkin/HAnim (0x116/0x11E) in any file. This is the older RW 3.0 "Bone" plugin. Animations live in `motion.zal` (DM-016) and presumably target frames by `boneId`.

Bikes are **not skinned**. They are 7 rigid atomics, each attached to its own frame (`BackWheel`, `FrontWheel`, `Sprocket`, `Handlebars`, pedals …). Their vertices are frame-local, so `meshes(world=True)` bakes the frame matrices. [V] With the frames applied, the parts assemble into a coherent bike in Blender (bind pose pitched about 15° nose-up relative to the root `Hipsdeck` frame [I]).

## 4. MDEF: model header + name table [V]

44-byte header, then `count` × 20-byte entries. Size = `44 + 20*count` in 115/115 files.

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | kind | 2 (riders) or 0 (bikes, sfx, shadows) |
| 0x04 | char[16] | name | `DAVEMIRRA1`, `HIPSDECK`, `SFXCUBE`, `ERROR` (ShadowVolume). Garbage after the NUL is kept verbatim |
| 0x14 | u32 | name_id | `name_hash(name)` |
| 0x18 | f32[4] | params | [I] in-game riders: about (1.20, 1.60, 1.06, 128.8). Test assets: (0, -0, 0, 0.017). Unknown (extents / scale?) |
| 0x28 | u16 | count | number of entries |
| 0x2A | u16 | unk | 0x0012, 0xFFFF or 0x7800 |
| 0x2C | entry[count] | `char name[16]; u32 id` | bone or part names. `id == name_hash(name)` except renamed copies (§6) |

[V] For riders and bikes the entries list the frames (by id, not in frame order). `skeleton()` names frames via `id → name`.

## 5. MATI: embedded textures [V]

```
MADF (80 or 84 bytes):
  0x00 u32   nameOffset = 0x48 (always)
  0x04 u8[64] material parameters (DM-014). Rider example: 0,0,0, 0.9,0.9,0.9, 0.5,0.5,0.5, 0.1,0.1,0.1, 0.25, 0,0,0 as f32 [I]
  0x44 u32   name_id = name_hash(name)                       (324/324)
  0x48 char  name[], NUL, zero-padded to 4 bytes             e.g. "NAME_917_0", "TIRE", "SPARK"
TBMP / TPNG: a complete .bmp / .png file, zero-padded to a multiple of 4 bytes.
```

- [V] 215 TBMP: BMP size = the `bfSize` field. Formats: 24-bit 64², 128², 256², 32², 16²; one 8-bit 64², one 8-bit 32², one 4-bit 32². 109 TPNG: the data ends after `IEND`+CRC. Padding is 0 to 3 zero bytes in both cases.
- [V] **How textures are referenced:** the RW `Texture` name string (e.g. `Name_917_0`, `tire`) equals the MADF name **case-insensitively** (MADF stores it upper-cased), and the MADF id is the name hash. For all 115 files, every RW texture name resolves to a MATI in the same file and every MATI is used. Textures do not come from `material.zal` (that ZAL presumably serves level geometry, DM-013/DM-014). `Zmd.texture(name)` performs the lookup.
- MADF parameters, image constraints and the shared `Mati` class (`tools/dmbmx/formats/mati.py`, also used for level `Mat._sd`): see TEXTURE.md (DM-014).

## 6. Name hash [V]

```
name_hash(s) = (toupper(s[-1]) << 16) + Σ_i (i+1) * toupper(s[i])      (32-bit)
```

It matches 2205/2227 ids over MDEF names, MDEF entries and MADF names. All 22 misses are renamed copies that keep the original's id (`rider_shadow.zmd` `Hips01` has the id of `Hips`; `bike_hires` `wheel1`/`wheel2` have the id of `wheel`). Examples: `Head` → 0x4402A5, `RightHand` → 0x440CC8, `DAVEMIRRA` → 0x410D2A. [V] This is the engine's `zHashName` (VA 0x004828C0, DM-010: `dmbmx.core.hash.zhash`); `zmd.name_hash` delegates to it.

**Bone naming** (`zmd.bone_name`, used by `skeleton()`): MDEF entry for the id, else reverse lookup over `extra_names` + `zmd.BONE_NAMES` (all MDEF base names in the corpus; 26 of them also occur as strings in NgBMX.exe and hash to frame ids [V]; collisions exist, e.g. `LeftGrip`/`tabletop`, `Hips`/`jHPS`, so MDEF wins), else `bone_0x%08X`. Frame id `0xFFFFFFFF` = no bone (bike frame 5, ShadowVolume helper) gives `frame_<i>`. [V] The only unresolved id in the corpus is `0x00440AE8` (DaveMirra_shadow LOD 1 root).

## 7. Object model (`dmbmx.formats.zmd`)

```
Zmd.read(bytes, strict=True) / zmd.read() -> Zmd ;  zmd.write(Zmd) -> bytes
Zmd.items          [Lod | Mdef | Mati | Chunk]  (MODL children in order)
Zmd.lods           [Lod(mldf, clump: Clump, rw_version)]
Clump              frames [Frame], atomics [Atomic(frame, flags, unused, geometry, ext)], ext
Geometry           flags, surface, prelit, uv_sets, triangles(b,a,mat,c), morph_targets[MorphTarget],
                   materials [Material(color, texture: Texture(name, mask), surface?)], material_indices,
                   bin_mesh: BinMesh, bones [(first, count, id)], ext [Chunk]
Zmd.mdef           Mdef(kind, name, name_id, params, unk, entries [MdefEntry(name, id)])
Zmd.materials()    [Mati(name, name_id, kind 'bmp'|'png', image, pad, madf_head)]
Zmd.texture(name)  Mati for an RW texture name
Zmd.skeleton(lod)  [{index, parent, id, name, matrix, world, inverse_bind}]
Zmd.meshes(lod, world=False) -> dicts: positions, normals, uvs, uvs2, colors, indices (CCW),
                   material_ids, materials (texture names), joints, weights, frame, matrix
name_hash(str)
```

Plugins that are not decoded (Morph, anything unknown) stay as raw `Chunk`s in the `ext` lists. `strict=False` keeps an undecodable MDFF as raw bytes (`Lod.clump = None`). The default raises `FormatError` subclasses (`ZmdError`, `ChunkTruncatedError`, …), which the fuzz tests check.

CLI: `dmbmx zmd info <f> [--tree]`, `dmbmx zmd dump-obj <f> <out.obj> [--lod N] [--local]` (writes .mtl + textures next to it), `dmbmx zmd extract-textures <f> <dir>`.

## 8. Invariants checked by tests

- Byte-identical `write(read(f))` for 117/117 files (`test_zmd_roundtrip`).
- Per-chunk re-encode equals the original chunk for the frame list, geometry struct, BinMesh, materials and MDEF (`test_zmd_reencode_chunks`).
- Triangle and material indices in range, joints in range (`test_zmd_geometry_decodes`).
- MATI ↔ RW texture links and the name hash (`test_zmd_textures_linked`).
- Rider skeleton names, ids, model-space bind and height (`test_zmd_skinning_davemirra`).
- Edits survive write → read (`test_zmd_edit_roundtrip`). Fuzz (`test_zmd_fuzz`, `test_synthetic_fuzz`).

## 9. Open questions

1. ~~MLDF flags and the second u32~~ (DM-059, docs/re/zrw.md 2): the PC engine **ignores word 0** (7 / 3 / 0x1B); word 1 is
   the LOD switch distance as a float, 0 = the defaults 0 / 50 / 100 / 150 (every corpus file has 0) [V]. At most 4 MLODs
   are read. Nothing switches LODs: only LOD 0 is ever drawn [V: zrw code].
2. ~~MDEF `params` f32[4] and `unk` u16~~ (DM-059): **not read** by the engine [V]. MDEF `kind` is a flag word: 2 = skinned
   (RpBone), set exactly for the models with Bone plugins [V]; the other bits (1, 4, 8, 0x10, 0x40, 0x80) are coded but unused
   by the corpus (zrw.md 2). Entry names keep at most 15 characters in the engine (byte 15 is forced to 0) [V].
3. MADF parameter block: resolved in TEXTURE.md (DM-014).
4. Atomic `flags` = 5 and `unused`, Morph plugin `u32 0`: standard RW meanings assumed [I]. DM-059: the engine overrides the
   atomic flags after loading (collision test on unless MDEF 0xC0; flag 0x80 for one-part models) [V].
5. `MikeLaird5.ZMD`: 29 Bone entries for 39 frames (the extra frames are unskinned helpers?).
6. Whether the game needs BinMesh tristrips (flags 1) or also accepts lists (flags 0 exists in 10 geometries) when importing new meshes.
7. Is `DaveMirra.ZMD` (17 bones, MLDF 3, non-strip) a leftover test asset? The in-game riders are `DaveMirra1..5`.

# `dmbmx import`: glTF (.glb) → ZMD

Owner: DM-021 (Agent G). Code: `tools/dmbmx/convert/gltf_import.py`, `tools/dmbmx/convert/glb_edit.py` (scripted
edits), `tools/dmbmx/convert/imaging.py` (texture re-encoding), CLI `tools/dmbmx/cli/import_.py`.
Tests: `tests/test_import_gltf.py`. Play-test: `tools/testing/playtest/make_playtest_dm021.py`.
Counterpart: `docs/tools/export.md` (layout and `extras.dmbmx` schema). Evidence tags: **[V]** means verified by code or tests, **[I]** means inferred.

## 1. Usage

```bash
tools/testing/py.sh -m dmbmx export assets/DMbike01.zmd out/bike.glb
#   ... edit out/bike.glb (script with GlbDoc, or any tool that keeps node/primitive extras) ...
tools/testing/py.sh -m dmbmx import out/bike.glb out/DMbike01.zmd [--base assets/DMbike01.zmd] [--binmesh auto|strips|lists] [--strict]
```

- The command prints one line per change (`change:`), one per warning (`WARNING:`) and which BinMesh chunks it
  regenerated, then the output size and SHA-256. It adds `(identical to --base)` when nothing changed.
- `--base` is the original ZMD. The importer checks it against the SHA-256 stored in the .glb and warns on
  a mismatch. It also supplies the LODs that a `--lod` (partial) export left out.
- `--strict` turns every warning into an error.
- **It refuses to write inside the game install or `_pristine` (AD-1).** Write to `out/` and install into
  `_testgame` with a play-test script. Exit codes: 0 means OK, 1 means bad input, 2 means usage or a refused path.

Python: `zmd_bytes, report = gltf_import.import_glb(glb, base=None, binmesh="auto", strict=False)`.
`report.changes`, `report.warnings` and `report.binmesh` (per atomic: `kept`, `strips` or `lists`) describe what happened.

## 2. Principle: keep, unless it is no longer true

Non-geometry data (MDEF, MADF, MaterialList and materials, frame records, Morph and other plugins, clump
and atomic extensions) is always rebuilt **verbatim from `extras.dmbmx`**. Geometry and pixels come from
the glTF buffers. Three chunks are *derived* from the geometry. Each one is kept byte-for-byte while it is
still consistent with the imported geometry, and regenerated otherwise:

| Derived data | Kept when | Otherwise |
|---|---|---|
| BinMesh (0x50E) | it draws exactly the imported triangles, with the same winding, per material, and the vertex count is unchanged. [V] This holds for all 720 (geometry, material) pairs of the unedited corpus. | Regenerated (§3) |
| Bone (0x10E) ranges | the vertex → bone mapping from JOINTS_0 equals the stored ranges | Rebuilt (§4) |
| Bounding sphere | it is still centred on the bbox centre and encloses every vertex. [V] All 410 corpus spheres are bbox-centred and enclose all vertices; the radius is usually a bit larger than the maximum distance. | centre = bbox centre, radius = maximum distance × (1 + 1e-5) |

**[V] Unedited round-trip: export → import gives byte-identical output for 115/115 non-empty ZMDs,
with zero changes and zero warnings** (`test_unedited_import_is_byte_identical`).

## 3. Geometry

- **Positions, normals, UVs and prelit colours** come from POSITION, NORMAL, TEXCOORD_0/1 and COLOR_0.
  COLOR_0 can be UBYTE, USHORT or FLOAT, VEC3 or VEC4. Primitives may share one vertex stream (as
  `dmbmx export` writes them) or have one stream each, in which case the streams are concatenated.
  `byteStride` is supported. Sparse accessors, morph targets (shape keys), non-indexed primitives and
  modes other than triangles are refused.
- **Materials:** each primitive maps to an original material slot through `extras.dmbmx.material_slot`,
  or, failing that, a material whose name equals the slot's texture name. **New materials are refused.**
- **Triangle order:** if the triangle count per material is unchanged, the original interleaving is
  restored from `tri_materials`. Otherwise triangles are written grouped by material, which is reported
  as a change. glTF `(a, b, c)` CCW is stored in the file as `(b, a, slot, c)`.
- **Frame records** of unchanged frames are written back from the stored 56 raw bytes
  (`_patch_frames`), so the output is exact even on hosts that flush denormal floats (Blender, DM-022).
- **Rigid parts:** a transform on the mesh node itself (relative to its frame node) is baked into the
  vertices. Normals use the inverse-transpose. A skinned mesh node's transform is ignored, per glTF.
- **Frames:** if a frame node's matrix or TRS differs from the stored record (compared in float32), the
  frame's right/up/at/pos are updated, which is reported as a change. Re-parenting is ignored with a
  warning. **Adding or deleting frames is refused**, because animations in `motion.zal` address them by bone id.
- The model root rotation (the rider upright correction) and the LOD node transform are presentation
  only. Changes to them are ignored with a warning.
- **BinMesh regeneration** (`--binmesh`):
  - `auto` (the default) uses the **same form as the original geometry**: strips for the 0x35-flag
    riders and bikes, lists for the 10 list geometries.
  - `strips` uses a greedy stripifier. Sub-strips are joined with degenerate triangles plus a parity pad,
    so every triangle keeps its winding. Each result is checked by decoding it again.
  - `lists` writes plain triangle lists.
  - The geometry flag `0x01` (TRISTRIP) is set or cleared to match. [V] It matches the BinMesh form in
    all 410 corpus geometries.
  - [I] The engine accepts lists: `sfx.zmd` and `ShadowVolume.ZMD` use them. But no in-game rider or bike
    does, so `auto` avoids depending on that.

## 4. Skinning (riders)

- Each vertex gets **one** bone: the JOINTS_0 entry with the largest WEIGHTS_0 weight. If a vertex has
  more than one non-zero weight, the importer warns once with a count. Joint index → frame comes from the
  joint node's extras.
- The skin must contain exactly the model's frames. **A change in bone count is refused.**
- When the mapping changes, the **stored entry order is kept**. [V] That order is the authoring order,
  not the frame order: DaveMirra1 runs `_Abdomen, Chest, LeftLowLeg, ...`, with ascending `first`.
  Vertices are stable-sorted by their bone's entry, so the ranges are contiguous from 0, and all
  attributes and indices are permuted to match.
  - A bone that newly gets vertices takes the first all-zero (unused) entry. If there is none, an entry is
    appended, with a warning.
  - A bone that loses all its vertices gets the entry `(0, 0, 0)`, the same form as the corpus's unused
    entries, with a warning.
- A rigid part cannot be turned into a skinned one: a skin or JOINTS_0 on a rigid part is ignored, with a warning. A skinned part that lost its skin is refused.

## 5. Textures

An image is treated as edited when its SHA-256 differs from `png_sha256`.

- **TPNG:** an 8-bit RGBA, non-interlaced PNG (the format of every corpus TPNG) is **written as-is**.
  Any other PNG is re-encoded to RGBA8, with a warning. [I] libpng 1.0.5 checks chunk CRCs, and
  `imaging.png_encode` writes valid ones.
- **TBMP:** the BMP is rebuilt at the **original bit depth**, with the original header and palette:
  - 24-bit: RGB, with the original header.
  - 4/8-bit palettised: if the PNG is a palette PNG of the same depth and palette, its indices are used
    as painted. Otherwise, by default (`--palettised promote`, DM-022) the texture is **promoted to a 24-bit
    BMP** with exact colours: DM-014 [V] found that TBMP payloads go through RenderWare's generic BMP reader,
    which loads 4/8/24-bit in any slot. `--palettised quantize` instead maps colours to the nearest colour
    of the original palette, with a warning (the DM-021 behaviour).
  - Alpha: pixels with alpha < 128 become the colour key **255,0,255**, which the game turns transparent
    (DM-014, 0x004978F0). This is reported as a change.
  - If the size changes, the header is resized, with a warning.
- The importer also warns about non-power-of-two sizes (every corpus texture is a power of two) and
  sizes above 256 (the largest in the corpus).

## 6. Refusals (all `GltfImportError`, a `FormatError`; limits are `GltfImportLimit`)

The importer refuses:
- a .glb without the dmbmx root extras
- an extras version mismatch
- a partial export without `--base`
- more than 65535 vertices in one geometry (the triangle indices are u16)
- missing TEXCOORD on a textured geometry
- a skinned geometry that lost its skin or JOINTS_0
- skin joints that are not this LOD's frames
- frames or atomics added, deleted or duplicated
- a mesh deleted from a part
- new materials
- non-triangle, non-indexed, sparse or morph-target data
- external buffers or images
- bad accessor bounds, strides or non-finite floats
- bad base64 or extras data

Any other malformed structure is caught at the `import_glb` boundary and re-raised as a
`GltfImportError` (error contract). [V] `fuzz_parser(import_glb, sfx.glb, n=200)` is clean.

## 7. Scripted edits: `GlbDoc`

```python
from dmbmx.convert.glb_edit import GlbDoc
doc = GlbDoc(glb)
doc.map_positions(doc.atomic_node(frame_name="FrontWheel"), lambda p: (p[0]*1.5, p[1]*1.5, p[2]*1.5))
doc.set_image(doc.image_index("NAME_431_0"), new_png)
zmd_bytes, rep = import_glb(doc.to_bytes())
```

## 8. Play-test DM-021-PT1 (M2)

`tools/testing/py.sh tools/testing/playtest/make_playtest_dm021.py [--out DIR]` builds `out/playtest/dm-021/`. It takes **all 47
in-game bikes** (every ZMD whose root frame is `Hipsdeck`, shadows excluded) from `_pristine` through
export → `GlbDoc` edit → `import_glb(strict=True)`, and verifies each result. Three variants:

- `wheels`: FrontWheel and BackWheel ×1.5 about their hubs. Only the vertices and sphere change; the BinMesh strips are kept.
- `paint`: the frame texture is colour-inverted.
- `both`: the two edits together.

`install.sh wheels|paint|both|--restore` follows docs/testing.md §5. Outcome: pending (see the card).

## 9. Known limitations

- **Blender round-trip:** a .glb re-exported by Blender's glTF exporter is only importable if it keeps the
  node and primitive extras ("Include → Custom Properties"). It must also keep one node per frame and per
  part, and the skin joints. Blender's importer turns the LOD node into the armature, and its exporter
  re-splits vertices. This is untested here. The Blender add-on (DM-022) should call `dmbmx` directly
  rather than go through Blender's glTF exporter.
- The vertex count can change (adding or removing vertices works), but the part count, frame count and
  material count cannot.

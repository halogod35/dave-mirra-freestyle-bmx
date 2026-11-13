# `dmbmx export`: ZMD → glTF 2.0 (.glb) + OBJ

Code: `tools/dmbmx/convert/gltf.py`, `tools/dmbmx/convert/obj.py`,
`tools/dmbmx/convert/imaging.py`, CLI `tools/dmbmx/cli/export.py`. Tests: `tests/test_export_gltf.py`,
`tests/test_imaging_unit.py`, `tests/test_cli_export.py`. Blender checks: `tools/blender/`.
Source format: `docs/formats/ZMD.md`.

Evidence tags as in the format docs: **[V]** verified by running code over the corpus or in Blender 5.3, **[I]** inferred.

## 1. Usage

```bash
tools/testing/py.sh -m dmbmx export assets/DaveMirra1.ZMD out/DaveMirra1.glb          # all LODs
tools/testing/py.sh -m dmbmx export assets/DaveMirra_shadow.zmd out/s.glb --lod 1     # one LOD (not restorable)
tools/testing/py.sh -m dmbmx export assets/DMbike01.zmd out/bike.glb --obj            # + bike.obj/.mtl/bike_<tex>.png
tools/testing/py.sh -m dmbmx export --all out/glb [--game-dir DIR] [--obj]            # every .zmd in the install
```

- `--all` scans `--game-dir` (default `$DMBMX_GAME_DIR`, else the project install), skipping
  `_pristine`, `_testgame`, `wt`, `dave-mirra-bmx` and `dave-mirra-freestyle-bmx`. It writes `<outdir>/<stem>.glb`. The two
  0-byte files (`TroyMcMurray1v2/1v3.ZMD`) print `skip ...: empty file (0 bytes)`.
  [V] `exported 115, skipped 2 (empty), failed 0, of 117 files` in about 5 s with `--obj`.
- `--no-upright` leaves out the rider root rotation (§2).
- `--anims all|1,5,10-12` adds motion.zal clips as glTF animations (rotation tracks on the
  channel frames). For riders it also removes the upright root rotation, because the animated Hips
  stands the skeleton up. The tracks are the game's frame rotations: the arm table and the engine -> model
  conversion (x, −y, −z, w) of every channel. `--motion-zal PATH` overrides the bank. See
  `docs/formats/ANIM.md` §3-4.
- Exit codes: 0 means OK, 1 means an export failed or the file was empty, 2 means bad usage.

Python API:

```python
from dmbmx.convert import gltf
glb = gltf.export_glb(zmd.read(data), source=data, source_name="DaveMirra1.ZMD", lods=None, upright=True)
original = gltf.restore_zmd(glb)        # unedited .glb -> the original ZMD bytes
js, bin_ = gltf.read_glb(glb)           # JSON dict + BIN chunk, with bounds checks
```

## 2. Coordinate system

**RW → glTF mapping: identity.** Positions, normals and frame matrices are written unchanged,
and there is no handedness flip. The glTF root node adds one rotation for riders only.

| Evidence | Finding |
|---|---|
| [V] Bikes (`*bike*.zmd`), `sfx.zmd`, `ShadowVolume.ZMD`, test rider `DaveMirra.ZMD` | Up is +Y: the bike's wheels sit at y ≈ −0.3, the seat at y ≈ +0.5, and the front wheel is at +Z. ShadowVolume spans y 0..1.48. |
| [V] Bike `LeftGrip` x = +0.25, rider `LeftHand` x = +0.73 | With +Y up and +Z forward, the model's left is +X. That is a right-handed system, the same as glTF ("+Y up, front faces +Z"). |
| [V] In-game riders `*1..5`, `rider_shadow`, `DaveMirra_shadow` | The bind pose is **lying along +Z**: Head z = +0.65, feet z = −0.75, and forward is −Y. The `Hips` frame (first child of the root `Skin` frame) has `up = (0,0,1)`, `at = (0,−1,0)`. In other words, the skeleton below the Hips is authored Y-up and the Hips frame rotates it to model +Z. [I] In game, animations replace the Hips matrix, so the rider stands up in the Y-up world. |

**Upright correction** (`gltf.upright_correction`). For a skinned LOD, the exporter looks at the first
child of the root frame. If that frame's world local-Y axis points along model ±Z (|z| > 0.9),
the model root node gets `rotation = Rx(∓90°)`. For riders this is `Rx(−90°)` = quaternion
`[−0.7071068, 0, 0, 0.7071068]`:

```
glTF = Rx(-90°) · RW        (x, y, z) -> (x, z, -y)
       | 1  0  0 |
       | 0  0  1 |     model +Z (rider up) -> glTF +Y,  model -Y (rider forward) -> glTF +Z
       | 0 -1  0 |
```

Everything else gets no rotation. The rotation sits only on the root node. Vertices, joints and
inverse-bind matrices stay in RW model space, so the data is unchanged and the glTF importer just drops
the root rotation. `extras.dmbmx.axis = {"rw_to_gltf": "identity", "root_correction": "rx-90" | null}`
records the decision.

The Blender glTF importer maps glTF +Y to Blender +Z and glTF +Z to Blender −Y. In Blender,
riders and bikes therefore stand on +Z and face −Y (towards the Front view).
[V] 64 exports are skinned. The 63 that have `Head`/`LeftFoot` joints all pass the "Head joint is more
than 1.0 above LeftFoot" check in `verify_glb.py` (smallest difference 1.40). `rider_shadow` has
renamed bones (`Hips01`, ...) and is not covered by this check. The thumbnail shows DaveMirra1
standing upright with its left hand on +X.

The OBJ export (`--obj`) bakes the frame matrices and applies the same correction to positions and
normals, so it lines up with the .glb.

**Winding / UVs.** `Zmd.meshes()` already yields CCW triangles `(v1, v0, v2)`, and these are written
as-is (glTF front faces are CCW). [V] In Blender, 100% of faces agree with the vertex normals on
every in-game rider and on DMbike01. Lower ratios appear only where the source normals are smoothed
across thin parts: `DaveMirra.ZMD` 1186/1540 (this matches ZMD.md §3.2), `ShadowVolume` 264/288 and
`bike_hires` 1893/1990. RW UVs have a top-left origin, as glTF does, so they are written unchanged.
The OBJ writes `1 − v`.

## 3. glTF layout

```
scene
└─ <MDEF name>                 model root (rotation for riders), extras.dmbmx = file-level meta (§4)
   └─ <name>_LOD<i>            one node per exported LOD, extras.dmbmx = LOD meta
      ├─ <frame 0 name>        one node per RwFrame, local `matrix` (omitted when identity),
      │  └─ <frame ...>        hierarchy = the frame parent links; names from skeleton() (MDEF / zhash)
      │     └─ <frame>_mesh<a> RIGID atomic (bikes, sfx, shadows): child of its frame, vertices frame-local
      └─ <frame>_mesh<a>       SKINNED atomic (riders): child of the LOD node, has `skin`
```

- **Meshes.** There is one glTF mesh per atomic and one primitive per material slot that triangles
  use. All primitives share the same vertex accessors: `POSITION`, `NORMAL`, `TEXCOORD_0`
  (+`TEXCOORD_1`), `COLOR_0` (prelit RGBA as normalized UBYTE), and `JOINTS_0`/`WEIGHTS_0`.
  Indices are UNSIGNED_SHORT. Every vertex is written, including vertices no triangle references.
- **Skin.** There is one skin per skinned LOD. `joints` = **all** frame nodes in frame order, so a
  joint index equals the RW frame index, `skeleton` = the root frame, and the
  `inverseBindMatrices` are `inverse(world(frame))`, because vertices are stored in model space
  (ZMD.md §3.5). `JOINTS_0 = (frame, 0, 0, 0)` and `WEIGHTS_0 = (1, 0, 0, 0)` (rigid, one bone per vertex).
- **Bikes / rigid models.** Each atomic's mesh node hangs under its frame node with an identity
  transform. The frame-node chain carries the transforms, so the 7 parts assemble correctly.
  [V] This was checked visually in `out/thumb_DMbike01.png`.
- **Names.** Frame and joint names come from `Zmd.skeleton()` (MDEF entry, else zhash reverse
  lookup, else `bone_0x...` / `frame_<i>`). The source is stored as `name_source` in the frame extras.
- **Materials.** One glTF material per distinct (texture, RW colour). It uses `baseColorTexture`
  and `baseColorFactor` = RW colour / 255, metallic 0 and roughness 1. `alphaMode` is BLEND if the
  texture has partial alpha or the colour alpha is below 255, and MASK for 0/255-only alpha.
  The sampler is LINEAR / LINEAR_MIPMAP_LINEAR with REPEAT ([I] from the RW filter/addressing value `0x106`).
- **Images.** There is one image per MATI, in file order (image index = MATI index), all `image/png`.
  **TPNG** images are embedded byte-for-byte. **TBMP** images are converted by
  `imaging.bmp_to_png`: 24-bit → RGB8, and 4/8-bit → palette PNG with the **same indices and bit
  depth**, so the conversion is lossless and reversible.

## 4. Lossless re-import data: `extras.dmbmx` (chosen over a sidecar)

**Decision:** the rebuild data lives **inside the .glb** as `extras.dmbmx` on nodes and
primitives. There is no `.zmdmeta.json` sidecar. One file can't get separated from its metadata,
and Blender imports the extras as custom properties. Every non-geometry byte is stored as base64.
Geometry and texture pixels come from the glTF buffers.

[V] `gltf.restore_zmd(export_glb(f)) == f` **byte-for-byte for 115/115** non-empty ZMDs
(`test_export_restore_lossless`). This includes all 215 TBMP rebuilt from their PNG plus the
stored header (`test_corpus_bmp_png_roundtrip`). The glTF importer starts from this path and only needs to
handle what the user changed.

| Where | Key | Content |
|---|---|---|
| root node | `format`, `version` | `"dmbmx-zmd"`, `1` |
| | `source` | `{name, size, sha256}` of the original ZMD |
| | `axis` | §2 |
| | `complete` | false for a `--lod` export (cannot be restored) |
| | `items` | MODL children **in file order**: `{"type":"MLOD","lod","node"}`, `{"type":"MDEF","data":b64 payload}`, `{"type":"MATI","mati","image","madf":b64 MADF payload,"img_tag":"TBMP"/"TPNG","pad":b64 padding,"png_sha256", "bmp_header":b64 (bytes before the pixels), "bmp_tail":b64 (bytes after them)}`, `{"type":"chunk","chunk":b64}` for unknown tags |
| LOD node | `lod`, `mldf`, `rw_version` | MLDF words, RW version (0x301/0x303) |
| | `clump_struct_extra`, `clump_ext` | b64 |
| | `frames`, `atomic_nodes` | frame count; node index of each atomic **in atomic order** |
| frame node | `frame`, `parent`, `anim_id`, `name_source` | RW frame index etc. |
| | (note) | `record` is taken from the source file's raw bytes when it matches the decoded frame, because Blender's Python flushes denormal floats and would lose them. |
| | `record` | b64 of the 56-byte RW frame record (exact matrix + parent + flags) |
| | `ext` | b64 of the frame's extension chunks (Animation 0x108 = bone id) |
| atomic mesh node | `atomic`, `frame`, `flags`, `unused`, `ext` | RpAtomic fields, b64 extension |
| | `geometry` | `flags`, `surface`, `vertices`, `triangles`, `morph_targets` [`{sphere, has_vertices, has_normals}`], `material_list` (b64 RW MaterialList chunk: materials, colours, texture names with their garbage padding), `ext` (b64 BinMesh + Bone + Morph chunks), `tri_materials` (run-length `[[slot, count], ...]` = the original triangle order across primitives) |
| primitive | `material_slot` | RW material-list slot of this primitive |

Rebuild rule (implemented in `restore_zmd`): for each atomic, take the triangles of every primitive
in order and interleave them following `tri_materials`. Each glTF `(a, b, c)` becomes the file's
`(b, a, slot, c)`. Positions, normals, UVs and prelit colours come from the accessors and are exact,
because float32 is stored unchanged and all corpus normals are unit-length. Everything else comes
from the b64 chunks.

For **edited** models: when the geometry changes, `BinMesh` must be regenerated and
`Bone` ranges re-derived from `JOINTS_0`, and vertices must be re-sorted by bone (ZMD.md §3.4/3.5).
The stored `ext` chunks are only valid while the topology is unchanged. `png_sha256` tells the
importer whether a texture was edited.

## 5. Verification

```bash
tools/testing/py.sh -m pytest tests -k "export or imaging"         # structure, counts, lossless restore
tools/testing/py.sh -m dmbmx export --all out/glb
"C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
    --python tools/blender/scripts/verify_glb.py -- out/glb --json out/blender_verify.json
"C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup \
    --python tools/blender/scripts/render_thumb.py -- out/glb/DaveMirra1.glb out/thumb_DaveMirra1.png
```

`verify_glb.py` imports each .glb with the stock importer and compares it with `dmbmx` reading the
source ZMD. It checks triangles, vertices, bones (= frames of skinned LODs), empties (root, unskinned
LOD nodes and frames), the upright check (Head above LeftFoot) and winding (face normal against
corner normals, at least 70%). [V] 2026-11-12: **115/115 OK**.

Blender importer behaviour the checker accounts for ([V]):
- A vertex used by triangles of two materials is **duplicated** (once per primitive). Unreferenced
  vertices are dropped. The expected count is then the number of (material, vertex) pairs
  ("split"). Only `DaveMirra.ZMD` and `bike_hires.zmd` are affected; all other files match exactly.
- Blender cannot hold two faces on the same vertex set: `bike_hires.zmd` has **96 duplicate
  triangles** (double-sided faces), and these collapse (2086 → 1990). The .glb itself has all 2086.
- The importer creates a hidden `Icosphere` bone-shape helper in the `glTF_not_exported`
  collection, which is ignored. It turns the LOD node of a skinned LOD into the armature object.
- glTF joints have no length, so Blender guesses the bone tails. Joint *positions* are exact.

## 6. Known limitations / open points

- All LODs are exported and all of them are visible, overlapping. Only `DaveMirra_shadow.zmd` has
  two LODs. Use `--lod N` to view a single one.
- `MLDF` flags, MDEF `params` and MADF parameters have no glTF meaning. They are kept in the
  extras only.
- Materials are not double-sided. [I] The game presumably back-face culls, but this is unverified.
- `.glb`, `.obj` and `.png` output belongs in `out/` (gitignored) and must never be committed (AD-9).

## 7. Rider on bike: `dmbmx export-combo`

```bash
tools/testing/py.sh -m dmbmx export-combo assets/DaveMirra1.ZMD assets/DMbike01.zmd out/dm_on_bike.glb --anims 8
tools/testing/py.sh -m dmbmx export-combo <rider> <bike> <out.glb> [--anims all|8|1,5,10-12|none]
                                  [--motion-zal PATH] [--no-wheel-spin] [--gear-ratio 2.75]
```
Code: `tools/dmbmx/convert/combo.py`, `tools/dmbmx/cli/export_combo.py`. Tests: `tests/test_combo.py`.
Blender check and renders: `tools/blender/scripts/check_combo.py`.

**Layout.** Both models are exported as in §3, and the anim tracks are added as in `docs/formats/ANIM.md`
§4. The two are then merged into one scene:
`<rider>+<bike>` (root, `extras.dmbmx` = `{format: "dmbmx-combo", rider, bike, placement, wheels, clips}`) →
`HIPSDECK…` (bike) and `<RIDER>…` (rider; the upright root rotation is removed, because the Hips track
stands the rider up). There is one glTF animation per clip (`motion_NNN`), and each one drives:
- the 15 rider channels;
- the clip's bike channels (1-5);
- a **translation track on the rider root** (placement);
- two wheel rotation tracks.

Glb merging (`combo.merge_glbs`) offsets every index and concatenates the binary chunks. Animations with the
same name merge into one.

**Placement.** For each frame, the rider root is translated so that the mid-point of the rider's foot frames
lies on the mid-point of the bike's pedal frames.
- [V] In motion 8 every frame passes; the foot-to-pedal residual is 0.086-0.122 (with the engine's
  F·R·F the feet stand wider than the pedals; it was < 0.12 under the legacy arm table's rotations).
- A frame where a foot stays farther than 0.15 (0.12) from its pedal counts as "off the bike" (tricks).
  Over all 238 clips, 1,127 of 2,301 frames are on the pedals.
- **Off the pedals:** off-pedal frames follow the game's own placement (`combo.engine_offsets`, ANIM.md section 8.3):
  the clip's bike-section translation b is the bike's offset from the rider in engine units (100 x model
  units, Y and Z negated), so the rider root sits at F(-b) / 100 from the bike root plus a constant. The
  constant is calibrated on the clip's on-pedal frames (`ENGINE_OFFSET` = (0.007, -0.251, 0.044) when a clip
  has none). [V] Median residual against the feet placement: 0.023 with the per-clip constant, 0.054 with the
  global one (all 1,246 on-pedal frames). `rider_offsets(..., fill="interpolate")` keeps the combo exporter's linear
  interpolation between the nearest good frames. The combo root's `extras.dmbmx.placement` is now
  `"feet-on-pedals+engine"`.

**Wheels.** The clips have no wheel channels. With wheel spin on (the default), BackWheel and FrontWheel
turn about the bike's lateral axis by the accumulated crank (Sprocket) angle × the gear ratio (default
44/16 = 2.75, [I] typical BMX gearing). The rider.c pass: the game spins each wheel from physics (per-wheel angle +=
spin velocity, written into the BackWheel/FrontWheel channels, ANIM.md section 8.5), not from the crank, so
the crank coupling stays a stand-in. [V] In motion 8 the crank turns once (361°) and the wheels 2.75
times (993°).

**Verification** (`check_combo.py`, Blender 5.3 headless; `--riding` adds the on-bike thresholds):

| Clip | Feet → pedal bones (median / max) | Hands → bar mesh (median) | Back wheel | Result |
|---|---|---|---|---|
| 8 (pedalling) | 0.095 / 0.122 | 0.165 | turns | OK `--riding` |
| 15 | 0.097 / 0.104 | 0.124 | small | OK `--riding` |
| 59 (trick: bike spins, hands/feet off) | 0.35 | 0.67 | — | OK (no explosion); on-bike thresholds not applicable |

[V] Numbers with F conjugation (engine F·R·F + literal arm table). The combo exporter measured 0.054/0.077, 0.169; 0.064/0.091, 0.114;
0.19, 0.61 with the legacy arm table's rotations.

There are no explosions (bbox ratio ≤ 1.06). The Blender-measured wheel angle (1554° in motion 8) also
includes the frame-to-frame interpolation of the 24 fps resampling; the track itself has 993°.
Render: `out/frf/frf_strip.png` (ANIM.md §4); the combo exporter's was `out/combo/combo_strip.png`: rows are clips 8, 15 and 59, columns are Blender
frames 0, 12 and 24.

**Blender add-on.** File > Import > "Dave Mirra rider on bike (animated)" (`import_scene.dmbmx_combo`).
Pick the rider .zmd. The bike (default `DMbike01.zmd`) and `motion.zal` default to the same folder. Clips
default to 8. The operator builds the combo .glb in memory and loads it with Blender's glTF importer, so
the result is **view-only**: it is not linked to the lossless ZMD export.

**Limits.** The placement is by translation only (no rider tilt relative to the bike). Tricks where the feet
leave the pedals use the interpolated placement. With the engine rotations the hands sit 0.13-0.16
from the bar and the feet ~0.09 from the pedal frames (knees out; ANIM.md §3.1).

# 3ds Max tools: Dave Mirra Freestyle BMX file spec, parser and 3ds Max importers

Everything needed to read the game's files outside the game. The code in this folder has no dependency on
the rest of this repository; the format specification it implements is
[docs/formats/SPEC.md](../../docs/formats/SPEC.md) ("SPEC" below).

| Path | What |
|---|---|
| [`docs/formats/SPEC.md`](../../docs/formats/SPEC.md) | The file format specification: ZMD, MATI/MADF/TBMP/TPNG, level ZSD (SCEN/SCDF/SCFI), Mat._sd, ZAL (+ LZO1X), FORM (PLHD/PLYR, ZMHD/ZMAP), STRS, the motion bank, ZSND, SAVE.DAT, RenderWare chunks, the zhash name hash and the coordinate system |
| `parser/dmparse.py` | Simple reference parser: one file, Python 3.8+ standard library only |
| `maxscript/DaveMirraImport.ms` | 3ds Max importer in pure MAXScript (3ds Max **2017** or newer; no Python needed) |
| `python/dave_mirra_import.py` (+ `dmparse.py` next to it) | 3ds Max importer in Python / pymxs (3ds Max **2022** or newer) |

The game files themselves are not included. Point the tools at your own installation.

## The parser

```
python parser/dmparse.py tree     "C:\Games\Dave Mirra\assets\DaveMirra1.ZMD"   # chunk tree
python parser/dmparse.py dump     "C:\Games\Dave Mirra\assets\DMbike01.zmd"     # decoded fields as JSON
python parser/dmparse.py dump     "C:\Games\Dave Mirra\assets\AcclaimDirt"      # a level folder
python parser/dmparse.py textures "C:\Games\Dave Mirra\assets\DMbike01.zmd" out\tex
```

- `tree` and `dump` accept: `.zmd`, a level `.ZSD` or level folder, `Mat._sd`, `BSP._sd`/`temp.dff`
  (RenderWare tree), `.zal` (`motion.zal` also decodes its 238 clips), the miscdata/tables FORMs,
  `bin\audio\*.zsd` sound banks and `SAVE.DAT`.
- `dump --full` writes every array element instead of a summary.
- Python API: `read_zmd`, `read_mati`, `read_mat_sd`, `read_zsd`, `read_zsd_level`, `read_zal`,
  `read_form`, `read_plyr`, `read_zmap`, `read_strs`, `read_motion_bank`/`sample_motion`, `read_zsnd`,
  `read_save`, plus `zhash`, `skeleton`, `vertex_frames`, `upright_correction` and `channel_rotation`.
  Every function returns plain dicts and lists, and raises `ParseError` on bad input.

## 3ds Max: MAXScript importer (2017+)

1. Drag `maxscript/DaveMirraImport.ms` onto a 3ds Max viewport (or **Scripting → Run Script…**).
   A **Dave Mirra Importer** dialog opens.
2. Set the options:
   - **Scale**: default 100, so a rider is about 195 units tall.
   - **LOD**: 0. Only `DaveMirra_shadow.zmd` has a LOD 1.
   - **Stand riders upright**.
   - **Textures**: the folder to extract textures to. Empty means `%TEMP%\DaveMirraTextures\<model>`.
     The importer refuses any folder inside the game installation.
   - **Motion ids**: optional, e.g. `1,8,15`. The clips are keyed from `motion.zal` (see "Motion clips" below).
   - **Exact normals (Edit_Normals, slower)**: off by default. Puts the game's vertex normals on the meshes,
     for models and levels (see "Exact normals" below).
   - **Flip normals to faces (with Exact normals)**: off by default. Turns each exact normal that points
     against its faces around (see "Exact normals" below). Has no effect without Exact normals.
   - **Bones**: *aim at children* (default) or *game frame orientation* (see "Riders" below).
3. Click **Import ZMD…** and pick a model from the game's `assets` folder, e.g. `DaveMirra1.ZMD` or `DMbike01.zmd`.
   With motion ids, the importer uses `motion.zal` from the model's folder, or asks for it.
4. **Import Level (.ZSD)…** asks for a level folder, e.g. `assets\AcclaimDirt`. It imports the whole level
   (see "What you get"). This takes from several seconds to about a minute in MAXScript.

## 3ds Max: Python importer (2022+)

Keep `dave_mirra_import.py` and `dmparse.py` together in one folder. Then either:
- **Scripting → Run Script…** and pick `dave_mirra_import.py`, or
- from the MAXScript Listener: `python.ExecuteFile @"C:\path\to\tools\3dsmax\python\dave_mirra_import.py"`.

A Qt dialog opens with the same options as the MAXScript importer (Scale, LOD, upright, texture folder,
Motion ids, Exact normals, Flip normals to faces, Bones).

Scripted use:

```
python.Execute "import dave_mirra_import as d; d.import_zmd(r'C:\\Games\\Dave Mirra\\assets\\DaveMirra1.ZMD', scale=100)"
python.Execute "import dave_mirra_import as d; d.import_level(r'C:\\Games\\Dave Mirra\\assets\\AcclaimDirt', explicit_normals=True)"
```

`import_zmd` also takes `bone_aim_children=False` (game frame orientation) and `flip_normals=True`;
`import_level` takes `flip_normals=True`.

MAXScript entry points (after running the .ms once): `DM_ImportZmd path scale lod upright texDir`,
`DM_ImportZmdMotion path scale lod upright texDir motionPath motionIds` and `DM_ImportLevel folder scale texDir`,
each with an optional `exactNormals:true` and `flipNormals:true`; the two ZMD ones also take `boneAim:false`.

## What you get in the scene

- A root **Dummy** named after the model, e.g. `DAVEMIRRA1` or `HIPSDECK`. It carries the axis change:
  game Y-up → Max Z-up, plus the rider upright correction.
- **Riders:**
  - one Bone per frame (`Skin`, `Hips`, `Chest`, `Head`, … 20 bones), at the game's joints.
    - **Bones: aim at children** (default): each bone's local X axis points at its main child, and its
      length is the distance to it. The main child is the child that best continues the bone's incoming
      direction (parent joint → joint), e.g. `Chestx` → `Head`, not a shoulder. A bone whose parent is the
      root (`Hips`) takes the child with the most descendants (the spine, not a leg). A leaf (`Head`, hands,
      feet) continues its parent's direction at half its length. Bones are at least 2 cm (× scale).
      Each bone's transform is a constant rotation O times the game frame (`O * frame`); the motion keys go
      through the same O, so the skin and the animation are exactly the same as with the game frames.
    - **Bones: game frame orientation**: each bone has its game frame's orientation (the game's bind
      frames are mostly world-aligned, so the bones all point along +X) and a fixed length.
  - the body as an Editable Mesh with a **Skin** modifier, every vertex 100% on its one bone (the game's
    skinning is rigid).
- **Bikes:** one **Dummy** per frame (`Hipsdeck`, `FrontWheel`, `BackWheel`, `Handlebars`, `Sprocket`,
  pedals, grips), each of the 7 parts parented to its frame, so rotating `FrontWheel` turns the wheel.
- **Meshes:**
  - UVs on channel 1;
  - prelit vertex colours on channel 0 and alpha on channel −2 (shadows);
  - one smoothing group;
  - a Multimaterial with one Standard material per game material slot. The diffuse map is the extracted
    texture, and PNG textures also get it as an opacity map (alpha).
- **Levels** (root Dummy named after the folder; the level's world origin is placed at the Max origin):
  - `World`: one Editable Mesh with all world sectors. It has UVs, prelit (baked) vertex colours on
    channel 0, and a Multimaterial with one sub-material per world material slot.
    - Textures come from `Mat._sd`. BMPs that use the game's colour key (pure magenta 255,0,255) are
      written as PNGs with those pixels transparent, and get an opacity map (palm leaves, fences).
  - `Props`: every placed prop. Each definition's model is built once, and the other props using it are
    **instances**, placed and rotated as in the game. The user property `dmbmx_def_id` holds the definition.
    The rotation is the stored matrix as is. Before v1.2 (Python) / v1.3 (MAXScript) the yaw had the wrong
    sign (+80° showed as −80°). The game renders props through its mirrored quaternion converter
    (SPEC §2.6).
  - `Tags`: the collision faces, one mesh per tag code (`tag_1f41`, …; user property `dmbmx_tag_code`),
    **hidden**. Unhide them to see grind/trick/surface zones.
  - `ScenePoints`: Dummies `Player`, `PlayerT`, `Globals` and 3 × `SpotLight` (user property
    `dmbmx_point_type`).
  - `Panorama`: the sky model, at the world origin.
  - The root carries the component list (`dmbmx_components`); the Python importer also records the world
    origin (`dmbmx_world_origin`).
- **Exact normals** (both importers, when the option is on): an **Edit_Normals** modifier on each mesh
  that has game normals, every vertex's normal set to the game's (scaled to length 1) and made explicit.
  - Models: every mesh, below the Skin modifier on riders.
  - Levels: `World` (the sector normals), each PropDef model (its instances share the modifier) and the
    `Panorama`. Tag meshes get none. A zero-length game normal (a few in AcclaimStreet, GreenvilleVert,
    ProTourStreet, WoodwardVert and AcclaimStreet's prop models) is left to the smoothing group.
  - The normals are in each mesh's own space, so they come out right after the node transforms and the
    level re-centring (the tests check them in Max world space).
  - **Flip normals to faces** (optional, with Exact normals): a vertex normal is negated when it points
    against its faces. The faces vote with their CCW normals weighted by area (the sum of (b − a) × (c − a)
    over the vertex's triangles); the normal flips when its dot product with that sum is negative. So when
    a vertex's faces disagree, the larger area wins; a vertex in no face (or whose faces cancel exactly)
    keeps its normal. This fixes the floors of ProTourDirt and WoodwardStreet (see Known limits); with it,
    every level's World agrees with the winding on more than 95% of its faces.
- **Motion clips** (both importers, when you give motion ids): every frame node (bone or dummy) gets one
  transform key per game frame, from Max frame 0. Channel frames (SPEC §8.4) get the game's rotation with
  their bind translation; the other frames keep their bind pose. Several ids are keyed one after the other
  from frame 0, so a later clip overwrites an earlier one's keys (import one clip per model to view it).
  An animated rider has no upright correction on its root: the Hips channel stands it up.
- Model data keeps the game's coordinates under the root dummy. To export back to the game, use the
  Blender add-on or `dmbmx` (see the main repository): these importers are one-way.

## Feature matrix

| Feature | MAXScript | Python |
|---|---|---|
| ZMD meshes, UVs, vertex colours, smoothing | yes | yes |
| Exact game normals (Edit_Normals; levels: World, PropDefs, Panorama) | yes (optional) | yes (optional) |
| Multimaterial + extracted TBMP/TPNG bitmaps, PNG alpha | yes | yes |
| Rider skeleton as Bones + Skin (rigid, 1 bone per vertex) | yes | yes |
| Bones aimed at their children (or game frame orientation) | yes (default) | yes (default) |
| Flip normals to faces (with exact normals) | yes (optional) | yes (optional) |
| Bike parts parented to frame dummies | yes | yes |
| LOD choice | yes | yes |
| Axis conversion + rider upright | yes | yes |
| Motion clips from motion.zal as keyframes | yes | yes (riders: both drop the upright root rotation, see SPEC §8.4) |
| Level world mesh + Mat._sd textures, colour key as opacity | yes | yes |
| Level props as placed/rotated instances of their PropDef models | yes (yaw fixed in v1.3) | yes (yaw fixed in v1.2) |
| Level tag faces (hidden, one mesh per tag code) | yes | yes |
| Level scene points (Player, PlayerT, Globals, SpotLights) as dummies | yes | yes |
| Level panorama (sky model) | yes | yes |

## Known limits

- **Not run in a real 3ds Max yet.** 3ds Max is not installed on the development PC, so the importers are
  checked in two ways:
  - The Python importer runs against a recording mock of `pymxs` that checks every call's arguments:
    1-based indices, ranges, existing bitmap files, skin bone ids.
  - The MAXScript importer is **executed by a MAXScript-subset interpreter** against the same mock.

  For every one of the 115 models, both produce the right vertex/face/UV counts, materials and bitmaps,
  bone hierarchy and skin weights, and every vertex lands at the game's world position (checked against
  the main library). Real-Max behaviour (Skin modifier evaluation, Edit_Normals, Qt dialog, bone display)
  is a manual test in 3ds Max (passed), and another one for exact normals.
- **Bones** aim at their main child by default (see "Riders"). The joint positions are exact, and the skin
  and motion are unchanged: every vertex lands where it does with game-frame bones, at the bind pose and at
  every key (tested). The bone orientation and length are cosmetic, and a different main-child rule would
  give a different but equally valid rig. With *game frame orientation* the bones point along their
  frame's X axis, which is the game's own bind orientation.
- **Motion keys** have one key per game frame at 34 ms, keyed on integer Max frames (about 29.4 game fps on
  a 30 fps timeline). Root motion (translation) is not imported. The animation range is not changed: the
  clips are 2-43 frames long, inside Max's default 0-100.
- **MAXScript motion** decodes `motion.zal` itself (its own LZO1X decompressor, 142 KB → 202 KB, then the
  clip decoder). The first motion import of a 3ds Max session takes a few seconds [I]; the decoded bank is
  cached until Max is closed (or `motion.zal` changes size). MAXScript floats are 32-bit, so its keys can
  differ from the Python importer's in about the 6th-7th significant digit [I]; the tests compare the two
  at 1e-5.
- **Levels:**
  - Nothing is triangulated or welded differently from the game: the world mesh is the game's sectors
    concatenated, and it is smoothed as one smoothing group.
  - Without "Exact normals" the world is shaded from its smoothing group. The game lights levels with the
    baked vertex colours, not with these normals.
  - The RW collision blocks and the tag kd-tree are not imported (they're not needed for viewing).
  - Levels are large: with Scale 100 a level spans about 12,000 Max units.
- **Exact normals** cost one Edit_Normals call per face corner plus two per normal: about 2,600 calls for
  a bike, 5,400 for a rider and 32,000-47,000 for a level (World ~95% of them). Both importers set them in
  one pass per mesh, with the modifier current in the Modify panel once, and undo and redraw off. Expected:
  about a second for a model and a few seconds to ~20 s extra for a level [I], to be measured in 3ds Max.
  - They are the game's data, kept as is. In **ProTourDirt** and **WoodwardStreet** most floor normals point
    down (against the face winding: 85% and 70% of the world's faces), so with exact normals those floors
    shade as if lit from below. The other levels agree with the winding on 91-100% of the world's faces.
    **Flip normals to faces** fixes them (it changes the game's data, so it is off by default). On models it
    helps less: DaveMirra.ZMD goes from 77% to 84%, because the rest are faces whose own winding disagrees
    with their neighbours'.
  - The modifier selects each mesh and opens the Modify panel while it works, so the last imported mesh
    stays selected.
- Two models are empty (0 bytes) in the original game: `TroyMcMurray1v2/1v3.ZMD`. Importing them shows
  a message.

## Tests (repository)

| File | Covers |
|---|---|
| `tests/test_3dsmax_parser.py` | dmparse against the main `dmbmx` library on the whole install: 117 models, 1,349 ZAL assets, 238 motion clips frame by frame, 22 sound banks, SAVE.DAT |
| `tests/test_3dsmax_levels.py` | dmparse's level readers against `dmbmx` on all 12 levels: world sectors, materials, props + placement, prop-def and panorama models, scene points, tag faces, colour key |
| `tests/test_3dsmax_max_python.py` | the Python importer on every model and every level, with the pymxs mock |
| `tests/test_3dsmax_maxscript.py` | the MAXScript importer source on every model and every level, through `tests/maxscript_interp.py` + the mock |
| `tests/test_3dsmax_normals.py` | exact normals: the Edit_Normals mock's rules, both importers on a synthetic mesh (missing / zero / non-unit normals) and on real models and a level (same normals), the level-normal facts. The every-model and every-level imports above run with exact normals on (`max_checks.check_normals`: right vertices, unit length, = dmparse in Max world space, winding kept face by face) |
| `tests/test_3dsmax_props_bones.py` | The prop rotation pinned to the exe (the mirrored quaternion converter) and to the game's prop collision boxes on all 12 levels; glTF prop matrices; bones aimed at children (rule, both importers, joints/skin/keys identical to game-frame bones at every key); the createBone/Skin mock rules; "flip normals to faces" (rule, every level, MAXScript = Python) |
| `tests/test_3dsmax_maxscript_motion.py` | the MAXScript motion path: LZO1X/ZAL/clip decoder = dmparse on all 238 clips frame by frame, and keyed rider/bike imports = the Python importer's keys |

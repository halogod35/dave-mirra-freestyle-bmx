# Editing Dave Mirra models in Blender (the `io_dmbmx` add-on)

This guide shows how to open a rider or bike from Dave Mirra Freestyle BMX in Blender, change it, save it
back into the game's own format (`.zmd`), and try it in the test copy of the game. You don't need to
program. You need Blender 5.3 (it's installed at `C:\Program Files\Blender Foundation\Blender 5.3\`).

> **Golden rule:** never put files into the real game folder (`F:\Projects\Dave Mirra freestyle BMX\assets`).
> Always test in the **test copy**, `F:\Projects\Dave Mirra freestyle BMX\_testgame`. The add-on refuses
> to save into the real game folder.

---

## 1. Install the add-on (once)

1. Get the add-on file: `out\io_dmbmx.zip` in your checkout of this repository.
   If it's missing, run `tools/testing/py.sh tools/blender/build_blender_addon.py` to make it.
2. Start Blender. Open **Edit → Preferences → Get Extensions**.
3. Click the small **⌄** arrow at the top right and choose **Install from Disk…**. Pick `io_dmbmx.zip`.
4. "Dave Mirra ZMD (.zmd)" now appears in the list with its checkbox ticked. Close Preferences.

You now have:
- **File → Import → Dave Mirra ZMD (.zmd)**
- **File → Export → Dave Mirra ZMD (.zmd)**
- A **Dave Mirra** tab in the 3D view's sidebar (press **N** to show the sidebar) with the same buttons
  plus **Replace Texture**.

To update the add-on later, install the new zip the same way.

## 2. Open a rider or a bike

1. Start with an empty scene: **File → New → General**, then select the default cube and delete it (**X**).
2. **File → Import → Dave Mirra ZMD (.zmd)**, go to `F:\Projects\Dave Mirra freestyle BMX\assets` and pick a file:
   - Riders: `DaveMirra1.ZMD` … `DaveMirra5.zmd`, `ChadKagy1.ZMD`, … The number is the rider's outfit.
   - Bikes: `DMbike01.zmd` … (`DM` = Dave Mirra; `CK` = Chad Kagy, etc.)
3. Options on the right side of the file browser (the defaults are fine):
   - **Stand riders upright**: riders are stored lying down in the game files; this stands them up.
   - **Scale**: only changes how big the model looks in Blender, not the game file.
   - **Texture folder**: if you choose a folder, the textures are saved there as `.png` pictures, so
     you can paint them in any paint program. Leave it empty to keep them inside the Blender file.
   - **LOD**: leave it at −1 (all).
4. Click **Import Dave Mirra ZMD**.

What you get (look at the Outliner, top right):

| Model | In Blender |
|---|---|
| Rider | An empty called e.g. `DAVEMIRRA1` (the model's anchor), an **armature** (the skeleton: Hips, Chest, Head, LeftHand, …) and one **mesh** (the body), textured |
| Bike | An empty `HIPSDECK`, small sphere empties for each moving part (`FrontWheel`, `BackWheel`, `Handlebars`, pedals, …) and 7 meshes, one per part |

Don't rename, delete or re-parent these objects. The add-on uses them to rebuild the game file.

### Watch a rider ride (view only)

**File > Import > Dave Mirra rider on bike (animated)** (also in the Dave Mirra sidebar tab):
1. Pick a rider `.zmd`.
2. In the panel on the right, set the **Bike** (default `DMbike01.zmd` in the same folder), **motion.zal**
   (default: same folder), and **Clips** (default `8`, the pedalling clip; `1,5,10-12` or `all` also work).
3. Press Space to play.

The rider sits on the bike, and the wheels turn with the cranks. This import uses Blender's own glTF importer,
so you can't export it back to a game file. Use the normal **Dave Mirra ZMD** import for editing. Details:
`docs/tools/export.md` §7.

## 3. Make changes

Save your work as a normal Blender file (**File → Save**) whenever you like. You can close Blender and
export later.

### Things that work

| Change | How |
|---|---|
| **Reshape a part** (bigger wheels, longer handlebars, a different helmet) | Select the mesh, press **Tab** (Edit Mode), select vertices and move (**G**), scale (**S**) or rotate (**R**). You can add or delete faces too. |
| **Scale or move a whole bike part** | In Object Mode, select the part's mesh (e.g. the front wheel) and scale/move it. |
| **Move a part's pivot** (where a wheel turns) | Move the part's sphere empty, e.g. `FrontWheel`. |
| **Change which bone moves a rider vertex** | Weight Paint mode, or Edit Mode → select vertices → Object Data properties → Vertex Groups → **Assign**. The game uses one bone per vertex: the strongest one wins. |
| **Recolour / repaint a texture** | **Texture Paint** mode in Blender; or, if you imported with a Texture folder, edit the `.png` in any paint program and save it. In both cases, keep the picture the same size. |
| **Replace a texture with your own picture** | Sidebar (N) → Dave Mirra → **Replace Texture**: pick the texture and your `.png`/`.bmp`. It's resized to fit automatically. |
| **Transparency** | In bike/rider BMP textures, the exact colour **magenta (255, 0, 255)** shows as transparent in the game. When a BMP texture is saved, transparent pixels in your picture are turned into magenta for you (PNG textures keep real transparency). |

### Things that don't work (the export stops and tells you why)

- Adding or deleting **bones**, or adding or deleting bike **parts** (objects). The game's animations depend on them.
- Adding **new materials**. Reuse the model's existing material slots.
- Deleting the **UV map**. The game needs it to place the texture.
- More than **65,535 vertices** in one part.
- Rider vertices that belong to **no bone** at all.

### Good to know

- **Posing** the rider (Pose Mode) is ignored. Only the rest shape is exported. Animations live elsewhere in the game.
- **Modifiers** (Subdivision, Mirror, …) are **not** applied automatically. Apply them first (Ctrl+A → Apply in the modifier panel).
- **Normals** (lighting direction): by default the game's original normals are kept, which is best for
  small edits. After big shape changes, choose **Normals: Recalculate** when exporting.
- **Textures with 16 or 256 colours:** if you edit one, it's saved as a full-colour picture, so your
  colours stay exact. The game loads either kind. Choose "Keep palette" in the export options to stay
  with the original colours instead.

## 4. Export back to a game file

1. Click any object of the model (e.g. the body or a wheel).
2. **File → Export → Dave Mirra ZMD (.zmd)**, or the **Export** button in the sidebar.
3. Choose where to save it, e.g. your Documents folder. **Keep the original file name** (e.g. `DMbike01.zmd`).
   The game looks for that name.
4. Options:
   - **Copy into _testgame**: also puts the file straight into the test copy of the game,
     replacing the original there. Tick this for a quick test.
   - **Triangle layout**, **Edited 16/256-colour textures**, **Normals**: leave the defaults unless you know why.
   - **Fail on warnings**: stop instead of saving when something looks risky.
5. Click **Export Dave Mirra ZMD**. The status bar shows what changed. If you changed nothing, it says
   "identical to the original": the file is byte-for-byte the same as the game's.

If the export fails, the message says what to fix (for example "3 vertices are not assigned to any bone").

## 5. Try it in the game

1. Make sure your file is in `F:\Projects\Dave Mirra freestyle BMX\_testgame\assets\` with the original name.
   The **Copy into _testgame** option does this for you. Otherwise copy it there yourself and replace the existing file.
2. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
3. Pick the rider or bike you changed. For example, `DMbike01.zmd` is Dave Mirra's first bike.
4. When you're done, put the test copy back to normal. In Git Bash, run:
   ```bash
   bash tools/testing/restore_testgame.sh      # run from the repository folder
   ```
   It ends with "OK: N files match" (N = the number of files in your manifest).

The game doesn't check model files with a checksum, so edited models load as long as they are valid.

## 6. Troubleshooting

| Problem | Fix |
|---|---|
| "Select an object of an imported Dave Mirra model first" | Click a part of the model in the viewport or the Outliner, then export again. |
| "refusing to write into the game install" | Save somewhere else and tick **Copy into _testgame**. |
| "…is an empty (0-byte) ZMD" | `TroyMcMurray1v2/1v3.ZMD` are empty in the original game. Nothing to edit. |
| "…new materials are not supported" | You added a material slot. Remove it and use the existing slots. |
| Texture looks wrong in game | Keep it the same size as the original, and remember that magenta = transparent. |
| The rider lies on the floor | You turned off "Stand riders upright". That's only how it looks in Blender. The export is the same either way. |

---

## For developers

- Code: `tools/blender/io_dmbmx/` (`__init__.py` operators/UI, `zmd_import.py`, `zmd_export.py`). The
  `dmbmx` library is vendored into the zip under `vendor/` by `tools/blender/build_blender_addon.py` (reproducible
  zip; fails if `dmbmx` imports anything outside the stdlib).
- **Import:** `dmbmx.convert.gltf.export_glb` runs in memory, and the add-on builds Blender objects from
  that glTF directly. Blender's glTF importer isn't used. The glTF JSON (with all `extras.dmbmx` rebuild
  data, docs/tools/export.md §4) is stored on the root empty as the custom property `dmbmx_gltf`, along
  with `dmbmx_uid` (links images), the source path and its SHA-256.
- **Coordinates:** mesh and frame data stay in exact RW model space. Only the root empty carries
  `scale · Rx(+90°) · upright`, which takes RW/glTF Y-up to Blender Z-up plus the rider correction.
- **Export:** `zmd_export.build_glb` rebuilds every mesh accessor (POSITION, NORMAL, TEXCOORD, COLOR_0,
  JOINTS/WEIGHTS, indices per material slot) and every image from Blender. Frame/bone/mesh transforms
  are written only when they differ from the transform recorded at import (`dmbmx_rest`). It then calls
  `dmbmx.convert.gltf_import.import_glb`, so BinMesh, Bone and bounding-sphere regeneration, texture
  re-encoding and all refusals are the glTF importer's (docs/tools/import.md).
- **Exactness:**
  - Positions are float32 in Blender, so they are exact.
  - Blender stores custom normals as 16-bit, so the original normals are kept in the point attribute
    `dmbmx_normal`. The `keep` mode uses them; `recalc` uses Blender's.
  - UVs are stored as `(u, 1 − v)`, which isn't exact in float32, so the original V is kept in the corner
    attribute `dmbmx_uvN_v` and used wherever Blender's value still equals `1 − v`.
  - Images are exported as the original PNG bytes unless they were painted, reloaded or replaced.
  - Corner seams added in Blender split vertices; existing vertices keep their index.
- **Blender flushes denormal floats to zero** in its Python ([V]: `bike_hires.zmd`'s two denormal frame
  floats became −0.0). Frame records are therefore taken from the file's raw bytes on export
  (`gltf._raw_frame_records`) and written back verbatim on import when unchanged
  (`gltf_import._patch_frames`). No other corpus float is denormal.
- **Tests:**
  - `tools/blender/scripts/test_addon.py [--zip out/io_dmbmx.zip]` runs headless. [V] 2026-11-12, from the
    built zip: **115/115 byte-identical** import → export through the real operators, the 2 empty files
    refused, and 12 scenarios passing: upright, bike wheel scale, frame move, face deletion, Edit Mode
    no-op, rider vertex move, re-weighting, texture replace, texture paint (palettised → 24-bit),
    refusing install paths, Copy into _testgame (unedited, so `_testgame` stays pristine), and
    save/reopen of the .blend. It takes about 21 s.
  - In pytest: `tests/test_blender_addon.py` (zip/manifest/stdlib checks and the denormal path). The
    Blender run there is opt-in with `DMBMX_RUN_BLENDER=1`.
  - [V] The zip installs through Blender's own **Install from Disk** path
    (`bpy.ops.extensions.package_install_files`, tested with a temporary `BLENDER_USER_RESOURCES`), and
    File → Import works from the installed copy.

# dmbmx: command line and library reference

`dmbmx` is the Python package and command-line tool for the files of Dave Mirra Freestyle BMX (PC, 2000).
It reads every format losslessly, writes them back byte for byte, exports and imports models (glTF / OBJ),
edits saves, menus, texts, textures, sounds and archives, and writes a patched copy of the exe. The library is
pure standard library (numpy is optional), so the Blender add-on can vendor it.

This page lists every command. The formats themselves are described in [`docs/formats/`](../../docs/formats/)
and the bigger workflows in [`docs/tools/`](../../docs/tools/); each section below links to the relevant one.

## Contents

- [Install and run](#install-and-run)
- [Command table](#command-table)
- [Archives: `zal`](#archives-zal)
- [Models: `zmd`, `export`, `export-combo`, `import`](#models-zmd-export-export-combo-import)
- [Textures: `tex`](#textures-tex)
- [Levels: `level`](#levels-level)
- [Animation: `motion`](#animation-motion)
- [Replays: `replay`](#replays-replay)
- [Audio: `audio`](#audio-audio)
- [Saves: `save`](#saves-save)
- [Menus, texts and HUD fonts: `gui`](#menus-texts-and-hud-fonts-gui)
- [Exe and load-time checks: `patch`, `integrity`](#exe-and-load-time-checks-patch-integrity)
- [Anything: `info`](#anything-info)
- [Library use](#library-use)

## Install and run

From the repository root (Git Bash on Windows; any Python 3.10+, the project uses 3.15):

```bash
python -m venv .venv
.venv/Scripts/python.exe -m pip install -e .            # or -e ".[test]" for the test suite
.venv/Scripts/dmbmx --help                              # the console script installed by pip
.venv/Scripts/python.exe -m dmbmx --help                # the same, as a module
tools/testing/py.sh -m dmbmx --help                     # the same, without installing (puts tools/ on the path)
```

`dmbmx <command> --help` and `dmbmx <command> <subcommand> --help` print the options of every command.

**`DMBMX_GAME_DIR`** is the game install (read only). Commands with a `--game-dir` option or an optional
file argument (`export --all`, `export --anims`, `export-combo`, `motion`, `replay`) use it as their default,
falling back to `F:\Projects\Dave Mirra freestyle BMX`. The examples below use:

```bash
export DMBMX_GAME_DIR="C:/Games/Dave Mirra Freestyle BMX"
G="$DMBMX_GAME_DIR"
```

**Never write into the game folder.** Every command that writes takes an explicit output path and refuses to
overwrite its input; write to `out/` and test the result in a copy of the game (`docs/tooling.md`, Game trees).

## Command table

| Command | Subcommands | What |
|---|---|---|
| [`zal`](#archives-zal) | `list`, `extract`, `pack`, `replace` | `.zal` asset libraries |
| [`zmd`](#models-zmd-export-export-combo-import) | `info`, `dump-obj`, `extract-textures` | ZMD model files |
| [`export`](#models-zmd-export-export-combo-import) | | ZMD -> glTF `.glb` (+ OBJ), optionally with animations |
| [`export-combo`](#models-zmd-export-export-combo-import) | | a rider on a bike as one animated `.glb` |
| [`import`](#models-zmd-export-export-combo-import) | | edited `.glb` -> ZMD |
| [`tex`](#textures-tex) | `list`, `export`, `replace` | textures in ZMD, `Mat._sd` and `.zal` |
| [`level`](#levels-level) | `info`, `dump-obj`, `export`, `rewrite` | level folders |
| [`motion`](#animation-motion) | `list`, `dump` | the motion bank `motion.zal` |
| [`replay`](#replays-replay) | `list`, `dump` | the demo replays `replay.zal` |
| [`audio`](#audio-audio) | `list`, `extract`, `replace`, `ztrk-dump` | sound banks `bin/audio/*.zsd` |
| [`save`](#saves-save) | `info`, `fix-crc` (alias `resign`), `list-fields`, `get`, `set`, `unlock-all`, `complete-all` | `SAVE.DAT` |
| [`gui`](#menus-texts-and-hud-fonts-gui) | `list`, `export`, `import`, `texts`, `text-set`, `text-import`, `hud-export`, `hud-import` | menu art, screens, fonts, texts |
| [`patch`](#exe-and-load-time-checks-patch-integrity) | `list`, `apply`, `nocrc` | patched copy of `NgBMX.exe` |
| [`integrity`](#exe-and-load-time-checks-patch-integrity) | `check`, `fix-png`, `resign-zsd` | the game's load-time checks |
| [`info`](#anything-info) | | magic / first chunk tag of any file |

## Archives: `zal`

Format: [ZAL.md](../../docs/formats/ZAL.md).

| Subcommand | Arguments and options |
|---|---|
| `zal list` | `file`; `-r`/`--recursive` also lists nested `.zal` entries |
| `zal extract` | `file outdir`; `-r`/`--recursive` unpacks nested `.zal` entries; `--raw` writes the stored (still compressed) bytes, not packable |
| `zal pack` | `dir out`: rebuild a `.zal` from an `extract` directory (edited files are re-encoded); `-v`/`--verbose` lists every asset and whether it was kept |
| `zal replace` | `file id payload out`: `id` is a group id (`0x7531`, `30001`), a name (hashed with zhash) or a nested path like `0/0xe003` |

```bash
dmbmx zal list -r "$G/assets/Intwrap.zal"
dmbmx zal extract "$G/assets/material.zal" out/material
dmbmx zal pack out/material out/material.zal -v
dmbmx zal extract "$G/assets/miscdata.zal" out/misc         # files named by id: 00007531.bin, 00007919.strs, ...
dmbmx zal replace "$G/assets/miscdata.zal" 0x7531 out/misc/00007531.bin out/miscdata.zal
```

## Models: `zmd`, `export`, `export-combo`, `import`

Formats: [ZMD.md](../../docs/formats/ZMD.md), [ANIM.md](../../docs/formats/ANIM.md). Workflows:
[export.md](../../docs/tools/export.md), [import.md](../../docs/tools/import.md),
[blender-addon.md](../../docs/tools/blender-addon.md).

| Command | Arguments and options |
|---|---|
| `zmd info` | `file`; `--tree` prints the full chunk tree |
| `zmd dump-obj` | `file out`: quick OBJ (+MTL+textures) of one LOD; `--lod N`; `--local` leaves the parts at the origin |
| `zmd extract-textures` | `file outdir`: the embedded TBMP/TPNG images as-is |
| `export` | `<file.zmd> <out.glb>`, or `--all <outdir>`; `--game-dir DIR` (install scanned by `--all`); `--lod N` (one LOD only; not restorable losslessly); `--obj` (also `.obj/.mtl/.png`); `--no-upright` (raw RW model space); `--anims IDS` (`all` or `1,5,10-12`: motion.zal clips as glTF animations); `--motion-zal FILE` |
| `export-combo` | `rider bike out`; `--anims IDS` (default `8`, the pedalling clip; `none` = static); `--motion-zal FILE`; `--game-dir DIR`; `--no-wheel-spin`; `--gear-ratio R` (default 2.75) |
| `import` | `glb out`; `--base ORIG.zmd` (checked against the SHA-256 in the `.glb`, supplies LODs a `--lod` export left out); `--binmesh {auto,strips,lists}`; `--palettised {promote,quantize}` (edited 4/8-bit BMPs); `--strict` (warnings are errors) |

```bash
dmbmx zmd info "$G/assets/DMbike01.zmd"
dmbmx zmd dump-obj --lod 0 "$G/assets/DMbike01.zmd" out/bike/bike.obj
dmbmx zmd extract-textures "$G/assets/DaveMirra1.ZMD" out/dm1_tex
dmbmx export "$G/assets/DMbike01.zmd" out/bike.glb --obj
dmbmx export --all out/glb                                   # every .zmd of the install
dmbmx export "$G/assets/DaveMirra1.ZMD" out/dm1.glb --anims 1,5,8
dmbmx export-combo "$G/assets/DaveMirra1.ZMD" "$G/assets/DMbike01.zmd" out/combo.glb --anims 8
dmbmx import out/bike.glb out/DMbike01.zmd --base "$G/assets/DMbike01.zmd"   # after editing bike.glb
```

## Textures: `tex`

Format: [TEXTURE.md](../../docs/formats/TEXTURE.md).

| Subcommand | Arguments and options |
|---|---|
| `tex list` | `file`: every texture with format, size and MADF fields |
| `tex export` | `file outdir` (`.zmd`, `Mat._sd` or `.zal`); `--png` also writes RGBA PNGs; `--colorkey` (with `--png`) makes 255,0,255 BMP pixels transparent as the game does |
| `tex replace` | `file name image out` (`.zmd` or `Mat._sd`; name case-insensitive, e.g. `NAME_801_1`; `.bmp` or `.png`); `--resize` scales to the original size; `--keep-size` allows a different size |

```bash
dmbmx tex list "$G/assets/DMbike01.zmd"
dmbmx tex export --png --colorkey "$G/assets/WoodwardDirt/Mat._sd" out/wd_tex
dmbmx tex export --png "$G/assets/DMbike01.zmd" out/bike_tex      # edit out/bike_tex/TIRE.png
dmbmx tex replace --resize "$G/assets/DMbike01.zmd" tire out/bike_tex/TIRE.png out/DMbike01.zmd
```

## Levels: `level`

Format: [LEVEL.md](../../docs/formats/LEVEL.md); collision: [physics.md](../../docs/re/physics.md).

| Subcommand | Arguments and options |
|---|---|
| `level info` | `dir` (e.g. `assets/AcclaimDirt`); `--props` lists every prop instance; `--points` every scene point |
| `level dump-obj` | `dir out`: OBJ (+MTL+PNG) of the world and props; `--no-props`; `--tags` adds the tagged collision faces |
| `level export` | `dir out`: glTF with world, props as nodes, tags and scene points (extras); `--no-props`, `--no-tags`, `--no-points`, `--panorama` (include the sky) |
| `level rewrite` | `dir out`: read and write the whole folder (re-encodes every file, re-signs the ZSD); `out` must not be the input |

```bash
dmbmx level info --props "$G/assets/WoodwardDirt"
dmbmx level export --panorama "$G/assets/WoodwardDirt" out/woodward_dirt.glb
dmbmx level dump-obj --tags "$G/assets/AcclaimDirt" out/acclaim/acclaim.obj
dmbmx level rewrite "$G/assets/WoodwardDirt" out/WoodwardDirt
```

## Animation: `motion`

Format: [ANIM.md](../../docs/formats/ANIM.md). glTF animation output is `export --anims` / `export-combo`.

| Subcommand | Arguments and options |
|---|---|
| `motion list` | `[file]` (default: the install's `assets/motion.zal`): one line per motion |
| `motion dump` | `id [file]`: decoded per-frame channel values as JSON; `--quat` adds the local rotation quaternions |

```bash
dmbmx motion list
dmbmx motion dump --quat 8 > out/motion8.json
```

## Replays: `replay`

Format: [REPLAY.md](../../docs/formats/REPLAY.md).

| Subcommand | Arguments and options |
|---|---|
| `replay list` | `[file]` (default: the install's `assets/replay.zal`): layout, size, frames, events per record |
| `replay dump` | `id [file]`: header and events as JSON; `--frames` adds the per-frame input |

```bash
dmbmx replay list
dmbmx replay dump --frames 0 > out/replay0.json
```

## Audio: `audio`

Format: [AUDIO.md](../../docs/formats/AUDIO.md); how the game plays it: [sound.md](../../docs/re/sound.md).

| Subcommand | Arguments and options |
|---|---|
| `audio list` | `bank`: the sample files of a bank |
| `audio extract` | `bank outdir`: every sample as WAV; `--id N` (repeatable) only these |
| `audio replace` | `bank id wav output`: replace sample `id`, encoded to the bank's native format |
| `audio ztrk-dump` | `bank`: disassemble the ZTRK sequencer tracks; `--track N` (repeatable) |

```bash
dmbmx audio list "$G/bin/audio/sfxcom.zsd"
dmbmx audio extract "$G/bin/audio/sfxcom.zsd" out/sfxcom
dmbmx audio replace "$G/bin/audio/sfxcom.zsd" 3 out/sfxcom/sfxcom_003.wav out/sfxcom.zsd
dmbmx audio ztrk-dump --track 0 "$G/bin/audio/sfxgame.zsd"
```

## Saves: `save`

Format: [SAVE.md](../../docs/formats/SAVE.md); load-time checks: [integrity-checks.md](../../docs/re/integrity-checks.md).

| Subcommand | Arguments and options |
|---|---|
| `save info` | `file`: decode and say whether the game would accept it; `--scores` lists all 12x24 high-score entries |
| `save fix-crc` (alias `save resign`) | `input output`: fix the header (magic + size); SAVE.DAT has no CRC |
| `save list-fields` | `[file]`: editable field patterns (type, range, evidence), with values when a file is given; `--grep TEXT`; `--max N` (default 24) |
| `save get` | `file field [field ...]` |
| `save set` | `file field value [field value ...]`; `-o`/`--output OUT` or `--in-place` (one is required); `--lenient` accepts a bad header |
| `save unlock-all` | `file`; `-o`/`--output OUT` or `--in-place`; `--lenient`: all riders, levels, styles, bikes, cheats and movies |
| `save complete-all` | `file`; `-o`/`--output OUT` or `--in-place`; `--lenient`: every challenge and competition for every rider, with the game's rewards (raise-only) |

```bash
dmbmx save info --scores "$G/save/SAVE.DAT"
dmbmx save list-fields --grep cheat "$G/save/SAVE.DAT"
dmbmx save get "$G/save/SAVE.DAT" cheats_unlocked volume_0d
dmbmx save set "$G/save/SAVE.DAT" cheats_unlocked 255 volume_0d 9 -o out/SAVE.DAT
dmbmx save unlock-all "$G/save/SAVE.DAT" -o out/SAVE.DAT
dmbmx save fix-crc out/SAVE.DAT out/SAVE_fixed.DAT          # after a hex edit
```

## Menus, texts and HUD fonts: `gui`

Workflow: [menu-modding.md](../../docs/tools/menu-modding.md); formats: [ZAL.md](../../docs/formats/ZAL.md) §3.1-3.3,
[frontend.md](../../docs/re/frontend.md), [hud.md](../../docs/re/hud.md).

| Subcommand | Arguments and options |
|---|---|
| `gui list` | `intwrap`: GUI groups and assets; `--screen ID` (e.g. `0xA005`, the main menu) lists one screen's elements |
| `gui export` | `intwrap outdir`: art as PNG, screens and fonts as JSON; `--screen ID` (repeatable) only that screen's group |
| `gui import` | `intwrap dir out`: rebuild `Intwrap.zal` from an edited export directory |
| `gui texts` | `misc` (`MiscData.ZAL`): print the STRS text tables; `--table ID` (repeatable); `--json FILE` exports instead |
| `gui text-set` | `misc text_id text out`: change one text (e.g. `0x24`, PROQUEST); `--table ID` limits the tables (default: every language that has the id) |
| `gui text-import` | `misc json out`: apply an edited `texts --json` file |
| `gui hud-export` | `assets outdir`: the 4 HUD fonts (metrics JSON + glyph sheet PNG) from the assets folder |
| `gui hud-import` | `assets dir outdir`: rebuild `MiscData.ZAL` / `Material.ZAL` (only the changed ones) from an edited hud-export |

```bash
dmbmx gui list --screen 0xA005 "$G/assets/Intwrap.zal"
dmbmx gui export "$G/assets/Intwrap.zal" out/gui
dmbmx gui import "$G/assets/Intwrap.zal" out/gui out/Intwrap.zal
dmbmx gui texts --table 0x7919 "$G/assets/miscdata.zal"
dmbmx gui text-set "$G/assets/miscdata.zal" 0x24 "MOD QUEST" out/miscdata.zal
dmbmx gui hud-export "$G/assets" out/hud && dmbmx gui hud-import "$G/assets" out/hud out/hud_zal
```

## Exe and load-time checks: `patch`, `integrity`

Docs: [integrity-checks.md](../../docs/re/integrity-checks.md); patch list: `tools/dmbmx/patch/patches.json`.

| Subcommand | Arguments and options |
|---|---|
| `patch list` | the patches and groups |
| `patch apply` | `input output`; `--id X` (repeatable) or `--group G`: the original SHA-256 is checked first, a new file is written |
| `patch nocrc` | `input output`: the `nocrc` group (the save size-field check; the exe has no CRC check) |
| `integrity check` | `paths ...` (files or folders): would the game accept them (SAVE.DAT, `.ZSD`, embedded PNG CRCs)? |
| `integrity fix-png` | `input`; `-o`/`--output OUT` or `--in-place`: recompute the chunk CRCs of every embedded PNG (length unchanged) |
| `integrity resign-zsd` | `input`; `-o`/`--output OUT` or `--in-place`: set the SCFI component sizes of a `.ZSD` to the files next to it |

```bash
dmbmx patch list
dmbmx patch apply "$G/NgBMX.exe" out/NgBMX.exe --id assert-continue
dmbmx patch nocrc "$G/NgBMX.exe" out/NgBMX_nocrc.exe
dmbmx integrity check out/WoodwardDirt out/SAVE.DAT
dmbmx integrity fix-png out/DMbike01.zmd -o out/DMbike01_fixed.zmd
dmbmx integrity resign-zsd --in-place out/WoodwardDirt/WoodDirt.ZSD
```

## Anything: `info`

`info file` prints the magic / first chunk tag of any game file.

```bash
dmbmx info "$G/assets/DMbike01.zmd"
```

## Library use

Every format module has `read(bytes)` and `write(obj) -> bytes` (or classmethods with the same meaning), and
`write(read(data)) == data` for every shipped file. Malformed input raises a `dmbmx.core.errors.FormatError`
subclass (`docs/testing.md` section 3).

```python
from pathlib import Path
from dmbmx.formats import zal, zmd, level, save, save_fields, motion, audio, replay, strs
from dmbmx.convert import gltf, gltf_import

G = Path("C:/Games/Dave Mirra Freestyle BMX")

arc = zal.read((G / "assets/Intwrap.zal").read_bytes())          # ZalArchive
model = zmd.read((G / "assets/DMbike01.zmd").read_bytes())       # Zmd: lods, materials, mdef, texture(name)
lvl = level.Level.read_dir(G / "assets/WoodwardDirt")             # ZSD + decoded components

s = save.read((G / "save/SAVE.DAT").read_bytes())                 # Save: options, profiles, scores
save_fields.unlock_all(s)
save_fields.set(s, "volume_0d", 9)
Path("out/SAVE.DAT").write_bytes(save.write(s))

bank = motion.read_bank((G / "assets/motion.zal").read_bytes())   # MotionBank of Motion clips
snd = audio.read((G / "bin/audio/sfxcom.zsd").read_bytes())      # Bank
demos = replay.records(zal.read((G / "assets/replay.zal").read_bytes()))   # {id: Replay}

glb = gltf.export_glb(model)                                      # bytes of a .glb
result = gltf_import.import_glb(glb)                              # ZMD bytes + an ImportReport (see its docstring)
```

Main modules:

| Module | Entry points | Doc |
|---|---|---|
| `dmbmx.formats.zal`, `zal_dir` | `read`, `write`, `ZalArchive.find`; `extract_dir`, `pack_dir`, `replace` | [ZAL.md](../../docs/formats/ZAL.md) |
| `dmbmx.formats.zmd`, `mati`, `texture` | `zmd.read` / `write`, `Mati`, `MatSd`, `texture.Image` | [ZMD.md](../../docs/formats/ZMD.md), [TEXTURE.md](../../docs/formats/TEXTURE.md) |
| `dmbmx.formats.level`, `rwworld`, `rwcoll` | `Level.read_dir`, the component classes, `rwcoll.build` | [LEVEL.md](../../docs/formats/LEVEL.md) |
| `dmbmx.formats.motion`, `tricks` | `read_bank`, `Motion.sample`, `channel_rotation`, `pose_matrices` | [ANIM.md](../../docs/formats/ANIM.md) |
| `dmbmx.formats.replay` | `read`, `write`, `records`, `Replay` | [REPLAY.md](../../docs/formats/REPLAY.md) |
| `dmbmx.formats.audio` | `read`, `write`, `Bank`, `ztrk_decode` / `ztrk_encode` | [AUDIO.md](../../docs/formats/AUDIO.md) |
| `dmbmx.formats.save`, `save_fields`, `progress` | `read`, `write`; `get`, `set`, `unlock_all`; `progress.complete_all` | [SAVE.md](../../docs/formats/SAVE.md) |
| `dmbmx.formats.scrn`, `strs`, `zprim`, `hudfont`, `gui_dir` | `read_intwrap`, `strs.read` / `write`, `export_gui` | [ZAL.md](../../docs/formats/ZAL.md) §3, [menu-modding.md](../../docs/tools/menu-modding.md) |
| `dmbmx.convert.gltf`, `gltf_import`, `obj`, `anim_gltf`, `combo`, `level_export` | `export_glb`, `import_glb`, `export_obj`, `export_combo`, `level_export.export_glb` | [export.md](../../docs/tools/export.md), [import.md](../../docs/tools/import.md) |
| `dmbmx.patch.patcher`, `dmbmx.integrity` | `apply_file`, `select`; `crc.check_save`, `crc.resign_save` | [integrity-checks.md](../../docs/re/integrity-checks.md) |
| `dmbmx.core` | `chunks`, `lzo`, `lzrw3a`, `hash` (zhash), `errors` | [ZAL.md](../../docs/formats/ZAL.md), [testing.md](../../docs/testing.md) |

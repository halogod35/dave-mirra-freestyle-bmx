# dave-mirra-pro-bmx

Reverse engineering of **Dave Mirra Freestyle BMX** for PC (Z-Axis / Acclaim, 2000; the
2000-11-22 build of `NgBMX.exe`): file format specs, lossless file tools, Blender and 3ds Max
importers, and a draft C decompilation of the game and engine code.

No game files are included. You need your own copy of the game.

## Quick start

Git Bash on Windows; Python 3.15 was used.

```bash
python -m venv .venv
.venv/Scripts/python.exe -m pip install -e ".[test]"          # add ,zig for the C syntax check
export DMBMX_GAME_DIR="C:/Games/Dave Mirra Freestyle BMX"     # your install (read only)

.venv/Scripts/dmbmx --help
.venv/Scripts/dmbmx export "$DMBMX_GAME_DIR/assets/DMbike01.zmd" out/bike.glb --obj
.venv/Scripts/dmbmx save info "$DMBMX_GAME_DIR/save/SAVE.DAT"
```

The tools never write into the game folder; write to `out/` and test changes in a copy of the game
(`docs/tooling.md`, Game trees). `tools/testing/py.sh` runs any script with the repo's venv.

## File formats

| Type | What | Spec |
|---|---|---|
| ZMD (`MODL`/`MLOD`/`MLDF`/`MDFF`/`MDEF`/`MATI`) | rider, bike and effect models: 4CC chunk tree around RenderWare geometry | [ZMD.md](docs/formats/ZMD.md) |
| ZAL | asset libraries: grouped assets, LZO1X compression, name-hash ids | [ZAL.md](docs/formats/ZAL.md) |
| ZSD level (`SCEN`/`SCDF`/`SCFI`) | level index file | [LEVEL.md](docs/formats/LEVEL.md) §3 |
| `BSP._sd` | level world (RenderWare World) and its collision trees | [LEVEL.md](docs/formats/LEVEL.md) §4, [physics.md](docs/re/physics.md) §3 |
| `Tag._sd`, `Prop._sd`, `PropDef._sd`, `Scene._sd` | tagged collision faces, placed props, prop models, scene points | [LEVEL.md](docs/formats/LEVEL.md) §5-6 |
| Textures (`MATI`/`MADF`/`TBMP`/`TPNG`) | embedded BMP/PNG textures and materials | [TEXTURE.md](docs/formats/TEXTURE.md) |
| Audio (`ZSND`/`ZTRK`) | sound banks `bin/audio/*.zsd` and their sequencer tracks | [AUDIO.md](docs/formats/AUDIO.md) |
| Motion bank | `motion.zal` animation clips and skeleton binding | [ANIM.md](docs/formats/ANIM.md) |
| Replay | replay buffers and the `replay.zal` demo records | [REPLAY.md](docs/formats/REPLAY.md) |
| SCRN / GUI art and fonts | menu screens, pictures, sprites, fonts, HUD fonts (`Intwrap.zal`, `MiscData.ZAL`) | [ZAL.md](docs/formats/ZAL.md) §3.1-3.3 |
| `STRS` | text tables | [ZAL.md](docs/formats/ZAL.md) §3.1 |
| `SAVE.DAT` | save file: options, career, records, profiles | [SAVE.md](docs/formats/SAVE.md) |

All of the above in one document: [tools/3dsmax/SPEC.md](tools/3dsmax/SPEC.md).

## Tools

| Tool | What | Docs |
|---|---|---|
| [`dmbmx`](tools/dmbmx/) CLI | export/import models (glTF, OBJ), save editor, exe patcher, menu modding (`gui`), audio, levels, replays, textures, ZAL | [export](docs/tools/export.md), [import](docs/tools/import.md), [menu modding](docs/tools/menu-modding.md), `dmbmx <command> --help` |
| [Blender add-on](tools/blender/) | import and export ZMD models in Blender 5.3 | [blender-addon.md](docs/tools/blender-addon.md) |
| [3ds Max importers](tools/3dsmax/) | MAXScript (3ds Max 2017+) and Python (2022+) importers, plus a one-file reference parser | [README](tools/3dsmax/README.md) |
| [Ghidra pipeline](tools/ghidra/) | Ghidra 12.2 wrapper, Java scripts, symbol import/export | [tooling.md](docs/tooling.md), [module-map.md](docs/re/module-map.md) |
| [Testing scripts](tools/testing/) | `py.sh`, install verification, test-copy restore, play-test generators | [tooling.md](docs/tooling.md), [testing.md](docs/testing.md) |

Also: [`tools/decomp/`](tools/decomp/) (decomp stub generator) and [`tools/research/`](tools/research/) (survey scripts).

## Decompilation

| Path | What |
|---|---|
| `decomp/src/` | C files laid out like the original source tree (`zaxis/Bmx/Src/...`, `NgEngine/Zge/...`, taken from paths embedded in the exe) |
| `decomp/include/` | recovered types and prototypes |
| `decomp/functions.csv` | every game/engine function: address, size, name, file, status, signature |
| `decomp/symbols/` | `symbols.csv` (names and types, applied to Ghidra) and `functions.csv` (Ghidra's function export) |

Status: all 2,458 game and engine functions are **draft** C (none reviewed). The C is
non-matching: written to be readable and functionally equivalent, with a `// VA 0x...` comment per
function. Library code (MSVC C runtime, RenderWare 3.1, libpng 1.0.5, zlib 1.1.3) is identified,
not decompiled.

Compiling: there is no build system and no compiler or linker setup. Nothing has been compiled into
a binary or byte-matched. What is checked is syntax: `tests/test_decomp_syntax.py` runs
`zig clang -fsyntax-only` for 32-bit Windows on every file (install the `zig` extra; see
[tooling.md](docs/tooling.md), C syntax check). The exe was built with MSVC (statically linked MSVC
runtime, [libraries.md](docs/re/libraries.md)); the headers assume the MSVC 6 data model. A matching
build would need the original compiler version and flags, a build that links the C with RenderWare
and the libraries, and a per-function comparison against the exe.

More: [PROGRESS.md](decomp/PROGRESS.md), [docs/re/](docs/re/), the
[module map](docs/re/module-map.md), and [integrity-checks.md](docs/re/integrity-checks.md) (the PC
build computes no CRC or checksum over game files, apart from the standard PNG chunk CRCs that
libpng checks).

## Tests

```bash
tools/testing/py.sh -m pytest tests
```

Corpus tests read every file of the game through `DMBMX_GAME_DIR` and skip without it; the full run
takes about 12 minutes. See [testing.md](docs/testing.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

The [Unlicense](LICENSE) covers this project's own work: the tools, the docs, and the reconstructed
C and annotations. No game files are included; you need your own copy of the game. This project is
not affiliated with or endorsed by Acclaim or Z-Axis. All trademarks belong to their owners.

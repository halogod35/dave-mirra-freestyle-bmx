# Contributing

## Setup

See the README's Quick start, then `docs/tooling.md` for every tool and `docs/testing.md` for the
test contract. Commands are for Git Bash on Windows, run from the repo root.

- Python: a `.venv` in the repo root (gitignored). `tools/testing/py.sh` runs it with `tools/` on
  `PYTHONPATH`. Record new dependencies in `requirements.txt` and `pyproject.toml`.
- Game install: point `DMBMX_GAME_DIR` at your copy of the game (2000-11-22 build of `NgBMX.exe`).
  Optional work folders inside it: `_pristine/` (stock copy, never modified), `_testgame/` (the copy
  in-game tests may modify) and the manifest `PRISTINE.sha256` (`docs/tooling.md`, Game trees).
- Scratch output goes to `out/` (gitignored).

## Rules

The docs and tests refer to these rules by number (`AD-n`).

| Id | Rule |
|---|---|
| AD-1 | The game install and `_pristine` are read-only. Tools refuse to write into the install; in-game tests use `_testgame` and restore it afterwards (`tools/testing/restore_testgame.sh`). |
| AD-2 | `dmbmx` (`tools/dmbmx/`) is the one shared implementation; the CLI, the Blender add-on and the play-test scripts are thin layers over it. Standard library only (numpy optional), so the Blender add-on can vendor it. |
| AD-3 | Round-trip rule: every reader has a writer, and `write(read(f)) == f` byte for byte for every file of that type in the install (a pytest over the whole corpus). Unknown fields are kept verbatim. |
| AD-4 | Formats are specified in `docs/formats/<FMT>.md`; keep the spec in step with the code. |
| AD-5 | Evidence tags: every claim in the docs is tagged **[V]** (verified by running code over the files or the exe) or **[I]** (inferred). Exe references are virtual addresses (image base 0x400000). |
| AD-6 | Ghidra is the RE database, but `decomp/symbols/symbols.csv` is the mergeable source of truth for names. Only the owner of the Ghidra project runs `tools/ghidra/apply_symbols.sh`; everyone else adds rows to `symbols.csv`. |
| AD-7 | File checks: prefer re-signing files (recompute what the game checks) over patching the exe. `dmbmx patch` writes a new, patched copy of `NgBMX.exe` and never edits the original. |
| AD-8 | The decomp is non-matching, readable C mirroring the original source paths under `decomp/src/`; every function carries a `// VA 0x...` comment. Library code is identified, not decompiled. |
| AD-9 | No game data in git: no game files, no files extracted or converted from them, no long hex dumps. Tests read the install through `DMBMX_GAME_DIR`; synthetic test inputs are built in code. |
| AD-10 | Interchange formats: glTF 2.0 (`.glb`) for models, OBJ/MTL as a simple alternative, PNG for textures. |

Parsers treat game bytes as untrusted data: every length, offset and count is bounds-checked, and
malformed input raises a `FormatError` (`docs/testing.md` section 3), never hangs or reads out of range.

## Conventions in the docs and tests

- `DM-0NN` names the work item that found or built something (for example "DM-048"). Play-test ids
  (`DM-0NN-PT1`) refer to in-game checks a person ran on `_testgame`.
- `tools/testing/playtest/make_playtest_dm0NN.py` rebuild those play-test packages under
  `out/playtest/` from your install; `docs/testing.md` section 5 describes the package layout.

## Ground facts (2000-11-22 build)

- `NgBMX.exe`: PE32, image base 0x400000, MSVC, RenderWare 3.x (`_rwcseg`), zlib, libpng 1.0.5. The
  PDB path `C:\zaxis\BMX\Build\Pc\Release\NgBMX.pdb` is embedded (the PDB is not shipped).
- About 90 original source paths are embedded as strings (`C:\zaxis\Bmx\Src\...`,
  `C:\NgEngine\Zge\...`); `decomp/src/` uses them to name modules.
- ZMD: 4CC chunk tree `MODL{MLOD{MLDF, MDFF{...}}}` (u32 tag, u32 size, little-endian); MDFF wraps
  RenderWare chunks (u32 id, u32 size, u32 version).
- ZAL and SAVE.DAT both start with the magic `1C 51 81 2A`; SAVE.DAT dword[1] is the file size.

## Before you send a change

```bash
tools/testing/py.sh -m pytest tests          # about 12 minutes with the game install; corpus tests skip without it
tools/testing/check_pristine.sh              # the install and _pristine still match the manifest
```

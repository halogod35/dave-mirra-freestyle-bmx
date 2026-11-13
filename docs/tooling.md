# Tooling

How to run everything in this repository. All commands are for **Git Bash** on Windows and are run
from the repo root (a checkout or a git worktree; the scripts resolve paths themselves). Paths
contain spaces, so always quote them.

| Thing | Where | Version |
|---|---|---|
| Python | `.venv` in the repo root (gitignored) | 3.15 was used; deps in `requirements.txt` / `pyproject.toml` |
| Java | on `PATH` | Ghidra needs JDK 21+ (25.0.3 was used) |
| Ghidra | `<repo>/.ghidra/ghidra_12.2_PUBLIC/` (gitignored, shared by all worktrees), or `DMBMX_GHIDRA_HOME` | **12.2** (`ghidra_12.2_PUBLIC_20261016.zip`, released 2026-10-22) |
| Game install (read-only) | `DMBMX_GAME_DIR` (default `F:\Projects\Dave Mirra freestyle BMX\`) | 2000-11-22 build |

Every script takes `--help`.

## Repository layout

| Path | What |
|---|---|
| `tools/dmbmx/` | the Python library and CLI (`python -m dmbmx`) |
| `tools/blender/` | Blender add-on `io_dmbmx/`, its build script, and headless Blender check scripts in `scripts/` |
| `tools/3dsmax/` | stand-alone file spec, reference parser and the 3ds Max importers (MAXScript and Python) |
| `tools/ghidra/` | Ghidra wrapper, Ghidra Java scripts and the symbol pipeline |
| `tools/decomp/` | `make_skeleton.py`, the decomp stub/progress generator |
| `tools/testing/` | `py.sh`, the game-tree scripts, the play-test generators (`playtest/`) |
| `tools/research/` | survey and analysis scripts used while reverse-engineering the formats |
| `decomp/` | the decompiled C (`src/`, `include/`), `functions.csv`, `PROGRESS.md`, and the RE symbol data in `symbols/` |
| `docs/` | format specs, RE notes, tool guides |
| `tests/` | the pytest suite |

## Python: `tools/testing/py.sh`

```bash
tools/testing/py.sh -m dmbmx --help
tools/testing/py.sh -m dmbmx info "F:/Projects/Dave Mirra freestyle BMX/assets/bike.zmd"   # prints ...: MODL ...
tools/testing/py.sh -m pytest tests                                                         # whole suite
tools/testing/py.sh -m pytest tests -k zmd -q
```

`py.sh` runs the repo's `.venv` Python with `tools/` on `PYTHONPATH` (that is where the `dmbmx`
package lives). From a git worktree it uses the worktree's code and the main checkout's `.venv`.
Without the wrapper, `pip install -e .` makes `dmbmx` importable from the venv (see `pyproject.toml`).
Add dependencies with `.venv/Scripts/python.exe -m pip install X` and record them in `requirements.txt`
and `pyproject.toml`.

### Tests
- Corpus tests read the install via `DMBMX_GAME_DIR` (default `F:\Projects\Dave Mirra freestyle BMX`);
  they skip if it is missing. Helpers in `tests/conftest.py`: `corpus_params('.zmd')`,
  `corpus_files('.zal')`, fixture `game_dir`. Work folders inside the install (`_pristine`, `_testgame`,
  `wt`, and repo checkouts named `dave-mirra-bmx` or `dave-mirra-freestyle-bmx`) are excluded from the corpus.
- `tests/conftest.py` puts `tools/` on `sys.path` and `PYTHONPATH`, so plain `python -m pytest tests`
  from the venv works too.
- `tests/test_tools.py` smoke-tests the shell tools (read-only).

### CLI
`python -m dmbmx <command>`; a command is a module `tools/dmbmx/cli/<name>.py` with `register(sub)`.

## Ghidra: `tools/ghidra/ghidra.sh`

```bash
tools/ghidra/ghidra.sh install     # download 12.2 from GitHub, verify SHA-256, unpack (no-op if present)
tools/ghidra/ghidra.sh version     # 12.2
tools/ghidra/ghidra.sh home        # <repo>\.ghidra\ghidra_12.2_PUBLIC (or $DMBMX_GHIDRA_HOME)
tools/ghidra/ghidra.sh headless    # no args: prints analyzeHeadless usage
tools/ghidra/ghidra.sh headless "F:\path\to\projDir" NgBMX -import "F:\path\to\NgBMX.exe" \
    -scriptPath "F:\path\to\scripts" -postScript MyScript.java
tools/ghidra/ghidra.sh gui         # GUI (avoid while a headless run holds the project lock)
```

- Verified [V] 2026-11-12: `headless` with no args prints the usage (Ghidra exits 1 for that);
  importing `NgBMX.exe` with `-noanalysis -readOnly` into a scratch project under a path with
  spaces and running a Java `-postScript` printed `lang=x86:LE:32:default base=00400000 java=25.0.3`
  (about 25 s). Java `GhidraScript`s work; PyGhidra was not set up or tested.
- `headless` passes its arguments to `support/analyzeHeadless.bat` through `cmd.exe` (the command line is handed over in the env var `DMBMX_CMDLINE`, so quoting
  survives). Pass
  **Windows-style paths** (`F:\...` or `F:/...`) to Ghidra, not `/f/...` MSYS paths.
- The default install is `<main checkout>/.ghidra/` (found via the git common dir, so worktrees use
  the main checkout's install). Set `DMBMX_GHIDRA_HOME` to use a Ghidra 12.2 installed anywhere else.
- Provenance: official release `Ghidra_12.2_build` of
  https://github.com/NationalSecurityAgency/ghidra, asset `ghidra_12.2_PUBLIC_20261016.zip`,
  SHA-256 `2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67`
  (matches the SHA-256 in the release notes and the GitHub asset digest). The pinned hash is in `tools/ghidra/ghidra.sh`.
- The Ghidra project for the exe goes in `<main checkout>/.ghidra/project/` (gitignored; override with
  `DMBMX_GHIDRA_PROJECT`). Ghidra project files can't be merged, so one person owns it and everyone else
  edits `decomp/symbols/symbols.csv` (see `docs/re/module-map.md`, section Rebuilding).
- Ghidra keeps per-user settings in `%APPDATA%\ghidra\ghidra_12.2_PUBLIC\`.

### Holes: `tools/ghidra/scan_holes.py`

```bash
tools/testing/py.sh tools/ghidra/scan_holes.py                    # every game/engine byte outside a function
tools/testing/py.sh tools/ghidra/scan_holes.py --functions FILE   # against another functions.csv export
```

It reads `NgBMX.exe` (from `DMBMX_GAME_DIR`) and `decomp/symbols/functions.csv`. Between two functions it
accepts padding, jump tables, index tables and import stubs, and reports everything else as a
hole: its size, whether anything references it, and its first instruction. `tests/test_decomp_cleanup.py`
lists the holes that are allowed, each with its reason. After new functions are created, re-export
`decomp/symbols/functions.csv` and run it again.

## C syntax check (zig)

The build PC has no MSVC. The `ziglang` PyPI package ships a complete clang and the MinGW headers,
which is enough to parse and type-check every decomp C file for 32-bit Windows.
`tests/test_decomp_syntax.py` runs `zig clang -fsyntax-only` on each file under `decomp/src`,
with `decomp/include` on the include path. Warnings are allowed, errors fail. The test **skips**
when it finds no zig.

Install it one of these two ways (0.17.1 unpacks to about 400 MB):

```bash
# a) into the repo's venv (the test then finds it by itself)
.venv/Scripts/python.exe -m pip install ziglang==0.17.1      # or: pip install -e ".[zig]"
# b) anywhere else, then point the test at the binary
python -m pip install --target "C:/tools/zig" ziglang==0.17.1
export DMBMX_ZIG="C:/tools/zig/ziglang/zig.exe"
```

The test looks for zig in this order: `DMBMX_ZIG`, then `zig` on `PATH`, then the `ziglang` package
importable from the test's Python. Verified [V] 2026-11-13 with ziglang 0.17.1: all decomp files
pass, and a run takes about 2 s. Not in `requirements.txt` because it is optional (it is the `zig`
extra in `pyproject.toml`).

To check one file by hand (the flags the test uses; `zig cc -fsyntax-only` reports a bogus
"failed to rename ... .obj" cache error on Windows, so the test calls `zig clang` directly):

```bash
Z="$DMBMX_ZIG"; L="$(dirname "$Z")/lib"; I="$L/libc/include"
"$Z" clang -fsyntax-only -target i386-pc-windows-gnu -nostdinc -ferror-limit=0 \
  -isystem "$L/include" -isystem "$I/x86-windows-gnu" -isystem "$I/generic-mingw" \
  -isystem "$I/x86-windows-any" -isystem "$I/any-windows-any" \
  -D__MSVCRT_VERSION__=0xE00 -D_WIN32_WINNT=0x0a00 -D__CRT__NO_INLINE \
  -I decomp/include decomp/src/NgEngine/Zge/zobj.c
```

This clang treats incompatible pointer and function-pointer types as errors. Write casts where
the exe passes a float array as an `RwV3d *` (the clean-up fixed game.c and physics.c that way).

## Game trees: `tools/testing/check_pristine.sh`, `tools/testing/restore_testgame.sh`

The manifest `<game>/PRISTINE.sha256` (one line per file, 255 for the install this was built on; paths relative to the tree root, `./NgBMX.exe`)
describes the stock install. Neither the manifest nor any game file is in this repository. To set
up the folders once, from a freshly installed, unmodified game (`GAME` = your install):

```bash
GAME="F:/Projects/Dave Mirra freestyle BMX"
cp -a "$GAME" "$GAME.tmp" && mv "$GAME.tmp" "$GAME/_pristine"     # stock copy (read-only from now on)
(cd "$GAME/_pristine" && find . -type f -print0 | xargs -0 sha256sum -b) > "$GAME/PRISTINE.sha256"
cp -a "$GAME/_pristine" "$GAME/_testgame"                           # the copy play-tests may modify
```

Run the first line while the install folder holds only the game's own files (before you put a
checkout or anything else inside it).

```bash
tools/testing/check_pristine.sh                 # verify _pristine and the install (default)
tools/testing/check_pristine.sh all             # ... and _testgame
tools/testing/check_pristine.sh -q testgame     # summary lines only
tools/testing/restore_testgame.sh               # make _testgame stock again, then verify it
tools/testing/restore_testgame.sh --check       # verify _testgame only
```

- `check_pristine.sh` is read-only. It reports `MISSING`, `DIFFERS` and `EXTRA` files
  (for the install, the work folders `_pristine`, `_testgame`, `wt`, the repo checkouts
  `dave-mirra-bmx` / `dave-mirra-freestyle-bmx` and the manifest itself are ignored). Exit 0 = clean, 1 = differences, 2 = setup error.
- `restore_testgame.sh` first verifies `_pristine` (refuses to copy from a damaged one), then
  deletes files in `_testgame` that are not in the manifest, re-copies only files that are missing
  or differ, and verifies the result. Idempotent; it never writes to the install or `_pristine`.
- Use it after every in-game test (the install itself stays read-only; see CONTRIBUTING.md).

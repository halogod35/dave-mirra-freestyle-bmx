# Testing strategy

This is the test contract for every change. Reviews check new tests against it (section 6).

Run everything (from the repo root or a worktree):

```
tools/testing/py.sh -m pytest tests
# without the wrapper (tests/conftest.py puts tools/ on the path itself):
.venv/Scripts/python.exe -m pytest tests
```

The suite must pass, with corpus tests **skipped, not failed**, on a machine that has no game install.

## 1. Test layers

| Layer | What | Where | Needs game data |
|---|---|---|---|
| Unit | Hand-built byte strings, helpers, error paths | `tests/test_<fmt>_unit.py` | no |
| Corpus round-trip (AD-3) | `write(read(f)) == f` byte-for-byte for **every** file of the type | `tests/test_<fmt>_corpus.py` | yes (skips) |
| Fuzz | Truncated / bit-flipped / u32-overwritten copies of real files must be rejected cleanly | `tests/test_<fmt>_fuzz.py` via `tests/fuzz.py` | yes (skips); unit fuzz on synthetic samples is encouraged too |
| CLI smoke | `python -m dmbmx <cmd> --help` and one real run per command, output into `tmp_path` | `tests/test_cli_*.py` | some |
| Export check | glTF/OBJ/PNG output is valid: structural checks in pytest; Blender headless import (`blender -b --python ...`) as a marked slow test | `tests/test_export_*.py` | yes |
| Exe patch | `dmbmx patch` verifies the original SHA-256 first, writes a *new* file, and the patched bytes match the documented patch list | `tests/test_integrity.py` | yes |
| Manual in-game check | A person runs the game in `_testgame\` (section 5) | `out/playtest/<name>/` | yes |

Slow tests (Blender, whole-corpus fuzz) get `@pytest.mark.slow`; the default run includes them unless the suite gets too slow (> ~2 min), at which point they may be deselected by default.

## 2. Corpus rules

- The corpus is the user's install: env `DMBMX_GAME_DIR`, default `F:\Projects\Dave Mirra freestyle BMX` (AD-9). Fixtures live in `tests/conftest.py`:
  - `game_dir` fixture: the install path, or **skip** when absent.
  - `corpus_files(ext)`: sorted list of every file with that extension, excluding `_pristine`, `_testgame`, `wt`, `dave-mirra-bmx`, `dave-mirra-freestyle-bmx`.
  - `corpus_params(ext)`: for `@pytest.mark.parametrize("path", corpus_params(".zmd"))`; one test id per file (relative path), and a single skipped param when there is no corpus.
- **Read-only** (AD-1). Tests never open install files for writing. All output goes to pytest's `tmp_path` or `out/` (gitignored). Never write into `_pristine`; tests don't touch `_testgame` either (only the manual-test install scripts do).
- **No game data in git** (AD-9): no committed sample files, no hex dumps longer than a header, no extracted textures. Synthetic test inputs are built in code (see `tests/test_fuzz_helper.py` for a chunk-tree example).
- **Whole corpus, not a sample.** A round-trip test parametrises over *every* file of its type. Pin the expected count too (as `test_smoke.py` does: 117 `.zmd`, 6 `.zal`), so a silently shrinking glob is caught.
- Case-insensitive extensions (`.ZSD` vs `.zsd`) are handled by `corpus_files`; `_sd` files use `corpus_files("._sd")`.

Round-trip template:

```python
import pytest
from conftest import corpus_params
from dmbmx.formats import zmd

@pytest.mark.parametrize("path", corpus_params(".zmd"))
def test_zmd_roundtrip(path):
    data = path.read_bytes()
    assert zmd.write(zmd.read(data)) == data
```

On a mismatch, report the first differing offset (not a 100 KB diff). Unknown fields are kept as raw bytes, never dropped (AD-3).

## 3. Error contract for parsers

Defined in `tools/dmbmx/core/errors.py`:

| Class | Use |
|---|---|
| `FormatError(ValueError)` | Base: bytes are not a valid file of this format. `FormatError(msg, fmt="ZMD", offset=0x40)` renders as `ZMD: msg (at 0x40)`. |
| `TruncatedError` | A read or a declared size/offset runs past the end of the data or the parent chunk. |
| `BadMagicError` | Wrong magic / 4CC / chunk id where the grammar requires a specific one. |
| `ChecksumError` | Stored CRC/size field disagrees with the data. Default is to raise; a reader may offer an explicit lenient flag. |
| `LimitError` | Count, size or nesting depth beyond a sane limit. |
| `need(data, off, size, fmt=, what=)` | Bounds-check helper; raises `TruncatedError`, returns `off + size`. |

Rules (CONTRIBUTING.md):
1. Malformed input raises a `FormatError` subclass with a non-empty message (prefer `fmt` and `offset`). Never `IndexError`, `struct.error`, `KeyError`, `UnicodeDecodeError`, `RecursionError`, `MemoryError`, `ZeroDivisionError`, `AssertionError`. Catch and re-raise at the boundary if you must (`raise TruncatedError(...) from e`).
2. **Every length, offset and count is checked** against the remaining bytes of its parent before use. A child chunk must end inside its parent.
3. **No allocation from an unchecked size.** `bytearray(size)`, `[x] * count`, `b"\0" * pad` only after the size is bounded by the data actually present (or a documented `LimitError` cap).
4. **Bounded recursion and loops.** Chunk trees carry a depth limit (16 is plenty for ZMD/ZSD/ZAL); a loop over chunks must advance `off` by at least the header size each iteration (a zero-size chunk must not loop forever); pointer/offset tables must detect cycles.
5. Format-specific errors may subclass these (e.g. `class ZalError(FormatError)`), so the fuzz contract still holds.
6. Writers raise `ValueError`/`FormatError` for unrepresentable models (overflowing a u16 field, etc.), never write a corrupt file silently.

## 4. Fuzz rules

`tests/fuzz.py` provides `fuzz_parser(read_fn, data, n=200, seed=0, *, timeout=1.0, allowed=(FormatError,))`:

- Generates `n` deterministic mutations of a **real** file: always first empty input, truncation to `len-1`, truncation to `len/2`, a bit flip in byte 0; then a random mix of truncations, 1-8 bit flips, and aligned u32 overwrites with `0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF8, len, len+1, random` (these hit size/offset/count fields).
- Each case runs in a worker thread with a **per-case time budget** (`timeout`, seconds). Pure-Python loops are interrupted by a tracer; anything stuck in C is caught by the join timeout. Either way it is reported as a hang.
- A case is **clean** if `read_fn` returns (a mutation may still be valid) or raises one of `allowed` with a non-empty message. Everything else fails. All failures are collected and raised as one `AssertionError` listing case index, mutation and exception; reproduce with `dict((i, b) for i, _, b in mutations(data, n, seed))[case]`.
- Returns a `FuzzReport` (`cases`, `returned`, `rejected`, `slowest`) on success.

Usage:

```python
import pytest
from fuzz import fuzz_parser
from conftest import corpus_files
from dmbmx.formats import zal

def test_zal_fuzz():
    files = corpus_files(".zal")
    if not files:
        pytest.skip("no corpus")
    for f in (min(files, key=lambda p: p.stat().st_size), files[0]):
        fuzz_parser(zal.read, f.read_bytes(), n=200, seed=0)
```

Minimum per format: fuzz the **smallest** corpus file and one representative file, `n >= 200`, fixed seed. If one parse of the full file takes longer than ~0.1 s, fuzz a sub-structure parser (e.g. one ZAL entry, one MDFF stream) or raise `timeout` and say so in the test. Widening `allowed` beyond `FormatError` needs a written reason in the test and in the change description. The helper's own self-test is `tests/test_fuzz_helper.py` (toy chunk-tree parser plus deliberately buggy parsers: unchecked reads, plain `ValueError`, empty message, recursion, `MemoryError`, Python-loop hang, C-level block, `except Exception` swallowing).

## 5. Manual in-game checks

The tests never launch the game (fullscreen DX7 takes over the desktop). Claims about in-game
behaviour are checked by hand: a generator script prepares a package, the person testing runs one
install script and follows numbered steps, then restores the test copy.

### 5.1 `out/playtest/<name>/` convention

```
out/playtest/<name>/
  install.sh        # the only thing you run (Git Bash)
  files/            # the modified files, laid out relative to the install root
    save/SAVE.DAT
    assets/.../foo.zmd
  MANIFEST.sha256   # sha256 of every file under files/ (sha256sum format, ./relative paths)
  CHECK.md          # the steps and the expected results (template in 5.2)
```

`out/` is gitignored, so the *generator* is what gets committed: a `tools/testing/playtest/make_playtest_<name>.py`
script that rebuilds the folder from the corpus. The existing generators:

| Generator | What it checks in game |
|---|---|
| `make_playtest_save_edit.py` | an edited SAVE.DAT loads; a wrong size field is rejected |
| `make_playtest_level_edit.py` | the three Woodward levels edited through `dmbmx.level` |
| `make_playtest_texture_swap.py` | Dave Mirra's bike 1 with two swapped textures |
| `make_playtest_sound_tones.py` | sound banks with test tones |
| `make_playtest_gltf_bikes.py` | bikes edited through glTF (an edited model loads in game) |
| `make_playtest_zal_repack.py` | repacked ZALs with recoloured images |
| `make_playtest_unlock_all.py` | `dmbmx save unlock-all` on your save |
| `make_playtest_integrity.py` | patched vs stock exe, and a PNG with a bad chunk CRC |
| `make_playtest_complete_all.py` | `dmbmx save complete-all` on your save |
| `make_playtest_world_geometry.py` | new world geometry in WoodwardDirt, with and without the rebuilt collision tree |
| `make_playtest_menu_edit.py` | a main-menu label and a recoloured menu sprite |

`install.sh` must:
1. `set -euo pipefail`; resolve `GAME="F:/Projects/Dave Mirra freestyle BMX"`, `TEST="$GAME/_testgame"`, `PRIS="$GAME/_pristine"`.
2. Refuse to run unless the target is exactly `_testgame` (never the install root, never `_pristine`) (AD-1).
3. **Restore first:** verify `_testgame` against `PRISTINE.sha256` (`cd "$TEST" && sha256sum -c --quiet "$GAME/PRISTINE.sha256"`); on any mismatch, copy the differing files back from `_pristine` (or `cp -a "$PRIS/." "$TEST/"`) and re-verify. Remove files in `_testgame` that are not in the manifest only if a previous package added them (keep a list in `_testgame/.playtest_added`).
4. Verify `files/` against `MANIFEST.sha256`.
5. Copy `files/*` over `_testgame` and print every file it replaced, with old and new sha256.
6. Write `_testgame/.playtest` containing the package name, the variant and a timestamp.
7. Support `install.sh --restore`: put `_testgame` back to pristine (steps 3 only) and delete `.playtest`.
8. Never start `NgBMX.exe` itself. End by printing: `Ready. Start _testgame/NgBMX.exe and follow CHECK.md. Afterwards run: install.sh --restore`.

Scripts and tests may run `install.sh` (it touches only `_testgame`) and `install.sh --restore` to test the
script itself, and must leave `_testgame` pristine afterwards.

### 5.2 `CHECK.md` template

```markdown
### Manual in-game check: <name>
**Goal:** <one sentence: what this proves, e.g. "the game accepts a re-signed SAVE.DAT with 999 points">
**Prepare (Git Bash):**
    bash "<repo>/out/playtest/<name>/install.sh"
  Expected output ends with "Ready." It lists the replaced files: <list>.
**Steps:**
1. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`.
2. <exact menu path, e.g. "Main menu → Proquest → Load → slot 1">
3. <...>
**Expected result:**
- At step N: <what should be on screen; exact text/number where possible>
**Failure looks like:** <e.g. "CRC error" / "memory card data is invalid" message, crash to desktop, missing model, T-pose>
**Please report:** pass/fail per step, any message text verbatim, a screenshot if something looks wrong (Win+PrtScn), and whether the game crashed.
**Clean up:** `bash ".../out/playtest/<name>/install.sh" --restore` (expected: "_testgame restored, N files verified").
**Time needed:** ~<N> minutes.
```

When you report a result (in an issue or a pull request), give the check, the date, PASS/FAIL per
step, any message text verbatim and the evidence (screenshot or quote).

A claim that "the game accepts X" is **[V]** only with a recorded PASS; before that it stays [I] (CONTRIBUTING.md AD-5).

## 6. Review checklist

Before a change is merged, check it on its branch **and** after a trial merge onto current `main`.
Report findings as `severity: finding; how to reproduce; suggested fix`. Severities: **blocker**
(must fix before merge), **major** (fix in this change unless deliberately deferred), **minor** (can be a follow-up).

1. **Scope:** only the files the change is about changed; unrelated findings are follow-ups.
2. **Tests pass:** full `pytest tests` output attached; corpus tests ran (not skipped) on this machine.
3. **Round-trip (AD-3):** every reader has a writer; corpus test parametrises over *all* files of the type with a pinned count; byte-for-byte equality.
4. **Bounds / fuzz:** `fuzz_parser` run on the smallest and one representative file per format (truncated + bit-flipped covered), default `allowed`; the reviewer also re-runs it with 2 other seeds.
5. **Error contract:** parsers raise `FormatError` subclasses; no bare `assert` for input validation; no unchecked allocations.
6. **Evidence tags (AD-5):** every claim in `docs/formats/*.md` and reports is tagged [V] or [I]; exe references are VAs.
7. **No game data committed (AD-9):** `git diff main --stat` and `git ls-files` show no asset files, no large binary blobs, no big hex dumps.
8. **Read-only install (AD-1):** `PRISTINE.sha256` still verifies for the install and `_testgame` after running the change's tests and scripts.
9. **Docs current (AD-4):** the format spec matches the code (field names, sizes); open questions listed.
10. **Manual in-game check:** if the change claims in-game behaviour, there is a generator for `out/playtest/<name>/` with a `CHECK.md` in the template above, and its outcome is recorded (or the claim is marked unverified).
11. **Pure stdlib:** `dmbmx` imports only stdlib (+ optional numpy) so the Blender add-on can vendor it.

"""Build out/playtest/dm-030/ (DM-030-PT3): patched exe vs stock exe, and a PNG with a bad chunk CRC.

Variants (folders under files/, installed with install.sh <variant>):
  badsave  save/SAVE.DAT = the DM-012 edit (profile 0 "DM012", record 0 = 999999) with the size
           field 0x235D (the stock exe rejects it) + NgBMX_nocrc.exe (`dmbmx patch nocrc`, accepts it)
  badpng   assets/DMbike01.zmd with the CRC of its first PNG's IDAT chunk corrupted (critical chunk)

Usage: tools/testing/py.sh tools/testing/playtest/make_playtest_dm030.py [--game DIR] [--out DIR]   (reads _pristine; writes out/ only)
"""
import argparse
import hashlib
import importlib.util
import os
import struct
from pathlib import Path

from dmbmx.formats import save
from dmbmx.integrity import png
from dmbmx.integrity.crc import check_save
from dmbmx.patch import patcher

REPO = Path(__file__).resolve().parents[3]


def _pt012():
    spec = importlib.util.spec_from_file_location("pt012", Path(__file__).with_name("make_playtest_dm012.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def corrupt_first_idat_crc(data):
    """Flip the low CRC byte of the first IDAT chunk of the first embedded PNG. Returns (bytes, info)."""
    p = png.find(data)[0]
    for o, t, n, st, _cp in png.chunks(data, p):
        if t == "IDAT":
            out = bytearray(data)
            out[o + 8 + n + 3] ^= 0xFF
            return bytes(out), f"PNG at 0x{p:x}, IDAT at 0x{o:x}, CRC {st:08x} -> {struct.unpack_from('>I', out, o + 8 + n)[0]:08x}"
    raise SystemExit("no IDAT found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-030"))
    a = ap.parse_args()
    pris = Path(a.game) / "_pristine"
    out = Path(a.out)
    pt = _pt012()

    # --- badsave: edited save with a wrong size field + patched exe ---
    s, old_name = pt.edited((pris / "save" / "SAVE.DAT").read_bytes())
    s.size_field = save.SIZE + 1
    bad = save.write(s)
    if check_save(bad)[0]:
        raise SystemExit("internal error: bad save is accepted by the stock check")
    spec = patcher.load()
    exe = patcher.apply_bytes((pris / "NgBMX.exe").read_bytes(), patcher.select(spec, group="nocrc"), spec)
    files = {"badsave/save/SAVE.DAT": bad, "badsave/NgBMX_nocrc.exe": exe}

    # --- badpng: DMbike01.zmd with a broken critical-chunk CRC ---
    zmd_name = next(p.name for p in (pris / "assets").iterdir() if p.name.lower() == "dmbike01.zmd")
    zmd, info = corrupt_first_idat_crc((pris / "assets" / zmd_name).read_bytes())
    if not any(b[5] for b in png.check(zmd)):
        raise SystemExit("internal error: corrupted PNG not detected")
    files[f"badpng/assets/{zmd_name}"] = zmd

    lines = []
    for rel, data in files.items():
        p = out / "files" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(sorted(lines, key=lambda x: x.split('*')[1])), newline="\n")
    (out / "install.sh").write_text(pt.install_sh("DM-030"), newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.format(out=out.as_posix(), old=old_name, zmd=zmd_name, info=info),
                                    newline="\n")
    print(f"wrote {out}\n  badsave: size field 0x235D + NgBMX_nocrc.exe\n  badpng: {zmd_name}: {info}")
    print("".join(lines), end="")


REQUEST = r'''### Play-test request DM-030-PT3
**Goal:** settle the "the game CRC-checks files" question in-game. (A) the only save check (the size field) is real and the `nocrc` patched exe removes it; (B) what an edited PNG with a bad chunk CRC does (libpng's CRC check, the only CRC check in the exe).

**Part A: patched vs stock exe (about 6 min).** The save is the DM-012 edit (profile 0 "DM012", best scores 999999) with a wrong size field (0x235D).
1. `bash "{out}/install.sh" badsave`. It ends with "Ready." and lists `replaced save/SAVE.DAT` and `added NgBMX_nocrc.exe`.
2. Run **`F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX_nocrc.exe`** (the patched copy). Look at the profile name and a level's records screen, then quit.
   **Expected:** the edit loads: name **DM012** (instead of "{old}"), best score **999999**.
3. `bash "{out}/install.sh" badsave` again (the game may have rewritten the save on exit).
4. Run the stock **`_testgame\NgBMX.exe`** and look at the same screens, then quit.
   **Expected:** the save is rejected: "LOAD FAILED." or default data (no DM012, no 999999), as in PT1 part B.

**Part B: bad PNG CRC (about 4 min).** `{zmd}` (Dave Mirra's first bike) has one byte of its first texture's IDAT CRC changed ({info}).
5. `bash "{out}/install.sh" badpng`, then run the stock `_testgame\NgBMX.exe`.
6. Pick **Dave Mirra** and his first bike (HARO MIRRA 540 AIR) and start any level (or wait for the bike preview).
   **Expected [I]:** a "RenderWare(tm) Application Error" box saying `***ASSERT*** ... Why: Failed to create texture` (zrwTxr.c). Press **No** (= continue). The bike then appears with a missing/white texture; the game keeps running. (Yes = debugger break/crash, Cancel = quit, so press No.)
   If nothing at all happens and the bike looks normal, report that too (it would mean the texture comes from elsewhere).

**Failure looks like:** Part A: the patched exe also rejects the save, or the stock exe accepts it. Part B: a crash to desktop, or a different message.
**Please report:** for each step pass/fail, the exact text of any message box (a photo or screenshot of the ASSERT box), and whether the game kept running after "No".
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified"; it also deletes NgBMX_nocrc.exe from _testgame).
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

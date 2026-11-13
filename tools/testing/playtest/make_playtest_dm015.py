"""Build out/playtest/dm-015/ (DM-015-PT4): sound banks with test tones.

Variants (install.sh <variant>):
  tones  bin/audio/sfxcom.zsd: sample i (0..5) -> sine tone of the same length at 400 + 200*i Hz
  music  bin/audio/music.zsd: track t (0..9) -> 10 s of (t+1) beeps per 2 s (IMA-ADPCM, 44100 Hz)

Usage: tools/testing/py.sh tools/testing/playtest/make_playtest_dm015.py [--game DIR] [--out DIR]   (reads _pristine; writes out/ only)
"""
import argparse
import hashlib
import importlib.util
import math
import os
import struct
from pathlib import Path

from dmbmx.formats import audio

REPO = Path(__file__).resolve().parents[3]


def tone(freq, rate, frames, amp=12000, fade=200):
    out = []
    for k in range(frames):
        env = min(1.0, k / fade, (frames - 1 - k) / fade) if frames > 2 * fade else 1.0
        out.append(int(amp * env * math.sin(2 * math.pi * freq * k / rate)))
    return struct.pack(f"<{frames}h", *out)


def beeps(n, rate=44100, secs=10, freq=660):
    """n short beeps at the start of every 2 s, for ``secs`` seconds."""
    period, beep, gap = 2 * rate, int(0.12 * rate), int(0.08 * rate)
    block = bytearray(period * 2)
    for b in range(n):
        start = b * (beep + gap)
        block[start * 2:(start + beep) * 2] = tone(freq, rate, beep)
    return bytes(block) * (secs // 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("DMBMX_GAME_DIR", r"F:\Projects\Dave Mirra freestyle BMX"))
    ap.add_argument("--out", default=str(REPO / "out" / "playtest" / "dm-015"))
    a = ap.parse_args()
    src = Path(a.game) / "_pristine" / "bin" / "audio"
    out = Path(a.out)
    spec = importlib.util.spec_from_file_location("pt012", Path(__file__).with_name("make_playtest_dm012.py"))
    pt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pt)

    files = {}
    b = audio.read((src / "sfxcom.zsd").read_bytes())
    table = []
    for i in range(len(b.samples)):
        codec, rate, _ = b.sample_info(i)
        frames = b.frames(i)
        audio.replace_sample(b, i, tone(400 + 200 * i, rate, frames), rate)
        table.append(f"| {i} | {400 + 200 * i} Hz | {frames / rate:.2f} s |")
    files["tones/bin/audio/sfxcom.zsd"] = audio.write(b)

    m = audio.read((src / "music.zsd").read_bytes())
    for t in range(len(m.samples)):
        audio.replace_sample(m, t, beeps(t + 1, freq=330 if t % 2 == 0 else 660), 44100)
    files["music/bin/audio/music.zsd"] = audio.write(m)

    lines = []
    for rel, data in files.items():
        p = out / "files" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()} *./{rel}\n")
    (out / "MANIFEST.sha256").write_text("".join(lines), newline="\n")
    (out / "install.sh").write_text(pt.install_sh("DM-015"), newline="\n")
    (out / "REQUEST.md").write_text(REQUEST.format(out=out.as_posix(), table="\n".join(table)), newline="\n")
    print(f"wrote {out}")
    print("".join(lines), end="")


REQUEST = r'''### Play-test request DM-015-PT4
**Goal:** prove that rebuilt sound banks play correctly (PCM16 SFX and IMA-ADPCM music), and learn which sound is which.

**Part A: SFX tones (about 4 min).** All six sounds in `sfxcom.zsd` (the "common" bank loaded at start-up) are replaced by sine tones of the **same length**:

| sample | tone | length |
|---|---|---|
{table}

1. `bash "{out}/install.sh" tones`. It ends with "Ready." and lists `replaced bin/audio/sfxcom.zsd`.
2. Run `F:\Projects\Dave Mirra freestyle BMX\_testgame\NgBMX.exe`. Move around the menus (up/down, select, back), start a level and ride briefly (land a trick, crash once).
3. Quit.

**Expected:** wherever the game normally plays one of these common sounds (probably menu move/select/back and some generic effects) you hear a **clean beep** of that pitch, not noise, clicks or silence. Everything else sounds normal. Please note **which action gave which pitch** (low ≈ 400 Hz … high ≈ 1400 Hz): that maps the samples.

**Part B: music beeps (about 4 min).** Each of the 10 music tracks is replaced by 10 s of beeps repeating every 2 s: track 0 = 1 beep, track 1 = 2 beeps, … track 9 = 10 beeps (even tracks low 330 Hz, odd tracks high 660 Hz).
4. `bash "{out}/install.sh" music`, run the game, listen in the menus and in a level, then quit.

**Expected:** instead of music you hear groups of beeps; count the beeps per group (this tells which track plays where). After 10 s the track ends: please note whether it then loops, moves to another track, or goes silent.

**Failure looks like:** loud noise or static (wrong codec), sound at the wrong speed or pitch (wrong rate), a crash or an "***ASSERT***" box, or no sound at all where there used to be some.
**Please report:** pass/fail for A and B, the action → pitch list from part A, the beep counts from part B (menu, level), and any message box text.
**Clean up:** `bash "{out}/install.sh" --restore` (expected: "_testgame restored, 255 files verified").
**Time needed:** ~10 minutes.
'''

if __name__ == "__main__":
    main()

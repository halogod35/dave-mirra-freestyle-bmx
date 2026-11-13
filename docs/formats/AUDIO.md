# Sound banks `bin\audio\*.zsd` (ZSND)

The fields were settled from the decompiled sound code.
Code: `tools/dmbmx/formats/audio.py`, CLI `dmbmx audio list|extract|replace|ztrk-dump`.
Exe: `winLoad.c` (`zsndRegisterAsset` **0x0046FD00**), `winStrm.c` (streams), `winChan.c` (the software mixer), `zsndseq.c` (ZTRK sequencer), `bmxsound.c` (the game layer; `bmxsoundOpen` 0x0044DDA0 opens `SFXCOM.ZSD`, `MUSIC.ZSD`, `SPEECH.ZSD`). How the game uses all this: `docs/re/sound.md`.
Tags: [V] verified (disassembly/decompilation and/or code run over all 22 banks), [I] inferred.

> **Name clash:** the 22 `bin\audio\*.zsd` files are **sound banks** (magic `ZSND`). The 12 `assets\<Level>\*.ZSD` files are **level scenes** (magic `SCEN`, chunk tree `SCEN{SCDF, SCFI…}`, see `docs/re/integrity-checks.md` #9 and the level reader). Tools tell them apart by the first 4 bytes.

## 1. Summary

- [V] One container for all banks: a 0x48-byte header of (count, offset) table pairs, the tables, optional ZTRK sequence data, zero padding to 4 bytes, then the **sample files back to back** in table order. Nothing follows the last sample.
- [V] There are **no checksums**. The loader `strncmp`s `ZSND` and `WIN ` (asserts "invalid audio asset file header/platform") and relocates the table offsets.
- [V] A bank is either **resident** (sfx: read whole, the mixer plays from memory) or **streamed** (music, speech: only bytes `[0, +0x0C)` are read; the stream code opens the file again at a sample file's offset). The caller decides (`zsndLoadAsset(path, resident, &handle)`).
- [V] Encodings, which the bank itself does not record:
  - **sfx banks** (they have sample defs): **signed 16-bit mono PCM**, at the sample def's rate (8000-32000 Hz). Supporting evidence: every loop end lies inside `size/2` frames, and 32 of 72 looping defs end exactly on frame `size/2 − 1`.
  - **music** (no sample defs): **IMA-ADPCM, mono, 16-bit, 44100 Hz**, one continuous stream per track: low nibble first, predictor 0, step index 0 at the start, no block headers. The flags 0x44 pushed at 0x0044E860 reach the stream opener `zsndStreamOpenFile` 0x00472030 (0x40 → 44100 Hz; no 0x100 → mono; no 0x800 → ADPCM, which forces 1 channel and step 7). Decoder: `zsndImaDecode` 0x004724C0, tables 0x0055F1AC (index, int32) / 0x0055F1EC (step, uint16). The 10 tracks run 120-136 s each. [V] a port of the exe's chunked reader + decoder gives the same samples as `audio.ima_decode` on all 10 tracks.
  - **speech**: a stream bank with one 32-byte zero blob. [V] `bmxsoundOpen` loads its header and nothing ever streams from it.
- [V] `write(read(b)) == b` for all 22 banks.

## 2. Header (0x48 bytes)

| Off | Type | Meaning | Tag |
|---|---|---|---|
| 0x00 | char[4] | `ZSND` | V |
| 0x04 | char[4] | platform `WIN ` | V |
| 0x08 | u32 | total file size (= file length in all 22) | V |
| 0x0C | u32 | end of the header area (tables + ZTRK data). Samples start at `align4(this)` (pad = zeros) | V |
| 0x10 / 0x14 | u32 n, u32 off | **A**: voices, 20 bytes each (`zsndGetVoice`: "invalid voice number") | V |
| 0x18 / 0x1C | u32 n, u32 off | **B**: sample defs ("samples"), 24 bytes each ("sample number out of range") | V |
| 0x20 / 0x24 | u32 n, u32 off | **C**: sample files, `(u32 offset, u32 size)` ("sample file number out of range"); the entries are not relocated, sample +8 gets the pointer | V |
| 0x28 / 0x2C | u32 n, u32 off | **D**: phrases ("phrase number out of range"): u32 offsets of variable records (flag byte, count, 8-byte sub-entries at +0x10). Empty in all banks; only the loader touches them | V |
| 0x30 / 0x34 | u32 n, u32 off | **E**: track defs ("track def number out of range"), 16 bytes each (level banks only, 3-4 per bank) | V |
| 0x38 / 0x3C | u32, u32 | 0 and the end of table E; never read | V |
| 0x40 / 0x44 | u32 n, u32 off | **F**: keymaps ("keymap number out of range"): u32 offsets of records `{u8 firstNote, u8 count, u16 handle, {u8 voice, s8 semitones}[count]}`. Empty in all banks | V |

Load-time fixups (`zsndRegisterAsset` 0x0046FD00) write runtime bank handles into A+0x12, B+0x16, E+0xE and F-record+2, and pointers into B+8 (resident banks only) and E+8. In the files these runtime fields are 0 [V].

## 3. Tables

**A, voice (20 bytes)**, `ZsndVoice` / `audio.Voice`, all [V] (the mixer `winChan.c`):

| Off | Type | Meaning |
|---|---|---|
| +0x00 | u16 | sample def (B) index; 0xFFFF, or a sample def whose file is 0xFFFF, = silent (many level / game voices are such placeholders, with volume 0) |
| +0x02 | u16 | pitch, 4.12 (0x1000 = the sample's rate; e.g. 0x0D74); the step is pitch x rate / 22050 |
| +0x04 | u8 | volume 0..127 (the event's volume multiplies it, /128) |
| +0x05 | s8 | pan, -127 left .. 0 .. 127 right (events add to it, clamped) |
| +0x06 | u8 | attack: envelope time index (0 = start at full level) |
| +0x07 | u8 | decay: time index of 1.0 → sustain level (0 = none; with a decay the exe's rate has the wrong sign, but every such voice has sustain level 127 → rate 0) |
| +0x08 | u8 | sustain time: time index of sustain level → 0; **127 = hold** until key-off (485 of 490 voices) |
| +0x09 | u8 | release: time index of the fade after a key-off (0 = cut) |
| +0x0A | u8 | sustain level 0..127 (127 in every bank) |
| +0x0B | u8[6] | 0 in every bank; never read |
| +0x11 | u8 | 0/1 (29 voices); never read on the PC [I: a PSX SPU voice flag] |
| +0x12 | u16 | runtime bank handle (0 in files) |

Envelope times (`audio.ENV_TIME_MS`, table 0x0055EB7C): index → ms, 1, 2, 3, ... 28577, 30000 (index 126), 0 (127). Volumes and the envelope level go through a 128-entry gain table (0x0055E97C).

**B, sample def (24 bytes):**

| Off | Type | Meaning | Tag |
|---|---|---|---|
| +0x00 | u16 | sample file index (C), 0xFFFF = none; the loader asserts it is < nC ("invalid sample file number") | V |
| +0x02 | u16 | flags: bit 0 loop, bit 1 stereo, bit 2 unsigned 8-bit, bit 3 ping-pong (with loop). Only 0 and 1 occur | V |
| +0x04 | u32 | sample rate, Hz (8000, 11025, 16000, 22050, 32000, one 2229; 0 when unused) | V |
| +0x08 | u32 | runtime data pointer (0 in files) | V |
| +0x0C | u32 | loop start, frames: where a looping sample jumps back to | V |
| +0x10 | u32 | "loop end", frames. **Never read by the exe**: a loop always runs to the end of the sample file (the mixer uses the file size), so **`loop_end == 0` means nothing** (not filled in by the tool) | V |
| +0x14 | u16 | 0; never read | V |
| +0x16 | u16 | runtime bank handle (0 in files) | V |

**C, sample file (8 bytes):** `u32 offset` (from file start), `u32 size` (bytes) [V].

**E, track def (16 bytes)**, `ZsndTrackDef` / `audio.TrackDef` [V] (`zsndSeqCreateTrack` 0x0047C6B0): `+0 u8` volume (0x7F everywhere), `+1 s8` pan, `+2 u8` default voice (A index, 0xFF = none), `+3 u8` default keymap (F index, 0xFF = none), `+4 u32` 0, `+8 u32` offset of a `ZTRK` record in the header area (the events start after the 4-byte tag), `+0xC u16` 0, `+0xE u16` runtime handle. Every track def in the game has voice and keymap 0xFF: the tracks pick voices themselves.

### 3.1 ZTRK byte code [V]

Decoder: `zsndSeqDecodeEvent` 0x0047D490 (+ `zsndSeqPeekDelta` 0x0047D450); `audio.ztrk_decode` / `ztrk_encode` round-trip all 42 tracks byte for byte, and a port of the exe's decoder agrees with them event by event (`tests/test_decomp_sound.py`). `dmbmx audio ztrk-dump BANK` prints them.

A record is `"ZTRK"` + events; it ends where the next record (or the header area) starts. An event is

    u8  opcode | form           opcode = bits 3-7, form = bits 0-2
    [delta]  [length]          per form, little-endian
    parameters                 per opcode

| form | delta (ms before this event) | length (note duration, ms) |
|---|---|---|
| 0 | - | - |
| 1 | - | u8 |
| 2 | - | u32 |
| 3 | u8 | - |
| 4 | u8 | u8 |
| 5 | u8 | u32 |
| 6 | u32 | - |
| 7 | u32 | u32 |

| opcode | name | parameters | effect |
|---|---|---|---|
| 0x00 | note | u8 note, u8 velocity | play the track's voice at (note - 60) semitones (or the keymap's voice + transpose) for `length` ms; velocity becomes the running velocity |
| 0x08 | note (running) | u8 note | the same with the running velocity |
| 0x10 | control | u8 control, u8 value | 0 expression, 1 pan (s8), 4 voice (A index, 0xFF none), 5 keymap; 2, 3 ignored |
| 0x18 | marker | u8 marker, u8 value | marker 1 = end of track (stop, or restart if the track loops); all go to the track callback |
| 0x30 | jump | u32 offset | continue at the event at that offset (from the first event) |
| 0x20, 0x28, 0x38, 0x40 | - | none | no handler (the exe then calls a stale pointer); unused |
| 0xF8 | indirect | u8 opcode, u8 sources, then 3 bytes per parameter whose source has bit 1 set | parameters from the track's registers; the exe masks the 2-bit source with 2 (a bug); unused |
| other | - | - | the exe stops decoding (returns 0); `ztrk_decode` raises |

Timing: each game frame (30 Hz) the track clock advances 1000/30 ms; volume = track volume x velocity x expression (/128 each). In the banks: opcodes 0x00, 0x08, 0x10 (controls 1 and 4) and 0x30 only; forms 0, 2, 3, 6, 7; every track ends with a jump back (endless ambience loops of 14 to 300 s); `sfxlev00` track 1 has unreachable events after its jump. The level ambience voices are 20-28 of each level bank (`dmbmx audio list` marks them).

## 4. Banks (install, [V])

| Bank | Samples | Voices / defs / tracks | Notes |
|---|---|---|---|
| `music.zsd` | 10 IMA streams | 0 / 0 / 0 | 28.5 MB; 120-136 s per track |
| `speech.zsd` | 1 (32 zero bytes) | 0 / 0 / 0 | placeholder |
| `sfxcom.zsd` | 6 | 6 / 6 / 0 | loaded at sound init (0x0044DDA0); 22050 Hz, 0.04-0.97 s |
| `sfxgame.zsd`, `sfxgame2.zsd` | 30, 4 | 112 / 49, 14 / 14 | gameplay; many loops |
| `sfxchr00/01.zsd` | 4, 4 | 4 / 4 | rider |
| `sfxmen00/01.zsd` | 2, 2 | 2 / 2 | 32000 Hz, ~4.4 s looping |
| `sfxlev00..12.zsd` | 1-14 | 20-29 / 1-15 / 0-4 | per level, with ZTRK tracks |

## 5. Writing

`write()` always uses the canonical layout. Replacing a sample with one of a different size only changes the C table, the total size and the data. `replace_sample()` / `dmbmx audio replace BANK ID WAV OUT`:
- reads 8- or 16-bit PCM WAV, mono or stereo, at any rate;
- mixes to mono and resamples linearly to the native rate (the sample def's rate, or 44100 for music);
- encodes as PCM16 or IMA-ADPCM (decoder-exact encoder);
- keeps loop points inside the new length (a loop that ended on the last frame still does).

## 6. Which sound is which

Sounds are addressed as (bank, voice). `bmxsoundEvent` (0x0044E300) maps the game's event codes onto them; the full map is `docs/re/sound.md` section 6, and `dmbmx audio list` names every voice from it (`audio.voice_events`, `audio.ztrk_voices`). In short: `sfxcom` = the menu / HUD sounds (events 2000-2005), `sfxmenNN` = the menu music (two loops, alternating banks), `sfxgame` = rolling loops (0-20), air / landing / grind / impact / body sounds (0x15-0x5B), scoring and HUD (0x5C-0x6F), `sfxchrNN` = four rider voices (01 = the "silly grunt" cheat), `sfxlevNN` = props (4-7, 12-15), crowd (16-19) and the ZTRK ambience (20+), `music` = 10 streamed tracks (each level starts with its own), `sfxgame2` / `sfxlev12` / `speech` = never used.

## 7. Open questions

- Plain-language names for the event codes (menu move / select / back, ...): the manual in-game check `sound-tones`.
- Voice byte +0x11 (0/1, 29 voices): not read on the PC; a PSX SPU flag [I].
- Settled by the sound pass [V]: voice fields +0x04..+0x11, sample flag bits, `loop_end` (never read), the ZTRK byte code, `speech.zsd` (never streamed).

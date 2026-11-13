# The sound system

How NgBMX.exe plays sound, from the decompilation of `bmxsound.c`, `zsndseq.c`, `zsndsrc.c` and the
PC backend `winChan.c` / `winLoad.c` / `winStrm.c` (`decomp/src/zaxis/Bmx/Src/...`, types in
`decomp/include/zsound.h`). The bank file format is in `docs/formats/AUDIO.md`.
Tags: [V] read from the code (disassembly / decompilation) or checked over the files,
[I] inferred. The original API prefix is `zsnd` (the CRT `assert()` texts name the globals:
`fzsndcInitialized`, `kzsndNumChannelHandles`, `fzsndStreamInitialized`, `kzsndSeqMaxTracks`,
`kzsndNoteEventFlagActive`).

## 1. Layers

| Layer | File | VA | What |
|---|---|---|---|
| game | `bmxsound.c` | 0x0044DD40-0x00450043 | `bmxsoundOpen/Close/Update/Service`, the one entry point `bmxsoundEvent(event, a, b, c, d)` |
| sequencer | `zsndseq.c` | 0x0047C4C0-0x0047DE06 | ZTRK tracks (32), notes (64): the level ambience |
| 3-D sources | `zsndsrc.c` | 0x0047DE20-0x0047EBC2 | 24 positional sounds; also 12 unused `zsndMusic*` stream wrappers and `g_zsndUpdateRate` (30) |
| channels | `winChan.c` | 0x0046D940-0x0046FA0A | `zsndc*`: a software mixer on one DirectSound buffer (DSOUND.DLL loaded at run time) |
| banks | `winLoad.c` | 0x0046FB20-0x00470A47 | `zsnd*Asset`, getters for voices / samples / sample files / track defs / keymaps |
| streams | `winStrm.c` | 0x00470AA0-0x004725D1 | `zsndStream*`: file streaming into DirectSound buffers, IMA-ADPCM decoding |

All 129 functions are drafted. The sound pass created 41 functions Ghidra had missed (only reached through
ILT thunks or function pointers, see the file headers) and moved three functions the skeleton had put at
the start of `bmxsound.c` to `frontend.c` (`frontendShutdownApp` 0x0044DC30 prints
"APP: Shutdown MENU", the twin of `frontendInitApp`'s "APP: Init MENU").

## 2. The mixer (`winChan.c`) [V]

- `zsndcInit` loads DSOUND.DLL, creates a primary buffer (22050 Hz, 16-bit stereo, `DSSCL_PRIORITY`)
  and one looping secondary buffer of two 50 ms blocks. `zsndcUpdate` runs from a 16 ms
  multimedia timer (`bmxsoundTimerProc` 0x00450020, at most every 4 ms): it mixes the time that
  has passed into a ring buffer and copies one block ahead of the play cursor (restoring a lost
  buffer).
- 64 handles map onto 24 mixer channels. `zsndcAlloc(voice, priority)` takes a free channel or
  steals the lowest-priority one if that is <= the request (game priorities: 0x20 positional,
  0x21 menu / HUD, 0x22 menu music, 0xFE 3-D sources, 0xFF sequencer notes).
- A channel plays one voice of a *resident* bank from memory: 16-bit (or 8-bit) mono (or stereo)
  PCM, 20.12 fixed-point stepping without interpolation. Step = voice pitch (4.12) x sample rate /
  22050, bent by cents through a semitone table (2^(n/12), n = -36..36, 0x0055E820) and a cent
  table (0x0055E8B4).
- **Loops** (`ZSND_SAMPLE_LOOP`) jump back to `loopStart` when the position reaches the **end of
  the sample file** (`numFrames = file size / bytes per frame`). `ZsndSample.loopEnd` (+0x10) is
  never read anywhere in the exe, so `loop_end == 0` is harmless: it is a value the PC build ignores.
  Flags 2 (stereo), 4 (8-bit) and 8 (ping-pong) are implemented but unused by every bank.
- Gains: `g_zsndcGain[128]` (0x0055E97C, 0 .. 1, an audio taper) by volume, and for the far side
  by `(127 - |pan|) x volume / 128`.
- **Envelope** from the voice bytes (times through `g_zsndcEnvTimeMs[128]` 0x0055EB7C, 1 ms .. 30 s):
  attack (+6: 0 -> 1), decay (+7: 1 -> sustainLevel), sustain (+8: sustainLevel -> 0 over that
  time; 0x7F = hold), release on key-off (+9: current level -> 0; 0 = cut), sustain level (+0x0A).
  The decay rate is computed with the wrong sign (it would rise), but every voice with a decay time
  has sustain level 127, which makes it 0. A new stage starts from where the previous ramp ended
  (the code overwrites the stage's start level with the old ramp's next value).
- Effects after the dry mix: a chorus (1024-frame line, raised-cosine LFO) and an echo line.
  `zsndcSetEffect` accepts type 0 (echo) and 1 (chorus); `bmxsound` only ever sends type 2 with PSX
  reverb mode / depth values (1/0x11, 2/0x46, 2/1, 0/0), which is refused (0x10D), and the echo
  line has no room (`ZsndcInitParams.echoSeconds` = 0). **The PC build mixes dry.**
- Output: sum, round, clamp to +-0x10000, halve.

## 3. Streams (`winStrm.c`) [V]

- Up to 8 streams, each with its own DirectSound buffer (64 KB default; the music uses 256 KB) and
  a queue of files (`ZsndStreamQueue`: file + loop count).
- A file is a byte range of a bank (`zsndGetSampleFile`: bank path, offset, size). The data is
  raw PCM (`ZSND_STRM_PCM` 0x800) or **IMA-ADPCM**: mono (stereo is forced off), low nibble first,
  one continuous stream starting at predictor 0 / step 7 / index 0, no block headers
  (`zsndImaDecode` 0x004724C0, ported 1:1 by `dmbmx.formats.audio.ima_decode`; the test decodes all
  10 music tracks both ways, in the exe's 0x4000-byte chunks). Rate flags: 0x40 = 44100,
  0x10 = 16000, 0x08 = 11025, else 22050.
- `zsndStreamService` (also timer-driven, through `bmxsoundService`) refills half a buffer behind
  the play cursor; when the data runs out the rest is filled with silence and the stream stops
  (or is destroyed, `ZSND_STRM_AUTOFREE`) once the cursor reaches the end. `zsndStreamUpdate`
  (per frame) runs fades in a linear-dB approximation (`(volume - 127) x 12` hundredths of a dB)
  while `zsndStreamSetVolume` uses the table `g_zsndStreamAtten` (0x0055EC7C).

## 4. Banks (`winLoad.c`) [V]

- 16 asset slots. `zsndLoadAsset(path, resident, &handle)`: a **resident** bank (sfx) is read
  whole; a **streamed** bank (`MUSIC.ZSD`, `SPEECH.ZSD`) only up to `headerSize` (+0x0C), and its
  path is kept for the stream code.
- `zsndRegisterAsset` compares `"ZSND"` and `"WIN "` (nothing else is checked: no checksum),
  turns the six table offsets into pointers, stamps the bank handle into voice +0x12, sample +0x16,
  track def +0x0E and keymap +2, relocates track def +8 (ZTRK) and, for resident banks, sample +8
  (PCM pointer). The table names come from the getters' asserts: A voice, B sample, C sample file,
  D phrase, E track def, F keymap. D and F are empty in every bank.

## 5. The sequencer (`zsndseq.c`) [V]

ZTRK byte code (decoder `zsndSeqDecodeEvent` 0x0047D490; the format is in AUDIO.md 3.1):

- Each game frame (30 Hz) a playing track's clock advances 1000/30 ms (20.12 fixed point) and every
  event whose delta time has passed is executed. **Times are milliseconds** (at tempo 1).
- Note: the track's voice plays at `(note - 60)` semitones (or a keymap gives voice + transpose);
  volume = track volume x velocity x expression (all /128); pan = the track's pan. The length field
  is the note's duration in ms; the note is keyed off after `duration x 30 / 1000` frames (at least
  one). Notes live in a delta list per track.
- Control 0 expression, 1 pan, 4 voice (0xFF = none), 5 keymap; 2 and 3 are accepted and ignored.
- Marker 1 = end of track: stop, or restart with `ZSND_TRK_LOOP` (which drops the sounding notes
  without a key-off); every marker goes to the track callback.
- Jump: continue at the first event + u32. **Every one of the 42 tracks ends with a jump**: the level
  ambience loops forever. One track (`sfxlev00` track 1) has events after a jump that are never
  reached.
- Opcodes 0x20, 0x28, 0x38, 0x40 decode with no parameters and no handler: `zsndSeqUpdate` then calls
  whatever stale pointer is on its stack (none of them occurs in a bank). The 0xF8 "indirect"
  form (parameters taken from the track's four registers) masks its 2-bit source selector with 2
  instead of 3; no bank uses it.
- `bmxsound` starts every track of `SFXLEVnn.ZSD` with the level (volume = sfx volume) and pauses /
  resumes them with the game. Level 4 has an inside and an outside ambience: event 4001 (the camera
  position) turns one down to 20/128 and fades track 0 with the camera height.

## 6. The game layer and the event map (`bmxsound.c`) [V]

`bmxsoundOpen`: mixer at 22050 Hz, `g_zsndUpdateRate` = 30, sequencer, 3-D sources (range 2500,
life 100 ms), `SFXCOM.ZSD` resident, `MUSIC.ZSD` / `SPEECH.ZSD` headers (speech is never played),
the 16 ms timer. The game makes every sound through `bmxsoundEvent(event, a, b, c, d)`:

| Event | Arguments | Plays / does | Callers |
|---|---|---|---|
| 1000 | a = mode 0 off, 1 menu, 2 game, 3 paused; for 2: b = level, c = SFXCHR set (1 = the "silly grunt" cheat, save cheat bit 5, action.c), d = "gun" (always 0) | the mode switch: menu loads `SFXMEN00`/`01` (alternating) and keys on voices 0 and 1 as the menu music; game loads `SFXGAME` + `SFXLEVnn` (with d: `SFXGAME2` + `SFXLEV12`) + `SFXCHRnn`, creates the music stream (track of `g_bmxsoundLevelTrack[level]`), 2 x 2 rolling sources and the ambience tracks; pause plays SFXGAME voice 110 | action.c, game.c, overlay.c, frontend.c |
| 1001 | a = music mode (0-2 on, 3 stop, 4 fade) | music fade-out over 750 ms / stop | action.c, game.c |
| 1002 | a = 0 sfx / 1 music, b = volume | volumes (menu music, stream, ambience) | frontend.c |
| 1003 | a, b | PSX reverb request (refused on the PC) | none |
| 1004 | - | returns the current / next music track | action.c, overlay.c |
| 1005 | a = track | next music track | overlay.c |
| 2000 | - | SFXCOM voice 0 (priority 0x21) | frontend.c, overlay.c (most calls) |
| 2001 | - | SFXCOM voice 1 | overlay.c, overlymp.c, frontend.c |
| 2002 | - | SFXCOM voice 2 | frontend.c, overlay.c, overlymp.c |
| 2003 | - | SFXCOM voice 5 in the menu, 3 in the game | overlay.c, overlymp.c, frontend.c |
| 2004 | - | SFXCOM voice 4 | frontend.c, overlay.c, overlymp.c |
| 2005 | - | SFXCOM voice 5 | none |
| 2008 | - | nothing | frontend.c |
| 2009 | - | nothing | frontend.c |
| 3000 | a = 0, b = voice | any SFXCOM voice | none |
| 3001 | a = position, b = kind, c = strength | SFXGAME 0x15 + kind | physics.c (air) |
| 3002 | a = position, b = surface << 8 \| kind, c = strength | SFXGAME 0x28 + kind (surface 0/1), 0x31 + kind (2), 0x3A (3), 0x3C (4), 0x3B (5) | physics.c (grind, stall, landing) |
| 3003 | a = position, b = kind, c = strength | SFXGAME 0x1F + kind | physics.c (rolling) |
| 3004 | a = position, b = material << 8, c = force | SFXGAME 0x3D + 3 x material + min(force / 32, 2) | physics.c, prop.c, rider.c |
| 3005 | a = position, b = 0-3, c = strength | SFXGAME 0x58 + b | rider.c, prop.c |
| 3006 | a = position, c = strength | SFXCHRnn voice 0-3 at random, pitch + 64 x c cents | rider.c, prop.c |
| 3007 | a = position, b = voice | SFXGAME2 looping channel | none |
| 3008 | a = position, b | SFXGAME2 4 + b | none |
| 3009 | a = position, b | SFXGAME2 10 + b | none |
| 4000 | a = position, b = player << 28 \| kind bits, c / d = speeds | the player's two rolling sources: SFXGAME 0-11 (kind 1) or 12 + n (kind 2), and 9 + n; volume 4 x speed (full from 32), pitch 8 x speed - 416 cents | physics.c |
| 4001 | a = camera position | level 4 ambience swap (see 5.) | camera.c |
| 4002 | a = position, b = 0 create / 1 free / 2 move / 3 one-shot, c = kind or handle, d = volume | SFXLEVnn 4 + kind (loops are 3-D sources) | prop.c, rider.c |
| 4003 | as 4002 | SFXLEVnn 12 + kind | prop.c |
| 5000 | a = seconds left | SFXGAME 0x5C (1-10), 0x5D (0) | game.c |
| 5001 | a = combo count | SFXGAME 0x5E, pitch + 33 x a cents (max 500) | game.c |
| 5002 | - | nothing | none |
| 5003 | a = excitement, b | crowd: SFXLEVnn 0x10-0x13 (b, a >= 4), random pitch | game.c |
| 5004 | a | SFXGAME 0x60 / 0x5F / 0x61 | game.c |
| 5005 | a | SFXGAME 0x61 + a | game.c |
| 5006 | a | SFXGAME 100 + a | game.c |
| 5007 | - | SFXGAME 104 | game.c |
| 5008 | - | SFXGAME 105 | overlay.c |
| 5009 | - | SFXGAME 106 | game.c |
| 5010 | - | SFXGAME 107 | game.c |
| 5011 | - | SFXGAME 108 and 109 | overlay.c |
| 6000 | - | SFXGAME 111 (priority 0x21) | action.c |

Filters: nothing plays while closed; in music mode 2 the 5000-5999 events are muted; while paused
3001-4000 are dropped. Positional one-shots get their level and pan from `bmxsoundPlace` ->
`zsndSrcGetLevelPan` (level 127 x (1 - d^2/2500^2), pan from the sideways offset to the camera).
`dmbmx audio list` prints these names per voice (`audio.voice_events`), and marks the voices the
ZTRK tracks play as ambience. Which *sound* each event is (menu move / select / back, ...) still
needs the manual in-game check `sound-tones`.

The music: `bmxsoundUpdate` (per frame) starts `g_bmxsoundLevelTrack[level]`
(7, 1, 5, 2, 3, 4, 8, 6, 0, 9, 7, 6 for levels 0-11) when the stream is stopped, then, with
`g_bmxsoundMusicCycle` (1), the next track (wrapping after the 10th) each time one ends, at
15/16 of the music volume.

## 7. Bugs and oddities [V]

- `zsndcInit` clears 65 channel handles (`<=` on the end address) and `zsndcFreeAll` loops to 0x41:
  one past `g_zsndcHandles[64]`, onto `g_zsndcMixL`.
- `zsndSrcCreate` gives up after looking at one slot: creation fails whenever `g_zsndSrcNext` is in
  use (`cmp esi, eax; jne fail`).
- `g_zsndSrcDopplerScale = 0.0 / maxDist`: the Doppler shift is always 0.
- `zsndSeqCreateTrack` looks up the track def's default voice / keymap in the slot's *previous*
  bank (harmless: every track def has 0xFF / 0xFF).
- `zsndSeqSetTempo` scales the note lengths the wrong way (both rates are multiplied by the tempo).
- `zsndSeqPlayTrackDef` returns 0 instead of the track handle; `ZSND_TRK_PENDING` is never cleared.
- `zsndStreamOpenFile`'s failure path frees the slot *before* the one it took.
- `zsndLoadAsset`'s "file name too long" check measures the (empty) stored name, not the new one.
- `zsndMusicPause` is the same call as `zsndMusicStop`.

## 8. Open questions

- What the 29 voices with byte +0x11 = 1 meant on the PlayStation (the PC never reads it).
- The plain-language name of each event (PT4).
- The "gun" set: `SFXGAME2.ZSD` (14 voices, events 3007-3009) and `SFXLEV12.ZSD` (the level bank
  used instead of the level's own when event 1000 gets d != 0) look unused: `actionLoad`,
  `gameEndRun` and three of the four overlay.c sites push d = 0 [V] (overlay_00415400 pushes a
  register, not checked), and no code sends 3007-3009 [V]. A cut feature [I].

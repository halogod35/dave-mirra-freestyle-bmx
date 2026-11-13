# SAVE.DAT (`.\save\SAVE.DAT`)

Game-logic sections (2.2, 2.4, 7) come from the decompiled game.c. Code: `tools/dmbmx/formats/save.py`, `tools/dmbmx/formats/save_fields.py`, `tools/dmbmx/formats/progress.py` (Python port of game.c/comdata.c: goals, rewards, records), `tools/dmbmx/integrity/crc.py`, CLI `dmbmx save info|fix-crc|list-fields|get|set|unlock-all|complete-all`.
Decompilation: `decomp/src/zaxis/Bmx/Src/com/action/game.c`, `comdata.c`, types `decomp/include/game.h`, `save.h`.
Exe: `C:\zaxis\Bmx\Src\com\action\memcard.c`; load `memcard_LoadSave` **0x00412190**, save `memcard_WriteSave` **0x00412340**.
Tags: [V] verified (disassembly and/or code run over the real file), [I] inferred.

## 1. Summary

- [V] The file is a **raw dump of three global memory blocks** behind an 8-byte header. It is fixed size: **0x235C = 9,052 bytes**, little-endian.
- [V] **There is no CRC or checksum.** The load routine reads 0x235C bytes, checks `u32 @4 == 0x235C`, and `rep movsd`-copies the three blocks into globals; nothing else is computed (§4). Verified empirically too: no CRC-32/JAMCRC/Adler-32/CRC-16 (XMODEM/CCITT/ARC)/8- or 32-bit sum over any plausible range appears anywhere in the file.
- [V] The "RLE-like" patterns (`81 05 00`, `84 55 55`) are **not compression**. They are 3-byte goal entries (section 2.2); the file is copied to memory verbatim.
- [V] The game writes the file with `fopen(..., "wb")` (mode 2 of `zrwFile_ModeTable` 0x00563038) and reads it with `"rb"` (mode 1).

## 2. Layout

| File off | Size | Global (VA) | Content | Tag |
|---|---|---|---|---|
| 0x0000 | 4 | (buffer only) | magic `1C 51 81 2A` = `0x2A81511C` (written at 0x004123A7, **not checked** on load) | V |
| 0x0004 | 4 | (buffer only) | size `0x235C` (written at 0x004123AD, **checked** at 0x004121F8) | V |
| 0x0008 | 0xC3C | `g_saveBlockA` 0x006955E0 (0x30F dwords) | options + 12 rider career records + tail | V |
| 0x0C44 | 0x1680 | `g_saveRecords` 0x00551FD8 (0x5A0 dwords) | per-level records/scores: 12 × 24 × 20 bytes | V |
| 0x22C4 | 0x98 | `g_saveProfiles` 0x006952A0 (0x26 dwords) | 2 rider profiles × 0x4C | V |
| 0x235C | end | | | V |

The copies are at 0x00412200-0x00412231 (load) and 0x0041236B-0x004123A0 (save).

### 2.1 Block A: options (file 0x08..0x0F, global 0x006955E0..E7)

Defaults come from `comdataInitDefaults` 0x00407BA0 (the save reader's `save_InitDefaults`; the values the game uses when there's no save). [V] It clears bytes 0-2 of each goal info (byte0, tier, tier_goals_done) twelve times instead of the twelve goal entries (its inner loops never advance the pointer, 0x00407BDD), so the goal entries keep their zero `.bss` value. Field names are the `dmbmx save` names (section 6).

| Off | Type | Global | Default | Real save | Field / meaning | Tag |
|---|---|---|---|---|---|---|
| 0x08 | u16 | 0x006955E0 | 0 | 0 | `cheats_unlocked`: bit c = cheat c listed in the CHEATS menu (builder 0x00440DC0 walks table 0x0055A3BC: 0 First Person Cam, 1 Night Vision, 2 Super Crash, 3 Exorcist, 4 Suspension, 5 Silly Grunt, 6 Ghost Rider, 7 Sticky Crash). Set by reward type 10 (0x004111CF), the reward for finishing every level with riders 2-9 (table 0x00556968) | V |
| 0x0A | u16 | 0x006955E2 | 0 | 0 | `cheats_active`: bit c = cheat c switched on: OPTIONS > CHEATS (0x00441180) toggles it for the unlocked cheats; tested in-game (bit 3 at 0x0041FC40, bit 6 at 0x00405780) | V |
| 0x0C | u8 | 0x006955E4 | 18 | 9 | `volume_0c`: SOUND FX volume 0..18 (OPTIONS > AUDIO, 0x00442CE0, the front-end pass); ×7 → sound cmd 0x3EA ch 0 (0x0043F4E5) | V |
| 0x0D | u8 | 0x006955E5 | 18 | 9 | `volume_0d`: MUSIC volume 0..18 (OPTIONS > AUDIO); ×7 → sound cmd 0x3EA ch 1 (0x0043F4C8) | V |
| 0x0E | u8 | 0x006955E6 | 1 | 7 | `music_track`: soundtrack shown as text 0x102E+4v (artist) / 0x102F+4v (title) at 0x00416810. 0 = 59 Times the Pain … 7 = Sublime … 9 = Primer 55 | V (use), I (range) |
| 0x0F | u8 | 0x006955E7 | 10 | 11 | `riders_unlocked`: selectable riders. 11 = + Slim Jim Guy (level 11 hardcore reward), 12 = + Amish Boy (all levels done with riders 0-9; also the cheat at 0x0044716E). Reward type 3 asserts "goal completion error - char" | V |

### 2.2 Block A: 12 rider career records (file 0x10 + r×0x104, global 0x006955E8 + r×0x104)

[V] **One record per rider, not per level** (the save reader guessed levels). Every accessor indexes it by `profile.rider`, as `*(u8*)(0x006952A3 + p*0x4C) * 0x104` (0x00411090, 0x00407FC0, 0x00410C60). Rider order (pointer table 0x0055DC50 and the default score names): 0 Dave Mirra, 1 Ryan Nyquist, 2 Troy McMurray, 3 Mike Laird, 4 Chad Kagy, 5 Tim Mirra, 6 Kenan Harkin, 7 Shaun Butler, 8 Leigh Ramsdell, 9 Joey Garcia, 10 Slim Jim Guy, 11 Amish Boy.

| Rec off | Type | Default | Field / meaning | Tag |
|---|---|---|---|---|
| +0x00 | u32 | 0 | `flags`. Bit 0 = "all levels complete" reward type 8; for riders 0/1/11 that is their movie in MOVIES (0x0044A4B0 tests bit 0 of records 0, 1, 11). Bit 1 = all 12 levels hardcore done (0x00410F2A). Bits 2-5 = park k done (`4 << park`, 0x00410EF8). 0x40/0x80/0x100 = "new bike / style / level" notices: set with the unlock (0x004110F1/0x0041110F/0x00411172) and cleared at run start (`& 0xFFFFFE3F`, 0x004099C0) | V |
| +0x04 | u8 | 1 | `levels_unlocked` (1..12): length of the SELECT LEVEL list. Reward type 4 (`max(cur, p+1)`, asserts p ≤ 11 "goal completion error - level"); the cheat sets 12 | V |
| +0x05 | u8 | 1 | `styles_unlocked` (1..5): SELECT STYLE ("STYLE n", text 0xDB; page 0x00445B90 greys items ≥ this count). Reward type 2; the cheat on page 2 sets 5 | V |
| +0x06 | u8 | 1 | `bikes_unlocked` (1..5): SELECT BIKE (4 bikes + "SECRET BIKE"). Reward type 1; the cheat on page 1 sets 5 | V |
| +0x07 | 6 × 0x27 | 0 | goal info for the 6 **challenge levels** 0..5 (`0x00407FC0`, asserts level < 6 "invalid level for goal info"); see below | V |
| +0xF1 | 6 × u8 | 0 | `comp.<L>`: competition levels 6..11 at `+0xEB + L`, best result 0..3 (0x00410C60 stores `3 - placing`) [I: 3 = 1st] | V (offset) |
| +0xF7 | 11 bytes | 0 | `trick.<n>`: 88-bit "trick landed" set (0x0040DAE0 sets bit `trick_id`, and a second id offset by 0x2E) | V |
| +0x102 | 2 bytes | | unknown | I |

Goal info (39 bytes, one per challenge level):

| Off | Field | Meaning | Tag |
|---|---|---|---|
| +0 | `byte0` | never written by game.c; 0 in the real save (comdataInitDefaults clears it) | I |
| +1 | `tier` | current challenge tier: 0 amateur, 1 pro, 2 hardcore (texts 0x30-0x32). `gameAdvanceGoalTier` 0x0040CD40 recounts it at every run start: while the tier has 4 done goals it moves on, capped at 2 | V |
| +2 | `tier_goals_done` | goals done in the current tier; 4 = tier complete (`gameIsTierComplete` 0x0040CF70) | V |
| +3 + t×12 + g×3 | goal (t 0..2, g 0..3), 3 bytes | byte 0: 0x80 done, 0x40 done this session (set together with 0x80, cleared by `gameAdvanceGoalTier` at the next run start, so a save written at level end can still have it), low 6 bits = objectives done so far (incremented per objective; equals the goal's objective count when done) [V, the game.c pass]. Bytes 1-2: 4 nibbles, one per objective j (j odd = high nibble); the low 3 bits are its state, bit 3 a flag. States [V] (`gameUpdateObjectives` 0x0040EC40): 0 idle, 1 rider inside the objective's area, 2 active (its text and highlights shown), 3 pending (the trick is in the air; done if the set is landed), 4 done, 5 done and counted. `gameSetObjectiveState` 0x00410350 writes the whole nibble (clears the flag); the flag is set for every open objective at run start and cleared when the objective first becomes active ("not seen yet" [I]) | V |

What each goal asks for (the goal definitions 0x00555118), the reward tables and the order in which the game gives rewards: section 7.

### 2.3 Block A tail (file 0xC40..0xC43)

| Off | Type | Global | Default | Meaning | Tag |
|---|---|---|---|---|---|
| 0xC40 | u8 | 0x00696218 | 0 | `language`: passed to the frontend loader (0x0044D990, "FRONTEND: Failed to load frontend") and to 0x00401DD4 after load. 0 US English, 1 English, 2 French, 3 German: the frontend loads STRS 0x7919 + language (0x0044DAD0, the front-end pass) | V |
| 0xC41 | 3 bytes | | | padding/unknown | I |

### 2.4 Block B: records/scores (file 0xC44, 12 levels × 24 entries × 20 bytes)

- [V] Accessor `comdataGetRecordPtr` 0x00407D20 (the save reader's `save_GetRecordPtr`): entry `e` of the current level (`g_currentLevel`, byte 0x0056B614) at `0x00551FD8 + (level*24 + e) * 20`. Its live twin `gameGetLiveRecord` 0x0040BE50 has the same switch over 4 slots × 24 × 8 bytes at 0x0056B70C (slot 0 = the saved all-time value, loaded at level start by `gameInit`; 1 this game, 2 this round, 3 this run).
- [V] Entry layout: `u32 value; u8 rider; u8 b5; u8 b6; char name[13]` (NUL-padded). [I] `rider` is the rider index: the default names per level are the riders' nicknames in rider order (MiracleBoy, Nor Cal, warpig, H breed, k a g y, turbo, Hark Dog, Lumber, CaptInsano, The Boss).
- [V] **Record ids**: `mode << 24 | sub << 22 | stat << 16 | slot << 12 | index`. `mode` is the game mode (text 0x3001 + mode: 0 single player, 1 best run, 2 high five, 3 B-M-X, 4 random spot, 5 wipeout, 6 longest grind, 7 highest walltap, 8 big air contest, 9 furthest jump, 10 longest manual, 11 death match); `sub` the sub-mode (single player: 0 proquest, 1 session, 2 free ride [I]; best run / death match: the time limit 30 s / 60 s / 2 min / 5 min, `gameSetState` 0x0040AA50); `stat` the statistic (text 0x1094 + stat: 0 total score, 1 best run, 2 trick set, 3 single trick, 4 big air, 5 highest mark, 6 longest grind, 7 longest jump, 8 longest manual, 9 longest skid).
- [V] **The 24 entries** (`dmbmx.formats.progress.RECORD_ENTRIES`). Stats 2..9 always map to entries 16..23, whatever the mode; otherwise the mode decides:

  | Entry | Record | Entry | Record |
  |---:|---|---:|---|
  | 0-2 | single player: top 3 run scores (index = rank; the RECORDS screen shows entry 0) | 12-15 | death match, 30 s / 60 s / 2 min / 5 min |
  | 3-6 | best run, 30 s / 60 s / 2 min / 5 min | 16 | trick set (stat 2) |
  | 7 | high five | 17 | single trick (stat 3) |
  | 8 | B-M-X | 18 | big air (stat 4; = big air contest) |
  | 9 | random spot, run (stat 1) | 19 | highest mark (stat 5; = highest walltap) |
  | 10 | random spot, total (stat 0) | 20 | longest grind (stat 6; = the game) |
  | 11 | wipeout | 21 | longest jump (stat 7; = furthest jump) |
  | | | 22 | longest manual (stat 8; = the game) |
  | | | 23 | longest skid (stat 9) |

  Values are points, or centimetres for stats 4-9 (the goal texts: 5,486 = "180 feet", 914 = "30 feet") [V]. `gameSubmitRecord` 0x0040E030 updates the live slots and shows "NEW HIGH SCORE!" etc. (text 0x1063 + message id); the name-entry overlay copies the beaten slot-0 values into the save [V] (the overlay pass, below).
- [I] Level order (texts 0x126-0x131; park = level / 3, as the park rewards use it): 0 Greenville Park, 1 Vail Trails, 2 Backyard Pools, 3 Lot 8, 4 Woodward Trails, 5 Titanic, 6 Eastwood, 7 Sleepy's Ditch, 8 San Jose, 9-11 Acclaim Max Games Street/Dirt/Vert. Levels 0-5 are challenge levels, 6-11 competitions.

### 2.4a How records and the career reach SAVE.DAT

- [V] A run that beats an all-time record of the mode (live slot 0, flag bit 0) opens the NEW RECORD! name entry (overlay screen 17, `gameSetState`). On confirm, for every beaten stat 0..9 `overlayNameUpdate` (0x00418870) copies `value`, `rider` (player option +4), `b5` / `b6` (options +6 / +7) and the typed name into the block-B entry (`comdataGetRecordPtr`), and the name into `profile.name`. The RECORDS screen then shows the records with the PLYR full name of `rider` (entry rider x 5) next to the typed name.
- [V] SAVE.DAT is written (`memcard_WriteSave` 0x00412340) automatically at every level end, after these screens: by `gameSetState(LEVEL_END)` itself when no record was beaten and no results are due, otherwise when leaving RECORDS (19), RESULTS (16, after the rewards), the competition STANDINGS after the last run, or the level-end CHALLENGES list (15). The memory-card SAVE screen of the PSX version is still in the code but unreachable on PC. Details: docs/re/hud.md.

### 2.5 Block C: 2 player profiles (file 0x22C4 + p×0x4C, global 0x006952A0 + p×0x4C)

Defaults from 0x00407C40-0x00407CB3.

| Off | Type | Default | Field / meaning | Tag |
|---|---|---|---|---|
| +0x00 | u8 | 3 | `flags`; bit 1 = `tutorial_hints` (shown on/off in the pause OPTIONS, 0x00416060; cleared by the level-2 unlock reward, 0x004111AC) | I |
| +0x01 | u8 | profile index | `index`: the pad this player reads (`contrlerTestButtons` 0x00408590, `riderReadInput`) | V |
| +0x02 | u8 | 0 | `controller_config` 0..2 = CONFIGURATION A/B/C (STRS 0x23D-0x23F): the action -> button table 0x00553EE0 (`contrlerTestAction` 0x004084B0 asserts < 3; copied to the rider at 0x004316F2). The camera pass, docs/re/input-camera.md 3 | V |
| +0x03 | u8 | 0 | `rider` 0..11 (selects the career record) | V |
| +0x04 | u8 | 0 | `style` 0..4 (costume; argument 3 of `create rider` 0x004316C0) | V |
| +0x05 | u8 | 0 | `bike` 0..4 (0x004316C0: below 4 → bike `rider*4 + n`; 4 = secret bike) | V |
| +0x06 | u8 | 0 | `camera`: 0 behind, 1 wide, 2 above, 3 first person (text 0x1087+v at 0x00416060) | V |
| +0x07 | char[13] | "player" (0x00553E84) | `name`, NUL-terminated. The save reader read 29 bytes (to +0x23); but the CONTROLLER screen stores bindings from +0x14 on (below), so at most 12 characters are safe (`save_fields` limits it to 12; `save.py` still reads 29). The in-game name entry (overlay.c, the overlay pass) starts from this name, allows at most 10 characters (' ', A-Z, a-z) and writes the result back here [V] | V offset, V length used by the game |
| +0x14 | u32 ×4 | 0 | `dpad.<k>` up/down/left/right bindings of the CONTROLLER screen (rows 0-3; 0x00441CB0 shows them, 0x0044256B stores rows 0-10 to +0x14..+0x3F). Never applied: `contrlerUpdate` passes the d-pad through | V |
| +0x24 | u32 ×7 | 0x80, 0x20, 0x40, 0x10, 0x100, 0x200, 0x4000 | `control.<k>`: the device buttons (PAD_* mask) that produce logical button k = Cross 0x80 (pedal/jump), Square 0x20 (trick), Circle 0x40 (modifier), Triangle 0x10 (grind), L1 0x100 (spin left), R1 0x200 (spin right), Start 0x4000 (pause). `contrlerUpdate` 0x004080F0: logical bit set when `device.buttons & control[k]`; the default is the identity. Pad i uses profile i's masks | V |
| +0x40..+0x4A | | 0 | unknown | I |
| +0x4B | u8 | 0 | `keyboard_layout` 0..4 ("< KEYBOARD LAYOUT n >" 0x0055A43C); at level load pad 0's keyboard map becomes 0x0055BB98 + 32 x layout (0x0045BE60, profile 0 only) | V |

## 3. Integrity / acceptance

| Condition | Exe behaviour | Tag |
|---|---|---|
| File missing / can't open | returns 5 ("couldn't open file … for reading") | V |
| Fewer than 0x235C bytes read | returns 9 ("couldn't read from file") | V |
| More than 0x235C bytes | extra bytes ignored (only 0x235C are read) | V |
| `u32 @4 != 0x235C` | returns 8, prints "Error: memory card data is invalid (probably old version)", **nothing copied** | V |
| magic `@0` wrong | **accepted** (never read on load) | V (static) |
| any other byte changed | accepted, copied verbatim into the globals | V (static) |
| temp alloc fails | returns -1 | V |

Callers: 0x0043DD26 (start-up; stores `0x102 + (ret != 0)` in word 0x00691F00), 0x0043F3E0 (load menu: ret 0 → text 0x25E, ret -1 → text 0x272 then 0x261, other → text 0x261), 0x0043FA78. [V] 0x25E = "LOAD COMPLETE.", 0x261 = "LOAD FAILED.", 0x272 = "FILE MAY BE CORRUPTED.". The save editor decoded the STRS table in `miscdata.zal` (`u16 count` at +6, then `count` u16 offsets, then the strings); the save reader had guessed 0x25E = "LOADING...". Text ids ≥ 0x1000 select table `id >> 12` (lookup 0x004824F0).

**Re-signing** an edited save therefore means only: magic = 0x2A81511C, size = 0x235C, length exactly 0x235C (`dmbmx save fix-crc`, `Save.resign()`). In-game confirmation is pending (the manual in-game check `save-edit`); until then "the game accepts an edited save" is [I].

## 4. Codec

- `save.read(data, lenient=False)`: `TruncatedError` if < 0x235C bytes; unless `lenient`, `BadMagicError` (wrong magic; the codec is stricter than the game here) and `ChecksumError` (size field ≠ 0x235C, which the game rejects). Bytes past 0x235C are kept in `Save.extra`.
- `save.write(s)`: byte-exact; `write(read(b)) == b` [V on the real save].
- `crc.check_save(data)`: the exe's exact accept/reject decision. `crc.resign_save(data)`: fix the header.

## 5. Open questions

- Which of `volume_0c` / `volume_0d` is music and which is SFX, and their exact range (default 18).
- The front-end pass confirmed via the CHEATS menu code (0x00441180) that `cheats_active` bit c is cheat c switched on, and the `language` values (2.3). The camera pass: the bits follow the cheat order (`CHEAT_NAMES`): bit 1 night vision (green overlay, `cameraDrawNightVision` 0x004074C0), bit 5 silly grunt (sound init in `actionLoad`), bit 6 ghost rider (rider actor hidden, `cameraUpdate`) [V code, I names].
- Profile name length: 12 characters by the layout above [I]. The overlay pass: the in-game name entry (`overlayNameUpdate` 0x00418870) writes at most 10 characters here [V]; the frontend's own name editing is not checked. `save.py` (`PROFILE_NAME_LEN` = 29) should follow.
- Objective nibble bit 3 ("not seen yet" [I]); goal-info byte 0 (never written by game.c); rider-record bytes +0x102/+0x103. (the game.c pass settled the goal-entry low bits: objectives done.)
- Score entries: `b5`/`b6` = bytes +6 / +7 of the player's option block (`zrollerGetPlayer(player)`; +7 is the bike [I], +6 unknown), copied by the name entry together with `rider` (+4) [V]. (the game.c pass settled the 24 categories, section 2.4.)
- ~~Reward types 5, 6, 7~~: answered (section 7.2): RESULTS-screen texts only.
- Whether `memcard_Stub0` (0x00412170, `return 0`) is the PC stub of a PSX memory-card "format" or "delete" call.

## 6. Editor (`dmbmx save`, the save editor)

`tools/dmbmx/formats/save_fields.py` holds one `FieldDef` per field: a dotted name pattern, offset, type, allowed range, [V]/[I] tag, evidence VA and named values. That gives about 8,150 concrete fields; `rider.<r>.level.<L>.goal.<t>.<g>.objective.<j>` alone accounts for 3,456.

```
dmbmx save list-fields [SAVE.DAT] [--grep TEXT]          # patterns, types, ranges, tags (+ values)
dmbmx save get SAVE.DAT riders_unlocked profile.0.camera rider.5.bikes_unlocked
dmbmx save set SAVE.DAT rider.5.bikes_unlocked 5 profile.0.camera "first person" -o NEW.DAT
dmbmx save unlock-all SAVE.DAT -o NEW.DAT                # or --in-place
dmbmx save complete-all SAVE.DAT -o NEW.DAT              # every challenge + competition, with the rewards (7.3)
```

- `set`, `unlock-all` and `complete-all` write a **new** file unless `--in-place` is given; an `-o` equal to the input is refused. The header is always re-signed (section 3), so there is no separate CRC step.
- Values: decimal or `0x` hex, a named choice (`"Amish Boy"`, `wide`, `Sublime`), or for bools `on/off/true/false/1/0`. Out-of-range values, bad indices and unknown fields are refused with an error, and nothing is written.
- `unlock-all` only raises, never lowers: `riders_unlocked` = 12, all 8 cheats, and for every rider levels 12, styles 5, bikes 5, plus `movie_unlocked` for riders 0, 1 and 11. These are exactly what the game's own cheats (0x0044716E) and final rewards set. Progress, goals, scores and names are not touched.
- In-game confirmation: the manual in-game check `unlock-all` (`tools/testing/playtest/make_playtest_unlock_all.py`). Until it passes, "the game shows the unlocks" is [I].

## 7. Challenges, rewards and the career

Source: `game.c` (decompiled by the game.c pass), tables in the exe; Python port `tools/dmbmx/formats/progress.py`; tests `tests/test_decomp_game.py`.

### 7.1 Goal definitions (`g_goalDefs` 0x00555118)

[V] `GoalTierDef[18]`, index `level * 3 + tier` for the six challenge levels: 4 goals of 0x54 bytes = `u8 numObjectives` (1..4), `u8 props[3]` (bmxprop ids switched when the goal is done, 0xFF none), then 4 objectives of 0x14 bytes = `u16 type`, `u16 param[3]`, `s16 area[4]` (x0, z0, x1, z1 in cm: the objective is only live while the rider is inside), `u8 props[4]` (switched when the objective is done). A goal is done when all its objectives are. Objective types (`game.h` `GOAL_OBJ_*`; checked by `gameUpdateObjectives` 0x0040EC40, `gameCheckTransfer` 0x0040C300 at landings, `gameOnPropTouched` 0x0040CA60, `gameOnObjectHit` 0x0040CBE0 and `gameSubmitRecord` 0x0040E030):

| Type | Meaning (params) | Type | Meaning (params) |
|---:|---|---:|---|
| 0 | trick p0 (0xFF any) with >= p1 half spins, landed | 11 | as 10 with trick p2 in the set |
| 1 | trick p0/p1 off edge tag ~p2 (+-5) | 12 | touch a prop of group p0..p1 |
| 2 | trick p0/p1 off prop group p2 | 13 | transfer between edge tags ~p0 and ~p1 (either way) |
| 3 | trick p0/p1 off an object with tag >= p2 | 14 | as 13 with trick p2 |
| 4 | >= p0 air tricks in one set | 15 | as 13 with >= p2 half spins |
| 5 | rider state == p0 | 16 | transfer between objects p0 and p1 |
| 6 | rider state p0 on edge tag p1..p2 (0 riding, 4 manual) | 17 | transfer between prop groups p0 and p1 |
| 7 | rider state p0 on prop group p1..p2 | 18 | edge ~p0 to object p1.. |
| 8 | grind object tag p0..p1 (p2: backwards) | 19 | edge ~p0 to prop group p1..p2 |
| 9 | grind prop group p0..p1 (p2: backwards) | 20 | record stat p0 > p1 (section 2.4; cm for stats >= 4) |
| 10 | jump over prop group p0..p1 (not from it, not onto it) | 21 | hit an object with tag p0..p1 |

[V] The 72 goals, with the game's own text (text table 2, id `0x2089 + (goal index) * 3`, three lines) and the decoded objectives; the texts and the parameters agree everywhere (e.g. "pull a 540" = trick 0xFF with 3 half spins, "grind more than 180 feet" = stat 6 > 5,486 cm):

| Level | Tier | # | Challenge (game text) | Objectives: type(params) |
|---|---|---:|---|---|
| 0 Greenville Park | amateur | 0 | jump over the first dirt mound | 13 transfer(1100,1110,0) |
| 0 Greenville Park | amateur | 1 | jump over the log in the road (without touching it!) | 13 transfer(1080,1090,0) |
| 0 Greenville Park | amateur | 2 | hit the four electric boxes | 12 touch prop group(1,1,0); 12 touch prop group(2,2,0); 12 touch prop group(3,3,0); 12 touch prop group(4,4,0) |
| 0 Greenville Park | amateur | 3 | score over 5,000 points in a run | 20 record(1,5000,0) |
| 0 Greenville Park | pro | 0 | jump the creek gap | 13 transfer(1000,1010,0) |
| 0 Greenville Park | pro | 1 | jump over the tree trunk | 10 jump over prop group(30,30,0) |
| 0 Greenville Park | pro | 2 | use the trailer to jump over the fence | 13 transfer(1020,1030,0) |
| 0 Greenville Park | pro | 3 | score over 15,000 points in a run | 20 record(1,15000,0) |
| 0 Greenville Park | hardcore | 0 | jump over the flatbed car of the freight train | 10 jump over prop group(22,22,0) |
| 0 Greenville Park | hardcore | 1 | grind the phone line | 9 grind prop group(29,29,0) |
| 0 Greenville Park | hardcore | 2 | jump up to the roof of the house | 13 transfer(1040,1050,0) |
| 0 Greenville Park | hardcore | 3 | grind more than 180 feet on the power lines | 20 record(6,5486,0) |
| 1 Vail Trails | amateur | 0 | pull a 360 over the ramp park's box jump | 1 trick off edge(255,2,1010) |
| 1 Vail Trails | amateur | 1 | grind more than 50 feet anywhere | 20 record(6,1523,0) |
| 1 Vail Trails | amateur | 2 | grind the four wooden crates | 9 grind prop group(5,5,0); 9 grind prop group(6,6,0); 9 grind prop group(7,7,0); 9 grind prop group(8,8,0) |
| 1 Vail Trails | amateur | 3 | score over 10,000 points in a run | 20 record(1,10000,0) |
| 1 Vail Trails | pro | 0 | transfer over the fence (jump from ramp to ramp) | 13 transfer(3,16,0) |
| 1 Vail Trails | pro | 1 | transfer over the cargo box | 13 transfer(1020,1030,0) |
| 1 Vail Trails | pro | 2 | grind the tops of the four warehouse doors to open them | 9 grind prop group(31,31,0); 9 grind prop group(30,30,0); 9 grind prop group(28,28,0); 9 grind prop group(29,29,0) |
| 1 Vail Trails | pro | 3 | score over 25,000 points in a run | 20 record(1,25000,0) |
| 1 Vail Trails | hardcore | 0 | grind the warehouse roof | 8 grind object(1040,1040,0) |
| 1 Vail Trails | hardcore | 1 | jump between warehouse roofs | 13 transfer(1050,1060,0) |
| 1 Vail Trails | hardcore | 2 | grind each of the four sections of the rooftop cable | 9 grind prop group(1,1,0); 9 grind prop group(2,2,0); 9 grind prop group(3,3,0); 9 grind prop group(4,4,0) |
| 1 Vail Trails | hardcore | 3 | jump through four warehouse windows | 21 hit object(1000,1000,0); 21 hit object(1002,1002,0); 21 hit object(1004,1004,0); 21 hit object(1006,1006,0) |
| 2 Backyard Pools | amateur | 0 | knock over the four long blue deck chairs | 12 touch prop group(1,1,0); 12 touch prop group(2,2,0); 12 touch prop group(3,3,0); 12 touch prop group(4,4,0) |
| 2 Backyard Pools | amateur | 1 | clear 30 feet of air (use the height meter) | 20 record(4,914,0) |
| 2 Backyard Pools | amateur | 2 | pull a 540 | 0 trick(255,3,0) |
| 2 Backyard Pools | amateur | 3 | score over 15,000 points in a run | 20 record(1,15000,0) |
| 2 Backyard Pools | pro | 0 | transfer from pool to pool | 13 transfer(1050,1060,0) |
| 2 Backyard Pools | pro | 1 | grind the green lawnmower | 9 grind prop group(21,21,0) |
| 2 Backyard Pools | pro | 2 | transfer from the pool to the roof | 13 transfer(1080,1090,0) |
| 2 Backyard Pools | pro | 3 | score over 35,000 points in a run | 20 record(1,35000,0) |
| 2 Backyard Pools | hardcore | 0 | grind the clothesline | 9 grind prop group(31,31,0) |
| 2 Backyard Pools | hardcore | 1 | get into the secret pool | 6 state on edge (grind)(0,1070,1070) |
| 2 Backyard Pools | hardcore | 2 | grind the red lawnmower | 9 grind prop group(20,20,0) |
| 2 Backyard Pools | hardcore | 3 | transfer from one rail to the other between pools | 17 transfer prop groups(30,29,0) |
| 3 Lot 8 | amateur | 0 | clear each jump in the six-pack | 13 transfer(1000,1010,0); 13 transfer(1020,1030,0); 13 transfer(1040,1050,0) |
| 3 Lot 8 | amateur | 1 | jump the muddy creek | 13 transfer(1060,1070,0) |
| 3 Lot 8 | amateur | 2 | break off four cabin doors | 12 touch prop group(1,1,0); 12 touch prop group(2,2,0); 12 touch prop group(3,3,0); 12 touch prop group(4,4,0) |
| 3 Lot 8 | amateur | 3 | score over 20,000 points in a run | 20 record(1,20000,0) |
| 3 Lot 8 | pro | 0 | pull a backflip over each jump in the six-pack | 14 transfer with trick(1000,1010,3); 14 transfer with trick(1020,1030,3); 14 transfer with trick(1040,1050,3) |
| 3 Lot 8 | pro | 1 | fastplant over the woodward sign | 1 trick off edge(42,0,1100) |
| 3 Lot 8 | pro | 2 | jump the four golf carts | 10 jump over prop group(5,5,0); 10 jump over prop group(6,6,0); 10 jump over prop group(7,7,0); 10 jump over prop group(8,8,0) |
| 3 Lot 8 | pro | 3 | score over 40,000 points in a run | 20 record(1,40000,0) |
| 3 Lot 8 | hardcore | 0 | pull a 360 over each jump in the six-pack | 15 transfer with spin(1000,1010,2); 15 transfer with spin(1020,1030,2); 15 transfer with spin(1040,1050,2) |
| 3 Lot 8 | hardcore | 1 | jump up to the warehouse roof | 13 transfer(1110,1120,0) |
| 3 Lot 8 | hardcore | 2 | grind four wires between the cabins | 9 grind prop group(28,28,0); 9 grind prop group(29,29,0); 9 grind prop group(30,30,0); 9 grind prop group(31,31,0) |
| 3 Lot 8 | hardcore | 3 | grind two mini- bulldozers to open the tent bowl area | 9 grind prop group(20,20,0); 9 grind prop group(21,21,0) |
| 4 Woodward Trails | amateur | 0 | transfer across the fun-box | 13 transfer(1000,1010,0) |
| 4 Woodward Trails | amateur | 1 | do a flair (180 backflip) in the bowl | 0 trick(3,1,0) |
| 4 Woodward Trails | amateur | 2 | grind four fences around Lot 8 | 9 grind prop group(30,30,0); 9 grind prop group(31,31,0); 9 grind prop group(24,24,0); 9 grind prop group(25,25,0) |
| 4 Woodward Trails | amateur | 3 | score over 25,000 points in a run | 20 record(1,25000,0) |
| 4 Woodward Trails | pro | 0 | grind the tower above the bowl | 8 grind object(1000,1003,0) |
| 4 Woodward Trails | pro | 1 | transfer between the two corner quarterpipes | 13 transfer(31,49,0) |
| 4 Woodward Trails | pro | 2 | walltap all four fire alarms | 12 touch prop group(10,10,0); 12 touch prop group(11,11,0); 12 touch prop group(12,12,0); 12 touch prop group(13,13,0) |
| 4 Woodward Trails | pro | 3 | score over 45,000 points in a run | 20 record(1,45000,0) |
| 4 Woodward Trails | hardcore | 0 | backflip off the fun-box rail | 2 trick over prop group(3,0,28) |
| 4 Woodward Trails | hardcore | 1 | do a frontflip over the Mt. Everest fence | 11 jump over prop group with trick(29,29,10) |
| 4 Woodward Trails | hardcore | 2 | transfer up to the bridge guardrail | 19 transfer edge to prop group(1500,27,27) |
| 4 Woodward Trails | hardcore | 3 | manual the tower | 6 state on edge (grind)(4,1020,1020) |
| 5 Titanic | amateur | 0 | transfer across the Titanic channel | 13 transfer(32,1040,0) |
| 5 Titanic | amateur | 1 | clear 35 feet of air | 20 record(4,1066,0) |
| 5 Titanic | amateur | 2 | jump over the Titanic channel gap | 13 transfer(1050,1060,0) |
| 5 Titanic | amateur | 3 | score over 30,000 points in a run | 20 record(1,30000,0) |
| 5 Titanic | pro | 0 | jump over the mini channel gap | 13 transfer(1070,1080,0) |
| 5 Titanic | pro | 1 | grind both blue lifts to open THE CAGE! | 9 grind prop group(20,23,0); 9 grind prop group(24,27,0) |
| 5 Titanic | pro | 2 | pull a 900 | 0 trick(255,5,0) |
| 5 Titanic | pro | 3 | score over 50,000 points in a run | 20 record(1,50000,0) |
| 5 Titanic | hardcore | 0 | jump over the drop-in fence into The Cage | 10 jump over prop group(31,31,0) |
| 5 Titanic | hardcore | 1 | grind the Woodward sign between the two stone towers | 9 grind prop group(30,30,0) |
| 5 Titanic | hardcore | 2 | pull a double backflip | 0 trick(8,0,0) |
| 5 Titanic | hardcore | 3 | grind transfer from the sign to the rail below | 17 transfer prop groups(29,28,0) |

Competition levels 6..11 have no goals: two judged runs (`gameJudgeRun` 0x004105D0: five judges from the run score against the level's par score 40,000 / 50,000 / 60,000 / 45,000 / 55,000 / 70,000, trick variety, areas of the level used and the trick counts; the other riders get random scores) and the placing 1st/2nd/3rd is stored as `comp.<L>` = 3 / 2 / 1 [V].

### 7.2 Rewards (`g_rewards` 0x005568B8, `GameRewardTables`)

[V] Pairs `{u8 type, u8 param}`: `levelTier[12][3][2]` (+0x00), `parkTier[4][3]` (+0x90), `final[4]` (+0xA8), `riderFinal[12][2]` (+0xB0). For a competition level the "tier" is the placing: 3rd = 0, 2nd = 1, 1st = 2. Types (`gameApplyReward` 0x00411090, jump table 0x004111E4; unlocks only raise): 1 bike (`bikes_unlocked = max(.., p+1)`, notice 0x40), 2 style (0x80), 3 rider (`riders_unlocked`), 4 level (`levels_unlocked`, notice 0x100; param 1 also switches the tutorial hints off), 8 movie (record flag bit 0), 9 per rider (`riderFinal[rider][p]`), 10 cheat bit p. Types 5, 6 and 7 change nothing in the save: 5 (params 0..7) sits on Greenville pro, Woodward amateur and the competitions' 3rd place, 6 (param = level 0..11) on every level's hardcore tier / 2nd place and on Slim Jim Guy's final reward (param 12), 7 on the Greenville and Woodward hardcore park rewards; the texts of those tiers promise "sponsorships and clothing" and "secret game features, hints". [V] (the overlay pass, `overlayAddReward` 0x004180F0, test_reward_texts) they are RESULTS-screen texts only: **5 = sponsor** ("NEW SPONSOR! You've earned a sponsorship from Fox Racing!", "NEW RESPECT!" magazine covers, the Slim Jim Guy's "SLIM JIM!" and Amish Boy's "AMISH!" lines: text 0x21D5 + 2 `g_rewardSponsor[rider][param]`, table 0x00556998, 12 x 8), **6 = a gameplay tip** ("TIP! To get big air press the Jump button ...", 0x2243 + 2 param; param 12 = the Slim Jim sweepstakes, 13 = a DMFB2 code text), **7 = the rider's signature trick** ("TRICKTIP! Mirra-only special trick: right, up right, right + Trick button!", 0x225F + 2 (2 rider + param)). docs/re/hud.md section 4.

[V] `gameAwardLevelProgress` 0x00410C60 at level end (proquest only): for the tier completed in this run (challenge levels: `tier_goals_done == 4` and a goal with 0x40; competitions: every placing above the stored one):
1. `levelTier[level][tier]` (2 rewards);
2. if all 3 levels of the park (`level / 3`: Greenville, Woodward, Pro Tour, Acclaim Max Games) are done at that tier: `parkTier[park][tier]`; at hardcore also record flag `4 << park`;
3. if all 12 levels are done at hardcore: record flag 2 and `final[0]` (= type 9 -> the rider's own reward: the movie for Dave Mirra, Ryan Nyquist and Amish Boy, a cheat for riders 2..9); then `final[1]` (rider 12 = **Amish Boy**) once riders 0..9 all have flag 2, or `final[2]` for rider 11.

What the tables unlock, per tier (P = park reward; types 5-7 left out):

| | amateur | pro | hardcore |
|---|---|---|---|
| Greenville (0-2) | levels 2, 3; style 2 | bike 2; P: level 4 | - |
| Woodward (3-5) | style 3 | levels 5, 6, 7; bike 3 | - |
| Pro Tour (6-8: 3rd/2nd/1st) | style 4 | - | levels 8, 9, 10; P: bike 4 |
| Acclaim Max Games (9-11) | style 5 | - | levels 11, 12; Slim Jim Guy (rider 11); P: bike 5 (secret bike) |

[V] Completing everything with every rider gives exactly what `unlock-all` (the game's cheats) unlocks: 12 riders, all 8 cheats, 12 levels / 5 styles / 5 bikes per rider, the three movies (test `test_complete_all_gives_exactly_what_unlock_all_unlocks`). Replaying the rewards for the progress stored in the real save reproduces its unlocks (exactly for the riders played without cheats: 3, 6 and 9).

### 7.2a Trick ids in the objectives

The trick parameters of the objectives (types 0, 1, 2, 3, 11, 14) are the trick ids the rider code
selects from the stick: e.g. "pull a double backflip" (trick 8) = down, down + trick button; "do a
frontflip" (10) = up, up; "fastplant over the woodward sign" (42) = left + plant button; a "flair"
(trick 3, backflip, with one half spin) = down + trick button while spinning. The full selection rules
and tables are in ANIM.md section 8.2 (`dmbmx.formats.tricks.select_air_trick`, [V] rider.c).

### 7.3 `dmbmx save complete-all`

`complete_all()` marks, for every rider, every goal of every challenge level done (tier 2, `tier_goals_done` 4, goal bytes `0x80 | objectives`, objectives state 5) and 1st place in every competition, tier by tier, and applies the rewards exactly as `gameAwardLevelProgress` would (section 7.2). It only raises: done goals, higher tiers and better placings are kept; scores, names, profiles, options and the "trick landed" bits are not touched. In-game confirmation: the manual in-game check `complete-all` (`tools/testing/playtest/make_playtest_complete_all.py`); until then "the game shows everything complete" is [I].

# HUD and in-game overlays (overlay.c, overlymp.c, bmx/zoverii.c)

C: `decomp/src/zaxis/Bmx/Src/com/action/overlay.c` (122 functions), `overlymp.c` (11),
`decomp/src/zaxis/Bmx/Src/com/zge/bmx/zoverii.c` (18); types `decomp/include/overlay.h`, `zoverii.h`;
checks `tests/test_decomp_overlay.py`. [V] = checked against the exe or the data by that test, [I] = inferred.

85 of the overlay.c functions and 5 of overlymp.c were not functions in the Ghidra project: they are
reached only through the two function tables below (and two ILT thunks). `decomp/symbols/symbols.csv` creates them.

## 1. Layers

| Layer | What | Notes |
|---|---|---|
| `zoverii.c` | PSX-style primitive pools [V] | 4 pools (HUD shapes 16, HUD glyphs 128, menu shapes 28, menu glyphs 400), each double buffered, 0xA0 bytes per slot (`ZoverPrim`: a POLY_F4 / POLY_G4 / FT4-with-16-bit-UV body, `+0x9C` type, `+0x9D` flags, `+0x9E` next glyph). Handle = index \| class << 12 \| layer << 14. `zoveriiFlush` (once per frame, from `overlayDraw`) links the active layer's pools into the NgIFpsx ordering table, swaps buffers and copies every changed ("dirty") slot into the other buffer. Text = one textured quad per glyph, chained. Immediate draws (`zoveriiDrawRect/DrawText`) go straight to NgIFpsx. |
| colours | `0x0BRRGGBB` [V] | B = semi-transparency mode 1..7 (0 opaque); the text call's last argument is a palette (CLUT) offset: the HUD uses palettes 2..6 of the small font for coloured text (trick names 5, hints 5, goals 6, messages 4). |
| fonts | miscdata.zal 0x7531 small (82 chars, h 32), 0x7532 tiny (144, h 24, with accents), 0x7534 button glyphs (15: `wedcxzaqXT1SC2 `), 0x7535 icons (11: `LOCBWMTcpPR`) [V] | File: u8 count, u8 height at +4, then 13-byte records from +0xC (u8 char, u16 u at +1, v at +5, w at +9); the images are NgIFzres resources 0x30/0x32/0x33/0x34 = Material.ZAL 9/11/12/13 (`tools/dmbmx/formats/hudfont.py`, `dmbmx gui hud-export/hud-import`; ZAL.md 3.1). |
| texts | STRS 0x7D01 + language (ids 0x1000..), plus 0x7D05 + language (0x2000.., single player) or 0x7D09 + language (0x3000.., multiplayer) [V] | Loaded by `overlayLoadStrings`; a 0x2xxx id resolves only in single player, 0x3xxx only with two players. |

## 2. HUD items (`g_overlayItems[17]`, 0x00556F00: flags, init, draw, show, set, get) [V table, I names]

| # | Item | Data (`overlaySetItem`) | Shows |
|---:|---|---|---|
| 0 | letterbox | - | two black 640 x 60 bars (replays, demo) |
| 1 | score | `OverlayScoreReq` | run score at (0x24B, 400), rolls up by gap / 10 + 10 per frame; distance in the stat games |
| 2 | timer | u32 frames | `m:ss` (colon blinks, red under 5 s) |
| 3 | input | - | the stick directions entered so far, button-font arrows |
| 4 | tricks | - | the trick set: names slide up (cosine, 45 frames), "TO" between tricks, the set score below |
| 5 | meter | `OverlayMeterReq` | height / distance meter in feet, 20 ft window, the all-time best of `stat` as a red line |
| 6 | balance | float | grind / manual balance bar (marker +-80 px) |
| 7 | crash | u8 | bail reason, text 0x1056 + id ("look where you're going" ... "you died") |
| 8 | (unused) | u8 | stores the byte only |
| 9 | replay icon | u16 | icon font "R" record / "P" play / "p" pause, blinking |
| 10 | hint | `OverlayTimedText` | tutorial hint id 0..21: texts 0x114A + 3 id; capital letters A..L are buttons (`g_hudButtonGlyph` "adwxXSTC2143"), at most 4 |
| 11 | goal | `OverlayTimedText` | the goal's 3 text lines (0x2089 + 3 (12 level + id)) for 120 frames |
| 12 | message | `OverlayMessage` | text 0x1063 + id ("NEW HIGH SCORE!", "<n>-TRICK COMBO!", "LONGEST GRIND: 45' 3\"" ...) |
| 13 | track | `OverlayTimedText` | the soundtrack song (0x102E + 4 track: artist, title, ...) for 360 frames |
| 14 | logo | - | image resource 0x39 as a sprite at (0x1B1, 0x148) |
| 15 | demo | - | "DEMO PLAY" |
| 16 | fade | `OverlayFadeReq` | full-screen fade in / out |

## 3. Screens (`g_overlayScreens[26]`, 0x005571B8; `OverlayScreen` = enter, update, draw, count, sel) [V table, I names]

1 PAUSE, 2 OPTIONS (camera, tutorial hints, audio), 3 AUDIO (SFX / music volume 0..18, music track 0..9),
4/5/6 TRICK LIST (back to pause / judges / run over), 7 yes/no ("EXIT?" / "END RUN?"; returns to
`g_overlayConfirmReturn`), 8 STANDINGS (competitions), 9 judges' scores ("FIRST RUN"), 10 RUN FINISHED,
11 SAVE (the PSX memory-card dialogue, unreachable on PC: "save game" is never put in the run-over menu),
12 level / mode intro, 13/14/15 CHALLENGES (run start / pause / level end), 16 RESULTS (rewards),
17 name entry, 18-21 RECORDS (from pause / after the name entry / judges / run over), and in
overlymp.c 22 SELECT GAME, 23 B-M-X word, 24 "READY!" / player n, 25 "TIME UP" / winner. 26 = reopen the
last screen. A screen pauses the game (`actionSetMode(1)`); input is ignored for `g_overlayInputDelay`
frames (15 on most result screens).

## 4. Answers

**Trick names** [V code, ANIM.md 7.1]. `overlayTrickName` (0x0041EAB0) builds the HUD name of one
`BmxTrickRec`: spin ("180".."1260", "extreme", or "half-cab" with flag 4), "opposite" (flag 1),
"alley-oop" (flag 2), stacked-modifier word ("double", "triple", "quad", "sick" for 2..5+), the
modifier (`g_trickNames[46 + mod]`), the trick (`g_trickNames[trick]`), or " spin" for a bare spin;
then named combinations replace it (backflip + 180 = "flair", front flip + 360 = "flail", backflip +
barspin = "flip driver" / "limo driver" / ..., a spun barspin = "truck driver", superman + seat grab,
can-can + one footer = "no footer can-can"). The HUD shows it with `full = 1` (upper case when the trick
has a modifier); the B-M-X target list uses `full = 0`. The TRICK LIST screen shows the plain
`g_trickNames` entries and their input as button-font glyphs (texts 0x10BB + trick, 0x10E9 + modifier).

**Reward types 5/6/7** [V, test_reward_texts]: they only show a text on the RESULTS screen
(`overlayAddReward`): 5 = a sponsor / magazine / Slim Jim / Amish text, per rider
(`g_rewardSponsor[rider][param]`, 0x00556998: text 0x21D5 + 2 n, e.g. Dave Mirra: Fox Racing, Adidas, ...);
6 = a gameplay "TIP!" (0x2243 + 2 param; param 12 = the Slim Jim sweepstakes, 13 = the DMFB2 code text);
7 = the rider's signature trick input ("TRICKTIP! Mirra-only special trick: ...", 0x225F + 2 (2 rider + param)),
given by the Greenville and Woodward hardcore park rewards.

**Scoreboards and PLYR** [V]: the RECORDS screen prints, per stat, the rider's full name from the PLYR
table (`zresGetPlayer(rider * 5)` + 8, i.e. the style-0 entry of SaveRecordEntry.rider) next to the
typed name (SaveRecordEntry.name); the competition STANDINGS print the PLYR name of each of the ten
competitors (`g_compRanking`; riders 10/11 take rider 5's place).

**Where beaten records reach SAVE.DAT** [V]: a run that beats an all-time record of the mode (live slot
0, flag bit 0) opens the name entry (screen 17, from `gameSetState`). Its name starts as the profile
name, takes up to **10** characters (' ', A-Z, a-z), and on confirm every beaten record of stats 0..9 is
copied with `comdataGetRecordPtr` into block B: value, rider (player options +4), b5 / b6 (player
options +6 / +7), name; the name also becomes the profile name. SAVE.DAT itself is written
(`memcard_WriteSave`) automatically at every level end: by `gameSetState(LEVEL_END)` when nothing else
is pending, else after the RECORDS screen (19), the RESULTS screen (16), the competition STANDINGS
after the last run (8), or the level-end CHALLENGES list (15); "SAVING GAME DATA..." shows for 60
frames on the RUN FINISHED screen that follows.

## 5. Odds and ends

- L1 + Start on RUN FINISHED writes the run's replay to `s:\bmx\assets\PC\replay\demoLLRR.rpy`
  (level, rider): the developers' tool for the demo replays in replay.zal [V string, I purpose].
- `zoveriiMoveText` would move every glyph of a chain to the same x and give it zero height; nothing
  calls it [I].
- `zoveriiShow` writes a glyph's semi-transparency byte to +0x2C (v3) instead of +0x38 (a bug [V]).
- The button font has no glyphs for "3" / "4" (L2 / R2); no hint uses them [V].
- 0x0040BFD0 (`gameSetWord`, "horse word too long") is a game.c function Ghidra had missed; the dyn.c pass created it.

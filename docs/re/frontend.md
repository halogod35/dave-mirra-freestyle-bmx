# The front end: menus and GUI (frontend.c, gui/*.c)

DM-056. C: `decomp/src/zaxis/Bmx/Src/com/menu/frontend.c` (136 functions),
`decomp/src/zaxis/Bmx/Src/com/menu/gui/guiinput.c` (20), `guimain.c` (86), `guisfile.c`* (1),
`guitoggl.c` (13 + the generic element handler), `decomp/src/_unassigned/gap_rider__guiinput.c` (15);
types `decomp/include/gui.h`; screen file format `docs/formats/ZAL.md` 3.2 (`tools/dmbmx/formats/scrn.py`);
checks `tests/test_decomp_dm056.py`, `tests/test_scrn_*.py`. [V] = checked against the exe or the data by
those tests, [I] = inferred.

95 of the 273 functions were not functions in the Ghidra project: they are reached only through
/INCREMENTAL thunks (screen handlers, per-frame tasks, element handlers, function pointers).
`decomp/symbols/symbols.csv` rows of DM-056 create them (ApplySymbols). 0x00439FA1 is the opposite: a bogus
function Ghidra made at the "draw" case of the generic element handler 0x00439F80 (kept as
`guiElementDraw`; see guitoggl.c).

## 1. Layers

| Layer | File | What |
|---|---|---|
| input | `guiinput.c` | the menu pad: pad 0 only (asserts otherwise), straight from the zinput device (no profile remapping). Up / down / left / right auto-repeat after 20 frames every 5th; `frontendUpdateApp` adds a second repeat (every 3 frames, every frame while L1 / R1 is held) [V] |
| elements | `guimain.c` | 100 `GuiElement`s (0xF0 bytes, `g_guiElements` 0x0056D2C0) in a draw list sorted by priority; screen loading, slide-in, message plumbing, per-frame update; line (type 7) and box (8) elements |
| screen file | `guisfile.c`* | `guiLoadScreenFile`: SCRN header -> style (backdrop, focus / blur font and colour), one element per EDEF |
| element types | gap before guiinput.c, `guitoggl.c` | button (type 2: focus moves along `link[]`), the generic sprite + text element (types 1 / 5: `guiElementHandler`, text markup `!S` / `!B` / `!T`), toggle (type 3: "a\|b\|c" options cycled with left / right), null (6); the help-system stubs |
| screens | `frontend.c` | one handler per screen and page; animation through one per-frame task pointer `g_feTask` (non-NULL = busy, input ignored) |

[V] Every EDEF of the 34 shipped screens is type 1: buttons, toggles, lines and boxes exist only as
code. The frontend gives elements behaviour by setting `elem->handler` (tried before the type
handler) and by keeping all state itself.

### 1.1 Messages (`guiSendMessage(msg, elem, a, b)`)

| Id | Name | Sent by |
|---|---|---|
| 0x0A / 0x0D | create / destroy | `guiCreateElement` / `guiDestroyElement` |
| 0x0B / 0x0C | update / draw (a = draw as selected) | the default screen handler on 0x16 / 0x15 |
| 0x0E / 0x0F | focus / blur | `guiFocus`, the button handler |
| 0x10 | text changed (a = text) | `guiSetText`, `guiToggleSelect` |
| 0x14 / 0x17 / 0x18 | screen loaded / unloaded / slide-in finished | guimain.c |
| 0x15 / 0x16 | draw / frame (broadcast every frame: 0x16 then 0x15) | `guiUpdate` |
| 0x1E-0x21 | up, down, left, right (auto-repeat) | `guiUpdate` from pad 0 |
| 0x22 | accept: Cross (key D) or Enter | |
| 0x23 | Circle (key F) | |
| 0x24 / 0x25 | Start (Space) / back: Triangle (E) or Esc | |
| 0x26 | Square (S), help mode only | |
| 0x27-0x2A | L1, R1, L2, R2 | |
| 0x2B-0x37 | the same + 13: the PSX second pad. [V] Never sent on PC; every handler still accepts both sets | |
| 0x32 | = pad-2 back; sent every frame while `g_guiAutoBack` (0x005AB474) is set, which unwinds the menus [I] | |
| 0x3E / 0x3F | left / right held alone (every frame) | |

Input messages go first to the screen handler with the focused element (`guiPostInput` ->
`guiBroadcast`), then to the element, then once to the screen handler with no element. Keys are those
of the default keyboard layout; the keyboard layout chosen on the CONTROLLER screen applies to pad 0
in the game, not to the menus (`docs/re/input-camera.md` 2.1).

## 2. Menu flow (PC) [V]

```
NgApp -> frontendInitApp
  g_gameNextMode 1..3 (back from a game) -> SELECT screen on the bike / style / level page
  else -> MAIN MENU (frontendMainMenuEnter(0): loads SAVE.DAT, sets the volumes)
MAIN MENU 0xA005: PROQUEST | SESSION | FREERIDE | COOL STUFF | OPTIONS | QUIT   (7 rows scroll, wrap)
  PROQUEST / SESSION / FREERIDE -> g_numPlayers = 1, g_selectedSubMode = 0 / 1 / 2
     -> SELECT 0xA009: RIDER -> BIKE -> STYLE -> LEVEL -> start (frontendRequestExit(0): the game)
        back on the first page -> MAIN MENU
  COOL STUFF 0xA00D: CHALLENGES (goals of each challenge level and tier) | HIGH SCORES (records of
     stats 2..9 per level, the best of all levels highlighted) | MOVIES (INTRO MOVIE + unlocked rider
     movies) | CREDITS (movie)                                   back -> MAIN MENU
  OPTIONS 0xA00C: CONTROLLER (rebind 11 buttons, keyboard layout 1..5) | CHEATS (only when at least
     one cheat is unlocked) | AUDIO (MUSIC / SOUND FX 0..18)     back -> writes SAVE.DAT -> MAIN MENU
  QUIT -> zrwZglConfirmQuit (confirmation) -> frontendRequestExit(1): the program ends
  30 s without input on the main menu -> demo (g_numPlayers = 0: action.c plays a demo replay)
```

The select screen runs its pages once per player (with two players rider / bike / style alternate
between the players before the shared level). [V] Nothing on PC sets two players or game type 3 from
the menus (the multiplayer games start from `overlymp.c`'s SELECT GAME screen).

Unreachable on PC [V: no code or data reference to their entry functions]: the "PRESS START BUTTON"
title (`frontendTitleEnter`), the memory-card autoload (`frontendAutoloadEnter`; `g_feAutoload`
0x0056B618 is only ever cleared), the LANGUAGE screen 0xA014 (`frontendLanguageEnter`), the LOAD/SAVE
memory-card screen 0xA012 and its dialogue (`frontendLoadSaveEnter`, `frontendCard*`), COLOR ADJUST
(`frontendColorAdjustEnter`; text 0x134 is not an option), the vertical toggle and the help system.
The language is the save's `language` byte (0 US English, 1 English, 2 French, 3 German: STRS
0x7919 + language) [V]; the unused language screen offers ENGLISH / DEUTSCH / FRANCAIS but maps them to
1 / 2 / 3, so DEUTSCH would load the French texts and FRANCAIS the German ones [V code, I effect].

Movies: `ngappPlayMovie` plays the first entry of a playlist, with the ".str" of its path replaced by
"AVI" (`bin\movie\Intro.AVI`, ...). The exe names seven movies: ACCLAIM.AVI and ZAXIS.AVI (logos)
and Intro (played at start-up by `ngappInit`), Mirra / Nyquist / amish (MOVIES menu, once unlocked) and
credits (CREDITS). The user added the original `bin\movie` files (acclaim, amish, credits, intro, mirra,
nyquist, zaxis .avi; 2000-10/11) were added to the install and PRISTINE.sha256 on 2026-11-13. They were
missing from the first copy of the install, so before that date the MOVIES and CREDITS entries played
nothing. Windows file names are not case-sensitive, so `intro.avi` satisfies `INTRO.AVI`.

## 3. Unlock gating [V]

All from SAVE.DAT block A (`tools/dmbmx/formats/save_fields.py` names, SAVE.md 2.1 / 2.2); "the rider" is the
rider chosen on the RIDER page.

| Menu | Gate | save_fields |
|---|---|---|
| SELECT RIDER | riders 0 .. n - 1, wrapping (10 default, 11 + Slim Jim Guy, 12 + Amish Boy) | `riders_unlocked` |
| SELECT BIKE | bikes 0 .. n - 1 selectable; 4 are listed (the rest "* LOCKED *", grey), the 5th (secret bike) appears only when n = 5 | `rider.<r>.bikes_unlocked` |
| SELECT STYLE | styles 0 .. n - 1 (STYLE 1..5), the page opens on style n | `rider.<r>.styles_unlocked` |
| SELECT LEVEL | levels 0 .. n - 1 (12 shown, the rest locked, no wrap); with two players n = the highest of all 12 riders | `rider.<r>.levels_unlocked` |
| level ticker | challenge levels: the tier reached (amateur / pro / hardcore / all done); competitions: the medal | `rider.<r>.level.<L>.tier`, `.tier_goals_done`, `rider.<r>.comp.<L>` |
| medals on the RIDER page | 6 challenge tiers + 6 competition results of the rider | same |
| OPTIONS > CHEATS | refused (error sound) when no cheat is unlocked; lists the unlocked ones; left / right toggles ON / OFF | `cheats_unlocked` (list), `cheats_active` (the toggles) |
| COOL STUFF > MOVIES | INTRO MOVIE always; DAVE MIRRA / RYAN NYQUIST / AMISH BOY when that rider's record flag bit 0 is set (all levels completed) | `rider.<r>.movie_unlocked` (riders 0, 1, 11) |
| OPTIONS > AUDIO | MUSIC = `volume_0d` (sound channel 1), SOUND FX = `volume_0c` (channel 0), 0..18 | `volume_0d`, `volume_0c` |
| OPTIONS > CONTROLLER | rows 0..10 -> profile +0x14..+0x3F, layout -> +0x4B | `profile.<p>.dpad.*`, `.control.*`, `.keyboard_layout` |

`save_fields.py` agrees with every offset; DM-056 confirms what it had as [I]: `volume_0c` is the SFX
volume and `volume_0d` the music volume, `cheats_active` holds the CHEATS menu toggles, and the
`language` values.

## 4. Cheat codes [V]

**The CHEATS menu** (OPTIONS) is not a code entry: it lists the cheats the career has unlocked
(`cheats_unlocked`, reward type 10) and switches them on and off (`cheats_active`). Names (table
0x0055A3BC, bit = list order): FIRST PERSON CAM, NIGHT VISION MODE, SUPER CRASH MODE, EXORCIST MODE,
SUSPENSION MODE, SILLY GRUNT MODE, GHOST RIDER MODE, STICKY CRASH MODE.

**Button codes** are entered on the SELECT screen (`frontendSelectCheats` 0x00446AC0): every key message
the screen gets (up / down / circle / L1-R2 always, left / right when the carousel is idle) goes into a
16-entry ring, and a code is recognised when it appears in the ring as a run. The codes (pad 1; the
pad-2 variants are the same + 13), with the PC keys of the default layout (arrows, F = Circle):

| # | Page | Sequence | Effect |
|---|---|---|---|
| 0 | RIDER | Down, Down, Left, Right, Up, Up, Circle | `riders_unlocked` = 11 (Slim Jim Guy), when < 11 |
| 1 | RIDER | Up, Up, Up, Down, Up, Up, Up, Right, Circle | `riders_unlocked` = 12 (Amish Boy), when Slim Jim Guy is open (no sound) |
| 2 | BIKE | Up, Left, Up, Down, Up, Right, Left, Right, Circle | the rider's `bikes_unlocked` = 5 (secret bike) |
| 3 | STYLE | Left, Up, Right, Down, Left, Down, Right, Up, Left, Circle | the rider's `styles_unlocked` = 5 |
| 4 | LEVEL | Up, Up, Down, Down, Left, Right, Left, Circle | the rider's `levels_unlocked` = 12 |
| 5 | any | Down, Left, Down, Left, Down, Left, Down, Circle | nothing (its case is empty) |

The effects change the save globals in memory; they reach SAVE.DAT with the next write (leaving
OPTIONS, or a level end). Quirk: the compiled switch shares its tails, so a failing Amish Boy test
falls through to the bike / style / level tests (harmless: each also checks the page).

## 5. Texts

- The frontend text set ("macros", `frontendLoadMacros`) is miscdata.zal STRS **0x7919 + language**
  (ids 0x000-0x272) into a 16 KB buffer; menu items, the memory-card messages, rider / bike / level
  names, the ticker lines.
- HIGH SCORES loads STRS 0x7D01 + language (HUD set 0x1xxx: stat labels 0x1094 + stat), CHALLENGES
  0x7D05 + language (single-player set 0x2xxx: the goal texts 0x2089 + 3 (12 level + 4 tier + goal)).
- Modding (DM-066): `dmbmx gui text-set` changes a text in all four language tables; the menu art,
  fonts and screen layouts go through `dmbmx gui export / import` (docs/tools/menu-modding.md, ZAL.md 3.3).
  The big menu font 0x8003 has no lower-case glyphs, so labels must stay upper case.
- The select screen's info ticker: per rider (`g_feRiderBios` 0x0055AAA0), per bike, per level state;
  each line "#Y<label> #B<value>" (yellow left part, blue right part).

## 6. Odd findings

- `frontendRequestExit(int)`: 0 leaves the menus for the game (select screen done, demo), 1 quits the
  program (QUIT). DM-055 called the parameter `toGame`; it is the reverse (`quit`).
- `zprimGetFontHeight` (ZprimFont +0x02 `lineHeight`; DM-064 renamed it from `zprimGetFontFlags` / `flags`) is the font's line height (centring, line advance).
- The element draw aligns centred / right-aligned text by the width of all lines so far (it measures
  from the first line), so multi-line centred text drifts (a quirk of the original).
- 0x00479260 (`zprimCopyToPicture`, zprim.c), called by the unused `guiDrawSpriteToBackdrop`, is a bare
  `ret` on PC (DM-061 created the function).
- The main menu writes nothing; leaving OPTIONS writes SAVE.DAT (`frontendOptionsExit`), and so does the
  game at every level end (SAVE.md 2.4a).

## 7. Open questions

- Who sets `g_guiAutoBack` (0x005AB474): no instruction stores to it by address.
- `GuiPad` +0x10..+0x1C (cleared every poll) and `GuiToggleData` +0x00: meaning unknown.
- The movie descriptor fields after the size (0xD15, 0x18 for the credits: frames and rate?).
- Which object 0x00439F80 (the generic element handler) and the gap before guiinput.c really belong to.

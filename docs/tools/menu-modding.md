# Menu modding: `dmbmx gui` (menu art, screens, fonts, texts, HUD fonts)

Code: `tools/dmbmx/formats/zprim.py` (pictures, sprites, fonts), `tools/dmbmx/formats/scrn.py`
(GUI groups, SCRN screens; the front-end pass), `tools/dmbmx/formats/strs.py` (text tables), `tools/dmbmx/formats/hudfont.py`
(HUD fonts), `tools/dmbmx/formats/gui_dir.py` (export / import directories), CLI `tools/dmbmx/cli/gui.py`.
Specs: `docs/formats/ZAL.md` 3.1 (STRS, HUD fonts), 3.2 (screens), 3.3 (art and fonts).
Tests: `tests/test_zprim_corpus.py`, `tests/test_zprim_unit.py`, `tests/test_cli_gui.py`.
Play-test: `tools/testing/playtest/make_playtest_menu_edit.py`. Tags: **[V]** verified by code or tests, **[I]** inferred.

All commands read the game files and write new ones; they refuse to write inside the game install
(AD-1). Write to `out/` and copy the results into `_testgame` (or use a play-test script).

## 1. What lives where

| What you want to change | File | Command |
|---|---|---|
| Menu labels and other menu texts (PROQUEST, OPTIONS, rider names, ticker lines) | `assets/miscdata.zal`, STRS 0x7919-0x791C (one per language) | `gui texts`, `gui text-set`, `gui text-import` |
| HUD / in-game texts | `miscdata.zal` STRS 0x7D01-0x7D0C | same |
| Menu backdrops, portraits, icons, tabs (pictures 0x0xxx, sprites 0x4xxx) | `assets/Intwrap.zal` (nested archive, 35 groups) | `gui export`, `gui import` |
| Menu fonts (0x8001-0x8005): glyph shapes, metrics, colours | `Intwrap.zal` | `gui export`, `gui import` |
| Screen layouts: element positions, fonts, colours, placeholder texts | `Intwrap.zal` SCRN 0xA0xx | `gui export`, `gui import` |
| HUD fonts (score, trick names, button glyphs, replay icons) | `miscdata.zal` 0x7531-0x7535 + `assets/material.zal` 9 / 11-13 | `gui hud-export`, `gui hud-import` |

[V] Most texts on the screens are placeholders ("String to be preallocated") that the front end
replaces at run time with STRS 0x7919 + language. To rename a menu entry, change the STRS text, not the
screen. The save's `language` byte picks the table (0 US English, 1 English, 2 French, 3 German);
`text-set` changes all four unless `--table` is given.

## 2. Texts

```bash
tools/testing/py.sh -m dmbmx gui texts assets/miscdata.zal --table 0x7919            # print: 0x0024 'PROQUEST' ...
tools/testing/py.sh -m dmbmx gui text-set assets/miscdata.zal 0x24 "MOD QUEST" out/mod/miscdata.zal
tools/testing/py.sh -m dmbmx gui texts assets/miscdata.zal --json out/texts.json      # all 16 tables
#   ... edit out/texts.json ...
tools/testing/py.sh -m dmbmx gui text-import assets/miscdata.zal out/texts.json out/mod/miscdata.zal
```

- Only changed texts are written: an edited text is appended to its table and that id is pointed at it;
  everything else keeps its bytes [V].
- Text is latin-1. The big main-menu font 0x8003 has **no lower-case glyphs**: lower-case letters print
  as gaps there [V data]. Keep main-menu labels upper case.
- Limit [V]: a front-end table (0x7919-0x791C) must stay at most 0x3FFF bytes, or the game stops with
  "FRONTEND: Macro data too big or too small". The shipped ones are 11.5-12.8 KB; the tool refuses to go past
  the limit.
- Main menu ids: 0x24 PROQUEST, 0x25 SESSION, 0x26 FREERIDE, 0x29 COOL STUFF, 0x2A OPTIONS, 0x2C QUIT.

## 3. Menu art, fonts and screens (Intwrap.zal)

```bash
tools/testing/py.sh -m dmbmx gui list assets/Intwrap.zal                       # 35 groups and their assets
tools/testing/py.sh -m dmbmx gui list assets/Intwrap.zal --screen 0xA009        # one screen's elements and texts
tools/testing/py.sh -m dmbmx gui export assets/Intwrap.zal out/gui              # everything (~480 files, 3 s)
tools/testing/py.sh -m dmbmx gui export assets/Intwrap.zal out/gui --screen 0xA004   # one group only
#   ... edit PNGs / JSON in out/gui ...
tools/testing/py.sh -m dmbmx gui import assets/Intwrap.zal out/gui out/mod/Intwrap.zal
```

`--screen 0xA0nn` exports group 0xE0nn (the screen and its art); `--screen 0xA000` exports the permanent
group 0xE000 (fonts 0x8002 / 0x8003 / 0x8005 and the cursor frame 0x40C4). The export directory:

```
_gui.json            index: every asset, its type, the groups that hold it, its SHA-256
pictures/0001.png    + 0001.json   backdrops: 512x512, 8-bit palette
sprites/40B6_0.png   + 40B6.json   one PNG per frame; hot spots and the whole palette in the JSON
fonts/8003.png       + 8003.json   glyph sheet; line height, glyph metrics, the 16 colour banks
screens/A009.json                  the SCRN header and every element (fields + texts)
```

An asset in several groups (font 0x8001, ...) is exported once and imported into every group that holds
it. `import` rebuilds only the changed assets, re-compresses only the changed groups, checks that the
whole result parses again, and prints what changed. An unedited export imports back to the same bytes [V].

**Editing images.** The PNGs are palette images with the asset's bit depth (4 or 8). Keep them that
way in your editor (GIMP: Image > Mode > Indexed, "use existing palette") for full control:
- pixel edits keep the palette indices; a 4-bit image can use indices 0-15 only;
- editing a palette colour in the PNG changes it in the game (15-bit: each channel keeps its top 5 bits);
- transparent = palette entry with alpha 0 (the game's colour 0x7C1F, drawn as see-through);
- an RGB/RGBA PNG is accepted too: every pixel is mapped to the nearest colour of the existing palette
  (alpha < 128 -> the transparent entry), with a warning if colours had to be approximated.

Sizes: sprites may change size (keep the width even for 4-bit images; the frame offsets are recomputed);
element positions in the screens do not move with them. Keep backdrops 512x512 [I]. Frames cannot be added
or removed.

**Palettes in the JSON.** `palette.colours` (images) / `palette.banks` (fonts) are rows of 15-bit values
(hex, red in the low bits; `7C1F` = transparent). A value may be replaced by `#rrggbb`. The PNG only
carries the colours the game draws with (the first 16 or 256); a font's 16 banks are its text colours
(the main menu uses bank 1 of 0x8003), so recolour menu text by editing a bank row. If you change the same
entry in the PNG and in the JSON, the PNG wins.

**Fonts.** `fonts/<id>.json` lists one glyph per slot: `x, y, w, h` (the box in the sheet, bytes, so the
sheet's top-left 256x256), `xoff, yoff` (draw offset), `advance` (extra pixels after the glyph), `descent`
(extra height of the background box) and `code`. With fewer than 256 glyphs, slot k is character
`first + k` and an unused slot has `code` 0 and w = h = 0. To add a glyph, draw it in the sheet and fill its
slot. `line_height` is the line advance.

**Screens.** `screens/A0nn.json` has the header (backdrop, focus / blur font and colour) and one entry per
element: `x, y` position, `font`, `sprite`, `frame`, `text_rgb`, `flags`, ... (names as in ZAL.md 3.2) and
`text` / `toggle_text`. Numbers may be written as `"0x..."` strings. Elements cannot be added or removed;
the text block is rebuilt from the element texts.

## 4. HUD fonts

```bash
tools/testing/py.sh -m dmbmx gui hud-export assets out/hud        # small / tiny / button / icons: .json + .png
#   ... edit ...
tools/testing/py.sh -m dmbmx gui hud-import assets out/hud out/mod   # writes out/mod/miscdata.zal and/or material.zal
```

`<name>.json`: `height` (all glyphs) and one record per character: `char`, `u`, `v`, `w` (the box in the
sheet). The game advances `w + 1` per glyph. `<name>.png` is the Material.ZAL BMP as a palette PNG; a
palette PNG of the same depth keeps its indices and its palette replaces the BMP's, any other PNG is mapped
to the BMP palette. The sheet size is fixed. Magenta (255,0,255) is the transparent colour key (TEXTURE.md).

## 5. Installing and testing

Copy the new files over `_testgame/assets/` (file names are not case-sensitive) and run
`_testgame/NgBMX.exe`; restore with `tools/testing/restore_testgame.sh`. `tools/testing/playtest/make_playtest_menu_edit.py` builds a
ready-made example (MOD QUEST label + a recoloured rider portrait) under `out/playtest/menu-edit/`, including
the edited export directory `gui-edit/`.

[I] Until the manual in-game check `menu-edit` passes, that the game accepts edited art is inferred from the loaders (no checksums,
buffers sized from the archive: ZAL.md 2.4). The menu heap is 3 MB (`zmemDMMInit(NULL, 0x300000)`), so
very large new art could run out of memory [I].

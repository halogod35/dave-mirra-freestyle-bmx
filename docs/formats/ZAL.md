# ZAL: Z-Axis asset library (`*.zal`)

Code: `tools/dmbmx/formats/zal.py` (reader/writer), `tools/dmbmx/formats/zal_dir.py` (extract/pack), `tools/dmbmx/core/lzo.py` (LZO1X codec),
`tools/dmbmx/core/hash.py` (name hash), CLI `dmbmx zal list|extract|pack|replace`. Survey: `tools/research/survey_zal.py`.
Evidence tags as in CONTRIBUTING.md AD-5. VAs are for the 2000-11-22 `NgBMX.exe`.

## 1. Overview

A ZAL is a flat **group table** followed by the asset data. The engine module is
`C:\NgEngine\Zge\zasset.c` (open: `zasset_OpenLibrary` 0x004804D0). Each *group* has a 32-bit id and
`assets_per_group` assets (always 1 in the shipped files). Assets can be stored raw or **LZO1X**-compressed.
A group's payload may itself be a ZAL (Intwrap.zal group 0).

[V] All 6 shipped files (and the nested archive in Intwrap.zal) round-trip byte-for-byte through
`ZalArchive.read(...).write()` (tests/test_zal_corpus.py), and every one of the 1,349 assets (1,298 of
them LZO1X) decodes to exactly its declared size.

## 2. Grammar

```
ZAL      := Header GroupTable AssetData* Tail
Header   := u32 magic=0x2A81511C, u8 reserved[12], u16 flags, u16 count       (0x14 bytes)
GroupTable := Group[count & 0x3FFF]
Group    := u32 id, Asset[flags & 0x1FFF]
Asset    := u32 offset, u32 size [, u32 stored_size]   ; 3rd word only if compressed
AssetData := pad[0..3] bytes[stored_len]               ; at 0x10 + offset
Tail     := bytes                                      ; 0-3 bytes after the last asset
```

### 2.1 Header

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | u32 | magic | `1C 51 81 2A` = 0x2A81511C. [V] `zasset_OpenLibrary` reads 0x1C bytes and compares dword 0 with 0x2A81511C (0x0048058B); if it matches the header is 16 bytes, otherwise the loader assumes an **older 8-byte header** without magic and reads the flags/count word at offset 8. [I] The in-memory opener 0x00475200 also accepts an old header that starts with byte 0xFF (skips 8). No such file ships. |
| 0x04 | u8[12] | reserved | [V] all zero in every file; [V] not read by the loader (only the dword at 0x10 is used). Preserved verbatim. |
| 0x10 | u16 | flags | bits 0-12 = **assets per group** ("AssetsPerGroup", [V] 0x00480601 masks 0x1FFF); bit 13 = **ref mode** ([V] 0x0048062B; assets are names of external files, opened by `zasset_GetAssetSize` 0x00480850 before it returns the size; "Compression in ref mode" asserts if combined with compression); bit 14 = unknown (always 0); bit 15 = **compressed** (a). |
| 0x12 | u16 | count | bits 0-13 = **number of groups** ("NumGroups", [V] mask 0x3FFF at 0x004805F3); bit 14 = **codec select** ([V] 0x004805CF: 0 → method 1 = LZRW3-A, 1 → method 2 = LZO1X); bit 15 = **compressed** (b). |

[V] The loader computes `comp = (flags >> 15) * 2 + (count >> 15)` (0x004805B8..0x004805C7). If `comp != 0`
the asset record has 3 words (12 bytes), else 2 words (8 bytes). Group record size =
`4 + assets_per_group * (comp ? 12 : 8)`.

Values seen:

| File | flags | count | groups | compressed | codec |
|---|---|---|---|---|---|
| Intwrap.zal | 0x0001 | 0x0005 | 5 | no | - |
| Intwrap.zal group 0 (nested) | 0x8001 | 0x4023 | 35 | yes | LZO1X |
| material.zal | 0x8001 | 0x44EE | 1262 | yes | LZO1X |
| miscdata.zal | 0x0001 | 0x0015 | 21 | no | - |
| motion.zal | 0x8001 | 0x4001 | 1 | yes | LZO1X |
| replay.zal | 0x0001 | 0x0018 | 24 | no | - |
| tables.zal | 0x0001 | 0x0001 | 1 | no | - |

### 2.2 Group and asset records (table at 0x14)

| Off | Type | Name | Notes |
|---|---|---|---|
| +0 | u32 | id | Group id. [V] `zasset_FindAsset` 0x00480780 first tries `table[id]` (ids equal to their index), then a linear search on `id`; error "Failed to find asset Group=%d, AssetNum=%d". |
| +4 | u32 | offset | Data position **relative to 0x10** (the end of the magic+reserved header). [V] 0x00480963 `add ecx, 0x10` before the seek. |
| +8 | u32 | size | Uncompressed size. [V] the loader masks it with 0x00FFFFFF everywhere (0x0048085E, 0x0048094D, 0x00480B66); the top byte is always 0 in the corpus (meaning unknown, kept raw as `size_field`). |
| +12 | u32 | stored_size | Only when compressed: bytes to read from the file. [V] 0x00480958. |

[V] Asset data follows the table in table order, each start 4-byte aligned. The 0-3 alignment bytes are
not zero: in uncompressed archives they are `0xCD` (the MSVC debug-heap fill, [I] the packer ran a debug
build), in compressed archives they are leftover bytes from the packer's buffer. A trailing 0-3 bytes
follows the last asset (`CD` in miscdata.zal, `51 FE` in motion.zal, `FF FF FF` in the nested Intwrap
archive). The reader keeps all of these (`ZalAsset.pad`, `ZalArchive.tail`) so the writer reproduces
them; an edited asset with `pad=None` gets zero padding to the next 4-byte boundary.

### 2.3 Compression

[V] Codec dispatch is `zcompres_Decompress(method, ...)` 0x0049C080 ("ZCOMPRES: Unknown decompression
method"), called from `zasset_LoadAsset` 0x00480930 with method 1 or 2 from the count word's bit 14.
After decoding, the loader checks the output length against `size & 0xFFFFFF` ("ERROR - Size mismatch in
decompressed data").

| Method | Codec | Status |
|---|---|---|
| 2 | **LZO1X** (decoder inlined at 0x0049C0AC; first byte > 17 = initial literal run, `0x11 0x00 0x00` end marker) | [V] all 1,298 compressed assets decode with the standard LZO1X rules to exactly `size` bytes and consume exactly `stored_size` bytes. `dmbmx.core.lzo.decompress` (pure Python, bounds-checked). |
| 1 | **LZRW3-A** (Ross Williams 1991; the zasset pass). Decoder 0x0049BD10, encoder 0x0049B8E0 via `zcompresCompress` 0x0049C020 (accepts methods 0/1); 0x4010-byte work buffer = 4096 table pointers. Evidence: "123456789012345678" table init, 16-item control words OR 0x10000, 2-byte copy items (12-bit table index, length 3-18), 512 partitions x 8 slots with a 3-bit cycle, hash `-417*x` = 40543*x mod 2^13, 4-byte flag header (1 = stored) | [V] code; unused in the corpus. `dmbmx.core.lzrw3a` decodes it (transcribed from 0x0049BD10) and encodes it (decoder-exact table); `asset_data` / `set_data` support it. Not tested in-game. |

The game has no LZO compressor that we found ([I] the packer was an offline tool). The ZAL repacker added a
pure-Python encoder, `dmbmx.core.lzo.compress(data, level=2)`. It is a greedy LZO1X-1 style encoder over a
16-deep hash chain and emits only the standard instruction forms. [V] compress -> decompress is the
identity for every compressed payload in the corpus. Output size compared with the original packer:
material.zal +3.5%, motion.zal +4.4%, Intwrap nested archive +8.1% (level 1, the faster mode:
+8.5% / +8.6% / +22.8%). The stream bytes differ from the original. [I] The game should accept any valid
LZO1X stream, because the loader only decodes it and compares the decoded length with `size`. This is
unverified until the manual in-game check `zal-repack` passes.

### 2.4 Integrity checks

[V] ZAL files carry **no checksum**. Every header and table byte is accounted for in section 2 (reserved
bytes are never read). The loader path (`zasset_OpenLibrary` 0x004804D0, `zasset_GetAssetSize` 0x00480850,
`zasset_LoadAsset` 0x00480930) validates only:
(1) the magic, which selects the header size;
(2) the byte counts returned by its file reads ("FAILED - Could not load asset");
(3) the decoded length against `size & 0xFFFFFF` ("ERROR - Size mismatch in decompressed data").
Buffers are allocated from the table's size (0x0045B089 `malloc(zasset_GetAssetSize())` for Intwrap
group 0), so a grown asset needs no fixed buffer change. [I] The game's custom heap may still have a
total budget. For the integrity checks: no re-signing is needed for ZAL.

### 2.5 Repacking

- `ZalArchive.set_data(id, payload)` replaces one payload and re-encodes it (LZO1X if the archive is
  compressed). Other assets keep their stored bytes. Padding is reused only while the layout before an
  asset is unchanged (`ZalAsset.pad_from`); after that, data is re-aligned to 4 bytes with zero padding.
- `dmbmx.formats.zal_dir.extract_dir` / `pack_dir` round-trip through a directory (`_index.json`,
  format tag `dmbmx-zal-dir/1`, which records reserved/flags/count/tail, per-asset `size_field`, `pad`,
  `pad_from` and the payload SHA-256, plus `_stored/*.lzo` with the original streams of compressed
  archives). [V] Extract -> pack is byte-identical for all 6 files (nested Intwrap archive included).
- CLI: `dmbmx zal extract -r <zal> <dir>`, `dmbmx zal pack <dir> <out.zal>`,
  `dmbmx zal replace <zal> <id|name|outer/inner> <payload> <out.zal>` (e.g. `0/0xe009` for a nested group).

### 2.6 Headerless (in-memory) form

[V] The payload of motion.zal group 0 is a ZAL **without** the 16-byte magic/reserved header: it has
`u16 flags, u16 count` at offset 0, the group table at 4, and asset offsets relative to byte 0.
`zactorOpenMemBank` 0x00475200 (zactor.c; the ZAL reader called it `zasset_OpenMemLibrary`) accepts this form when the
magic is absent; the zactor/zprim pass: a first byte of 0xFF skips 8 more bytes.
`ZalArchive.read(data, headerless=True)` reads it, and `write()` keeps the form (`archive.headerless`).

## 3. Contents of the shipped archives

| Archive | Opened by | Groups | Payloads |
|---|---|---|---|
| **Intwrap.zal** ("interface wrapper") | string `intwrap.zal` 0x0055BE48, "Interface asset wrapper does not exist!" | ids 0-4 | [V] group 0 = a nested compressed ZAL (35 groups, ids 0xE000-0xE022): the front end's art library "intart.zal" (zprim groups of pictures, sprites, fonts and `SCRN` screens; section 3.2, the front-end pass); groups 1-4 = 263,224-byte 8-bit 512x512 BMP files (`BM`, palette offset 0x436): [V] (viewed) 1 = RenderWare logo splash, 2 = "LOADING" screen, 3 = "LOADING DEMO" screen, 4 = Acclaim/Z-Axis legal text screen. |
| **material.zal** | `Assets\Material.ZAL` in `zresOpenLibraries` 0x0046BCD0, handle `g_materialZal` 0x0067848C | ids 0-1261 (= index) | [V] every payload is a complete Windows **BMP file** (1,099 x 4-bit + 163 x 8-bit palettised; 0x96-0x1007C bytes). [V] no `MATI`/`MADF`/`TBMP`/`TPNG` chunks anywhere in it (raw or decompressed). Mapping from material id to textures/MATI records lives elsewhere. |
| **miscdata.zal** | `Assets\MiscData.ZAL`, handle `g_miscDataZal` 0x00678490 | ids 1, 0x7531-0x7535, 0x7919-0x791C, 0x7D01-0x7D0C | [V] id 1 = IFF `FORM` with `PLHD`/`PLYR` chunks (player data); 0x7531/0x7532/0x7534/0x7535 = the overlay fonts (3.1, the overlay pass); 0x7919-0x791C and 0x7D01-0x7D0C = `STRS` **string tables**, in sets of four language variants (two English, one French, one German; the order differs per set), e.g. 0x7919-0x791C pro-challenge texts, 0x7D01-0x7D04 park names, 0x7D05-0x7D08 level descriptions, 0x7D09-0x7D0C game-mode descriptions. [V] Ids are code constants: e.g. 0x0044DAFC calls `zasset_FindAsset(g_miscDataZal, 0x7919 + n, 0)`. **Note on the MATI question:** [V] the 6 `MATI` byte matches in miscdata.zal are inside words of the localised strings ("AUTO**MATI**QUE", "FOR**MATI**ERT", "FOR**MATI**EREN", ...), not chunk tags; this archive contains no material chunks. |
| **motion.zal** | `Assets\MOTION.ZAL` 0x00551ABC (ZARTVIEW "Failed to open motions"), Scene.Txt ANIM "MainBank" | id 0 | [V] one LZO1X group, 202,116 bytes decoded: the motion bank (starts `01 00 EE 00`, then a table of motion records). Format is the first motion reader. |
| **replay.zal** | `Assets\Replay.ZAL`, handle `g_replayZal` 0x0068FFA8; "replay data not found in asset library" | ids 0-23 | [V] replay records (sizes 0x103C, 0x183C, 0x1828, 0x2702), asset id = rider + 2 x level; decoded by the prop/replay/fx pass: docs/formats/REPLAY.md, `dmbmx.formats.replay` (round trip of all 24). |
| **tables.zal** | `Assets\Tables.ZAL` | id 0 | [V] IFF `FORM{ ZMHD(8), ZMAP(0x960) }`: level table with level names and per-level ZAL file names ("ACCLAIM DIRT" / "ACCLDIRT.ZAL", "PROTOUR DIRT" / "PROTDIRT.ZAL", ...). |

[V] The exe also names `system.zal`, `WSsnd.zal`, `geometry.zal`, `intart.zal` and per-level `*.ZAL`
files (from tables.zal) that are **not** shipped with this PC build.

### 3.1 Payload formats decoded by the 3ds Max importer (details: [SPEC.md](SPEC.md) section 7)

- [V] **miscdata.zal id 1 = player table** `FORM{ PLHD(8), PLYR(60 x 24) }`. FORM here has no form-type
  word; its children are plain 4CC chunks with little-endian sizes.
  - PLHD = `u16 count (60), u16 entrySize (24), u16 0, u16 6`. The loader `zresLoadPlayerTable` asserts
    "Out of sync player data" unless PLHD is 8 bytes and `entrySize == 0x18`.
  - PLYR entry = `u16 refOffset, u8 refLength, u8 refFlags (0x80), u32 id, char name[16]`.
  - **Entry index = rider x 5 + style.** `zresGetPlayer(index)` asserts "Invalid player" when
    `index >= count`. The scoreboard overlay prints the name of entry `rider x 5`.
  - [V] `refOffset[k+1] = refOffset[k] + refLength[k]` for all distinct entries, and the ids are
    consecutive from 0x2C. [I] Both point into a resource that is not shipped, and the PC code never
    reads them (only `name`).
- [V] **tables.zal id 0 = level table** `FORM{ ZMHD{u32 count = 24, u32 recordSize = 100}, ZMAP(24 x 100) }`.
  - Record: `u16[4], u32[13]` (values such as 1000/10000/20000, [I] score/goal thresholds), then
    `char name[20]` at 0x3C and `char zal[20]` at 0x50.
  - Read by level number in `zresGetMap`.
- [V] **0x7531 / 0x7532 / 0x7534 / 0x7535 = the four overlay fonts** (the overlay pass, `zoveriiLoadFont` 0x00459FC0;
  reader / writer `tools/dmbmx/formats/hudfont.py`, the menu-modding work):
  ```
  HudFont := u8 count, u8 pad[3], u8 height, u8 pad[7], Record[count]
  Record  := u8 char, u16 u, u16 0, u16 v, u16 0, u16 w, u16 0          (13 bytes)
  ```
  The game builds a table indexed by `char - min(char)` and draws glyph `w` x `height` at (u, v), advancing
  `w + 1`. The images are Material.ZAL BMPs through `g_zresImages` 0x0055D978 (resource 0x30 + n =
  Material.ZAL asset 9 + n):

  | Font | Name | Glyphs | Height | Image (resource) | Sheet |
  |---|---|---|---|---|---|
  | 0x7531 | small | 82 | 32 | Material.ZAL 9 (0x30) | 256x256, 4-bit |
  | 0x7532 | tiny (accents) | 144 | 24 | 11 (0x32) | 256x256, 8-bit |
  | 0x7534 | button glyphs `wedcxzaqXT1SC2 ` | 15 | 32 | 12 (0x33) | 128x128, 8-bit |
  | 0x7535 | icons `LOCBWMTcpPR` | 11 | 16 | 13 (0x34) | 64x48, 8-bit |

  [V] All record pads are 0; header bytes 1-3 are 0, bytes 5-11 hold leftover values (kept as they
  are). [V] The replay icons `p` / `P` / `R` (pause / play / record) sit at v = 48, below the end of the
  64x48 icon sheet; [I] what the game shows for them depends on how the driver pads the texture.
  `dmbmx gui hud-export / hud-import` (docs/tools/menu-modding.md): metrics JSON + the sheet as PNG.
- [V] Which STRS sets the game loads in a level (`overlayLoadStrings`): 0x7D01 + language (ids
  0x1000..) and either 0x7D05 + language (single player, ids 0x2000..) or 0x7D09 + language (two players,
  ids 0x3000..).
- [V] **STRS** = `"STRS", u16 baseTextId, u16 count, u16 offset[count]` (offsets from the chunk start),
  then NUL-terminated latin-1 strings. Text id = base + index. The base is 0, 0x1000, 0x2000 or 0x3000.
  - Example: table 0x7919, string 0x25E = "LOAD COMPLETE.".
  - [V] The tables share strings: 16-23 % of the offsets point at a string another id already
    uses (an equal text, or the NUL of the previous one for an empty text). `tools/dmbmx/formats/strs.py`
    keeps the string area as it is; an edited text is appended at the end and only its id is moved, so
    an unedited table writes back byte for byte (all 16 tables).
  - [V] Limits: the front-end set (0x7919 + language) is loaded into a 16 KB buffer and refused when it
    is larger than 0x3FFF bytes (`frontendLoadMacros`, "FRONTEND: Macro data too big or too small");
    the shipped tables are 11,801-13,072 bytes. The HUD sets are allocated at their own size.
    `zstringAddSet` only checks the tag; the set slot is `base >> 12`.
  - Menu labels: main menu 0x24 PROQUEST, 0x25 SESSION, 0x26 FREERIDE, 0x29 COOL STUFF, 0x2A OPTIONS,
    0x2C QUIT (`g_feMainItems` 0x0055A35C); OPTIONS 0x132 CONTROLLER, 0x133 CHEATS, 0x139 AUDIO.
    `dmbmx gui texts / text-set / text-import`.

### 3.2 GUI groups and SCRN screens (Intwrap.zal group 0; the front-end pass)

Code: `tools/dmbmx/formats/scrn.py` (reader / writer), tests `tests/test_scrn_*.py`; the game's reader is
`guiLoadScreenFile` 0x00439A00 (`decomp/src/zaxis/Bmx/Src/com/menu/gui/guisfile.c`), the types are in
`decomp/include/gui.h`; how the menus use them: `docs/re/frontend.md`.

**Group** (each of the 35 nested assets; `zprimLoadGroup` 0x0047B400):

```
Group := u32 count, Entry[count], Asset*          Entry := u32 asset_id, u32 offset (from the group start)
```

[V] In all 35 groups the assets follow the table in table order, back to back (no padding), so the
writer recomputes the offsets. Asset type = `asset_id & 0xE000` (`zprim.h` ZPRIM_TYPE_*): 0x0000
picture (12, the full-screen backdrops of `guiLoadBackdropImage`), 0x4000 sprite (260), 0x8000 font (6), 0xA000 screen (34). Group
0xE000 is loaded permanently (`guiInitializeMenuMode`: fonts 0x8002/0x8003/0x8005 and the shared
sprite 0x40C4, the selection frame); group 0xE000 | n holds screen 0xA000 | n and its art and is loaded
into slot 1 when the screen opens (`guiLoadScreenNow`). The select screen also loads groups 0xE001-0xE004
and the per-rider style groups 0xE017-0xE022 into slots 2 / 3; cool stuff loads 0xE00A (level thumbnails).

**SCRN** (one per group 0xE001-0xE022):

```
SCRN := Header(0x2C), EDEF[edef_count] (0xD4 each), STRS
STRS := "STRS", NUL-terminated strings, NUL padding to a multiple of 4
```

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | char[4] | tag | "SCRN" [V] checked ("GUISFILE: Error - bad screen %04.4X") |
| 0x04 | u32 | size | the whole record [V] = asset size |
| 0x08 | u32 | id | 0xA000 \| n [V] |
| 0x0C | u16 | backdrop | picture asset id, 0 = none ([V] 0xA008: 0x000A, 0xA00B: 0x0006) -> `guiLoadBackdrop` |
| 0x0E | u16 | unk_0e | 0 [V] |
| 0x10 | i32 | edef_count | [V] at most 100 elements exist (`guiCreateElement`) |
| 0x14 | i32 | edef_offset | 0x2C [V] |
| 0x18 | i32 | text_count | number of strings in STRS [V]; not read by the game |
| 0x1C | i32 | strs_offset | = 0x2C + 0xD4 * edef_count [V] |
| 0x20 | u16 | focus_font | font of a focused button (`guiSetFocusFont`) |
| 0x22 | u8[3] | focus_rgb | its colour; 0x25 pad |
| 0x26 | u16 | blur_font | |
| 0x28 | u8[3] | blur_rgb | 0x2B pad |

EDEF (0xD4 bytes; `GuiEdef`, read field by field by `guiLoadScreenFile` into a `GuiElement`):

| Off | Type | Name | Notes |
|---|---|---|---|
| 0x00 | char[4] | tag | "EDEF" [V] checked |
| 0x04 | u32 | id | screen << 16 \| number: the frontend finds elements by it (`guiFindElement`) |
| 0x08 | u8[4] | link | up / down / left / right neighbour (low byte of its id) for focus moves |
| 0x0C | u16 | flags | GUIF_*: 0x1 hidden, 0x2 focused, 0x10 focus on load, 0x20 scaled, 0x40 wide, 0x80 / 0x100 centre x / y |
| 0x0E | u16 | type | GUI_TYPE_* (1 sprite, 2 button, 3 toggle, 5, 6 null, 7 line, 8 box); [V] all 177 shipped EDEFs are 1 |
| 0x10 | u16 | sprite_param | when sprite_flags & 8 |
| 0x12 | i16 x 2 | x, y | position |
| 0x16 | i16 x 2 | w, h | box for alignment (sprite size wins when there is a sprite) |
| 0x1A | i16 x 2 | scale_x, scale_y | 8.8 fixed point, used when flags & 0x20 |
| 0x1E | i16 x 2 | slide_x, slide_y | start offset of the slide-in (4 / 3 px a frame) |
| 0x22 | u16 | priority | draw order (higher first) |
| 0x24 | u16 | sprite | asset id, 0 none |
| 0x26 | u16 | frame | |
| 0x28 | u16 | anim_count | frames of the animation (sprite_flags & 0x100) |
| 0x2A | u16[30] | anim_frames | |
| 0x66 | u16[30] | anim_times | frames each |
| 0xA2 | u16 | sel_sprite | drawn while selected |
| 0xA4 | u16 | sprite_flags | GUIS_*: 0x2 no sprite fields, 0x4 / 0x8 draw modes, 0x100 animate, 0x200 backwards, 0x400 loop, 0x800 ping-pong |
| 0xA6 | i16 x 2 | sprite_x, sprite_y | offset (line / box elements: the second corner) |
| 0xAA | u16 | font | font asset id |
| 0xAC | u16 | text_flags | GUIT_*: 0x2 no text fields, 0x4 / 0x8 / 0x10 draw modes, 0x20 centre x, 0x40 centre y, 0x80 right, 0x2000 fixed width |
| 0xAE | i16 x 2 | text_x, text_y | |
| 0xB2 | u16 | pad | |
| 0xB4 | u32 | text | offset from the "STRS" tag, 0 = none; '
' separates lines; a text starting with '!' is markup (`guiDrawMarkup`: `!S x,y,frame`, `!B$text$`, `!T x,y$text$`) |
| 0xB8 | u8[3] | text_rgb | 0xBB pad |
| 0xBC | i16 | spacing | extra pixels per glyph; 0xBE pad |
| 0xC0 | u16 | toggle_font | type 3 only |
| 0xC2 | u16 | toggle_flags | |
| 0xC4 | i16 x 2 | toggle_x, toggle_y | |
| 0xC8 | u32 | toggle_text | offset into STRS: the options "a\|b\|c" |
| 0xCC | u8[3] | toggle_rgb | 0xCF pad |
| 0xD0 | i16 | toggle_spacing | 0xD2 pad |

[V] Rules every shipped screen follows (and `Screen.rebuild_strs` reproduces byte for byte): the
STRS strings are the EDEF texts in EDEF order (text, then toggle text), one string per text-bearing
EDEF (empty strings included), `text_count` is their number, and the block is padded with NULs to a
multiple of 4. Screens with elements: 0xA005 main menu (16), 0xA009 select (34), 0xA00C options (76),
0xA00D cool stuff (23), 0xA012 load / save (20), 0xA013 (2), 0xA014 language (6); the other 27 SCRNs are
empty (backdrop-only groups). Most texts are placeholders that the frontend overwrites with STRS 0x7919
texts at run time ("String to be preallocated", "5", "1"), so a mod changes menu text in miscdata.zal
and layout here. An edited group goes back with `ZalArchive.set_data` on the nested archive (`dmbmx gui import`
does this; section 3.3); [I] that the game accepts it waits for the manual in-game check `menu-edit`.

### 3.3 Menu art: zprim pictures, sprites and fonts (Intwrap.zal group 0; the menu-modding work)

Code: `tools/dmbmx/formats/zprim.py` (reader / writer, PNG export / import), `tools/dmbmx/formats/gui_dir.py` and
`dmbmx gui` (export directory, docs/tools/menu-modding.md); tests `tests/test_zprim_*.py`,
`tests/test_cli_gui.py`. The game reads these assets in place: `zprimLoadPicture` 0x00479010,
`zprimLoadSprite` 0x00479660, `zprimLoadFont` 0x0047A420 (`decomp/src/zaxis/Bmx/Src/com/zge/zprim.c`),
turns pixels + palette into a RenderWare material with `zprimMakeMaterial` 0x0047C030, and draws glyphs
with `psx2DDrawGlyph` 0x00463C70 (`NgIFpsx.c`). Types: `decomp/include/zprim.h`.

[V] The 278 non-screen entries of the 35 groups are 215 distinct assets: an id found in more than one
group (font 0x8001, ...) holds the same bytes in every group. All 215 round-trip byte for byte, and
PNG export -> import of every one gives the same bytes.

**Image** (picture 0x0xxx: 12, all 512x512 8-bit backdrops; sprite 0x4xxx: 199):

```
Image   := Header(0x14), Frame[count] (0x10 each), pixels of frame 0..count-1, Palette
Header  := u16 count, u16 flags, u16 width, u16 height, u32 pixels, u32 palette, u16 bpp, u16 pad
Frame   := u16 w, u16 h, u32 pixels, i16 hot_x, i16 hot_y, u32 material
Palette := u16 unk (0), u16 count, u16 colour[count], rest of the block      (block = 0x204 bytes)
```

| Field | Notes |
|---|---|
| width, height | [V] = frame 0's size |
| pixels, Frame.pixels, palette | offsets from the asset start. [V] Canonical in all 211: frame pixels follow the frame table back to back and the palette follows the last frame; the writer lays them out again |
| bpp | 4 or 8. [V] 4-bit rows are `w / 2` bytes with the **left pixel in the low nibble** (zprimMakeMaterial swaps the nibbles for its BMP; checked visually and by `test_nibble_order`) |
| flags | [V] bit 0 is set on exactly the 33 4-bit sprites whose palette has 256 entries (16 banks of 16). zprim.h calls it "one palette per frame", but all 33 have one frame and the loader only uses colours 0-15 (`zprimSelectPaletteBank` copies another bank in for a copy of the sprite) |
| hot_x, hot_y | subtracted from the draw position for 0x4xxx sprites; [V] 0 everywhere |
| material | runtime (zprimMakeMaterial's result); [V] 0 in the file |
| Palette.colour | 15-bit, red in the low bits (`r = c & 0x1F`, `g = c >> 5 & 0x1F`, `b = c >> 10 & 0x1F`, each << 3). **0x7C1F = transparent**: it becomes the magenta colour key 0xFF00FF |
| Palette block | [V] always 0x204 bytes: count 256 fills it; with count 16 the other 480 bytes are left over (zero in 60 sprites, other data in 52) and are kept |

Counts [V]: 12 pictures 8-bit / 256 colours; sprites: 114 x 8-bit / 256, 52 x 4-bit / 16, 33 x 4-bit / 256
(flags 1). `zprimMakeMaterial` pads the image to a square power of two (4..256; larger sizes are left
alone) in its in-memory BMP, so any even width works. `psx2DDrawPicture` stretches a picture over 512 x
512 screen units, so backdrops should stay 512x512 [I].

**Font** (0x8xxx: 0x8001, 0x8002, 0x8003, 0x8005):

```
Font  := u16 count, u16 line_height, u32 pixels, u16 width, u16 height, u32 materials (0), u32 palette,
         u16 bpp, u16 unk_16, Glyph[count] (10 bytes each), pad to 4, bitmap, Palette (0x204 block)
Glyph := u8 x, u8 y, u8 w, u8 h, i8 xoff, i8 yoff, i8 advance, i8 descent, u16 code
```

| Font | Glyphs | line_height | Sheet | Use |
|---|---|---|---|---|
| 0x8001 | 221 (0x20..0xFC) | 20 | 128x113 | groups 0xE001 / 0xE00D / 0xE012 (select, cool stuff, load / save) |
| 0x8002 | 221 (0x20..0xFC) | 20 | 128x159 | permanent: small menu text |
| 0x8003 | 189 (0x20..0xDC) | 28 | 128x180 | permanent: main-menu items, titles; **no lower case** (those slots are empty) |
| 0x8005 | 6 ('1'..'6') | 30 | 128x46 | permanent: the pad-button icons of the prompts ("1" + a label) |

- [V] `line_height` (+0x02) is what `zprimGetFontFlags` returns. width (+0x08) and height (+0x0A) are the
  bitmap size, palette (+0x10) its offset and bpp (+0x14) = 4: psx2DDrawGlyph calls
  zprimMakeMaterial(width, height, bpp, ...). The glyph table starts at +0x18; after it 0-2 bytes of
  padding (0xCD, the MSVC fill).
- Lookup (`zprimFindGlyph`, inlined): with fewer than 256 glyphs, glyph k is character
  `glyph[0].code + k` ([V] every slot holds its own code or 0 = no glyph); otherwise it searches by
  `code`. Only a character *outside* the table falls back from lower to upper case, so in 0x8003
  lower-case letters hit their empty slots and print as gaps.
- A glyph is drawn from (x, y), w x h, at (pen + xoff, y + yoff); the pen advances `xoff + w + advance +
  spacing`; `descent` enlarges the background box. A glyph with w = h = 0 draws nothing.
- **Palette = 16 banks of 16 colours**, the text colours: `psx2DDrawGlyph` copies bank n to colours 0-15
  and makes one material per bank (`guiSetTextPalette`; the main menu uses bank 1).

**PNG mapping** (`zprim.frame_png`, `font_png`, `image_from_png`, `font_from_png`): a palette PNG of the
asset's bit depth. PLTE = the colours the material uses (the first 2^bpp; for fonts bank 0), each
15-bit channel << 3; tRNS gives 0x7C1F entries alpha 0. On import a palette PNG keeps its indices (each
must be < 2^bpp) and a changed PLTE entry becomes the 15-bit colour (alpha < 128 -> 0x7C1F); an
unchanged entry keeps its exact value. Any other PNG (RGB, RGBA) is mapped to the nearest palette
colour. The other banks and the left-over palette bytes are in the asset's JSON.

## 4. Asset ids and the name hash

[V] `zHashName` (0x004828C0, our name) is the engine's name hash. `zsceneParseObject` (0x00490230,
zScene.c) stores `zHashName(NAME)` at object+0x64 (0x004902D5), which is what Scene.Txt means by "the
NAME is hashed to create the asset id". The function upper-cases with the C-locale `toupper` (0x004A593C)
and computes, with `C[i]` the upper-cased **signed** chars, 1-based, `C[0] = 0`, mod 2^32:

```
h  = sum_i i * C[i]
h += sum_i (C[i] + 0xFFFF * C[i-1]) << 16
```

Implemented as `dmbmx.core.hash.zhash` (case-insensitive). [V] Proof against data: the hashes of the 15
body-part names (`Hips`, `Chest`, `Head`, `LeftUpArm`, ... `RightFoot`; strings in the exe) occur as u32
values in every rider `.zmd` (tests/test_hash_corpus.py). Examples: `zhash("Hips") = 0x00530316`,
`zhash("Head") = 0x004402A5`, `zhash("LeftFoot") = 0x00540AF9`.

[V] **No shipped ZAL uses hashed group ids.** All 1,349 ids are small constants (0..0x7D0C, 0xE000..0xE022)
and none equals `zhash()` of any exe string or Scene.Txt NAME. The only ZAL in Scene.Txt is
`Assets\Motion.ZAL` (ANIM "MainBank", `zhash = 0x004B0A2C`), whose single group has id 0. So the
"hashed NAME" mechanism exists in zScene.c but is unused by this build's data; group names therefore
come from the code (see section 3), not from a hash dictionary. [I] A `.ZAL` named in a MODEL/ANIM FILE
line would be looked up with `zasset_FindAsset(lib, zhash(NAME), 0)`; not verified (no such data).

## 5. Invariants checked by tests

- `read(f).write() == f` for all 6 files and the nested archive (tests/test_zal_corpus.py).
- Each asset decodes to `size & 0xFFFFFF` bytes; compressed streams are consumed exactly.
- Pinned shape: group counts, compression flags, `reserved == 0`, material.zal = 1262 BMPs with ids 0..1261.
- Reader rejects bad magic, truncated tables/data, overlapping or out-of-order assets with `FormatError`
  subclasses; fuzzed with `tests/fuzz.py` (tests/test_zal_fuzz.py).
- The ZAL repacker: extract -> pack identical for all 6 files; the encoder round-trips every compressed payload;
  edited/replaced assets change only their own stream (tests/test_zal_repack.py, tests/test_lzo_encoder.py).
- The menu-modding work: every GUI picture / sprite / font, the 16 STRS tables and the 4 overlay fonts round-trip; PNG
  export -> import is identical; `dmbmx gui export` -> `import` rebuilds Intwrap.zal byte for byte, and an
  edited one parses and round-trips (tests/test_zprim_corpus.py, test_zprim_unit.py, test_cli_gui.py).

## 6. Open questions

1. Flags bit 14 and the size word's top byte: always 0; meaning unknown.
2. Codec method 1 (0x0049BD10): not identified (not LZO1X; uses a 0x4010-byte work buffer). Not needed for the corpus.
3. Ref mode (flags bit 13): record layout of the referenced file name not checked against data (none ships).
4. The 8-byte pre-magic header variant: layout inferred from the loader only.
5. Payload formats inside the archives (STRS tables, replay records, the motion bank, the
   0x7531-0x7535 tables) were left for later (texture and motion work). The overlay pass: 0x7531-0x7535 are fonts (3.1).
   The front-end pass: the GUI groups and SCRN screens of Intwrap.zal (3.2). The menu-modding work: the zprim pictures / sprites / fonts
   inside them (3.3), the STRS tables and the overlay font files (3.1) all have readers / writers now.
6. Does the game accept re-encoded LZO1X streams and grown archives? Pending the manual in-game check `zal-repack`.
7. Where are material.zal's 1262 textures used in game? the manual in-game check `zal-repack` part B may tell. (assets
   9 and 11-13 are the overlay font sheets, resources 0x30-0x3A = assets 9-0x13 are the HUD pictures.)
8. What the game shows for the replay icons `p` / `P` / `R`, whose glyphs lie below the 64x48 icon sheet (3.1).

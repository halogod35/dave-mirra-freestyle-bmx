# Textures and materials: MATI / MADF / TBMP / TPNG (ZMD, level `Mat._sd`, material.zal)

Code: `tools/dmbmx/formats/mati.py` (`Mati`, `MatSd`; ZMD uses the same class),
`tools/dmbmx/formats/texture.py` (RGBA decode, 24-bit BMP encode, colour key, constraints; built on the glTF exporter's `tools/dmbmx/convert/imaging.py` for BMP/PNG parsing, PNG (un)filtering and PNG writing),
CLI `dmbmx tex list|export|replace`, contact sheets `tools/research/contact_sheets.py` → `out/textures/`.
Survey: `tools/research/survey_textures.py`. Tags per CONTRIBUTING.md AD-5; VAs are for the 2000-11-22 exe.

## 1. Where textures live

| Source | Container | Count | Formats [V] |
|---|---|---|---|
| `*.zmd` (115 non-empty) | `MODL{ …, MATI* }` | 324 MATI | 212 BMP24, 1 BMP8 (64²), 1 BMP8 (32², 160 colours), 1 BMP4; 109 PNG (8-bit RGBA, colour type 6, non-interlaced; 91 with `gAMA cHRM`, 18 also `iCCP`) |
| `<Level>/Mat._sd` (12) | flat sequence of `MATI` chunks, nothing else, no padding | 872 MATI | 640 BMP8 (palettes of 15-256 colours), 229 BMP4, 1 BMP24, 2 PNG RGBA |
| `material.zal` | ZAL groups 0-1261 | 1262 BMP | 4-bit (1,099) and 8-bit (163) palettised; 8x16 … 256x256, some non-square |

- [V] `Mat._sd` round-trips byte-for-byte for all 12 levels through `MatSd.read/write`.
- [V] Sizes: ZMD 16²-256²; Mat._sd 2²-512² (22 at 512², two non-power-of-two: 80², 96²); every ZMD/Mat._sd texture is square.
- [V] (viewed, `out/textures/material_zal.png`) material.zal holds HUD/effect art (sparks, logos, digits, splash),
  then low-colour (4/8-bit) copies of the **bike** textures and of the **rider body-part / sponsor** textures.
  [I] It is the source of the rider/bike customisation art or a leftover of the console build; the ZMD files carry
  their own textures (every RW texture name resolves inside its own ZMD). the manual in-game check `zal-repack` part B will show
  where the game uses it.
- `miscdata.zal` has **no** MATI chunks (the "MATI" byte matches are inside localised strings; the ZAL reader).
- [V] material.zal assets 9-0x13 are the overlay pictures 0x30-0x3A of `g_zresImages` (0x0055D978);
  assets 9, 11, 12 and 13 are the glyph sheets of the four HUD fonts (ZAL.md 3.1). `dmbmx gui hud-export /
  hud-import` edits them together with the glyph metrics (docs/tools/menu-modding.md).
- **Menu art is not here.** The front end's pictures, sprites and fonts are zprim assets in Intwrap.zal
  (15-bit palettes, 4/8-bit pixels, no BMP/PNG inside): ZAL.md 3.3, `dmbmx gui export / import`. They become
  RenderWare materials through an in-memory BMP (`zprimMakeMaterial` 0x0047C030), so the colour key below
  applies to them too: palette colour 0x7C1F is turned into 255,0,255.

## 2. Grammar

```
MATI := "MATI" u32 size { MADF  (TBMP | TPNG | TDDS) }
MADF := "MADF" u32 size(< 0x90) {
          0x00 u32  nameOffset = 0x48
          0x04 u32  flags        (0)      -> material+0x08; bit 0 tested by 0x00489BA0
          0x08 u32  word08       (0)      -> material+0x0C
          0x0C u32  word0c       (0)      -> material+0x10
          0x10 f32[3] diffuse?   (0.9 x3)      not read by the game
          0x1C f32[3] colour B   (0.5 x3; 11 distinct)   not read
          0x28 f32[3] colour C   (0.1 x3; 11 distinct)   not read
          0x34 f32    param      (0.25)        not read
          0x38 u32  surface/anim word            -> material+0x14
          0x3C u32  anim1        (0, FAN1: 0x0100FF05) -> material+0x18
          0x40 u32  anim2        (0, FAN1: 0x00008102 / 0x01018102) -> material+0x1C
          0x44 u32  nameId = zhash(name)
          0x48 char name[] NUL, zero-padded to 4 (upper-case RW texture name)
        }
TBMP := "TBMP" u32 size { complete .bmp file, zero-padded to 4 }
TPNG := "TPNG" u32 size { complete .png file, zero-padded to 4 }
TDDS := "TDDS" u32 size { .dds file }          registered by the engine (0x004986FA), never shipped
```

### 2.1 MADF evidence

- [V] Handlers are registered in `zrwMat.c` (0x00489440: `MATI` → 0x004892B0, `MADF` → 0x004893B0) and
  `zrwTxr.c` (0x00498660: `TBMP` → 0x004981F0, `TPNG` → 0x004982A0, `TDDS` → 0x00498320).
- [V] The MADF handler reads the whole chunk into a global buffer (0x00672320). It asserts "Data too big"
  if the chunk is ≥ 0x90 bytes (0x004893BE) and "MADF Mismatch" unless dword 0 is 0x48 (0x004893F5).
  It then hands the name to the texture module (0x00498B40).
- [V] After MADF and the image have been read, the MATI handler (0x004892B0) copies **only** MADF +0x04,
  +0x08, +0x0C, +0x38, +0x3C and +0x40 into the engine material (offsets +0x08…+0x1C). The ten floats at
  +0x10…+0x34 are never read (no other reference to that buffer exists), so editing them has no effect.
  [I] They are exporter defaults (3ds Max colours/shininess).
- [V] MADF lengths range from 80 to 104 bytes. The only variable part is the name; the name id is always
  `zhash(name)` (1196/1196).
- [I] **Surface type** = bits 0-3 of +0x38. This is inferred from texture names in the 12 levels:
  - 0 default (784)
  - 1 metal (rails, towers, fan housing)
  - 2 wood (decks, ramps)
  - 3 wall
  - 4 brick/buildings
  - 5 grass
  - 6 ocean
  - 7 foliage (palms, plants)
  - 8 rubber (tires)
  - 9 sand
  - 10 dirt
  - 11 water
  - 12 chain-link fence
  - 13 fence
  - 14 foam

  ZMD textures are all 0. [I] It probably selects sounds/particles. Not verified in the exe.
- [I] **Animated material:** the FAN1/FAN2/FAN3 textures have +0x38 = 0x131 / 0x2131 / 0x4131 (bits 4-5 and 8
  as flags, bits 13-14 as frame index), and FAN1 carries the extra +0x3C/+0x40 words. This matches the exe
  strings "zrollerSetAnimMaterialPalette" / "Associate material pal animation". The exact layout is open.

## 3. How the game decodes the images (constraints for replacements)

- [V] TBMP/TPNG payloads are handed to `zrwTxr_CreateTextureFromMemory` (0x00497B00). The kind is 0 = .bmp,
  1 = .png or 4 (.dds). It mounts the buffer as an in-memory file and loads it through RenderWare's image
  readers, which are registered at 0x0049871B for `bmp` (0x004979E0), `png` (0x00497A30, libpng 1.0.5) and
  `dds` (0x00497A80).
- [V] After decoding, 0x004978F0 applies a **colour key**. In 4/8-bit images every palette entry, and in
  truecolour images every pixel, that equals RGB **(255, 0, 255)** is set to RGBA 0,0,0,0 (transparent).
  The key is initialised to FF 00 FF in 0x00498660. Magenta in a BMP therefore means "transparent".
  `dmbmx tex export --png --colorkey` reproduces this.
- [V] (the ZglRw pass, corrected) The main path is `RwTextureRead(name, NULL)` on the in-memory file (extension
  "BMP" / "PNG"): a texture of the same **name already in the current texture dictionary is reused**,
  otherwise the registered reader decodes the image (with the colour key) and **RenderWare chooses the
  raster format** for it (paletted images stay paletted when the driver supports PAL4/PAL8 [I]). Only if
  that read fails is the image read again as a BMP and put into a 16-bit 565 texture raster
  (`RwRasterCreate(w, h, 16, 0x204)`), else a 32-bit one (`(w, h, 32, 4)`) (0x00497C09…0x00497C4A); a
  565 raster has no alpha, so the colour key has no effect on that path. The game never resizes images.
- [V] Filtering is linear (linear-mip-linear only with mipmapping on: ZGL value 0xE, off by default);
  addressing is **clamp** after zrwZglInit (ZGL value 0xC; wrap when cleared). The MADF name is the texture
  name; materials are looked up by `zHashName(name)` of their first texture (docs/re/zrw.md 3, 4).
- [V] The colour key port in `tests/test_decomp_zglrw.py` (palette-level for 4/8-bit BMPs, pixel-level for
  24-bit) agrees with `texture.apply_color_key(texture.decode_bmp(...))` on every TBMP of the corpus.
- [V] Optional preallocated load buffer: if set, a TBMP larger than it asserts "Load buffer is too small."
  (0x00498210). Its setter (0x00498CF0) is not called directly; we found no caller.

Rules enforced by `texture.check()` / `dmbmx tex replace`:

| Rule | Level | Reason |
|---|---|---|
| The slot keeps its kind (TBMP stays BMP, TPNG stays PNG); other inputs are converted | error → converted | the loader picks the decoder by the slot (0x00498258 / 0x004982CB) |
| BMP: uncompressed (BI_RGB), 4/8/24 bit, 40-byte header | error | what ships; [I] RW's BMP reader may handle more, untested |
| PNG: non-interlaced; 8-bit RGB/RGBA/palette recommended | error (interlace) / warning | what ships (all 8-bit RGBA) |
| Same size as the original texture (`--resize` scales nearest-neighbour, `--keep-size` overrides) | error unless flag | UVs are normalised, so any size works, but keeping the size is safest |
| Power-of-two width and height | warning | 2 level textures and 14 material.zal BMPs are not, so the loader accepts them; [I] some D3D7 cards require powers of two |
| ≤ 512×512 | warning | largest shipped |
| Alpha | — | PNG alpha is used as-is. For a BMP slot, pixels with alpha < 128 are written as 255,0,255 (the colour key) |

## 4. API / CLI

```
dmbmx tex list <file.zmd|Mat._sd|*.zal>
dmbmx tex export <file> <outdir> [--png] [--colorkey]    # as-is files + _textures.json (+ .png for BMPs)
dmbmx tex replace <file.zmd|Mat._sd> <texname> <image.bmp|png> <out> [--resize] [--keep-size]
```
`Mati.set_image()` swaps the embedded file and fixes the chunk sizes and padding. The rest of the file is
written back unchanged. `texture.encode_png` writes valid CRCs (checked by `decode_png(check_crc=True)` in tests).

## 5. Open questions

1. The surface-type names are inferred from texture names; the enum (and the physics/sound tables that use it) is not yet found in the exe.
2. The animated-material bit layout (+0x38 high bits, +0x3C, +0x40).
3. MADF +0x04/+0x08/+0x0C: always 0. Bit 0 of +0x04 is tested (0x00489BA0), but its meaning is unknown: the ZglRw pass found that its
   only caller (zrwTxr_ChunkTBMP) ignores the result, so it has no effect on the PC build [V].
4. Does the 16-bit raster format (0x204) drop alpha on some video cards? The ZglRw pass: that format is only the fallback when
   RwTextureRead fails (section 3); normally RenderWare picks the format. Does a 24-bit BMP in a level slot look right?
   Covered by the manual in-game check `texture-swap` for ZMD.
6. Two MATI chunks with the same name share one texture (the dictionary lookup), so a replacement must
   rename or replace every copy [I: consequence of the code, not play-tested].
5. What material.zal is used for (see §1).

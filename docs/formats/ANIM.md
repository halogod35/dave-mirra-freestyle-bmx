# Animations: motion.zal motion bank, skeleton binding, replay.zal

Owner: DM-016. Code: `tools/dmbmx/formats/motion.py` (bank, clip reader/writer, evaluator, rotations),
`tools/dmbmx/formats/zal.py` (`ZalArchive.read(..., headerless=True)`), `tools/dmbmx/convert/anim_gltf.py`
(glTF animations), CLI `dmbmx motion list|dump` and `dmbmx export --anims`. Blender check:
`tools/blender/scripts/check_anim.py`. Tests: `tests/test_motion.py`. Probe: `tools/research/probe_motion.py`.
Evidence tags as in CONTRIBUTING.md AD-5. The exe was read through the DM-002 Ghidra decompilation (`out/ghidra_c`).

## 1. The bank

```
motion.zal             ZAL (DM-010), 1 group (id 0), LZO1X
└─ payload (202,116 B) headerless ZAL: u16 flags=1, u16 count=238, 238 x {u32 id, u32 offset, u32 size}, data
   └─ motion 1..238    one clip each; offsets count from byte 0 of the payload
```

- [V] The headerless table is the in-memory ZAL form: `zactorOpenMemBank` 0x00475200 skips 16 bytes
  only if the magic is present. `zactorLoadMotionBank` 0x00475050 loads group 0 (≤ 0x40000 bytes, "Too big") and opens it
  that way. `ZalArchive.read(data, headerless=True)` round-trips it.
- [V] Motion ids are 1..238 in bank order. `action.c` walks the bank and stores 238 pointers (it asserts
  "Code does not match motion asset file" if there are more than 0xEE). `rider.c` indexes that table by motion
  number (`&DAT_006962fc + id*4`). There are **no names or hashes** for motions. Scene.Txt's ANIM
  "MainBank" (zhash 0x004B0A2C) only names the bank. The exe has a table of 84 trick names
  (0x00559390, "turndown" … "busdriver"); DM-046 found the trick → motion mapping (section 7).
- [V] Everything round-trips byte-for-byte: motion.zal, the bank payload and all 238 clips.

## 2. Clip format (decoder `zactorDecodeMotion`, VA 0x00472B10; C in DM-053's zactor.c)

```
0x00 u16  frames | 0x8000 (= a 100-byte block follows the section table; never set)
0x02 u16  flags: bits 4-7 = number of sections; bit 15 = 16-bit angles (else 8-bit); all files 0x9823
0x04 u32  frame time in ms (34 everywhere, about 29.4 fps)
0x08 u32  extra offset (0 everywhere)
0x0C      section[n]: u8 channels, u8 flags (0x80), u16 kind (0 = rider, 1 = bike)
...       [100 bytes if frames & 0x8000]
...       one init record per section (none if section flags & 0x02)
...       delta records, in time order
...       u32 0, u32 end time (= frames x 34)
```

[V] Every clip has 2 sections: rider (15 channels) and bike (1, 2, 3 or 5 channels).

**Records** start 4-byte aligned and are zero-padded to 4:

| Field | When | Content |
|---|---|---|
| u32 mask | always | bits 28-31 = section; bit 0 = long header; bit 1 = translation; bit 2+i = channel i present |
| u16 time | always | ms; a multiple of 34 |
| u16 h, f32 value | mask bit 0 | [V] DM-053: `h` is never read; `value` (rider section only) is stored in the decoder state (+0x1C) and the returned frame (+0x14) and never read by any code. Both are 0 in every clip |
| 3 × s16 translation | mask bit 1 | |
| 3 × s16 per present channel | bits 2.. | 8-bit mode: 3 × s8 (deltas) / u8 (init) |

The **rider init record** has a fixed layout: u32 mask, u16 time, u16 h, f32 value, 3 × s16 translation,
then every channel. The other init records use the generic layout (bit 2 = "all channels").

**Semantics** ([V] from the decoder; units [I]):
- Init values are absolute. Translation is × 1/8. Angles are s16 binary angles (65536 = 360°); in 8-bit mode u8 << 8.
- Delta records set a per-channel **delta** (s16, or s8 << 7), and translation deltas × 1/256.
- On every frame step, each channel adds its *current* delta. A delta **persists** until a later record
  replaces it, because the decoder never clears its delta buffer. This explains why 356 records store an
  explicit (0,0,0): that is how a rotation is stopped.
- Rider translation: x and z deltas are per-frame displacements (root motion, rotated by the actor heading,
  0x00481390). y accumulates (height). Bike translation accumulates. [I] These are world units, not model
  units, and they look Y-down: the rider sits about 140-180 units at *negative* y relative to the bike
  (e.g. motion 5: rider (−2.75, −89.1, −9.1), bike (+2.75, +89.1, +9.1)). Root motion is therefore
  **not** exported to glTF.
- [V] `Motion.sample()` reproduces this (one `Pose` per frame). [V] The pedalling clip (motion 8) turns
  Sprocket, RightPedal and LeftPedal one full revolution about X while y/z stay 0. That is the crank axis
  (bike +X = left), which confirms both the decoding and the axis order.

### 2.1 The decoder in detail (DM-053)

Source: `decomp/src/zaxis/Bmx/Src/com/zge/zactor.c` (`zactorDecodeMotion`, types `decomp/include/zactor.h`);
a literal Python port is in `tests/test_decomp_dm053.py`.

- [V] **Agreement:** the port and `Motion.sample()` give identical rider/bike angles, height, root motion
  and bike translation on **all 2,301 frames of all 238 clips**, plain and mirrored (motion flag 8), and on
  synthetic clips for the cases the bank does not use (8-bit angles, odd record times, a time-0 group, a
  section with flag 0x02, a stream that ends early). The game shows exactly `frames` frames of every clip:
  the last record group is at (frames - 1) x 34 ms in all clips.
- [V] **What is read:** header bits 4-7 (section count) and bit 15 (16-bit angles) of the flags word,
  bit 15 of the frames word (100-byte block), `frameMs`, `extra`; per section the channel count, the kind
  and **only bit 1** of the section flags. Not read anywhere: flags bits 0-3 / 8-14 (0x9823 -> `3`, `0x18`),
  section flag 0x80, the record u16 `h`, and the record f32 `value` beyond storing it (state +0x1C, frame
  +0x14, neither read by any code). The 100-byte block pointer and the `extra` pointer are stored, never read.
- [V] **Clock and records:** every call adds `ftol(34.0 x rate)` ms (constant 0x0055F35C, not the header's
  frameMs); per frame step (header frameMs) the decoder applies the *one* group of records that share the
  time of the next pending record, if that time is <= the clock, then adds every channel's current delta.
  Once the stream reaches its terminator the next call returns NULL (rider.c ends the clip).
  `Motion.sample()` now does the same (DM-053 fix; identical on the shipped bank): before, it applied
  records only at exactly k x frameMs, kept stepping after the stream ended, stepped sections with flag
  0x02 and did not wrap 8-bit init angles (u8 << 8) to s16.
- [V] **Section flag 0x02:** no init record, and the section is never stepped (its records are still
  parsed). A rider section (kind 0) always has its fixed init record.
- [V] **State not cleared on restart:** the delta buffers survive a restart, so a clip that does not set
  a delta at frame 1 inherits the previous clip's. Every shipped clip sets every channel and translation
  delta at frame 1, so this never shows. All non-rider sections share one bike state (only 2 sections in
  the bank).
- [V] **Mirror (flag 8):** rider channels go through `g_motionMirrorMap` 0x0055F364; y/z angles and the
  x translation are negated (bike channels are not swapped).
- [V] **Root motion:** the returned frame holds this call's x/z delta (the init record's x/z / 8 on a
  restart; on a loop restart the last frame's delta), rotated by the global heading
  (`zactorSetHeading(-0x4000, 1)` in action.c: +90 degrees) and scaled by the actor scale.
- [V] **Crossfade:** `zactorPlayMotion` can fade from the previous clip over 3 frames, but riderUpdateAll
  always passes 0x80 (cut), so it never runs. It is also buggy: the x-angle mix always adds 360 degrees
  to the smaller angle (its wrap test compares with -180) and the bike-translation mix uses weights 0.5 /
  1.5.

## 3. Channel → frame binding (NgIFzactor.c)

- [V] Rider channel order (`0x00465130`): Hips, Chest, Head, LeftUpArm, LeftLowArm, LeftHand, RightUpArm,
  RightLowArm, RightHand, LeftUpLeg, LeftLowLeg, LeftFoot, RightUpLeg, RightLowLeg, RightFoot.
- [V] Bike channel order (`0x00465340`): Hipsdeck, Handlebars, Sprocket, RightPedal, LeftPedal, BackWheel,
  FrontWheel, LeftGrip, RightGrip, SeatGrip. Motions use the first 1-5.
- [V] Mirroring (motion flag 8) swaps left/right channels (table 0x0055F364) and negates y/z.
- [V] Every frame (`0x004654b0`), each channel's frame is found **by zhash of its name** (`zanimFindNode`
  0x0049A540 compares the frame's bone id, i.e. the ZMD Animation(0x108) id), and its quaternion is
  **replaced** (`zanimNodeSetQuat` 0x0049A570). The bind translation stays.
- [V] Euler → quaternion (`zquatFromEuler` 0x004832B0): half angles via the 8192-entry sine table (`sin`
  table at 0x0065E2BC, `cos` = the same table + 1/4 period). The result is **q = qz · qy · qx** (x applied
  first), quaternion layout (x, y, z, w), Hamilton product (`zquatMul` 0x00483070 = a·b). `zquatToMatrix`
  0x00483470 builds the standard column-vector rotation in the RW "right/up/at" layout. [V] DM-045: the
  half angles are truncated to binary angles and the table steps by 8 units, so the game's quaternions
  differ from exact maths by up to 1.7e-3 per component (all 43,895 channel frames of the bank checked,
  `tests/test_decomp_dm045.py`).
- [V] **DM-045: the node quaternion is not the frame rotation.** `zrwModelUpdate` (the model update)
  builds every animated frame's RW matrix with `zrwModelQuatToRwMatrix` 0x00487E70, which first maps
  q = (x, y, z, w) to (x, −y, −z, w): the frame rotation is **F·R(q)·F** with F = diag(1, −1, −1) (the
  engine's Y-down axes → RenderWare), for rider and bike channels alike (the bind translation is added
  unchanged). Since DM-019 dmbmx does the same (`motion.engine_to_model`, applied by `channel_rotation` to
  every rider and bike channel); DM-016/017 put q in directly (§3.1).
- Arm corrections. Here Cz(±90°) is a rotation about Z by ±0x4000, and Rx(180°) a rotation about X by 0x8000.
  dmbmx (DM-019) uses NgIFzactor's literal table and then F; the DM-017 column is kept as `legacy=True`.

| Channel | **NgIFzactor_ApplyMotionPose, used by dmbmx (then F·R·F)** | DM-017 legacy (no F) |
|---|---|---|
| LeftUpArm | q · Cz(+90) | Rx(180) · q · Cz(+90) |
| LeftLowArm, LeftHand | Cz(−90) · q · Cz(+90) | same |
| RightUpArm | q · Cz(−90) | Rx(180) · q · Cz(−90) |
| RightLowArm, RightHand | Cz(+90) · q · Cz(−90) | same |
| all others (bike channels too) | q | q |

  The rider bind skeleton is a T-pose (arms along ±X, identity local rotations). The ±90° corrections turn
  the motion data's arm axis into the model's arm axis.

### 3.1 Hands-on-handlebars test (DM-017, DM-019, `tools/research/arm_pose_check.py`)

The test poses the bike with the clip's bike section and the rider with its rider section. It then places
the rider by translation only, so that the mid-point of its feet sits on the mid-point of the pedal frames.
Both models share units and axes.
- [V] **Bike FK check:** with the motion's bike channels both wheel axles are level (within 0.03) and
  0.9 apart, and the pedals rotate on the crank. The bike section is applied correctly.
- [V] **Placement check:** feet to pedals has a median of 0.068 (DaveMirra1 on DMbike01, motions
  8/5/36/22/57/106).
- [V] **Results** (medians, hands to bar):

  | Arm table | hand → nearest Handlebars-mesh vertex | hand → bar-end centroid | hand gap |
  |---|---|---|---|
  | as decompiled | 0.66 | 0.69 | 0.54 |
  | DM-017 | **0.13** | **0.21** | 0.54 (bar-end centroids ~0.6 apart) |

  Legs and torso are unchanged. [V] 54 of the 61 in-game rider ZMDs have this exact skeleton; the Troy McMurray models differ by less than 0.002, and MikeLaird5 has a different frame list. DMbike01 and RNbike01 have identical frames. So the result carries over to the other riders.
  We searched systematically:
  - all 24 axis-aligned corrections;
  - sign/axis variants;
  - Euler orders and conjugates;
  - a free continuous rotation.

  A ~176° turn about ≈X before the upper-arm quaternion (in the shoulder frame) is the only consistent
  winner. The optimum is (0.99, 0.11, −0.08) at 175.7°, with a bar-end median of 0.14. Rx(180) is its
  rounded form. Mirroring the Cz signs instead also brings the hands down (0.09 to the mesh), but it puts
  both hands at the stem (gap 0.19).
- [V] **Bar ends vs grip frames:** the LeftGrip/RightGrip *frames* are not on the bar ends in the motion pose.
  They sit about 0.2 above the bar mesh (0.37-0.42 from the hands with any arm table), so they are not
  usable as hand targets.
- [V] **Where the flip comes from (DM-045):** the F conjugation above. With F·R(q)·F on every animated
  frame (rider and bike) and NgIFzactor's arm table **as decompiled**, the hands land on the bar:
  hand → bar mesh 0.13, hand → bar-end 0.16 (better than DM-017's 0.21), feet → pedals 0.10, head above
  the hips in every frame of motion 8. Without F the same table gives 0.66/0.69, and DM-017's table *with*
  F gives 0.68: the Rx(180) is the leading F of the shoulder's F·R·F, and the trailing F is invisible
  because the arm bones lie along ±X, which F leaves fixed. Torso and legs also change under F (median
  joint shifts 0.03-0.15 relative to the hips), so DM-017's table is only an arm-local approximation of
  the game's chain. `tests/test_decomp_dm045.py::test_F_conjugation_explains_the_arm_flip`.
- [V] **DM-019 (dmbmx now uses F):** `tools/research/arm_pose_check.py` (motions 8/5/36/22/57/106, medians):
  F + literal table: feet 0.098, hand → bar 0.132, hand → bar-end 0.158, hand gap 0.56; legacy DM-017:
  0.068 / 0.127 / 0.206 / 0.54; literal without F: 0.068 / 0.660 / 0.692. The feet are the one metric that
  got worse: with F the feet are ~0.37 apart (pedals ~0.2), knees out, so each foot sits ~0.09 off its pedal
  frame while pedalling (max 0.12 in motion 8). [I] Whether the game shows the same knees-out stance needs
  an in-game screenshot; the conversion itself is [V] code.
- [V] The Hips channel already stands the skeleton up in a Y-up model space: Hips up ≈ (−0.10, 0.99, −0.04)
  in motion 1, frame 0. The bind Hips frame instead rotates Y→Z (DM-020 export.md §2), so the exporter
  **removes** the DM-020 upright root rotation when it adds rider animations.

## 4. glTF export (`dmbmx export <zmd> <out.glb> --anims all|1,5,10-12`)

- One glTF animation per motion (`motion_NNN`), LINEAR. Each channel frame node gets a rotation track
  (one key per frame at k × 34 ms; shortest-path sign fix). Animated nodes are written as TRS instead of
  `matrix`. Frames that are not channels (`_Abdomen`, `Chestx`, `_LeftShoulder`, …) keep their bind transform.
- Models with bike channel frames (Hipsdeck/Handlebars/…) get bike tracks instead (`which="auto"`).
- [V] Blender 5.3 headless (`tools/blender/scripts/check_anim.py`) imports the clips as actions. For every frame
  of motions 1, 2, 3, 8, 15 and 59 the skinned mesh stays finite, and its bounding box stays within 1.0× of
  the rest diagonal (no explosion). This is a pytest (`test_blender_anim_check`) when Blender is installed.
- [V] **Pose (DM-019):** the tracks carry the engine's F·R(q)·F with the literal arm table: legs pedal,
  the hands are on the grips. Renders: `out/dm019/frf_strip.png` (DM-019 worktree): rows are clips 8, 15
  and 59; columns are Blender frames 0/8/16/24 side-on and frame 0 front 3/4 (`tools/blender/scripts/render_pair.py`
  per view, tiled by `tools/blender/scripts/compose_strip.py`). The DM-017 strip is `out/pose/arm_pose_strip.png`
  in the DM-017 worktree.

## 5. replay.zal (demo replays)

- [V] `action.c` (demo mode) loads asset id `profile[0][3] + 2 × level` (12 levels × 2 = the 24 groups).
  It loads it into a replay buffer created with size 0x2800, and asserts "replay data too big for demo replay"
  if the asset is larger.
- [V] Shapes: 7 × 9,986 bytes (0x2702), 15 × 4,156 (0x103C), 1 × 6,204 (0x183C), 1 × 6,184 (0x1828).
- [V] (DM-052) Each record is the save area of a `bmxreplay` buffer (from header +0x0C on): 0x34 header
  bytes, the input event stream, then the snapshot blocks. 7 records are the PC recorder's (0x2702 bytes:
  0x1000 stream + 2 x 0xB40 prop state + 2 x 0x27 goal info), 16 a console recorder's (0x3C header), 1 is
  unknown. The stream is 3-byte events `kind << 5 | delta, u16` (buttons XOR mask / stick bytes).
  Full description: docs/formats/REPLAY.md; reader/writer `dmbmx.formats.replay`.

## 6. Open questions

1. ~~Which motion id belongs to which trick~~: answered by DM-046, section 7; the riding states,
   manual, fast plant and wall tap by DM-047, section 8.1.
2. Arm pose: explained by DM-045 and applied in dmbmx by DM-019 (§3, §3.1). Open: an in-game screenshot
   to confirm the knees-out leg stance that F produces (feet ~0.09 off the pedal frames).
3. ~~Record `value` (f32) and `h` (u16), section flags 0x80, header bits 0-3 / 8-14 (0x9823)~~: answered by
   DM-053 (section 2.1): none of them is read by the PC build; they carry no meaning for the game.
4. ~~Root-motion scale~~: answered by DM-047 (section 8.3): engine (ZGE) units = 100 x model units,
   axes F = diag(1, -1, -1). Still open: the constant rider-root offset (-0.25 in y) is only fitted.
5. ~~replay.zal input-stream encoding~~: answered by DM-052 (docs/formats/REPLAY.md). Still open there: the
   console-layout records' stream semantics.
6. Wheel spin (DM-047 section 8.5): the spin velocity comes from physics.c (DM-048); the factor from
   wheel speed to spin is not decoded yet.

## 7. Trick → motion map (DM-046)

[V] The rider code (`rider.c` 0x004348xx-0x00435xxx) requests a motion by writing its number to
`BmxRider` +0xB9C; the number indexes `g_motionTable` (0x006962FC, filled 1-based from the bank, so
it is the motion id of section 1). For a trick it comes from two u16 tables and two formulas
(`tools/dmbmx/formats/tricks.py`, checked against the exe by `tests/test_decomp_dm046.py`):

- air trick t (ids 0..26): `g_trickMotion[t]` (0x00558FF8) = start, +1 = hold, +2 = release
  (roles [I], from the rider state that requests them). The flip variants reuse the base trick's clip (e.g. turndown backflip = 25,
  like turndown; double/triple backflip = 34 like backflip); the flip itself is the engine's rotation
  (`g_trickFlags` 0x00558FB4 bits 0-2: 1 back, 2 double back, 3 triple back, 4 front, 5 double front);
- modifier m (0..37, name `g_trickNames[46 + m]`): `g_modMotion[m]` (0x00559030) = 73 + 3m, +1, +2;
- grind / stall t (0x1B..0x29): 3t + 0x6A enter, 3t + 0x6B hold, 3t + 0x6C exit (187..231);
- manual / nose wheelie (0x2C/0x2D): 19 enter (20 when the take-off clip 15 is playing), 20 hold, 21
  exit [V] (DM-047, `riderManual`); fast plant / wall tap (0x2A/0x2B) go through the stall state with the
  grind formula: 232-234 and 235-237 [V] (DM-047, `riderStall`).

Which trick the input selects: section 8.2 (DM-047).

| Id | Name (g_trickNames) | Kind | Motions (start, hold, end) |
|---|---|---|---|
| 0 (0x00) | turndown | air | 25, 26, 27 |
| 1 (0x01) | can-can | air | 28, 29, 30 |
| 2 (0x02) | tabletop | air | 31, 32, 33 |
| 3 (0x03) | backflip | air | 34, 35, 36 |
| 4 (0x04) | superman | air | 37, 38, 39 |
| 5 (0x05) | candy bar | air | 40, 41, 42 |
| 6 (0x06) | tailwhip | air | 43, 44, 45 |
| 7 (0x07) | tobaggan | air | 67, 68, 69 |
| 8 (0x08) | double backflip | air | 34, 35, 36 |
| 9 (0x09) | triple backflip | air | 34, 35, 36 |
| 10 (0x0A) | front flip | air | 46, 47, 48 |
| 11 (0x0B) | double front flip | air | 46, 47, 48 |
| 12 (0x0C) | turndown backflip | air | 25, 26, 27 |
| 13 (0x0D) | can-can backflip | air | 28, 29, 30 |
| 14 (0x0E) | tabletop backflip | air | 31, 32, 33 |
| 15 (0x0F) | tobaggan backflip | air | 67, 68, 69 |
| 16 (0x10) | superflip | air | 37, 38, 39 |
| 17 (0x11) | candy bar backflip | air | 40, 41, 42 |
| 18 (0x12) | tailwhip backflip | air | 43, 44, 45 |
| 19 (0x13) | decade backflip | air | 49, 50, 51 |
| 20 (0x14) | decade | air | 49, 50, 51 |
| 21 (0x15) | no footed can can | air | 52, 53, 54 |
| 22 (0x16) | un-turndown | air | 55, 56, 57 |
| 23 (0x17) | double tailwhip | air | 58, 59, 60 |
| 24 (0x18) | can-can lookdown | air | 61, 62, 63 |
| 25 (0x19) | candy bar lookdown | air | 64, 65, 66 |
| 26 (0x1A) | superman indian | air | 70, 71, 72 |
| 27 (0x1B) | double peg grind | grind | 187, 188, 189 |
| 28 (0x1C) | smith grind | grind | 190, 191, 192 |
| 29 (0x1D) | feeble grind | grind | 193, 194, 195 |
| 30 (0x1E) | toothpick grind | grind | 196, 197, 198 |
| 31 (0x1F) | icepick grind | grind | 199, 200, 201 |
| 32 (0x20) | lip slide | grind | 202, 203, 204 |
| 33 (0x21) | luc-e grind | grind | 205, 206, 207 |
| 34 (0x22) | crooked grind | grind | 208, 209, 210 |
| 35 (0x23) | sprocket grind | grind | 211, 212, 213 |
| 36 (0x24) | double peg stall | stall | 214, 215, 216 |
| 37 (0x25) | toothpick stall | stall | 217, 218, 219 |
| 38 (0x26) | icepick stall | stall | 220, 221, 222 |
| 39 (0x27) | nosepick | stall | 223, 224, 225 |
| 40 (0x28) | tail tap | stall | 226, 227, 228 |
| 41 (0x29) | disaster | stall | 229, 230, 231 |
| 42 (0x2A) | fast plant | plant | 232, 233, 234 |
| 43 (0x2B) | wall tap | plant | 235, 236, 237 |
| 44 (0x2C) | manual | manual | 19, 20, 21 |
| 45 (0x2D) | nose wheelie | manual | 19, 20, 21 |
| mod 0 | barspin | modifier | 73, 74, 75 |
| mod 1 | one footer | modifier | 76, 77, 78 |
| mod 2 | one hander | modifier | 79, 80, 81 |
| mod 3 | x-up | modifier | 82, 83, 84 |
| mod 4 | x-down | modifier | 85, 86, 87 |
| mod 5 | suicide no hander | modifier | 88, 89, 90 |
| mod 6 | seat grab | modifier | 91, 92, 93 |
| mod 7 | no footer | modifier | 94, 95, 96 |
| mod 8 | no hander | modifier | 97, 98, 99 |
| mod 9 | nothing | modifier | 100, 101, 102 |
| mod 10 | one hand one footer | modifier | 103, 104, 105 |
| mod 11 | one hand x-up | modifier | 106, 107, 108 |
| mod 12 | rocket air | modifier | 109, 110, 111 |
| mod 13 | double peg grab | modifier | 112, 113, 114 |
| mod 14 | one handed seat grab | modifier | 115, 116, 117 |
| mod 15 | no footed seat grab | modifier | 118, 119, 120 |
| mod 16 | one footed seat grab | modifier | 121, 122, 123 |
| mod 17 | double seat grab | modifier | 124, 125, 126 |
| mod 18 | switch hander | modifier | 127, 128, 129 |
| mod 19 | no footed x-up | modifier | 130, 131, 132 |
| mod 20 | one footed x-up | modifier | 133, 134, 135 |
| mod 21 | no footed one hander | modifier | 136, 137, 138 |
| mod 22 | no handed one footer | modifier | 139, 140, 141 |
| mod 23 | no handed barspin | modifier | 142, 143, 144 |
| mod 24 | barspin to barspin back | modifier | 145, 146, 147 |
| mod 25 | no footed barspin | modifier | 148, 149, 150 |
| mod 26 | one footed barspin | modifier | 151, 152, 153 |
| mod 27 | rocket x-up | modifier | 154, 155, 156 |
| mod 28 | crotch rocket | modifier | 157, 158, 159 |
| mod 29 | rocket one hander | modifier | 160, 161, 162 |
| mod 30 | rocket no hander | modifier | 163, 164, 165 |
| mod 31 | rocket barspin | modifier | 166, 167, 168 |
| mod 32 | rocket one footer | modifier | 169, 170, 171 |
| mod 33 | indian | modifier | 172, 173, 174 |
| mod 34 | switch footer | modifier | 175, 176, 177 |
| mod 35 | x-up seat grab | modifier | 178, 179, 180 |
| mod 36 | barspin seat grab | modifier | 181, 182, 183 |
| mod 37 | busdriver | modifier | 184, 185, 186 |

### 7.1 How the game shows trick names (DM-054)

[V] (overlay.c, `tests/test_decomp_dm054.py`) The names in the table above are `g_trickNames`
(0x00559390). The **TRICK LIST** screen prints them as they are (tiny font, coloured by kind) with the
input underneath as button-font glyphs (texts 0x10BB + trick, 0x10E9 + modifier, e.g. "x C" = down +
modifier button); a trick or modifier with flag 0x10 in `g_trickFlags` / `g_modFlags` (a rider's
signature trick) is listed only once the rider has landed it (career bits `SaveRiderRecord.tricks_landed`).

The **HUD** (item 4) names each trick of the current set with `overlayTrickName` (0x0041EAB0) from its
`BmxTrickRec`: words joined by spaces,

    [spin: "180" "360" ... "1260" "extreme" (g_trickSpinNames[half spins], 0x005594E0)
     | "half-cab" (+ the spin when more than 180), trick flag 4]
    ["opposite" (flag 1)] ["alley-oop" (flag 2)]
    ["double" / "triple" / "quad" / "sick" for 2 / 3 / 4 / 5+ stacked modifiers] [modifier name]
    [trick name | " spin" when there is neither trick nor modifier]

and a few combinations replace the whole name: backflip with one half spin = "<modifiers> flair",
front flip with two = "<modifiers> flail", backflip + barspin = "flip driver" / "limo driver" /
"triple flip driver" / "vette driver" (by modifier count), a barspin with a spin and no trick =
"<spin> truck driver", superman + seat grab = "superman seat grab", can-can + one footer = "no footer
can-can". The opposite / alley-oop / half-cab words only appear in the HUD (`full` = 1); a name with a
modifier is shown in upper case in another palette, and the B-M-X target list (overlymp.c) uses the
short form (`full` = 0).

## 8. The rider state machine, trick selection, root motion and wheels (DM-047)

Source: `decomp/src/zaxis/Bmx/Src/com/action/rider.c` (34 functions, decompiled by DM-047), types in
`decomp/include/rider.h`; Python port of the selection `tools/dmbmx/formats/tricks.py`; checks
`tests/test_decomp_dm047.py` (tables and immediates read back from the exe).

### 8.1 Which motion plays in which state

[V] Every frame `riderUpdateAll` 0x00431000 runs a three-level state machine (`BmxRider.state[0..2]`,
procedure tables 0x005588B8..0x00558903) and then steps the clip in `motion[0].id`. The procedures
request clips by number. These are all the constant ids (`mov word [r + 0xB9C], imm` in rider.c, plus
the computed manual entry and the idle clip passed in a register):

| Motion | State (procedure) | When |
|---:|---|---|
| 2 | riding / coast (`riderRideCoast`) | standing still on level ground; loops; after 30 s the idle flag |
| 5 | riding / coast; landing (`riderStateLanding`) | rolling; the 7-frame landing |
| 8 | riding / pedal (`riderRidePedal`) | pedalling; rate = max(1, speed x 0.05) |
| 10, 11 | riding / skid (`riderRideSkid`) | stick pulled back > 0.95 while upright: start, hold |
| 13, 14 | riding / crouch (`riderRideCrouch`) | jump button held: crouch, hold |
| 15 | take-off | jump released (riding, turnaround, manual): the hop |
| 17, 18 | riding / turnaround (`riderRideTurnaround`) | physics asks for a 180; 18 = turnaround button held (roll out backwards) |
| 19, 20, 21 | manual (`riderManual`) | enter (20 instead of 19 if clip 15 is playing), hold, exit |
| 22 | landing | landing variant (rider flag 0x10000) |
| 23 | air | left the ground without a trick; after a trick or modifier ends |
| 24 | air / idle (`riderAirIdle`) | trick button without a direction; air idle loop |
| 25..72 | air / trick (`riderAirTrick`) | `g_trickMotion[t]` start, +1 hold, +2 release (section 7) |
| 73..186 | air / modifier | `g_modMotion[m]` (+0/+1/+2), on its own (`riderAirModifier`) or blended over a trick |
| 187..231 | grind (`riderGrind`), stall (`riderStall`) | 3t + 0x6A enter, +1 hold (also the blend target), (t + 0x24) x 3 exit when leaving |
| 232..237 | stall | fast plant (232-234), wall tap (235-237) |
| 4 | - | default of the second slot `motion[1]` (`riderSetState`), sampled only by physics 0x0042B5E0 |

[V] Motions **1, 3, 6, 7, 9, 12, 16 and 238 are never requested**: rider.c and physics.c are the only
users of `g_motionTable`, and only rider.c writes the motion slots. The crash state (6) plays no clip:
the rag doll (`dyn.c`, DM-058; docs/re/physics.md 7) poses the rider.

The procedures (state[0] / state[1]): riding = coast, pedal, skid, crouch, turnaround, (5 empty);
air = idle, trick, modifier-only trick; grind; stall (incl. plants); manual; landing; crash = start,
rag doll, steer, settle, splash (water surface 13), wait. A procedure returns 0 after a state change and
is called again in the same frame. The physics contact state (`physics_00420c90`: 0 ground, 1 air,
2 crash, 3/4 grind enter/on, 5/6 stall enter/on) drives the state[0] changes [I names].

### 8.2 Trick selection

[V] (`riderUpdateAll`, `riderAirTrick`, `riderAirModifier`, `riderResetDirections`, `riderGrindTrick` ...)

- **Directions.** The d-pad bits (1 up, 2 down, 4 left, 8 right) and the stick beyond 0.5 (y < 0 = up)
  form a 4-bit mask. `g_dirFromMask` 0x00558904 turns it into 1 left, 2 right, 3 up, 4 down,
  5 up-left, 6 up-right, 7 down-left, 8 down-right. While the rider records (air, rider flag 0x10),
  each newly pushed direction is appended to a 4-entry buffer (`dirBuf`; the oldest drops out). A
  direction that only gains a second axis (left -> up-left) replaces the last entry. Taking off resets
  the buffer and records the direction already held.
- **Air trick** (trick button pressed, or pressed up to 8 frames before the first direction):
  1. the rider's **signature combos** `g_signatureTricks` 0x00553840 (12 riders x 4 x {length,
     dir[3], trick, modifier}; two per rider, all 3 directions long; the modifier comes with it):
     the last `length` directions must match;
  2. else the **last two** directions, `g_airTrickDir2` 0x00558B00 [previous - 1][last - 1] (0xFF none).
     Examples: down, down = double backflip; up, up = front flip; any direction then down (or down then
     it) = that trick + backflip; left, left = un-turndown; right, left = decade; down-right twice =
     double tailwhip; up-right twice = superman indian;
  3. else the **last** direction, `g_airTrickDir1` 0x00558AF7: left turndown, right can-can, up
     tabletop, down backflip, up-left tobaggan, up-right superman, down-left candy bar, down-right
     tailwhip. No direction: no trick (motion 24).
- **Modifier** (modifier button during a trick, or with no trick for a modifier on its own): the last
  two directions `g_modDir2` 0x00558B4C, else the last one `g_modDir1` 0x00558B40 (left one footer,
  right one hander, up x-up, down barspin, up-left rocket air, up-right seat grab, down-left no
  footer, down-right no hander), then `g_trickModTable` 0x00558B8C [trick 0..26][modifier 0..37]
  (27 x 38 bytes) filters it. The table is mostly the identity; 0xFF refuses (e.g. no rocket airs on a
  can-can), and some entries substitute (can-can + rocket air -> rocket one footer). The barspin
  (modifier 0) counts once more every 16 frames it is held (`modCount`).
- **Hold / release.** The trick plays its start clip, then the hold clip while the trick button is
  held, then the release clip. `g_trickFlags` bit 3 (whips, decades) plays the hold clip exactly once.
  Flips (`g_trickFlags` & 7) rotate `flip` with an acceleration of 3 x stats[2] deg/frame up to
  12 x stats[2] (front flips: -3/4 of that) until 300 / 660 / 1020 degrees (back) or 330 / 690 (front),
  then release.
- **Spin.** Spin buttons: 8 deg/frame, then x1.25 per frame up to 12 x stats[2]. The stick alone:
  0.2 deg/frame nudges that build up the same way. Nothing held: the rate halves every frame.
- **Grind / stall / plant** tricks come from the last buffered direction (the stick when the buffer is
  empty): `g_grindTrickDir` 0x00558F90 (none double peg, left smith, right feeble, up toothpick, down
  icepick, up-left lip slide, up-right luc-e, down-left crooked, down-right sprocket),
  `g_stallTrickDir` 0x00558F9C (none double peg stall, left/right disaster, up toothpick stall, down
  icepick stall, up-left/up-right nosepick, down-left/down-right tail tap), `g_plantTrickDir`
  0x00558FA8 (plant button: left fast plant, right wall tap).
- **Manual**: landing with down, down buffered and the stick pulled back -> manual (flag 0x100000);
  up, up and the stick pushed forward -> nose wheelie (0x200000); only where physics allows it
  (flag 0x400000). The trick id is 0x2C while the wheel record at +0xC88 (hub x = +55.25) is on the
  ground, else 0x2D [V code; which name fits which wheel is not checked in the game].

### 8.3 Root motion and its scale

- [V] `riderUpdateAll` accumulates the rider section's translation (`zactorPlayMotion` 0x004740C0): the
  per-frame x and z deltas are summed into `BmxRider.rootMotion` (+0xBFC), and rootMotion.y =
  -(the clip's height, decoder +0x100). A clip started with motion flag 2 clears it. Physics
  (`physics_0042C7E0`) transforms it by the rider actor's matrix into a world point (+0x430), and
  `NgIFzactor_ApplyMotionPose` puts the rider model at the actor position + (0, height, 0).
- [V] The bike section's translation is the bike's placement relative to the rider: `zactorGetBikeOffset` 0x00474DE0
  returns it (rotated by the heading) together with the bike root angles, and physics.c places the
  rider actor at the bike position minus that offset.
- [V] Units: these are engine (ZGE) units, the units of every BmxRider position: **ZGE = 100 x
  model (RW) units with Y and Z negated** (`zrwZgl(1, 100.0)`, LEVEL.md section 2; the same F as the
  rotations). Cross-check [V]: the bike's wheel hubs in BmxRider (x +55.25 / -36.52) are 100 x the
  DMbike01 axle frames along the bike (+0.539 / -0.364 model units).
- [V] Fit against the feet-on-pedals placement (all 1,246 on-pedal frames of the bank, DaveMirra1 on
  DMbike01): rider root - bike root = F(-b) / 100 + (0.007, -0.251, 0.044) leaves a median residual of
  0.054. Scales 1/200 and 1/50 give 0.076 and 0.28, so DM-016's 1/200 guess is out. With the constant
  calibrated per clip the median is 0.023. `dmbmx.convert.combo` now uses this for the frames where
  the feet leave the pedals (`fill="engine"`, export.md section 7).

### 8.4 Mirroring (motion flag 8)

[V] Motion flag 8 (`MOTION_MIRROR`) is set only by the grind / stall procedures, when physics has set
rider flag 0x80000 (the rail or lip is on the other side). Then the enter and hold clips are mirrored
(left/right channels swapped, y/z negated, section 3). Starting an air trick or a modifier clears it;
everything else plays unmirrored. The other motion flags: 2 = restart (and clear the root motion),
0x80 = restart without the decoder's crossfade (riderUpdateAll always sets it while stepping),
0x100 = frozen, 0x200 = blend motion[0] and motion[1] by motion[1].rate (set by physics).

### 8.5 Wheels

[V] The clips do not spin the wheels. Physics (`physics_0041FC40`, DM-048's module) keeps one record
per wheel (front +0xC88, rear +0xD0C, stride 0x84): angle (s16 at +0x6A) += spin velocity (+0x6C)
every frame. It writes the angles into the bike actor's BackWheel / FrontWheel channels (rear ->
BackWheel, front -> FrontWheel, Euler x). The cheat "Exorcist" (cheat 3) copies the front wheel's angle
to the rider's head. The spin velocity is zeroed while skidding (0x00426F67) and scaled by 0.99 in
some contact states; [I] otherwise it follows the wheel speed (radius 24.645 engine units). The exact
speed-to-spin factor is in physics.c (open question 6). The crank (Sprocket channel) is pure animation,
so DM-018's crank x 2.75 stand-in stays a heuristic.

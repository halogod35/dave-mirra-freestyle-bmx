# The NgIF layer (Bmx/src/com/zge/NgIF)

C: `decomp/src/zaxis/Bmx/Src/com/zge/NgIF/*.c`; types and prototypes `decomp/include/ngif.h` (plus
`physics.h` NgColFace / NgColEdge / NgColHit and `bmxprop.h` NgPropCol, completed here). Evidence tags
as in AD-5; the checks are `tests/test_decomp_ngif.py`.

NgIF ("NextGen InterFace", the version string is "NextGen BMX : Nov 22 2000,15:54:17") is the game's
implementation of the ZGE engine API on top of the PC engine (NgEngine / ZglRw). The game code was
written against the PlayStation ZGE; on the PC most of that API is either re-implemented here or a
stub that prints `"<file> <line> <name>"` through the (silent) debug printf.

## 1. Files and boundaries

| File | VA range | Functions | Evidence | What |
|---|---|---|---|---|
| MissingZGE.c | 0x0045A1A0-0x0045A322 | 8 | [V] path 0x0055BAC4 (7 stubs) | zconsoleLog, zglEnable/DisableHorizon, zsystemSetActionMode / MenuMode (switch `g_ngappMode`), zfxDestroyAll, zfxRegisterFuncTable; [I] the empty hook 0x0045A1A0 |
| NgApp.c | 0x0045A340-0x0045C93C | 28 | [V] path 0x0055BFD0, App_Start / App_Frame call ngappInit / ngappFrame | application hooks, level load / unload, lights, loading bar, adaptive far clip, debug tag view |
| NgIFfx.c | 0x0045C950-0x0045FDEB | 31 | [V] path 0x0055C634 (zfxCreate, line 0x682); [V] the type tables 0x0055C278.. | zfx particle types, spawners, animated textures, camera shake |
| NgIFpsx.c | 0x0045FE00-0x004646D6 | 39 | [V] path 0x0055C6A0 (DrawPrim messages, asserts) | PlayStation GPU primitive emulation |
| NgIFzactor.c | 0x004646F0-0x00465A23 | 12 | [V] path 0x0055CA78 | actor <-> model: channels, arm table, part positions |
| NgIFzbsp.c | 0x00465A50-0x0046951F | 137 | [V] path 0x0055CB34 (113 stubs, 6 asserts) | prop collision records, tag face / edge queries, sphere query, BSP stubs |
| NgIFzcollide.c | 0x00469530-0x0046AC85 | 17 | [V] path 0x0055D550 | segment test, prop shapes, prop rails |
| NgIfzmemDMM.c | 0x0046ACA0-0x0046B793 | 11 | [V] path 0x0055D674 | + zmemDMMStats 0x0046B730 |
| NgIFzres.c | 0x0046B7C0-0x0046C207 | 19 | [V] path 0x0055DA8C | asset libraries, map / player tables, overlay images |
| NgIFzroller.c | 0x0046C220-0x0046D923 | 92 | [V] path 0x0055E0B4 | level runner: actors, players, rails, start points, draw callbacks, stubs, memory-card stubs |

The NgIF pass absorbed eight gap files (zoverii/MissingZGE, MissingZGE/NgApp, NgApp/NgIFfx, NgIFfx/NgIFpsx,
NgIFpsx/NgIFzactor, NgIFzactor/NgIFzbsp, NgIFzres/NgIFzroller, NgIFzroller/winChan) and created **229
functions** Ghidra had not made (decomp/symbols/symbols.csv rows): 169 printing stubs (NgIFzbsp 113,
NgIFzcollide 8, NgIFzroller 48), 15 silent stubs, the zfx draw / update functions and spawners
(reached only through the type tables and zfxRegisterSpawner), the loading bar, the debug tag view,
zmemDMMStats, and a few unreferenced helpers. The three functions after zresFreeImages that the
module map had put in NgIFzres.c print "ZROLLER: Failed to intialize ZRES" and use NgIFzroller's
globals: they are zrollerStartup / zrollerInit / zrollerShutdown [I].

**The 17 PSX-compat stubs at 0x0046D720-0x0046D930** [V code, I file]: 17 functions 0x20 apart, each
`ret` or `xor eax, eax; ret`, linked between NgIFzroller.c's last [V] function (zrollerAddDrawCallback
0x0046D690) and winChan.c, with no path string. NgIFzroller.c already has silent stubs of the same kind
inside its range (0x0046D350, 0x0046D3A0, 0x0046D3C0, 0x0046D6E0, 0x0046D700), so they are attributed
to **NgIFzroller.c**. Five are called, all by the memory-card dialogues (overlay.c's save page,
frontend.c's load / save / format pages): `zcardSelectPort(slot)` 0x0046D720, `zcardCheck(callback)`
0x0046D7A0 (the callback never comes, so the page times out), `zcardFormat` 0x0046D8A0 (returns 0 =
formatted), `zcardStart` 0x0046D8C0 / `zcardStop` 0x0046D8E0 (around every memory-card page); the other twelve
are unreferenced (`zcardStub1`..`zcardStub16`). The PC save path itself is memcard.c (SAVE.md).

## 2. Prop collision (NgIFzbsp.c, NgIFzcollide.c)

**NgPropCol** (zprop user data, 500 bytes, `zbspPropColCreate` on first use, freed by
`zbspPropColFree`, the zprop's user destructor) [V]:

| Offset | Field | |
|---|---|---|
| +0x000 | defIndex | the def's position in zpropGetDefList (`zbspGetPropDefIndex`) |
| +0x002 | flags | below |
| +0x004 | rot[3] | Euler angles (zquatToEuler of the scene object) |
| +0x00A | pos[3] | whole units |
| +0x010 | scale[3] | 1.0 |
| +0x01C | unk_1c[2] | zeroed |
| +0x024 | rails | NgColEdge[4] (box) or [1] (rail / edge shape), `zcolCreatePropRails` |
| +0x028 | 0x1C4 bytes | zeroed; nothing in the PC code reads or writes them [V: the NgIF layer and every caller use only the fields in this table; I: PSX leftovers] |
| +0x1EC | game | BmxProp * (prop.c) |
| +0x1F0 | prop | the Zprop |

**Flags** [V: every read of +2 / +3 in the game code, by a scan of all functions]:

| Bit | Name | Set by | Read by |
|---|---|---|---|
| 0x001F | group | OBJI byte 7 & 0x1F | game.c (prop groups) |
| 0x0040 | MOVING | zbspPropSetMode 2 (model flag 8) / 3 clears | rail queries skip; rotation |
| **0x0080** | **OFF ("knocked")** | zbspPropSetMode 0: **physics.c, when a rider crashes hard into the prop** (wall impact above the crash threshold, `knock_prop`) | `zbspFindEdgesNear` (grind edge search) and the debug view skip the prop's rails; `zbspPropSetRotation` stops moving the rails of a prop that is both MOVING and OFF. **Never cleared** (not by replays or bmxpropReset): after a hard crash into a prop, its grind rails are gone until the level is reloaded [V code path, I gameplay] |
| 0x0100 | FLAG100 | zbspPropSetMode 1 (no caller) | rail queries skip |
| 0x0200 | HIDDEN | zbspPropSetMode 4 / 5 clears | rail queries skip |
| **0x0400** | DRIVEN | OBJI byte 6 bit 0; bmxpropInit sets it on table-slot props | **nothing** |
| **0x0800** | OBJI4 | OBJI byte 4 bit 0 | **nothing** |
| **0x1000** | OBJI5 | OBJI byte 5 bit 0 | **nothing** |

So the prop/replay/fx pass's open items: 0x80 = knocked / rails off (above), 0x400 / 0x800 / 0x1000 are written but have
no reader in this build (all 0 in the corpus OBJI anyway).

**Shapes** (`NgPropShape`, 0x38, `ZpropDefInfo.collision`, `zbspInitPropDefShape`): OBJI byte 0x16 =
file class 1..4 -> shape class 2 (box: 4 top rails, both ends open), 3 (one rail from the middle of
corner edge 0-1 to the middle of 4-5; the exe mixes the corners' x / y with the middle z), 1 ("Not
coded" assert), 4 (one rail on corner edge 0-1, low end open); size = extents x (50, 100, 50), raised
by (extents.y / 2 + extra[1] + extra[3]) x 100; with extra[3] > 0 the +x side's top is lowered by
extra[3] x 10000 (a ramp). Rails exist only for props whose def's OBJI byte 0 bit 0 is set
(`zrollerLoadAssets`); they follow the prop (`zcolPropMoved` / `zcolPropRotated`). The shape's byte +2
(rail kind / grind sound) is 0 at creation and nothing sets it on the PC.

**Queries** (physics.md 2): `zcolSegmentTest` (0x00469B00) = RW line test both ways, world faces get the
nearest Tag._sd face's code / flags (class 1 without `extra` keeps only the id; flags bit0 -> 0x10,
bit1 -> 0x08, bit2 -> 0x20, bit3 -> 0x40); **a prop whose OBJI u16 at +8 (switch id) is non-zero answers
as a tagged face whose code is that id** (the class-9 trigger path of prop.c's breakables); other props
are type 2. `zbspColSphere` (0x00468070) = RW sphere test with the same tagging and the nearest point
per triangle (projected centre if inside, else a corner, else an edge point). `zbspFindEdgesNear`,
`zbspGetTaggedEdge` (n-th edge with a code), `zbspFindTaggedFaces` / `zbspFindTaggedEdges` (tag id in a
range), `zbspFindPropsByGroup` (OBJI byte 7 in a range). Faces of the queries are NgColFace records
(physics.h, now complete: integer corner pointers +0x1C, the ZtagFace +0x28, the prop +0x2C, float
corners +0x30.., integer corners +0x60, the RW triangle +0x84); Tag._sd faces cache theirs in
`ZtagFace.runtime[3]` and their edges in `runtime[0..2]`.

**Spine faces**: at level load `ngappLinkSpineFaces` stores, in each tagged face record (+8), the first
face (from itself on) that has an edge whose code equals the face's id - the coping edge of class 2/3
faces [V: test port, 1,591 + 106 faces on the 12 levels all find one].

## 3. Actors (NgIFzactor.c)

Rider channels (15) and bike channels (10) bind to model frames by zHashName of the names (ANIM.md 3);
mirror partners: rider (0,1,2,6,7,8,3,4,5,12,13,14,9,10,11), bike swaps only the pedals. The arm
table [V: the stack stores of NgIFzactor_ApplyMotionPose]: Cz(+90), Cz(-90), Cz(-90), Cz(+90);
LeftUpArm q Cz(+90), LeftLowArm / LeftHand Cz(-90) q Cz(+90), RightUpArm q Cz(-90), RightLowArm /
RightHand Cz(+90) q Cz(-90) = `dmbmx.formats.motion.ARM_FIX`. The models are then placed at the actor's
position + the clip height and turned by (x, -y, -z). Part positions for physics / IK
(`zactorGetAbsolutePartPosition`) turn the local offset a quarter turn for the arm parts.

## 4. Effects (NgIFfx.c)

Seven zfx particle types on the engine emitter (vtable 0x38: destroy, addAffector +8, create +0x10,
spawn +0x18, update +0x1C, draw +0x20): spark (streaks, or lines without a material), shard (spinning
triangles), splash (billboards), dust (billboards cycling DUST01..16), poster (static quads: posters,
skid marks), ripple (growing rings, own update), lightning (jittered streak chains). Particles are 0xA0
records (`ZfxParticle`), lifetimes in ms with the fade over the last 0.25-2 s. Spawners by name:
spark, shard, splash, dust, poster, skid (poster with the SKID or RIPPLE material), ripple (three
rings), lightning. `zfxFlash` (fx.c's "flash") is a camera shake: byte pairs x 0.005, -128 ends.
Animated textures: props of the def "ANIM" rotate the textures of FAN1..FAN3 at 30 per second.

## 5. PSX emulation (NgIFpsx.c)

libgpu packets (POLY_F3 / F4 / G4 / FT4 / GT4, LINE_F2 / G2, a PC clip packet 0xF8 and DR_TPAGE rate
packets) in an ordering table (ClearOTagR / DrawOTag), each with a PC "mode" word (3-D float
positions, 512-wide coordinates, alpha, depth) and drawn as ZGL quads / triangles / lines. Two users:
the overlay and debug layer (zoverii.c, overlay.c, physics / game debug draws) into OT[0], and the
menu 2-D layer (zprim.c) into a descending OT pointer, y offset 104. Not coded (print only): FT3, G3,
GT3, LINE_F3/F4/G3/G4, SPRT, SPRT8, SPRT16.

## 6. Application (NgApp.c) and resources (NgIFzres.c)

`ngappInit` (App_Start): window title, 4 MB static memory, the ACCLAIM / ZAXIS / INTRO movies (an input device is open meanwhile; [I] a key
skips), sound, actors, the game, the front end. `ngappFrame` (App_Frame): the LEGAL / LEGAL2 screens on
the first frame; then either the front end (with a restart after the front end's idle request) or the
game; the menu -> game switch loads the level behind the loading bar (intwrap.zal group 2, 3 in attract
mode), the "Globals" scene point's OBJI as the fog colour, lights from the SpotLight / SpotLightT
points, prop tints (OBJI 0..3), light props; the game -> menu switch unloads it. The far clip adapts
between 60 and 200 to keep ~26-28 fps. The ambient light follows the prelit colour of the ground under
the rider every third frame.

zres: MiscData / Tables / Replay / Material .ZAL; the map table (Tables.ZAL asset 0: FORM { ZMHD, ZMAP
x 100 bytes }) and the player table (MiscData.ZAL group 1: FORM { PLHD, PLYR x 0x18 }); 11 overlay
pictures (Material.ZAL assets 9..0x13) made into materials "Overlay<n>" in the PSX image slots.

## 7. Open questions

1. The NgPropCol bytes +0x1C..+0x23 and +0x28..+0x1EB: no reader found in the PC build.
2. NgPropShape +0x02 (rail kind): never set on the PC, so prop rails always use grind kind 0.
3. Play-test (unverified): that a hard crash into a prop removes its grind rails for the session.

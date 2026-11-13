# Props, replays and effects (prop.c, replay.c, fx.c)

C: `decomp/src/zaxis/Bmx/Src/com/action/{prop,replay,fx}.c`; types `decomp/include/{bmxprop,replay,fx}.h`.
Replay format: `docs/formats/REPLAY.md`. Evidence tags as in AD-5; the tests are `tests/test_decomp_prop_replay_fx.py`.

## 1. Module boundaries

| File | VA range | Functions | Evidence |
|---|---|---|---|
| fx.c | 0x00408960-0x00409980 | 21 | [V] path 0x00554C2C (asserts of fxPsxCreateAt / fxPsxCreate, lines 0x42F / 0x44C); [I] the rest uses only fx.c's data 0x00554718-0x00554C64 (spawner names, PSX blocks, the zfxRegisterFuncTable table) |
| prop.c | 0x0042DE70-0x0042FCA1 | 18 | [V] path 0x00558678 (bmxpropInit, "state info overflow"), "bmxprop..." messages |
| replay.c | 0x0042FCE0-0x00430E50 | 13 | [V] path 0x005587E4 (9 functions), "bmxreplay..." messages |

The prop/replay/fx pass absorbed the gaps contrler/fx (9), fx/game (2) and prop/replay (1), and created 12 functions
Ghidra had not made (decomp/symbols/symbols.csv rows): fx.c 0x00408960, 0x00408980, 0x00409880
(zfxRegisterFuncTable entries), 0x00408A40, 0x00408A60, 0x00408C50 (only ILT thunks), 0x004098B0,
0x004098F0 (hooks stored by fxInit); prop.c 0x0042EB50 (draw callback); replay.c 0x00430CF0
(bmxreplayGetFrame), 0x00430E30, 0x00430E50 (empty, unreferenced; [I] could be rider.c's).
Also named (no source file): zfxGetSystem 0x0045F480, zfxInitTypes 0x0045F780, zfxShutdownTypes
0x0045FC10, zfxFlash 0x0045FDC0, zbspPropSetRotation / AddRotation / SetPosition 0x004661C0 /
0x00466230 / 0x004662B0, g_zfxEventHook / g_zfxSpawnHook 0x005F34B4 / B8.

## 2. Props (prop.c) [V unless marked]

Three things are driven, all keyed on ids in the props' OBJI blocks (LEVEL.md 5.4):

1. **Table slots.** `g_bmxpropTable[14][16]` 0x00557D28 (`BmxPropDef`, 10 bytes: id, type, motion,
   link[2], trigger sound, loop sound). bmxpropInit binds each slot of the current level:
   - types 0..3 to the first prop whose **OBJI byte 7** equals the id (zbspFindPropsByGroup; a second
     one prints "extra prop with tag"); the slot gets the prop's collision record (`NgPropCol`,
     NgIFzbsp's 500-byte record), position, orientation, and the record's flag 0x400 + back pointer
     (+0x1EC) so physics.c can find it;
   - types 4 / 5 to the first prop whose **OBJI u16 at +8** equals the id.
   Missing props only print "error finding prop / face with tag".
2. **Breakable props** (`BmxPropSwitch`, up to 64): every prop with a non-zero OBJI u16 id.
   physics.c reports class-9 trigger faces (tag code = id) to `bmxpropHitTrigger`: the prop `id` is
   hidden, `id + 1` shown, 32 glass shards fly with the rider's velocity, sound 0xFA2, and
   gameOnObjectHit marks GOAL_OBJ_HIT_OBJECT objectives. The shipped levels pair them as
   whole / broken windows (Greenville Street 37864..37871) and fences (ids 1000/1001, 1020/1021,
   1080/1081, 1090/1091). No class-9 face ships (physics.md), so in the PC data only the fence pairs
   switch, through the table (type 5).
3. **The state block** `g_bmxprops` 0x00694460 (`BmxPropState` 0xB40 = 16 x 0x84 + 64 x 0xC), a
   copy after init `g_bmxpropsStart` 0x0056C4F8 (bmxpropReset), saved and loaded by replays
   (bmxpropSetState re-applies visibility, collision, position and rotation).

Slot types: 0 mover (by itself, `motion`), 1..3 goal props (game.c: `bmxpropActivate` when a goal is
done, `bmxpropActivateNow` to re-apply done goals at a run start; touching one calls
gameOnPropTouched = GOAL_OBJ_COLLECT), 2 swings back after link[0] seconds, 3 is one-way, 4 only a
looping sound, 5 a model pair (`bmxpropShowPair`). Motions (bmxpropTrigger; `immediate` applies the
end state at once): see the BMXMOVE_* list in bmxprop.h: lifts / bobbing (+-400 / +-200 vertical),
swings about x / z, slides and travels along the prop's right axis (13..16; 16 is a three-leg move),
delays (17..20), **traffic** 8/9 (runs along -z at 40 per frame for ever, wrapping at -8200; a rider
inside its box is crashed with 1.05 x its velocity: Greenville Dirt ids 20..25 start running in
bmxpropInit) and **knock-over** 11/12 (pushed with 1.25 x the rider's sideways velocity, spinning,
the rider passes through: bmxpropTouched returns 0). Movers 13..20 activate their `link` slots when
they stop. A rider grinding / stalling on a moving prop (surface 12, or a rail of it) is carried along.

Table summary [V] (levels 0..5 have entries, also 7 and 11): Greenville Dirt 16 movers (6 traffic
20..25, 5 swings, ...), Greenville Street 4 lifts + 2 bobbing, Greenville Vert 12 knock-over props,
two travellers (20 opens the fence pair 1000) and the pair, Woodward Dirt 4 tumbling props, 4 movers,
2 sliders that open the fence pairs 1090 / 1080, Woodward Street 4 goal-3 props + 2 lifts, Woodward
Vert 8 delay slots (20 / 24 open the fence pairs 1000 / 1020); ProTour Street 3 and Acclaim Street 1
sound-only slots (ids 3000+; no prop carries them, so they print "error finding face").

### OBJI answers (the zScene pass open items; LEVEL.md 5.4)

- **Prop OBJI (10 bytes)**: bytes 0..3 = RGBA **tint**: ngappApplyPropTints (level load) forces the
  alpha to 0xFF and passes it to 0x00488CA0, which stores it at model +0x84 and applies it to the
  model's materials (callback 0x0048F530) [V call chain; visual effect I]; skipped when all four bytes
  are 0. Bytes 4, 5, 6 bit 0 -> collision record flags 0x800 / 0x1000 / 0x400 (zbspPropColCreate;
  all 0 in the corpus). **Byte 7 = group id**: bits 0-4 go to the collision record (the goal
  "prop groups" of game.c), the whole byte is the key of table slots 0..3. **u16 at +8 = switch
  id** (pairs / breakables, odd = the "after" model).
- **PropDef OBJI byte 0 bit 0 = grind rails**: zrollerLoadAssets builds rail edges from the
  def's collision box for every prop whose def has it (zcolCreatePropRails: engine class 2 -> the
  box's 4 top edges, 3 / 4 -> 1 edge, 1 -> assert "Not coded"). In the corpus 92 defs set it, all
  with file class 1, 2 or 4 (never 3 = engine 1) [V].

## 3. Replays (replay.c)

See REPLAY.md. Functions: bmxreplayInit / Shutdown, Update (record / play), Create, Destroy,
SetMode (snapshots), Get/SetPlayer, GetMode, GetFrame, GetSaveArea.

## 4. Effects (fx.c)

- PC effects go through the zfx spawner registry (zfxInitTypes registers types "lightning",
  "spark", "shard", "splash", "dust", "poster", "ripple", affectors "gravity", "floorcoll" and the
  spawners). fx.c: `fxSpark` (grinding sparks, physics.c), `fxDust` (wheels / landings), `fxShards`
  (breaking glass, 32 shards), `fxSkid` (a quad skid mark, 5 s), `fxWaterSplash` (ripple + splash,
  rider.c crashes into water), `fxCollectEffect` (a screen flash, zfxFlash; goal props, traffic hits,
  rider collisions). `fxSetRider` names the rider whose actor anchors dust and skids.
- The PlayStation path (zfxCreate with effect scripts 0x00554A9C, zfxRegisterFuncTable /
  zfxDestroyAll, the 42-point body burst) is dead on the PC: zfxCreate prints "Unsupported
  effect", the others are MissingZGE stubs. fxDestroyAll, fxDestroy, fxPuff and fxBodyBurst have no
  callers.

## 5. Open questions

1. Answered by the NgIF pass (docs/re/ngif.md 2): NgPropCol flag 0x80 = "knocked" (zbspPropSetMode 0, set by
   physics.c on a hard crash into the prop, never cleared): the prop's grind rails are skipped by
   zbspFindEdgesNear from then on [V code, I gameplay]; the OBJI flags 0x400 / 0x800 / 0x1000 are
   written but never read in this build [V: scan of all game code].
2. Play-test: the prop tint, the moving props and the attract demo were not checked in game.

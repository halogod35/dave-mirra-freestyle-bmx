# Decompilation progress (NgBMX.exe)

DM-040. Non-matching, readable C (AD-8), laid out like the original source tree. Library code
(MSVC CRT, RenderWare 3.1, libpng 1.0.5, zlib 1.1.3; see `docs/re/libraries.md`) is not decompiled.

## Layout

| Path | What |
|---|---|
| `decomp/functions.csv` | every game/engine function: `va,size,name,file,confidence,method,status,signature` |
| `decomp/src/zaxis/Bmx/Src/...` | game code (`C:\zaxis\Bmx\Src\...`) |
| `decomp/src/NgEngine/Zge/...` | engine code (`C:\NgEngine\Zge\...`) |
| `decomp/src/_unassigned/gap_<prev>__<next>.c` | functions not yet attributed; they sit between those two files in link order |
| `decomp/include/` | recovered types: `dmtypes.h` (base + Ghidra pseudo-types), `zengine.h` (zdebug + umbrella for `zmem.h`, `zlist.h`, `zstring.h`, DM-044), `rwtypes.h`, `zfile.h`, `zasset.h`, `save.h`, `rider.h`, `game.h` (DM-046), `physics.h` (DM-048), `contrler.h`, `camera.h` (DM-051), `bmxprop.h`, `replay.h`, `fx.h` (DM-052), `zscene.h`, `zactor.h`, `zprim.h` (DM-053), `zoverii.h`, `overlay.h` (DM-054), `zsound.h` (DM-055), `ngif.h` (DM-057), `zglrw.h` (DM-059: the ZglRw types, driver tables, prototypes and the RenderWare 3.1 calls), `gui.h` (DM-056), `dyn.h` (DM-058), `zobj.h`, `zpc.h`, `ztools.h` (DM-061), `zanim.h`, `zmath.h` (DM-045; included by the files that use them); umbrella `decomp.h` |
| `decomp/include/ghidra_types.txt` | which headers `tools/ghidra/ImportTypes.java` imports into Ghidra (+ size checks) |
| `tools/decomp/make_skeleton.py` | regenerates `functions.csv`, the stubs and the tables below |

- `confidence`/`method`: `V direct` = the function references the file's path string; `I` =
  inferred (`strings`, `between`, `callers`: DM-002; `seed`, `range`, `orphan`, `split`: DM-040).
  Method definitions: `tools/ghidra/build_symbols.py` and `docs/re/module-map.md`.
- `status`: `todo` (stub) → `draft` (C written) → `reviewed`. The generator rewrites a source file
  only while all its functions are `todo`, so module cards never lose work.
- Stub signatures come from Ghidra's decompiler (`DecompileAll.java`); `undefined4` & co. are
  typedef'd in `dmtypes.h`. The stubs are meant to compile with MSVC or any C89 compiler, but this
  has not been tried (no C compiler on the build PC yet).

## Workflow for a module card

1. Read the file's stubs in `decomp/src/...` and the Ghidra C in `out/ghidra_c/<file>.c`
   (`tools/ghidra/apply_symbols.sh --no-apply --no-export --decompile`).
2. Rewrite each function as readable C, keep its `/* VA 0x... */` comment, set its status in
   `decomp/functions.csv` to `draft`.
3. New names and types: rows in `decomp/symbols/symbols.csv` (AD-6), structs in `decomp/include/*.h` (listed in
   `ghidra_types.txt` with a size check). The RE owner runs `tools/ghidra/apply_symbols.sh` (imports the
   types, applies the names) and re-runs `tools/testing/py.sh tools/decomp/make_skeleton.py`.
4. Functions found to belong to a neighbouring file: move them and fix `STRING_SEEDS`/`RANGE_FILES`
   in `build_symbols.py` so the map agrees.

## Recovered structs (DM-040)

| Type | Size | Header | Used by | Tag |
|---|---|---|---|---|
| `SaveBlockA`, `SaveRiderRecord`, `SaveGoalInfo` | 0xC3C, 0x104, 0x27 | `save.h` | `g_saveBlockA` 0x006955e0 | V (SAVE.md) |
| `SaveRecordEntry` | 0x14 | `save.h` | `g_saveRecords[12][24]` 0x00551fd8 | V |
| `SaveProfile` | 0x4C | `save.h` | `g_saveProfiles[2]` 0x006952a0 | V |
| `ZassetLib` (completed DM-042), `ZalHeader` | 0x128, 0x14 | `zasset.h` | `zasset*` 0x004804d0-0x00480930, `g_materialZal` ... | V offsets, I names |
| `ZiffChunk`, `ZiffFile`, `ZiffHandler`, `ZiffGlobals` | 0x20, 0x108, 0x10, 0x1C | `ziff.h` | `ziff*` 0x0049a6d0-0x0049b2a0, `g_ziff` 0x006746d0 | V offsets, I names (DM-042) |
| `ZanimNode`, `ZanimNodeTable`, `ZanimContents`, `ZanimChannel`, `ZanimNameList` | 0x44, 0xC, 0x14, 0x10, 0xC | `zanim.h` | node tables, loaded ANIM IFF (`zanimLoadIff`) | V offsets, I names (DM-042; DM-045 completed, renamed the links) |
| `ZanimKey`, `ZanimDefName`, `ZanimTrack`, `ZanimObj` (partial), `ZanimWeight`, `ZanimWeights` | 0x14, 0x24, 0x3C, 0x34, 8, 8 | `zanim.h` | `zanim.c` 0x00498D80-0x0049A6CB, track pool 0x006746C4 | V offsets, I names (DM-045) |
| `Zquat` (+ zmath/zquat prototypes) | 0x10 | `zmath.h` | `zmath.c` 0x00480D20-0x00482081, `zquat.c` 0x00483020-0x00483A5E, tables 0x0065E2BC-0x006702D3 | V (DM-045) |
| `zfileDevice`, `zfileHandle` | 0x40, 0x8 | `zfile.h` | `zrwFileDevice` 0x00562ff8, `zfileDevice2` 0x00563240, `zfile*` | all 16 slots named (DM-041): V GetName/MakeFileName/Exists/Open..GetSize, I CheckDevice/Format/CheckFormatted/GetFreeSpace/Delete |
| `zfileCallback`, `zfileCallbackInfo`, `zfileDeviceList`, `zramdiskFile` | 8, 0x10, 0x44, 0x10 | `zfile.h` | device callbacks, `g_zfileDevices` 0x00670630, RAM-disk files | V (DM-041) |
| `RwFileFunctions` | 0x28 | `rwtypes.h` | `g_zrwFileFuncs` 0x006752c0 | V (rwfseek at +0x20 is what zrwFile_Seek calls) |
| `ZActor`, `ZActorAnim`, `ZMotionState`, `ZMotionHeader`/`ZMotionSection`, `ZactorFrame`, `ZactorBikeOffset`, `ZactorBank`, `ZactorBankEntry` | 0x278, 0xD60, 0x17C, -/4, 0x18, 0x18, 0x14, 8 | `zactor.h` | `zactor.c` (actor pool `g_zactorPool` 0x00641E1C, motion decoder, bank 0x00601DC8), NgIFzactor.c, rider.c, physics.c | V offsets, I names (DM-053) |
| `ZprimAsset`, `ZprimState`, `ZprimImage`/`ZprimFrame`, `ZprimPalette`, `ZprimFont`/`ZprimGlyph` | 0x10, 0x54, -/0x10, -, -/0xA | `zprim.h` | `zprim.c` (state stack 0x006783FC, asset pool 0x00678404, groups 0x00678418) | V offsets, I names (DM-053) |
| `BmxRider` | 0xF70 | `rider.h` | `g_riders[2]` 0x00691f40 (`rider_Create` 0x004316c0) | V offsets, I meaning; partial |
| `ZsceneDef`, `ZsceneEntry`, `ZsceneAnim`, `ZsceneLoadReq`, `ZsceneLoader`, `ZsceneKindInfo`, `ZsceneDefKey`, `ZsceneEntryKey`, `ZsceneGlobals`, `ZsceneChunkHandler` | 0x78, 0x10, 0xC, 0xC, 8, 8, 0x10, 0xC, 0x8C, 8 | `zscene.h` | `zScene.c`, `g_zscene` 0x00673f70, handler tables 0x00561900 | V offsets, I names (DM-043) |
| `ZpropDefInfo`/`ZpropDef`, `ZpropInfo`/`Zprop`, `ZpropPdinRecord`, `ZpropPrblRecord`, `ZobjiHeader` (lists: DM-044's `zList`) | 0x4C/0x54, 0x2C/0x34, 0x38, 0x48, 8 | `zprop.h` | `zprop.c`, `g_zpropDefs` 0x006722b8, `g_zprops` 0x006722c4 | V (DM-043) |
| `ZtagFace`, `ZtagNode`, `ZtagHit`, `ZtagHitList`, `ZtagVec`, `ZtagTagdRecord`, `ZtagTagfRecord`, `ZtagTagnRecord` | 0x6C, 0x34, 0x38, 8, 0x10, 8, 0x50, 0x28 | `ztaginf.h` | `ztaginf.c`, root node 0x006728c4, hits 0x00672908 | V (DM-043) |
| `ZscnPoint`, `ZscnSdptRecord`, `ZscnptsGlobals` | 0x50, 0x4C, 0x18 | `zscnpts.h` | `zscnpts.c`, `g_zscnpts` 0x00674000 | V (DM-043) |
| `RwV3d`, `RwMatrix` | 0xC, 0x40 | `rwtypes.h` | scene points | I (RW SDK layout; DM-043) |
| `ZsdScfiRecord`, `ZrwWorldApi`, `ZrwWorldStats`, `ZrwWorldGlobals` | 0x28, 0x2C, 0x1C, 0xAC | `zrwworld.h` | `zrwWorld.c`, `g_zrwWorldApi` 0x005620e0, `g_zrwWorldState` 0x006741e0 | V (DM-043) |
| `ZmemBlock`, `ZmemPoolFuncs`, `ZmemPool` | 8, 8, 0x14 | `zmem.h` | memory pools (first-fit heaps) `zmemPool*` 0x00486410-0x004868C0, `g_zmemPools[128]` 0x006718B0 | V offsets, I names (DM-044) |
| `ZmemFreeNode`, `ZmemFreeList`, `ZmemGlobals` | 8, 0x20, 0x24 | `zmem.h` | free lists `zmemFreeList*` 0x00485E80-0x004862F0, `g_zmemFreeLists[128]` 0x006708A8, `g_zmem` 0x00670880 | V offsets, I names (DM-044) |
| `ZmemDMMHandle`, `ZmemDMMFree`, `ZmemDMMGlobals` | 0x10, 0xC, 0x28 | `zmem.h` | `NgIfzmemDMM.c` movable-block heap, `g_zmemDMM` 0x005F8F30 | V offsets, I names (DM-044) |
| `zListNode`, `zList`, `zHeapList` | 8, 0xC, 0x10 | `zlist.h` | `zList.c` 0x0048C110-0x0048C5B4 | V offsets, I names (DM-044) |
| `ZstringSet`, `ZfxRegEntry` | 8, 0x48 | `zstring.h` | "STRS" text sets (`zTextGet`), zfx name registries | V offsets, I names (DM-044) |
| `BmxRider` (DM-046 fields: trick input, score block, trick log, area stats, judging, position/velocity, contacts) + `BmxTrickRec`, `BmxScoreBlock`, `BmxAreaStats`, `BmxJudgeRun`, `BmxContact` | 0xF70; 0x10, 0x80, 0xE, 0xC, 0x20C | `rider.h` | `game.c` (offsets as read/written there) | V offsets, I meaning (DM-046) |
| `BmxRider` (DM-047 fields: direction buffer, input block, state machine `state`/`stateTimer`, motion slots, root motion, steering rates, flip rotation, trick matrix, bike matrix, pedal force / wheel drag, wheel contacts) + `BmxInput`, `BmxMotionSlot`, `BmxSigCombo` | 0xF70; 0x28, 0x10, 0x6 | `rider.h` | `rider.c` (offsets checked against rider.c's code) | V offsets, I meaning (DM-047) |
| `GoalObjective`, `GoalDef`, `GoalTierDef` | 0x14, 0x54, 0x150 | `game.h` | `g_goalDefs[18]` 0x00555118 | V (DM-046; 72 goals decoded in SAVE.md 7.1) |
| `GameReward`, `GameRewardTables`, `GameLiveRecord`, `GameTrickInfo` | 2, 0xE0, 8, 4 | `game.h` | `g_rewards` 0x005568B8, `g_gameLive[4][24]` 0x0056B70C, `g_trickInfo[85]` 0x00559118 | V (DM-046) |
| `BmxRider` (DM-048 physics fields: `physState`, `physTimer` (was DM-047's snapFrames), `physHipOffset`, `physSlopeAccel`, `physLockStep`, `physSurfaceCopy`, `physLockPos`, `physLockMatrix`, `physAirMatrix`, `physAirRot` (carriedSpin = [1]); physics also uses DM-047's `rotVel`, `airLean`, `trickMatrix`, `flipVel`, `rootMotion`, `bikeMatrix`, `pedalForce`, `wheelDrag`) + `BmxWheel` | 0xF70; 0x84 | `rider.h` | `physics.c`, `rider.c` (`wheel[2]` replaces wheelFront/R/Load/Contact and wheelRear/...) | V offsets, I meaning (DM-048) |
| `NgColFace`, `NgColEdge`, `NgColHit`, `BmxProbe` | 0x88, 0x38, 0xD8, 0x30 | `physics.h` | collision results from `NgIFzcollide_00469B00` / `NgIFzbsp`, `g_bmxphysHit` 0x0056C128 | V offsets, I names (DM-048; docs/re/physics.md) |
| `BmxActionBinding`, `ZInputDevice`, `BmxPad` (+ `PAD_*` buttons, `ACT_*` actions, `CONTRLER_TEST_*`) | 8, 0x74, 0x3C | `contrler.h` | `g_contrlerActions[3][21][3]` 0x00553EE0, `g_pads[2]` 0x0056B628, zinput devices | V offsets + tables, I names (DM-051; docs/re/input-camera.md) |
| `BmxCamParams`, `BmxCamera`, `BmxCamView` (+ `CAM_MODE_*`, `CAM_VIEW_*`, `CAMF_*`) | 0x18, 0xB8, 0x68 | `camera.h` | `g_cameras[2]` 0x0056B448, parameter tables 0x00551D04-0x00551EE3 | V offsets + tables, I names (DM-051) |
| `BmxPropDef`, `BmxProp`, `BmxPropSwitch`, `BmxPropState`, `NgPropCol` (partial) | 0xA, 0x84, 0xC, 0xB40, 0x1F4 | `bmxprop.h` | `g_bmxpropTable[14][16]` 0x00557D28, `g_bmxprops` 0x00694460, `g_bmxpropsStart` 0x0056C4F8, NgIFzbsp's prop collision record | V offsets + table, I names (DM-052; docs/re/props-replay-fx.md) |
| `BmxReplayDesc`, `BmxReplay`, `BmxReplayRider` | 8, 0x40, 0x2DE8 | `replay.h` | `g_replays[5]` 0x0056D278, replay.zal records | V (DM-052; docs/formats/REPLAY.md) |
| `FxParticle`, `FxDust`, `FxShard`, `FxSkidCorner`, `FxSkidQuad`, `FxRipple`, `FxPsxEffect`, `FxPsxBurst`, `FxBodyPoint` | 0x34, 0x2C, 0x40, 0x10, 0x4C, 0x24, 0x20, 0x40, 8 | `fx.h` | zfx spawner parameters, PSX effect blocks 0x00554AB8 / 0x00554AD8 | V layouts, I names (DM-052) |
| `ZoverPrim`, `ZoverPolyF4`/`G4`/`FT4`, `ZoverTexture`, `ZoverFont`, `ZoverGlyph`, `ZoverFontHeader`/`Record`, `ZoverSprite` | 0xA0, 0x24/0x30/0x3C, 0x14, 0xC, 8, 0xC/0xD, 0x14 | `zoverii.h` | `zoverii.c` primitive pools 0x00574E70-0x005A1398, fonts miscdata.zal 0x7531-0x7535, image slots 0x0068C3E0 | V offsets + font files, I names (DM-054) |
| `GuiElement`, `GuiToggleData`, `GuiStyle`, `GuiPad`, `GuiScrnHeader`, `GuiEdef` | 0xF0, 0x24, 0x18, 0x20, 0x2C, 0xD4 | `gui.h` | menu elements `g_guiElements[100]` 0x0056D2C0, screen style 0x00573088, menu pad 0x0056D298, the SCRN / EDEF screen records of Intwrap.zal | V offsets + data, I names (DM-056; docs/re/frontend.md, ZAL.md 3.2) |
| `OverlayScreen`, `OverlayItem`, `OverlayMenuEntry`, `OverlaySavePage`, `OverlayTrickEntry`, `OverlayScoreReq`, `OverlayMeterReq`, `OverlayTimedText`, `OverlayMessage`, `OverlayFadeReq`, `OverlayFade` | 0x10, 0x18, 4, 4, 2, 8, 0xC, 4, 8, 6, 0x20 | `overlay.h` | HUD items `g_overlayItems[17]` 0x00556F00, screens 0x00557098 / `g_overlayScreens[26]` 0x005571B8 | V tables, I names (DM-054; docs/re/hud.md) |
| `ZsndVoice`, `ZsndSample`, `ZsndSampleFile`, `ZsndTrackDef`, `ZsndKeymap`/`ZsndKeymapKey`, `ZsndBankHeader`, `ZsndAsset`, `ZsndFileDesc` | 0x14, 0x18, 8, 0x10, -/2, 0x48, 0x48, 0xC | `zsound.h` | sound banks (`winLoad.c`, `g_zsndAssets[16]` 0x00600D98) | V (DM-055; AUDIO.md) |
| `ZsndcInitParams`, `ZsndcEffect`, `ZsndcChannel`, `ZsndcHandle` | 0x14, 0x14, 0x48, 2 | `zsound.h` | the mixer (`winChan.c`, `g_zsndcChannels[24]` 0x005FB770, `g_zsndcHandles[64]` 0x005FBE38) | V offsets, I names (DM-055) |
| `ZsndStreamFile`, `ZsndStreamQueue`, `ZsndStream` | 0x24, 0xC, 0x44 | `zsound.h` | streams (`winStrm.c`, 0x00601350 / 0x006019F8 / 0x006017D0) | V offsets, I names (DM-055) |
| `ZsndSeqTrack`, `ZsndSeqNote`, `ZsndSeqEvent`, `ZsndSrc` | 0x48, 0x14, 0x10, 0x28 | `zsound.h` | ZTRK sequencer (`g_zsndSeqTrackPool[32]` 0x0064F048, notes 0x0064EB38), 3-D sources (`g_zsndSrcs[24]` 0x0064FCD0) | V offsets, I names (DM-055; docs/re/sound.md) |
| `ZglVertex`, `ZglLine`/`Tri`/`Quad`, `ZfxParticle`, `ZfxSystem`, `ZfxType`, `ZfxTexAnim` | 0x1C, 0xC/0x10/0x14, 0xA0, 0x30, 0x38, 0x50 | `ngif.h` | ZGL immediate mode (NgApp, NgIFfx, NgIFpsx), zfx particle types 0x0055C278.. | V offsets, I names (DM-057; docs/re/ngif.md) |
| `PsxPolyF3`/`F4`/`G4`/`FT4`/`GT4` (+ `PsxGT4Corner`), `PsxLineF2`/`G2`, `PsxClip`, `PsxVert3D` | 0x70/0x84/0x90/0x9C/0xA4 (0xC), 0x5C/0x60, 0x18, 0x14 | `ngif.h` | NgIFpsx packets (heads = zoverii.h ZoverPoly*) | V offsets, I names (DM-057) |
| `NgPropShapeInfo`, `NgPropShape`, `ZrwColTri`, `ZrwColHit`, `ZrwColTriList`, `ZbspSphereHit`, `ZbspSphereResult` (+ `NgColFace`, `NgPropCol` completed) | 0x14, 0x38, 0x4C, 0x5C, 8, 0x1C, 0x40 | `ngif.h` (`physics.h`, `bmxprop.h`) | prop shapes, RW collision results, the sphere query | V offsets, I names (DM-057) |
| `ZresImageDef`, `ZresMapHeader`, `ZresPlayerHeader`, `ZrollerPlayerDesc`, `ZrollerPlayer`, `ZrollerShadows` | 0x10, 8, 8, 4, 0x10, 0x28 | `ngif.h` | `g_zresImages` 0x0055D978, ZMHD / PLHD, players 0x005FB6F0, `g_zrollerShadows` 0x00678440 | V offsets, I names (DM-057) |
| `DynParticleDesc`, `DynParticle`, `DynSpring`, `DynLimitDesc`, `DynLimit`, `DynBody`, `DynSlab`; `BmxIkTarget`, `BmxIk` (BmxRider +0xD98, was `camera[0x100]` + camScale..) | 0x20, 0xB4, 0x20, 0x24, 0x28, 0xE88, 0x1C; 0x20, 0x1D8 | `dyn.h`, `rider.h` | `dyn.c` crash rag doll (rider 14 particles / 21 springs / 6 limits, bike 3 / 3), `g_dynSlab` 0x0068FFC0, the limb IK | V offsets, I names (DM-058; docs/re/physics.md 7) |
| `ZObj`, `ZobjLink`, `ZobjVtbl`, `ZobjCameraData`, `ZobjPEmitData` | 0x3C, 0x10, 0x24, 0x104, 0x3C | `zobj.h` | the zobj scene nodes (`zobj.c`, pool 0x006722D4), the four type vtables 0x00560518 / 0x00560FA8 / 0x00560FD0 / 0x00561858 / 0x00562098 | V offsets + vtables, I names (DM-061; docs/re/zge.md 2) |
| `PcGuid`, `PcJoyMapHeader`, `PcInputDevice`, `PcAviOptions`, `PcMovie` | 0x10, 8, 0x14, 0x2C, 0x28C | `zpc.h` | `pcinput.c` (joystick maps 0x00561F2C), `pcmovie.c` (AVI player) | V offsets + tables, I names (DM-061; input-camera.md 2.4, zge.md 5) |
| `ZtokenFile`, `ZfontPoint`, `ZfontStroke`, `ZfontGlyph`, `Zfont`, `ZrwRamDiskFile`, `ZprofileBar`, `ZprofileLayout`, `ZprofileGlobals`, `ZheapItem`, `Zheap`, `ZtimerEvent` | 0x30C, 0x1C, 0xC, 0xC, 0x10, 0xC, 0x24, 0x30, 0x68, 8, 0x10, 0x20 | `ztools.h` | `ztoken.c` (`g_ztokenFiles[4]` 0x00675380), `zFont.c`, `zrwRamDisk.c`, `zprofile.c` (`g_zprofile` 0x00676438), `ztimer.c`, `zheap.c` | V offsets, I names (DM-061; zge.md 3-7) |
| `RwRGBA`, `RwImage` (+ `u8`...`s32`) | 4, 0x1C | `rwtypes.h` | `zrwTxr_*` (DM-014) | I (RW 3.x SDK layout) |
| `ZglTexture`, `ZrwTxrGlobals`, `ZglTextureApi` | 0x28, 0x430, 0x3C | `zglrw.h` | `zrwTxr.c`, `g_zrwTxr` 0x00674290, `g_texDriverVtbl` 0x005623F0 | V offsets + table, I names (DM-059; docs/re/zrw.md 3) |
| `ZrwMadf`, `ZglMatPass`, `ZglMaterial`, `ZrwMatGlobals`, `ZglMaterialApi` | 0x88, 0xC, 0x74, 0x24, 0x3C | `zglrw.h` | `zrwMat.c`, MADF buffer 0x00672320, `g_zrwMat` 0x006723A8, `g_zglMaterialApi` 0x00561018 | V (DM-059) |
| `ZglModelDefEntry`, `ZglModelDef`, `ZglModelPart`, `ZglModelLod`, `ZglModel`, `ZglModelApi` | 0x14, 0xBC, 0x28, 0xC, 0x8C, 0xA4 | `zglrw.h` | `zrwModel*.c`, `g_zglModelApi` 0x00560E78 (41 slots) | V offsets + table, I names (DM-059; zrw.md 2) |
| `ZglLight`, `ZglLightApi`, `ZglCamera`, `ZglCameraApi`, `ZglColApi`, `ZglPrimApi`, `ZglSystemApi` | 0xC, 0x28, 0xC8, 0x3C, 0x10, 0x1C, 0x34 | `zglrw.h` | the light / camera / collision / primitive / system driver tables 0x00563420, 0x00561D88, 0x00562060, 0x00561720, 0x00561468 | V tables, I names (DM-059) |
| `RwCollTri`, `RwIntersection`, `ZglPoly`, `ZrwDbgShape`, `ZrwShadow` | 0x28, 0x1C, 0xC, 0x6C, 0x18 | `zglrw.h` | `zrwCol.c` (RW collision callbacks), `zrwPrim.c`, `zrwDbg.c` shapes 0x00650A9C, `zrwShadow.c` | V offsets, I names (DM-059) |

All of them are in the Ghidra project (category `/dmbmx`), and with them every type in
`decomp/symbols/symbols.csv` now applies (0 type warnings, was 31).

## Proposed module order

Smallest and most useful first: the file/asset/save path that the format and editor tracks rely
on, then the engine core, then game logic, then presentation. Every card is one or a few files
from the table below; the "Functions/Bytes" columns size them.

<!-- BEGIN GENERATED by tools/decomp/make_skeleton.py; do not edit by hand -->

### Totals

- Functions: **2458** (522952 bytes) in 87 source files + 2 unassigned gap files.
- Status: todo 0, draft 2458, reviewed 0.
- Attributed to a source file: 2429 (98%), of which [V] 359.

### Proposed module cards

| # | Module card | Files | Functions | Bytes | Done |
|---:|---|---|---:|---:|---:|
| 1 | memcard + save globals | `memcard.c` | 4 | 678 | 0 |
| 2 | zfile + file devices | `zfile.c`, `zrwFile.c`, `zramdisk.c` | 69 | 12308 | 0 |
| 3 | zasset + zcompres (ZAL loading) | `zasset.c`, `zcompres.c` | 17 | 4913 | 0 |
| 4 | ziff (IFF chunk reader) | `ziff.c` | 22 | 3011 | 0 |
| 5 | zScene + scene points | `zScene.c`, `zscnpts.c`, `zpanorama.c` | 55 | 7972 | 0 |
| 6 | zmem + zList + zstring + zutil (engine basics) | `zmem.c`, `zList.c`, `zstring.c`, `zutil.c`, `zdebug.c` | 117 | 9131 | 0 |
| 7 | zmath + zquat + zanim | `zmath.c`, `zquat.c`, `zanim.c` | 112 | 13448 | 0 |
| 8 | game progression (game, comdata) | `game.c`, `comdata.c` | 50 | 27094 | 0 |
| 9 | rider | `rider.c` | 34 | 17120 | 0 |
| 10 | physics | `physics.c` | 50 | 45590 | 0 |
| 11 | action + controller + camera | `action.c`, `contrler.c`, `camera.c` | 32 | 13934 | 0 |
| 12 | props, replay, fx | `prop.c`, `replay.c`, `fx.c` | 52 | 12309 | 0 |
| 13 | overlays (HUD) | `overlay.c`, `overlymp.c`, `zoverii.c` | 152 | 48467 | 0 |
| 14 | frontend + GUI | `frontend.c`, `guimain.c`, `guiinput.c`, `guisfile.c`, `guitoggl.c` | 257 | 68755 | 0 |
| 15 | sound | `bmxsound.c`, `zsndseq.c`, `zsndsrc.c`, `winChan.c`, `winLoad.c`, `winStrm.c` | 129 | 29129 | 0 |
| 16 | NgIF layer (game <-> engine) | `NgApp.c`, `MissingZGE.c`, `NgIFfx.c`, `NgIFpsx.c`, `NgIFzactor.c`, `NgIFzbsp.c`, `NgIFzcollide.c`, `NgIfzmemDMM.c`, `NgIFzres.c`, `NgIFzroller.c` | 394 | 58553 | 0 |
| 17 | actors + primitives | `zactor.c`, `zprim.c` | 158 | 29950 | 0 |
| 18 | rag doll + limb IK | `dyn.c` | 74 | 23732 | 0 |
| 19 | engine objects | `zobj.c`, `zobjCamera.c`, `zobjModel.c`, `zobjLight.c`, `zobjPEmit.c`, `zprop.c`, `ztaginf.c`, `zinput.c`, `zge.c` | 149 | 15451 | 0 |
| 20 | RenderWare glue (zrw*) | `zrwModel.c`, `zrwModelCreate.c`, `zrwModelGeom.c`, `zrwModelRender.c`, `zrwMat.c`, `zrwTxr.c`, `zrwWorld.c`, `zrwCam.c`, `zrwLight.c`, `zrwShadow.c`, `zrwCol.c`, `zrwPrim.c`, `zrwDbg.c`, `zrwRamDisk.c` | 359 | 50228 | 0 |
| 21 | app, platform, font (WinMain, zrwZgl, pc*) | `zrwZgl.c`, `pcinput.c`, `pcmovie.c`, `pcProfile.c`, `zprofile.c`, `zFont.c` | 107 | 21728 | 0 |
| | (not in a card yet) | `NgEngine/Zge/ztoken.c`, `NgEngine/Zge/ztimer.c`, `NgEngine/Zge/zheap.c` | 36 | 6906 | |
| | (unassigned gaps) | 2 files | 29 | 2545 | |

### Per file

| File (decomp/src/...) | Funcs | Bytes | V | I | todo/draft/reviewed |
|---|---:|---:|---:|---:|---|
| `zaxis/Bmx/Src/com/action/action.c` | 8 | 5072 | 3 | 5 | 0/8/0 |
| `zaxis/Bmx/Src/com/action/camera.c` | 13 | 7160 | 1 | 12 | 0/13/0 |
| `zaxis/Bmx/Src/com/action/comdata.c` | 3 | 847 | 1 | 2 | 0/3/0 |
| `zaxis/Bmx/Src/com/action/contrler.c` | 11 | 1702 | 5 | 6 | 0/11/0 |
| `zaxis/Bmx/Src/com/action/fx.c` | 21 | 3074 | 2 | 19 | 0/21/0 |
| `zaxis/Bmx/Src/com/action/game.c` | 47 | 26247 | 10 | 37 | 0/47/0 |
| `zaxis/Bmx/Src/com/action/memcard.c` | 4 | 678 | 1 | 3 | 0/4/0 |
| `zaxis/Bmx/Src/com/action/overlymp.c` | 11 | 6914 | 2 | 9 | 0/11/0 |
| `zaxis/Bmx/Src/com/action/overlay.c` | 122 | 33470 | 5 | 117 | 0/122/0 |
| `zaxis/Bmx/Src/com/action/physics.c` | 50 | 45590 | 3 | 47 | 0/50/0 |
| `zaxis/Bmx/Src/com/action/prop.c` | 18 | 5854 | 1 | 17 | 0/18/0 |
| `zaxis/Bmx/Src/com/action/replay.c` | 13 | 3381 | 7 | 6 | 0/13/0 |
| `zaxis/Bmx/Src/com/action/rider.c` | 34 | 17120 | 3 | 31 | 0/34/0 |
| `_unassigned/gap_rider__guiinput.c` | 15 | 962 | 0 | 0 | 0/15/0 |
| `zaxis/Bmx/Src/com/menu/gui/guiinput.c` | 20 | 1455 | 2 | 18 | 0/20/0 |
| `zaxis/Bmx/Src/com/menu/gui/guimain.c` | 86 | 6543 | 0 | 86 | 0/86/0 |
| `zaxis/Bmx/Src/com/menu/gui/guisfile.c` | 1 | 1116 | 0 | 1 | 0/1/0 |
| `zaxis/Bmx/Src/com/menu/gui/guitoggl.c` | 14 | 4819 | 10 | 4 | 0/14/0 |
| `zaxis/Bmx/Src/com/menu/frontend.c` | 136 | 54822 | 1 | 135 | 0/136/0 |
| `zaxis/Bmx/Src/com/zge/bmx/bmxsound.c` | 10 | 6940 | 3 | 7 | 0/10/0 |
| `zaxis/Bmx/Src/com/zge/bmx/dyn.c` | 74 | 23732 | 0 | 74 | 0/74/0 |
| `zaxis/Bmx/Src/com/zge/bmx/zoverii.c` | 19 | 8083 | 13 | 6 | 0/19/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/MissingZGE.c` | 8 | 212 | 7 | 1 | 0/8/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgApp.c` | 28 | 7384 | 2 | 26 | 0/28/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFfx.c` | 31 | 10407 | 1 | 30 | 0/31/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFpsx.c` | 39 | 13991 | 5 | 34 | 0/39/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFzactor.c` | 12 | 3735 | 4 | 8 | 0/12/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFzbsp.c` | 137 | 10639 | 5 | 132 | 0/137/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFzcollide.c` | 17 | 4564 | 3 | 14 | 0/17/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIfzmemDMM.c` | 11 | 2119 | 8 | 3 | 0/11/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFzres.c` | 19 | 1894 | 6 | 13 | 0/19/0 |
| `zaxis/Bmx/Src/com/zge/NgIF/NgIFzroller.c` | 92 | 3608 | 12 | 80 | 0/92/0 |
| `zaxis/Bmx/Src/Pc/zge/sound/winChan.c` | 21 | 6689 | 12 | 9 | 0/21/0 |
| `zaxis/Bmx/Src/Pc/zge/sound/winLoad.c` | 17 | 2969 | 11 | 6 | 0/17/0 |
| `zaxis/Bmx/Src/Pc/zge/sound/winStrm.c` | 27 | 5368 | 15 | 12 | 0/27/0 |
| `zaxis/Bmx/Src/com/zge/zactor.c` | 85 | 15865 | 2 | 83 | 0/85/0 |
| `zaxis/Bmx/Src/com/zge/zprim.c` | 73 | 14085 | 6 | 67 | 0/73/0 |
| `zaxis/Bmx/Src/com/zge/zsndseq.c` | 28 | 4684 | 7 | 21 | 0/28/0 |
| `zaxis/Bmx/Src/com/zge/zsndsrc.c` | 26 | 2479 | 9 | 17 | 0/26/0 |
| `NgEngine/Zge/zdebug.c` | 7 | 402 | 1 | 6 | 0/7/0 |
| `NgEngine/Zge/Com/ZglRw/zrwDbg.c` | 32 | 5161 | 2 | 30 | 0/32/0 |
| `NgEngine/Zge/zinput.c` | 6 | 296 | 1 | 5 | 0/6/0 |
| `NgEngine/Zge/zasset.c` | 11 | 1959 | 4 | 7 | 0/11/0 |
| `NgEngine/Zge/zmath.c` | 59 | 4742 | 0 | 59 | 0/59/0 |
| `NgEngine/Zge/ZgeMods/zobjCamera.c` | 11 | 800 | 2 | 9 | 0/11/0 |
| `NgEngine/Zge/zstring.c` | 39 | 2802 | 3 | 36 | 0/39/0 |
| `NgEngine/Zge/zquat.c` | 17 | 2518 | 4 | 13 | 0/17/0 |
| `NgEngine/Zge/zfile.c` | 35 | 7747 | 19 | 16 | 0/35/0 |
| `NgEngine/Zge/zmem.c` | 41 | 3555 | 12 | 29 | 0/41/0 |
| `NgEngine/Zge/ZgeMods/zprop.c` | 8 | 1429 | 0 | 8 | 0/8/0 |
| `NgEngine/Zge/zobj.c` | 33 | 2768 | 3 | 30 | 0/33/0 |
| `NgEngine/Zge/Com/ZglRw/zrwModel.c` | 47 | 4364 | 2 | 45 | 0/47/0 |
| `NgEngine/Zge/ZgeMods/zobjModel.c` | 35 | 1111 | 1 | 34 | 0/35/0 |
| `NgEngine/Zge/Com/ZglRw/zrwMat.c` | 25 | 2378 | 9 | 16 | 0/25/0 |
| `NgEngine/Zge/Com/ZglRw/zrwZgl.c` | 43 | 8806 | 6 | 37 | 0/43/0 |
| `NgEngine/Zge/zList.c` | 20 | 1028 | 1 | 19 | 0/20/0 |
| `NgEngine/Zge/Com/ZglRw/zrwPrim.c` | 12 | 4330 | 1 | 11 | 0/12/0 |
| `NgEngine/Zge/ZgeMods/ztaginf.c` | 28 | 7141 | 0 | 28 | 0/28/0 |
| `NgEngine/Zge/Com/ZglRw/zrwModelRender.c` | 19 | 2822 | 3 | 16 | 0/19/0 |
| `NgEngine/Zge/ZgeMods/zobjLight.c` | 11 | 470 | 2 | 9 | 0/11/0 |
| `NgEngine/Zge/zScene.c` | 39 | 6832 | 12 | 27 | 0/39/0 |
| `NgEngine/Zge/ZgeMods/zscnpts.c` | 11 | 997 | 0 | 11 | 0/11/0 |
| `NgEngine/Zge/Com/ZglRw/zrwCam.c` | 33 | 3784 | 1 | 32 | 0/33/0 |
| `NgEngine/Zge/Pc/pcinput.c` | 11 | 2898 | 3 | 8 | 0/11/0 |
| `NgEngine/Zge/Com/ZglRw/zrwCol.c` | 21 | 5623 | 2 | 19 | 0/21/0 |
| `_unassigned/gap_zrwCol__zobjPEmit.c` | 14 | 1583 | 0 | 0 | 0/14/0 |
| `NgEngine/Zge/ZgeMods/zobjPEmit.c` | 12 | 890 | 1 | 11 | 0/12/0 |
| `NgEngine/Zge/Com/ZglRw/zrwWorld.c` | 33 | 6311 | 3 | 30 | 0/33/0 |
| `NgEngine/Zge/Com/ZglRw/zrwTxr.c` | 49 | 5089 | 4 | 45 | 0/49/0 |
| `NgEngine/Zge/zanim.c` | 36 | 6188 | 4 | 32 | 0/36/0 |
| `NgEngine/Zge/ziff.c` | 22 | 3011 | 13 | 9 | 0/22/0 |
| `NgEngine/Zge/Com/ZglRw/zrwShadow.c` | 5 | 1314 | 1 | 4 | 0/5/0 |
| `NgEngine/Zge/zcompres.c` | 6 | 2954 | 0 | 6 | 0/6/0 |
| `NgEngine/Zge/Com/ZglRw/zrwModelCreate.c` | 51 | 6622 | 7 | 44 | 0/51/0 |
| `NgEngine/Zge/Com/ZglRw/zrwModelGeom.c` | 9 | 554 | 3 | 6 | 0/9/0 |
| `NgEngine/Zge/Com/ZglRw/zrwFile.c` | 17 | 2088 | 17 | 0 | 0/17/0 |
| `NgEngine/Zge/ZgeMods/zramdisk.c` | 17 | 2473 | 16 | 1 | 0/17/0 |
| `NgEngine/Zge/Com/ZglRw/zrwLight.c` | 10 | 995 | 1 | 9 | 0/10/0 |
| `NgEngine/Zge/zge.c` | 5 | 546 | 1 | 4 | 0/5/0 |
| `NgEngine/Zge/Pc/pcmovie.c` | 22 | 5260 | 0 | 22 | 0/22/0 |
| `NgEngine/Zge/ZgeMods/zpanorama.c` | 5 | 143 | 0 | 5 | 0/5/0 |
| `NgEngine/Zge/ztoken.c` | 18 | 5219 | 0 | 18 | 0/18/0 |
| `NgEngine/Zge/ZgeMods/zFont.c` | 11 | 1533 | 3 | 8 | 0/11/0 |
| `NgEngine/Zge/zutil.c` | 10 | 1344 | 0 | 10 | 0/10/0 |
| `NgEngine/Zge/Com/ZglRw/zrwRamDisk.c` | 13 | 881 | 1 | 12 | 0/13/0 |
| `NgEngine/Zge/zprofile.c` | 15 | 3032 | 1 | 14 | 0/15/0 |
| `NgEngine/Zge/ztimer.c` | 9 | 891 | 0 | 9 | 0/9/0 |
| `NgEngine/Zge/Pc/pcProfile.c` | 5 | 199 | 1 | 4 | 0/5/0 |
| `NgEngine/Zge/zheap.c` | 9 | 796 | 0 | 9 | 0/9/0 |

<!-- END GENERATED -->

# Controls, the action loop and the camera (`action.c`, `contrler.c`, `camera.c`)

Sources: `decomp/src/zaxis/Bmx/Src/com/action/{action,contrler,camera}.c` (27 functions, VA
0x00403C30-0x004088C5), types `decomp/include/contrler.h` and `camera.h`, tests
`tests/test_decomp_action_camera.py`. Exe = the 2000-11-22 PC build. Evidence tags as in CONTRIBUTING.md AD-5:
**[V]** read from the code or the exe's tables (the test reads them back), **[I]** inferred.
Nothing here is verified in-game.

## 1. Data flow, once per frame

```
DirectInput keyboard + joystick
  --pcinput keymap / joystick map-->  ZInputDevice.channel[22]   (0x00493910, pcinput.c)
  --fixed channel->bit table------->  ZInputDevice.buttons (PAD_* bits)
  --contrlerUpdate 0x004080F0-------> BmxPad.input: held, changed, 2 sticks (+ previous)
       7 bits remapped through SaveProfile.control[] (save profile +0x24), the rest pass through
  --riderReadInput 0x00430E70------> BmxRider.input (sticks quantised to 1/127 for replays)
  --contrlerTestAction 0x004084B0--> g_contrlerActions[config][ACT_*][pressed/held/released]
                                     = (button mask, test) --> contrlerTestMask 0x00408740
```

`actionUpdate` 0x004040E0 (called by NgApp every frame) does: pads (`contrlerUpdate`), the quit combo,
then by action mode: **play** = `bmxreplayUpdate` (record), `riderReadInput`, `riderUpdateAll`,
`bmxphysUpdate`, props, `gameUpdate`; **pause** = the pause menu (and the replay behind its replay
screens); **replay** / **demo** = the replay viewer; then `cameraUpdate`, then the engine's frame
callbacks (`zrollerRunDrawCallbacks`, e.g. `cameraDrawNightVision`). [V]

## 2. Buttons [V bits, I PSX names]

The game was written for the PlayStation pad. `PAD_*` (contrler.h) are the bits of `buttons`; the
frontend's own names (0x0055A3F4) are "Up" .. "Fire 6".

| Bit | Frontend name | PSX | Default key (pad 0) | Default joystick button |
|---|---|---|---|---|
| 0x00001 / 2 / 4 / 8 | Up / Down / Left / Right | d-pad | arrow keys | axes (y < 0 is up); stick 0 also counts beyond 0.5 |
| 0x00010 | Fire 1 | Triangle | E | 4 |
| 0x00020 | Fire 2 | Square | S | 1 |
| 0x00040 | Fire 3 | Circle | F | 3 |
| 0x00080 | Fire 4 | Cross | D | 2 |
| 0x00100 / 0x00200 | L Trig 1 / R Trig 1 | L1 / R1 | W / R | 5 / 6 |
| 0x00400 / 0x00800 | L Trig 2 / R Trig 2 | L2 / R2 | 3 / 4 (pcinput default only, see 2.1) | 7 / 8 |
| 0x01000 / 0x02000 | L Trig 3 / R Trig 3 | (L3 / R3) | - (no channel feeds them) | - |
| 0x04000 | Start | Start | Space | 10 |
| 0x08000 | Select | Select | - | 9 |
| 0x10000 | Fire 5 | (PC) | Esc | - |
| 0x20000 | Fire 6 | (PC) | Enter | - |

The identification of E/S/F/D as Triangle/Square/Circle/Cross [I] comes from the diamond layout of the
default keys and from the controller-screen labels (STRS 0x14C-0x157: PEDAL, NAVIGATION, SPIN LEFT,
SPIN RIGHT, TRICK, BRAKE, JUMP, PEDAL/JUMP, START/JUMP, TURN AROUND, GRIND/STALL, MODIFIER), which
match the PSX game's Cross = pedal/jump, Square = trick, Circle = modifier, Triangle = grind.

### 2.1 Keyboard layouts and joystick maps [V tables]

`zinputOpen(2)` opens a joystick + keyboard device per pad (if that fails the caller retries with zinputOpen(0), joystick only; inside pcinputOpen a missing joystick leaves a keyboard-only record, 2.4).
`pcinputOpen` gives the first device keymap 0x00561E68 and the second 0x00561E88. **At every level
load** NgApp (0x0045BE60) replaces pad 0's keymap with the profile's keyboard layout
(`g_keyLayouts` 0x0055BB98 + 32 x `profile[0].keyboard_layout`, save +0x4B; "< KEYBOARD LAYOUT n >").
A keymap is (DirectInput scan code, channel) byte pairs, 0xFF-terminated.

| keymap | Up | Down | Left | Right | Tri | Sq | Ci | X | L1 | R1 | L2 | R2 | Start | Esc | Enter |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| layout 1 (0x0055BB98) | Up | Down | Left | Right | E | S | F | D | W | R | - | - | Space | Esc | Enter |
| layout 2 | Num8, Up | Num5, Down | Num4 | Num6 | Space | S | D | Num0 | A | F | - | - | Esc | ` (tilde) | Enter |
| layout 3 | W, Up | S, Down | A | D | Num8 | Num4 | Num6 | Num5 | Num7 | Num9 | - | - | Esc | Num+ | Enter |
| layout 4 | Up | Down | Left | Right | Num5 | Num4 | Num6 | RCtrl | Num0 | NumEnter | - | - | Esc | End | Enter |
| layout 5 | W, Up | S, Down | A | D | Home | Ins | PgUp | Space | Del | PgDn | - | - | Esc | ` (tilde) | Enter |
| pcinput pad 0 default (0x00561E68) | Up | Down | Left | Right | E | S | F | D | W | R | 3 | 4 | Space | Esc | Enter |
| pcinput pad 1 default (0x00561E88) | I | K | J | L | Z | X | C | V | B | M | N | H | Space | - | - |

The controller screen's key labels (0x0055A450, 5 x 18 strings) name rows 0-10 the same way; its row
10 shows the "Esc" column (the key that pauses besides Start).

Joystick maps (`pcinput` matches the device name; button numbers 1-based):

| joystick map | Tri | Sq | Ci | X | L1 | R1 | L2 | R2 | Start | Select |
|---|---|---|---|---|---|---|---|---|---|---|
| default (0x00561EA8) | 4 | 1 | 3 | 2 | 5 | 6 | 7 | 8 | 10 | 9 |
| Gravis GrIP / GamePad Pro (0x00561ED8) | 4 | 1 | 3 | 2 | 5 | 6 | 7 | 8 | 10 | 9 |
| Microsoft SideWinder game pad (0x00561F10) | 5 | 4 | 2 | 1 | 7 | 8 | - | - | 9 | 10 |

### 2.4 The PC devices: `zinput.c` and `pcinput.c` [V code, I names]

Source: `decomp/src/NgEngine/Zge/zinput.c`, `Pc/pcinput.c`, types `contrler.h` (ZInputDevice) and
`zpc.h` (PcInputDevice, PcJoyMapHeader); test `tests/test_decomp_zobj.py`.

- `zgeInit` -> `zinputInit` 0x004802F0 (pool of two ZInputDevice records) -> `pcinputInit` 0x00493160:
  `DirectInputCreateEx(hinstance, 0x0700, IID_IDirectInput7A)` on the ZGL window (zglGet 0x15), then
  `EnumDevices(DIDEVTYPE_JOYSTICK, ATTACHEDONLY)`; the callback keeps the **first** attached joystick's
  instance GUID and stops. Only one joystick is ever used, by every pad that asks for one.
- `zinputOpen(type)` 0x00480370 -> `pcinputOpen(type, dev)` 0x00493310, type 0 joystick, 1 keyboard,
  2 both (the game opens pad devices with 2):
  - keyboard: one shared device (`GUID_SysKeyboard`, `c_dfDIKeyboard`, cooperative level
    NONEXCLUSIVE | FOREGROUND), created on first use. The **first** opened record gets keymap 1
    (0x00561E68), every later one keymap 2 (0x00561E88) (table in 2.1).
  - joystick: `c_dfDIJoystick`, EXCLUSIVE | FOREGROUND; X and Y range -127..127, dead zone 1000
    (10 %). If no joystick can be created it prints "PCINPUT: Could not create controller" and the
    record falls back to the keyboard (the same shared device and keymap rule).
  - the button map: the default 0x00561EA8 unless the joystick's `tszProductName` equals a name of an
    entry of `g_pcinputJoyMaps` 0x00561F2C (Gravis: "Gravis GrIP", "Gravis GamePad Pro"; SideWinder:
    "Microsoft SideWinder game pad", "Microsoft SideWinder (Auto Detect)") or its `guidProduct` equals
    a listed product GUID (SideWinder only, 0x0054E0F0). Later entries win. The pairs are (0-based
    button, channel); channels 8-11 Tri/Sq/Ci/X, 14/15 L1/R1, 16/17 L2/R2, 20 Start, 21 Select.
  - The devices are created as `IDirectInputDevice2A` (IID 0x0054F408) through `CreateDeviceEx`.
- `zinputSetRumble` 0x00480440 calls 0x00493CB0, which returns 0: no force feedback on PC.
- `pcinputShutdown` 0x00493240 unacquires everything, releases the keyboard and DirectInput.
- The poll (0x00493910), the close (0x004937F0) and the layout setter (0x00493CC0) are in the ZglRw pass's
  gap file after pcinput.c.

**Default bindings** (unchanged from 2.1; a manual in-game check should
confirm them): pad 0 keyboard arrows + E/S/F/D (Tri/Sq/Ci/X), W/R (L1/R1), 3/4 (L2/R2, pcinput map
only), Space (Start), Esc, Enter; pad 1 I/K/J/L, Z/X/C/V, B/M, N/H, Space; joystick buttons
4/1/3/2 = Tri/Sq/Ci/X, 5/6 = L1/R1, 7/8 = L2/R2, 10 = Start, 9 = Select (SideWinder: 5/4/2/1,
7/8, 9 = Start, 10 = Select).

### 2.2 Remapping: the save's control masks (profile +0x24) [V]

`contrlerUpdate` builds the pad's logical buttons bit by bit: for the seven bits
**Cross 0x80, Square 0x20, Circle 0x40, Triangle 0x10, L1 0x100, R1 0x200, Start 0x4000** (in that
order) the bit is set when `device.buttons & profile.control[k]` is non-zero; every other bit is
copied. `control[k]` is therefore "the device button(s) that act as button k"; the defaults
(`comdataInitDefaults`, 0x00407C72) are the identity 0x80, 0x20, 0x40, 0x10, 0x100, 0x200, 0x4000.
Pad *i* uses profile *i*'s masks (the players then read pad `profile.index`). The d-pad, L2/R2 and
Select are never remapped.

The CONTROLLER screen (frontend 0x00441CB0, input 0x004420A0, accept 0x0044256B) shows 11 rows
(Up, Down, Left, Right, then the seven), each with the layout's key label and the bound button's name;
"waiting for a button" stores the lowest set device bit; accept copies all 11 to profile **+0x14**..+0x3F
and the layout to +0x4B. So **+0x14..+0x23 are four d-pad bindings** that nothing applies, and the
profile name (+0x07) can only use 12 characters + NUL (`save_fields.py` limits it; `save.py` still
reads 29 bytes). `save_fields`: `profile.<p>.control.<k>`, `.dpad.<k>`, `.controller_config`,
`.keyboard_layout`; `dmbmx save get` labels masks with the button names.

### 2.3 Held / changed / pause [V]

- `changed = (held_old ^ logical) & ~ignore`, `held ^= changed`, `ignore &= logical`: buttons in
  `ignore` keep their old state until they are released.
- `contrlerSetPaused(1)` (entering the pause menu) saves `held` and ignores everything down;
  `contrlerSetPaused(0)` restores the saved `held` and ignores what was pressed during the pause, so
  the button that closes the menu does not make the rider jump.
- `contrlerTestMask` adds stick 0 as the d-pad (beyond 0.5); crossing 0.5 either way is a change.
- Tests: 0 pressed (any), 1 pressed (all held, one changed), 2 held (all), 3 held (any), 4 released
  (none held, one changed), 5 released (any), 6 always, 7 never.
- A pad whose device disconnects is cleared; `contrlerUpdate` returns 1 (actionUpdate opens the pause
  menu) or 2 when it comes back. `contrlerRumble` 0x004086D0 is a bare `ret` on PC.

## 3. Actions (`ACT_*`) and the three configurations [V table, I names]

`contrlerTestAction(config, input, action, status)` asserts config < 3, action <= 20, status < 3 and
looks up `g_contrlerActions` 0x00553EE0 `[3][21][3]` of (mask, test). `config` is the profile's
**+0x02** byte, CONFIGURATION A/B/C (STRS 0x23D-0x23F), copied into the rider (`profileByte2`).
rider.c is the only caller; the (action, status) pairs it asks are pinned by the test against the
exe's call sites (the camera pass fixed one swapped pair in rider.c: turnaround is asked "pressed" and jump
"held" at that site).

| id | name | used by rider.c | config A | config B | config C |
|---:|---|---|---|---|---|
| 0 | START | pressed | X released | X released | X released |
| 1 | PEDAL | held | X pressed / Up held | X | Up |
| 2 | BRAKE | - | Down | Down | Down |
| 3 | TRICK | all | Square | Square | Square |
| 4 | MODIFIER | all | Circle | Circle | Circle |
| 5 | (05) | - | X held / held / released | same | same |
| 6 | JUMP | held, released | X | **X released** for all three | X |
| 7 | GRIND | held | Triangle (released: never) | same | same |
| 8 | PLANT | pressed, released | X pressed (held: always, released: never) | same | same |
| 9 | TURNAROUND | pressed | Up pressed (else never) | same | same |
| 10 | SPIN_LEFT | held | L1 or L2 | L1 or L2 | L1 |
| 11 | SPIN_RIGHT | held | R1 or R2 | R1 or R2 | R1 |
| 12-15 | - | - | never | never | empty mask (held = always true) |
| 16 | BAIL_A | pressed | Tri+Sq+Ci+X all held, one just pressed (held: always) | same | same |
| 17 | BAIL_B | pressed | never | never | never |
| 18 | GO_LIMP | held | Circle | Circle | Circle |
| 19 | RAGDOLL | held | Triangle | Triangle | Triangle |
| 20 | (20) | - | Square | Square | Square |

"X" = Cross. A cell with one button means the three statuses test pressed / held / released of that
button. "L1 or L2" (mask 0x500): pressed = either goes down (test 0), held = either is held (test 3),
released = both are up and one just went up (test 4).

So: **A** = Cross pedals (tap) and jumps (hop strength = frames held); Up held also pedals. **B** =
Cross pedals and the jump goes off on release. **C** = Up pedals, Cross jumps, only L1/R1 spin.
Single-player auto-pedalling (rider.c) applies to configs A and B (`profileByte2 < 2`).

Raw button tests outside the riding (actionUpdate, `contrlerTestButtons` with the player's pad):
pause = Start or Esc pressed; quit = Start + Select held > 60 frames (sets 0x00551FD4, back to the
menus); replay viewer: Start/Esc stop, Square pause, Circle (paused) orbit the frozen rider, any
face button resume, Cross next camera shot, Triangle restart, Select HUD on/off; the attract demo
ends on any button. In the pause menu a held d-pad auto-repeats after 20 frames, then every 3 frames
(every frame with L1/R1 held).

## 4. The action loop (`action.c`) [V]

| Function | VA | What |
|---|---|---|
| actionStartup | 0x00403C30 | opens `Assets\MOTION.ZAL`, save defaults, menu mode |
| actionPickDemoLevel | 0x00403CE0 | level of the next attract demo |
| actionLoad | 0x00403D10 | pads, level resources, physics, fx, riders (demo: `g_actionDemoRuns` {Dave Mirra / Woodward Trails, Ryan Nyquist / Greenville Park, Ryan Nyquist / Vail Trails}, costume 5), overlays, `cameraInit`, motion table (asserts <= 238 clips), sound (cheat bit 5 "silly grunt"), props, game rules; demo / 1P `gameStart` / 2P screen 0x16 |
| actionUpdate | 0x004040E0 | the per-frame loop (section 1); `g_actionFrameSkip` 0x00551A3C runs the body every n-th call (1); -1 is a debug switch that holds R2 on pad 0 |
| actionShutdown | 0x00404ED0 | "APP: Shutdown ACTION" |
| actionSetMode | 0x00404F40 | 0 play, 1 pause, 2 view replay, 3 replay of `g_actionInstantReplay` (that handle is never written: [I] unused), 4 attract demo (replay.zal group `rider + level * 2` into a 0x2800-byte replay), 0xFF none |
| actionGetMode | 0x004056A0 | |

Replay viewer states (`g_actionReplayState` 0x0056B3E0): 0 fade in (30 frames), 1 playing, 2 fade out
(30), 3 paused, 4 restarted (30 frames, then 1), 5 paused + orbit, 6 ended (30, then 2), 7 crash
physics for 240 frames (no code in these files sets it). The demo ends after 3900 frames (130 s) or at
the replay's end and shows its banner at frames 30 and 3450.

## 5. The camera (`camera.c`) [V code, I meanings]

**Ownership:** camera.c is 0x004056C0-0x00407B85: the skeleton had put cameraInit / cameraUpdate /
cameraApply in action.c and left five functions in a gap; all ten only use the camera globals, and the
file's one assert (line 1183) needs ~1200 lines before 0x00407530, which cameraUpdate supplies.
`cameraDrawNightVision` 0x004074C0 was missing from Ghidra (reached only through a callback pointer).

One `BmxCamera` (0xB8) per player at 0x0056B448; only `g_cameraPlayer`'s runs and is handed to the
engine (`cameraApply`: scene camera 0x0068FF98 FOV in binary degrees, position, look-at; the sound
listener follows the camera).

### 5.1 Modes (`cameraSetMode` 0x00407760)

| Mode | Set by | Behaviour |
|---|---|---|
| 0 FOLLOW | game.c at run start / turn | the player's view (5.2); in a crash (not first person) a high wide shot: 300 out, 700 up, looking at the rider |
| 1 REPLAY | replays, demo | TV shots (5.3), starts on shot 6, cycles 0..8 by itself |
| 2 (still) | nobody | target not moved (its smoothing set exists) |
| 3 ORBIT | game.c run intro | 500 out, 700 up, +300 binary angle per frame around the rider |
| 4 FLYBY | level load, standings, menus | every 256 frames a random spot of the level (1000 inside the box, 400 grid, plain ground, no prop / hazard), 500 up; the look-at circles it (-50 per frame) |

Leaving or entering orbit / flyby / replay, and first person, cuts (`snap`). Orbit and flyby (and
first person) switch off the wall push and the 250 minimum distance.

### 5.2 Views: the save's profile +6 (`cameraSetView` 0x00407620, pause menu "camera type")

All look at the hips (rider position + physics hip offset), a third of the way towards the bike once
moving. Distances in cm. Smoothing sets from `g_camViewParams` 0x00551E6C:

| View | Position | Look-at lead | spring | damping | pos follow | look follow | FOV |
|---|---|---|---|---|---|---|---|
| 0 behind | 275 behind along the motion (facing when still), 160 up rolling (and grinding, stalling) / 70 standing still / 250 in the air (275 rising up a quarter-pipe or vert face, with look follow 0.75) / 125 locking on | 5 frames of velocity (vertical part only when falling faster than 15 per frame) | 0.06 | 0.45 | 0.70 | 0.35 | 50 |
| 1 wide | same | same | 0.06 | 0.45 | 0.75 | 0.50 | 60 |
| 2 above | 800 up, 150 behind, looking down | 10 frames of horizontal velocity | 0.02 | 0.30 | 1.0 | 0.25 | 60 |
| 3 first person | the head (hips + rider up x 100), with the rider's matrix; rider and bike hidden; horizon off | 500 ahead + 80 up | 0.16 | 0.40 | 1.0 | 0.50 | 70 |
| crash (0-2) | 300 out, 700 up | the rider | 0.008 | 0.60 | 1.0 | 0.13 | 60 |

"Behind" and "wide" differ only in field of view and smoothing. The camera is a damped spring: the
wanted position is smoothed (pos follow), the velocity gains `spring x distance` towards it and
loses `damping` of itself each frame; walls within 170 that face the camera push it out
(`zbspColSphere`), it stays at least 250 from the hips horizontally, and if the hips are hidden
for more than 20 frames it cuts (follow) or changes shot (replay).

### 5.3 Replay shots (`cameraSetShot` 0x004078C0, `g_camShotParams` 0x00551D04)

| Shot | Kind | Detail |
|---|---|---|
| 0 | orbit | 500 out, 300 up, +300 / frame; 180 frames |
| 1, 3, 5, 7, 10 | fixed point | 2.5 m ahead of the rider (or 25 frames of motion), 400 grid, on respawn ground, raised 300 / 500 / 500 / 120 / 800; zooms in (FOV 60 / 25 / 30 / - / 30) while the rider passes within 13 m (shot 1) or 20 m; shot 7 rises 5 per frame (crane) |
| 2, 4, 6 | ride-along | offset in the rider's frame {-200, 100, 0}, {-100, 700, 0}, {150, 100, -200}; 150 frames |
| 8, 9 | far fixed point | moves on when the rider is 15 m (8) / 30 m (9) away; shot 9 zooms to FOV 15 while `g_cameraShake` 0x0056B5B8 is set |

A cut to a fixed shot flashes the screen half of the time when the rider is airborne, locking on or
grinding (physics states 1, 3-5; not in the pause menu, not shots 9-10).

### 5.4 Cheats seen here [V code]

`cheats_active` bits follow the cheat list: bit 1 **night vision** = a semi-transparent green
640 x 480 rectangle drawn every frame (`cameraDrawNightVision`); bit 6 **ghost rider** = the rider
actor is hidden by the camera update (only the bike shows); bit 5 **silly grunt** is passed to the
sound init by `actionLoad`.

## 6. Open questions

- Who sets `g_actionInstantReplay` 0x0056B3E2, action mode 3 and replay state 7 (nothing in the exe
  writes the handle; mode 3 may be dead code from the PSX version).
- ACT_05, ACT_12-15, ACT_20 and ACT_BAIL_B are never asked; ACT_BRAKE neither (braking reads the stick).
- The PSX names of the buttons are inferred; confirming them needs the in-game CONTROLLER screen.
- Profile name length (12 by the layout) is not checked against the name-entry code.
- `zrollerStartCrossFade` (the cut flash) and `zsndSrcResetDoppler` (called on every cut) are only named
  from their use here.

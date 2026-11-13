# Rider physics (`physics.c`) and what it reads from the levels

DM-048. Source: `decomp/src/zaxis/Bmx/Src/com/action/physics.c` (50 functions, VA 0x0041FA20-0x0042DE22),
types `decomp/include/physics.h` and the physics fields of `rider.h`, Python port
`tools/dmbmx/formats/physics.py`, collision-tree decoder `tools/dmbmx/formats/rwcoll.py`, tests
`tests/test_decomp_dm048.py`; the tree builder and the walk ports are DM-049 (3.1,
`tests/test_rwcoll_build.py`). Exe = the 2000-11-22 PC build. Evidence tags as in CONTRIBUTING.md AD-5:
**[V]** read from the code (disassembly / decompilation) or checked over the corpus by code;
**[I]** inferred. Nothing here is verified in-game.

Conventions [V]: ZGE world units (the level is in centimetres: RW units x 100), **y points down**
(the "up" vector is `{0, -1, 0}` at 0x00557888), velocities are per frame, angles are 16-bit binary
angles (0x10000 = 360 degrees).

## 1. Structure

The rider is a **7-state machine** [V] (`BmxRider.physState`, rider +0x3DC). `bmxphysUpdate`
0x0041FC40 runs, per active rider and frame, six tables of 7 function pointers (0x00557898-0x0055793F):

| State | Name | enter | orientation | forces | position | post |
|---|---|---|---|---|---|---|
| 0 | ROLL | StateInitRoll | UpdateOrientationRoll | (nothing) | **UpdatePositionRoll** 0x00425940 | PostRoll |
| 1 | AIR | **StateInitAir** 0x00422E50 | UpdateOrientationAir | UpdateForcesAir | **UpdatePositionAir** 0x00428DF0 | PostAir |
| 2 | CRASH | StateInitCrash | - | - | UpdatePositionCrash (ragdoll) | - |
| 3 | GRIND_LOCK | StateInitLockOn | UpdateOrientationLockOn | UpdateForcesLockOn | UpdatePositionLockOn | PostAir |
| 4 | GRIND | StateInitGrind | UpdateOrientationGrind | UpdateForcesGrind | UpdatePositionGrind | PostGrind |
| 5 | STALL_LOCK | StateInitLockOn | UpdateOrientationLockOn | UpdateForcesLockOn | UpdatePositionLockOn | PostAir |
| 6 | STALL | StateInitStall | UpdateOrientationGrind | - | - | PostGrind |

Rider fields and flags: `physState`, `physTimer`, `physLock*`, `physAir*`, `wheel[2]` (`BmxWheel`) in rider.h;
`stateFlags` bits with rider.c's `RIDER_FLAG_*` names (physics sets 0x40000 TURNAROUND = moving backwards,
0x80000 MIRROR = grind / stall / wall hit from the edge's other side, 0x400000 CAN_MANUAL = one wheel down on a
slope under 50 deg, 0x800000 HIT_WALL, 0x10000 LAND_ALT = hard landing, 0x2000 FAKIE, 0x4000 FORWARD_HOP).

Exit functions: AIR / the lock-ons re-attach the rider and record the landing contact; GRIND clears
the rotation. `bmxphysSetState` = exit[old] + enter[new]; an enter function may redirect (a lock-on
that finds no target keeps the old state; StateInitGrind on a quarterpipe coping may return ROLL).

**Ghidra had missed 15 functions** [V]: they are reached only through these tables or through the
incremental-link thunk table (0x00401005..). `decomp/symbols/symbols.csv` rows (card DM-048) create them:
0x004203A0, 0x00420F00, 0x004210C0, 0x00421190, 0x00422830, 0x00422E50, 0x004243B0, 0x00424A10,
0x00425610, 0x00425740, 0x00425760, 0x00425910, 0x00425940, 0x00428DF0, 0x0042BA50 (34 KB, 3x the
code DM-040 had attributed to physics.c). The real `bmxphysUpdatePositionAir` is 0x00428DF0; DM-040's
seed of that name (0x0042DAD0) only prints the same message and is renamed `bmxphysSetSurfaceFromHit`.
`bmxphysInit` 0x0041FA20 / `bmxphysShutdown` 0x0041FA40 (called from action.c) came from the
overlay/physics gap.

## 2. Tag codes as physics reads them (LEVEL.md 6.2, open question 3) [V code paths, I names]

Tag._sd face records reach physics through the collision layer: `zcolSegmentTest` (segment test)
takes the RW hit, finds the nearest tagged face within 2 units (`ztaginfFindNearestFace`) and copies
its record into the hit's face (`NgColFace`): **code** at +0x16 (class 1 records with `extra` == 0
keep only the id), **flags** remapped at +0x15.

### Face classes (`code >> 12`) -> `BmxRider.surface`

| Class | Meaning | What the code does |
|---|---|---|
| 0 | ground | plain surface. **Ids 0xFA1-0xFA4 = respawn faces** (`bmxphysFindRespawnFace` falls back to the up-facing one nearest in x/z) |
| 1 | (zone id) | **Never reaches physics as class 1 in the shipped data** [V]: the collision layer keeps only the id when the record's `extra` is 0 (0x00469E91-0x00469EB0), and `extra` is 0 in all 12 levels, so these faces (13,458, the largest group, 200 ids) behave as ground; the id stays in the face / contact record, where game.c reads it [I: gap / transfer zones]. Physics' own class-1 case (as 3, but keeping the horizontal speed: FAKIE air, `bmxphysAlignToRamp`, 180 turn for the way down, LIP guard) is reachable only through prop collision codes |
| 2 | spine | id = the edge-tag code of its coping edge [V: 1,591/1,591 class-2 face ids are edge codes; 1-8 distinct ids per level] (`bmxphysFindTaggedEdge`: nearest edge with that code; the surface becomes that edge; "quarterpipe face with no assigned edge, ID=%d" and class 0 when missing). Going up it within 200 below the coping and 300 of its plane, the air **hops over** the coping: lands (dist + 150) beyond, flight time from 0.85 x the take-off speed (min 15 frames), rolled over. Grind button = grind the coping |
| 3 | vert wall | id = coping edge [V 106/106, WoodwardVert only]. Air going up it (facing the coping plane within 55 deg): FAKIE (land back on the wall), aligned to the ramp and kept 120 off the coping plane, velocity across the plane removed, **no horizontal speed** (straight up and down), a 180 turn scheduled for the way down; LIP guard keeps the rider inside unless there is room behind the coping |
| 4-8 | launch ramps | the take-off horizontal speed is capped at **11, 15, 21, 23, 35** (classes 4..8; 16 for anything else), x (1 + sin a) when the ramp is within 30 deg of 45 deg |
| 9 | trigger | **not solid**: a wheel or the body touching it calls `bmxpropHitTrigger(slot, face)` (prop.c 0x0042EF60, DM-052: breaks the prop whose OBJI id = the face code) and passes through. (No class 9 face in the corpus.) |
| 10 | launch pad | the **id is a launch speed** (corpus ids 30-50): velocity loses its normal component and gains `id` units/frame along the face normal, state AIR, sound 0x109 |
| 11-15 | - | no case: treated as 0 |

### Face flags (TAGI face record byte 3) [V]

Remapped `bit0 -> 0x10, bit1 -> 0x08, bit2 -> 0x20, bit3 -> 0x40` (`dmbmx.formats.physics.face_flags`).
Physics tests **0x10** (TAGI bit 0): not a respawn face, not a wall-plant target; **0x20** (bit 2): not a
wall-plant target. 0x08 / 0x40 are not read by physics. The face `param` (u32 at +4) is not read by
physics [V: no access in physics.c, NgIFzbsp/NgIFzcollide copy only code and flags].

### Edge records (TAGI edge record: code, kind)

Grind edges come from Tag._sd edge tags (`zbspFindEdgesNear` / `00467410`, 0x38-byte `NgColEdge`) and
from prop rails. **code** = the id that class 1-3 faces name; **kind** (byte 2) = grind sound / sparks:
1 metal (sound 7, sparks), 2 wood (sound 2, no sparks), 0 and 3 default (sound 0, sparks). StateInitGrind
refuses a coping edge of kind 0 when grinding straight from the ground.

### Materials (not tags)

The wheel's surface type is the **RW material's flags & 0xF** (`NgColHit.materialFlags`, from the
material plugin) or the prop's class: 1/3 metal, 2 wood, 4, 5/9/10 dirt (spray), **6 hazard** (crash:
bits 19-20 pick the bail, surfaces 13/14), 0xB rumble (controller vibration), rolling-drag groups
2 -> 0.0015, 5/6/10/11 -> 0.0018, else 0.0015 (table 0x005577EC).

## 3. The 384-byte collision block (LEVEL.md 4.3, open question 1) [V]

**It is used for all world collision.** `zcolSegmentTest` -> `zrwCol_00494C70` -> the zrwCol
function pointer 0x00493E20 -> RW `RpCollisionWorldForAllIntersections` 0x004C1B10 (line: 0x004C05D0 ->
the world BSP walk 0x004BD400 -> the per-sector line test **0x004BE560**, which walks `sector + 0x54`);
spheres use 0x004BED60, which walks the same data. Tag._sd is only consulted afterwards for the tag of
the face that was hit.

Layout (`dmbmx.formats.rwcoll`): **128 nodes x 3 bytes**, an implicit binary tree (root = node 1,
children of i are 2i (low side) and 2i+1 (high side), node 0 unused = 0):

| Byte | Split node (bit 7 set) | Leaf |
|---|---|---|
| 0 | bit 7 = 1; bits 5-6 axis (0 x, 1 y, 2 z); bits 0-4 count of straddling triangles | bits 0-4 triangle count |
| 1 | index of the sector vertex whose coordinate is the split plane | - |
| 2 | first triangle (index into the sector's triangle list) | first triangle |

[V over all 784 sectors of the 12 levels] node 0 is 0; every triangle of the sector is referenced
exactly once; every triangle below a split lies on its side of the plane (0.001 tolerance).
**Consequence:** the line/sphere tests only visit the triangles the tree references, by index, so a
geometry edit that adds, removes or re-orders triangles must rebuild this block (and sort the triangles
so each node's are contiguous) or collision silently uses the wrong triangles; `rwcoll.check(sector)`
reports a stale tree. Editing vertex positions only is safe for the tree if no triangle crosses its
split plane afterwards. Tag._sd must be kept in step separately (it carries the tags and the grind
edges). `World.colSectorSize` is resolved in 3.1 (DM-049).

### 3.1 Rebuilding the tree (DM-049)

**How RW loads, sizes and saves the block** [V, decompiled from a private Ghidra copy]:

- The sector stream reader **0x004C2F50** reads the 384 bytes into a stack buffer, asks **0x004BB760**
  for the used size, and copies only that many bytes into the world's memory block. 0x004BB760 walks
  the reachable nodes in pre-order and returns `3 x (highest node index) + 3` (`rwcoll.used_size`).
  The rest of the 384 bytes is never loaded; on disk it is non-zero junk in 783 of 784 sectors (the
  writer **0x004C2CD0** copies the used bytes into an uninitialised 384-byte stack buffer and writes
  all of it).
- **`World.colSectorSize` = the sum over sectors of `used_size` rounded up to 4** [V: 12/12 levels].
  The world writer **0x004C3AE0** computes it exactly that way (and ORs **0x40000000** into the format
  word it writes: that is the unexplained format bit, always set by RW 3.1's writer [V]). The world
  reader **0x004C3E60** allocates one block for the whole world: world struct + sectors + 0x18 per plane
  + per-vertex arrays + 4 per triangle + `colSectorSize`, and the sector readers carve the trees out of
  it. **A wrong `colSectorSize` overruns that block**, so the dmbmx writer recomputes it
  (`rwcoll.col_sector_size`, `World.update_counts`).
- The world walk **0x004BD400** routes a segment through the PlaneSects using only `leftValue` and
  `rightValue` (a side is visited where the segment reaches its value; the plane `value` is not read),
  so sectors may overlap: the writer widens the values above an edited sector when its geometry leaves
  its region (`World._widen_ancestors`).
- The triangle test **0x004BEAB0** is Moller-Trumbore with a determinant threshold of 1e-6 (0x0069B354);
  whether it culls back faces depends on its register operands, not settled [I]. The sphere walk
  0x004BED60 has a quirk [V]: a sphere of radius 0 centred exactly on a split plane revisits the same
  node forever.

**RW's builder is not in NgBMX.exe** [I, strong]: besides the functions above (read, size, write,
the walks) no code touches the block; a scan for code packing the node byte (OR 0x80 with a shift by
5) finds nothing in the RenderWare range. The trees were built offline by RW's world importer.

**Exact reproduction of the shipped trees is not possible from the files** [V]: **0/784** sectors are
reproduced byte for byte (and 0/784 if only the used bytes are compared), because information the
builder had is not in the file:

1. The **vertex arrays were renumbered after the build** [V 784/784]: every sector's vertices are
   grouped by material and, within a material, in first-use order of the stored (tree-sorted)
   triangles. The plane vertex (byte 1) was chosen in the old numbering: among the 9,962 splits, in
   2,996 the plane vertex is used only by triangles outside the node's subtree and its ancestors, and
   in 3,590 it lies outside the node's region. Only its coordinate can be checked.
2. **Build-time coordinates differed slightly from the stored ones** [V]: the triangle placement
   follows one rule (`rwcoll.side`: a triangle goes to a child only if it has no vertex on the other
   side and at least two vertices strictly on its own side; crossing triangles and triangles with an
   edge or the whole face in the plane stay at the node) for 188,767 of 190,234 placements (99.2 %);
   the 1,467 exceptions are all triangles touching their plane (97.7 % of those agree) and disagree in
   both directions, as if the touching vertices were a hair above or below at build time. 850 planes
   lie strictly between the coordinates of their node's triangles (taken from an unrelated vertex).
3. The **split choice** could not be pinned down: maximising (triangles below) x (triangles above)
   picks the shipped partition at 69 % of the splits (6,898 / 9,962); balance + straddler costs,
   medians / means snapped to a vertex, and the longest axis all do worse (57 % or less).

**The dmbmx builder** (`rwcoll.tree` / `rwcoll.build`, `AtomicSector.rebuild_collision`): RW's node
format, the 128-node array (nodes 64-127 are leaves), at most 31 triangles per node, the `side` rule;
its own split choice: at every node with 2+ triangles, the vertex coordinate (on any axis) that
maximises below x above with both sides non-empty and at most 31 left at the node (ties: fewer at the
node, the longer axis, the lower plane); no such split = leaf. Triangles are stored in pre-order
(node, low subtree, high subtree), keeping their input order; byte 1 is the lowest-index vertex with the
plane coordinate; unused nodes and the tail are zero. On the shipped geometry it reproduces the whole
structure (node layout, axes, plane coordinates, counts) of 26/784 trees.

**Functionally the rebuilt trees are equivalent** [V, Python ports `rwcoll.line_test` /
`rwcoll.sphere_test` of the walks 0x004BE560 / 0x004BED60]: rebuilding all 784 sectors gives trees that
pass `check()`, and for 31,360 random segments and 31,360 random spheres (40 + 40 per sector) the
original tree, the rebuilt tree and brute force find exactly the same triangles; the walks test
977,626 (original) vs 978,088 (rebuilt) triangles, so the cost is the same. (The triangle tests in the
ports are generic two-sided segment / closest-point tests [I], used only to compare trees.)

**Too big for one tree**: a node needs more than 31 triangles that no plane separates, a node would
start past triangle 255, or the sector has more than 256 vertices: `rwcoll.CollCapacityError`. What RW
does there is unknown (the builder is not in the exe). dmbmx **splits the sector**:
`World.rebuild_collision(split=True)` (the default when writing) replaces it by a PlaneSect with two
sectors, whole triangles going to the side of their centroid (median, longest axis), each half keeping
only its vertices, `leftValue`/`rightValue` set to the halves' extents (overlap allowed, see the walk
above), and recurses. The shipped levels are far from the limits (at most 147 triangles and 256
vertices per sector, 14 triangles in the fullest leaf).

**New geometry and Tag._sd**: the segment test 0x00469B00 looks for a tagged face within 2 units of
the RW hit; when there is none the hit gets code 0 (plain ground) [V], so untagged new geometry is
solid ground.

**In the dmbmx writers**: `World.write()` / `Level.files()` / `dmbmx level rewrite` rebuild the tree of
every sector whose vertices or triangles differ from what was read (`AtomicSector.geometry_changed`),
widen the sector's box and the PlaneSect values above it, and recompute the header counts and
`colSectorSize`. Unedited sectors keep their original 384 bytes, so unedited levels still round-trip
byte for byte. `World.write(rebuild_collision=False)` keeps stale trees (used for the play-test
control).

## 4. Constants [V] (.data 0x005577EC-0x00557893; `dmbmx.formats.physics.CONSTANTS`)

| VA | Value | Use |
|---|---|---|
| 0x00557810 | **1.58** | gravity, per frame^2 (+y = down) |
| 0x00557800 | 0.001 | air drag (velocity x 0.999 per frame) |
| 0x005577EC | 0.0015, 0.0085, 0.0015, 0.019, 0.0018 | rolling drag per surface group; [1] is the grind friction (x (1 + 10 (1 - stat 5))) |
| 0x0055781C | **60** | wall impact speed that bails (x rider stat 4) |
| 0x00557820 | **85** | landing impact that bails (x stat 4) |
| 0x00557824 | 35 | sideways wheel speed at landing that bails |
| 0x0055782C | 16 | take-off speed cap off plain ground (kickers: 11/15/21/23/35, in code) |
| 0x00557818 / 0x00557828 | 60 / 12 | roll: steering lean scales from 12 to 60; handlebar turn only below 12 |
| 0x00557884 | 45 | maximum lean into a turn (degrees) |
| 0x00557838 | 6 | slower than this along an edge = stall instead of grind |
| 0x00557868 / 0x00557870 | 150 / 30 | lock-on nearest-edge radius / edge search radius in the air |
| 0x0055785C / 0x00557860 | 12 / 24 | lock-on look-ahead frames (grind / stall) |
| 0x00557864 | 15 | air auto-rotation frames |
| 0x00557834 | 0.85 | spine hop height factor |
| 0x00557880 | 0.15 | per-frame blend towards a target orientation |
| 0x00557804 / 0x0055787C | 0.35 / 0.3 | yaw-rate decay / slope yaw gain |
| 0x0055783C | 50, 40, 40 | air body box half-size (forward, up, side) for the wall probes |
| 0x00557848 | 0x3777, 0x238E, 0x3777, 0x2000, 0x4000, 0x2EEE, 0x2AAA, 0x238E, 0x271C | angle limits (78, 50, 78, 45, 90, 66, 60, 50, 55 deg): wall vs floor, ramp follow, landing tolerance, grind lean, quarterpipe window, vert |
| 0x00557888 | 0, -1, 0 | up |

There is **no explicit maximum speed**: speed is bounded by the drags, the take-off caps and the
crash thresholds. Bike tuning per rider (rider +0xC68..0xC87, `rider_Create`): centre-of-mass height
50, weight-transfer divisor 5, spring divisor 0.2, suspension {min -15, max 25, stiffness 0.3,
damping 0.1 / 0.1} (the suspension cheat; 0 otherwise [I, DM-046]), wheels at x = 55.25 / -36.52,
radius 24.65.

## 5. The integrators (summary)

- **Roll** (`bmxphysUpdatePositionRoll`): per wheel: wall probes ahead of the leading wheel (+-40 to
  the side; head-on = both wheels bounce at -1/4, else deflect and lose up to 67 %), suspension, a ground
  probe along -up (accepts surfaces within 78 deg of the bike's up, slides along them), weight transfer
  on slopes, surface (front wheel), rolling with gravity along the slope; the frame between the hubs is
  probed (anything there = crash 5); new bike frame from the two hubs, lean into turns, wheel
  velocities re-derived from the moved hubs; both wheels off = AIR. Manual / nose manual: the raised
  wheel is steered around a 40 / 25 deg balance point (`grindBalance` = meter).
- **Air**: gravity + drag; each wheel probes for a landing (impact >= 85 x stat 4 or sideways >= 35 =
  crash; class 10 = relaunch); while both are up, the 8 corners of the body box probe walls (bounce and
  turn along, or crash), and the ground under the hips gives `wallHeight` (air height). Landing while
  a trick is unfinished = bail (crashKind 7/8); landing more than 78 deg off = crash 5; else ROLL.
- **Lock-on** (grind/stall button in the air): the parabola (velocity + gravity) is followed for 12 /
  24 frames with a growing radius; the target is an edge (nearest, or the best aligned within 50
  units), a steep wall face (tricks 0x2A/0x2B: fast plant / wall tap), or a prop of class 10..13; the
  rider then flies `physTimer` frames of `physLockStep` (straight in x/z, a parabola in y) and the
  state change snaps him onto `physLockPos`.
- **Grind**: gravity along the edge, friction; balance (lean > 45 deg = fall off with a push); past the
  end of the edge: continue on a connected edge (within 30, less than 75 deg off) or hop off.

Oddities [V, as compiled]: the spin decay of airborne wheels computes `(int16)(spin * 0.99 * 65536)`
(keeps the fraction bits: the wheels spin erratically instead of slowing); the head-on bounce writes
the other wheel's speed into this wheel; the set-state from "none" calls exit[CRASH].

## 6. Open

- ~~`World.colSectorSize`, format bit 0x40000000~~ resolved in 3.1 (DM-049). Still open: RW's own split
  choice (3.1), and whether the triangle test 0x004BEAB0 culls back faces.
- Exact meanings of materials 4, 7, 8, 0xB and of edge kind 3; face flags 0x08 / 0x40 (not read here).
- The rider-state values `rider_00431dd0(r, level)` that physics tests (rider.c, DM-047).
- In-game confirmation of the class meanings (a play-test could retag a face to class 10 with id 40
  and expect a launch).

## 7. The crash rag doll and the limb IK (`dyn.c`, DM-058)

Source: `decomp/src/zaxis/Bmx/Src/com/zge/bmx/dyn.c` (74 functions, VA 0x00450060-0x00457897, the old
`gap_bmxsound__zoverii.c`), types `decomp/include/dyn.h` and `BmxIk` in `rider.h`, tests
`tests/test_decomp_dm058.py`. The file has no strings, so its name is [I]: it takes DM-047's `dyn*` prefix
and sits between bmxsound.c and zoverii.c, which are linked in alphabetical order within `com\zge\bmx\`. It
is one object file [I]: the rag doll and the IK share helpers, and their .bss statics interleave
(0x005746C4-0x00574D0C). Its .data is two floats at 0x0055B958. The last function of the old gap,
0x004578F0, is zoverii.c's `zoveriiInit`.

### 7.1 Hand-over [V]

- `riderCreate` builds two bodies per rider: `dynCreateRider(riderActor, superCrash)` (cheat 2) into
  `BmxRider.animCtl`, and `dynCreateBike()` into `bikeCtl`. It also sets the impact callbacks
  (`riderOnRiderImpact`, `riderOnBikeImpact`) and picks `dynSetStickyCrash` (cheat 7) or `dynSetNormalCrash`.
- On a crash, `bmxphysStateInitCrash` (physics state 2) sets `vel.y -= 5`. Then `dynStartRider` puts the
  particles back on the skeleton's joints (the current pose, `zactorGetBonePos`). Every particle gets **half
  the rider's velocity**, the body centre goes to the pelvis and the actor's rotation is cleared. The rider's
  spin (`rotVel`) is passed in but not read. The first step runs without limits or stick input.
  `dynStartBike` places the 3-particle triangle with the bike matrix and half the velocity; physics passes a
  zero spin.
- Every crash frame, `bmxphysUpdatePositionCrash` calls `dynStepRider`. physics then:
  - copies the actor's decoder block 1 angles into block 0 (the drawn pose);
  - sets the rider matrix from the pelvis particle's frame;
  - sets the position from the body centre (+0xE70) and the velocity from particle 0.

  `dynStepBike` moves the bike the same way. rider.c's `riderStateCrash` handles the player's input and ends
  the crash after 5 s, after 16 frames nearly at rest, or on a splash.

### 7.2 The particle system [V]

A `DynBody` (0xE88 bytes, from `zmemTempAlloc`) holds up to 15 particles (0xB4 each), 22 springs (0x20),
7 limits (0x28), the mass centre and the impact callback. One step (`dynStep`) does this:

1. **Integrate**: `prev = pos`. Moving particles get `vel.y += 1.58` and `pos += vel`. The 1.58 is gravity
   (`g_dynGravity` 0x0055B958), the same 1.58 / frame^2 as the rider physics; +y points down. Positions are
   then made relative to the centre.
2. **Limits** (skipped in the first step): one-sided plane constraints. Each pushes a knee or elbow back onto
   its side of a plane through a pivot joint; the plane's normal comes from a segment frame. The push is the
   limit's strength times the particle's share of the two masses. The type-7 variant moves at most 4 units per
   step.
3. **Springs**, in 2 relaxation passes. Each particle's new position is a weighted mean of:
   - its own position, with its mass as the weight;
   - for each spring, the other end plus or minus the rest length along the current direction, with the
     spring's weight.

   Fixed particles keep their place.
4. **World**: every moving particle is swept as a **sphere of radius 20**, from `prev` to `pos + vel`,
   through `zbspColSphere` (level faces and props).
   - Each face found gives one contact: 4 bisection steps along the sweep, then a push out to exactly 20 from
     the plane. The contacts are averaged.
   - The velocity loses its normal part, is scaled by `1 - friction x 0.35`, then gets the normal part back
     times `bounce x 0.95`. Below 1 unit per step the particle stops.
   - Particles faster than 100 units per step skip collision.
   - A hit calls the impact callback with the particle, the normal speed and an `NgColHit` (the world face's
     material, or the prop). rider.c plays the crash sounds and sparks from it.
5. **Centre**: the centre moves by the mass-weighted mean displacement.

There is **no separate damping**: speed is lost only in collisions, through the friction term. Nothing in this
file caps the speed except the 100-per-step collision cut-off.

### 7.3 The rider body [V offsets and tables, I joint names]

The rider body has 14 particles on skeleton joints. Each sits at its bone's position (`zactorGetBonePos`); the
bone names are `dmbmx.formats.motion.RIDER_CHANNELS`.

| # | Joint | Bone | | # | Joint | Bone |
|---|---|---|---|---|---|---|
| 0 | pelvis (mass 0.4) | 1 Chest | | 7 | right shoulder | 6 RightUpArm |
| 1 | right hip | 12 RightUpLeg | | 8 | left shoulder | 3 LeftUpArm |
| 2 | left hip | 9 LeftUpLeg | | 9 | right elbow | 7 RightLowArm |
| 3 | right knee | 13 RightLowLeg | | 10 | left elbow | 4 LeftLowArm |
| 4 | left knee | 10 LeftLowLeg | | 11 | right wrist | 8 RightHand |
| 5 | right ankle | 14 RightFoot | | 12 | left wrist | 5 LeftHand |
| 6 | left ankle | 11 LeftFoot | | 13 | head top (25 up the Head bone) | 2 Head |

All particles have mass 0.1 (the pelvis 0.4), bounce 0.6 (**1.75 with Super Crash**) and friction 1.0. The
body is lifted so its lowest particle is at y = 0.

It has **21 springs** (the number is the spring's weight):
- torso box: pelvis-hips and hip-hip 24; shoulders-pelvis 48; shoulder-shoulder 24; shoulders-hips 48;
- limbs, all 24: hip-knee, knee-ankle, shoulder-elbow, elbow-wrist;
- neck: head-pelvis 4.8, head-shoulders 24.

It has **6 limits**: the elbows (strength 1.2) and the knees (0.5, plus two type-7 limits at 1.0).

The bike body is a triangle: (0, 40, 0) and (+/-30, 0, 0) in the bike frame, with mass 0.1, bounce 0.8,
friction 0.4 and 3 springs of weight 16. The bike's matrix is the triangle's frame with its axes rotated.

### 7.4 Player control during a crash [V code, I in-game meaning]

- **Tuck**: ACT_GO_LIMP (Circle) held more than 1 m above the ground calls `dynTuck`.
  - The limb springs and limits turn off.
  - Every step, the elbows, wrists, knees and ankles move 20 % of the way towards fixed points in the pelvis
    and hip frames, so the rider **curls into a ball**.
  - Nothing happens while a particle is touching something. `dynUntuck` restores the springs and limits.
- **Spin**: the stick, more than 1 m above the ground, calls `dynSetPitchInput` / `dynSetRollInput`
  (+/-10 deg).
  - Each step, particles 1-12 turn about the pelvis: pitch about its x axis, roll about its y axis.
  - They are then pushed out of the world. The player **spins the tumbling body**.
- **Grab**: ACT_RAGDOLL (Triangle) held calls `dynGrab`.
  - Each step, the game looks for grind edges within 400 of the right knee.
  - A wrist whose nearest edge point is closer than 120 is **pinned 10 units above it** (`g_dynGrabDrop`,
    -10), so the rider **hangs from a rail or ledge**. `dynIsHanging` reports it; rider.c's state is then
    CRASH_STEER.
  - It works once per crash. `dynRelease` lets go.
- **Sticky Crash** (cheat): `dynSetStickyCrash` makes a particle that touches anything fixed for good.

Some functions are never called:
- DM-058 created four that are reachable only through their incremental-link thunks: `dynBrace` (arms held
  out 54 / 29 units in front of the shoulders), `dynSetLimitsOff`, `dynSetLimitsOn` and `dynIkSetLooseGains`.
- Also uncalled: `dynAddBendSpring`, `dynAddAngleSpring` (its solver is empty), `dynAddAxisLimit` and
  `dynIkBlendAxes`.

Bugs in the original [V]:
- `dynCreateRider` stores all three neck spring indices in one variable. So `dynUntuck` sets the
  head-to-left-shoulder spring to 4.8 (it was created at 24) and resets spring 0 to 24.
- The actor position mixes the pelvis and hips, but its y and z terms use the wrong particles.
- The type-7 limit checks particle a's type but moves particle b.

### 7.5 From the rag doll to the pose [V]

After the step, `dynStepRider` builds a frame for each body segment from the particles:
- shoulders from particles 7-8 and 8-0; pelvis from 1-2 and 0-1; the head;
- thighs and upper arms, by blending the parent segment's axes;
- shins, as a hinge about the thigh's x axis;
- forearms, reflected through the upper arm's right axis.

With the limits off or while tucked, the shins and forearms are free frames instead. The frames are stored in
the particles, where the limits read them. The pose comes from the frames:
- The pelvis frame becomes the actor's orientation, and its angles go to the **Hips** channel.
- Every other channel gets the Euler angles of its segment relative to the parent segment: Chest (relative to
  the pelvis), Head, Left/RightUpArm (to the shoulders), Left/RightUpLeg (to the pelvis), Left/RightLowLeg
  (to the thigh) and Left/RightLowArm (to the upper arm). Hands and feet are zero.

These angles go to the actor's **decoder block 1** (`ZActorAnim.block[1].rot`). physics.c copies them to block
0, which `NgIFzactor_ApplyMotionPose` turns into the model's frame quaternions (ngif.md 3). The actor stands at
0.5 x the pelvis plus 0.25 x each hip.

### 7.6 The limb IK [V]

The IK state is `BmxRider.ik` (`BmxIk`, at +0xD98, 0x1D8 bytes). DM-046/047 called it a camera block, and named
`dynIkReset` / `dynIkMeasure` `cameraInitRider` / `cameraMeasureRider`.

`dynIkMeasure` (called by riderCreate) reads the limb lengths from the model with `zactorGetPartLength`. A
limb's reach is its two lengths added (arms: minus 1). Each frame physics.c sets targets:
- `bmxphysSetLimbTargets`: the hands go to the bike's grips (LeftGrip / RightGrip, 22 up), bent like the
  forearms;
- `bmxphysPostAir`: targets for grab tricks (bike bones 5..8).

`dynIkSolve` then solves each limb that has a target, with a two-bone law of cosines:
- the solve works in the plane of the target and the bend direction;
- the target is clamped to the limb's reach;
- the Up/Low channels of the arms (relative to root x Chest) and the legs (relative to the root) go into
  block 0.

The gains that `dynIkSetDefaultGains` sets (0.1, 0.01) are never read.

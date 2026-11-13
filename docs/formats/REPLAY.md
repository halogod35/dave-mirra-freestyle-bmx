# REPLAY: replay buffers and the replay.zal records

Owner: DM-052 (Agent H8). Code: `tools/dmbmx/formats/replay.py` (reader, writer, event codec, playback and
recorder ports), CLI `dmbmx replay list|dump`. Decomp: `decomp/src/zaxis/Bmx/Src/com/action/replay.c`,
`decomp/include/replay.h`. Tests: `tests/test_replay_unit.py`, `tests/test_replay_corpus.py`,
`tests/test_cli_replay.py`, `tests/test_decomp_dm052.py`. Answers DM-016's open question 5 (ANIM.md 6).

## 1. What a replay is [V]

`replay.c` records one player's pad and, optionally, snapshots of the rider, the props and the goal
progress. Up to 5 replays exist (`g_replays[5]` 0x0056D278, handles 0..4).

- game.c creates one per player at the start of a session (`BmxReplayDesc` flags 1, stream
  0x1000 / players bytes; with 2 players also a shared one with flags 7) and records every run
  (`bmxreplaySetPlayer`, `bmxreplaySetMode(h, 1)`).
- action.c plays them back for "view replay" (`bmxreplaySetMode(h, 2)`, 4 = reached the end,
  Triangle = 4 then 2 = from the start) and runs the **attract demo** from replay.zal.
- `bmxreplayUpdate` 0x0042FD50 runs once per simulated frame: in record mode it writes events, in
  play mode it overwrites `g_riders[player].input` (buttons, changed, sticks, previous sticks) from
  the stream, so the normal rider/physics code replays the run.

## 2. The buffer (`zmemTempAlloc`, bmxreplayCreate 0x00430180) [V]

| Buffer offset | Size | Content |
|---|---|---|
| 0x00 | 0x40 | `BmxReplay` header (below) |
| 0x40 | dataSize | input event stream (dataSize = desc size rounded up to 4, or 0x2000) |
| riderStart / riderLive | 2 x 0x2DE8 | `BmxReplayRider` snapshots, unless flags & 1 |
| propsStart / propsLive | 2 x 0xB40 | `BmxPropState` (16 `BmxProp` + 64 `BmxPropSwitch`, bmxprop.h), unless flags & 2 |
| goalsStart / goalsLive | 2 x 0x27 | `SaveGoalInfo` of the player on this level (SAVE.md), unless flags & 4 |

"Start" = the state when recording began, "Live" = the state saved when playback started and
restored when it stops. Goal info is only copied in proquest on levels 0..5.

**BmxReplay (0x40)**

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | i32 | tempMark | zmemTempMark() before the allocation |
| 0x04 | u8 | mode | 0 off, 1 record, 2 play, 4 end |
| 0x05 | u8 | delta | record: frames since the last event; play: frames to the next |
| 0x06 | u8 | player | rider slot |
| 0x07 | u8 | flags | 1 no rider, 2 no props, 4 no goals |
| 0x08 | u32 | frame | |
| 0x0C | u32 | controlConfig | rider's controller configuration (BmxRider +4) when recording started |
| 0x10 | u32 | frames | length (30 per second) |
| 0x14..0x28 | u32 x 6 | riderStart, riderLive, propsStart, propsLive, goalsStart, goalsLive | buffer offsets, 0 = absent (goals are not zeroed when absent) |
| 0x2C | u32 | dataOffset | 0x40 |
| 0x30 | u32 | dataSize | |
| 0x34 | ptr | cursor | next event |
| 0x38 | u32 | buttons | buttons after the last event |
| 0x3C | s8[2][2] | stick | stick n stored as (y, x) bytes, value x 127 |

**BmxReplayRider (0x2DE8)**: BmxRider 0xF70, rider actor 0x278, actor +0x48 block 0xD60, rag-doll
body (`animCtl`) 0xE88, bike position 3 x f32 (+0x2DD0), bike root height f32 (+0x2DE0), pad buttons (+0x2DE4).

## 3. Event stream [V]

Every event is 3 bytes:

```
u8  head   kind << 5 | delta      delta 0..31 = frames since the previous event (0 = same frame)
u16 value  little-endian
kind 0: XOR mask of the low 16 button bits (PAD_* in contrler.h; Escape/Enter, bits 16-17, are not recorded)
kind 1: stick 0, kind 2: stick 1: lo byte = y * 127, hi byte = x * 127 (s8)
```

Recorder (bmxreplayUpdate, record mode, per frame): delta++; if the buttons changed **or delta reached
31**, write a kind-0 event (the keep-alive has mask 0); then for each stick whose s8 pair changed,
write a kind-1/2 event (delta 0 if a button event was just written). Stopping (any mode change away
from 1) writes a closing kind-0 event with mask 0 and delta + 1. So the deltas of the used events sum
to **frames + 1**. A full buffer prints "bmxreplayUpdate: reached end of replay buffer" and stops.

Player (play mode, per frame): the sticks of the last frame become `prevStick`; delta--; while delta
reaches 0, apply the event under the cursor (kind 0: `changed` = mask; 1/2: stick; kinds 3..7 only
advance the cursor by **one** byte) and load the next delta; then `buttons ^= changed`, the sticks
are scaled back by 1/127, and at frame == frames the mode becomes 4. Playing starts with buttons =
the rider snapshot's (0 without one) and sticks 0.

Ports: `replay.play_stream` / `Replay.play()` and `replay.record()`; `record(play(r)) == r.events()`
for all 7 PC records [V].

## 4. replay.zal records [V]

A record is the buffer from +0x0C to its end: the area `bmxreplayGetSaveArea` 0x00430D70 returns
(`size = dataSize + 0x34 (+ 0x5BD0) (+ 0x1680) (+ 0x4E)` per the flags). The attract demo (action.c)
creates a replay with flags 7 and a 0x2800 stream, asks for that area (0x2834 bytes), checks the
asset size ("replay data too big for demo replay buffer") and loads asset `rider + 2 * level` of
`g_replayZal` over it. The flags byte (+7) stays 7, so the record's snapshot blocks are ignored;
`frames`, `dataOffset`, `dataSize` come from the record; `bmxreplaySetMode(2)` rewinds the cursor.

24 records (one per rider 0/1 x level 0..11). Three layouts:

| Layout | Ids | Size | Content |
|---|---|---|---|
| pc | 0-4, 8, 10 | 0x2702 | 0x34 header bytes, 0x1000 stream, props (2 x 0xB40), goals (2 x 0x27): flags 1, recorded by game.c in single player |
| psx | 5-7, 11-23 | 0x103C / 0x183C | console recorder: 0x3C-byte header (+0x08 frames, +0x1C data offset 0x3C, +0x20 data size, +0x24 data pointer, +0x28 cursor), stream of 3-byte events (cursor - data pointer = used bytes, always whole events) |
| raw | 9 | 0x1828 | none of the above (+0x0C = 0x34, +0x10 = 0x1800); kept verbatim |

- The demo plays `g_actionDemoRuns` {0, 4}, {1, 0}, {1, 1} = records **8, 1 and 3**: all PC records
  that fit the 0x2834-byte area [V, tests].
- A psx record would not play on the PC: its +0x04 (frames for the PC) is 0 for most, and
  `bmxreplaySetMode(2)` turns a 0-frame replay off [V by code; not reached in the shipped demo].
- In the PC records `riderStart` = 0, `propsStart` = 0x1040, `goalsStart` = 0x26C0; the stale
  `cursor` is a heap pointer of the recording machine: 0x01F8xxxx (records 1, 3, 4, 8) or 0x8C79xxxx
  (0, 2, 10; Dreamcast RAM is at 0x8C000000 [I]). The props blocks match what bmxpropInit sets up
  on that level (slot index, table links, Greenville Dirt traffic already moving) and, for the four
  PC-made ones, exactly the slots and breakable props the PC level data binds [V tests]. Some goal
  blocks are uninitialised memory (recorded outside proquest).
- The bytes after the used events of a stream are leftover memory; `Replay.slack` keeps them.

`dmbmx.formats.replay.read(data).write() == data` for all 24 records [V].

## 5. Editing

To make a new demo: build events with `replay.record(frames)` (or edit `r.events()`),
`encode_events`, pad the stream to `data_size` (the demo accepts up to 0x2800), set `frames` and keep
the block offsets consistent (`Replay._check_blocks`). The total must stay <= 0x2834 bytes, and
the ZAL asset size field must match (dmbmx zal repack). Unverified in game.

## 6. Open questions

1. The psx/raw records' event semantics (deltas do not add up to their frame field with the PC
   rules; kinds 3..7 occur). Not needed by the PC build.
2. Which machine recorded which PC record (the pointer heuristic above is [I]).

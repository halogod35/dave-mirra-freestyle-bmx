"""replay.zal records: saved bmxreplay buffers (DM-052). Spec: docs/formats/REPLAY.md.

A record is the part of a replay buffer from header +0x0C to its end (bmxreplayGetSaveArea
0x00430D70); action.c's attract demo loads record ``rider + 2 * level`` into a fresh buffer and plays
it. Three layouts occur in the shipped replay.zal (24 records):

- ``"pc"``  (7 records, 0x2702 bytes): the PC recorder's buffer. 0x34 header bytes (= BmxReplay
  +0x0C..+0x3F), the input event stream (``data_size`` bytes, at buffer offset 0x40), then the
  snapshot blocks the recording kept (rider 2 x 0x2DE8, props 2 x 0xB40, goal info 2 x 0x27).
- ``"psx"`` (16 records): a console recorder's buffer with a 0x3C-byte header (data offset 0x3C).
  The PC never plays them; their header is kept as raw bytes, the stream is split off.
- ``"raw"`` (1 record, id 9): anything else, kept verbatim.

Event stream (PC, [V] bmxreplayUpdate 0x0042FD50): 3 bytes per event, ``head = kind << 5 | delta``,
then a little-endian u16. ``delta`` = frames since the previous event (1..31; 0 = same frame);
kind 0 = XOR mask of the 16 low button bits, 1 / 2 = stick 0 / 1 as two s8 (``lo`` = y * 127,
``hi`` = x * 127). The recorder adds a mask-0 event when delta reaches 31 and a closing mask-0 event
(delta + 1) when it stops, so the deltas of the used events sum to ``frames + 1``. The bytes after the
last used event are whatever the buffer held (kept as ``slack``).

    rec = replay.read(data); rec.write() == data
    rec.events()            # [Event(delta, kind, value)], the used part of the stream
    rec.play()              # per-frame inputs, as bmxreplayUpdate plays them back
    replay.encode_events(evs) / replay.record(frames) (port of the recorder)
"""
import struct
from collections import namedtuple

from ..core.errors import FormatError, LimitError, TruncatedError

FMT = "REPLAY"

HEADER = 0x40              # BmxReplay
SAVE_START = 0x0C          # a record starts at header +0x0C
PC_HEADER = HEADER - SAVE_START   # 0x34 bytes of header in a record
PSX_HEADER = 0x3C
RIDER_SIZE = 0x2DE8
PROPS_SIZE = 0xB40
GOALS_SIZE = 0x27
DEMO_BUFFER = 0x2800       # action.c: stream bytes of the demo replay
DEMO_SAVE_AREA = DEMO_BUFFER + PC_HEADER   # largest record the demo accepts (flags 7)
MAX_DATA = 0x100000        # sanity cap for a stream size field

NO_RIDER, NO_PROPS, NO_GOALS = 1, 2, 4      # REPLAYF_*

EV_BUTTONS, EV_STICK0, EV_STICK1 = 0, 1, 2
DELTA_MAX = 0x1F

# record offsets 0x00..0x33 = BmxReplay +0x0C..+0x3F (13 u32 + 4 stick bytes)
PC_FIELDS = ("control_config", "frames", "rider_start", "rider_live", "props_start", "props_live",
             "goals_start", "goals_live", "data_offset", "data_size", "cursor", "buttons")
_PC = struct.Struct("<12I4s")

Event = namedtuple("Event", "delta kind value")
Frame = namedtuple("Frame", "buttons changed sticks")   # sticks: ((x0, y0), (x1, y1)) as s8 x 127


class ReplayError(FormatError):
    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt=FMT, offset=offset)


def _u32(data, off):
    if off + 4 > len(data):
        raise TruncatedError("header runs past the end", fmt=FMT, offset=off)
    return struct.unpack_from("<I", data, off)[0]


def layout_of(data):
    """``"pc"`` / ``"psx"`` / ``"raw"`` from the data-offset fields (see the module doc)."""
    if len(data) >= PC_HEADER and _u32(data, 0x20) == HEADER:
        return "pc"
    if len(data) >= PSX_HEADER and _u32(data, 0x1C) == PSX_HEADER:
        return "psx"
    return "raw"


class Replay:
    """One replay.zal record. ``layout``; for ``pc``: ``fields`` (PC_FIELDS -> int), ``stick``
    (4 bytes: y0, x0, y1, x1 as s8), ``stream`` (data_size bytes), ``blocks`` (the snapshot blocks,
    raw); for ``psx``: ``header`` (0x3C raw bytes), ``stream``, ``blocks`` (anything after the
    stream); for ``raw``: ``blocks`` = the whole record."""

    def __init__(self):
        self.layout = "raw"
        self.fields = {}
        self.stick = b"\0" * 4
        self.header = b""
        self.stream = b""
        self.blocks = b""

    # ------------------------------------------------------------------ read / write
    @classmethod
    def read(cls, data):
        data = bytes(data)
        r = cls()
        r.layout = layout_of(data)
        if r.layout == "pc":
            vals = _PC.unpack_from(data, 0)
            r.fields = dict(zip(PC_FIELDS, vals[:12]))
            r.stick = vals[12]
            size = r.fields["data_size"]
            if size > MAX_DATA:
                raise LimitError(f"stream size 0x{size:x} too large", fmt=FMT, offset=0x24)
            end = PC_HEADER + size
            if end > len(data):
                raise TruncatedError(f"stream of 0x{size:x} bytes runs past the end", fmt=FMT, offset=0x24)
            r.stream = data[PC_HEADER:end]
            r.blocks = data[end:]
            r._check_blocks()
        elif r.layout == "psx":
            size = _u32(data, 0x20)
            if size > MAX_DATA:
                raise LimitError(f"stream size 0x{size:x} too large", fmt=FMT, offset=0x20)
            end = PSX_HEADER + size
            if end > len(data):
                raise TruncatedError(f"stream of 0x{size:x} bytes runs past the end", fmt=FMT, offset=0x20)
            r.header = data[:PSX_HEADER]
            r.stream = data[PSX_HEADER:end]
            r.blocks = data[end:]
        else:
            r.blocks = data
        return r

    def write(self):
        if self.layout == "pc":
            f = self.fields
            if f["data_size"] != len(self.stream):
                raise ValueError("data_size does not match the stream length")
            return _PC.pack(*(f[k] & 0xFFFFFFFF for k in PC_FIELDS), bytes(self.stick)) + self.stream + self.blocks
        if self.layout == "psx":
            return self.header + self.stream + self.blocks
        return bytes(self.blocks)

    # ------------------------------------------------------------------ PC layout
    def flags(self):
        """REPLAYF_* inferred from the block offsets (the flags byte itself is not saved)."""
        f = self.fields
        return ((0 if f["rider_start"] else NO_RIDER) | (0 if f["props_start"] else NO_PROPS) |
                (0 if f["goals_start"] else NO_GOALS))

    def _block_offsets(self):
        """{name: buffer offset} the recorder lays out after the stream (bmxreplayCreate)."""
        fl, off, out = self.flags(), HEADER + self.fields["data_size"], {}
        for bit, names, size in ((NO_RIDER, ("rider_start", "rider_live"), RIDER_SIZE),
                                 (NO_PROPS, ("props_start", "props_live"), PROPS_SIZE),
                                 (NO_GOALS, ("goals_start", "goals_live"), GOALS_SIZE)):
            if not fl & bit:
                out[names[0]], out[names[1]] = off, off + size
                off += 2 * size
        return out, off

    def _check_blocks(self):
        want, end = self._block_offsets()
        for name, off in want.items():
            if self.fields[name] != off:
                raise ReplayError(f"{name} = 0x{self.fields[name]:x}, the layout puts it at 0x{off:x}",
                                  offset=PC_FIELDS.index(name) * 4)
        if end - SAVE_START != PC_HEADER + len(self.stream) + len(self.blocks):
            raise ReplayError(f"record is {PC_HEADER + len(self.stream) + len(self.blocks):#x} bytes, "
                              f"the blocks end at {end - SAVE_START:#x}")

    def block(self, name):
        """Raw bytes of a snapshot block ("rider_start", "props_live", ...), or None if not kept."""
        if self.layout != "pc":
            return None
        want, _ = self._block_offsets()
        if name not in want:
            return None
        size = RIDER_SIZE if name.startswith("rider") else PROPS_SIZE if name.startswith("props") else GOALS_SIZE
        off = want[name] - HEADER - len(self.stream)
        return self.blocks[off:off + size]

    def stream_events(self, limit=None):
        """Every 3-byte event of the stream area (also the slack), as Event tuples."""
        s = self.stream
        n = len(s) // 3 if limit is None else min(limit, len(s) // 3)
        return [Event(s[3 * i] & DELTA_MAX, s[3 * i] >> 5, s[3 * i + 1] | s[3 * i + 2] << 8) for i in range(n)]

    def used_length(self):
        """Bytes of the stream that hold the recording: events up to the one whose deltas reach
        frames + 1 (PC). PSX: the header's cursor - data pointer [V: always a whole number of
        events]."""
        if self.layout == "psx":
            ptr, cur = struct.unpack_from("<II", self.header, 0x24)
            used = cur - ptr
            if not 0 <= used <= len(self.stream):
                raise ReplayError(f"cursor 0x{cur:x} outside the stream", offset=0x28)
            return used
        if self.layout != "pc":
            return 0
        want, total, i = self.fields["frames"] + 1, 0, 0
        s = self.stream
        while total < want:
            if 3 * i + 3 > len(s):
                raise ReplayError(f"stream ends after {total} of {want} frames", offset=PC_HEADER + 3 * i)
            total += s[3 * i] & DELTA_MAX
            i += 1
        if total != want:
            raise ReplayError(f"event deltas reach {total}, not frames + 1 = {want}", offset=PC_HEADER + 3 * i)
        return 3 * i

    def events(self):
        """The recorded events (PC layout)."""
        return self.stream_events(self.used_length() // 3)

    @property
    def slack(self):
        return self.stream[self.used_length():]

    def play(self):
        """Per-frame input as bmxreplayUpdate plays the stream back (``frames`` Frame tuples):
        buttons (16-bit), changed (the XOR mask of that frame), sticks ((x0, y0), (x1, y1)) as s8."""
        if self.layout != "pc":
            raise ReplayError("only the PC layout can be played")
        self.used_length()                 # frames must be covered by the stream (bounds the loop)
        return play_stream(self.stream, self.fields["frames"])


def _s8(v):
    return v - 256 if v > 127 else v


def play_stream(stream, frames):
    """Port of the playback half of bmxreplayUpdate (and bmxreplaySetMode(2)'s rewind)."""
    stream = bytes(stream)
    end = len(stream) - 3              # the exe: cursor < data + size - 3
    cur, buttons = 0, 0
    stick = [[0, 0], [0, 0]]           # as stored: [n][0] = y byte, [n][1] = x byte
    delta = stream[0] & DELTA_MAX if stream else 0
    out = []
    for _ in range(frames):
        changed = 0
        delta = (delta - 1) & 0xFF
        while delta == 0 and cur < end:
            kind, v = stream[cur] >> 5, stream[cur + 1] | stream[cur + 2] << 8
            if kind == EV_BUTTONS:
                changed = v
                cur += 3
            elif kind <= EV_STICK1:
                stick[kind - 1] = [v & 0xFF, v >> 8]
                cur += 3
            else:
                cur += 1
            delta = stream[cur] & DELTA_MAX
        buttons ^= changed
        out.append(Frame(buttons, changed, tuple((_s8(s[1]), _s8(s[0])) for s in stick)))
    return out


def encode_events(events):
    """Events -> stream bytes (3 bytes each)."""
    out = bytearray()
    for e in events:
        if not (0 <= e.delta <= DELTA_MAX and 0 <= e.kind <= 7 and 0 <= e.value <= 0xFFFF):
            raise ValueError(f"event out of range: {e}")
        out += bytes((e.kind << 5 | e.delta, e.value & 0xFF, e.value >> 8))
    return bytes(out)


def record(frames):
    """Port of the recording half of bmxreplayUpdate + the closing event of bmxreplaySetMode.
    ``frames``: iterable of (buttons, ((x0, y0), (x1, y1))) per frame, sticks as s8 (x 127).
    Returns the events (the initial buttons and sticks are 0, as when recording without a rider
    snapshot)."""
    evs, delta, last = [], 0, 0
    stick = [0, 0]
    n = 0
    for buttons, sticks in frames:
        n += 1
        delta += 1
        if buttons != last or delta == DELTA_MAX:
            evs.append(Event(delta, EV_BUTTONS, (last ^ buttons) & 0xFFFF))
            delta = 0
            last = buttons
        for i in range(2):
            x, y = sticks[i]
            v = (x & 0xFF) << 8 | (y & 0xFF)
            if v != stick[i]:
                evs.append(Event(delta, i + 1, v))
                delta = 0
                stick[i] = v
    evs.append(Event(delta + 1, EV_BUTTONS, 0))
    return evs


def read(data):
    return Replay.read(data)


def write(rec):
    return rec.write()


def records(archive):
    """{id: Replay} of every record of a replay.zal archive (dmbmx.formats.zal.ZalArchive)."""
    return {e.id: Replay.read(e.data()) for e in archive.entries}

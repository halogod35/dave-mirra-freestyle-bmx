"""Sound banks ``bin/audio/*.zsd`` (magic ``ZSND`` ``WIN ``). Not the level ``.ZSD`` scenes (``SCEN``).

Spec: docs/formats/AUDIO.md (DM-015, DM-055). Loader: ``zsndRegisterAsset`` 0x0046FD00 (winLoad.c);
the game side (mixer, sequencer, event map): docs/re/sound.md and decomp/src/.../sound/*.c, zsnd*.c.

Layout (all LE; table pointers are file offsets, relocated at load):

    0x00 char[4] 'ZSND'   0x04 char[4] 'WIN '   (both strncmp-checked by the loader)
    0x08 u32 total file size
    0x0C u32 end of the header area (tables + ZTRK data); sample data starts at align4(this)
    0x10 u32 nA, 0x14 u32 offA   voices, 20 bytes each       (+0x12 runtime bank handle)
    0x18 u32 nB, 0x1C u32 offB   samples (sample defs), 24 bytes each (+0 u16 sample file or 0xFFFF)
    0x20 u32 nC, 0x24 u32 offC   sample files: (u32 offset, u32 size)
    0x28 u32 nD, 0x2C u32 offD   phrases (empty in all banks) u32 offsets of records
    0x30 u32 nE, 0x34 u32 offE   track defs, 16 bytes, +8 = offset of a ZTRK record (level banks)
    0x38 u32, 0x3C u32           (0, end of E)
    0x40 u32 nF, 0x44 u32 offF   keymaps (empty in all banks)
    0x48 ...                     tables, ZTRK data, zero padding to 4, then the sample files
                                 back to back in table order; nothing after the last one.

Sample encodings (the bank does not store them; the exe decides):
* banks with sample defs (``sfx*``): signed 16-bit mono PCM at the rate in the sample def;
* stream banks without sample defs (``music``, ``speech``): IMA-ADPCM, mono, 44100 Hz, low nibble
  first, one continuous stream starting at predictor 0 / step index 0 (music: flags 0x44 passed
  to the stream opener 0x00472030 at 0x0044E860; decoder 0x004724C0, tables 0x0055F1AC/0x0055F1EC).

``write(read(b)) == b`` for every bank in the install. ``write`` always lays the sample files out
canonically, so replacing a sample with one of a different size just works.

DM-055 (from the exe's sound code): ``Voice`` / ``TrackDef`` decode tables A / E, ``ztrk_decode`` /
``ztrk_encode`` the ZTRK sequencer byte code (round-trips every track), and ``voice_events`` names a
bank's voices by the bmxsoundEvent codes that play them.
"""
import struct
from dataclasses import dataclass, field

from dmbmx.core.errors import BadMagicError, ChecksumError, FormatError, LimitError, TruncatedError, need

FMT = "AUDIO"
MAGIC, PLATFORM = b"ZSND", b"WIN "
HEADER_LEN = 0x48
A_LEN, B_LEN, C_LEN, E_LEN = 20, 24, 8, 16
MAX_ENTRIES = 1 << 16
STREAM_RATE = 44100

# table name -> (count offset, pointer offset, entry size or None for u32 offset lists)
TABLES = {"A": (0x10, 0x14, A_LEN), "B": (0x18, 0x1C, B_LEN), "C": (0x20, 0x24, C_LEN),
          "D": (0x28, 0x2C, 4), "E": (0x30, 0x34, E_LEN), "F": (0x40, 0x44, 4)}


# SampleDef.flags (the mixer zsndcMix 0x0046F5F0 / zsndcAlloc 0x0046E410) [V]
SAMPLE_LOOP = 0x0001       # loop back to loop_start at the end of the sample file
SAMPLE_STEREO = 0x0002     # interleaved stereo frames (no bank uses it)
SAMPLE_8BIT = 0x0004       # unsigned 8-bit (no bank uses it)
SAMPLE_PINGPONG = 0x0008   # with LOOP: back and forth (no bank uses it)


@dataclass
class SampleDef:
    """24-byte sample def (table B; the exe calls it a "sample", ZsndSample)."""
    sample: int        # +0x00 u16 index into the sample files, 0xFFFF = none                [V]
    flags: int         # +0x02 u16 SAMPLE_* (only 0 / LOOP occur)                             [V]
    rate: int          # +0x04 u32 sample rate (Hz); playback step = pitch x rate / mix rate [V]
    ptr: int           # +0x08 u32 runtime data pointer (0 in files)                         [V]
    loop_start: int    # +0x0C u32 loop start, sample frames                                  [V]
    loop_end: int      # +0x10 u32 never read by the PC build: a loop always runs to the end
    #                    of the sample file, so 0 just means "not filled in" [V]
    x14: int           # +0x14 u16 (0 in files; not read)                                    [V]
    handle: int        # +0x16 u16 runtime bank handle (0 in files)                          [V]

    FMT = "<HHIIIIHH"

    @classmethod
    def parse(cls, b):
        return cls(*struct.unpack(cls.FMT, b))

    def pack(self):
        try:
            return struct.pack(self.FMT, self.sample, self.flags, self.rate, self.ptr, self.loop_start,
                               self.loop_end, self.x14, self.handle)
        except struct.error as e:
            raise ValueError(f"sample def field out of range: {e}") from e


@dataclass
class Bank:
    header: bytearray                 # bytes [0, data_end): header + tables + ZTRK, kept verbatim
    samples: list = field(default_factory=list)   # sample file payloads (bytes), table C order
    pad: bytes = b""                  # bytes between data_end and the first sample (zeros)

    # ---- header fields ----
    def _u32(self, off):
        return struct.unpack_from("<I", self.header, off)[0]

    @property
    def data_end(self):
        return self._u32(0x0C)

    def count(self, t):
        return self._u32(TABLES[t][0])

    def offset(self, t):
        return self._u32(TABLES[t][1])

    @property
    def sample_defs(self):
        o = self.offset("B")
        return [SampleDef.parse(self.header[o + i * B_LEN:o + (i + 1) * B_LEN]) for i in range(self.count("B"))]

    def set_sample_def(self, i, sd):
        o = self.offset("B") + i * B_LEN
        self.header[o:o + B_LEN] = sd.pack()

    def sound_defs(self):
        o = self.offset("A")
        return [bytes(self.header[o + i * A_LEN:o + (i + 1) * A_LEN]) for i in range(self.count("A"))]

    @property
    def voices(self):
        """Table A as ``Voice`` records."""
        return [Voice.parse(b) for b in self.sound_defs()]

    @property
    def track_defs(self):
        """Table E as ``TrackDef`` records."""
        o = self.offset("E")
        return [TrackDef.parse(self.header[o + i * E_LEN:o + (i + 1) * E_LEN]) for i in range(self.count("E"))]

    def ztrk_ranges(self):
        """[(start, end)] of each track def's ZTRK record in the header area (end = the next
        record or the end of the header area)."""
        tds = self.track_defs
        starts = sorted({td.ztrk for td in tds})
        lo = self.offset("E") + E_LEN * len(tds)
        out = []
        for td in tds:
            nxt = [s for s in starts if s > td.ztrk]
            end = nxt[0] if nxt else self.data_end
            if not (lo <= td.ztrk and td.ztrk + 4 <= end <= self.data_end):
                raise FormatError(f"ZTRK record 0x{td.ztrk:x} outside the header area", fmt=FMT)
            out.append((td.ztrk, end))
        return out

    def ztrk_tracks(self):
        """[(TrackDef, [ZtrkEvent])] for every track def."""
        return [(td, ztrk_decode(bytes(self.header[s:e]))) for td, (s, e) in zip(self.track_defs, self.ztrk_ranges())]

    @property
    def is_stream_bank(self):
        """music / speech: no sample defs, the files are IMA-ADPCM streams."""
        return self.count("B") == 0

    def sample_info(self, i):
        """(codec, rate, users) for sample file i. codec 'pcm16' or 'ima'."""
        if self.is_stream_bank:
            return "ima", STREAM_RATE, []
        users = [j for j, sd in enumerate(self.sample_defs) if sd.sample == i]
        rates = {self.sample_defs[j].rate for j in users}
        return "pcm16", (rates.pop() if len(rates) == 1 else 22050), users

    def frames(self, i):
        codec, _r, _u = self.sample_info(i)
        return len(self.samples[i]) * 2 if codec == "ima" else len(self.samples[i]) // 2


@dataclass
class Voice:
    """20-byte voice (table A): what the mixer plays (ZsndVoice, decomp/include/zsound.h). [V]

    Envelope (zsndcKeyOn / zsndcMix): with ``attack`` the level rises 0 -> 1 over
    ENV_TIME_MS[attack]; with ``decay`` it would fall from 1 to sustain_level (the exe computes a
    rising rate there, but every voice with a decay has sustain_level 127, i.e. rate 0); then it
    falls from sustain_level to 0 over ENV_TIME_MS[sustain_time] (127 = hold until key-off); a
    key-off fades it out over ENV_TIME_MS[release] (0 = cut). Level, volume and pan select gains
    from a 128-entry table (0x0055E97C)."""
    sample: int         # +0x00 u16 sample (table B) index, 0xFFFF = none
    pitch: int          # +0x02 u16 4.12 rate factor (0x1000 = the sample's rate)
    volume: int         # +0x04 u8 0..127
    pan: int            # +0x05 s8 -127 left .. 0 .. 127 right
    attack: int         # +0x06 u8 envelope time index, 0 = none
    decay: int          # +0x07 u8 time index, 0 = none
    sustain_time: int   # +0x08 u8 time index, 127 = hold
    release: int        # +0x09 u8 time index, 0 = cut at key-off
    sustain_level: int  # +0x0A u8 0..127
    unused_0b: bytes    # +0x0B 6 bytes, 0 in every bank, not read
    psx_flag: int       # +0x11 u8 0/1 (29 voices); not read on the PC [I: a PSX SPU flag]
    handle: int         # +0x12 u16 runtime bank handle (0 in files)

    FMT = "<HHBbBBBBB6sBH"

    @classmethod
    def parse(cls, b):
        return cls(*struct.unpack(cls.FMT, b))

    def pack(self):
        try:
            return struct.pack(self.FMT, self.sample, self.pitch, self.volume, self.pan, self.attack, self.decay,
                               self.sustain_time, self.release, self.sustain_level, self.unused_0b,
                               self.psx_flag, self.handle)
        except struct.error as e:
            raise ValueError(f"voice field out of range: {e}") from e


# envelope stage times in ms by index (0x0055EB7C, 128 x u16)
ENV_TIME_MS = (1, 2, 3, 4, 5, 6, 8, 9, 11, 14, 18, 21, 26, 30, 35, 41, 47, 54, 61, 68, 77, 85, 94, 104, 115, 126,
               137, 149, 162, 175, 190, 204, 220, 236, 253, 270, 288, 307, 327, 347, 369, 390, 413, 437, 461, 487,
               513, 539, 567, 596, 626, 656, 687, 720, 753, 787, 822, 858, 895, 933, 973, 1013, 1054, 1096, 1139,
               1190, 1253, 1329, 1419, 1522, 1638, 1766, 1909, 2064, 2232, 2414, 2609, 2817, 3038, 3273, 3521, 3782,
               4056, 4343, 4644, 4959, 5286, 5627, 5981, 6349, 6730, 7124, 7531, 7953, 8387, 8835, 9296, 9771,
               10259, 10761, 11277, 11805, 12348, 12903, 13473, 14056, 14652, 15262, 15886, 16523, 17174, 17838,
               18516, 19208, 19913, 20632, 21365, 22111, 22871, 23645, 24433, 25234, 26049, 26878, 27720, 28577,
               30000, 0)


@dataclass
class TrackDef:
    """16-byte track def (table E, ZsndTrackDef). [V]"""
    volume: int        # +0x00 u8 (0x7F everywhere)
    pan: int           # +0x01 s8
    voice: int         # +0x02 u8 default voice, 0xFF = none
    keymap: int        # +0x03 u8 default keymap (table F), 0xFF = none
    pad_04: int        # +0x04 u32 0
    ztrk: int          # +0x08 u32 offset of the "ZTRK" record (events start at +4)
    pad_0c: int        # +0x0C u16 0
    handle: int        # +0x0E u16 runtime bank handle (0 in files)

    FMT = "<BbBBIIHH"

    @classmethod
    def parse(cls, b):
        return cls(*struct.unpack(cls.FMT, b))


# ---------------------------------------------------------------- ZTRK sequencer byte code
# zsndSeqDecodeEvent 0x0047D490 (decomp/src/zaxis/Bmx/Src/com/zge/zsndseq.c). An event is one
# byte (opcode & 0xF8) | form, then the delta time / length fields of the form, then the
# parameters. Times are milliseconds at tempo 1 (the sequencer steps 1000/30 ms per game frame).
ZTRK_TAG = b"ZTRK"
OP_NOTE, OP_NOTE_RUN, OP_CONTROL, OP_MARKER, OP_JUMP, OP_INDIRECT = 0x00, 0x08, 0x10, 0x18, 0x30, 0xF8
OP_NAMES = {OP_NOTE: "note", OP_NOTE_RUN: "note-run", OP_CONTROL: "control", OP_MARKER: "marker",
            OP_JUMP: "jump", 0x20: "op20", 0x28: "op28", 0x38: "op38", 0x40: "op40", OP_INDIRECT: "indirect"}
# parameter bytes per opcode (0x20/0x28/0x38/0x40 have no handler and no parameters; any other
# opcode makes the exe stop decoding, and ztrk_decode reject the track)
OP_PARAMS = {OP_NOTE: 2, OP_NOTE_RUN: 1, OP_CONTROL: 2, OP_MARKER: 2, OP_JUMP: 4, 0x20: 0, 0x28: 0,
             0x38: 0, 0x40: 0}
# form -> (delta bytes, length bytes); delta = wait before this event, length = note duration
FORMS = ((0, 0), (0, 1), (0, 4), (1, 0), (1, 1), (1, 4), (4, 0), (4, 4))
CONTROL_NAMES = {0: "expression", 1: "pan", 4: "voice", 5: "keymap"}
MAX_ZTRK_EVENTS = 1 << 16


@dataclass
class ZtrkEvent:
    op: int                 # opcode (bits 3-7)
    form: int               # bits 0-2: which delta / length fields are present
    delta: int = 0          # ms after the previous event (forms 3-7)
    length: int = 0         # note length in ms (forms 1, 2, 4, 5, 7)
    params: bytes = b""     # raw parameter bytes
    offset: int = 0         # position in the record (after the tag); jump targets use it

    def describe(self):
        p = self.params
        name = OP_NAMES.get(self.op, f"op{self.op:02x}")
        if self.op == OP_NOTE:
            arg = f"note {p[0]} vel {p[1]}"
        elif self.op == OP_NOTE_RUN:
            arg = f"note {p[0]}"
        elif self.op == OP_CONTROL:
            val = struct.unpack("b", p[1:2])[0] if p[0] == 1 else p[1]
            arg = f"{CONTROL_NAMES.get(p[0], p[0])} = {val}"
        elif self.op == OP_MARKER:
            arg = f"marker {p[0]} value {p[1]}" + ("  (end of track)" if p[0] == 1 else "")
        elif self.op == OP_JUMP:
            arg = f"-> +{struct.unpack('<I', p)[0]}"
        else:
            arg = p.hex(" ")
        tail = f"  len {self.length} ms" if FORMS[self.form][1] else ""
        return f"+{self.delta} ms  {name} {arg}{tail}"


def _indirect_size(src):
    """Parameter bytes after the opcode / source bytes of the 0xF8 form, as the exe reads them
    (it masks each 2-bit source with 2, so only source 2 consumes bytes: 3 each)."""
    return sum(3 for i in range(4) if (src >> (2 * i)) & 2)


def ztrk_decode(record):
    """A whole ZTRK record ("ZTRK" + events) -> [ZtrkEvent]. Raises FormatError on a bad tag, an
    opcode the exe would stop at, a truncated event or a jump that misses an event start."""
    record = bytes(record)
    if record[:4] != ZTRK_TAG:
        raise BadMagicError(f"expected 'ZTRK', got {record[:4]!r}", fmt=FMT, offset=0)
    data = record[4:]
    events, pos = [], 0
    while pos < len(data):
        if len(events) >= MAX_ZTRK_EVENTS:
            raise LimitError("too many ZTRK events", fmt=FMT, offset=4 + pos)
        b = data[pos]
        op, form = b & 0xF8, b & 7
        nd, nl = FORMS[form]
        p = need(data, pos + 1, nd + nl, fmt=FMT, what="ZTRK event header")
        delta = int.from_bytes(data[pos + 1:pos + 1 + nd], "little")
        length = int.from_bytes(data[pos + 1 + nd:p], "little")
        if op == OP_INDIRECT:
            need(data, p, 2, fmt=FMT, what="ZTRK indirect event")
            size = 2 + _indirect_size(data[p + 1])
        elif op in OP_PARAMS:
            size = OP_PARAMS[op]
        else:
            raise FormatError(f"unknown ZTRK opcode 0x{op:02x}", fmt=FMT, offset=4 + pos)
        end = need(data, p, size, fmt=FMT, what=f"ZTRK {OP_NAMES[op]} parameters")
        events.append(ZtrkEvent(op, form, delta, length, data[p:end], pos))
        pos = end
    starts = {e.offset for e in events}
    for e in events:
        if e.op == OP_JUMP and struct.unpack("<I", e.params)[0] not in starts:
            raise FormatError(f"ZTRK jump to +{struct.unpack('<I', e.params)[0]} misses every event",
                              fmt=FMT, offset=4 + e.offset)
    return events


def ztrk_encode(events):
    """[ZtrkEvent] -> a ZTRK record (tag included). Inverse of ztrk_decode."""
    out = bytearray(ZTRK_TAG)
    for e in events:
        if not 0 <= e.form < 8 or e.op & 7 or not 0 <= e.op <= 0xF8:
            raise ValueError(f"bad ZTRK event {e}")
        nd, nl = FORMS[e.form]
        if (e.delta and not nd) or (e.length and not nl):
            raise ValueError(f"form {e.form} cannot hold delta {e.delta} / length {e.length}")
        out.append(e.op | e.form)
        try:
            out += e.delta.to_bytes(nd, "little") + e.length.to_bytes(nl, "little")
        except OverflowError as ex:
            raise ValueError(f"ZTRK field too large for form {e.form}: {ex}") from ex
        out += e.params
    return bytes(out)


# ---------------------------------------------------------------- what the game plays
# bmxsoundEvent 0x0044E300 (bmxsound.c): (bank, voice) -> the events that play it. Event codes
# and the calling module are [V] (call sites); the plain-language meaning is [I].
# Full table: docs/re/sound.md section 6.
def _sfxgame_events():
    ev = {}

    def add(v, s):
        ev.setdefault(v, []).append(s)
    for v in range(0, 12):
        add(v, "4000 rolling loop, surface kind 1 (physics.c; 11 when flag bits 24-27 are set)")
    for v in range(12, 21):
        add(v, f"4000 rolling loop, surface kind 2 / {v - 12} (physics.c)")
    add(9, "4000 second rolling source (9 + n; physics.c passes n = 0)")
    for n in range(10):
        add(0x15 + n, f"3001 take-off / air, kind {n} (physics.c)")
    for n in range(9):
        add(0x1F + n, f"3003 kind {n} (physics.c, rolling)")
    for n in range(9):
        add(0x28 + n, f"3002 landing / grind, surface 0-1, kind {n} (physics.c)")
    for n in range(9):
        add(0x31 + n, f"3002 landing / grind, surface 2, kind {n} (physics.c)")
    add(0x3A, "3002 landing / grind, surface 3")
    add(0x3B, "3002 landing / grind, surface 5")
    add(0x3C, "3002 landing / grind, surface 4")
    for m in range(9):
        for g in range(3):
            add(0x3D + 3 * m + g, f"3004 impact, material {m}, strength {g} (physics.c / prop.c / rider.c)")
    for n in range(4):
        add(0x58 + n, f"3005 body hit {n} (rider.c, prop.c)")
    add(0x5C, "5000 timer beep, 1-10 s left (game.c)")
    add(0x5D, "5000 timer, 0 s left (game.c)")
    add(0x5E, "5001 trick in a combo, pitch rises with the count (game.c)")
    add(0x5F, "5004 a = 1 (game.c, landed tricks)")
    add(0x60, "5004 a = 0 (game.c, landed tricks)")
    add(0x61, "5004 a >= 2 / 5005 a = 0 (game.c)")
    for n in range(1, 3):
        add(0x61 + n, f"5005 a = {n} (game.c, objectives)")
    for n in range(4):
        add(100 + n, f"5006 a = {n} (game.c, objectives)")
    for code, v in ((5007, 104), (5008, 105), (5009, 106), (5010, 107)):
        add(v, f"{code} (game.c / overlay.c)")
    add(0x6C, "5011 with voice 109 (overlay.c)")
    add(0x6D, "5011 with voice 108 (overlay.c)")
    add(0x6E, "1000 mode 3: the game pauses")
    add(0x6F, "6000 (action.c)")
    return ev


def voice_events(stem, n_voices=None):
    """{voice index: [descriptions]} for a bank file stem ('sfxgame', 'sfxlev03', ...)."""
    stem = stem.lower()
    if stem == "sfxcom":
        ev = {0: ["2000 menu sound (frontend.c / overlay.c; the most frequent one)"],
              1: ["2001 menu sound (overlay.c / overlymp.c / frontend.c)"],
              2: ["2002 menu sound (frontend.c / overlay.c)"],
              3: ["2003 in the game (overlay.c / overlymp.c)"],
              4: ["2004 menu sound (frontend.c / overlay.c)"],
              5: ["2003 in the menu", "2005 (no caller)"]}
    elif stem.startswith("sfxmen"):
        ev = {0: ["menu music, first of two loops played together (music volume)"],
              1: ["menu music, second loop"]}
    elif stem == "sfxgame":
        ev = _sfxgame_events()
    elif stem == "sfxgame2":
        ev = {}
        for v in range(14):
            ev.setdefault(v, []).append("3007 loop (no caller in this build)")
        for n in range(4):
            ev.setdefault(4 + n, []).append(f"3008 kind {n} (no caller)")
            ev.setdefault(10 + n, []).append(f"3009 kind {n} (no caller)")
    elif stem.startswith("sfxchr"):
        ev = {v: ["3006 rider voice, one of four at random (rider.c, prop.c)"] for v in range(4)}
    elif stem.startswith("sfxlev"):
        ev = {}
        for c in range(4):
            ev.setdefault(4 + c, []).append(f"4002 prop sound kind {c} (prop.c: loops and one-shots)")
            ev.setdefault(12 + c, []).append(f"4003 prop sound kind {c} (prop.c)")
        ev.setdefault(0x10, []).append("5003 crowd, a < 4, b = 1 (game.c)")
        ev.setdefault(0x11, []).append("5003 crowd, a >= 4, b = 1 (game.c)")
        ev.setdefault(0x12, []).append("5003 crowd, a < 4, b = 0 (game.c)")
        ev.setdefault(0x13, []).append("5003 crowd, a >= 4, b = 0 (game.c)")
    else:
        ev = {}
    if n_voices is not None:
        ev = {v: d for v, d in ev.items() if v < n_voices}
    return ev


def ztrk_voices(bank):
    """{voice index: [track numbers]} of the voices a bank's ZTRK tracks play."""
    out = {}
    for t, (td, events) in enumerate(bank.ztrk_tracks()):
        used = [e.params[1] for e in events if e.op == OP_CONTROL and e.params[0] == 4 and e.params[1] != 0xFF]
        if td.voice != 0xFF:
            used.append(td.voice)
        for v in used:
            lst = out.setdefault(v, [])
            if t not in lst:
                lst.append(t)
    return out


def _align4(n):
    return (n + 3) & ~3


def read(data, *, lenient=False):
    """Parse a sound bank. Unless ``lenient``, the total-size field must equal the file length."""
    data = bytes(data)
    need(data, 0, HEADER_LEN, fmt=FMT, what="ZSND header")
    if data[:4] != MAGIC:
        raise BadMagicError(f"expected 'ZSND', got {data[:4]!r}", fmt=FMT, offset=0)
    if data[4:8] != PLATFORM:
        raise BadMagicError(f"expected platform 'WIN ', got {data[4:8]!r}", fmt=FMT, offset=4)
    total, data_end = struct.unpack_from("<II", data, 8)
    if total != len(data) and not lenient:
        raise ChecksumError(f"total size field {total} != file length {len(data)}", fmt=FMT, offset=8)
    if not HEADER_LEN <= data_end <= len(data):
        raise TruncatedError(f"header end 0x{data_end:x} outside the file", fmt=FMT, offset=0x0C)
    for t, (co, po, esz) in TABLES.items():
        n, off = struct.unpack_from("<II", data, co)
        if n > MAX_ENTRIES:
            raise LimitError(f"table {t}: {n} entries", fmt=FMT, offset=co)
        if off < HEADER_LEN or off + n * esz > data_end:
            raise TruncatedError(f"table {t} (0x{off:x} + {n}x{esz}) outside the header area", fmt=FMT, offset=po)
    bank = Bank(bytearray(data[:data_end]))
    n_c, off_c = bank.count("C"), bank.offset("C")
    pos = _align4(data_end)
    if pos > len(data) and n_c:
        raise TruncatedError("sample data starts past the end", fmt=FMT, offset=0x0C)
    bank.pad = data[data_end:min(pos, len(data))]
    for i in range(n_c):
        o, n = struct.unpack_from("<II", data, off_c + i * C_LEN)
        if o < data_end or o + n > len(data):
            raise TruncatedError(f"sample {i} (0x{o:x}+{n}) outside the data area", fmt=FMT, offset=off_c + i * C_LEN)
        bank.samples.append(data[o:o + n])
    for sd in bank.sample_defs:
        if sd.sample != 0xFFFF and sd.sample >= n_c:
            raise FormatError(f"sample def refers to sample {sd.sample} of {n_c}", fmt=FMT)
    return bank


def write(bank):
    """Serialise with the canonical layout (header, zero pad to 4, samples back to back)."""
    h = bytearray(bank.header)
    data_end = len(h)
    if struct.unpack_from("<I", h, 0x0C)[0] != data_end:
        raise ValueError("header length does not match its end field")
    pos = _align4(data_end)
    pad = bank.pad if len(bank.pad) == pos - data_end else b"\0" * (pos - data_end)
    off_c = struct.unpack_from("<I", h, 0x24)[0]
    if struct.unpack_from("<I", h, 0x20)[0] != len(bank.samples):
        raise ValueError("sample count does not match table C")
    for i, s in enumerate(bank.samples):
        struct.pack_into("<II", h, off_c + i * C_LEN, pos, len(s))
        pos += len(s)
    struct.pack_into("<I", h, 8, pos)
    return bytes(h) + pad + b"".join(bytes(s) for s in bank.samples)


# ---------------------------------------------------------------- IMA-ADPCM (exe variant)
IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
            73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
            449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
            2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
            9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)


def ima_decode(data):
    """IMA-ADPCM nibbles (low first) -> bytes of signed 16-bit LE samples, as 0x004724C0 does."""
    out = bytearray(len(data) * 4)
    pred, idx = 0, 0
    step = IMA_STEP[0]
    o = 0
    for b in data:
        for nib in (b & 0xF, b >> 4):
            diff = step >> 3
            if nib & 4:
                diff += step
            if nib & 2:
                diff += step >> 1
            if nib & 1:
                diff += step >> 2
            pred = pred - diff if nib & 8 else pred + diff
            pred = 32767 if pred > 32767 else (-32768 if pred < -32768 else pred)
            struct.pack_into("<h", out, o, pred)
            o += 2
            idx += IMA_INDEX[nib]
            idx = 0 if idx < 0 else (88 if idx > 88 else idx)
            step = IMA_STEP[idx]
    return bytes(out)


def ima_encode(pcm):
    """Signed 16-bit LE mono samples -> IMA-ADPCM bytes that ``ima_decode`` reproduces (odd count padded)."""
    n = len(pcm) // 2
    samples = struct.unpack(f"<{n}h", pcm[:n * 2])
    pred, idx = 0, 0
    out = bytearray((n + 1) // 2)
    for k, s in enumerate(samples):
        step = IMA_STEP[idx]
        d = s - pred
        nib = 0
        if d < 0:
            nib, d = 8, -d
        if d >= step:
            nib |= 4
            d -= step
        if d >= step >> 1:
            nib |= 2
            d -= step >> 1
        if d >= step >> 2:
            nib |= 1
        # reconstruct exactly like the decoder
        diff = step >> 3
        if nib & 4:
            diff += step
        if nib & 2:
            diff += step >> 1
        if nib & 1:
            diff += step >> 2
        pred = pred - diff if nib & 8 else pred + diff
        pred = 32767 if pred > 32767 else (-32768 if pred < -32768 else pred)
        idx += IMA_INDEX[nib]
        idx = 0 if idx < 0 else (88 if idx > 88 else idx)
        out[k >> 1] |= nib << (4 * (k & 1))
    return bytes(out)


# ---------------------------------------------------------------- PCM helpers
def to_mono16(raw, channels, width):
    """Interleaved PCM (8-bit unsigned or 16-bit signed) -> mono signed 16-bit LE bytes."""
    if width == 1:
        vals = [(b - 128) << 8 for b in raw]
    elif width == 2:
        vals = list(struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2]))
    else:
        raise ValueError("only 8- or 16-bit PCM WAV input is supported")
    if channels > 1:
        vals = [sum(vals[i:i + channels]) // channels for i in range(0, len(vals) - channels + 1, channels)]
    return struct.pack(f"<{len(vals)}h", *vals)


def resample(pcm, src_rate, dst_rate):
    """Linear-interpolation resample of mono signed 16-bit LE bytes."""
    if src_rate == dst_rate or not pcm:
        return pcm
    s = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    n_out = max(1, round(len(s) * dst_rate / src_rate))
    out = []
    for k in range(n_out):
        x = k * src_rate / dst_rate
        i = int(x)
        f = x - i
        a = s[min(i, len(s) - 1)]
        b = s[min(i + 1, len(s) - 1)]
        out.append(int(round(a + (b - a) * f)))
    return struct.pack(f"<{n_out}h", *out)


def replace_sample(bank, i, pcm16_mono, rate):
    """Replace sample file ``i`` with mono 16-bit PCM at ``rate`` Hz, encoded in the bank's native format.

    The audio is resampled to the native rate; loop points of the sample defs that use it are kept
    inside the new length (a loop that ended on the last frame keeps ending on the last frame).
    Returns a list of change descriptions.
    """
    if not 0 <= i < len(bank.samples):
        raise ValueError(f"sample {i} out of range 0..{len(bank.samples) - 1}")
    codec, native, users = bank.sample_info(i)
    old_frames = bank.frames(i)
    pcm = resample(pcm16_mono, rate, native)
    data = ima_encode(pcm) if codec == "ima" else pcm
    bank.samples[i] = data
    new_frames = bank.frames(i)
    notes = [f"sample {i}: {codec} {native} Hz, {old_frames} -> {new_frames} frames ({len(data)} bytes)"]
    for j in users:
        sd = bank.sample_defs[j]
        if sd.flags & 1 or sd.loop_end:
            old = (sd.loop_start, sd.loop_end)
            sd.loop_end = new_frames - 1 if sd.loop_end >= old_frames - 1 else min(sd.loop_end, new_frames - 1)
            sd.loop_start = min(sd.loop_start, sd.loop_end)
            bank.set_sample_def(j, sd)
            if old != (sd.loop_start, sd.loop_end):
                notes.append(f"sample def {j}: loop {old} -> {(sd.loop_start, sd.loop_end)}")
    return notes

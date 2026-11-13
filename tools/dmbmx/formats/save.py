"""SAVE.DAT (``.\\save\\SAVE.DAT``): the PC build of the PSX "memory card" save.

Spec: docs/formats/SAVE.md. Code: ``memcard.c`` in NgBMX.exe,
load = 0x00412190, save = 0x00412340.

The file is a raw dump of three global blocks behind an 8-byte header. There is
**no CRC or checksum** (verified statically, see docs/re/integrity-checks.md).
The game's only acceptance tests on load are:

* the read of 0x235C bytes succeeds (a shorter file is rejected, extra bytes are ignored);
* ``u32 @4 == 0x235C`` (else "memory card data is invalid (probably old version)").

The magic ``1C 51 81 2A`` is written on save but not checked on load.

Layout (file offset -> exe global):

    0x0000  u32   magic 0x2A81511C
    0x0004  u32   size  0x235C            (checked)
    0x0008  0xC3C block A -> 0x006955E0   options (8 bytes), 12 rider records x 0x104, 4 bytes
    0x0C44  0x1680 block B -> 0x00551FD8  records/high scores: 12 levels x 24 entries x 20 bytes
    0x22C4  0x98  block C -> 0x006952A0   2 rider profiles x 0x4C
    0x235C  end

Every byte is kept: ``write(read(b)) == b`` for any input that ``read`` accepts.
"""
import struct
from dataclasses import dataclass, field

from dmbmx.core.errors import BadMagicError, ChecksumError, need

FMT = "SAVE"
MAGIC = 0x2A81511C
SIZE = 0x235C

OPTIONS_OFF, OPTIONS_LEN = 0x08, 0x08
RIDERS_OFF, RIDER_LEN, RIDER_COUNT = 0x10, 0x104, 12   # per-rider career records (formerly called levels)
LEVEL_COUNT = 12
LEVELS_OFF, LEVEL_LEN = RIDERS_OFF, RIDER_LEN          # deprecated aliases
BLOCK_A_TAIL_OFF, BLOCK_A_TAIL_LEN = 0xC40, 4
SCORES_OFF, SCORE_LEN, SCORES_PER_LEVEL = 0xC44, 20, 24
PROFILES_OFF, PROFILE_LEN, PROFILE_COUNT = 0x22C4, 0x4C, 2

SCORE_NAME_LEN = 13          # bytes 7..19 of a score entry
PROFILE_NAME_OFF, PROFILE_NAME_LEN = 7, 0x24 - 7
PROFILE_CONTROLS_OFF, PROFILE_CONTROLS_COUNT = 0x24, 7

# exe globals the three blocks are copied to (for reference / docs)
BLOCKS = (
    ("A", 0x0008, 0x0C3C, 0x006955E0),
    ("B", 0x0C44, 0x1680, 0x00551FD8),
    ("C", 0x22C4, 0x0098, 0x006952A0),
)


def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("latin-1")


@dataclass
class ScoreEntry:
    """20 bytes: u32 score, 3 unknown bytes (byte 0 looks like a rider index [I]), char name[13]."""
    score: int
    b4: int
    b5: int
    b6: int
    name_raw: bytes

    @classmethod
    def parse(cls, raw):
        score, b4, b5, b6 = struct.unpack_from("<IBBB", raw)
        return cls(score, b4, b5, b6, bytes(raw[7:20]))

    def pack(self):
        if len(self.name_raw) != SCORE_NAME_LEN:
            raise ValueError("score name_raw must be 13 bytes")
        try:
            return struct.pack("<IBBB", self.score, self.b4, self.b5, self.b6) + self.name_raw
        except struct.error as e:
            raise ValueError(f"score entry field out of range: {e}") from e

    @property
    def name(self):
        return _cstr(self.name_raw)

    @name.setter
    def name(self, text):
        b = text.encode("latin-1")
        if len(b) >= SCORE_NAME_LEN:
            raise ValueError(f"name {text!r} too long: max {SCORE_NAME_LEN - 1} bytes")
        self.name_raw = b + b"\0" * (SCORE_NAME_LEN - len(b))


@dataclass
class Profile:
    """0x4C-byte rider profile (global 0x006952A0 + i*0x4C). Raw bytes are authoritative."""
    raw: bytearray

    @property
    def name(self):
        return _cstr(self.raw[PROFILE_NAME_OFF:PROFILE_NAME_OFF + PROFILE_NAME_LEN])

    @name.setter
    def name(self, text):
        b = text.encode("latin-1")
        if len(b) >= PROFILE_NAME_LEN:
            raise ValueError(f"name {text!r} too long: max {PROFILE_NAME_LEN - 1} bytes")
        self.raw[PROFILE_NAME_OFF:PROFILE_NAME_OFF + PROFILE_NAME_LEN] = b + b"\0" * (PROFILE_NAME_LEN - len(b))

    @property
    def controls(self):
        """7 u32 at +0x24, defaults 0x80,0x20,0x40,0x10,0x100,0x200,0x4000 (init 0x00407C72) [I: button masks]."""
        return list(struct.unpack_from(f"<{PROFILE_CONTROLS_COUNT}I", self.raw, PROFILE_CONTROLS_OFF))


@dataclass
class Save:
    magic: int = MAGIC
    size_field: int = SIZE
    options: bytearray = field(default_factory=lambda: bytearray(OPTIONS_LEN))
    riders: list = field(default_factory=list)       # 12 x bytearray(0x104), one career record per rider
    block_a_tail: bytearray = field(default_factory=lambda: bytearray(BLOCK_A_TAIL_LEN))
    scores: list = field(default_factory=list)       # 12 x 24 x ScoreEntry
    profiles: list = field(default_factory=list)     # 2 x Profile
    extra: bytes = b""                               # bytes past 0x235C (ignored by the game)

    # --- options block (file 0x08..0x0F, global 0x006955E0..E7) ---
    def _opt(self, fmt, off):
        return struct.unpack_from(fmt, self.options, off)[0]

    @property
    def word_08(self):
        return self._opt("<H", 0)

    @property
    def word_0a(self):
        return self._opt("<H", 2)

    @property
    def volume_0c(self):
        """u8 @0x0C (0x006955E4), default 18; sent x7 to sound cmd 0x3EA channel 0 [I: a volume]."""
        return self.options[4]

    @property
    def volume_0d(self):
        """u8 @0x0D (0x006955E5), default 18; sent x7 to sound cmd 0x3EA channel 1 [I: a volume]."""
        return self.options[5]

    @property
    def current_profile(self):
        """u8 @0xC40 (0x00696218), default 0. Passed to the frontend loader 0x0044D990 [I: language]."""
        return self.block_a_tail[0]

    @property
    def levels(self):
        """Deprecated alias of ``riders`` (the per-rider records were first mislabelled as levels)."""
        return self.riders

    def is_valid(self):
        """Exactly the game's own acceptance test (memcard load 0x00412190)."""
        return self.size_field == SIZE

    def resign(self):
        """Make the game accept this save: correct size field + magic, drop ignored trailing bytes.

        There is no CRC to recompute (docs/re/integrity-checks.md).
        """
        self.magic = MAGIC
        self.size_field = SIZE
        self.extra = b""
        return self


def read(data, *, lenient=False):
    """Parse SAVE.DAT bytes.

    Raises TruncatedError if shorter than 0x235C (the game rejects that too).
    Unless ``lenient``: BadMagicError on a wrong magic (the game itself does not
    check it) and ChecksumError on a wrong size field (the game rejects it).
    """
    data = bytes(data)
    need(data, 0, SIZE, fmt=FMT, what="SAVE.DAT body")
    magic, size_field = struct.unpack_from("<II", data, 0)
    if not lenient:
        if magic != MAGIC:
            raise BadMagicError(f"magic 0x{magic:08X}, expected 0x{MAGIC:08X}", fmt=FMT, offset=0)
        if size_field != SIZE:
            raise ChecksumError(
                f"size field 0x{size_field:X}, expected 0x{SIZE:X} (game: 'memory card data is invalid')",
                fmt=FMT, offset=4)
    s = Save(magic=magic, size_field=size_field)
    s.options = bytearray(data[OPTIONS_OFF:OPTIONS_OFF + OPTIONS_LEN])
    s.riders = [bytearray(data[RIDERS_OFF + i * RIDER_LEN:RIDERS_OFF + (i + 1) * RIDER_LEN])
                for i in range(RIDER_COUNT)]
    s.block_a_tail = bytearray(data[BLOCK_A_TAIL_OFF:BLOCK_A_TAIL_OFF + BLOCK_A_TAIL_LEN])
    s.scores = []
    for lv in range(LEVEL_COUNT):
        row = []
        for e in range(SCORES_PER_LEVEL):
            o = SCORES_OFF + (lv * SCORES_PER_LEVEL + e) * SCORE_LEN
            row.append(ScoreEntry.parse(data[o:o + SCORE_LEN]))
        s.scores.append(row)
    s.profiles = [Profile(bytearray(data[PROFILES_OFF + i * PROFILE_LEN:PROFILES_OFF + (i + 1) * PROFILE_LEN]))
                  for i in range(PROFILE_COUNT)]
    s.extra = data[SIZE:]
    return s


def write(save):
    """Serialise ``save`` exactly as stored (call ``save.resign()`` first to fix the header)."""
    if not (0 <= save.magic <= 0xFFFFFFFF and 0 <= save.size_field <= 0xFFFFFFFF):
        raise ValueError("magic/size_field must be u32")
    out = bytearray(struct.pack("<II", save.magic, save.size_field))
    parts = [(save.options, OPTIONS_LEN, "options")]
    if len(save.riders) != RIDER_COUNT:
        raise ValueError(f"need {RIDER_COUNT} rider records")
    parts += [(r, RIDER_LEN, "rider record") for r in save.riders]
    parts.append((save.block_a_tail, BLOCK_A_TAIL_LEN, "block A tail"))
    for raw, n, what in parts:
        if len(raw) != n:
            raise ValueError(f"{what} must be {n} bytes, got {len(raw)}")
        out += raw
    if len(save.scores) != LEVEL_COUNT or any(len(r) != SCORES_PER_LEVEL for r in save.scores):
        raise ValueError(f"need {LEVEL_COUNT}x{SCORES_PER_LEVEL} score entries")
    for row in save.scores:
        for e in row:
            out += e.pack()
    if len(save.profiles) != PROFILE_COUNT:
        raise ValueError(f"need {PROFILE_COUNT} profiles")
    for p in save.profiles:
        if len(p.raw) != PROFILE_LEN:
            raise ValueError(f"profile must be {PROFILE_LEN} bytes")
        out += p.raw
    return bytes(out) + bytes(save.extra)

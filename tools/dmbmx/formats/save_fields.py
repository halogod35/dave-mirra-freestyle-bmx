"""Named, range-checked fields of SAVE.DAT (DM-024 save editor).

Every field has a dotted name pattern (``rider.<r>.bikes_unlocked``), a location,
a type, an allowed range, an evidence tag ([V]/[I]) and the exe VA that shows it.
Spec: docs/formats/SAVE.md section 6.

    fields.get(save, "rider.0.bikes_unlocked")          -> 5
    fields.set(save, "rider.0.bikes_unlocked", "5")     # raises FieldError if out of range
    fields.unlock_all(save)                             -> list of (name, old, new)
"""
import re
import struct
from dataclasses import dataclass

from dmbmx.formats import save as S

RIDER_NAMES = ("Dave Mirra", "Ryan Nyquist", "Troy McMurray", "Mike Laird", "Chad Kagy", "Tim Mirra",
               "Kenan Harkin", "Shaun Butler", "Leigh Ramsdell", "Joey Garcia", "Slim Jim Guy", "Amish Boy")
LEVEL_NAMES = ("Greenville Park", "Vail Trails", "Backyard Pools", "Lot 8", "Woodward Trails", "Titanic",
               "Eastwood", "Sleepy's Ditch", "San Jose", "Acclaim Max Games Street",
               "Acclaim Max Games Dirt", "Acclaim Max Games Vert")
PARK_NAMES = ("Greenville", "Woodward", "Pro Tour", "Acclaim Max Games")
TIER_NAMES = ("amateur", "pro", "hardcore")
CHEAT_NAMES = ("first person cam", "night vision", "super crash", "exorcist", "suspension",
               "silly grunt", "ghost rider", "sticky crash")
CAMERA_NAMES = ("behind", "wide", "above", "first person")
# DM-051 (docs/re/input-camera.md): controller configuration (STRS 0x23D-0x23F), keyboard layouts
# (frontend 0x0055A43C, keymaps 0x0055BB98), and the logical button each control slot feeds
# (contrlerUpdate 0x004080F0: control[k] = the device bits that act as button CONTROL_BITS[k]).
CONTROLLER_CONFIG_NAMES = ("A", "B", "C")
KEYBOARD_LAYOUT_NAMES = ("layout 1", "layout 2", "layout 3", "layout 4", "layout 5")
CONTROL_BITS = (0x80, 0x20, 0x40, 0x10, 0x100, 0x200, 0x4000)
CONTROL_NAMES = ("cross (pedal/jump)", "square (trick)", "circle (modifier)", "triangle (grind)",
                 "L1 (spin left)", "R1 (spin right)", "start (pause)")
# PAD_* bit names as the frontend shows them (0x0055A3F4), bit 0..17
PAD_BUTTON_NAMES = ("Up", "Down", "Left", "Right", "Fire 1", "Fire 2", "Fire 3", "Fire 4", "L Trig 1", "R Trig 1",
                    "L Trig 2", "R Trig 2", "L Trig 3", "R Trig 3", "Start", "Select", "Fire 5", "Fire 6")
DPAD_NAMES = ("up", "down", "left", "right")
# The CONTROLLER screen (0x00441CB0, accept 0x0044256B) stores 11 bindings at profile +0x14..+0x3F: the
# 4 d-pad rows (never applied by the game) and the 7 control slots. The name therefore ends at +0x13:
# 12 characters + NUL (like the record names). [V] for the bindings, [I] for the name limit.
PROFILE_NAME_MAX = 12
PROFILE_DPAD_OFF = 0x14
MUSIC_NAMES = ("59 Times the Pain", "Cypress Hill", "Deftones", "Dropkick Murphys", "Pennywise",
               "Rancid", "Social Distortion", "Sublime", "Swingin Utters", "Primer 55")
PLACING_NAMES = ("none", "3rd", "2nd", "1st")
# riders whose "all levels complete" reward is their movie (reward table 0x00556968 type 8) [V]
MOVIE_RIDERS = (0, 1, 11)

GOAL_LEVELS = 6            # levels 0..5 have challenge goals (0x00407FC0 asserts level < 6) [V]
GOAL_INFO_OFF, GOAL_INFO_LEN = 7, 0x27
COMP_OFF = 0xEB            # + level (6..11): best placing [V offset 0x00410C60]
TRICKS_OFF, TRICKS_LEN = 0xF7, 11


class FieldError(ValueError):
    """Unknown field name or a value the field can't hold."""


@dataclass(frozen=True)
class FieldDef:
    pattern: str        # e.g. "rider.<r>.levels_unlocked"
    kind: str           # u8 u16 u32 bool nibble str
    lo: int
    hi: int
    block: str          # opt | tail | rider | profile | score
    off: object         # int, or callable(*idx) -> int (offset inside the block buffer)
    bit: object = None  # bool: bitmask int or callable(*idx); nibble: shift callable
    idx: tuple = ()     # allowed range per <placeholder>
    tag: str = "I"
    va: str = ""
    doc: str = ""
    choices: tuple = ()

    @property
    def regex(self):
        return re.compile("^" + re.sub(r"<\w+>", r"(\\d+)", re.escape(self.pattern)) + "$")


def _r(lo, hi):
    return range(lo, hi + 1)


def _goal(L, t, g):
    return GOAL_INFO_OFF + L * GOAL_INFO_LEN + 3 + t * 12 + g * 3


R12, P2, L12, E24 = _r(0, 11), _r(0, 1), _r(0, 11), _r(0, 23)
GL, T3, G4, J4 = _r(0, GOAL_LEVELS - 1), _r(0, 2), _r(0, 3), _r(0, 3)

FIELDS = (
    # ---- global options (file 0x08..0x0F = 0x006955E0..E7) ----
    FieldDef("cheats_unlocked", "u16", 0, 0xFF, "opt", 0, tag="V", va="0x00440DC0 (cheat menu), 0x004111CF",
             doc="bit c = cheat c unlocked (see cheat_unlocked.<c>); awarded per rider via table 0x00556968"),
    FieldDef("cheat_unlocked.<c>", "bool", 0, 1, "opt", 0, bit=lambda c: 1 << c, idx=(_r(0, 7),), tag="V",
             va="0x00440DC0, table 0x0055A3BC", doc="cheat unlocked", choices=CHEAT_NAMES),
    FieldDef("cheats_active", "u16", 0, 0xFFFF, "opt", 2, tag="V", va="0x00441180 (CHEATS menu), 0x00405780, 0x0041FC40",
             doc="bit c = cheat c switched on (OPTIONS > CHEATS toggles it, DM-056); tested in-game"),
    FieldDef("volume_0c", "u8", 0, 18, "opt", 4, tag="V", va="0x0043F4E5, 0x00442CE0 (OPTIONS > AUDIO)",
             doc="SOUND FX volume (x7 -> sound cmd 0x3EA ch 0); default 18"),
    FieldDef("volume_0d", "u8", 0, 18, "opt", 5, tag="V", va="0x0043F4C8, 0x00442CE0 (OPTIONS > AUDIO)",
             doc="MUSIC volume (x7 -> sound cmd 0x3EA ch 1); default 18"),
    FieldDef("music_track", "u8", 0, 9, "opt", 6, tag="I", va="0x00416810 (text 0x102E+4v)",
             doc="selected soundtrack; default 1", choices=MUSIC_NAMES),
    FieldDef("riders_unlocked", "u8", 1, 12, "opt", 7, tag="V", va="0x0041112D, cheats 0x0044716E",
             doc="number of selectable riders: 10 default, 11 = Slim Jim Guy, 12 = Amish Boy"),
    FieldDef("language", "u8", 0, 3, "tail", 0, tag="V", va="0x0044D990, 0x0044DAD0 (STRS 0x7919 + language)",
             doc="u8 @0xC40: 0 US English, 1 English, 2 French, 3 German (the text sets); default 0"),
    # ---- per-rider career record (file 0x10 + r*0x104 = 0x006955E8 + r*0x104) ----
    FieldDef("rider.<r>.flags", "u32", 0, 0xFFFFFFFF, "rider", 0, idx=(R12,), tag="V", va="0x00411090, 0x00410C60",
             doc="bit0 all-complete reward (movie), bit1 all levels hardcore, bits2-5 park complete, "
                 "0x40/0x80/0x100 'new bike/style/level' notices (cleared at 0x004099C0)"),
    FieldDef("rider.<r>.movie_unlocked", "bool", 0, 1, "rider", 0, bit=1, idx=(R12,), tag="V",
             va="0x0044A4B0 (movie menu: riders 0, 1, 11)", doc="rider's movie in the MOVIES menu"),
    FieldDef("rider.<r>.all_levels_complete", "bool", 0, 1, "rider", 0, bit=2, idx=(R12,), tag="V", va="0x00410F2A"),
    FieldDef("rider.<r>.park_complete.<k>", "bool", 0, 1, "rider", 0, bit=lambda r, k: 4 << k, idx=(R12, _r(0, 3)),
             tag="V", va="0x00410EF8", doc="all 3 levels of park k done", choices=PARK_NAMES),
    FieldDef("rider.<r>.levels_unlocked", "u8", 1, 12, "rider", 4, idx=(R12,), tag="V", va="0x00411172, cheat 0x00447261",
             doc="levels selectable on SELECT LEVEL (order: LEVEL_NAMES)"),
    FieldDef("rider.<r>.styles_unlocked", "u8", 1, 5, "rider", 5, idx=(R12,), tag="V", va="0x0041110F, 0x00445B90 (SELECT STYLE)",
             doc="costumes (STYLE 1..5) selectable"),
    FieldDef("rider.<r>.bikes_unlocked", "u8", 1, 5, "rider", 6, idx=(R12,), tag="V", va="0x004110F1, cheat 0x004471EC (SELECT BIKE)",
             doc="bikes selectable (4 + secret bike)"),
    FieldDef("rider.<r>.level.<L>.byte0", "u8", 0, 0xFF, "rider", lambda r, L: GOAL_INFO_OFF + L * GOAL_INFO_LEN,
             idx=(R12, GL), tag="I", doc="unknown (0 in the real save)"),
    FieldDef("rider.<r>.level.<L>.tier", "u8", 0, 2, "rider", lambda r, L: GOAL_INFO_OFF + L * GOAL_INFO_LEN + 1,
             idx=(R12, GL), tag="V", va="0x0040CD40", doc="current challenge tier", choices=TIER_NAMES),
    FieldDef("rider.<r>.level.<L>.tier_goals_done", "u8", 0, 4, "rider", lambda r, L: GOAL_INFO_OFF + L * GOAL_INFO_LEN + 2,
             idx=(R12, GL), tag="V", va="0x0040CD40, 0x0040CF70", doc="goals done in the current tier (4 = tier complete)"),
    FieldDef("rider.<r>.level.<L>.goal.<t>.<g>.flags", "u8", 0, 0xFF, "rider", lambda r, L, t, g: _goal(L, t, g),
             idx=(R12, GL, T3, G4), tag="V", va="0x0040CD40",
             doc="0x80 done, 0x40 done this session [I], low bits = objective count [I]"),
    FieldDef("rider.<r>.level.<L>.goal.<t>.<g>.done", "bool", 0, 1, "rider", lambda r, L, t, g: _goal(L, t, g), bit=0x80,
             idx=(R12, GL, T3, G4), tag="V", va="0x0040CD40, 0x0040C300"),
    FieldDef("rider.<r>.level.<L>.goal.<t>.<g>.objective.<j>", "nibble", 0, 15, "rider",
             lambda r, L, t, g, j: _goal(L, t, g) + 1 + (j >> 1), bit=lambda r, L, t, g, j: (j & 1) * 4,
             idx=(R12, GL, T3, G4, J4), tag="V", va="0x004102C0 / 0x00410350",
             doc="objective state: low 3 bits (5 = done, rewards given), bit 3 flag"),
    FieldDef("rider.<r>.comp.<L>", "u8", 0, 3, "rider", lambda r, L: COMP_OFF + L, idx=(R12, _r(6, 11)), tag="V",
             va="0x00410C60", doc="competition levels 6..11: best placing [I: 0 none .. 3 1st]", choices=PLACING_NAMES),
    FieldDef("rider.<r>.trick.<n>", "bool", 0, 1, "rider", lambda r, n: TRICKS_OFF + (n >> 3), bit=lambda r, n: 1 << (n & 7),
             idx=(R12, _r(0, TRICKS_LEN * 8 - 1)), tag="V", va="0x0040DAE0", doc="trick landed at least once (trick id n)"),
    FieldDef("rider.<r>.byte_102", "u8", 0, 0xFF, "rider", 0x102, idx=(R12,), tag="I", doc="unknown"),
    FieldDef("rider.<r>.byte_103", "u8", 0, 0xFF, "rider", 0x103, idx=(R12,), tag="I", doc="unknown"),
    # ---- player profiles (file 0x22C4 + p*0x4C = 0x006952A0 + p*0x4C) ----
    FieldDef("profile.<p>.name", "str", 0, PROFILE_NAME_MAX, "profile", S.PROFILE_NAME_OFF, idx=(P2,), tag="V",
             va="0x00407C69", doc="player name (max 28 chars, latin-1)"),
    FieldDef("profile.<p>.flags", "u8", 0, 0xFF, "profile", 0, idx=(P2,), tag="I", va="0x00416060", doc="default 3"),
    FieldDef("profile.<p>.tutorial_hints", "bool", 0, 1, "profile", 0, bit=2, idx=(P2,), tag="I", va="0x00416060, 0x004111AC",
             doc="on/off option shown in the pause OPTIONS; cleared by the first level reward"),
    FieldDef("profile.<p>.index", "u8", 0, 1, "profile", 1, idx=(P2,), tag="V", va="0x00407C5C, 0x00408590",
             doc="pad (controller) this player reads (contrlerTestButtons / riderReadInput)"),
    FieldDef("profile.<p>.controller_config", "u8", 0, 2, "profile", 2, idx=(P2,), tag="V",
             va="0x004084B0 (asserts < 3), 0x004316F2 (copied to the rider)",
             doc="controller configuration A/B/C: the action -> button table 0x00553EE0", choices=CONTROLLER_CONFIG_NAMES),
    FieldDef("profile.<p>.rider", "u8", 0, 11, "profile", 3, idx=(P2,), tag="V", va="0x00411090, 0x00403D10",
             doc="selected rider; also selects the rider record", choices=RIDER_NAMES),
    FieldDef("profile.<p>.style", "u8", 0, 4, "profile", 4, idx=(P2,), tag="V", va="0x004316C0", doc="selected costume (STYLE n+1)"),
    FieldDef("profile.<p>.bike", "u8", 0, 4, "profile", 5, idx=(P2,), tag="V", va="0x004316C0", doc="selected bike (4 = secret bike)"),
    FieldDef("profile.<p>.camera", "u8", 0, 3, "profile", 6, idx=(P2,), tag="V", va="0x00416060 (text 0x1087+v)",
             choices=CAMERA_NAMES),
    FieldDef("profile.<p>.control.<k>", "u32", 0, 0xFFFFFFFF, "profile", lambda p, k: S.PROFILE_CONTROLS_OFF + 4 * k,
             idx=(P2, _r(0, S.PROFILE_CONTROLS_COUNT - 1)), tag="V", va="0x004080F0, 0x00407C72 (defaults), 0x0044256B",
             doc="device buttons (PAD_* mask) that act as logical button k; default = that button "
                 "(k: 0 cross 0x80, 1 square 0x20, 2 circle 0x40, 3 triangle 0x10, 4 L1 0x100, 5 R1 0x200, 6 start 0x4000)"),
    FieldDef("profile.<p>.dpad.<k>", "u32", 0, 0xFFFFFFFF, "profile", lambda p, k: PROFILE_DPAD_OFF + 4 * k,
             idx=(P2, _r(0, 3)), tag="V", va="0x00441CB0, 0x0044256B",
             doc="CONTROLLER-screen binding of up/down/left/right; stored but not applied (the d-pad is not remapped)"),
    FieldDef("profile.<p>.keyboard_layout", "u8", 0, 4, "profile", 0x4B, idx=(P2,), tag="V",
             va="0x0045BE60 (pad 0 keymap 0x0055BB98 + 32 * layout), 0x00441CB0",
             doc="keyboard layout 1..5 (profile 0 only is applied)", choices=KEYBOARD_LAYOUT_NAMES),
    # ---- per-level records (file 0xC44, 12 x 24 x 20 bytes) ----
    FieldDef("score.<L>.<e>.value", "u32", 0, 0xFFFFFFFF, "score", "score", idx=(L12, E24), tag="V", va="0x00407D20",
             doc="record value (entry 0 = best score [I])"),
    FieldDef("score.<L>.<e>.rider", "u8", 0, 11, "score", "b4", idx=(L12, E24), tag="I", choices=RIDER_NAMES),
    FieldDef("score.<L>.<e>.b5", "u8", 0, 0xFF, "score", "b5", idx=(L12, E24), tag="I"),
    FieldDef("score.<L>.<e>.b6", "u8", 0, 0xFF, "score", "b6", idx=(L12, E24), tag="I"),
    FieldDef("score.<L>.<e>.name", "str", 0, S.SCORE_NAME_LEN - 1, "score", "name", idx=(L12, E24), tag="V"),
)


def _call(x, idx):
    return x(*idx) if callable(x) else x


def resolve(name):
    """Return (FieldDef, idx tuple) for a concrete field name, or raise FieldError."""
    for f in FIELDS:
        m = f.regex.match(name)
        if not m:
            continue
        idx = tuple(int(g) for g in m.groups())
        for i, (v, rng) in enumerate(zip(idx, f.idx)):
            if v not in rng:
                raise FieldError(f"{name}: index {i} = {v} out of range {rng.start}..{rng.stop - 1}")
        return f, idx
    raise FieldError(f"unknown field {name!r} (see 'dmbmx save list-fields')")


def _buf(save, f, idx):
    if f.block == "opt":
        return save.options
    if f.block == "tail":
        return save.block_a_tail
    if f.block == "rider":
        return save.riders[idx[0]]
    if f.block == "profile":
        return save.profiles[idx[0]].raw
    raise AssertionError(f.block)


_INT = {"u8": "<B", "u16": "<H", "u32": "<I"}


def _width(mask):
    return 4 if mask > 0xFFFF else 2 if mask > 0xFF else 1


def get(save, name):
    f, idx = resolve(name)
    if f.block == "score":
        e = save.scores[idx[0]][idx[1]]
        return e.name if f.off == "name" else getattr(e, f.off)
    buf, off = _buf(save, f, idx), _call(f.off, idx)
    if f.kind in _INT:
        return struct.unpack_from(_INT[f.kind], buf, off)[0]
    if f.kind == "bool":
        mask = _call(f.bit, idx)
        return int(bool(int.from_bytes(buf[off:off + _width(mask)], "little") & mask))
    if f.kind == "nibble":
        return (buf[off] >> _call(f.bit, idx)) & 0xF
    if f.kind == "str":
        return S._cstr(buf[off:off + f.hi + 1])
    raise AssertionError(f.kind)


def parse_value(f, text):
    """Parse CLI text for field ``f`` (ints: decimal or 0x hex, or a choice name; bool: 0/1/on/off/true/false)."""
    if f.kind == "str":
        return text
    t = str(text).strip().lower()
    if f.kind == "bool" and t in ("on", "true", "yes", "y"):
        return 1
    if f.kind == "bool" and t in ("off", "false", "no", "n"):
        return 0
    for i, c in enumerate(f.choices):
        if f.kind != "bool" and t == c.lower():
            return i
    try:
        return int(t, 0)
    except ValueError:
        raise FieldError(f"{f.pattern}: {text!r} is not a number" + (f" or one of {list(f.choices)}" if f.choices and f.kind != "bool" else "")) from None


def set(save, name, value):  # noqa: A001 - mirrors the CLI verb
    """Set field ``name`` to ``value`` (text or int). Raises FieldError on a bad name or range."""
    f, idx = resolve(name)
    v = parse_value(f, value) if isinstance(value, str) else value
    if f.kind == "str":
        b = str(v).encode("latin-1", errors="strict") if all(ord(c) < 256 for c in str(v)) else None
        if b is None or len(b) > f.hi or b"\0" in b:
            raise FieldError(f"{name}: text must be latin-1, no NUL, at most {f.hi} chars")
    elif not (f.lo <= v <= f.hi):
        raise FieldError(f"{name}: {v} out of range {f.lo}..{f.hi}")
    if f.block == "score":
        e = save.scores[idx[0]][idx[1]]
        if f.off == "name":
            e.name = v
        else:
            setattr(e, f.off, v)
        return
    buf, off = _buf(save, f, idx), _call(f.off, idx)
    if f.kind in _INT:
        struct.pack_into(_INT[f.kind], buf, off, v)
    elif f.kind == "bool":
        mask = _call(f.bit, idx)
        width = _width(mask)
        cur = int.from_bytes(buf[off:off + width], "little")
        cur = (cur | mask) if v else (cur & ~mask)
        buf[off:off + width] = cur.to_bytes(width, "little")
    elif f.kind == "nibble":
        sh = _call(f.bit, idx)
        buf[off] = (buf[off] & ~(0xF << sh) & 0xFF) | (v << sh)
    elif f.kind == "str":
        n = f.hi + 1
        buf[off:off + n] = str(v).encode("latin-1") + b"\0" * (n - len(str(v)))


def label(f, v):
    """Human label for value v of field f, if it has named choices (button masks: the button names)."""
    if f.kind == "bool" and f.choices:
        return None
    if f.pattern.endswith((".control.<k>", ".dpad.<k>")) and isinstance(v, int):
        return control_label(v)
    if isinstance(v, int) and 0 <= v < len(f.choices):
        return f.choices[v]
    return None


def unlock_all(save):
    """Unlock what the game's own cheats / career rewards unlock. Returns [(field, old, new)].

    riders 12 (Slim Jim Guy + Amish Boy), all 8 cheats, and for every rider: 12 levels,
    5 styles, 5 bikes, plus the movie for riders 0, 1 and 11. Goals/scores are untouched.
    """
    want = [("riders_unlocked", 12), ("cheats_unlocked", get(save, "cheats_unlocked") | 0xFF)]
    for r in range(S.RIDER_COUNT):
        want += [(f"rider.{r}.levels_unlocked", 12), (f"rider.{r}.styles_unlocked", 5),
                 (f"rider.{r}.bikes_unlocked", 5)]
        if r in MOVIE_RIDERS:
            want.append((f"rider.{r}.movie_unlocked", 1))
    changes = []
    for name, v in want:
        old = get(save, name)
        new = max(old, v)        # never lowers anything (for cheats, v already includes the old bits)
        if new != old:
            set(save, name, new)
            changes.append((name, old, new))
    return changes


def control_label(mask):
    """Names of the PAD_* buttons in a control / d-pad binding mask, e.g. 0x80 -> 'Fire 4'."""
    names = [n for b, n in enumerate(PAD_BUTTON_NAMES) if mask & (1 << b)]
    extra = mask & ~((1 << len(PAD_BUTTON_NAMES)) - 1)
    if extra:
        names.append(f"0x{extra:x}")
    return "+".join(names) if names else "none"


def concrete_names(f):
    """All concrete names of a field pattern (for small patterns) - used by tests and list-fields --all."""
    import itertools
    for combo in itertools.product(*f.idx):
        name = f.pattern
        for v in combo:
            name = re.sub(r"<\w+>", str(v), name, count=1)
        yield name

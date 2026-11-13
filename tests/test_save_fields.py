"""Save editor: field registry, unlock-all, CLI (synthetic + real save)."""
import struct

import pytest

from conftest import corpus_files
from dmbmx.cli import main
from dmbmx.formats import save
from dmbmx.formats import save_fields as F
from dmbmx.integrity.crc import check_save


def synthetic():
    """A save as the game's defaults (save_InitDefaults 0x00407BA0) would write it."""
    d = bytearray(save.SIZE)
    struct.pack_into("<II", d, 0, save.MAGIC, save.SIZE)
    d[0x0C:0x10] = bytes([18, 18, 1, 10])
    for r in range(12):
        d[0x10 + r * 0x104 + 4:0x10 + r * 0x104 + 7] = b"\1\1\1"
    for p in range(2):
        o = save.PROFILES_OFF + p * save.PROFILE_LEN
        d[o:o + 2] = bytes([3, p])
        d[o + 7:o + 13] = b"player"
    return bytes(d)


ALL = [(f, n) for f in F.FIELDS for n in F.concrete_names(f)]


def test_every_name_resolves_to_its_own_field():
    assert len(ALL) > 8000
    for f, n in ALL[::7]:
        assert F.resolve(n)[0] is f, n


@pytest.mark.parametrize("f", F.FIELDS, ids=lambda f: f.pattern)
def test_set_get_roundtrip_and_locality(f):
    base = synthetic()
    for n in list(F.concrete_names(f))[:40]:
        s = save.read(base)
        old = F.get(s, n)
        new = ("ZZ" if old != "ZZ" else "Y") if f.kind == "str" else (f.hi if old != f.hi else f.lo)
        F.set(s, n, new)
        assert F.get(s, n) == new
        out = save.write(s)
        changed = [i for i in range(len(base)) if base[i] != out[i]]
        assert changed and max(changed) - min(changed) < 32, (n, changed[:5])
        F.set(s, n, old)
        assert save.write(s) == base, n


def test_ranges_and_errors():
    s = save.read(synthetic())
    for name, bad in [("rider.0.bikes_unlocked", 6), ("rider.0.bikes_unlocked", 0), ("riders_unlocked", 13),
                      ("profile.0.camera", 4), ("rider.0.level.0.tier", 3), ("profile.0.name", "x" * 29),
                      ("score.0.0.name", "x" * 13), ("rider.0.comp.6", "4"), ("music_track", "nope")]:
        with pytest.raises(F.FieldError):
            F.set(s, name, bad)
    for name in ("bogus", "rider.12.flags", "rider.0.level.6.tier", "rider.0.comp.5", "profile.2.name"):
        with pytest.raises(F.FieldError):
            F.resolve(name)
    assert save.write(s) == synthetic()       # failed sets change nothing


def test_value_parsing():
    s = save.read(synthetic())
    F.set(s, "profile.0.camera", "first person")
    F.set(s, "profile.1.rider", "Amish Boy")
    F.set(s, "cheat_unlocked.6", "on")
    F.set(s, "rider.3.flags", "0x104")
    assert (F.get(s, "profile.0.camera"), F.get(s, "profile.1.rider")) == (3, 11)
    assert F.get(s, "cheats_unlocked") == 0x40 and F.get(s, "rider.3.flags") == 0x104
    assert F.get(s, "rider.3.park_complete.0") == 1 and F.get(s, "rider.3.movie_unlocked") == 0


def test_unlock_all_defaults():
    s = save.read(synthetic())
    ch = F.unlock_all(s)
    assert F.get(s, "riders_unlocked") == 12 and F.get(s, "cheats_unlocked") == 0xFF
    for r in range(12):
        assert [F.get(s, f"rider.{r}.{k}_unlocked") for k in ("levels", "styles", "bikes")] == [12, 5, 5]
        assert F.get(s, f"rider.{r}.movie_unlocked") == (r in F.MOVIE_RIDERS)
    assert len(ch) == 2 + 12 * 3 + 3
    assert F.unlock_all(s) == []                 # idempotent
    out = save.write(s.resign())
    assert check_save(out)[0] and save.write(save.read(out)) == out


def test_unlock_all_never_lowers():
    s = save.read(synthetic())
    F.set(s, "cheats_active", 0x48)
    F.set(s, "rider.0.flags", 0x1FF)
    F.unlock_all(s)
    assert F.get(s, "rider.0.flags") == 0x1FF and F.get(s, "cheats_active") == 0x48


def test_real_save_known_values(game_dir):
    s = save.read(corpus_files(".dat")[0].read_bytes())
    expect = {"riders_unlocked": 11, "music_track": 7, "rider.0.levels_unlocked": 12, "rider.1.styles_unlocked": 3,
              "rider.0.level.0.tier": 2, "rider.0.level.0.goal.2.3.done": 1, "rider.0.comp.6": 3,
              "profile.0.name": "Tomos", "profile.0.rider": 10, "profile.1.name": "player",
              "score.0.0.value": 123688, "score.1.1.rider": 1}
    assert {k: F.get(s, k) for k in expect} == expect
    for f, n in ALL:                             # every field readable on the real save
        F.get(s, n)


def test_cli_set_and_unlock(tmp_path, capsys):
    src = tmp_path / "SAVE.DAT"
    src.write_bytes(synthetic())
    out = tmp_path / "out.dat"
    assert main(["save", "set", str(src), "rider.2.bikes_unlocked", "4", "profile.0.name", "Bob", "-o", str(out)]) == 0
    assert src.read_bytes() == synthetic()       # input untouched
    s = save.read(out.read_bytes())
    assert F.get(s, "rider.2.bikes_unlocked") == 4 and F.get(s, "profile.0.name") == "Bob"
    with pytest.raises(SystemExit):
        main(["save", "set", str(src), "rider.2.bikes_unlocked", "9", "-o", str(out)])
    with pytest.raises(SystemExit):
        main(["save", "set", str(src), "rider.2.bikes_unlocked", "4"])          # no -o / --in-place
    with pytest.raises(SystemExit):
        main(["save", "set", str(src), "rider.2.bikes_unlocked", "4", "-o", str(src)])
    assert main(["save", "unlock-all", str(src), "--in-place"]) == 0
    assert F.get(save.read(src.read_bytes()), "riders_unlocked") == 12
    capsys.readouterr()
    assert main(["save", "get", str(src), "riders_unlocked", "profile.0.camera"]) == 0
    assert "riders_unlocked = 12" in capsys.readouterr().out
    assert main(["save", "list-fields", "--grep", "bikes"]) == 0
    assert main(["save", "get", str(src), "nope"]) == 2


def test_cli_fixes_bad_header_only_when_lenient(tmp_path):
    bad = bytearray(synthetic())
    struct.pack_into("<I", bad, 4, 0)
    src = tmp_path / "bad.dat"
    src.write_bytes(bad)
    with pytest.raises(SystemExit):
        main(["save", "unlock-all", str(src), "-o", str(tmp_path / "o.dat")])
    assert main(["save", "unlock-all", str(src), "--lenient", "-o", str(tmp_path / "o.dat")]) == 0
    assert check_save((tmp_path / "o.dat").read_bytes())[0]

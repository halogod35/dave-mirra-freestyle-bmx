"""DM-015 sound banks: unit (synthetic), corpus round-trip (22 banks), IMA codec, replace, fuzz, CLI."""
import math
import struct
import wave

import pytest

from conftest import GAME_DIR, corpus_files
from fuzz import fuzz_parser
from dmbmx.cli import main
from dmbmx.core.errors import FormatError
from dmbmx.formats import audio


def synth_bank(samples, rates, stream=False, loop=None):
    """Bank with one sound def + one sample def per sample (or a stream bank)."""
    nA = nB = 0 if stream else len(samples)
    nC = len(samples)
    offA = 0x48
    offB = offA + nA * 20
    offC = offB + nB * 24
    end = offC + nC * 8 + 1                    # odd end -> exercises the 4-byte padding
    h = bytearray(end)
    h[0:8] = b"ZSNDWIN "
    struct.pack_into("<II", h, 8, 0, end)
    for co, n, off in ((0x10, nA, offA), (0x18, nB, offB), (0x20, nC, offC), (0x28, 0, end), (0x30, 0, end),
                       (0x40, 0, end)):
        struct.pack_into("<II", h, co, n, off)
    struct.pack_into("<II", h, 0x38, 0, end)
    for i in range(nB):
        struct.pack_into("<HHI", h, offA + 20 * i, i, 0x1000, 0)
        fl, le = (1, len(samples[i]) // 2 - 1) if loop else (0, 0)
        struct.pack_into("<HHIIIIHH", h, offB + 24 * i, i, fl, rates[i], 0, 0, le, 0, 0)
    b = audio.Bank(h, list(samples), b"")
    return audio.write(b)


def tone(freq, rate, secs, amp=8000):
    n = int(rate * secs)
    return struct.pack(f"<{n}h", *(int(amp * math.sin(2 * math.pi * freq * k / rate)) for k in range(n)))


def test_synthetic_roundtrip_and_layout():
    data = synth_bank([tone(440, 22050, 0.01), tone(880, 11025, 0.02)], [22050, 11025])
    b = audio.read(data)
    assert audio.write(b) == data
    assert b.data_end % 4 == 1 and data[b.data_end:b.data_end + 3] == b"\0\0\0"
    assert [b.sample_info(i)[:2] for i in range(2)] == [("pcm16", 22050), ("pcm16", 11025)]
    assert struct.unpack_from("<I", data, 8)[0] == len(data)


def test_read_errors():
    good = synth_bank([b"\1\0" * 10], [22050])
    with pytest.raises(FormatError):
        audio.read(b"ZSNDWIN ")                         # truncated
    with pytest.raises(FormatError):
        audio.read(b"ZSNX" + good[4:])
    with pytest.raises(FormatError):
        audio.read(good[:4] + b"PSX " + good[8:])
    with pytest.raises(FormatError):
        audio.read(good + b"x")                          # total size field mismatch
    assert audio.read(good + b"x", lenient=True).samples[0] == b"\1\0" * 10
    bad = bytearray(good)
    struct.pack_into("<I", bad, 0x20, 0x7FFFFFFF)      # huge count
    with pytest.raises(FormatError):
        audio.read(bytes(bad))


def test_ima_codec():
    pcm = tone(300, 44100, 0.05)
    enc = audio.ima_encode(pcm)
    assert len(enc) == (len(pcm) // 2 + 1) // 2
    dec = audio.ima_decode(enc)[:len(pcm)]
    a = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    b = struct.unpack(f"<{len(dec) // 2}h", dec)
    err = max(abs(x - y) for x, y in zip(a[200:], b[200:]))
    assert err < 600, err                               # ADPCM tracks a sine closely after warm-up
    assert audio.ima_encode(audio.ima_decode(enc)) == enc   # encoder is decoder-exact


def test_replace_pcm_and_stream():
    data = synth_bank([tone(440, 22050, 0.01), tone(880, 22050, 0.02)], [22050, 22050], loop=True)
    b = audio.read(data)
    notes = audio.replace_sample(b, 0, tone(1000, 11025, 0.05), 11025)    # resampled to 22050
    out = audio.write(b)
    b2 = audio.read(out)
    assert b2.frames(0) == round(0.05 * 11025) * 2 and b2.samples[1] == b.samples[1]
    assert b2.sample_defs[0].loop_end == b2.frames(0) - 1 and notes
    s = synth_bank([b"\0" * 32], [0], stream=True)
    bs = audio.read(s)
    audio.replace_sample(bs, 0, tone(500, 44100, 0.01), 44100)
    assert audio.read(audio.write(bs)).sample_info(0)[0] == "ima"
    with pytest.raises(ValueError):
        audio.replace_sample(bs, 5, b"", 44100)


def test_to_mono_and_resample():
    st = struct.pack("<4h", 100, 300, -100, -300)
    assert audio.to_mono16(st, 2, 2) == struct.pack("<2h", 200, -200)
    assert audio.to_mono16(bytes([128, 255]), 1, 1) == struct.pack("<2h", 0, 127 << 8)
    assert len(audio.resample(tone(100, 8000, 0.1), 8000, 16000)) == 1600 * 2


# ---------------------------------------------------------------- corpus
def test_corpus_count(game_dir):
    banks = [p for p in corpus_files(".zsd") if p.read_bytes()[:4] == b"ZSND"]
    assert len(banks) == 22


@pytest.mark.parametrize("path", [pytest.param(p, id=p.name) for p in corpus_files(".zsd")
                                  if p.parent.name.lower() == "audio"] or
                         [pytest.param(None, marks=pytest.mark.skip(reason="no corpus"))])
def test_corpus_roundtrip(path):
    data = path.read_bytes()
    b = audio.read(data)
    out = audio.write(b)
    assert out == data, f"first diff at 0x{next(i for i, (x, y) in enumerate(zip(out, data)) if x != y):x}"
    for i in range(len(b.samples)):
        codec, rate, users = b.sample_info(i)
        assert rate > 0
        if codec == "pcm16":
            assert len(b.samples[i]) % 2 == 0
            for j in users:                               # loop points inside the 16-bit frame count
                assert b.sample_defs[j].loop_end < b.frames(i)


def test_corpus_replace_keeps_others(game_dir):
    data = (GAME_DIR / "bin" / "audio" / "sfxcom.zsd").read_bytes()
    b = audio.read(data)
    old = list(b.samples)
    audio.replace_sample(b, 2, tone(660, 22050, 0.3), 22050)
    b2 = audio.read(audio.write(b))
    assert [s for k, s in enumerate(b2.samples) if k != 2] == [s for k, s in enumerate(old) if k != 2]
    # in the header area only the total size (0x08) and table C (offsets/sizes) may change
    c0, c1 = b2.offset("C"), b2.offset("C") + 8 * len(b2.samples)
    diff = [k for k in range(len(b2.header)) if b2.header[k] != data[k]]
    assert diff and all(8 <= k < 12 or c0 <= k < c1 for k in diff)


def test_corpus_music_decodes_like_audio(game_dir):
    b = audio.read((GAME_DIR / "bin" / "audio" / "music.zsd").read_bytes())
    pcm = audio.ima_decode(b.samples[0][50000:60000])
    v = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    step = sum(abs(v[k + 1] - v[k]) for k in range(len(v) - 1)) / (len(v) - 1)
    rms = (sum(x * x for x in v) / len(v)) ** 0.5
    assert rms > 300 and step < 0.3 * rms                # smooth music, not noise (wrong layout = noise)


def test_corpus_fuzz(game_dir):
    a = GAME_DIR / "bin" / "audio"
    for name in ("speech.zsd", "sfxlev12.zsd", "sfxchr00.zsd", "sfxlev00.zsd"):
        fuzz_parser(audio.read, (a / name).read_bytes(), n=200, seed=0)


def test_unit_fuzz():
    fuzz_parser(audio.read, synth_bank([tone(440, 8000, 0.01)] * 3, [8000] * 3, loop=True), n=300, seed=3)


# ---------------------------------------------------------------- CLI
def test_cli(tmp_path, capsys):
    bank = tmp_path / "b.zsd"
    bank.write_bytes(synth_bank([tone(440, 22050, 0.02), tone(220, 16000, 0.02)], [22050, 16000]))
    assert main(["audio", "list", str(bank)]) == 0
    assert "pcm16  16000 Hz" in capsys.readouterr().out
    assert main(["audio", "extract", str(bank), str(tmp_path / "w")]) == 0
    with wave.open(str(tmp_path / "w" / "b_001.wav")) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()) == (16000, 1, 2, 320)
    src = tmp_path / "in.wav"
    with wave.open(str(src), "wb") as w:
        w.setnchannels(2), w.setsampwidth(2), w.setframerate(44100)
        w.writeframes(struct.pack("<8h", *[1000, -1000] * 4))
    out = tmp_path / "o.zsd"
    assert main(["audio", "replace", str(bank), "1", str(src), str(out)]) == 0
    assert audio.read(out.read_bytes()).frames(1) == round(4 * 16000 / 44100)
    with pytest.raises(SystemExit):
        main(["audio", "replace", str(bank), "1", str(src), str(bank)])
    with pytest.raises(SystemExit):
        main(["audio", "replace", str(bank), "9", str(src), str(out)])


def test_integrity_walk_skips_non_game_files(tmp_path, capsys):
    (tmp_path / "x.pyc").write_bytes(b"\x89PNG\r\n\x1a\n garbage")
    (tmp_path / "b.zsd").write_bytes(synth_bank([b"\0\0"], [8000]))
    assert main(["integrity", "check", str(tmp_path)]) == 0
    out = capsys.readouterr().out
    assert "x.pyc" not in out and "audio bank" in out

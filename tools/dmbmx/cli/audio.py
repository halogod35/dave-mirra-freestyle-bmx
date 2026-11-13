"""dmbmx audio list|extract|replace|ztrk-dump: sound banks bin/audio/*.zsd <-> WAV (stdlib ``wave``)."""
import wave
from pathlib import Path

from dmbmx.core.errors import FormatError
from dmbmx.formats import audio


def register(sub):
    p = sub.add_parser("audio", help="sound banks (bin/audio/*.zsd): list, extract to WAV, replace")
    ss = p.add_subparsers(dest="audio_cmd", metavar="<audio command>")
    pl = ss.add_parser("list", help="list the sample files of a bank")
    pl.add_argument("bank")
    pl.set_defaults(func=run_list)
    pe = ss.add_parser("extract", help="write every sample (or --id N) as WAV")
    pe.add_argument("bank")
    pe.add_argument("outdir")
    pe.add_argument("--id", type=int, action="append", help="only this sample index (repeatable)")
    pe.set_defaults(func=run_extract)
    pr = ss.add_parser("replace", help="replace sample <id> with a WAV (encoded to the bank's native format)")
    pr.add_argument("bank")
    pr.add_argument("id", type=int)
    pr.add_argument("wav")
    pr.add_argument("output", help="the new bank file (never the input)")
    pr.set_defaults(func=run_replace)
    pz = ss.add_parser("ztrk-dump", help="disassemble the bank's ZTRK sequencer tracks (level ambience)")
    pz.add_argument("bank")
    pz.add_argument("--track", type=int, action="append", help="only this track (repeatable)")
    pz.set_defaults(func=run_ztrk_dump)
    p.set_defaults(func=lambda a: p.print_help() or 1)


def _load(path):
    try:
        return audio.read(Path(path).read_bytes())
    except FormatError as e:
        raise SystemExit(f"error: {e}")


def run_list(args):
    b = _load(args.bank)
    print(f"{args.bank}: {len(b.samples)} sample file(s), {b.count('A')} sound defs, {b.count('B')} sample defs, "
          f"{b.count('E')} ZTRK tracks{'  [stream bank: IMA-ADPCM mono 44100 Hz]' if b.is_stream_bank else ''}")
    defs = b.sample_defs
    for i, s in enumerate(b.samples):
        codec, rate, users = b.sample_info(i)
        loops = [f"B{j} loop {defs[j].loop_start}-{defs[j].loop_end}" for j in users if defs[j].flags & 1]
        print(f"  {i:3d}  {codec:5} {rate:6d} Hz  {b.frames(i) / rate:7.2f} s  {len(s):9d} bytes"
              f"  defs={users}" + (f"  {'; '.join(loops)}" if loops else ""))
    if b.count("A"):
        _list_voices(b, Path(args.bank).stem)
    return 0


def _list_voices(b, stem):
    """The voices, with the game events that play them (bmxsoundEvent, docs/re/sound.md)."""
    try:
        amb = audio.ztrk_voices(b)
    except FormatError:
        amb = {}
    names = audio.voice_events(stem, b.count("A"))
    defs = b.sample_defs
    print(f"voices (event names from the exe's bmxsoundEvent; bank '{stem}'):")
    for i, v in enumerate(b.voices):
        if v.sample == 0xFFFF or v.sample >= len(defs):
            where = "no sample"
        else:
            where = f"sample def {v.sample} -> file {defs[v.sample].sample}"
        env = f"att {v.attack} dec {v.decay} sus {v.sustain_time}/{v.sustain_level} rel {v.release}"
        what = list(names.get(i, []))
        if i in amb:
            what.append(f"ambience (ZTRK track {', '.join(map(str, amb[i]))})")
        print(f"  v{i:<3d} {where:28s} vol {v.volume:3d} pan {v.pan:4d} pitch 0x{v.pitch:04x}  {env}"
              + (f"  : {' | '.join(what)}" if what else ""))


def run_ztrk_dump(args):
    b = _load(args.bank)
    try:
        tracks = b.ztrk_tracks()
    except FormatError as e:
        raise SystemExit(f"error: {e}")
    if not tracks:
        print(f"{args.bank}: no ZTRK tracks")
        return 0
    for t, (td, events) in enumerate(tracks):
        if args.track and t not in args.track:
            continue
        print(f"track {t}: ZTRK at 0x{td.ztrk:x}, volume {td.volume}, pan {td.pan}, voice "
              f"{'-' if td.voice == 0xFF else td.voice}, keymap {'-' if td.keymap == 0xFF else td.keymap}, "
              f"{len(events)} events")
        clock = 0
        for e in events:
            clock += e.delta
            print(f"  +{e.offset:<5d} t={clock / 1000:9.3f} s  {e.describe()}")
    return 0


def write_wav(path, pcm, rate):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def run_extract(args):
    b = _load(args.bank)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    stem = Path(args.bank).stem
    ids = args.id if args.id else range(len(b.samples))
    for i in ids:
        if not 0 <= i < len(b.samples):
            raise SystemExit(f"error: sample {i} out of range 0..{len(b.samples) - 1}")
        codec, rate, _ = b.sample_info(i)
        pcm = audio.ima_decode(b.samples[i]) if codec == "ima" else b.samples[i]
        p = out / f"{stem}_{i:03d}.wav"
        write_wav(p, pcm, rate)
        print(f"wrote {p} ({codec}, {rate} Hz, {len(pcm) // 2 / rate:.2f} s)")
    return 0


def run_replace(args):
    if Path(args.output).resolve() == Path(args.bank).resolve():
        raise SystemExit("error: output equals the input bank; write a new file")
    b = _load(args.bank)
    try:
        with wave.open(args.wav, "rb") as w:
            if w.getcomptype() != "NONE":
                raise SystemExit("error: WAV must be uncompressed PCM")
            ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            raw = w.readframes(w.getnframes())
    except (wave.Error, EOFError) as e:
        raise SystemExit(f"error: cannot read {args.wav}: {e}")
    try:
        pcm = audio.to_mono16(raw, ch, width)
        notes = audio.replace_sample(b, args.id, pcm, rate)
    except ValueError as e:
        raise SystemExit(f"error: {e}")
    data = audio.write(b)
    Path(args.output).write_bytes(data)
    for n in notes:
        print(n)
    print(f"wrote {args.output} ({len(data)} bytes)")
    return 0

"""PNG chunk CRCs of the PNG images embedded in game files (TPNG in ZMD and Mat._sd).

The game decodes TPNG through RenderWare's PNG reader (0x005068A0) on libpng 1.0.5 with the
default CRC policy (no png_set_crc_action call): a bad CRC on a **critical** chunk (IHDR, PLTE,
IDAT, IEND) is a libpng error -> the image read returns NULL -> zrwTxr asserts "Failed to create
texture" (0x00497CF6) and the texture is missing; a bad CRC on an **ancillary** chunk is a warning
and the chunk is dropped. See docs/re/integrity-checks.md.

``check(data)`` finds bad chunk CRCs in every embedded PNG; ``fix(data)`` rewrites them.
Both work on any file (a bare .png, a ZMD, a Mat._sd) and never change the file length.
"""
import struct
import zlib

from dmbmx.core.errors import FormatError, TruncatedError

SIG = b"\x89PNG\r\n\x1a\n"
FMT = "PNG"
MAX_CHUNKS = 100_000


def chunks(data, start=0):
    """Yield (offset, type, data_len, stored_crc, computed_crc) for the PNG at ``start`` until IEND.

    Raises TruncatedError/FormatError on a malformed stream (bounds-checked, no allocation).
    """
    if data[start:start + 8] != SIG:
        raise FormatError("no PNG signature", fmt=FMT, offset=start)
    o = start + 8
    for _ in range(MAX_CHUNKS):
        if o + 12 > len(data):
            raise TruncatedError("chunk header/CRC runs past the end", fmt=FMT, offset=o)
        n = struct.unpack_from(">I", data, o)[0]
        if n > len(data) - o - 12:
            raise TruncatedError(f"chunk length {n} runs past the end", fmt=FMT, offset=o)
        ctype = bytes(data[o + 4:o + 8])
        if not all(65 <= c <= 90 or 97 <= c <= 122 for c in ctype):
            raise FormatError(f"invalid chunk type {ctype!r}", fmt=FMT, offset=o + 4)
        stored = struct.unpack_from(">I", data, o + 8 + n)[0]
        computed = zlib.crc32(memoryview(data)[o + 4:o + 8 + n]) & 0xFFFFFFFF
        yield o, ctype.decode("ascii"), n, stored, computed
        o += 12 + n
        if ctype == b"IEND":
            return
    raise FormatError("too many chunks", fmt=FMT, offset=o)


def find(data):
    """Offsets of every PNG signature in ``data``."""
    out, i = [], data.find(SIG)
    while i >= 0:
        out.append(i)
        i = data.find(SIG, i + 1)
    return out


def check(data):
    """List of problems: (png_offset, chunk_offset, type, stored, computed, critical) for bad CRCs.

    A PNG that can't be walked is reported as (png_offset, None, error message, ...).
    """
    bad = []
    for p in find(data):
        try:
            for o, t, _n, st, cp in chunks(data, p):
                if st != cp:
                    bad.append((p, o, t, st, cp, t[0].isupper()))
        except FormatError as e:
            bad.append((p, None, str(e), None, None, True))
    return bad


def fix(data):
    """Return ``data`` with every embedded PNG chunk CRC recomputed (length unchanged)."""
    out = bytearray(data)
    for p in find(data):
        try:
            for o, _t, n, st, cp in chunks(data, p):
                if st != cp:
                    struct.pack_into(">I", out, o + 8 + n, cp)
        except FormatError:
            continue            # not a walkable PNG (e.g. a stray signature): leave it alone
    return bytes(out)

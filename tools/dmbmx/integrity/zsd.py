"""Level scene file (.ZSD) component-size check: the SCFI entries.

A ZSD is ``SCEN{ SCDF, SCFI x N }`` (u32 tag, u32 size, LE). Each SCFI payload is 0x28 bytes:

    +0x00 u8      component type (1 Mat, 3 Tag, 4 BSP, ...)
    +0x04 u32     component file size
    +0x08 char[]  file name ("BSP._sd"), NUL-terminated

The zScene.c component loader (assert string 0x00561A54) loads each component with ``zfileLoad(name, size)`` (0x00485070): it allocates
``size`` bytes and reads exactly ``size`` bytes. A **shorter** file fails the read -> NULL ->
assert "Scene component file not found" (zScene.c line 0x2EE) and the level does not load; a **longer**
file is silently truncated to ``size`` bytes. So after editing a ``_sd`` file the SCFI size must be
set to the new length: ``resign()`` does that. [V] all 72 SCFI sizes in the corpus equal the real file sizes.
"""
import os
import struct
from dataclasses import dataclass

from dmbmx.core.errors import BadMagicError, FormatError, TruncatedError, need

FMT = "ZSD"
SCFI_LEN = 0x28


@dataclass
class ScfiEntry:
    offset: int        # file offset of the SCFI payload
    type: int
    size: int
    name: str

    @property
    def size_offset(self):
        return self.offset + 4


def entries(data):
    """Parse the SCFI entries of a ZSD (bounds-checked)."""
    need(data, 0, 8, fmt=FMT, what="SCEN header")
    if data[:4] != b"SCEN":
        raise BadMagicError(f"expected SCEN, got {bytes(data[:4])!r}", fmt=FMT, offset=0)
    end = 8 + struct.unpack_from("<I", data, 4)[0]
    if end > len(data):
        raise TruncatedError(f"SCEN size runs past the end ({end} > {len(data)})", fmt=FMT, offset=4)
    out, o = [], 8
    while o + 8 <= end:
        tag = bytes(data[o:o + 4])
        n = struct.unpack_from("<I", data, o + 4)[0]
        if n > end - o - 8:
            raise TruncatedError(f"{tag!r} size {n} runs past SCEN", fmt=FMT, offset=o)
        if tag == b"SCFI":
            if n < SCFI_LEN:
                raise FormatError(f"SCFI payload {n} < 0x28 bytes", fmt=FMT, offset=o)
            p = o + 8
            raw = bytes(data[p + 8:p + n])
            name = raw.split(b"\0", 1)[0].decode("latin-1")
            out.append(ScfiEntry(p, data[p], struct.unpack_from("<I", data, p + 4)[0], name))
        o += 8 + n
    return out


def check(zsd_path):
    """[(entry, real_size_or_None, ok)] for a ZSD on disk; ok = the game would load it unchanged."""
    data = open(zsd_path, "rb").read()
    folder = os.path.dirname(os.path.abspath(zsd_path))
    res = []
    for e in entries(data):
        p = os.path.join(folder, e.name)
        real = os.path.getsize(p) if os.path.isfile(p) else None
        res.append((e, real, real == e.size))
    return res


def resign(data, sizes):
    """Return ZSD bytes with SCFI sizes replaced from ``sizes`` ({file name (case-insensitive): size})."""
    out = bytearray(data)
    low = {k.lower(): v for k, v in sizes.items()}
    for e in entries(data):
        v = low.get(e.name.lower())
        if v is not None:
            if not 0 <= v <= 0xFFFFFFFF:
                raise ValueError(f"{e.name}: size {v} out of u32 range")
            struct.pack_into("<I", out, e.size_offset, v)
    return bytes(out)


def resign_file(zsd_path):
    """ZSD bytes re-signed against the component files next to it on disk."""
    data = open(zsd_path, "rb").read()
    folder = os.path.dirname(os.path.abspath(zsd_path))
    sizes = {}
    for e in entries(data):
        p = os.path.join(folder, e.name)
        if os.path.isfile(p):
            sizes[e.name] = os.path.getsize(p)
    return resign(data, sizes)

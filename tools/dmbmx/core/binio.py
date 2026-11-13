"""Small, generic little-endian binary helpers with bounds checks.

Every read goes through ``errors.need`` so a short buffer raises ``TruncatedError``
(never ``struct.error`` / ``IndexError``).
"""
import struct

from .errors import need

_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")


def u16(data, off, *, fmt=None, what="u16"):
    need(data, off, 2, fmt=fmt, what=what)
    return _U16.unpack_from(data, off)[0]


def u32(data, off, *, fmt=None, what="u32"):
    need(data, off, 4, fmt=fmt, what=what)
    return _U32.unpack_from(data, off)[0]


def u32s(data, off, count, *, fmt=None, what="u32 array"):
    """Read ``count`` consecutive u32 values (bounds-checked first)."""
    need(data, off, 4 * count, fmt=fmt, what=what)
    return struct.unpack_from(f"<{count}I", data, off)


def take(data, off, size, *, fmt=None, what="data"):
    """Return ``bytes(data[off:off+size])`` after checking it exists."""
    need(data, off, size, fmt=fmt, what=what)
    return bytes(data[off:off + size])


def p16(v):
    return _U16.pack(v)


def p32(v):
    return _U32.pack(v)


def align(n, a):
    """Round ``n`` up to a multiple of ``a``."""
    return (n + a - 1) // a * a

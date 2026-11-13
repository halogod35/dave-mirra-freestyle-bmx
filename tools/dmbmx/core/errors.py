"""Error contract for every dmbmx parser (see docs/testing.md).

Rule: when a reader is given malformed bytes it raises a ``FormatError`` (or a
subclass) with a message that says what was wrong and, where known, at which
offset. It must never raise IndexError, struct.error, KeyError, RecursionError,
MemoryError, UnicodeDecodeError, ... and it must never hang. ``tests/fuzz.py``
enforces this.

``FormatError`` subclasses ``ValueError`` so callers that already catch
``ValueError`` keep working.
"""


class FormatError(ValueError):
    """Base class: the bytes are not a valid file of the expected format.

    ``fmt``    short format name, e.g. "ZMD", "ZAL", "SAVE".
    ``offset`` file offset where the problem was found (int) or None.
    """

    def __init__(self, msg, *, fmt=None, offset=None):
        self.msg = msg
        self.fmt = fmt
        self.offset = offset
        prefix = f"{fmt}: " if fmt else ""
        where = f" (at 0x{offset:x})" if offset is not None else ""
        super().__init__(f"{prefix}{msg}{where}")


class TruncatedError(FormatError):
    """A read or a declared length runs past the end of the data/container."""


class BadMagicError(FormatError):
    """A magic number / 4CC tag / chunk id is not what the format requires."""


class ChecksumError(FormatError):
    """A stored CRC/checksum/size field does not match the data.

    Readers that support a lenient mode (e.g. to inspect a corrupt save) may
    skip this check on request, but the default must be to raise it.
    """


class LimitError(FormatError):
    """A count, size or nesting depth exceeds a sane limit (anti-DoS guard)."""


def need(data, offset, size, *, fmt=None, what="data"):
    """Bounds check helper: raise TruncatedError unless data[offset:offset+size] exists.

    Returns ``offset + size`` so it can be chained: ``end = need(buf, off, 8)``.
    """
    if offset < 0 or size < 0 or offset + size > len(data):
        raise TruncatedError(
            f"{what}: need {size} bytes, {max(0, len(data) - offset)} left",
            fmt=fmt, offset=offset)
    return offset + size

"""Self-test of tests/fuzz.py against toy parsers (no game data needed)."""
import struct
import threading
import time

import pytest

from fuzz import fuzz_parser, mutations
from dmbmx.core.errors import (FormatError, TruncatedError, BadMagicError,
                               LimitError, need)

# --- toy format: 4CC chunk tree like ZMD ("u32 tag, u32 size", LE) --------------
CONTAINERS = {b"MODL", b"MLOD", b"MDFF"}
MAX_DEPTH = 16


def toy_read(data, off=0, end=None, depth=0):
    """Correct parser: bounds-checks everything and raises only FormatError."""
    if end is None:
        end = len(data)
        need(data, 0, 4, fmt="TOY", what="magic")
        if data[:4] != b"MODL":
            raise BadMagicError(f"expected MODL, got {data[:4]!r}", fmt="TOY", offset=0)
    if depth > MAX_DEPTH:
        raise LimitError("chunk nesting too deep", fmt="TOY", offset=off)
    out = []
    while off < end:
        hdr = need(data, off, 8, fmt="TOY", what="chunk header")
        if hdr > end:
            raise TruncatedError("chunk header crosses parent end", fmt="TOY", offset=off)
        tag, size = struct.unpack_from("<4sI", data, off)
        body_end = hdr + size
        if body_end > end:
            raise TruncatedError(f"chunk {tag!r} size {size} past parent end", fmt="TOY", offset=off)
        if tag in CONTAINERS:
            out.append((tag, toy_read(data, hdr, body_end, depth + 1)))
        else:
            out.append((tag, data[hdr:body_end]))
        off = body_end
    return out


def chunk(tag, body):
    return struct.pack("<4sI", tag, len(body)) + body


SAMPLE = chunk(b"MODL", chunk(b"MLOD", chunk(b"MLDF", bytes(8)) +
                              chunk(b"MDFF", chunk(b"RWxx", bytes(range(64))))))


def test_toy_sample_parses():
    tree = toy_read(SAMPLE)
    assert tree[0][0] == b"MODL"


def test_correct_parser_passes():
    rep = fuzz_parser(toy_read, SAMPLE, n=300, seed=1)
    assert rep.cases == 300 and not rep.failures
    assert rep.rejected > 0  # the mutations really hit the error paths


def test_mutations_deterministic_and_cover_minimum():
    a = list(mutations(SAMPLE, 50, seed=7))
    b = list(mutations(SAMPLE, 50, seed=7))
    assert a == b and len(a) == 50
    descs = [d for _, d, _ in a]
    assert descs[0] == "truncate to 0"
    assert any(d.startswith("truncate") for d in descs[1:])
    assert any(d.startswith("flip") for d in descs)
    assert any(d.startswith("u32") for d in descs)
    assert all(buf != SAMPLE for _, _, buf in a)
    assert list(mutations(SAMPLE, 50, seed=8)) != a


def test_tiny_inputs():
    assert len(list(mutations(b"", 10))) == 1
    assert len(list(mutations(b"\x01", 10))) == 10
    assert len(list(mutations(SAMPLE, 2))) == 2


# --- buggy parsers must be caught ----------------------------------------------
def unchecked_read(data):
    """Bug: no bounds checks -> struct.error / IndexError on truncation."""
    tag, size = struct.unpack_from("<4sI", data, 0)
    return data[8 + size - 1]


def test_unchecked_parser_fails():
    with pytest.raises(AssertionError) as ei:
        fuzz_parser(unchecked_read, SAMPLE, n=20)
    msg = str(ei.value)
    assert "error" in msg and "case 0 [truncate to 0]" in msg


def test_plain_valueerror_is_not_clean_by_default():
    def read(data):
        if len(data) < len(SAMPLE):
            raise ValueError("short")
        return data
    with pytest.raises(AssertionError, match="ValueError: short"):
        fuzz_parser(read, SAMPLE, n=10)
    # ... unless the test explicitly widens the contract
    fuzz_parser(read, SAMPLE, n=10, allowed=(ValueError,))


def test_empty_message_is_not_clean():
    def read(data):
        raise FormatError("")
    with pytest.raises(AssertionError, match="empty message"):
        fuzz_parser(read, SAMPLE, n=3)


def test_recursion_and_memory_errors_fail():
    def deep(data):
        return deep(data)
    with pytest.raises(AssertionError, match="RecursionError"):
        fuzz_parser(deep, SAMPLE, n=2)

    def alloc(data):
        raise MemoryError()
    with pytest.raises(AssertionError, match="MemoryError"):
        fuzz_parser(alloc, SAMPLE, n=2)


def test_python_loop_hang_detected():
    """Zero-size chunk -> infinite loop in pure Python: interrupted by the tracer."""
    def loop_read(data):
        off = 0
        while off < len(data):
            size = data[off]  # bug: size 0 never advances
            off += size
        return off
    data = bytes([0]) + SAMPLE
    t0 = time.monotonic()
    with pytest.raises(AssertionError, match="hang"):
        fuzz_parser(loop_read, data, n=2, timeout=0.2)
    assert time.monotonic() - t0 < 5


def test_hang_outside_python_detected():
    """Blocked in C (no bytecode runs): caught by the join timeout."""
    release = threading.Event()

    def blocked_read(data):
        release.wait(30)
    try:
        t0 = time.monotonic()
        with pytest.raises(AssertionError, match="hang"):
            fuzz_parser(blocked_read, SAMPLE, n=1, timeout=0.2)
        assert time.monotonic() - t0 < 5
    finally:
        release.set()


def test_except_exception_cannot_swallow_timeout():
    def stubborn(data):
        while True:
            try:
                while True:
                    pass
            except Exception:
                pass
    with pytest.raises(AssertionError, match="hang"):
        fuzz_parser(stubborn, SAMPLE, n=1, timeout=0.2)


def test_formaterror_is_valueerror_and_formats_offset():
    e = TruncatedError("short", fmt="ZMD", offset=0x10)
    assert isinstance(e, ValueError) and isinstance(e, FormatError)
    assert str(e) == "ZMD: short (at 0x10)"
    assert e.offset == 0x10 and e.fmt == "ZMD"
    assert need(b"abcd", 1, 3) == 4
    with pytest.raises(TruncatedError):
        need(b"abcd", 2, 3)
    with pytest.raises(TruncatedError):
        need(b"abcd", -1, 1)

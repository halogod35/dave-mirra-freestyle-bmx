"""Truncation / bit-flip fuzz helper for dmbmx parsers (see docs/testing.md).

Usage in a format test::

    from fuzz import fuzz_parser
    from dmbmx.formats import zmd

    def test_zmd_fuzz(game_dir):
        data = (game_dir / "assets/.../x.zmd").read_bytes()
        fuzz_parser(zmd.read, data, n=200, seed=0)

Contract checked for every mutated input (``read_fn(bytes)``):
  * it returns normally (some mutations are still valid files), OR
  * it raises an exception from ``allowed`` (default: ``FormatError``) whose
    message is non-empty;
  * and it finishes within ``timeout`` seconds.
Anything else (IndexError, struct.error, KeyError, RecursionError,
MemoryError, UnicodeDecodeError, a hang, ...) is a failure. All failures are
collected and reported in one AssertionError; every case is reproducible from
``(seed, case index)`` via ``mutations()``.
"""
import random
import struct
import sys
import threading
import time

from dmbmx.core.errors import FormatError

__all__ = ["fuzz_parser", "mutations", "FuzzReport"]

# Values written over aligned u32 fields: typical length/offset/count killers.
_INTERESTING_U32 = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF8)


class _Hang(BaseException):
    """Raised inside the parser thread by the tracer when its time is up.

    BaseException so a parser's ``except Exception`` cannot swallow it.
    """


def mutations(data, n=200, seed=0):
    """Yield ``n`` deterministic ``(index, description, mutated_bytes)`` cases.

    Mix: ~1/3 truncations, ~1/3 bit flips (1-8 bits), ~1/3 u32 field
    overwrites with interesting values. The first cases are always: empty
    input, truncation to len-1, truncation to half, and a single bit flip in
    byte 0 (so a tiny ``n`` still covers the checklist's "one truncated and one
    bit-flipped" minimum).
    """
    data = bytes(data)
    L = len(data)
    rng = random.Random(seed)
    fixed = [("truncate to 0", b"")]
    if L:
        fixed.append((f"truncate to {L - 1}", data[: L - 1]))
        fixed.append((f"truncate to {L // 2}", data[: L // 2]))
        fixed.append(("flip bit 0 of byte 0x0", bytes([data[0] ^ 1]) + data[1:]))
    i = 0
    for desc, buf in fixed[:n]:
        yield i, desc, buf
        i += 1
    if not L:
        return
    while i < n:
        kind = rng.randrange(3)
        if kind == 0:
            cut = rng.randrange(L)
            yield i, f"truncate to {cut}", data[:cut]
        elif kind == 1 or L < 4:
            buf = bytearray(data)
            bits = sorted(rng.sample(range(L * 8), min(rng.randint(1, 8), L * 8)))
            for b in bits:
                buf[b >> 3] ^= 1 << (b & 7)
            where = ", ".join(f"0x{b >> 3:x}.{b & 7}" for b in bits)
            yield i, f"flip bits {where}", bytes(buf)
        else:
            off = rng.randrange(L // 4) * 4
            val = rng.choice(_INTERESTING_U32 + (L, L + 1, rng.getrandbits(32)))
            buf = bytearray(data)
            buf[off:off + 4] = struct.pack("<I", val)
            yield i, f"u32 at 0x{off:x} = 0x{val:x}", bytes(buf)
        i += 1


class FuzzReport:
    """Result summary; ``fuzz_parser`` returns it when every case was clean."""

    def __init__(self):
        self.cases = 0
        self.returned = 0          # parser accepted the mutated bytes
        self.rejected = 0          # parser raised an allowed exception
        self.failures = []         # (index, description, problem)
        self.slowest = 0.0         # seconds

    def __repr__(self):
        return (f"FuzzReport(cases={self.cases}, returned={self.returned}, "
                f"rejected={self.rejected}, failures={len(self.failures)}, "
                f"slowest={self.slowest:.3f}s)")


def _run_with_deadline(fn, arg, timeout):
    """Run fn(arg) in a daemon thread. Returns (status, value, seconds).

    status: "ok" (value = return value), "exc" (value = exception),
    "hang" (value = None). Pure-Python loops are interrupted by a tracer that
    raises _Hang after the deadline; code stuck in C (e.g. a huge allocation
    or ``bytes * n``) is detected by the join timeout and the thread is
    abandoned (daemon), which is still reported as a hang.
    """
    out = {}
    deadline = time.monotonic() + timeout
    counter = [0]

    def tracer(frame, event, _arg):
        counter[0] += 1
        if counter[0] & 0x3FF == 0 and time.monotonic() > deadline:
            raise _Hang()
        return tracer

    def worker():
        sys.settrace(tracer)
        t0 = time.monotonic()
        try:
            out["value"] = fn(arg)
            out["status"] = "ok"
        except _Hang:
            out["status"] = "hang"
        except BaseException as e:  # noqa: BLE001 - we classify everything
            out["status"] = "exc"
            out["value"] = e
        finally:
            sys.settrace(None)
            out["secs"] = time.monotonic() - t0

    th = threading.Thread(target=worker, name="fuzz-case", daemon=True)
    t0 = time.monotonic()
    th.start()
    th.join(timeout + max(1.0, timeout))  # grace for the tracer to fire
    if th.is_alive() or "status" not in out:
        return "hang", None, time.monotonic() - t0
    return out["status"], out.get("value"), out["secs"]


def fuzz_parser(read_fn, data, n=200, seed=0, *, timeout=1.0,
                allowed=(FormatError,), max_report=20):
    """Fuzz ``read_fn`` with ``n`` truncated / bit-flipped variants of ``data``.

    read_fn   callable taking ``bytes``; its return value is ignored.
    data      a valid sample file (bytes); fuzzing starts from real files.
    n         number of cases; seed makes them deterministic.
    timeout   time budget per case, in seconds. Exceeding it is a "hang".
    allowed   exception classes that count as a clean rejection.
    Raises AssertionError listing (up to ``max_report``) failing cases;
    otherwise returns a ``FuzzReport``.
    """
    allowed = tuple(allowed)
    rep = FuzzReport()
    for idx, desc, buf in mutations(data, n, seed):
        rep.cases += 1
        status, value, secs = _run_with_deadline(read_fn, buf, timeout)
        rep.slowest = max(rep.slowest, secs)
        if status == "ok":
            rep.returned += 1
        elif status == "hang":
            rep.failures.append((idx, desc, f"no result within {timeout}s (hang)"))
        elif isinstance(value, allowed) and str(value):
            rep.rejected += 1
        elif isinstance(value, allowed):
            rep.failures.append((idx, desc, f"{type(value).__name__} with empty message"))
        else:
            rep.failures.append((idx, desc, f"{type(value).__name__}: {value}"))
    if rep.failures:
        name = getattr(read_fn, "__qualname__", repr(read_fn))
        lines = [f"{name}: {len(rep.failures)}/{rep.cases} fuzz cases not clean "
                 f"(seed={seed}, len={len(data)}, allowed="
                 f"{', '.join(c.__name__ for c in allowed)}):"]
        for idx, desc, problem in rep.failures[:max_report]:
            lines.append(f"  case {idx} [{desc}]: {problem}")
        if len(rep.failures) > max_report:
            lines.append(f"  ... {len(rep.failures) - max_report} more")
        lines.append("  reproduce: dict((i, b) for i, _, b in fuzz.mutations(data, n, seed))[case]")
        raise AssertionError("\n".join(lines))
    return rep

"""Pure-Python LZO1X codec (the format of compressed ZAL entries; DM-010 decoder, DM-023 encoder).

``decompress`` implements the LZO1X decoding rules (as in lzo1x_d.ch / the Linux kernel's
lzo1x_decompress_safe), with bounds checks on every read and write. Raises LzoError on malformed input.

``compress`` is a greedy LZO1X-1 style encoder (single 3-byte hash table, last position wins). It only
emits the instruction forms the reference LZO1X-1 compressor emits (first-literal byte 17+n, literal
runs, M2/M3/M4 matches, trailing literals in the low two bits, end marker 11 00 00), so any standard
LZO1X decoder (including NgBMX.exe 0x0049C0AC) accepts it. Its output is not byte-identical to the
original packer's.
"""


from .errors import FormatError


MAX_OUTPUT = 64 << 20   # cap when the caller gives no expected size


class LzoError(FormatError):
    """Malformed LZO1X stream."""

    def __init__(self, msg, *, offset=None):
        super().__init__(msg, fmt="LZO1X", offset=offset)


def decompress(src, out_len=None):
    """Decompress one LZO1X stream. If out_len is given the result must have exactly that size."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    ip = 0

    def need(k):
        if ip + k > n:
            raise LzoError(f"input overrun: need {k} bytes, {n - ip} left", offset=ip)

    limit = out_len if out_len is not None else MAX_OUTPUT

    def copy_match(dist, length):
        start = len(out) - dist
        if start < 0:
            raise LzoError(f"lookbehind overrun: dist {dist} > {len(out)}", offset=ip)
        if len(out) + length > limit:
            raise LzoError(f"output overrun: {len(out) + length} > {limit}", offset=ip)
        if dist >= length:
            out.extend(out[start:start + length])
        else:
            # overlapping copy == repeating the last `dist` bytes
            pat = bytes(out[start:])
            reps, rem = divmod(length, dist)
            out.extend(pat * reps + pat[:rem])

    def copy_lit(k):
        nonlocal ip
        need(k)
        if len(out) + k > limit:
            raise LzoError(f"output overrun: {len(out) + k} > {limit}", offset=ip)
        out.extend(src[ip:ip + k])
        ip += k

    def run_len(base, mask_val):
        # zero-byte extension for long lengths
        nonlocal ip
        if mask_val:
            return mask_val
        extra = 0
        while True:
            need(1)
            b = src[ip]; ip += 1
            if b:
                return extra + base + b
            extra += 255

    if n == 0:
        raise LzoError("empty input", offset=0)
    state = 0  # number of literals copied by the previous instruction (0, 1-3, or 4 = "4+")
    t = src[0]
    if t > 17:
        ip = 1
        t -= 17
        copy_lit(t)
        state = 4 if t >= 4 else t
    while True:
        need(1)
        t = src[ip]; ip += 1
        if t < 16:
            if state == 0:
                # literal run
                length = run_len(15, t) + 3 if t == 0 else t + 3
                copy_lit(length)
                state = 4
                continue
            elif state == 4:
                # 3-byte match, distance 2049..3072
                need(1)
                dist = (t >> 2) + (src[ip] << 2) + 2049; ip += 1
                copy_match(dist, 3)
                lit = t & 3
            else:
                need(1)
                dist = (t >> 2) + (src[ip] << 2) + 1; ip += 1
                copy_match(dist, 2)
                lit = t & 3
        elif t >= 64:
            need(1)
            dist = ((t >> 2) & 7) + (src[ip] << 3) + 1; ip += 1
            copy_match(dist, (t >> 5) - 1 + 2)
            lit = t & 3
        elif t >= 32:
            length = run_len(31, t & 31) + 2 if (t & 31) == 0 else (t & 31) + 2
            need(2)
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            copy_match((v >> 2) + 1, length)
            lit = v & 3
        else:  # 16..31
            length = run_len(7, t & 7) + 2 if (t & 7) == 0 else (t & 7) + 2
            need(2)
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            dist = ((t & 8) << 11) + (v >> 2)
            if dist == 0:
                # end of stream
                if length != 3:
                    raise LzoError("bad end marker", offset=ip)
                break
            copy_match(dist + 16384, length)
            lit = v & 3
        copy_lit(lit)
        state = lit
    if out_len is not None and len(out) != out_len:
        raise LzoError(f"size mismatch: got {len(out)}, expected {out_len}", offset=ip)
    if ip != n:
        raise LzoError(f"trailing input: {n - ip} bytes", offset=ip)
    return bytes(out)


# --------------------------------------------------------------------------- encoder (DM-023)
M2_MAX_LEN, M2_MAX_OFFSET = 8, 0x0800
M3_MAX_LEN, M3_MAX_OFFSET = 33, 0x4000
M4_MAX_LEN, M4_MAX_OFFSET = 9, 0xBFFF
MIN_MATCH = 3
MAX_MATCH = 0xFFFF          # keeps the zero-extension runs short; any length is legal
CHAIN_DEPTH = 16


def _emit_literals(out, src, start, end):
    t = end - start
    if t == 0:
        return
    if not out and t <= 238:
        out.append(17 + t)
    elif t <= 3:
        out[-2] |= t                      # trailing-literal bits of the previous instruction
    elif t <= 18:
        out.append(t - 3)
    else:
        tt = t - 18
        out.append(0)
        while tt > 255:
            out.append(0)
            tt -= 255
        out.append(tt)
    out += src[start:end]


def _emit_match(out, m_len, m_off):
    if m_len <= M2_MAX_LEN and m_off <= M2_MAX_OFFSET:
        m_off -= 1
        out.append(((m_len - 1) << 5) | ((m_off & 7) << 2))
        out.append(m_off >> 3)
        return
    if m_off <= M3_MAX_OFFSET:
        m_off -= 1
        if m_len <= M3_MAX_LEN:
            out.append(32 | (m_len - 2))
        else:
            m_len -= M3_MAX_LEN
            out.append(32)
            while m_len > 255:
                out.append(0)
                m_len -= 255
            out.append(m_len)
    else:
        m_off -= 0x4000
        hi = (m_off >> 11) & 8
        if m_len <= M4_MAX_LEN:
            out.append(16 | hi | (m_len - 2))
        else:
            m_len -= M4_MAX_LEN
            out.append(16 | hi)
            while m_len > 255:
                out.append(0)
                m_len -= 255
            out.append(m_len)
    m_off &= 0x3FFF                       # M4: bit 14 went into the opcode above
    out.append((m_off << 2) & 0xFF)
    out.append(m_off >> 6)


def _match_len(src, i, cand, lim):
    j, k = i + 3, cand + 3
    step = 64
    while j + step <= lim and src[j:j + step] == src[k:k + step]:
        j += step
        k += step
    while j < lim and src[j] == src[k]:
        j += 1
        k += 1
    return j - i


def compress(src, level=2):
    """Compress ``src`` into one LZO1X stream that ``decompress(result, len(src)) == src``.

    level 1: greedy, one candidate per 3-byte hash (fast, LZO1X-1 like).
    level 2 (default): greedy over a hash chain of up to CHAIN_DEPTH recent candidates, every position
             indexed; slower but within a few percent of the game's original packer on the corpus.
    """
    src = bytes(src)
    n = len(src)
    out = bytearray()
    chains = {}
    depth = 1 if level <= 1 else CHAIN_DEPTH
    lit = 0          # start of the pending literal run
    i = 0
    end = n - MIN_MATCH
    while i <= end:
        key = src[i:i + 3]
        cands = chains.get(key)
        best_len = best_off = 0
        if cands:
            lim = min(n, i + MAX_MATCH)
            for cand in reversed(cands[-depth:]):
                off = i - cand
                if off > M4_MAX_OFFSET:
                    break
                ml = _match_len(src, i, cand, lim)
                if ml > best_len:
                    best_len, best_off = ml, off
                    if ml >= lim - i:
                        break
            # a 3-byte match beyond M2 range costs as much as the literals; skip it
            if best_len == 3 and best_off > M2_MAX_OFFSET:
                best_len = 0
        if cands is None:
            chains[key] = [i]
        else:
            cands.append(i)
            if len(cands) > 4 * CHAIN_DEPTH:
                del cands[:-CHAIN_DEPTH]
        if best_len:
            _emit_literals(out, src, lit, i)
            _emit_match(out, best_len, best_off)
            stop = min(i + best_len, end + 1)
            step = 1 if level > 1 else 4
            for q in range(i + 1, stop, step):
                kq = src[q:q + 3]
                c = chains.get(kq)
                if c is None:
                    chains[kq] = [q]
                else:
                    c.append(q)
                    if len(c) > 4 * CHAIN_DEPTH:
                        del c[:-CHAIN_DEPTH]
            i += best_len
            lit = i
        else:
            i += 1
    _emit_literals(out, src, lit, n)
    out += b"\x11\x00\x00"
    return bytes(out)

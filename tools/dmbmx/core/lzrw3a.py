"""LZRW3-A (Ross Williams, 1991): ZAL compression method 1.

The exe's codec 1 is LZRW3-A: decoder ``zcompresDecompressLZRW3A`` 0x0049BD10, encoder 0x0049B8E0,
work buffer 0x4010 bytes = 4096 table pointers (+16 alignment slack). Evidence [V], from the decoder:
- the table is initialised to point at the 18-byte string "123456789012345678" (0x00562C04);
- a 16-bit control word, OR'ed with 0x10000, governs each group of 16 items (bit set = copy);
- copy items are 2 bytes: a 12-bit table index ((b0 & 0xF0) << 4 | b1) and length (b0 & 0xF) + 3;
- the table is 512 partitions x 8 slots, filled round-robin by a 3-bit cycle counter;
- HASH(p) = ((40543 * ((p0 << 8) ^ (p1 << 4) ^ p2)) >> 4 & 0x1FF) << 3. The exe computes
  -417*x (0x0049BEED..0x0049BF06), which equals 40543*x modulo 2^13: the only bits kept;
- a 4-byte flag header: first byte 1 = stored (FLAG_COPY), 0 = compressed.

``decompress`` is a transcription of the exe's decoder (bounds-checked). ``compress`` is a greedy
encoder that keeps the same table as the decoder, so ``decompress(compress(x)) == x`` for any x.
The exact bytes differ from the original packer's, which the decoder accepts.
"""
from dmbmx.core.errors import FormatError

START = b"123456789012345678"
FLAG_COMPRESS, FLAG_COPY = 0, 1
FLAG_BYTES = 4
TABLE_LEN, DEPTH = 4096, 8
MIN_LEN, MAX_LEN = 3, 18
FMT = "LZRW3A"


def _hash(b, i):
    return ((40543 * ((b[i] << 8) ^ (b[i + 1] << 4) ^ b[i + 2])) >> 4 & 0x1FF) << 3


def decompress(src, expected=None):
    """Decode one method-1 stream. ``expected``: optional size check (FormatError on mismatch)."""
    src = bytes(src)
    if len(src) < FLAG_BYTES:
        raise FormatError("stream shorter than the 4-byte flag header", fmt=FMT, offset=0)
    if src[0] == FLAG_COPY:
        out = src[FLAG_BYTES:]
    else:
        out = bytearray()
        table = [-1] * TABLE_LEN          # -1 = the START string, else an index into ``out``
        cycle = literals = 0
        control = 1
        p, end = FLAG_BYTES, len(src)
        while p != end:
            if control == 1:
                if p + 2 > end:
                    raise FormatError("truncated control word", fmt=FMT, offset=p)
                control = 0x10000 | src[p] | src[p + 1] << 8
                p += 2
                continue
            if control & 1:
                if p + 2 > end:
                    raise FormatError("truncated copy item", fmt=FMT, offset=p)
                b0, b1 = src[p], src[p + 1]
                p += 2
                index = (b0 & 0xF0) << 4 | b1
                n = (b0 & 0xF) + MIN_LEN
                q = table[index]
                d = len(out)
                if q < 0:
                    out += START[:n]
                else:
                    for k in range(n):            # byte by byte: the source may overlap the output
                        out.append(out[q + k])
                if literals:
                    r = d - literals
                    table[_hash(out, r) + cycle] = r
                    cycle = (cycle + 1) & 7
                    if literals == 2:
                        table[_hash(out, r + 1) + cycle] = r + 1
                        cycle = (cycle + 1) & 7
                    literals = 0
                table[(index & 0xFF8) + cycle] = d
                cycle = (cycle + 1) & 7
            else:
                out.append(src[p])
                p += 1
                literals += 1
                if literals == 3:
                    r = len(out) - 3
                    table[_hash(out, r) + cycle] = r
                    cycle = (cycle + 1) & 7
                    literals = 2
            control >>= 1
        out = bytes(out)
    if expected is not None and len(out) != expected:
        raise FormatError(f"decoded {len(out)} bytes, expected {expected}", fmt=FMT)
    return bytes(out)


def compress(data):
    """Greedy LZRW3-A encoder (decoder-exact table maintenance)."""
    data = bytes(data)
    n = len(data)
    table = [-1] * TABLE_LEN
    cycle = literals = 0
    out = bytearray([FLAG_COMPRESS] * FLAG_BYTES)
    ctrl_pos, ctrl, nitems = None, 0, 16
    i = 0

    def src_byte(q, k):
        return START[k] if q < 0 else data[q + k]

    while i < n:
        if nitems == 16:
            if ctrl_pos is not None:
                out[ctrl_pos:ctrl_pos + 2] = (ctrl & 0xFFFF).to_bytes(2, "little")
            ctrl_pos, ctrl, nitems = len(out), 0, 0
            out += b"\0\0"
        best_len, best_slot = 0, 0
        if i + MIN_LEN <= n:
            h = _hash(data, i)
            limit = min(MAX_LEN, n - i)
            for s in range(DEPTH):
                q = table[h + s]
                k = 0
                while k < limit and src_byte(q, k) == data[i + k]:
                    k += 1
                if k > best_len:
                    best_len, best_slot = k, s
        if best_len >= MIN_LEN:
            index = h + best_slot
            out += bytes(((index >> 4) & 0xF0 | (best_len - MIN_LEN), index & 0xFF))
            ctrl |= 1 << nitems
            if literals:
                r = i - literals
                table[_hash(data, r) + cycle] = r
                cycle = (cycle + 1) & 7
                if literals == 2:
                    table[_hash(data, r + 1) + cycle] = r + 1
                    cycle = (cycle + 1) & 7
                literals = 0
            table[h + cycle] = i
            cycle = (cycle + 1) & 7
            i += best_len
        else:
            out.append(data[i])
            i += 1
            literals += 1
            if literals == 3:
                r = i - 3
                table[_hash(data, r) + cycle] = r
                cycle = (cycle + 1) & 7
                literals = 2
        nitems += 1
    if ctrl_pos is not None:
        out[ctrl_pos:ctrl_pos + 2] = (ctrl & 0xFFFF).to_bytes(2, "little")
    if len(out) > n + FLAG_BYTES:
        return bytes([FLAG_COPY, 0, 0, 0]) + data
    return bytes(out)

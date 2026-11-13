/*
 * NgEngine/Zge/zcompres.c
 * Original: C:\NgEngine\Zge\zcompres.c (no path string in the exe: the module has no asserts;
 * name from the "ZCOMPRES:" message prefix and link order between ziff.c and zrwModelCreate.c)
 * 5 functions, 3017 bytes, VA 0x0049b8e0-0x0049c5f3.
 * Draft. Non-matching C (AD-8).
 *
 * Method 1 is LZRW3-A by Ross N. Williams (1991), [V] identified from the decoder: the
 * "123456789012345678" table initialisation, 16-item control words with a 0x10000 sentinel,
 * 2-byte copy items (12-bit table index + 4-bit length-3), 512 partitions x 8 slots with a
 * round-robin cycle, HASH constant 40543 (the compiler emits -417*x; equal modulo 2^13, the only
 * bits kept) and the 4-byte FLAG_COMPRESS/FLAG_COPY header. Method 2 is LZO1X (decoder inlined).
 * Python references: tools/dmbmx/core/lzrw3a.py (transcribed from this file's decoder), tools/dmbmx/core/lzo.py.
 */
#include "decomp.h"

/* other modules: zfile.h/zengine.h via decomp.h; the rest until they are decompiled */
void *memcpy(void *dst, const void *src, size_t n);

static const uint8_t s_lzrw3aStart[] = "123456789012345678";   /* 0x00562C04 */

static const uint8_t **lzrw3aTable(void *work)
{
    return (const uint8_t **)(((uint32_t)work + 3) & ~3u);
}

static void lzrw3aInitTable(const uint8_t **table)
{
    int i;
    for (i = 0; i < LZRW3A_TABLE_LEN; i++)
        table[i] = s_lzrw3aStart;
}

/* VA 0x0049b8e0, 1070 bytes, [I] range, status draft */
void zcompresCompressLZRW3A(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    /*
     * Ross Williams' LZRW3-A compressor. Literal positions are entered into the table two items
     * late (pending1/pending2), exactly mirroring the decoder's "literals" counter, so both sides
     * always hold identical tables. The last 18 bytes are emitted as literals (no room for a match).
     */
    const uint8_t **table = lzrw3aTable(work);
    const uint8_t *p = src;
    const uint8_t *srcEnd = src + srcLen;
    const uint8_t *srcMax = srcEnd - 18;            /* last position where a full match fits */
    uint8_t *out = dst + LZRW3A_FLAG_BYTES;
    uint8_t *outMax = dst + srcLen;                 /* beyond this, store the data instead */
    uint8_t *control;
    uint32_t ctrl = 0, nbits = 0;                   /* control bits of the current group */
    const uint8_t **pending1 = 0;                   /* partition of the previous literal */
    const uint8_t **pending2 = 0;                   /* partition of the literal before it */
    uint32_t cycle = 0;

    dst[0] = LZRW3A_FLAG_COMPRESS;
    dst[1] = dst[2] = dst[3] = 0;
    lzrw3aInitTable(table);
    control = out;
    out += 2;

    while (p < srcEnd) {
        uint32_t bestLen = 0, bestSlot = 0;
        const uint8_t **part = 0;

        if (out > outMax) {                         /* expanding: emit a stored stream */
            dst[0] = LZRW3A_FLAG_COPY;
            memcpy(dst + LZRW3A_FLAG_BYTES, src, srcLen);
            *dstLen = srcLen + LZRW3A_FLAG_BYTES;
            return;
        }
        if (p <= srcMax) {                          /* longest match in the partition (3..18) */
            uint32_t slot;
            part = table + LZRW3A_HASH(p);
            for (slot = 0; slot < 8 && bestLen < 18; slot++) {
                /* (the exe keeps comparing after an 18-byte match and so reads p[18], one byte
                 * past the input in the worst case; stopping early selects the same slot) */
                const uint8_t *q = part[slot];
                uint32_t n;
                if (p[bestLen] != q[bestLen])       /* cannot beat the best: skip */
                    continue;
                for (n = 0; n < 18 && p[n] == q[n]; n++)
                    ;
                if (n > bestLen) {
                    bestLen = n;
                    bestSlot = slot;
                }
            }
        }
        if (bestLen >= 3) {                         /* copy item */
            uint32_t index = (uint32_t)(part - table) + bestSlot;
            out[0] = (uint8_t)(((index >> 4) & 0xF0) | (bestLen - 3));
            out[1] = (uint8_t)index;
            out += 2;
            ctrl |= 1u << nbits;
            if (pending1) {                         /* flush the pending literal inserts */
                if (pending2) {
                    pending2[cycle] = p - 2;
                    cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
                    pending2 = 0;
                }
                pending1[cycle] = p - 1;
                cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
                pending1 = 0;
            }
            part[cycle] = p;
            cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
            p += bestLen;
        } else {                                    /* literal item */
            *out++ = *p;
            if (pending2) {                         /* the literal two items back enters now */
                pending2[cycle] = p - 2;
                cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
            }
            pending2 = pending1;
            pending1 = part;                        /* NULL in the tail: no insert there */
            p++;
        }
        if (++nbits == 16) {                        /* group complete: store its control word */
            control[0] = (uint8_t)ctrl;
            control[1] = (uint8_t)(ctrl >> 8);
            control = out;
            out += 2;
            ctrl = nbits = 0;
        }
    }
    control[0] = (uint8_t)ctrl;                     /* last (partial) group */
    control[1] = (uint8_t)(ctrl >> 8);
    if (nbits == 0)                                 /* empty group: drop its control word */
        out -= 2;
    *dstLen = (uint32_t)(out - dst);
    /* NOTE(decomp): the exe keeps the control bits in one 32-bit word whose top half counts the
     * free slots (starts at 0xFFFF0000, shifted right per item; copy items set bit 15) and unrolls
     * 16 items while 16*18 input bytes remain. The output bytes are the same. */
}

/* VA 0x0049bd10, 781 bytes, [I] range, status draft */
void zcompresDecompressLZRW3A(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    const uint8_t **table = lzrw3aTable(work);
    const uint8_t *p = src + LZRW3A_FLAG_BYTES;
    const uint8_t *srcEnd = src + srcLen;
    uint8_t *out = dst;
    uint32_t control = 1;          /* 1 = no bits left: read a control word */
    uint32_t cycle = 0;
    uint32_t literals = 0;         /* literals since the last table insert (0..2) */

    if (src[0] == LZRW3A_FLAG_COPY) {
        memcpy(dst, p, srcLen - LZRW3A_FLAG_BYTES);
        *dstLen = srcLen - LZRW3A_FLAG_BYTES;
        return;
    }
    lzrw3aInitTable(table);

    while (p != srcEnd) {
        int unroll;
        if (control == 1) {
            control = 0x10000u | p[0] | ((uint32_t)p[1] << 8);
            p += 2;
        }
        unroll = (p <= srcEnd - 32) ? 16 : 1;
        while (unroll--) {
            if (control & 1) {                                  /* copy item */
                uint32_t index = ((uint32_t)(p[0] & 0xF0) << 4) | p[1];
                uint32_t len = (p[0] & 0x0F) + 3;
                const uint8_t *from = table[index];
                uint8_t *start = out;
                p += 2;
                while (len--)                                   /* may overlap: byte by byte */
                    *out++ = *from++;
                if (literals) {                                 /* insert the pending literals */
                    const uint8_t *r = start - literals;
                    table[LZRW3A_HASH(r) + cycle] = r;
                    cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
                    if (literals == 2) {
                        r++;
                        table[LZRW3A_HASH(r) + cycle] = r;
                        cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
                    }
                    literals = 0;
                }
                table[(index & ~(uint32_t)LZRW3A_DEPTH_MASK) + cycle] = start;
                cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
            } else {                                            /* literal item */
                *out++ = *p++;
                if (++literals == 3) {
                    const uint8_t *r = out - 3;
                    table[LZRW3A_HASH(r) + cycle] = r;
                    cycle = (cycle + 1) & LZRW3A_DEPTH_MASK;
                    literals = 2;
                }
            }
            control >>= 1;
        }
    }
    *dstLen = (uint32_t)(out - dst);
}

/* VA 0x0049c020, 85 bytes, [I] range, status draft */
int zcompresCompress(int method, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    /* Methods 0 and 1 both mean LZRW3-A here; there is no LZO1X compressor in the exe. */
    if (method == 0 || method == ZCOMPRES_LZRW3A) {
        void *work = zmemAlloc(LZRW3A_WORK_SIZE);
        zcompresCompressLZRW3A(work, src, srcLen, dst, dstLen);
        zmemFree(work);
        return 1;
    }
    g_pfnDebugPrintf("ZCOMPRES: Unsupported compression method\n");
    return 0;
}

/* VA 0x0049c080, 950 bytes, [I] seed, status draft */
int zcompresDecompress(int method, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    enum { LZO_LITERAL_RUN, LZO_FIRST_LITERAL_RUN, LZO_TRAILING_LITERALS } state;
    const uint8_t *ip;
    uint8_t *op;
    uint32_t t;

    if (method == ZCOMPRES_LZRW3A) {
        void *work = zmemAlloc(LZRW3A_WORK_SIZE);
        zcompresDecompressLZRW3A(work, src, srcLen, dst, dstLen);
        zmemFree(work);
        return 0;
    }
    if (method != ZCOMPRES_LZO1X) {
        g_pfnDebugPrintf("ZCOMPRES: Unknown decompression method\n");
        return 1;
    }

    /*
     * LZO1X decompressor, inlined in the exe (0x0049C0AC..): the standard lzo1x_decompress control
     * flow written as a state loop. Like the original "fast" decoder it does no bounds checking;
     * the only input check is "Illegal starting byte" (a first literal run below 18).
     */
    ip = src;
    op = dst;
    *dstLen = 0;
    state = LZO_LITERAL_RUN;
    t = 0;

    if (*ip > 17) {                                 /* the stream starts with a literal run */
        t = *ip++ - 17;
        if (t < 4) {
            state = LZO_TRAILING_LITERALS;
        } else {
            while (t--)
                *op++ = *ip++;
            state = LZO_FIRST_LITERAL_RUN;
        }
    }

    for (;;) {
        const uint8_t *m;
        uint32_t len;

        if (state == LZO_TRAILING_LITERALS) {       /* 1..3 literals carried by the last match */
            while (t--)
                *op++ = *ip++;
            t = *ip++;                              /* a match must follow */
        } else {
            t = *ip++;
            if (t < 16) {
                if (state == LZO_LITERAL_RUN) {     /* literal run of t+3 bytes */
                    if (t == 0) {
                        t = 15;
                        while (*ip == 0) {
                            t += 255;
                            ip++;
                        }
                        t += *ip++;
                    }
                    t += 3;
                    while (t--)
                        *op++ = *ip++;
                    state = LZO_FIRST_LITERAL_RUN;
                    continue;
                }
                /* right after a literal run: a 3-byte match 0x801..0xC00 back */
                m = op - (1 + 0x800) - (t >> 2) - ((uint32_t)*ip++ << 2);
                *op++ = *m++;
                *op++ = *m++;
                *op++ = *m++;
                t = ip[-2] & 3;
                state = t ? LZO_TRAILING_LITERALS : LZO_LITERAL_RUN;
                continue;
            }
        }

        if (t >= 64) {                              /* M2: 3..8 bytes, distance 1..2048 */
            m = op - 1 - ((t >> 2) & 7) - ((uint32_t)*ip++ << 3);
            len = (t >> 5) + 1;
        } else if (t >= 32) {                       /* M3: distance 1..16384 */
            len = t & 31;
            if (len == 0) {
                len = 31;
                while (*ip == 0) {
                    len += 255;
                    ip++;
                }
                len += *ip++;
            }
            len += 2;
            m = op - 1 - ((ip[0] >> 2) + ((uint32_t)ip[1] << 6));
            ip += 2;
        } else if (t >= 16) {                       /* M4: distance 16385..49151, or end of stream */
            uint32_t dist = (t & 8) << 11;
            len = t & 7;
            if (len == 0) {
                len = 7;
                while (*ip == 0) {
                    len += 255;
                    ip++;
                }
                len += *ip++;
            }
            len += 2;
            dist += (ip[0] >> 2) + ((uint32_t)ip[1] << 6);
            ip += 2;
            if (dist == 0)                          /* end marker 0x11 0x00 0x00 */
                break;
            m = op - dist - 0x4000;
        } else {                                    /* M1 after a match: 2 bytes, distance 1..1024 */
            m = op - 1 - (t >> 2) - ((uint32_t)*ip++ << 2);
            len = 2;
        }
        while (len--)
            *op++ = *m++;
        t = ip[-2] & 3;                             /* literals packed into the match */
        state = t ? LZO_TRAILING_LITERALS : LZO_LITERAL_RUN;
    }
    *dstLen = (uint32_t)(op - dst);
    return 0;
    /* NOTE(decomp): "Illegal starting byte" (0x00562C84) / "Decompression failed!" are printed by the
     * inlined decoder for a first byte of 1..17 [I]; not modelled above. */
}

/* VA 0x0049b880, 34 bytes, [I] new function, status draft (unreferenced wrapper; it sits just
 * before 0x0049B8E0, between zrwShadow.c and this file's first function) */
void zcompresLzrwCompress(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    zcompresCompressLZRW3A(work, src, srcLen, dst, dstLen);
}

/* VA 0x0049b8b0, 34 bytes, [I] new function, status draft (unreferenced wrapper) */
void zcompresLzrwDecompress(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen)
{
    zcompresDecompressLZRW3A(work, src, srcLen, dst, dstLen);
}

/*
 * zcompres.h - NgEngine compression (NgEngine/Zge/zcompres.c).
 *
 * Method 1 = LZRW3-A (Ross Williams, 1991) [V]; method 2 = LZO1X (decoder inlined in
 * zcompresDecompress 0x0049C080) [V]. The packer-side compressor only supports method 1
 * ("ZCOMPRES: Unsupported compression method"). Python equivalents: dmbmx.core.lzrw3a, dmbmx.core.lzo.
 */
#ifndef ZCOMPRES_H
#define ZCOMPRES_H
#include "dmtypes.h"

#define ZCOMPRES_LZRW3A       1
#define ZCOMPRES_LZO1X        2

#define LZRW3A_FLAG_BYTES     4          /* first byte: 0 = compressed, 1 = stored */
#define LZRW3A_FLAG_COMPRESS  0
#define LZRW3A_FLAG_COPY      1
#define LZRW3A_TABLE_LEN      4096       /* 512 partitions x 8 slots */
#define LZRW3A_DEPTH_MASK     7
#define LZRW3A_PARTITION_MASK 0x1FF
#define LZRW3A_WORK_SIZE      0x4010     /* table + alignment slack, zmemAlloc'd per call */

/* hash of the 3 bytes at p: partition base (multiple of 8) */
#define LZRW3A_HASH(p) \
    ((((40543u * ((((uint32_t)(p)[0]) << 8) ^ (((uint32_t)(p)[1]) << 4) ^ (uint32_t)(p)[2])) >> 4) \
      & LZRW3A_PARTITION_MASK) << 3)

/* GHIDRA-SKIP-BEGIN */
int  zcompresCompress(int method, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen);
int  zcompresDecompress(int method, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen);
void zcompresCompressLZRW3A(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen);
void zcompresDecompressLZRW3A(void *work, const uint8_t *src, uint32_t srcLen, uint8_t *dst, uint32_t *dstLen);
/* GHIDRA-SKIP-END */

#endif /* ZCOMPRES_H */

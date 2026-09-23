/*
 * Copyright (c) 2026, the uf-lzma2 authors
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

#include <string.h>
#include "uf2_xz.h"

const BYTE XZ_magic[XZ_MAGIC_SIZE] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };

int XZ_isXz(const void *src, size_t srcSize)
{
    return srcSize >= XZ_MAGIC_SIZE && memcmp(src, XZ_magic, XZ_MAGIC_SIZE) == 0;
}

/* ---------- CRC32 (IEEE 802.3) and CRC64 (ECMA-182), reflected, table driven ---------- */

#include "uf2_xz_tables.h"

/* Slicing-by-16: sixteen input bytes per step, looked up in sixteen tables at
 * once. The byte-at-a-time loop measured at about 22% of LZMA2 decode time with a
 * check enabled, slicing-by-8 at about 6%. Words are read little endian by
 * MEM_readLE32/64, so the loop is correct on either byte order. */
U32 XZ_crc32(U32 crc, const void *buf, size_t size)
{
    const BYTE *p = (const BYTE *)buf;
    crc = ~crc;
    for (; size >= 16; p += 16, size -= 16) {
        U32 const a = MEM_readLE32(p) ^ crc;
        U32 const b = MEM_readLE32(p + 4);
        U32 const c = MEM_readLE32(p + 8);
        U32 const d = MEM_readLE32(p + 12);
        crc = crc32Table[15][a & 0xFF] ^ crc32Table[14][(a >> 8) & 0xFF]
            ^ crc32Table[13][(a >> 16) & 0xFF] ^ crc32Table[12][a >> 24]
            ^ crc32Table[11][b & 0xFF] ^ crc32Table[10][(b >> 8) & 0xFF]
            ^ crc32Table[9][(b >> 16) & 0xFF] ^ crc32Table[8][b >> 24]
            ^ crc32Table[7][c & 0xFF] ^ crc32Table[6][(c >> 8) & 0xFF]
            ^ crc32Table[5][(c >> 16) & 0xFF] ^ crc32Table[4][c >> 24]
            ^ crc32Table[3][d & 0xFF] ^ crc32Table[2][(d >> 8) & 0xFF]
            ^ crc32Table[1][(d >> 16) & 0xFF] ^ crc32Table[0][d >> 24];
    }
    while (size--)
        crc = crc32Table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

U64 XZ_crc64(U64 crc, const void *buf, size_t size)
{
    const BYTE *p = (const BYTE *)buf;
    crc = ~crc;
    for (; size >= 16; p += 16, size -= 16) {
        U64 const x = MEM_readLE64(p) ^ crc;
        U64 const y = MEM_readLE64(p + 8);
        crc = crc64Table[15][x & 0xFF] ^ crc64Table[14][(x >> 8) & 0xFF]
            ^ crc64Table[13][(x >> 16) & 0xFF] ^ crc64Table[12][(x >> 24) & 0xFF]
            ^ crc64Table[11][(x >> 32) & 0xFF] ^ crc64Table[10][(x >> 40) & 0xFF]
            ^ crc64Table[9][(x >> 48) & 0xFF] ^ crc64Table[8][x >> 56]
            ^ crc64Table[7][y & 0xFF] ^ crc64Table[6][(y >> 8) & 0xFF]
            ^ crc64Table[5][(y >> 16) & 0xFF] ^ crc64Table[4][(y >> 24) & 0xFF]
            ^ crc64Table[3][(y >> 32) & 0xFF] ^ crc64Table[2][(y >> 40) & 0xFF]
            ^ crc64Table[1][(y >> 48) & 0xFF] ^ crc64Table[0][y >> 56];
    }
    while (size--)
        crc = crc64Table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int XZ_checkSize(unsigned check)
{
    if (check > 15)
        return -1;
    if (check == 0)
        return 0;
    /* The specification fixes the sizes by group: 1-3: 4 bytes, 4-6: 8, 7-9: 16,
     * 10-12: 32, 13-15: 64, whether or not a type in the group is defined. */
    return 4 << ((check - 1) / 3);
}

/* ---------- variable length integers ---------- */

size_t XZ_vliEncode(U64 value, BYTE *out)
{
    size_t n = 0;
    while (value >= 0x80) {
        out[n++] = (BYTE)(value | 0x80);
        value >>= 7;
    }
    out[n++] = (BYTE)value;
    return n;
}

size_t XZ_vliDecode(const BYTE *in, size_t size, U64 *value)
{
    U64 v = 0;
    for (size_t n = 0; n < XZ_VLI_BYTES_MAX && n < size; ++n) {
        BYTE const b = in[n];
        v |= (U64)(b & 0x7F) << (7 * n);
        if ((b & 0x80) == 0) {
            /* a multi-byte VLI may not end in a zero byte: that encoding is not minimal */
            if (n > 0 && b == 0)
                return 0;
            *value = v;
            return n + 1;
        }
    }
    return 0;
}

/* ---------- writers ---------- */

void XZ_writeStreamHeader(BYTE *out, unsigned check)
{
    memcpy(out, XZ_magic, XZ_MAGIC_SIZE);
    out[6] = 0;
    out[7] = (BYTE)check;
    MEM_writeLE32(out + 8, XZ_crc32(0, out + 6, 2));
}

size_t XZ_writeBlockHeader(BYTE *out, U64 compressedSize, U64 uncompressedSize, BYTE dictProp)
{
    size_t n = 2;
    /* one filter; Compressed Size and Uncompressed Size both present */
    out[1] = 0x00 | 0x40 | 0x80;
    n += XZ_vliEncode(compressedSize, out + n);
    n += XZ_vliEncode(uncompressedSize, out + n);
    out[n++] = XZ_LZMA2_FILTER_ID;
    out[n++] = 1;           /* size of the filter properties */
    out[n++] = dictProp;
    while ((n + 4) & 3)
        out[n++] = 0;       /* Header Padding */
    out[0] = (BYTE)((n + 4) / 4 - 1);
    MEM_writeLE32(out + n, XZ_crc32(0, out, n));
    return n + 4;
}

size_t XZ_writeIndex(BYTE *out, const U64 *unpaddedSizes, const U64 *uncompressedSizes, size_t count)
{
    size_t n = 0;
    out[n++] = 0x00;        /* Index Indicator */
    n += XZ_vliEncode(count, out + n);
    for (size_t i = 0; i < count; ++i) {
        n += XZ_vliEncode(unpaddedSizes[i], out + n);
        n += XZ_vliEncode(uncompressedSizes[i], out + n);
    }
    while (n & 3)
        out[n++] = 0;       /* Index Padding */
    MEM_writeLE32(out + n, XZ_crc32(0, out, n));
    return n + 4;
}

void XZ_writeStreamFooter(BYTE *out, size_t indexSize, unsigned check)
{
    MEM_writeLE32(out + 4, (U32)(indexSize / 4 - 1));   /* Backward Size */
    out[8] = 0;
    out[9] = (BYTE)check;
    MEM_writeLE32(out, XZ_crc32(0, out + 4, 6));
    out[10] = 'Y';
    out[11] = 'Z';
}

/*
 * Copyright (c) 2026, the uf-lzma2 authors
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/* The .xz container, as specified in "The .xz File Format", version 1.2.1.
 * Only the pieces this library needs: a single LZMA2 filter, and the CRC32 and
 * CRC64 integrity checks. Everything here is framing; the LZMA2 data inside a
 * block is produced and consumed by the library's ordinary encoder and decoder. */

#ifndef UF2_XZ_H
#define UF2_XZ_H

#include "mem.h"

#if defined (__cplusplus)
extern "C" {
#endif

#define XZ_MAGIC_SIZE        6
#define XZ_STREAM_HEADER_SIZE 12
#define XZ_STREAM_FOOTER_SIZE 12
#define XZ_BLOCK_HEADER_MAX  28   /* size byte, flags, two 9-byte VLIs, one filter, padding, CRC32 */
#define XZ_INDEX_MAX_1       24   /* index holding a single record, with padding and CRC32 */
#define XZ_LZMA2_FILTER_ID   0x21
#define XZ_VLI_BYTES_MAX     9

/* check types, numbered as in the specification */
#define XZ_CHECK_NONE   0
#define XZ_CHECK_CRC32  1
#define XZ_CHECK_CRC64  4
#define XZ_CHECK_SHA256 10

/* worst-case bytes a single-block .xz adds around the LZMA2 data */
#define XZ_OVERHEAD_MAX (XZ_STREAM_HEADER_SIZE + XZ_BLOCK_HEADER_MAX + 3 + 8 + XZ_INDEX_MAX_1 + XZ_STREAM_FOOTER_SIZE)

extern const BYTE XZ_magic[XZ_MAGIC_SIZE];

int XZ_isXz(const void *src, size_t srcSize);

U32 XZ_crc32(U32 crc, const void *buf, size_t size);
U64 XZ_crc64(U64 crc, const void *buf, size_t size);

/* Size in bytes of the check field for a check type, or -1 if the type is not
 * one the specification defines. */
int XZ_checkSize(unsigned check);

/* Encode a VLI; returns bytes written (1..9). */
size_t XZ_vliEncode(U64 value, BYTE *out);
/* Decode a VLI; returns bytes read, or 0 if malformed or it runs past `size`. */
size_t XZ_vliDecode(const BYTE *in, size_t size, U64 *value);

void XZ_writeStreamHeader(BYTE *out, unsigned check);
/* returns the Block Header size written, always a multiple of 4 */
size_t XZ_writeBlockHeader(BYTE *out, U64 compressedSize, U64 uncompressedSize, BYTE dictProp);
/* returns the index size written, a multiple of 4; `count` records */
size_t XZ_writeIndex(BYTE *out, const U64 *unpaddedSizes, const U64 *uncompressedSizes, size_t count);
void XZ_writeStreamFooter(BYTE *out, size_t indexSize, unsigned check);

#if defined (__cplusplus)
}
#endif

#endif /* UF2_XZ_H */

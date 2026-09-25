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

/* ---------- CRC32 (IEEE 802.3) and CRC64 (ECMA-182), reflected ---------- */

#include "uf2_xz_tables.h"

/* Slicing-by-16 on the raw register (no inversion): sixteen input bytes per step,
 * looked up in sixteen tables at once. The byte-at-a-time loop measured at about
 * 22% of LZMA2 decode time with a check enabled, slicing-by-8 at about 6%. Words
 * are read little endian by MEM_readLE32/64, so the loop is correct on either
 * byte order. */
static U32 XZ_crc32Tables(U32 crc, const BYTE *p, size_t size)
{
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
    return crc;
}

static U64 XZ_crc64Tables(U64 crc, const BYTE *p, size_t size)
{
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
    return crc;
}

U32 XZ_crc32Portable(U32 crc, const void *buf, size_t size)
{
    return ~XZ_crc32Tables(~crc, (const BYTE *)buf, size);
}

U64 XZ_crc64Portable(U64 crc, const void *buf, size_t size)
{
    return ~XZ_crc64Tables(~crc, (const BYTE *)buf, size);
}

/* ---- folding with a carry-less multiply ----
 * Where the processor has one (PCLMULQDQ on x86, PMULL on ARM64), both CRCs fold
 * the data 64 bytes at a time into a 128-bit state instead of looking it up in
 * tables; tools/gen_xz_tables.py explains the arithmetic and checks it. The state
 * is congruent to the data folded, so it is itself a 16-byte message with the
 * same CRC, and the table code finishes it and the bytes that remain. The one
 * difference between the two CRCs is the constants. XZ_NO_HW_CRC builds without. */

#define XZ_FOLD_MIN 64      /* shorter input stays on the tables */

#if !defined(XZ_NO_HW_CRC) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) \
    && (defined(__GNUC__) || defined(_MSC_VER))
#  define XZ_HW_CRC 1
#  include <emmintrin.h>
#  include <wmmintrin.h>
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <intrin.h>
#    define XZ_TARGET
#  else
#    include <cpuid.h>
#    define XZ_TARGET __attribute__((target("pclmul,sse2")))
#  endif

static int XZ_hwDetect(void)
{
    unsigned regs[4] = { 0, 0, 0, 0 };
#  if defined(_MSC_VER) && !defined(__clang__)
    int info[4];
    __cpuid(info, 1);
    regs[2] = (unsigned)info[2];
    regs[3] = (unsigned)info[3];
#  else
    if (!__get_cpuid(1, &regs[0], &regs[1], &regs[2], &regs[3]))
        return 0;
#  endif
    return (regs[2] & (1U << 1)) && (regs[3] & (1U << 26));    /* PCLMULQDQ, SSE2 */
}

XZ_TARGET
static __m128i XZ_fold(__m128i x, __m128i k)
{
    /* k holds the constant for the high-degree half in its low lane */
    return _mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x00), _mm_clmulepi64_si128(x, k, 0x11));
}

/* Fold p[0..size) with the register reg applied to its first bytes, size >= 64.
 * Writes the state to state[16] and returns the bytes folded, a multiple of 16. */
XZ_TARGET
static size_t XZ_foldHw(U64 reg, const BYTE *p, size_t size, const U64 K[4], BYTE state[16])
{
    U64 const init[2] = { reg, 0 };
    U64 const k16[2] = { K[1], K[0] };
    U64 const k64[2] = { K[3], K[2] };
    __m128i const fold16 = _mm_loadu_si128((const __m128i *)k16);
    __m128i const fold64 = _mm_loadu_si128((const __m128i *)k64);
    const BYTE *const start = p;

    __m128i x0 = _mm_xor_si128(_mm_loadu_si128((const __m128i *)p), _mm_loadu_si128((const __m128i *)init));
    __m128i x1 = _mm_loadu_si128((const __m128i *)(p + 16));
    __m128i x2 = _mm_loadu_si128((const __m128i *)(p + 32));
    __m128i x3 = _mm_loadu_si128((const __m128i *)(p + 48));
    p += 64;
    size -= 64;
    /* four independent lanes, so that the multiplies overlap */
    for (; size >= 64; p += 64, size -= 64) {
        x0 = _mm_xor_si128(XZ_fold(x0, fold64), _mm_loadu_si128((const __m128i *)p));
        x1 = _mm_xor_si128(XZ_fold(x1, fold64), _mm_loadu_si128((const __m128i *)(p + 16)));
        x2 = _mm_xor_si128(XZ_fold(x2, fold64), _mm_loadu_si128((const __m128i *)(p + 32)));
        x3 = _mm_xor_si128(XZ_fold(x3, fold64), _mm_loadu_si128((const __m128i *)(p + 48)));
    }
    x1 = _mm_xor_si128(XZ_fold(x0, fold16), x1);
    x2 = _mm_xor_si128(XZ_fold(x1, fold16), x2);
    x3 = _mm_xor_si128(XZ_fold(x2, fold16), x3);
    for (; size >= 16; p += 16, size -= 16)
        x3 = _mm_xor_si128(XZ_fold(x3, fold16), _mm_loadu_si128((const __m128i *)p));
    _mm_storeu_si128((__m128i *)state, x3);
    return (size_t)(p - start);
}

#elif !defined(XZ_NO_HW_CRC) && (defined(__aarch64__) || defined(_M_ARM64)) && defined(__GNUC__) \
    && (defined(__linux__) || defined(__APPLE__))
#  define XZ_HW_CRC 1
#  include <arm_neon.h>
#  if defined(__linux__)
#    include <sys/auxv.h>
#    ifndef HWCAP_PMULL
#      define HWCAP_PMULL (1 << 4)
#    endif
#  endif
#  if defined(__clang__)
#    define XZ_TARGET __attribute__((target("aes")))
#  else
#    define XZ_TARGET __attribute__((target("+crypto")))
#  endif

static int XZ_hwDetect(void)
{
#  if defined(__APPLE__)
    return 1;   /* every Apple ARM64 processor has PMULL */
#  else
    return (getauxval(AT_HWCAP) & HWCAP_PMULL) != 0;
#  endif
}

XZ_TARGET
static uint64x2_t XZ_fold(uint64x2_t x, U64 kHigh, U64 kLow)
{
    uint64x2_t const a = vreinterpretq_u64_p128(vmull_p64((poly64_t)vgetq_lane_u64(x, 0), (poly64_t)kHigh));
    uint64x2_t const b = vreinterpretq_u64_p128(vmull_p64((poly64_t)vgetq_lane_u64(x, 1), (poly64_t)kLow));
    return veorq_u64(a, b);
}

XZ_TARGET
static size_t XZ_foldHw(U64 reg, const BYTE *p, size_t size, const U64 K[4], BYTE state[16])
{
    U64 const init[2] = { reg, 0 };
    const BYTE *const start = p;

    uint64x2_t x0 = veorq_u64(vreinterpretq_u64_u8(vld1q_u8(p)), vld1q_u64(init));
    uint64x2_t x1 = vreinterpretq_u64_u8(vld1q_u8(p + 16));
    uint64x2_t x2 = vreinterpretq_u64_u8(vld1q_u8(p + 32));
    uint64x2_t x3 = vreinterpretq_u64_u8(vld1q_u8(p + 48));
    p += 64;
    size -= 64;
    for (; size >= 64; p += 64, size -= 64) {
        x0 = veorq_u64(XZ_fold(x0, K[3], K[2]), vreinterpretq_u64_u8(vld1q_u8(p)));
        x1 = veorq_u64(XZ_fold(x1, K[3], K[2]), vreinterpretq_u64_u8(vld1q_u8(p + 16)));
        x2 = veorq_u64(XZ_fold(x2, K[3], K[2]), vreinterpretq_u64_u8(vld1q_u8(p + 32)));
        x3 = veorq_u64(XZ_fold(x3, K[3], K[2]), vreinterpretq_u64_u8(vld1q_u8(p + 48)));
    }
    x1 = veorq_u64(XZ_fold(x0, K[1], K[0]), x1);
    x2 = veorq_u64(XZ_fold(x1, K[1], K[0]), x2);
    x3 = veorq_u64(XZ_fold(x2, K[1], K[0]), x3);
    for (; size >= 16; p += 16, size -= 16)
        x3 = veorq_u64(XZ_fold(x3, K[1], K[0]), vreinterpretq_u64_u8(vld1q_u8(p)));
    vst1q_u8(state, vreinterpretq_u8_u64(x3));
    return (size_t)(p - start);
}

#endif

#ifdef XZ_HW_CRC
/* 0 not yet known, 1 absent, 2 present. Any thread that finds it unknown runs
 * the detection and stores the same answer, so a race only repeats the work. */
static volatile int XZ_hwState = 0;

static int XZ_hwAvailable(void)
{
    int state = XZ_hwState;
    if (state == 0) {
        state = XZ_hwDetect() ? 2 : 1;
        XZ_hwState = state;
    }
    return state == 2;
}
#endif

U32 XZ_crc32(U32 crc, const void *buf, size_t size)
{
    const BYTE *p = (const BYTE *)buf;
    U32 reg = ~crc;
#ifdef XZ_HW_CRC
    if (size >= XZ_FOLD_MIN && XZ_hwAvailable()) {
        BYTE state[16];
        size_t const done = XZ_foldHw(reg, p, size, XZ_CRC32_K, state);
        reg = XZ_crc32Tables(0, state, sizeof(state));
        p += done;
        size -= done;
    }
#endif
    return ~XZ_crc32Tables(reg, p, size);
}

U64 XZ_crc64(U64 crc, const void *buf, size_t size)
{
    const BYTE *p = (const BYTE *)buf;
    U64 reg = ~crc;
#ifdef XZ_HW_CRC
    if (size >= XZ_FOLD_MIN && XZ_hwAvailable()) {
        BYTE state[16];
        size_t const done = XZ_foldHw(reg, p, size, XZ_CRC64_K, state);
        reg = XZ_crc64Tables(0, state, sizeof(state));
        p += done;
        size -= done;
    }
#endif
    return ~XZ_crc64Tables(reg, p, size);
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
    /* one filter; each size present unless unknown */
    out[1] = 0x00;
    if (compressedSize != XZ_SIZE_UNKNOWN) {
        out[1] |= 0x40;
        n += XZ_vliEncode(compressedSize, out + n);
    }
    if (uncompressedSize != XZ_SIZE_UNKNOWN) {
        out[1] |= 0x80;
        n += XZ_vliEncode(uncompressedSize, out + n);
    }
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

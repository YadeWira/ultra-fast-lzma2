/*
 * Copyright (c) 2026, the uf-lzma2 authors
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/* crc_check: the .xz CRCs, hardware paths against the tables.
 *
 * XZ_crc32() and XZ_crc64() fold with a carry-less multiply where the processor
 * has one and fall back to tables elsewhere; XZ_crc32Portable() and
 * XZ_crc64Portable() are always the tables. They must agree on every length from
 * 0 to 1100 bytes at every alignment, from random starting values, when a buffer
 * is split in two and continued, and on large buffers; and both must give the
 * standard check values. These are internal functions, so this links the static
 * library. Exits nonzero on any failure. */

#include <stdio.h>
#include <stdlib.h>
#include "uf2_xz.h"

static int failures = 0;
static long cases = 0;

#define FAIL(...) do { if (failures++ < 10) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static unsigned long long rngState = 0x243F6A8885A308D3ULL;

static U64 rng(void)
{
    rngState = rngState * 6364136223846793005ULL + 1442695040888963407ULL;
    return rngState ^ (rngState >> 29);
}

static void compare(const BYTE *p, size_t n, U64 init)
{
    U32 const a32 = XZ_crc32((U32)init, p, n), b32 = XZ_crc32Portable((U32)init, p, n);
    U64 const a64 = XZ_crc64(init, p, n), b64 = XZ_crc64Portable(init, p, n);
    cases += 2;
    if (a32 != b32)
        FAIL("CRC32 of %zu bytes at alignment %u from %08X: %08X, tables give %08X",
            n, (unsigned)((size_t)p & 15), (unsigned)(U32)init, (unsigned)a32, (unsigned)b32);
    if (a64 != b64)
        FAIL("CRC64 of %zu bytes at alignment %u: %016llX, tables give %016llX",
            n, (unsigned)((size_t)p & 15), (unsigned long long)a64, (unsigned long long)b64);
}

int main(void)
{
    static const char check[] = "123456789";
    size_t const big = 3 * 1024 * 1024 + 77;
    BYTE *const buf = malloc(big + 64);
    if (buf == NULL)
        return 2;
    for (size_t i = 0; i < big + 64; ++i)
        buf[i] = (BYTE)(rng() >> 56);

    ++cases;
    if (XZ_crc32(0, check, 9) != 0xCBF43926 || XZ_crc32Portable(0, check, 9) != 0xCBF43926)
        FAIL("CRC32 check value of \"123456789\"");
    ++cases;
    if (XZ_crc64(0, check, 9) != 0x995DC9BBDF1939FAULL || XZ_crc64Portable(0, check, 9) != 0x995DC9BBDF1939FAULL)
        FAIL("CRC64 check value of \"123456789\"");

    /* every length up to 1100 at every alignment, from zero and from a random value */
    for (size_t n = 0; n <= 1100; ++n)
        for (size_t align = 0; align < 16; ++align) {
            compare(buf + align, n, 0);
            compare(buf + align, n, rng());
        }

    /* split and continued: the CRC of a buffer equals the CRC of its tail started
     * from the CRC of its head, wherever the split falls */
    for (int i = 0; i < 2000; ++i) {
        size_t const n = (size_t)(rng() % 5000);
        size_t const cut = n ? (size_t)(rng() % n) : 0;
        const BYTE *const p = buf + (rng() & 15);
        U64 const init = rng();
        cases += 2;
        if (XZ_crc32(XZ_crc32((U32)init, p, cut), p + cut, n - cut) != XZ_crc32Portable((U32)init, p, n))
            FAIL("CRC32 of %zu bytes split at %zu", n, cut);
        if (XZ_crc64(XZ_crc64(init, p, cut), p + cut, n - cut) != XZ_crc64Portable(init, p, n))
            FAIL("CRC64 of %zu bytes split at %zu", n, cut);
    }

    /* large buffers */
    for (size_t k = 0; k < 8; ++k)
        compare(buf + k, big - k * 4099, rng());

    free(buf);
    printf("%s: %ld cases, %d failures\n", failures ? "FAILED" : "passed", cases, failures);
    return failures != 0;
}

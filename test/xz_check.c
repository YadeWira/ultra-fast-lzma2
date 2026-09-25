/*
 * Copyright (c) 2026, the uf-lzma2 authors
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/* xz_check: the .xz reader and writer, one-shot and streaming.
 *
 *     xz_check <fixture dir> [output dir]
 *
 * The fixtures in test/xz were written by xz 5.8.3 and 7-Zip 26.03 from
 * data.txt, empty.orig and one.orig. Every one is decoded one-shot on one and
 * four threads and streamed in several chunk sizes, and must give its original
 * or fail with the expected error. Then the library's own .xz output, one-shot
 * and streamed through every streaming API, is decoded every way. Last, the
 * framing of two files is mutated and truncated: no mutation may decode to
 * completion with wrong data.
 *
 * With an output dir, a few of the files written are saved there so that
 * `make check` can have xz itself test them. Exits nonzero on any failure. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uf-lzma2.h"
#include "uf2_errors.h"

#define MIN_LEN(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    unsigned char *data;
    size_t size;
} buffer;

static int failures = 0;
static int cases = 0;

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); ++failures; } while (0)

static buffer readFile(const char *dir, const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    buffer b;
    fseek(f, 0, SEEK_END);
    b.size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(b.size + 1);
    if (b.data == NULL || (b.size && fread(b.data, 1, b.size, f) != b.size)) {
        perror(path);
        exit(2);
    }
    fclose(f);
    return b;
}

static void writeFile(const char *dir, const char *name, buffer b)
{
    if (dir == NULL)
        return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL || fwrite(b.data, 1, b.size, f) != b.size) {
        perror(path);
        exit(2);
    }
    fclose(f);
}

static buffer concat(buffer a, buffer b)
{
    buffer c;
    c.size = a.size + b.size;
    c.data = malloc(c.size + 1);
    memcpy(c.data, a.data, a.size);
    memcpy(c.data + a.size, b.data, b.size);
    return c;
}

/* Deterministic test input: word-like text with long repeats, then a stretch
 * of incompressible bytes, so that blocks see both kinds of data. */
static buffer makeInput(size_t size)
{
    static const char *const words[] = { "radix", "match", "finder", "literal", "chunk", "dictionary",
        "stream", "block", "index", "check", "LZMA2", "encoder", "decoder", "0123", "\n", " " };
    buffer b;
    b.data = malloc(size + 1);
    b.size = size;
    unsigned long long s = 0x9E3779B97F4A7C15ULL;
    size_t pos = 0;
    while (pos < size) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        if (pos > 4096 && (s >> 60) == 0) {
            /* repeat an earlier stretch */
            size_t const len = MIN_LEN(size - pos, 64 + (size_t)((s >> 20) & 1023));
            size_t const from = (size_t)((s >> 32) % (pos - len));
            memmove(b.data + pos, b.data + from, len);
            pos += len;
        }
        else if (pos > size * 7 / 8) {
            b.data[pos++] = (unsigned char)(s >> 56);
        }
        else {
            const char *w = words[(s >> 40) & 15];
            while (*w && pos < size)
                b.data[pos++] = (unsigned char)*w++;
        }
    }
    return b;
}

/* ---------- decoding ---------- */

static size_t decodeOneShot(buffer c, unsigned char *out, size_t cap, unsigned threads)
{
    return threads > 1 ? UF2_decompressMt(out, cap, c.data, c.size, threads)
        : UF2_decompress(out, cap, c.data, c.size);
}

/* Stream c through ds in chunks. Returns the last result: 0 finished, 1 not
 * finished when the input ran out, or an error code. *outSize gets the output. */
static size_t decodeStream(UF2_DStream *ds, buffer c, unsigned char *out, size_t cap,
    size_t inChunk, size_t outChunk, size_t *outSize)
{
    size_t ip = 0, op = 0, r = 1;
    int idle = 0;
    UF2_initDStream(ds);
    for (;;) {
        size_t const n = MIN_LEN(c.size - ip, inChunk);
        size_t const o = MIN_LEN(cap - op, outChunk);
        UF2_inBuffer in = { c.data + ip, n, 0 };
        UF2_outBuffer ob = { out + op, o, 0 };
        r = UF2_decompressStream(ds, &ob, &in);
        while (UF2_isTimedOut(r))
            r = UF2_waitDStream(ds);
        ip += in.pos;
        op += ob.pos;
        if (UF2_isError(r))
            break;
        if (in.pos == 0 && ob.pos == 0) {
            if (++idle > 3)
                break;
        }
        else {
            idle = 0;
        }
        if (ip == c.size && (r == 0 || ob.pos < o))
            break;
    }
    *outSize = op;
    return r;
}

typedef enum { EXPECT_OK, EXPECT_UNSUPPORTED, EXPECT_CORRUPT } expectation;

static int errorMatches(size_t r, expectation e)
{
    if (!UF2_isError(r))
        return 0;
    if (e == EXPECT_UNSUPPORTED)
        return UF2_getErrorCode(r) == UF2_error_parameter_unsupported;
    return UF2_getErrorCode(r) == UF2_error_corruption_detected || UF2_getErrorCode(r) == UF2_error_checksum_wrong;
}

/* Decode c every way the library can and compare with orig. */
static void checkDecode(const char *name, buffer c, buffer orig, expectation e, int streamIncomplete)
{
    unsigned char *const out = malloc(orig.size + 64);
    size_t const cap = orig.size + 64;
    unsigned const threads[] = { 1, 4 };

    for (int t = 0; t < 2; ++t) {
        ++cases;
        size_t const r = decodeOneShot(c, out, e == EXPECT_OK ? orig.size : cap, threads[t]);
        if (e == EXPECT_OK) {
            if (UF2_isError(r) || r != orig.size || memcmp(out, orig.data, orig.size))
                FAIL("%s: one-shot, %u thread(s): %s", name, threads[t], UF2_isError(r) ? UF2_getErrorName(r) : "wrong output");
        }
        else if (!errorMatches(r, e)) {
            FAIL("%s: one-shot, %u thread(s): expected an error, got %s", name, threads[t],
                UF2_isError(r) ? UF2_getErrorName(r) : "success");
        }
    }
    if (e == EXPECT_OK) {
        ++cases;
        unsigned long long const size = UF2_findDecompressedSize(c.data, c.size);
        if (size != orig.size)
            FAIL("%s: UF2_findDecompressedSize() gave %llu, expected %zu", name, size, orig.size);
    }

    static const size_t chunks[][2] = { { 1, 1 }, { 7, 13 }, { 65536, 65536 } };
    for (int k = 0; k < 3; ++k) {
        for (unsigned t = 1; t <= 2; ++t) {
            if (t == 2 && k != 2)
                continue;
            UF2_DStream *const ds = UF2_createDStreamMt(t);
            size_t outSize = 0;
            ++cases;
            size_t const r = decodeStream(ds, c, out, cap, chunks[k][0], chunks[k][1], &outSize);
            if (e == EXPECT_OK) {
                if (r != 0 || outSize != orig.size || memcmp(out, orig.data, orig.size))
                    FAIL("%s: streamed %zu/%zu, %u thread(s): %s", name, chunks[k][0], chunks[k][1], t,
                        UF2_isError(r) ? UF2_getErrorName(r) : (r ? "unfinished" : "wrong output"));
            }
            else if (streamIncomplete) {
                /* A streaming decoder cannot tell that the input has ended; bad Stream
                 * Padding at the very end leaves it unfinished, which is how a caller
                 * sees a truncated file. */
                if (r != 1)
                    FAIL("%s: streamed %zu/%zu: expected unfinished, got %s", name, chunks[k][0], chunks[k][1],
                        UF2_isError(r) ? UF2_getErrorName(r) : "finished");
            }
            else if (!errorMatches(r, e)) {
                FAIL("%s: streamed %zu/%zu: expected an error, got %s", name, chunks[k][0], chunks[k][1],
                    UF2_isError(r) ? UF2_getErrorName(r) : (r ? "unfinished" : "finished"));
            }
            UF2_freeDStream(ds);
        }
    }
    free(out);
}

/* ---------- encoding ---------- */

static buffer compressXz(buffer src, int level, unsigned check, size_t blockSize, unsigned threads, size_t cap)
{
    buffer c;
    if (cap == 0)
        cap = UF2_compressBound(src.size);
    c.data = malloc(cap);
    UF2_CCtx *const cctx = UF2_createCCtxMt(threads);
    UF2_CCtx_setParameter(cctx, UF2_p_format, UF2_format_xz);
    UF2_CCtx_setParameter(cctx, UF2_p_xzCheck, check);
    UF2_CCtx_setParameter(cctx, UF2_p_xzBlockSize, blockSize);
    c.size = UF2_compressCCtx(cctx, c.data, cap, src.data, src.size, level);
    UF2_freeCCtx(cctx);
    return c;
}

static void append(buffer *b, size_t *cap, const void *p, size_t n)
{
    if (b->size + n > *cap) {
        *cap = (b->size + n) * 2;
        b->data = realloc(b->data, *cap);
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

/* Streamed compression. mode: 's' UF2_compressStream, 'f' the same with a flush
 * every third input, 'z' the zero-copy dictionary functions, 'a' with a timeout. */
static buffer compressStreamXz(buffer src, char mode, int level, unsigned threads, unsigned check,
    size_t inChunk, size_t outChunk)
{
    buffer c = { NULL, 0 };
    size_t cap = 0;
    UF2_CStream *const cs = UF2_createCStreamMt(threads, mode == 'a');
    unsigned char *const tmp = malloc(outChunk);
    size_t r;

    UF2_CStream_setParameter(cs, UF2_p_format, UF2_format_xz);
    UF2_CStream_setParameter(cs, UF2_p_xzCheck, check);
    if (mode == 'a')
        UF2_setCStreamTimeout(cs, 1);
    r = UF2_initCStream(cs, level);
    if (UF2_isError(r))
        goto error;
    if (mode == 'z') {
        size_t pos = 0;
        UF2_cBuffer cb;
        while (pos < src.size) {
            UF2_dictBuffer db;
            r = UF2_getDictionaryBuffer(cs, &db);
            if (UF2_isError(r))
                goto error;
            size_t const n = MIN_LEN(MIN_LEN((size_t)db.size, src.size - pos), inChunk);
            memcpy(db.dst, src.data + pos, n);
            pos += n;
            r = UF2_updateDictionary(cs, n);
            if (UF2_isError(r))
                goto error;
            while (r) {
                size_t const k = UF2_getNextCompressedBuffer(cs, &cb);
                if (UF2_isError(k)) {
                    r = k;
                    goto error;
                }
                if (k == 0)
                    break;
                append(&c, &cap, cb.src, cb.size);
            }
        }
        do {
            r = UF2_endStream(cs, NULL);
            if (UF2_isError(r))
                goto error;
            for (;;) {
                size_t const k = UF2_getNextCompressedBuffer(cs, &cb);
                if (UF2_isError(k)) {
                    r = k;
                    goto error;
                }
                if (k == 0)
                    break;
                append(&c, &cap, cb.src, cb.size);
            }
        } while (r);
    }
    else {
        size_t pos = 0;
        int calls = 0;
        while (pos < src.size) {
            size_t const n = MIN_LEN(src.size - pos, inChunk);
            UF2_inBuffer in = { src.data + pos, n, 0 };
            while (in.pos < in.size) {
                UF2_outBuffer out = { tmp, outChunk, 0 };
                do {
                    r = UF2_compressStream(cs, &out, &in);
                } while (UF2_isTimedOut(r));
                if (UF2_isError(r))
                    goto error;
                append(&c, &cap, tmp, out.pos);
            }
            pos += n;
            if (mode == 'f' && ++calls % 3 == 0) {
                do {
                    UF2_outBuffer out = { tmp, outChunk, 0 };
                    do {
                        r = UF2_flushStream(cs, &out);
                    } while (UF2_isTimedOut(r));
                    if (UF2_isError(r))
                        goto error;
                    append(&c, &cap, tmp, out.pos);
                } while (r);
            }
        }
        do {
            UF2_outBuffer out = { tmp, outChunk, 0 };
            do {
                r = UF2_endStream(cs, &out);
            } while (UF2_isTimedOut(r));
            if (UF2_isError(r))
                goto error;
            append(&c, &cap, tmp, out.pos);
        } while (r);
    }
    /* an empty result still needs a buffer, so that NULL means failure */
    if (c.data == NULL)
        c.data = malloc(1);
    goto done;
error:
    FAIL("streamed .xz compression, mode %c level %d: %s", mode, level, UF2_getErrorName(r));
    free(c.data);
    c.data = NULL;
    c.size = 0;
done:
    UF2_freeCStream(cs);
    free(tmp);
    return c;
}

/* ---------- mutation ---------- */

/* Flip every bit of the chosen bytes of c, and truncate it at many lengths.
 * Nothing may decode to completion with output other than orig. */
static void mutate(const char *name, buffer c, buffer orig, size_t stride)
{
    unsigned char *const out = malloc(orig.size + 64);
    size_t const cap = orig.size + 64;
    UF2_DStream *const ds = UF2_createDStream();
    int bad = 0, n = 0;

    for (size_t i = 0; i < c.size; ++i) {
        /* every byte of the first 64 and last 64, which hold the Stream Header, the
         * first Block Header, the Index and the Footer; a sample of the rest */
        if (i >= 64 && i + 64 < c.size && i % stride)
            continue;
        for (int bit = 0; bit < 8; ++bit) {
            c.data[i] ^= (unsigned char)(1 << bit);
            size_t r = decodeOneShot(c, out, orig.size, (i & 1) ? 4 : 1);
            ++n;
            if (!UF2_isError(r) && (r != orig.size || memcmp(out, orig.data, orig.size))) {
                if (bad++ < 3)
                    FAIL("%s: byte %zu bit %d flipped: one-shot success with wrong output", name, i, bit);
            }
            size_t outSize = 0;
            r = decodeStream(ds, c, out, cap, 4096, 4096, &outSize);
            ++n;
            if (r == 0 && (outSize != orig.size || memcmp(out, orig.data, orig.size))) {
                if (bad++ < 3)
                    FAIL("%s: byte %zu bit %d flipped: streamed success with wrong output", name, i, bit);
            }
            c.data[i] ^= (unsigned char)(1 << bit);
        }
    }
    for (size_t t = 0; t < c.size; t += 1 + c.size / 500) {
        buffer const part = { c.data, t };
        size_t outSize = 0;
        ++n;
        if (!UF2_isError(decodeOneShot(part, out, orig.size, 1))) {
            if (bad++ < 3)
                FAIL("%s: truncated to %zu bytes: one-shot success", name, t);
        }
        ++n;
        if (decodeStream(ds, part, out, cap, 4096, 4096, &outSize) == 0) {
            if (bad++ < 3)
                FAIL("%s: truncated to %zu bytes: streamed success", name, t);
        }
    }
    if (bad > 3)
        FAIL("%s: %d more mutation failures", name, bad - 3);
    cases += n;
    UF2_freeDStream(ds);
    free(out);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture dir> [output dir]\n", argv[0]);
        return 2;
    }
    /* unbuffered, so that a crash still shows how far the run got */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *const dir = argv[1];
    const char *const outDir = argc > 2 ? argv[2] : NULL;

    /* ---- files written by xz and 7-Zip ---- */
    buffer const data = readFile(dir, "data.txt");
    buffer const empty = readFile(dir, "empty.orig");
    buffer const one = readFile(dir, "one.orig");
    buffer const data2 = concat(data, data);
    static const struct {
        const char *file;
        int orig;           /* 0 data.txt, 1 empty, 2 one byte, 3 data.txt twice */
        expectation e;
        int streamIncomplete;
    } fixtures[] = {
        { "crc32.xz", 0, EXPECT_OK, 0 },
        { "crc64.xz", 0, EXPECT_OK, 0 },
        { "none.xz", 0, EXPECT_OK, 0 },
        { "preset0.xz", 0, EXPECT_OK, 0 },
        { "preset9e.xz", 0, EXPECT_OK, 0 },
        { "blocks_sizes.xz", 0, EXPECT_OK, 0 },     /* 5 blocks, sizes in the headers: decoded in parallel */
        { "blocks_nosizes.xz", 0, EXPECT_OK, 0 },   /* 5 blocks, no sizes: decoded in order */
        { "7z.xz", 0, EXPECT_OK, 0 },
        { "empty.xz", 1, EXPECT_OK, 0 },
        { "one.xz", 2, EXPECT_OK, 0 },
        { "concat.xz", 3, EXPECT_OK, 0 },           /* two Streams and Stream Padding */
        { "sha256.xz", 0, EXPECT_UNSUPPORTED, 0 },
        { "bcj.xz", 0, EXPECT_UNSUPPORTED, 0 },
        { "delta.xz", 0, EXPECT_UNSUPPORTED, 0 },
        { "pad3.xz", 0, EXPECT_CORRUPT, 1 },        /* Stream Padding not a multiple of four */
        { "junk.xz", 0, EXPECT_CORRUPT, 0 },        /* bytes after the Stream */
    };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        buffer const origs[] = { data, empty, one, data2 };
        buffer c = readFile(dir, fixtures[i].file);
        checkDecode(fixtures[i].file, c, origs[fixtures[i].orig], fixtures[i].e, fixtures[i].streamIncomplete);
        free(c.data);
    }
    printf("fixtures: %d cases, %d failures\n", cases, failures);

    /* ---- the library's own .xz, one-shot ---- */
    buffer const input = makeInput(3 * 1024 * 1024 + 12345);
    static const int levels[] = { 1, 6 };
    static const unsigned checks[] = { 0, 1, 4 };
    for (int l = 0; l < 2; ++l) {
        for (int k = 0; k < 3; ++k) {
            for (size_t bs = 0; bs <= 1; ++bs) {
                char name[64];
                snprintf(name, sizeof(name), "one-shot level %d check %u block %s", levels[l], checks[k], bs ? "1 MiB" : "default");
                buffer c = compressXz(input, levels[l], checks[k], bs << 20, 2, 0);
                ++cases;
                if (UF2_isError(c.size)) {
                    FAIL("%s: %s", name, UF2_getErrorName(c.size));
                    free(c.data);
                    continue;
                }
                checkDecode(name, c, input, EXPECT_OK, 0);
                if (l == 1 && k == 2)
                    writeFile(outDir, bs ? "oneshot_blocks.xz" : "oneshot.xz", c);
                free(c.data);
            }
        }
    }
    {
        buffer c = compressXz(empty, 6, 4, 0, 1, 0);
        ++cases;
        if (UF2_isError(c.size)) {
            FAIL("one-shot empty input: %s", UF2_getErrorName(c.size));
        }
        else {
            checkDecode("one-shot empty input", c, empty, EXPECT_OK, 0);
            writeFile(outDir, "oneshot_empty.xz", c);
        }
        free(c.data);
    }
    /* UF2_compressBound() must hold for incompressible input in the smallest blocks */
    {
        buffer noise;
        noise.size = 2 * 1024 * 1024 + 777;
        noise.data = malloc(noise.size);
        unsigned long long s = 1;
        for (size_t i = 0; i < noise.size; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            noise.data[i] = (unsigned char)(s >> 56);
        }
        buffer c = compressXz(noise, 6, 4, 1 << 20, 2, UF2_compressBound(noise.size));
        ++cases;
        if (UF2_isError(c.size))
            FAIL("incompressible input in 1 MiB blocks into UF2_compressBound() bytes: %s", UF2_getErrorName(c.size));
        else
            checkDecode("incompressible input", c, noise, EXPECT_OK, 0);
        free(c.data);
        free(noise.data);
    }
    {
        UF2_CCtx *const cctx = UF2_createCCtx();
        ++cases;
        if (!UF2_isError(UF2_CCtx_setParameter(cctx, UF2_p_xzBlockSize, UF2_XZ_BLOCKSIZE_MIN - 1)))
            FAIL("UF2_p_xzBlockSize accepted a size below UF2_XZ_BLOCKSIZE_MIN");
        ++cases;
        if (!UF2_isError(UF2_CCtx_setParameter(cctx, UF2_p_xzCheck, 10)))
            FAIL("UF2_p_xzCheck accepted SHA-256");
        UF2_freeCCtx(cctx);
    }
    printf("one-shot .xz: %d cases, %d failures\n", cases, failures);

    /* ---- the library's own .xz, streamed ---- */
    static const char modes[] = { 's', 'f', 'z', 'a' };
    static const char *const modeFiles[] = { "stream.xz", "stream_flush.xz", "stream_zerocopy.xz", "stream_async.xz" };
    for (int m = 0; m < 4; ++m) {
        for (int l = 0; l < 2; ++l) {
            char name[64];
            snprintf(name, sizeof(name), "streamed mode %c level %d", modes[m], levels[l]);
            buffer c = compressStreamXz(input, modes[m], levels[l], 1 + (unsigned)l, 4, 100000, 7777);
            ++cases;
            if (c.data == NULL)
                continue;
            checkDecode(name, c, input, EXPECT_OK, 0);
            if (l == 0)
                writeFile(outDir, modeFiles[m], c);
            free(c.data);
        }
    }
    {
        buffer c = compressStreamXz(one, 's', 6, 1, 1, 1, 1);
        ++cases;
        if (c.data != NULL) {
            checkDecode("streamed one byte, 1-byte buffers", c, one, EXPECT_OK, 0);
            free(c.data);
        }
        c = compressStreamXz(empty, 's', 6, 1, 0, 1, 1);
        ++cases;
        if (c.data != NULL) {
            checkDecode("streamed empty input", c, empty, EXPECT_OK, 0);
            writeFile(outDir, "stream_empty.xz", c);
            free(c.data);
        }
    }
    printf("streamed .xz: %d cases, %d failures\n", cases, failures);

    /* ---- regressions ---- */
    {
        /* A native stream whose property byte has the hash flag and an invalid
         * dictionary size crashed the streaming decoder (fixed in 1.4.0). */
        static unsigned char bad[] = { 0xFF, 0x00 };
        buffer const c = { bad, sizeof(bad) };
        unsigned char out[16];
        size_t outSize;
        UF2_DStream *const ds = UF2_createDStream();
        ++cases;
        size_t const r = decodeStream(ds, c, out, sizeof(out), 64, 16, &outSize);
        if (!UF2_isError(r))
            FAIL("native stream FF 00: expected an error, got %s", r ? "unfinished" : "finished");
        UF2_freeDStream(ds);
    }
    printf("regressions: %d cases, %d failures\n", cases, failures);

    /* ---- mutation ---- */
    {
        buffer c = readFile(dir, "blocks_sizes.xz");
        mutate("blocks_sizes.xz", c, data, 3);
        free(c.data);
        c = compressXz(data, 6, 4, 0, 1, 0);
        mutate("own .xz", c, data, 3);
        free(c.data);
    }
    printf("mutation: %d cases, %d failures\n", cases, failures);

    free(input.data);
    free(data2.data);
    free(data.data);
    free(empty.data);
    free(one.data);
    printf("%s: %d cases, %d failures\n", failures ? "FAILED" : "passed", cases, failures);
    return failures != 0;
}

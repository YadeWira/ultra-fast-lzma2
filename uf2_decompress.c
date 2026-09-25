/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
* Parts based on zstd_decompress.c copyright Yann Collet
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include <string.h>
#include "uf-lzma2.h"
#include "uf2_xz.h"
#include "uf2_errors.h"
#include "uf2_internal.h"
#include "mem.h"
#include "util.h"
#include "lzma2_dec.h"
#include "uf2_threading.h"
#include "uf2_pool.h"
#include "atomic.h"
#ifndef NO_XXHASH
#  include "xxhash.h"
#endif


#define LZMA2_PROP_UNINITIALIZED 0xFF


/* The uncompressed size of an .xz file is the sum of its Index records. It is
 * read from the end, stream by stream, the way xz --list does it, so `src` must
 * hold the whole file. Every structure consulted is CRC-checked. */
static unsigned long long UF2_xzFindDecompressedSize(const BYTE *in, size_t size)
{
    unsigned long long total = 0;
    size_t end = size;
    while (end > 0) {
        size_t pad = 0;
        while (end > 0 && in[end - 1] == 0) {       /* Stream Padding */
            --end;
            ++pad;
        }
        if (pad & 3)
            return UF2_CONTENTSIZE_ERROR;
        if (end == 0)
            break;
        if (end < XZ_STREAM_HEADER_SIZE + XZ_STREAM_FOOTER_SIZE)
            return UF2_CONTENTSIZE_ERROR;

        const BYTE *const f = in + end - XZ_STREAM_FOOTER_SIZE;
        if (f[10] != 'Y' || f[11] != 'Z' || MEM_readLE32(f) != XZ_crc32(0, f + 4, 6))
            return UF2_CONTENTSIZE_ERROR;
        size_t const indexSize = ((size_t)MEM_readLE32(f + 4) + 1) * 4;
        if (indexSize > (size_t)(f - in) - XZ_STREAM_HEADER_SIZE)
            return UF2_CONTENTSIZE_ERROR;
        const BYTE *const idx = f - indexSize;
        if (idx[0] != 0 || MEM_readLE32(idx + indexSize - 4) != XZ_crc32(0, idx, indexSize - 4))
            return UF2_CONTENTSIZE_ERROR;

        size_t q = 1, n;
        size_t const limit = indexSize - 4;
        U64 count, blocks = 0, uncompressed = 0;
        if ((n = XZ_vliDecode(idx + q, limit - q, &count)) == 0)
            return UF2_CONTENTSIZE_ERROR;
        q += n;
        for (U64 i = 0; i < count; ++i) {
            U64 unpadded, usize;
            if ((n = XZ_vliDecode(idx + q, limit - q, &unpadded)) == 0)
                return UF2_CONTENTSIZE_ERROR;
            q += n;
            if ((n = XZ_vliDecode(idx + q, limit - q, &usize)) == 0)
                return UF2_CONTENTSIZE_ERROR;
            q += n;
            blocks += (unpadded + 3) & ~(U64)3;
            uncompressed += usize;
        }
        /* the blocks must fill the gap between the Stream Header and the Index exactly */
        if (blocks > (U64)(idx - in) - XZ_STREAM_HEADER_SIZE)
            return UF2_CONTENTSIZE_ERROR;
        const BYTE *const h = idx - blocks - XZ_STREAM_HEADER_SIZE;
        if (!XZ_isXz(h, XZ_STREAM_HEADER_SIZE) || h[6] != f[8] || h[7] != f[9])
            return UF2_CONTENTSIZE_ERROR;
        total += uncompressed;
        end = (size_t)(h - in);
    }
    return total;
}

UF2LIB_API unsigned long long UF2LIB_CALL UF2_findDecompressedSize(const void *src, size_t srcSize)
{
    if (XZ_isXz(src, srcSize))
        return UF2_xzFindDecompressedSize((const BYTE*)src, srcSize);
    return LZMA2_getUnpackSize(src, srcSize);
}

UF2LIB_API size_t UF2LIB_CALL UF2_getDictSizeFromProp(unsigned char prop)
{
    return LZMA2_getDictSizeFromProp(prop);
}

typedef struct
{
    LZMA2_DCtx* dec;
    const void *src;
    size_t packPos;
    size_t packSize;
    size_t unpackPos;
    size_t unpackSize;
    size_t res;
    LZMA2_finishMode finish;
} UF2_blockDecMt;

struct UF2_DCtx_s
{
    LZMA2_DCtx dec;
#ifndef UF2_SINGLETHREAD
    UF2_blockDecMt *blocks;
    UF2POOL_ctx *factory;
    size_t nbThreads;
#endif
    BYTE lzma2prop;
};

UF2LIB_API size_t UF2LIB_CALL UF2_decompress(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize)
{
    return UF2_decompressMt(dst, dstCapacity, src, compressedSize, 1);
}

UF2LIB_API size_t UF2LIB_CALL UF2_decompressMt(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize,
    unsigned nbThreads)
{
    UF2_DCtx* const dctx = UF2_createDCtxMt(nbThreads);
    if(dctx == NULL)
        return UF2_ERROR(memory_allocation);

    size_t const dSize = UF2_decompressDCtx(dctx,
        dst, dstCapacity,
        src, compressedSize);

    UF2_freeDCtx(dctx);

    return dSize;
}

UF2LIB_API UF2_DCtx* UF2LIB_CALL UF2_createDCtx(void)
{
    return UF2_createDCtxMt(1);
}

UF2LIB_API UF2_DCtx *UF2LIB_CALL UF2_createDCtxMt(unsigned nbThreads)
{
    DEBUGLOG(3, "UF2_createDCtx");

    UF2_DCtx* const dctx = UF2_malloc(sizeof(UF2_DCtx));

    if (dctx == NULL)
        return NULL;

    LZMA_constructDCtx(&dctx->dec);

    dctx->lzma2prop = LZMA2_PROP_UNINITIALIZED;

    nbThreads = UF2_checkNbThreads(nbThreads);

#ifndef UF2_SINGLETHREAD
    dctx->nbThreads = 1;
    dctx->blocks = NULL;
    dctx->factory = NULL;

    if (nbThreads > 1) {
        dctx->blocks = UF2_malloc(nbThreads * sizeof(UF2_blockDecMt));
        dctx->factory = UF2POOL_create(nbThreads - 1);

        if (dctx->blocks == NULL || dctx->factory == NULL) {
            UF2_freeDCtx(dctx);
            return NULL;
        }
        dctx->blocks[0].dec = &dctx->dec;

        for (; dctx->nbThreads < nbThreads; ++dctx->nbThreads) {

            dctx->blocks[dctx->nbThreads].dec = UF2_malloc(sizeof(LZMA2_DCtx));

            if (dctx->blocks[dctx->nbThreads].dec == NULL) {
                UF2_freeDCtx(dctx);
                return NULL;
            }
            LZMA_constructDCtx(dctx->blocks[dctx->nbThreads].dec);
        }
    }
#endif

    return dctx;
}

UF2LIB_API size_t UF2LIB_CALL UF2_freeDCtx(UF2_DCtx* dctx)
{
    if (dctx == NULL)
        return UF2_error_no_error;

    DEBUGLOG(3, "UF2_freeDCtx");

    LZMA_destructDCtx(&dctx->dec);

#ifndef UF2_SINGLETHREAD
    if (dctx->blocks != NULL) {
        for (unsigned thread = 1; thread < dctx->nbThreads; ++thread) {
            LZMA_destructDCtx(dctx->blocks[thread].dec);
            UF2_free(dctx->blocks[thread].dec);
        }
        UF2_free(dctx->blocks);
    }
    UF2POOL_free(dctx->factory);
#endif
    UF2_free(dctx);

    return UF2_error_no_error;
}

#ifndef UF2_SINGLETHREAD

UF2LIB_API unsigned UF2LIB_CALL UF2_getDCtxThreadCount(const UF2_DCtx * dctx)
{
    return (unsigned)dctx->nbThreads;
}

/* UF2_decompressCtxBlock() : UF2POOL_function type */
static void UF2_decompressCtxBlock(void* const jobDescription, ptrdiff_t const n)
{
    UF2_blockDecMt* const blocks = (UF2_blockDecMt*)jobDescription;
    size_t srcLen = blocks[n].packSize;

    DEBUGLOG(4, "Thread %u: decoding block of input size %u, output size %u", (unsigned)n, (unsigned)srcLen, (unsigned)blocks[n].unpackSize);

    blocks[n].res = LZMA2_decodeToDic(blocks[n].dec, blocks[n].unpackSize, blocks[n].src, &srcLen, blocks[n].finish);

    /* If no error occurred, store into res the dic_pos value, which is the end of the decompressed data in the buffer */
    if (!UF2_isError(blocks[n].res))
        blocks[n].res = blocks[n].dec->dic_pos;
}

static size_t UF2_decompressCtxBlocksMt(UF2_DCtx* const dctx, const BYTE *const src, BYTE *const dst, size_t const dstCapacity, size_t const nbThreads)
{
    UF2_blockDecMt* const blocks = dctx->blocks;

    /* Initial check for block 0. The others are uncalculated */
    if (dstCapacity < blocks[0].unpackSize)
        return UF2_ERROR(dstSize_tooSmall);

    blocks[0].packPos = 0;
    blocks[0].unpackPos = 0;
    blocks[0].src = src;

    BYTE const prop = dctx->lzma2prop & UF2_LZMA_PROP_MASK;

    for (size_t thread = 1; thread < nbThreads; ++thread) {
        blocks[thread].packPos = blocks[thread - 1].packPos + blocks[thread - 1].packSize;
        blocks[thread].unpackPos = blocks[thread - 1].unpackPos + blocks[thread - 1].unpackSize;
        blocks[thread].src = src + blocks[thread].packPos;
        CHECK_F(LZMA2_initDecoder(blocks[thread].dec, prop, dst + blocks[thread].unpackPos, blocks[thread].unpackSize));
    }
    if (dstCapacity < blocks[nbThreads - 1].unpackPos + blocks[nbThreads - 1].unpackSize)
        return UF2_ERROR(dstSize_tooSmall);

    /* Decompress thread 1..n */
    UF2POOL_addRange(dctx->factory, UF2_decompressCtxBlock, blocks, 1, nbThreads);

    /* Decompress thread 0 */
    CHECK_F(LZMA2_initDecoder(blocks[0].dec, prop, dst + blocks[0].unpackPos, blocks[0].unpackSize));
    UF2_decompressCtxBlock(blocks, 0);

    UF2POOL_waitAll(dctx->factory, 0);

    size_t dSize = 0;
    for (size_t thread = 0; thread < nbThreads; ++thread) {
        if (UF2_isError(blocks[thread].res))
            return blocks[thread].res;
        dSize += blocks[thread].res;
    }
    return dSize;
}

static void UF2_resetMtBlocks(UF2_DCtx* const dctx)
{
    for (size_t thread = 0; thread < dctx->nbThreads; ++thread) {
        dctx->blocks[thread].finish = LZMA_FINISH_ANY;
        dctx->blocks[thread].packSize = 0;
        dctx->blocks[thread].unpackSize = 0;
    }
}

/* Decompress an entire stream stored in memory */
static size_t UF2_decompressDCtxMt(UF2_DCtx* const dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t *const srcLen)
{
    size_t srcSize = *srcLen;
    *srcLen = 0;

    UF2_resetMtBlocks(dctx);

    size_t unpackSize = 0;
    UF2_blockDecMt* const blocks = dctx->blocks;
    size_t thread = 0;
    size_t pos = 0;
    while (pos < srcSize) {
        LZMA2_chunk inf;
        int type = LZMA2_parseInput(src, pos, srcSize - pos, &inf);

        /* All src data must be in memory so CHUNK_MORE_DATA is an error */
        if (type == CHUNK_ERROR || type == CHUNK_MORE_DATA)
            return UF2_ERROR(corruption_detected);

        /* CHUNK_DICT_RESET is used to signal block completion except for pos 0 */
        if (pos == 0 && type == CHUNK_DICT_RESET)
            type = CHUNK_CONTINUE;

        if (type == CHUNK_DICT_RESET || type == CHUNK_FINAL) {
            if (type == CHUNK_FINAL) {
                /* The finish value will be passed to the decoder */
                blocks[thread].finish = LZMA_FINISH_END;
                /* CHUNK_FINAL means a single 0 byte */
                assert(inf.pack_size == 1);
                ++blocks[thread].packSize;
            }
            /* Move to the next thread. Decoding will begin if all threads are used. */
            ++thread;
        }
        if (type == CHUNK_FINAL || (type == CHUNK_DICT_RESET && thread == dctx->nbThreads)) {
            size_t res = UF2_decompressCtxBlocksMt(dctx, (BYTE*)src, dst, dstCapacity, thread);
            if (UF2_isError(res))
                return res;

            unpackSize += res;
            /* Store the unpack size in decoder 0 where it would be in single thread */
            dctx->dec.dic_pos = unpackSize;
            /* Input used is the end of data consumed by the last thread */
            *srcLen += blocks[thread - 1].packPos + blocks[thread - 1].packSize;

            if (type == CHUNK_FINAL)
                return LZMA_STATUS_FINISHED;

            /* Only excecuted at a dict reset. pos is the location of the reset */
            src = (BYTE*)src + pos;
            srcSize -= pos;
            dst = (BYTE*)dst + res;
            dstCapacity -= res;
            pos = 0;
            thread = 0;

            UF2_resetMtBlocks(dctx);
        }
        else {
            /* Not the end or a dict reset, so add it to the current block */
            blocks[thread].packSize += inf.pack_size;
            blocks[thread].unpackSize += inf.unpack_size;
            pos += inf.pack_size;
        }
    }
    return UF2_ERROR(srcSize_wrong);
}

#endif /* !defined UF2_SINGLETHREAD */

UF2LIB_API size_t UF2LIB_CALL UF2_initDCtx(UF2_DCtx * dctx, unsigned char prop)
{
    if((prop & UF2_LZMA_PROP_MASK) > 40)
        return UF2_ERROR(corruption_detected);

    dctx->lzma2prop = prop;
    return UF2_error_no_error;
}

/* Decode one raw LZMA2 stream: no property byte, no hash. Shared by the native
 * format and by each block of an .xz file. On success returns the bytes written
 * to dst and sets *srcConsumed to the bytes of src used, end marker included.
 * dst doubles as the dictionary, so each call starts a fresh one; both LZMA2 in
 * this library's framing and every .xz block begin with a dictionary reset. */
static size_t UF2_decompressLzma2(UF2_DCtx* dctx, BYTE prop,
    void* dst, size_t dstCapacity,
    const BYTE* src, size_t srcSize, size_t* srcConsumed)
{
    size_t srcPos = srcSize;
    size_t dicPos = 0;
    size_t res;
#ifndef UF2_SINGLETHREAD
    if (dctx->blocks != NULL) {
        dctx->lzma2prop = prop;
        res = UF2_decompressDCtxMt(dctx, dst, dstCapacity, src, &srcPos);
    }
    else
#endif
    {
        res = LZMA2_initDecoder(&dctx->dec, prop, dst, dstCapacity);
        if (!UF2_isError(res)) {
            dicPos = dctx->dec.dic_pos;
            res = LZMA2_decodeToDic(&dctx->dec, dstCapacity, src, &srcPos, LZMA_FINISH_END);
        }
    }

    /* reset on every path, errors included, so the context is clean for its next use */
    dctx->lzma2prop = LZMA2_PROP_UNINITIALIZED;

    if (UF2_isError(res))
        return res;
    /* All src data must be in memory */
    if (res == LZMA_STATUS_NEEDS_MORE_INPUT)
        return UF2_ERROR(srcSize_wrong);

    *srcConsumed = srcPos;
    return dctx->dec.dic_pos - dicPos;
}

/* ---------- .xz ---------- */

typedef struct {
    U64 unpadded;
    U64 uncompressed;
} XZ_record;

typedef struct {
    size_t headerSize;
    BYTE flags;
    BYTE prop;
    U64 cSize;      /* valid if flags & 0x40 */
    U64 uSize;      /* valid if flags & 0x80 */
} XZ_blockHeader;

#define XZ_HAS_CSIZE 0x40
#define XZ_HAS_USIZE 0x80

/* Parse and verify the Block Header at in[0], in[0] being nonzero (not the Index
 * Indicator). Returns 0 or an error code. */
static size_t XZ_parseBlockHeader(const BYTE* in, size_t inSize, XZ_blockHeader* h)
{
    size_t const headerSize = ((size_t)in[0] + 1) * 4;
    if (headerSize > inSize)
        return UF2_ERROR(srcSize_wrong);
    size_t const headerEnd = headerSize - 4;
    if (MEM_readLE32(in + headerEnd) != XZ_crc32(0, in, headerSize - 4))
        return UF2_ERROR(corruption_detected);

    BYTE const flags = in[1];
    if (flags & 0x3C)
        return UF2_ERROR(parameter_unsupported);    /* reserved block flags */
    if ((flags & 0x03) != 0)
        return UF2_ERROR(parameter_unsupported);    /* a filter chain: only a lone LZMA2 is supported */

    size_t q = 2, n;
    h->cSize = 0;
    h->uSize = 0;
    if (flags & XZ_HAS_CSIZE) {
        if ((n = XZ_vliDecode(in + q, headerEnd - q, &h->cSize)) == 0 || h->cSize == 0)
            return UF2_ERROR(corruption_detected);
        q += n;
    }
    if (flags & XZ_HAS_USIZE) {
        if ((n = XZ_vliDecode(in + q, headerEnd - q, &h->uSize)) == 0)
            return UF2_ERROR(corruption_detected);
        q += n;
    }
    U64 filterId, propsSize;
    if ((n = XZ_vliDecode(in + q, headerEnd - q, &filterId)) == 0)
        return UF2_ERROR(corruption_detected);
    q += n;
    if (filterId != XZ_LZMA2_FILTER_ID)
        return UF2_ERROR(parameter_unsupported);    /* BCJ, delta and the rest */
    if ((n = XZ_vliDecode(in + q, headerEnd - q, &propsSize)) == 0 || propsSize != 1 || q + n >= headerEnd)
        return UF2_ERROR(corruption_detected);
    q += n;
    BYTE const prop = in[q++];
    if (prop > 40)
        return UF2_ERROR(corruption_detected);
    for (; q < headerEnd; ++q)
        if (in[q] != 0)
            return UF2_ERROR(corruption_detected);  /* Header Padding must be zero */

    h->headerSize = headerSize;
    h->flags = flags;
    h->prop = prop;
    return 0;
}

/* Verify the check of one decoded block against the field at `field`. */
static size_t XZ_verifyCheck(unsigned check, const BYTE* field, const BYTE* data, size_t size)
{
    if (check == XZ_CHECK_CRC32) {
        if (MEM_readLE32(field) != XZ_crc32(0, data, size))
            return UF2_ERROR(checksum_wrong);
    }
    else if (check == XZ_CHECK_CRC64) {
        if (MEM_readLE64(field) != XZ_crc64(0, data, size))
            return UF2_ERROR(checksum_wrong);
    }
    return 0;
}

static size_t XZ_addRecord(XZ_record** records, size_t* nRecords, size_t* capRecords, U64 unpadded, U64 uncompressed)
{
    if (*nRecords == *capRecords) {
        size_t const newCap = *capRecords ? *capRecords * 2 : 16;
        XZ_record *const r = realloc(*records, newCap * sizeof(XZ_record));
        if (r == NULL)
            return UF2_ERROR(memory_allocation);
        *records = r;
        *capRecords = newCap;
    }
    (*records)[*nRecords].unpadded = unpadded;
    (*records)[*nRecords].uncompressed = uncompressed;
    ++*nRecords;
    return 0;
}

#ifndef UF2_SINGLETHREAD

/* One .xz block to decode on its own. */
typedef struct {
    const BYTE* src;    /* LZMA2 data */
    size_t cSize;
    BYTE* dst;
    size_t uSize;
    const BYTE* check;  /* the check field */
    BYTE prop;
    size_t res;
} XZ_blockJob;

typedef struct {
    UF2_DCtx* dctx;
    XZ_blockJob* jobs;
    size_t nJobs;
    size_t nThreads;
    unsigned check;
} XZ_blocksMt;

/* UF2POOL_function type: thread n decodes blocks n, n + nThreads, ... Blocks are
 * of equal size except the last, so striding balances the load without a queue. */
static void UF2_decompressXzBlocks(void* const opaque, ptrdiff_t const n)
{
    XZ_blocksMt* const mt = (XZ_blocksMt*)opaque;
    LZMA2_DCtx* const dec = mt->dctx->blocks[n].dec;

    for (size_t j = (size_t)n; j < mt->nJobs; j += mt->nThreads) {
        XZ_blockJob* const job = mt->jobs + j;
        size_t res = LZMA2_initDecoder(dec, job->prop, job->dst, job->uSize);
        if (!UF2_isError(res)) {
            size_t used = job->cSize;
            res = LZMA2_decodeToDic(dec, job->uSize, job->src, &used, LZMA_FINISH_END);
            /* the block must end exactly where both of its header's sizes say */
            if (!UF2_isError(res)
                && (res != LZMA_STATUS_FINISHED || used != job->cSize || dec->dic_pos != job->uSize))
                res = UF2_ERROR(corruption_detected);
        }
        if (!UF2_isError(res))
            res = XZ_verifyCheck(mt->check, job->check, job->dst, job->uSize);
        job->res = res;
    }
}

/* Decode the blocks of one Stream on all threads. Only possible when every Block
 * Header states both sizes, which is how this library, and xz when it compresses
 * on several threads, write them; otherwise a block's end is found only by
 * decoding it. Returns 1 if the blocks were not decoded here and the caller must
 * decode them in sequence, else 0 with *p at the Index Indicator and the records
 * filled in, or an error code. */
static size_t UF2_decompressXzBlocksMt(UF2_DCtx* dctx,
    BYTE* out, size_t outCapacity,
    const BYTE* in, size_t inSize, unsigned check,
    size_t* p, size_t* op, XZ_record** records, size_t* nRecords, size_t* capRecords)
{
    size_t const checkSize = (size_t)XZ_checkSize(check);
    XZ_blockJob* jobs = NULL;
    size_t nJobs = 0, capJobs = 0;
    size_t q = *p, o = 0;
    size_t res = 0;

    /* ---- walk the headers: every block's place in the input and in the output ---- */
    for (;;) {
        if (q >= inSize) {
            res = UF2_ERROR(srcSize_wrong);
            goto done;
        }
        if (in[q] == 0x00)
            break;
        XZ_blockHeader h;
        res = XZ_parseBlockHeader(in + q, inSize - q, &h);
        if (UF2_isError(res))
            goto done;
        if ((h.flags & (XZ_HAS_CSIZE | XZ_HAS_USIZE)) != (XZ_HAS_CSIZE | XZ_HAS_USIZE)) {
            res = 1;
            goto done;
        }
        size_t const d = q + h.headerSize;
        if (h.cSize > inSize - d) {
            res = UF2_ERROR(srcSize_wrong);
            goto done;
        }
        if (h.uSize > outCapacity - o) {
            res = UF2_ERROR(dstSize_tooSmall);
            goto done;
        }
        size_t const cSize = (size_t)h.cSize;
        size_t e = d + cSize;
        size_t const padding = (4 - (cSize & 3)) & 3;
        if (padding + checkSize > inSize - e) {
            res = UF2_ERROR(srcSize_wrong);
            goto done;
        }
        for (size_t k = 0; k < padding; ++k)
            if (in[e + k] != 0) {
                res = UF2_ERROR(corruption_detected);   /* Block Padding must be zero */
                goto done;
            }
        e += padding;

        if (nJobs == capJobs) {
            size_t const newCap = capJobs ? capJobs * 2 : 16;
            XZ_blockJob *const j = realloc(jobs, newCap * sizeof(XZ_blockJob));
            if (j == NULL) {
                res = UF2_ERROR(memory_allocation);
                goto done;
            }
            jobs = j;
            capJobs = newCap;
        }
        jobs[nJobs].src = in + d;
        jobs[nJobs].cSize = cSize;
        jobs[nJobs].dst = out + o;
        jobs[nJobs].uSize = (size_t)h.uSize;
        jobs[nJobs].check = in + e;
        jobs[nJobs].prop = h.prop;
        jobs[nJobs].res = 0;
        ++nJobs;
        res = XZ_addRecord(records, nRecords, capRecords, h.headerSize + cSize + checkSize, h.uSize);
        if (UF2_isError(res))
            goto done;

        o += (size_t)h.uSize;
        q = e + checkSize;
    }
    /* A lone block is left to the sequential path, whose decoder can still split
     * it at any dictionary resets inside. */
    if (nJobs < 2) {
        res = 1;
        goto done;
    }

    /* ---- decode ---- */
    {
        XZ_blocksMt mt;
        mt.dctx = dctx;
        mt.jobs = jobs;
        mt.nJobs = nJobs;
        mt.nThreads = MIN(dctx->nbThreads, nJobs);
        mt.check = check;
        UF2POOL_addRange(dctx->factory, UF2_decompressXzBlocks, &mt, 1, (ptrdiff_t)mt.nThreads);
        UF2_decompressXzBlocks(&mt, 0);
        UF2POOL_waitAll(dctx->factory, 0);
    }
    /* the first failing block decides the error, as if they had been decoded in order */
    for (size_t j = 0; j < nJobs; ++j)
        if (UF2_isError(jobs[j].res)) {
            res = jobs[j].res;
            goto done;
        }
    *p = q;
    *op = o;
    res = 0;

done:
    free(jobs);
    if (res == 1)
        *nRecords = 0;      /* the sequential path starts the records over */
    return res;
}

#endif /* !UF2_SINGLETHREAD */

/* Decode one .xz Stream starting at in[0]. Validates every CRC32 of the framing,
 * the check of each block, and the Index against the blocks actually decoded,
 * the way xz itself does. */
static size_t UF2_decompressXzStream(UF2_DCtx* dctx,
    BYTE* out, size_t outCapacity,
    const BYTE* in, size_t inSize, size_t* inConsumed)
{
    if (inSize < XZ_STREAM_HEADER_SIZE + XZ_STREAM_FOOTER_SIZE)
        return UF2_ERROR(srcSize_wrong);
    if (!XZ_isXz(in, inSize))
        return UF2_ERROR(corruption_detected);
    if (MEM_readLE32(in + 8) != XZ_crc32(0, in + 6, 2))
        return UF2_ERROR(corruption_detected);
    if (in[6] != 0 || (in[7] & 0xF0))
        return UF2_ERROR(parameter_unsupported);    /* reserved stream flags */

    unsigned const check = in[7];
    if (check != XZ_CHECK_NONE && check != XZ_CHECK_CRC32 && check != XZ_CHECK_CRC64)
        return UF2_ERROR(parameter_unsupported);    /* SHA-256, or a type the spec reserves */
    size_t const checkSize = (size_t)XZ_checkSize(check);

    XZ_record *records = NULL;
    size_t nRecords = 0, capRecords = 0;
    size_t err = 0;
    size_t p = XZ_STREAM_HEADER_SIZE;
    size_t op = 0;

#define XZ_FAIL(code) do { err = UF2_ERROR(code); goto fail; } while (0)

#ifndef UF2_SINGLETHREAD
    if (dctx->blocks != NULL) {
        size_t const r = UF2_decompressXzBlocksMt(dctx, out, outCapacity, in, inSize, check,
            &p, &op, &records, &nRecords, &capRecords);
        if (UF2_isError(r)) {
            err = r;
            goto fail;
        }
        if (r == 0)
            goto index;
    }
#endif

    /* ---- blocks, until the Index Indicator ---- */
    for (;;) {
        if (p >= inSize)
            XZ_FAIL(srcSize_wrong);
        if (in[p] == 0x00)
            break;

        XZ_blockHeader h;
        err = XZ_parseBlockHeader(in + p, inSize - p, &h);
        if (UF2_isError(err))
            goto fail;

        size_t const d = p + h.headerSize;
        size_t avail = inSize - d;
        if (h.flags & XZ_HAS_CSIZE) {
            if (h.cSize > avail)
                XZ_FAIL(srcSize_wrong);
            avail = (size_t)h.cSize;
        }
        if ((h.flags & XZ_HAS_USIZE) && h.uSize > outCapacity - op)
            XZ_FAIL(dstSize_tooSmall);

        size_t used = 0;
        size_t const dSize = UF2_decompressLzma2(dctx, h.prop, out + op, outCapacity - op, in + d, avail, &used);
        if (UF2_isError(dSize)) {
            err = dSize;
            goto fail;
        }
        if ((h.flags & XZ_HAS_CSIZE) && used != h.cSize)
            XZ_FAIL(corruption_detected);
        if ((h.flags & XZ_HAS_USIZE) && dSize != h.uSize)
            XZ_FAIL(corruption_detected);

        size_t e = d + used;
        size_t const padding = (4 - (used & 3)) & 3;
        if (padding + checkSize > inSize - e)
            XZ_FAIL(srcSize_wrong);
        for (size_t k = 0; k < padding; ++k)
            if (in[e + k] != 0)
                XZ_FAIL(corruption_detected);       /* Block Padding must be zero */
        e += padding;

        err = XZ_verifyCheck(check, in + e, out + op, dSize);
        if (UF2_isError(err))
            goto fail;
        err = XZ_addRecord(&records, &nRecords, &capRecords, h.headerSize + used + checkSize, dSize);
        if (UF2_isError(err))
            goto fail;

        op += dSize;
        p = e + checkSize;
    }

#ifndef UF2_SINGLETHREAD
index:
#endif
    /* ---- Index: it must describe exactly the blocks just decoded ---- */
    {
        size_t const indexStart = p;
        size_t q = p + 1, n;
        U64 count;
        if ((n = XZ_vliDecode(in + q, inSize - q, &count)) == 0 || count != nRecords)
            XZ_FAIL(corruption_detected);
        q += n;
        for (size_t i = 0; i < nRecords; ++i) {
            U64 unpadded, uncompressed;
            if ((n = XZ_vliDecode(in + q, inSize - q, &unpadded)) == 0 || unpadded != records[i].unpadded)
                XZ_FAIL(corruption_detected);
            q += n;
            if ((n = XZ_vliDecode(in + q, inSize - q, &uncompressed)) == 0 || uncompressed != records[i].uncompressed)
                XZ_FAIL(corruption_detected);
            q += n;
        }
        while ((q - indexStart) & 3) {
            if (q >= inSize || in[q] != 0)
                XZ_FAIL(corruption_detected);       /* Index Padding must be zero */
            ++q;
        }
        if (inSize - q < 4 + XZ_STREAM_FOOTER_SIZE)
            XZ_FAIL(srcSize_wrong);
        if (MEM_readLE32(in + q) != XZ_crc32(0, in + indexStart, q - indexStart))
            XZ_FAIL(corruption_detected);
        size_t const indexSize = q + 4 - indexStart;

        /* ---- Stream Footer ---- */
        const BYTE *const f = in + q + 4;
        if (MEM_readLE32(f) != XZ_crc32(0, f + 4, 6)
            || ((size_t)MEM_readLE32(f + 4) + 1) * 4 != indexSize
            || f[8] != in[6] || f[9] != in[7]
            || f[10] != 'Y' || f[11] != 'Z')
            XZ_FAIL(corruption_detected);

        *inConsumed = (size_t)(f + XZ_STREAM_FOOTER_SIZE - in);
    }
    free(records);
    return op;

fail:
    free(records);
    return err;
#undef XZ_FAIL
}

/* One or more .xz Streams, as xz reads them: concatenated, optionally separated
 * by Stream Padding (zero bytes in multiples of four). Anything else trailing is
 * an error, never silently ignored. */
static size_t UF2_decompressXz(UF2_DCtx* dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    const BYTE *const in = (const BYTE*)src;
    BYTE *const out = (BYTE*)dst;
    size_t ip = 0, op = 0;
    for (;;) {
        size_t used = 0;
        size_t const r = UF2_decompressXzStream(dctx, out + op, dstCapacity - op, in + ip, srcSize - ip, &used);
        if (UF2_isError(r))
            return r;
        op += r;
        ip += used;
        size_t pad = 0;
        while (ip + pad < srcSize && in[ip + pad] == 0)
            ++pad;
        if (pad & 3)
            return UF2_ERROR(corruption_detected);
        ip += pad;
        if (ip == srcSize)
            return op;
        if (!XZ_isXz(in + ip, srcSize - ip))
            return UF2_ERROR(corruption_detected);
    }
}

UF2LIB_API size_t UF2LIB_CALL UF2_decompressDCtx(UF2_DCtx* dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    BYTE prop = dctx->lzma2prop;
    const BYTE *srcBuf = src;

    if (prop == LZMA2_PROP_UNINITIALIZED) {
        /* 0xFD opens every .xz file and can never be a native property byte, so the
         * format is recognised without any ambiguity. */
        if (XZ_isXz(src, srcSize))
            return UF2_decompressXz(dctx, dst, dstCapacity, src, srcSize);
        if (srcSize == 0)
            return UF2_ERROR(srcSize_wrong);
        prop = *(const BYTE*)src;
        ++srcBuf;
        --srcSize;
    }

#ifndef NO_XXHASH
    BYTE const doHash = prop >> UF2_PROP_HASH_BIT;
#endif
    prop &= UF2_LZMA_PROP_MASK;

    DEBUGLOG(4, "UF2_decompressDCtx : dict prop 0x%X, do hash %u", prop, doHash);

    size_t srcPos = 0;
    size_t const dicPos = UF2_decompressLzma2(dctx, prop, dst, dstCapacity, srcBuf, srcSize, &srcPos);
    if (UF2_isError(dicPos))
        return dicPos;

#ifndef NO_XXHASH
    if (doHash) {
        XXH32_canonical_t canonical;
        U32 hash;

        DEBUGLOG(4, "Checking hash");

        if (srcSize - srcPos < XXHASH_SIZEOF)
            return UF2_ERROR(srcSize_wrong);

        memcpy(&canonical, srcBuf + srcPos, XXHASH_SIZEOF);
        hash = XXH32_hashFromCanonical(&canonical);
        if (hash != XXH32(dst, dicPos, 0))
            return UF2_ERROR(checksum_wrong);
    }
#endif
    return dicPos;
}

/*===== Streaming decompression functions =====*/

typedef enum
{
    UF2DEC_STAGE_INIT,
    UF2DEC_STAGE_DECOMP,
#ifndef UF2_SINGLETHREAD
    UF2DEC_STAGE_MT_WRITE,
#endif
    UF2DEC_STAGE_HASH,
    UF2DEC_STAGE_FINISHED
} UF2_decStage;

#ifndef UF2_SINGLETHREAD

typedef struct UF2_decInbuf_s UF2_decInbuf;

struct UF2_decInbuf_s
{
    UF2_decInbuf *next;
    size_t length;
    BYTE inBuf[1];
};

typedef struct
{
    UF2_decInbuf *first;
    UF2_decInbuf *last;
    size_t startPos;
    size_t endPos;
    size_t unpackSize;
} UF2_decBlock;

typedef struct
{
    LZMA2_DCtx dec;
    UF2_decBlock inBlock;
    BYTE *outBuf;
    size_t bufSize;
    size_t res;
} UF2_decJob;

typedef struct
{
    UF2POOL_ctx* factory;
    UF2_decInbuf *head;
    UF2_decInbuf *cur;
    size_t curPos;
    size_t numThreads;
    size_t maxThreads;
    size_t srcThread;
    size_t srcPos;
    size_t memTotal;
    size_t memLimit;
    BYTE isFinal;
    BYTE failState;
    BYTE canceled;
    BYTE prop;
#ifndef NO_XXHASH
    XXH32_canonical_t hash;
#endif
    UF2_decJob threads[1];
} UF2_decMt;

#endif /* !defined UF2_SINGLETHREAD */

#define LZMA_OVERLAP_SIZE (LZMA_REQUIRED_INPUT_MAX * 2)

struct UF2_DStream_s
{
#ifndef UF2_SINGLETHREAD
    UF2_decMt *decmt;
    UF2POOL_ctx* decompressThread;
#endif
    LZMA2_DCtx dec;
    UF2_outBuffer* asyncOutput;
    UF2_inBuffer* asyncInput;
    size_t asyncRes;
    U64 streamTotal;
    size_t overlapSize;
    UF2_atomic progress;
    unsigned timeout;
#ifndef NO_XXHASH
    XXH32_state_t *xxh;
    XXH32_canonical_t xxhIn;
    size_t xxhPos;
#endif
    UF2_decStage stage;
    BYTE doHash;
    BYTE loopCount;
    BYTE wait;
    BYTE overlap[LZMA_OVERLAP_SIZE];
};

static size_t UF2_decompressInput(UF2_DStream* fds, UF2_outBuffer* output, UF2_inBuffer* input)
{
    if (fds->stage == UF2DEC_STAGE_DECOMP) {
        size_t destSize = output->size - output->pos;
        size_t srcSize = input->size - input->pos;
        size_t const res = LZMA2_decodeToBuf(&fds->dec, (BYTE*)output->dst + output->pos, &destSize, (const BYTE*)input->src + input->pos, &srcSize, LZMA_FINISH_ANY);

        DEBUGLOG(5, "Decoded %u bytes", (U32)destSize);

#ifndef NO_XXHASH
        if (fds->doHash)
            XXH32_update(fds->xxh, (BYTE*)output->dst + output->pos, destSize);
#endif
        UF2_atomic_add(fds->progress, (long)destSize);

        output->pos += destSize;
        input->pos += srcSize;

        if (UF2_isError(res))
            return res;
        if (res == LZMA_STATUS_FINISHED) {
            DEBUGLOG(4, "Found end mark");
            fds->stage = fds->doHash ? UF2DEC_STAGE_HASH : UF2DEC_STAGE_FINISHED;
        }
    }
    return UF2_error_no_error;
}

static size_t UF2_decompressOverlappedInput(UF2_DStream* fds, UF2_outBuffer* output, UF2_inBuffer* input)
{
    if (fds->overlapSize != 0) {
        size_t toRead = MIN(input->size - input->pos, LZMA_OVERLAP_SIZE - fds->overlapSize);
        memcpy(fds->overlap + fds->overlapSize, (BYTE*)input->src + input->pos, toRead);
        UF2_inBuffer temp = { fds->overlap, fds->overlapSize + toRead, 0 };
        CHECK_F(UF2_decompressInput(fds, output, &temp));
        if (temp.pos >= fds->overlapSize) {
            input->pos += temp.pos - fds->overlapSize;
            fds->overlapSize = 0;
        }
        else {
            fds->overlapSize -= temp.pos;
            memmove(fds->overlap, fds->overlap + temp.pos, fds->overlapSize);
        }
    }
    if(input->pos == input->size)
        return UF2_error_no_error;

    if(fds->overlapSize == 0)
        CHECK_F(UF2_decompressInput(fds, output, input));

    size_t toRead = input->size - input->pos;
    /* More input needed if not finished, output not full and input is below minimum.
     * Safe to take all input because stream will be beyond decomp stage if the terminator is present. */
    if (fds->stage == UF2DEC_STAGE_DECOMP && output->pos < output->size && toRead <= LZMA_REQUIRED_INPUT_MAX) {
        toRead = MIN(toRead, LZMA_OVERLAP_SIZE - fds->overlapSize);
        memcpy(fds->overlap + fds->overlapSize, (BYTE*)input->src + input->pos, toRead);
        input->pos += toRead;
        fds->overlapSize += toRead;
    }
    return UF2_error_no_error;
}

#ifndef UF2_SINGLETHREAD

/* Free buffer nodes from node to the end, except keep */
static void LZMA2_freeInbufNodeChain(UF2_decMt *const decmt, UF2_decInbuf *node, UF2_decInbuf *const keep)
{
    while (node) {
        UF2_decInbuf *const next = node->next;
        if (node != keep) {
            decmt->memTotal -= sizeof(UF2_decInbuf) + LZMA2_MT_INPUT_SIZE - 1;
            UF2_free(node);
        }
        else {
            node->next = NULL;
        }
        node = next;
    }
}

/* Free all buffer nodes except the head */
static void LZMA2_freeExtraInbufNodes(UF2_decMt *const decmt)
{
    LZMA2_freeInbufNodeChain(decmt, decmt->head->next, NULL);
    decmt->head->next = NULL;
    decmt->head->length = 0;
}

static void UF2_freeOutputBuffers(UF2_decMt *const decmt)
{
    for (size_t thread = 0; thread < decmt->maxThreads; ++thread)
        if(decmt->threads[thread].outBuf != NULL) {
            decmt->memTotal -= decmt->threads[thread].bufSize;
            UF2_large_free(decmt->threads[thread].outBuf);
            decmt->threads[thread].outBuf = NULL;
        }
    decmt->numThreads = 0;
}

static void UF2_lzma2DecMt_cleanup(UF2_decMt *const decmt)
{
    if (decmt) {
        UF2_freeOutputBuffers(decmt);
        LZMA2_freeExtraInbufNodes(decmt);
    }
}

static void UF2_lzma2DecMt_free(UF2_decMt *const decmt)
{
    if (decmt) {
        UF2_freeOutputBuffers(decmt);
        LZMA2_freeInbufNodeChain(decmt, decmt->head, NULL);
        UF2POOL_free(decmt->factory);
        UF2_free(decmt);
    }
}

static void UF2_lzma2DecMt_init(UF2_decMt *const decmt)
{
    if (decmt) {
        decmt->cur = NULL;
        decmt->failState = 0;
        decmt->isFinal = 0;
        decmt->canceled = 0;
        decmt->memTotal = 0;
        UF2_freeOutputBuffers(decmt);
        LZMA2_freeExtraInbufNodes(decmt);
        decmt->threads[0].inBlock.first = decmt->head;
        decmt->threads[0].inBlock.last = decmt->head;
        decmt->threads[0].inBlock.startPos = 0;
        decmt->threads[0].inBlock.endPos = 0;
        decmt->threads[0].inBlock.unpackSize = 0;
    }
}

static int UF2_lzma2DecMt_initProp(UF2_decMt *const decmt, BYTE prop)
{
    decmt->prop = prop;
    size_t const dictSize = LZMA2_getDictSizeFromProp(prop);
    /* Minimum memory is for two threads, one dict size per thread plus a minimal amount of
     * compressed data for each. Compression to < 1/6 is uncommon. */
    if (decmt->memLimit < (dictSize + dictSize / 6U) * 2U) {
        DEBUGLOG(3, "Using ST decompression due to dict size %u, memory limit %u", (unsigned)dictSize, (unsigned)decmt->memLimit);
        decmt->failState = 1;
        return 1;
    }
    return 0;
}

static UF2_decInbuf * UF2_createInbufNode(UF2_decMt *const decmt, UF2_decInbuf *const prev)
{
    decmt->memTotal += sizeof(UF2_decInbuf) + LZMA2_MT_INPUT_SIZE - 1;
    if (decmt->memTotal > decmt->memLimit)
        return NULL;

    UF2_decInbuf *const node = UF2_malloc(sizeof(UF2_decInbuf) + LZMA2_MT_INPUT_SIZE - 1);
    if (node == NULL)
        return NULL;

    node->next = NULL;
    node->length = 0;
    if (prev) {
        /* Node buffers overlap by LZMA_REQUIRED_INPUT_MAX */
        memcpy(node->inBuf, prev->inBuf + prev->length - LZMA_REQUIRED_INPUT_MAX, LZMA_REQUIRED_INPUT_MAX);
        prev->next = node;
        node->length = LZMA_REQUIRED_INPUT_MAX;
    }
    return node;
}

static UF2_decMt *UF2_lzma2DecMt_create(unsigned maxThreads)
{
    maxThreads += !maxThreads;

    UF2_decMt *const decmt = UF2_malloc(sizeof(UF2_decMt) + (maxThreads - 1) * sizeof(UF2_decJob));
    if (decmt == NULL)
        return NULL;

    decmt->memTotal = 0;
    decmt->memLimit = (size_t)1 << 29;
    decmt->maxThreads = 0;

    /* The head always exists and is only freed on deallocation */
    decmt->head = UF2_createInbufNode(decmt, NULL);
    if (decmt->head == NULL) {
        UF2_free(decmt);
        return NULL;
    }

    decmt->factory = UF2POOL_create(maxThreads - 1);

    if (maxThreads > 1 && decmt->factory == NULL) {
        UF2_lzma2DecMt_free(decmt);
        return NULL;
    }
    decmt->numThreads = 0;
    decmt->maxThreads = maxThreads;

    for (size_t n = 0; n < maxThreads; ++n) {
        decmt->threads[n].outBuf = NULL;
        LZMA_constructDCtx(&decmt->threads[n].dec);
    }
    UF2_lzma2DecMt_init(decmt);

    return decmt;
}

/* Read chunk headers and advance inBlock->endPos to the next chunk
 * until it points beyond the available data.
 * Add the size of each chunk to inBlock->unpackSize
 */
static LZMA2_parseRes UF2_parseMt(UF2_decBlock* const inBlock)
{
    LZMA2_parseRes res = CHUNK_MORE_DATA;
    UF2_decInbuf *const cur = inBlock->last;
    if (cur == NULL)
        return res;

    int first = inBlock->unpackSize == 0;

    while (inBlock->endPos < cur->length) {
        LZMA2_chunk inf;
        res = LZMA2_parseInput(cur->inBuf, inBlock->endPos, cur->length - inBlock->endPos, &inf);
        if (first && res == CHUNK_DICT_RESET)
            res = CHUNK_CONTINUE;
        if (res != CHUNK_CONTINUE)
            break;

        inBlock->endPos += inf.pack_size;
        inBlock->unpackSize += inf.unpack_size;

        first = 0;
    }
    /* Skip the 1-byte end marker if found */
    inBlock->endPos += (res == CHUNK_FINAL);
    return res;
}

/* Decompress an entire block starting with a dict reset and ending with
 * the last chunk before the next dict reset, or the terminator.
 * The input is a chain of buffers.
 */
static size_t UF2_decompressBlockMt(UF2_DStream* const fds, size_t const thread)
{
    UF2_decMt *const decmt = fds->decmt;
    UF2_decJob *const ti = &decmt->threads[thread];
    LZMA2_DCtx *const dec = &ti->dec;

    DEBUGLOG(4, "Thread %u: decoding block of size %u", (unsigned)thread, (unsigned)ti->bufSize);

    CHECK_F(LZMA2_initDecoder(dec, decmt->prop, ti->outBuf, ti->bufSize));

    /* Input buffer node containing the starting chunk. If thread > 0 this is usually
     * the last input buffer node of the previous thread. */
    UF2_decInbuf *cur = ti->inBlock.first;
    /* Position of the starting chunk. */
    size_t inPos = ti->inBlock.startPos;
    /* Flag to indicate this block ends with the terminator */
    BYTE const last = decmt->isFinal && (thread == decmt->numThreads - 1);

    while (!decmt->canceled) {
        size_t srcSize = cur->length - inPos;
        size_t const dicPos = dec->dic_pos;

        size_t const res = LZMA2_decodeToDic(dec,
            ti->bufSize,
            cur->inBuf + inPos, &srcSize,
            last && cur == ti->inBlock.last ? LZMA_FINISH_END : LZMA_FINISH_ANY);

        CHECK_F(res);

        UF2_atomic_add(fds->progress, (long)(dec->dic_pos - dicPos));

        if (res == LZMA_STATUS_FINISHED)
            DEBUGLOG(4, "Found end mark");

        if (cur == ti->inBlock.last)
            break;

        /* Advance the position and switch to the next input buffer in the chain if necessary */
        inPos += srcSize;
        if (inPos + LZMA_REQUIRED_INPUT_MAX >= cur->length) {
            inPos -= cur->length - LZMA_REQUIRED_INPUT_MAX;
            cur = cur->next;
        }
    }

    if (decmt->canceled)
        return UF2_ERROR(canceled);

    return UF2_error_no_error;
}

/*
 * Write the data from the output buffer of each thread.
 */
static size_t UF2_writeStreamBlocks(UF2_DStream* const fds, UF2_outBuffer* const output)
{
    UF2_decMt *const decmt = fds->decmt;

    for (; decmt->srcThread < fds->decmt->numThreads; ++decmt->srcThread) {
        UF2_decJob *thread = decmt->threads + decmt->srcThread;
        size_t to_write = MIN(thread->bufSize - decmt->srcPos, output->size - output->pos);
        memcpy((BYTE*)output->dst + output->pos, thread->outBuf + decmt->srcPos, to_write);

#ifndef NO_XXHASH
        if (fds->doHash)
            XXH32_update(fds->xxh, (BYTE*)output->dst + output->pos, to_write);
#endif
        decmt->srcPos += to_write;
        output->pos += to_write;

        if (decmt->srcPos < thread->bufSize)
            break;

        decmt->srcPos = 0;
    }
    if (decmt->srcThread < fds->decmt->numThreads)
        return 0;

    UF2_freeOutputBuffers(fds->decmt);
    fds->decmt->numThreads = 0;

    return 1;
}

/* UF2_decompressBlock() : UF2POOL_function type */
static void UF2_decompressBlock(void* const jobDescription, ptrdiff_t const n)
{
    UF2_DStream* const fds = (UF2_DStream*)jobDescription;
    fds->decmt->threads[n].res = UF2_decompressBlockMt(fds, n);
}

static size_t UF2_decompressBlocksMt(UF2_DStream* const fds)
{
    /* Set the threads to work on the blocks */
    UF2_decMt * const decmt = fds->decmt;
    UF2POOL_addRange(decmt->factory, UF2_decompressBlock, fds, 1, decmt->numThreads);

    /* Do block 0 in the main thread */
    decmt->threads[0].res = UF2_decompressBlockMt(fds, 0);
    UF2POOL_waitAll(fds->decmt->factory, 0);

    /* Free all input buffers except the last */
    UF2_decInbuf *const keep = decmt->threads[decmt->numThreads - 1].inBlock.last;
    LZMA2_freeInbufNodeChain(decmt, decmt->head, keep);
    /* The last becomes the new head */
    decmt->head = keep;
    decmt->threads[0].inBlock.first = keep;
    decmt->threads[0].inBlock.last = keep;
    /* Initialize the start and end to the next chunk */
    decmt->threads[0].inBlock.endPos = decmt->threads[decmt->numThreads - 1].inBlock.endPos;
    decmt->threads[0].inBlock.startPos = decmt->threads[0].inBlock.endPos;
    decmt->threads[0].inBlock.unpackSize = 0;

    for (size_t thread = 0; thread < decmt->numThreads; ++thread)
        if (UF2_isError(decmt->threads[thread].res))
            return decmt->threads[thread].res;

    decmt->srcThread = 0;
    decmt->srcPos = 0;

    return UF2_error_no_error;
}

static size_t UF2_handleFinalChunkMt(UF2_decMt *const decmt, size_t res)
{
    UF2_decBlock *inBlock = &decmt->threads[decmt->numThreads].inBlock;

    UF2_decJob * const done = decmt->threads + decmt->numThreads;
    ++decmt->numThreads;

    done->bufSize = done->inBlock.unpackSize;
    decmt->memTotal += done->bufSize;
    if (decmt->memTotal > decmt->memLimit)
        return UF2_ERROR(memory_allocation);

    /* Decompressed data will be stored in outBuf */
    done->outBuf = UF2_large_malloc(done->bufSize);
    if (done->outBuf == NULL)
        return UF2_ERROR(memory_allocation);

    decmt->isFinal = (res == CHUNK_FINAL);

    if (decmt->numThreads == decmt->maxThreads || decmt->isFinal)
        return 1;

    /* Set up the start of the next series of chunks. The first buffer is the last of the those already loaded. */
    inBlock = &decmt->threads[decmt->numThreads].inBlock;
    inBlock->first = done->inBlock.last;
    inBlock->last = inBlock->first;
    inBlock->endPos = done->inBlock.endPos;
    inBlock->startPos = inBlock->endPos;
    inBlock->unpackSize = 0;

    return 0;
}

/* Read input into the buffer chain, adding new nodes when necessary. 
 * The chunks in each buffer are parsed before a new buffer is allocated.
 * No new buffers will be allocated after the terminator is encountered.
 * Returns 1 if the terminator was found or enough work exists for all threads,
 * 0 if input is empty,
 * or UF2_error_corruption_detected, or UF2_error_memory_allocation.
 * The memory limit is enforced by returning UF2_error_memory_allocation.
 */
static size_t UF2_loadInputMt(UF2_decMt *const decmt, UF2_inBuffer* const input)
{
    UF2_decBlock *inBlock = &decmt->threads[decmt->numThreads].inBlock;
    LZMA2_parseRes res = CHUNK_CONTINUE;
    /* Continue while input is available or the parse pos is not beyond the end */
    while (input->pos < input->size || inBlock->endPos < inBlock->last->length) {
        if (inBlock->endPos < inBlock->last->length) {
            res = UF2_parseMt(inBlock);
            if (res == CHUNK_ERROR)
                return UF2_ERROR(corruption_detected);

            if (res == CHUNK_DICT_RESET || res == CHUNK_FINAL) {
                /* We have a complete series of chunks starting from a dict reset and
                 * ending with another reset or the terminator. Set up the thread job. */
                size_t end = UF2_handleFinalChunkMt(decmt, res);

                /* end is nonzero if memory limit hit or ready to decode */
                if (end != 0) {
                    inBlock = &decmt->threads[decmt->numThreads - 1].inBlock;
                    /* rewind input in case data beyond terminator was read. Required for xxhash and container formats */
                    size_t back = MIN(input->pos, inBlock->last->length - inBlock->endPos);
                    input->pos -= back;
                    inBlock->last->length -= back;
                    return end;
                }
                inBlock = &decmt->threads[decmt->numThreads].inBlock;
            }
        }
        if (inBlock->last->length >= LZMA2_MT_INPUT_SIZE && inBlock->endPos + LZMA_REQUIRED_INPUT_MAX >= inBlock->last->length) {
            /* Create a new buffer if endPos is within the overlap region. The function copies the overlap. */
            UF2_decInbuf *const next = UF2_createInbufNode(decmt, inBlock->last);
            if (next == NULL) {
                if (inBlock->endPos < inBlock->last->length) {
                    size_t back = MIN(input->pos, inBlock->last->length - inBlock->endPos);
                    input->pos -= back;
                    inBlock->last->length -= back;
                }
                return UF2_ERROR(memory_allocation);
            }
            inBlock->last = next;
            inBlock->endPos -= LZMA2_MT_INPUT_SIZE - LZMA_REQUIRED_INPUT_MAX;
        }
        /* Read as much input as possible */
        size_t toread = MIN(input->size - input->pos, LZMA2_MT_INPUT_SIZE - inBlock->last->length);
        memcpy(inBlock->last->inBuf + inBlock->last->length, (BYTE*)input->src + input->pos, toread);
        inBlock->last->length += toread;
        input->pos += toread;

        /* Do not continue if we have an incomplete chunk header */
        if (res == CHUNK_MORE_DATA && toread == 0)
            break;
    }
    return 0;
}

/* Handle MT buffer allocation failure.
 * Decompress input from the MT buffer chain
 * until it is possible to switch to the caller's input buffer
 */
static size_t UF2_decompressFailedMt(UF2_DStream* const fds, UF2_outBuffer* const output, UF2_inBuffer* const input)
{
    UF2_decMt *const decmt = fds->decmt;

    if(decmt->head->length == 0)
        return UF2_decompressOverlappedInput(fds, output, input);

    if (!decmt->failState) {
        /* On first call of this function, free any output buffers already allocated,
         * and set up the read position in the input buffer chain. The main thread's decoder needs initialization too. */
        DEBUGLOG(3, "Switching to ST decompression. Memory: %u, limit %u", (unsigned)decmt->memTotal, (unsigned)decmt->memLimit);

        UF2_freeOutputBuffers(decmt);

        decmt->cur = decmt->threads[0].inBlock.first;
        decmt->curPos = decmt->threads[0].inBlock.startPos;

        decmt->failState = 1;

        CHECK_F(LZMA2_initDecoder(&fds->dec, decmt->prop, NULL, 0));
    }
    UF2_decInbuf *const cur = decmt->cur;

    UF2_inBuffer temp;
    temp.src = cur->inBuf;
    temp.pos = decmt->curPos;
    temp.size = cur->length;

    CHECK_F(UF2_decompressInput(fds, output, &temp));

    decmt->curPos = temp.pos;

    if (temp.pos + LZMA_REQUIRED_INPUT_MAX >= temp.size) {
        if (cur->next == NULL) {
            /* The last buffer in the chain */
            fds->overlapSize = temp.size - temp.pos;
            memcpy(fds->overlap, cur->inBuf + temp.pos, fds->overlapSize);
            decmt->cur = NULL;
            LZMA2_freeExtraInbufNodes(decmt);
        }
        else {
            decmt->curPos -= cur->length - LZMA_REQUIRED_INPUT_MAX;
            decmt->cur = cur->next;
        }
    }

    return UF2_error_no_error;
}

static size_t UF2_decompressStreamMt(UF2_DStream* const fds, UF2_outBuffer* const output, UF2_inBuffer* const input)
{
    UF2_decMt *const decmt = fds->decmt;

    /* failState is set if the memory limit was hit or allocation failed */
    if(decmt->failState)
        return UF2_decompressFailedMt(fds, output, input);

    if (fds->stage == UF2DEC_STAGE_DECOMP) {
        /* Allocate and fill the input buffer chain */
        size_t const res = UF2_loadInputMt(decmt, input);

        /* Failover if allocation failed */
        if (UF2_getErrorCode(res) == UF2_error_memory_allocation)
            return UF2_decompressFailedMt(fds, output, input);
        CHECK_F(res);

        /* res > 0 means all threads have input or the terminator was encountered */
        if (res > 0) {
            CHECK_F(UF2_decompressBlocksMt(fds));
            fds->stage = UF2DEC_STAGE_MT_WRITE;
        }
    }
    if (fds->stage == UF2DEC_STAGE_MT_WRITE) {
        if (UF2_writeStreamBlocks(fds, output))
            fds->stage = decmt->isFinal ? (fds->doHash ? UF2DEC_STAGE_HASH : UF2DEC_STAGE_FINISHED)
                : UF2DEC_STAGE_DECOMP;
    }
    return fds->stage != UF2DEC_STAGE_FINISHED;
}

UF2LIB_API void UF2LIB_CALL UF2_setDStreamMemoryLimitMt(UF2_DStream * fds, size_t limit)
{
    if (fds->decmt != NULL)
        fds->decmt->memLimit = limit;
}

UF2LIB_API size_t UF2LIB_CALL UF2_setDStreamTimeout(UF2_DStream * fds, unsigned timeout)
{
    /* decompressThread is only used if a timeout is specified */
    if (timeout != 0) {
        if (fds->decompressThread == NULL) {
            fds->decompressThread = UF2POOL_create(1);
            if (fds->decompressThread == NULL)
                return UF2_ERROR(memory_allocation);
        }
    }
    else if (!fds->wait) {
        /* Only free the thread if decompression not underway */
        UF2POOL_free(fds->decompressThread);
        fds->decompressThread = NULL;
    }
    fds->timeout = timeout;
    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_waitDStream(UF2_DStream * fds)
{
    if (UF2POOL_waitAll(fds->decompressThread, fds->timeout) != 0)
        return UF2_ERROR(timedOut);
    /* decompressThread writes the result into asyncRes before sleeping */
    return fds->asyncRes;
}

UF2LIB_API void UF2LIB_CALL UF2_cancelDStream(UF2_DStream *fds)
{
    if (fds->decompressThread != NULL) {
        fds->decmt->canceled = 1;

        UF2POOL_waitAll(fds->decompressThread, 0);

        fds->decmt->canceled = 0;
    }
    UF2_lzma2DecMt_cleanup(fds->decmt);
}

static inline void UF2_createDStream_threads(UF2_DStream *fds, unsigned nbThreads)
{
    fds->decompressThread = NULL;
    fds->decmt = (nbThreads > 1) ? UF2_lzma2DecMt_create(nbThreads) : NULL;
}

static inline void UF2_freeDStream_threads(UF2_DStream* fds)
{
    UF2POOL_free(fds->decompressThread);
    UF2_lzma2DecMt_free(fds->decmt);
}

#else /* UF2_SINGLETHREAD */

UF2LIB_API void UF2LIB_CALL UF2_setDStreamMemoryLimitMt(UF2_DStream * fds, size_t limit)
{
    (void)fds;
    (void)limit;
}

UF2LIB_API size_t UF2LIB_CALL UF2_setDStreamTimeout(UF2_DStream * fds, unsigned timeout)
{
    (void)fds;
    (void)timeout;
    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_waitDStream(UF2_DStream * fds)
{
    return fds->asyncRes;
}

UF2LIB_API void UF2LIB_CALL UF2_cancelDStream(UF2_DStream *fds)
{
    (void)fds;
}

static inline void UF2_createDStream_threads(UF2_DStream *fds, unsigned nbThreads)
{
    (void)fds;
    (void)nbThreads;
}

static inline void UF2_freeDStream_threads(UF2_DStream* fds)
{
    (void)fds;
}

#endif /* !defined UF2_SINGLETHREAD */

UF2LIB_API UF2_DStream* UF2LIB_CALL UF2_createDStream(void)
{
    return UF2_createDStreamMt(1);
}

static void UF2_resetDStream(UF2_DStream *fds)
{
    fds->stage = UF2DEC_STAGE_INIT;
    fds->asyncRes = 0;
    fds->streamTotal = 0;
    fds->overlapSize = 0;
    fds->progress = 0;
#ifndef NO_XXHASH
    fds->xxhPos = 0;
#endif
    fds->loopCount = 0;
    fds->wait = 0;
}

UF2LIB_API UF2_DStream *UF2LIB_CALL UF2_createDStreamMt(unsigned nbThreads)
{
    UF2_DStream* const fds = UF2_malloc(sizeof(UF2_DStream));
    DEBUGLOG(3, "UF2_createDStream");

    if (fds != NULL) {
        LZMA_constructDCtx(&fds->dec);

        nbThreads = UF2_checkNbThreads(nbThreads);

        UF2_resetDStream(fds);
        fds->timeout = 0;

        UF2_createDStream_threads(fds, nbThreads);

#ifndef NO_XXHASH
        fds->xxh = NULL;
#endif
        fds->doHash = 0;
    }

    return fds;
}

UF2LIB_API size_t UF2LIB_CALL UF2_freeDStream(UF2_DStream* fds)
{
    if (fds != NULL) {
        DEBUGLOG(3, "UF2_freeDStream");
        LZMA_destructDCtx(&fds->dec);
        UF2_freeDStream_threads(fds);
#ifndef NO_XXHASH
        XXH32_freeState(fds->xxh);
#endif
        UF2_free(fds);
    }
    return 0;
}

UF2LIB_API size_t UF2LIB_CALL UF2_initDStream(UF2_DStream* fds)
{
    DEBUGLOG(4, "UF2_initDStream");

    if (fds->wait)
        return UF2_ERROR(stage_wrong);

    UF2_resetDStream(fds);

#ifndef UF2_SINGLETHREAD
    UF2_lzma2DecMt_init(fds->decmt);
#endif
    return UF2_error_no_error;
}

UF2LIB_API unsigned long long UF2LIB_CALL UF2_getDStreamProgress(const UF2_DStream * fds)
{
    return fds->streamTotal + fds->progress;
}

static size_t UF2_initDStream_prop(UF2_DStream* const fds, BYTE prop)
{
    fds->doHash = prop >> UF2_PROP_HASH_BIT;
    prop &= UF2_LZMA_PROP_MASK;

    /* If MT decoding is enabled and the dict is not too large, decoder init will occur elsewhere */
#ifndef UF2_SINGLETHREAD
    if (fds->decmt == NULL || UF2_lzma2DecMt_initProp(fds->decmt, prop))
#endif
        CHECK_F(LZMA2_initDecoder(&fds->dec, prop, NULL, 0));

#ifndef NO_XXHASH
    if (fds->doHash) {
        if (fds->xxh == NULL) {
            DEBUGLOG(3, "Creating hash state");
            fds->xxh = XXH32_createState();
            if (fds->xxh == NULL)
                return UF2_ERROR(memory_allocation);
        }
        XXH32_reset(fds->xxh, 0);
    }
#endif
    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_initDStream_withProp(UF2_DStream* fds, unsigned char prop)
{
    CHECK_F(UF2_initDStream(fds));
    CHECK_F(UF2_initDStream_prop(fds, prop));
    fds->stage = UF2DEC_STAGE_DECOMP;
    return UF2_error_no_error;
}

static size_t UF2_decompressStream_blocking(UF2_DStream* fds, UF2_outBuffer* output, UF2_inBuffer* input)
{
#ifndef UF2_SINGLETHREAD
    UF2_decMt *const decmt = fds->decmt;
#endif
    size_t const prevOut = output->pos;
    size_t const prevIn = input->pos;

    if (input->pos < input->size
#ifndef UF2_SINGLETHREAD
        || decmt
#endif
        ) {
        if (fds->stage == UF2DEC_STAGE_INIT) {
            BYTE prop = ((const BYTE*)input->src)[input->pos];
            /* .xz is decoded by the one-shot functions only, for now */
            if (prop == XZ_magic[0])
                return UF2_ERROR(parameter_unsupported);
            ++input->pos;
            UF2_initDStream_prop(fds, prop);
            fds->stage = UF2DEC_STAGE_DECOMP;
        }
#ifndef UF2_SINGLETHREAD
        if (decmt) {
            size_t res = UF2_decompressStreamMt(fds, output, input);
            if (UF2_isError(res)) {
                UF2_lzma2DecMt_cleanup(decmt);
                return res;
            }
        }
        else
#endif
        {
            CHECK_F(UF2_decompressOverlappedInput(fds, output, input));
        }
        if (fds->stage == UF2DEC_STAGE_HASH) {
#ifndef NO_XXHASH
#ifndef UF2_SINGLETHREAD
            if (fds->overlapSize != 0) {
                /* Must copy buffered data before using input */
                size_t toRead = MIN(XXHASH_SIZEOF - fds->xxhPos, fds->overlapSize);
                memcpy(fds->xxhIn.digest + fds->xxhPos, fds->overlap, toRead);
                fds->xxhPos += toRead;
                fds->overlapSize = 0;
            }
#endif
            size_t toRead = MIN(XXHASH_SIZEOF - fds->xxhPos, input->size - input->pos);
            memcpy(fds->xxhIn.digest + fds->xxhPos, (BYTE*)input->src + input->pos, toRead);
            input->pos += toRead;
            fds->xxhPos += toRead;
            if (fds->xxhPos == XXHASH_SIZEOF) {
                DEBUGLOG(4, "Checking hash");
                U32 hash = XXH32_hashFromCanonical(&fds->xxhIn);
                if (hash != XXH32_digest(fds->xxh))
                    return UF2_ERROR(checksum_wrong);
                fds->stage = UF2DEC_STAGE_FINISHED;
            }
#else
            fds->stage = UF2DEC_STAGE_FINISHED;
#endif /* NO_XXHASH */
        }
    }
    if (fds->stage != UF2DEC_STAGE_FINISHED && prevOut == output->pos && prevIn == input->pos) {
        /* No progress was made */
        ++fds->loopCount;
        if (fds->loopCount > 2) {
            UF2_cancelDStream(fds);
            return UF2_ERROR(buffer);
        }
    }
    else {
        fds->loopCount = 0;
    }

    if (fds->stage == UF2DEC_STAGE_FINISHED) {
#ifndef UF2_SINGLETHREAD
        UF2_lzma2DecMt_cleanup(decmt);
#endif
        return 0;
    }
    else {
        return 1;
    }
}

/* UF2_decompressStream_async() : UF2POOL_function type */
static void UF2_decompressStream_async(void* const jobDescription, ptrdiff_t const n)
{
    UF2_DStream* const fds = (UF2_DStream*)jobDescription;

    fds->asyncRes = UF2_decompressStream_blocking(fds, fds->asyncOutput, fds->asyncInput);
    fds->wait = 0;

    (void)n;
}

UF2LIB_API size_t UF2LIB_CALL UF2_decompressStream(UF2_DStream* fds, UF2_outBuffer* output, UF2_inBuffer* input)
{
    fds->streamTotal += fds->progress;
    fds->progress = 0;

#ifndef UF2_SINGLETHREAD
    if (fds->decompressThread != NULL) {
        /* Calling UF2_decompressStream() while waiting for decompressThread to fall idle is not allowed */
        if (fds->wait)
            return UF2_ERROR(stage_wrong);

        fds->asyncOutput = output;
        fds->asyncInput = input;
        /* UF2_decompressStream_async will reset fds->wait upon completion */
        fds->wait = 1;

        UF2POOL_add(fds->decompressThread, UF2_decompressStream_async, fds, 0);

        /* Wait for completion or a timeout */
        CHECK_F(UF2_waitDStream(fds));

        /* UF2_decompressStream_async() stores result in asyncRes */
        return fds->asyncRes;
    }
    else
#endif
    {
        return UF2_decompressStream_blocking(fds, output, input);
    }
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateDCtxSize(unsigned nbThreads)
{
    nbThreads = UF2_checkNbThreads(nbThreads);
    if (nbThreads > 1)
        return nbThreads * (sizeof(UF2_blockDecMt) + sizeof(UF2_DCtx));

    return sizeof(UF2_DCtx);
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateDStreamSize(size_t dictSize, unsigned nbThreads)
{
    nbThreads = UF2_checkNbThreads(nbThreads);
    if (nbThreads > 1) {
        /* Estimate 50% compression and a block size of 4 * dictSize */
        return nbThreads * sizeof(UF2_DCtx) + (dictSize + dictSize / 2) * 4 * nbThreads;
    }
    return LZMA2_decMemoryUsage(dictSize);
}
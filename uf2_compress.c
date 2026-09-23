/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
* Parts based on zstd_compress.c copyright Yann Collet
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include <string.h>
#include "uf-lzma2.h"
#include "uf2_errors.h"
#include "uf2_internal.h"
#include "platform.h"
#include "mem.h"
#include "util.h"
#include "uf2_compress_internal.h"
#include "uf2_threading.h"
#include "uf2_pool.h"
#include "radix_mf.h"
#include "lzma2_enc.h"
#include "uf2_xz.h"

#define UF2_MAX_LOOPS 10U

/*-=====  Pre-defined compression levels  =====-*/

#define MB *(1U<<20)

#define UF2_MAX_HIGH_CLEVEL 10

#ifdef UF2_XZ_BUILD

#define UF2_CLEVEL_DEFAULT  6
#define UF2_MAX_CLEVEL      9

static const UF2_compressionParameters UF2_defaultCParameters[UF2_MAX_CLEVEL + 1] = {
    { 0,0,0,0,0,0,0,0 },
    { 1 MB, 1, 7, 0, 6, 32, 1, UF2_fast }, /* 1 */
    { 2 MB, 2, 7, 0, 14, 32, 1, UF2_fast }, /* 2 */
    { 2 MB, 2, 7, 0, 14, 40, 1, UF2_opt }, /* 3 */
    { 8 MB, 2, 7, 0, 26, 40, 1, UF2_opt }, /* 4 */
    { 16 MB, 2, 8, 0, 42, 48, 1, UF2_opt }, /* 5 */
    { 16 MB, 2, 9, 1, 42, 48, 1, UF2_ultra }, /* 6 */
    { 32 MB, 2, 10, 1, 50, 64, 1, UF2_ultra }, /* 7 */
    { 64 MB, 2, 11, 2, 62, 96, 1, UF2_ultra }, /* 8 */
    { 128 MB, 2, 12, 3, 90, 128, 1, UF2_ultra }, /* 9 */
};

#elif defined(UF2_7ZIP_BUILD)

#define UF2_CLEVEL_DEFAULT  5
#define UF2_MAX_CLEVEL      9

static const UF2_compressionParameters UF2_defaultCParameters[UF2_MAX_CLEVEL + 1] = {
    { 0,0,0,0,0,0,0,0 },
    { 1 MB, 1, 7, 0, 6, 32, 1, UF2_fast }, /* 1 */
    { 2 MB, 2, 7, 0, 10, 32, 1, UF2_fast }, /* 2 */
    { 2 MB, 2, 7, 0, 10, 32, 1, UF2_opt }, /* 3 */
    { 4 MB, 2, 7, 0, 14, 32, 1, UF2_opt }, /* 4 */
    { 16 MB, 2, 9, 0, 42, 48, 1, UF2_ultra }, /* 5 */
    { 32 MB, 2, 10, 0, 50, 64, 1, UF2_ultra }, /* 6 */
    { 64 MB, 2, 11, 1, 62, 96, 1, UF2_ultra }, /* 7 */
    { 64 MB, 4, 12, 2, 90, 273, 1, UF2_ultra }, /* 8 */
    { 128 MB, 2, 14, 3, 254, 273, 0, UF2_ultra } /* 9 */
};

#else

#define UF2_CLEVEL_DEFAULT   6
#define UF2_MAX_CLEVEL      10

static const UF2_compressionParameters UF2_defaultCParameters[UF2_MAX_CLEVEL + 1] = {
    { 0,0,0,0,0,0,0,0 },
    { 1 MB, 1, 7, 0, 6, 32, 1, UF2_fast }, /* 1 */
    { 2 MB, 2, 7, 0, 10, 32, 1, UF2_fast }, /* 2 */
    { 2 MB, 2, 7, 0, 10, 32, 1, UF2_opt }, /* 3 */
    { 4 MB, 2, 7, 0, 26, 40, 1, UF2_opt }, /* 4 */
    { 8 MB, 2, 8, 0, 42, 48, 1, UF2_opt }, /* 5 */
    { 16 MB, 2, 9, 0, 42, 48, 1, UF2_ultra }, /* 6 */
    { 32 MB, 2, 10, 0, 50, 64, 1, UF2_ultra }, /* 7 */
    { 64 MB, 2, 11, 1, 62, 96, 1, UF2_ultra }, /* 8 */
    { 64 MB, 4, 12, 2, 90, 273, 1, UF2_ultra }, /* 9 */
    { 128 MB, 2, 14, 3, 254, 273, 0, UF2_ultra } /* 10 */
};

#endif

static const UF2_compressionParameters UF2_highCParameters[UF2_MAX_HIGH_CLEVEL + 1] = {
    { 0,0,0,0,0,0,0,0 },
    { 1 MB, 4, 9, 2, 254, 273, 0, UF2_ultra }, /* 1 */
    { 2 MB, 4, 10, 2, 254, 273, 0, UF2_ultra }, /* 2 */
    { 4 MB, 4, 11, 2, 254, 273, 0, UF2_ultra }, /* 3 */
    { 8 MB, 4, 12, 2, 254, 273, 0, UF2_ultra }, /* 4 */
    { 16 MB, 4, 13, 3, 254, 273, 0, UF2_ultra }, /* 5 */
    { 32 MB, 4, 14, 3, 254, 273, 0, UF2_ultra }, /* 6 */
    { 64 MB, 4, 14, 4, 254, 273, 0, UF2_ultra }, /* 7 */
    { 128 MB, 4, 14, 4, 254, 273, 0, UF2_ultra }, /* 8 */
    { 256 MB, 4, 14, 5, 254, 273, 0, UF2_ultra }, /* 9 */
    { 512 MB, 4, 14, 5, 254, 273, 0, UF2_ultra } /* 10 */
};

#undef MB

/* The top level of each table is the level below it plus the lc/lp/pb search, so
 * it has no row of its own: every use of a level as an index goes through here. */
#define UF2_SEARCH_CLEVEL      (UF2_MAX_CLEVEL + 1)
#define UF2_SEARCH_HIGH_CLEVEL (UF2_MAX_HIGH_CLEVEL + 1)

static int UF2_tableRow(int level, int high)
{
    int const searchLevel = high ? UF2_SEARCH_HIGH_CLEVEL : UF2_SEARCH_CLEVEL;
    return level == searchLevel ? searchLevel - 1 : level;
}

UF2LIB_API int UF2LIB_CALL UF2_maxCLevel(void)
{
    return UF2_SEARCH_CLEVEL;
}

UF2LIB_API int UF2LIB_CALL UF2_maxHighCLevel(void)
{
    return UF2_SEARCH_HIGH_CLEVEL;
}

static void UF2_fillParameters(UF2_CCtx *const cctx, const UF2_compressionParameters* const params)
{
    UF2_lzma2Parameters* const cParams = &cctx->params.cParams;
    cParams->lc = 3;
    cParams->lp = 0;
    cParams->pb = 2;
    cParams->fast_length = params->fastLength;
    cParams->match_cycles = 1U << params->cyclesLog;
    cParams->strategy = params->strategy;
    cParams->second_dict_bits = params->chainLog;

    RMF_parameters* const rParams = &cctx->params.rParams;
    rParams->dictionary_size = MIN(params->dictionarySize, UF2_DICTSIZE_MAX); /* allows for reduced dict in 32-bit version */
    rParams->match_buffer_resize = UF2_BUFFER_RESIZE_DEFAULT;
    rParams->overlap_fraction = params->overlapFraction;
    rParams->divide_and_conquer = params->divideAndConquer;
    rParams->depth = params->searchDepth;
#ifdef RMF_REFERENCE
    rParams->use_ref_mf = 1;
#endif
}

#ifdef UF2_SINGLETHREAD

static inline int UF2_createCCtx_threads(UF2_CCtx *cctx, int const dualBuffer)
{
    (void)cctx;
    (void)dualBuffer;
    return 0;
}

static inline void UF2_freeCCtx_threads(UF2_CCtx *cctx)
{
    (void)cctx;
}

static inline void UF2_CCtx_zero_timeout(UF2_CCtx *cctx)
{
    (void)cctx;
}

static inline size_t UF2_mf_threadCount(UF2_CCtx *cctx)
{
    (void)cctx;
    return 1;
}

static inline size_t UF2_enc_threadCount(UF2_CCtx *cctx)
{
    (void)cctx;
    return 1;
}

static inline void UF2_cancelCStream_async(UF2_CStream *fcs)
{
    (void)fcs;
}

UF2LIB_API size_t UF2LIB_CALL UF2_setCStreamTimeout(UF2_CStream *fcs, unsigned timeout)
{
    (void)fcs;
    (void)timeout;
    return UF2_error_no_error;
}

static inline int UF2_CStream_waitAll(UF2_CStream *fcs)
{
    (void)fcs;
    return 0;
}

static inline UF2POOL_ctx *UF2_CCtx_asyncThread(UF2_CCtx *cctx)
{
    (void)cctx;
    return NULL;
}

static inline UF2POOL_ctx *UF2_CCtx_pool(UF2_CCtx *cctx)
{
    (void)cctx;
    return NULL;
}

#else

static int UF2_createCCtx_threads(UF2_CCtx *cctx, int const dualBuffer)
{
    cctx->compressThread = NULL;
    cctx->pool = UF2POOL_create(cctx->jobCount - 1);
    if (cctx->jobCount > 1 && cctx->pool == NULL)
        return -1;

    if (dualBuffer) {
        cctx->compressThread = UF2POOL_create(1);
        if (cctx->compressThread == NULL)
            return -1;
    }
    return 0;
}

static inline void UF2_freeCCtx_threads(UF2_CCtx *cctx)
{
    UF2POOL_free(cctx->pool);
    UF2POOL_free(cctx->compressThread);
}

static inline void UF2_CCtx_zero_timeout(UF2_CCtx *cctx)
{
    UF2POOL_free(cctx->compressThread);
    cctx->compressThread = NULL;
    cctx->timeout = 0;
}

static inline size_t UF2_mf_threadCount(UF2_CCtx *cctx)
{
    size_t mfThreads = cctx->curBlock.end / RMF_MIN_BYTES_PER_THREAD;
    mfThreads = MIN(RMF_threadCount(cctx->matchTable), mfThreads);
    return mfThreads + !mfThreads;
}

static inline size_t UF2_enc_threadCount(UF2_CCtx *cctx)
{
    size_t const encodeSize = (cctx->curBlock.end - cctx->curBlock.start);
    size_t nbThreads = MIN(cctx->jobCount, encodeSize / ENC_MIN_BYTES_PER_THREAD);
    return nbThreads + !nbThreads;
}

static inline void UF2_cancelCStream_async(UF2_CStream *fcs)
{
    if (fcs->compressThread != NULL) {
        fcs->canceled = 1;

        RMF_cancelBuild(fcs->matchTable);
        UF2POOL_waitAll(fcs->compressThread, 0);

        fcs->canceled = 0;
    }
}

UF2LIB_API size_t UF2LIB_CALL UF2_setCStreamTimeout(UF2_CStream * fcs, unsigned timeout)
{
    if (timeout != 0) {
        if (fcs->compressThread == NULL) {
            fcs->compressThread = UF2POOL_create(1);
            if (fcs->compressThread == NULL)
                return UF2_ERROR(memory_allocation);
        }
    }
    else if (!DICT_async(&fcs->buf) && fcs->dictMax == 0) {
        /* Only free the thread if not dual buffering and compression not underway */
        UF2POOL_free(fcs->compressThread);
        fcs->compressThread = NULL;
    }
    fcs->timeout = timeout;
    return UF2_error_no_error;
}

static inline int UF2_CStream_waitAll(UF2_CStream *fcs)
{
    return UF2POOL_waitAll(fcs->compressThread, fcs->timeout);
}

static inline UF2POOL_ctx *UF2_CCtx_asyncThread(UF2_CCtx *cctx)
{
    return cctx->compressThread;
}

static inline UF2POOL_ctx *UF2_CCtx_pool(UF2_CCtx *cctx)
{
    return cctx->pool;
}

#endif /* UF2_SINGLETHREAD */

static UF2_CCtx *UF2_createCCtx_internal(unsigned nbThreads, int const dualBuffer)
{
    nbThreads = UF2_checkNbThreads(nbThreads);

    DEBUGLOG(3, "UF2_createCCtxMt : %u threads", nbThreads);

    UF2_CCtx *const cctx = UF2_calloc(1, sizeof(UF2_CCtx) + (nbThreads - 1) * sizeof(UF2_job));
    if (cctx == NULL)
        return NULL;

    cctx->jobCount = nbThreads;
    for (unsigned u = 0; u < nbThreads; ++u)
        cctx->jobs[u].enc = NULL;

#ifndef NO_XXHASH
    cctx->params.doXXH = 1;
    cctx->params.format = UF2_format_native;
    cctx->params.xzCheck = XZ_CHECK_CRC64;
    cctx->params.propSearch = 0;
    cctx->params.levelSearch = 0;
#endif

    cctx->matchTable = NULL;
    DICT_construct(&cctx->buf, dualBuffer);

    if(UF2_createCCtx_threads(cctx, dualBuffer)) {
        UF2_free(cctx);
        return NULL;
    }

    for (unsigned u = 0; u < nbThreads; ++u) {
        cctx->jobs[u].enc = LZMA2_createECtx();
        if (cctx->jobs[u].enc == NULL) {
            UF2_freeCCtx(cctx);
            return NULL;
        }
        cctx->jobs[u].cctx = cctx;
    }

    UF2_CCtx_setParameter(cctx, UF2_p_compressionLevel, UF2_CLEVEL_DEFAULT);
    cctx->params.cParams.reset_interval = 4;

    return cctx;
}

UF2LIB_API UF2_CCtx* UF2LIB_CALL UF2_createCCtx(void)
{
    return UF2_createCCtx_internal(1, 0);
}

UF2LIB_API UF2_CCtx* UF2LIB_CALL UF2_createCCtxMt(unsigned nbThreads)
{
    return UF2_createCCtx_internal(nbThreads, 0);
}

UF2LIB_API void UF2LIB_CALL UF2_freeCCtx(UF2_CCtx *cctx)
{
    if (cctx == NULL) 
        return;

    DEBUGLOG(3, "UF2_freeCCtx : %u threads", cctx->jobCount);

    DICT_free(&cctx->buf);

    for (unsigned u = 0; u < cctx->jobCount; ++u) {
        LZMA2_freeECtx(cctx->jobs[u].enc);
    }

    UF2_freeCCtx_threads(cctx);

    RMF_freeMatchTable(cctx->matchTable);
    UF2_free(cctx);
}

UF2LIB_API unsigned UF2LIB_CALL UF2_getCCtxThreadCount(const UF2_CCtx *cctx)
{
    return cctx->jobCount;
}

/* UF2_buildRadixTable() : UF2POOL_function type */
static void UF2_buildRadixTable(void* const jobDescription, ptrdiff_t const n)
{
    UF2_CCtx *const cctx = (UF2_CCtx*)jobDescription;

    RMF_buildTable(cctx->matchTable, n, 1, cctx->curBlock);
}

/* UF2_compressRadixChunk() : UF2POOL_function type */
static void UF2_compressRadixChunk(void* const jobDescription, ptrdiff_t const n)
{
    UF2_CCtx *const cctx = (UF2_CCtx*)jobDescription;

    cctx->jobs[n].cSize = LZMA2_encode(cctx->jobs[n].enc, cctx->matchTable,
        cctx->jobs[n].block,
        &cctx->params.cParams,
        -1,
        &cctx->progressIn, &cctx->progressOut, &cctx->canceled);
}

static int UF2_initEncoders(UF2_CCtx *const cctx)
{
    for(unsigned u = 0; u < cctx->jobCount; ++u) {
        if (LZMA2_hashAlloc(cctx->jobs[u].enc, &cctx->params.cParams) != 0)
            return 1;
    }
    return 0;
}

static void UF2_initProgress(UF2_CCtx *const cctx)
{
    RMF_initProgress(cctx->matchTable);
    cctx->progressIn = 0;
    cctx->streamCsize += cctx->progressOut;
    cctx->progressOut = 0;
    cctx->canceled = 0;
}

/* UF2_compressCurBlock_blocking() :
 * Compress cctx->curBlock and wait until complete.
 * Write streamProp as the first byte if >= 0
 */
static size_t UF2_compressCurBlock_blocking(UF2_CCtx *const cctx, int const streamProp)
{
    size_t const encodeSize = (cctx->curBlock.end - cctx->curBlock.start);
    size_t nbThreads = UF2_enc_threadCount(cctx);

    DEBUGLOG(5, "UF2_compressCurBlock : %u threads, %u start, %u bytes", (U32)nbThreads, (U32)cctx->curBlock.start, (U32)encodeSize);

    size_t sliceStart = cctx->curBlock.start;
    size_t const sliceSize = encodeSize / nbThreads;
    cctx->jobs[0].block.data = cctx->curBlock.data;
    cctx->jobs[0].block.start = sliceStart;
    cctx->jobs[0].block.end = sliceStart + sliceSize;

    for (size_t u = 1; u < nbThreads; ++u) {
        sliceStart += sliceSize;
        cctx->jobs[u].block.data = cctx->curBlock.data;
        cctx->jobs[u].block.start = sliceStart;
        cctx->jobs[u].block.end = sliceStart + sliceSize;
    }
    cctx->jobs[nbThreads - 1].block.end = cctx->curBlock.end;

    /* initialize to length 2 */
    RMF_initTable(cctx->matchTable, cctx->curBlock.data, cctx->curBlock.end);

    if (cctx->canceled) {
        RMF_resetIncompleteBuild(cctx->matchTable);
        return UF2_ERROR(canceled);
    }

    size_t mfThreads = UF2_mf_threadCount(cctx);
    UF2POOL_addRange(UF2_CCtx_pool(cctx), UF2_buildRadixTable, cctx, 1, mfThreads);

    int err = RMF_buildTable(cctx->matchTable, 0, mfThreads > 1, cctx->curBlock);

    UF2POOL_waitAll(UF2_CCtx_pool(cctx), 0);

    if (err)
        return UF2_ERROR(canceled);

#ifdef RMF_CHECK_INTEGRITY
    err = RMF_integrityCheck(cctx->matchTable, cctx->curBlock.data, cctx->curBlock.start, cctx->curBlock.end, cctx->params.rParams.depth);
    if (err)
        return UF2_ERROR(internal);
#endif

    UF2POOL_addRange(UF2_CCtx_pool(cctx), UF2_compressRadixChunk, cctx, 1, nbThreads);

    cctx->jobs[0].cSize = LZMA2_encode(cctx->jobs[0].enc, cctx->matchTable,
        cctx->jobs[0].block,
        &cctx->params.cParams, streamProp,
        &cctx->progressIn, &cctx->progressOut, &cctx->canceled);

    UF2POOL_waitAll(UF2_CCtx_pool(cctx), 0);

    for (size_t u = 0; u < nbThreads; ++u)
        if (UF2_isError(cctx->jobs[u].cSize))
            return cctx->jobs[u].cSize;

    cctx->threadCount = nbThreads;

    return UF2_error_no_error;
}

/* UF2_compressCurBlock_async() : UF2POOL_function type */
static void UF2_compressCurBlock_async(void* const jobDescription, ptrdiff_t const n)
{
    UF2_CCtx *const cctx = (UF2_CCtx*)jobDescription;

    cctx->asyncRes = UF2_compressCurBlock_blocking(cctx, (int)n);
}

/* UF2_compressCurBlock() :
 * Update total input size.
 * Clear the compressed data buffers.
 * Init progress info.
 * Start compression of cctx->curBlock, and wait for completion if no async compression thread exists.
 */
static size_t UF2_compressCurBlock(UF2_CCtx *const cctx, int const streamProp)
{
    UF2_initProgress(cctx);

    if (cctx->curBlock.start == cctx->curBlock.end)
        return UF2_error_no_error;

    /* update largest dict size used */
    cctx->dictMax = MAX(cctx->dictMax, cctx->curBlock.end);

    cctx->outThread = 0;
    cctx->threadCount = 0;
    cctx->outPos = 0;

    U32 rmfWeight = ZSTD_highbit32((U32)cctx->curBlock.end);
    U32 depthWeight = 2 + (cctx->params.rParams.depth >= 12) + (cctx->params.rParams.depth >= 28);
    U32 encWeight;

    if (rmfWeight >= 20) {
        rmfWeight = depthWeight * (rmfWeight - 10) + (rmfWeight - 19) * 12;
        if (cctx->params.cParams.strategy == 0)
            encWeight = 20;
        else if (cctx->params.cParams.strategy == 1)
            encWeight = 50;
        else
            encWeight = 60 + cctx->params.cParams.second_dict_bits + ZSTD_highbit32(cctx->params.cParams.fast_length) * 3U;
        rmfWeight = (rmfWeight << 4) / (rmfWeight + encWeight);
        encWeight = 16 - rmfWeight;
    }
    else {
        rmfWeight = 8;
        encWeight = 8;
    }

    cctx->rmfWeight = rmfWeight;
    cctx->encWeight = encWeight;

    if(UF2_CCtx_asyncThread(cctx) != NULL)
        UF2POOL_add(UF2_CCtx_asyncThread(cctx), UF2_compressCurBlock_async, cctx, streamProp);
    else
        cctx->asyncRes = UF2_compressCurBlock_blocking(cctx, streamProp);

    return cctx->asyncRes;
}

/* UF2_getProp() :
 * Get the LZMA2 dictionary size property byte. If xxhash is enabled, includes the xxhash flag bit.
 */
static BYTE UF2_getProp(UF2_CCtx *const cctx, size_t const dictionarySize)
{
#ifndef NO_XXHASH
    return LZMA2_getDictSizeProp(dictionarySize) | (BYTE)((cctx->params.doXXH != 0) << UF2_PROP_HASH_BIT);
#else
    (void)cctx;
    return LZMA2_getDictSizeProp(dictionarySize);
#endif
}

static void UF2_preBeginFrame(UF2_CCtx *const cctx, size_t const dictReduce)
{
    /* Free unsuitable match table before reallocating anything else */
    if (cctx->matchTable && !RMF_compatibleParameters(cctx->matchTable, &cctx->params.rParams, dictReduce)) {
        RMF_freeMatchTable(cctx->matchTable);
        cctx->matchTable = NULL;
    }
}

static size_t UF2_beginFrame(UF2_CCtx *const cctx, size_t const dictReduce)
{
    if (UF2_initEncoders(cctx) != 0) /* Create hash objects together, leaving the (large) match table last */
        return UF2_ERROR(memory_allocation);

    if (cctx->matchTable == NULL) {
        cctx->matchTable = RMF_createMatchTable(&cctx->params.rParams, dictReduce, cctx->jobCount);
        if (cctx->matchTable == NULL)
            return UF2_ERROR(memory_allocation);
    }
    else {
        DEBUGLOG(5, "Have compatible match table");
        RMF_applyParameters(cctx->matchTable, &cctx->params.rParams, dictReduce);
    }

    cctx->dictMax = 0;
    cctx->streamTotal = 0;
    cctx->streamCsize = 0;
    cctx->progressIn = 0;
    cctx->progressOut = 0;
    RMF_initProgress(cctx->matchTable);
    cctx->asyncRes = 0;
    cctx->outThread = 0;
    cctx->threadCount = 0;
    cctx->outPos = 0;
    cctx->curBlock.start = 0;
    cctx->curBlock.end = 0;
    cctx->lockParams = 1;

    return UF2_error_no_error;
}

static void UF2_endFrame(UF2_CCtx *const cctx)
{
    cctx->dictMax = 0;
    cctx->asyncRes = 0;
    cctx->lockParams = 0;
}

/* Compress a memory buffer which may be larger than the dictionary.
 * The property byte is written first unless the omit flag is set.
 * Return: compressed size.
 */
static size_t UF2_compressBuffer(UF2_CCtx *const cctx,
    const void* const src, size_t srcSize,
    void* const dst, size_t dstCapacity)
{
    if (srcSize == 0)
        return 0;

    BYTE* dstBuf = dst;
    size_t const dictionarySize = cctx->params.rParams.dictionary_size;
    size_t const blockOverlap = OVERLAP_FROM_DICT_SIZE(dictionarySize, cctx->params.rParams.overlap_fraction);
    int streamProp = cctx->params.omitProp ? -1 : UF2_getProp(cctx, MIN(srcSize, dictionarySize));

    cctx->curBlock.data = src;
    cctx->curBlock.start = 0;

    size_t blockTotal = 0;

    do {
        cctx->curBlock.end = cctx->curBlock.start + MIN(srcSize, dictionarySize - cctx->curBlock.start);
        blockTotal += cctx->curBlock.end - cctx->curBlock.start;

        CHECK_F(UF2_compressCurBlock(cctx, streamProp));

        streamProp = -1;

        for (size_t u = 0; u < cctx->threadCount; ++u) {
            DEBUGLOG(5, "Write thread %u : %u bytes", (U32)u, (U32)cctx->jobs[u].cSize);

            if (dstCapacity < cctx->jobs[u].cSize) 
                return UF2_ERROR(dstSize_tooSmall);

            const BYTE* const outBuf = RMF_getTableAsOutputBuffer(cctx->matchTable, cctx->jobs[u].block.start);
            memcpy(dstBuf, outBuf, cctx->jobs[u].cSize);

            dstBuf += cctx->jobs[u].cSize;
            dstCapacity -= cctx->jobs[u].cSize;
        }
        srcSize -= cctx->curBlock.end - cctx->curBlock.start;
        if (cctx->params.cParams.reset_interval
            && blockTotal + MIN(dictionarySize - blockOverlap, srcSize) > dictionarySize * cctx->params.cParams.reset_interval) {
            /* periodically reset the dictionary for mt decompression */
            DEBUGLOG(4, "Resetting dictionary after %u bytes", (unsigned)blockTotal);
            cctx->curBlock.start = 0;
            blockTotal = 0;
        }
        else {
            cctx->curBlock.start = blockOverlap;
        }
        cctx->curBlock.data += cctx->curBlock.end - cctx->curBlock.start;
    } while (srcSize != 0);
    return dstBuf - (const BYTE*)dst;
}

static size_t UF2_compressCCtxNative(UF2_CCtx *cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel)
{
    if (dstCapacity < 2U - cctx->params.omitProp) /* empty LZMA2 stream is byte sequence {0, 0} */
        return UF2_ERROR(dstSize_tooSmall);

    if (compressionLevel > 0)
        UF2_CCtx_setParameter(cctx, UF2_p_compressionLevel, compressionLevel);

    DEBUGLOG(4, "UF2_compressCCtx : level %u, %u src => %u avail", cctx->params.compressionLevel, (U32)srcSize, (U32)dstCapacity);

    /* No async compression for in-memory function */
    UF2_CCtx_zero_timeout(cctx);

    UF2_preBeginFrame(cctx, srcSize);
    CHECK_F(UF2_beginFrame(cctx, srcSize));

    size_t const cSize = UF2_compressBuffer(cctx, src, srcSize, dst, dstCapacity);

    if (UF2_isError(cSize))
        return cSize;

    BYTE* dstBuf = dst;
    BYTE* const end = dstBuf + dstCapacity;

    dstBuf += cSize;
    if(dstBuf >= end)
        return UF2_ERROR(dstSize_tooSmall);

    if (cSize == 0)
        *dstBuf++ = UF2_getProp(cctx, 0);

    *dstBuf++ = LZMA2_END_MARKER;

#ifndef NO_XXHASH
    if (cctx->params.doXXH && !cctx->params.omitProp) {
        XXH32_canonical_t canonical;
        DEBUGLOG(5, "Writing hash");
        if(end - dstBuf < XXHASH_SIZEOF)
            return UF2_ERROR(dstSize_tooSmall);
        XXH32_canonicalFromHash(&canonical, XXH32(src, srcSize, 0));
        memcpy(dstBuf, &canonical, XXHASH_SIZEOF);
        dstBuf += XXHASH_SIZEOF;
    }
#endif
    
    UF2_endFrame(cctx);

    return dstBuf - (BYTE*)dst;
}

/* A single-block .xz file around the library's own LZMA2 data. The encoder runs
 * exactly as it does for the native format, with the property byte omitted; the
 * dictionary property goes into the Block Header instead, and the check replaces
 * the xxhash. See "The .xz File Format" 1.2.1. */
static size_t UF2_compressCCtxXz(UF2_CCtx *cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel)
{
    BYTE *const out = (BYTE*)dst;
    unsigned const check = cctx->params.xzCheck;
    size_t const checkSize = (size_t)XZ_checkSize(check);
    /* room reserved before the LZMA2 data, and after it for padding, check, index and footer */
    size_t const head = XZ_STREAM_HEADER_SIZE + XZ_BLOCK_HEADER_MAX;
    size_t const tail = 3 + checkSize + XZ_INDEX_MAX_1 + XZ_STREAM_FOOTER_SIZE;
    U64 unpadded = 0;
    U64 const uncompressed = srcSize;
    size_t records = 0;

    if (dstCapacity < head + tail + 1)
        return UF2_ERROR(dstSize_tooSmall);

    XZ_writeStreamHeader(out, check);
    size_t pos = XZ_STREAM_HEADER_SIZE;

    /* An empty input is a stream with no blocks, which is what xz itself writes. */
    if (srcSize > 0) {
        BYTE const omitProp = cctx->params.omitProp;
        cctx->params.omitProp = 1;
        size_t const cSize = UF2_compressCCtxNative(cctx, out + head, dstCapacity - head - tail,
            src, srcSize, compressionLevel);
        cctx->params.omitProp = omitProp;
        if (UF2_isError(cSize))
            return cSize;

        /* The header's size depends on the compressed size, so it is built after the
         * data and the data is moved down to meet it. */
        BYTE header[XZ_BLOCK_HEADER_MAX];
        /* The dictionary actually used cannot exceed the input, so the property is
         * reduced the same way the native format reduces its own (UF2_compressBuffer).
         * Without this a 1-byte input at level 10 asks every decoder for 128 MiB. */
        BYTE const dictProp = LZMA2_getDictSizeProp(MIN(srcSize, cctx->params.rParams.dictionary_size));
        size_t const headerSize = XZ_writeBlockHeader(header, cSize, srcSize, dictProp);
        memmove(out + pos + headerSize, out + head, cSize);
        memcpy(out + pos, header, headerSize);
        pos += headerSize + cSize;

        size_t const padding = (4 - (cSize & 3)) & 3;
        memset(out + pos, 0, padding);
        pos += padding;

        if (check == XZ_CHECK_CRC32)
            MEM_writeLE32(out + pos, XZ_crc32(0, src, srcSize));
        else if (check == XZ_CHECK_CRC64)
            MEM_writeLE64(out + pos, XZ_crc64(0, src, srcSize));
        pos += checkSize;

        unpadded = headerSize + cSize + checkSize;
        records = 1;
    }

    size_t const indexSize = XZ_writeIndex(out + pos, &unpadded, &uncompressed, records);
    pos += indexSize;
    XZ_writeStreamFooter(out + pos, indexSize, check);
    return pos + XZ_STREAM_FOOTER_SIZE;
}

static size_t UF2_compressCCtxFormat(UF2_CCtx *cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    if (cctx->params.format == UF2_format_xz)
        return UF2_compressCCtxXz(cctx, dst, dstCapacity, src, srcSize, 0);
    return UF2_compressCCtxNative(cctx, dst, dstCapacity, src, srcSize, 0);
}

/* lc/lp/pb search. The caller's own setting is tried first, so the result can
 * only improve on it; then two settings chosen by measurement. Of the 60 legal
 * combinations these two, added to the default 3/0/2, recovered most of what
 * picking the best of all 60 per file gains: Silesia -0.39% in size, AIT -2.16%,
 * a varied 52-file corpus -0.28%, and -1.06% on 80 files not used to choose them,
 * none of them larger than with the default alone. A single fixed setting could
 * not do this: 3/1/3, the best one for Silesia, made 51 of the 52 varied files
 * larger. The output stays standard LZMA2: lc/lp/pb are carried in the chunk
 * headers, where any LZMA2 decoder reads them. */
static const BYTE UF2_searchCandidates[][3] = { { 4, 0, 1 }, { 1, 2, 2 } };

static size_t UF2_compressSearch(UF2_CCtx *cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    UF2_lzma2Parameters *const cParams = &cctx->params.cParams;
    unsigned const lc = cParams->lc, lp = cParams->lp, pb = cParams->pb;
    BYTE *const scratch = malloc(dstCapacity);
    if (scratch == NULL)
        return UF2_ERROR(memory_allocation);

    size_t best = UF2_compressCCtxFormat(cctx, dst, dstCapacity, src, srcSize);
    /* two buffers take turns: the best output so far and a spare to try the next into */
    BYTE *bestBuf = (BYTE*)dst, *spare = scratch;

    for (size_t i = 0; !UF2_isError(best) && i < sizeof(UF2_searchCandidates) / sizeof(UF2_searchCandidates[0]); ++i) {
        const BYTE *const c = UF2_searchCandidates[i];
        if (c[0] == lc && c[1] == lp && c[2] == pb)
            continue;
        cParams->lc = c[0];
        cParams->lp = c[1];
        cParams->pb = c[2];
        size_t const cSize = UF2_compressCCtxFormat(cctx, spare, dstCapacity, src, srcSize);
        if (UF2_isError(cSize)) {
            /* a candidate whose output would not even fit cannot be the smallest */
            if (UF2_getErrorCode(cSize) == UF2_error_dstSize_tooSmall)
                continue;
            best = cSize;
            break;
        }
        if (cSize < best) {
            BYTE *const t = bestBuf;
            bestBuf = spare;
            spare = t;
            best = cSize;
        }
    }

    cParams->lc = lc;
    cParams->lp = lp;
    cParams->pb = pb;
    if (!UF2_isError(best) && bestBuf != (BYTE*)dst)
        memcpy(dst, bestBuf, best);
    free(scratch);
    return best;
}

UF2LIB_API size_t UF2LIB_CALL UF2_compressCCtx(UF2_CCtx *cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel)
{
    if (compressionLevel > 0)
        UF2_CCtx_setParameter(cctx, UF2_p_compressionLevel, compressionLevel);

    if ((cctx->params.propSearch || cctx->params.levelSearch) && srcSize > 0)
        return UF2_compressSearch(cctx, dst, dstCapacity, src, srcSize);
    return UF2_compressCCtxFormat(cctx, dst, dstCapacity, src, srcSize);
}

UF2LIB_API size_t UF2LIB_CALL UF2_compressMt(void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel,
    unsigned nbThreads)
{
    UF2_CCtx *const cctx = UF2_createCCtxMt(nbThreads);
    if (cctx == NULL)
        return UF2_ERROR(memory_allocation);

    size_t const cSize = UF2_compressCCtx(cctx, dst, dstCapacity, src, srcSize, compressionLevel);

    UF2_freeCCtx(cctx);

    return cSize;
}

UF2LIB_API size_t UF2LIB_CALL UF2_compress(void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel)
{
    return UF2_compressMt(dst, dstCapacity, src, srcSize, compressionLevel, 1);
}

UF2LIB_API BYTE UF2LIB_CALL UF2_getCCtxDictProp(UF2_CCtx *cctx)
{
    return LZMA2_getDictSizeProp(cctx->dictMax ? cctx->dictMax : cctx->params.rParams.dictionary_size);
}

#define MAXCHECK(val,max) do {            \
    if ((val)>(max)) {     \
        return UF2_ERROR(parameter_outOfBound);  \
}   } while(0)

#define CLAMPCHECK(val,min,max) do {            \
    if (((val)<(min)) | ((val)>(max))) {     \
        return UF2_ERROR(parameter_outOfBound);  \
}   } while(0)


UF2LIB_API size_t UF2LIB_CALL UF2_CCtx_setParameter(UF2_CCtx *cctx, UF2_cParameter param, size_t value)
{
    if (cctx->lockParams
        && param != UF2_p_literalCtxBits && param != UF2_p_literalPosBits && param != UF2_p_posBits)
        return UF2_ERROR(stage_wrong);

    switch (param)
    {
    case UF2_p_compressionLevel:
        if (cctx->params.highCompression) {
            CLAMPCHECK(value, 1, UF2_SEARCH_HIGH_CLEVEL);
            UF2_fillParameters(cctx, &UF2_highCParameters[UF2_tableRow((int)value, 1)]);
            cctx->params.levelSearch = value == UF2_SEARCH_HIGH_CLEVEL;
        }
        else {
            CLAMPCHECK(value, 1, UF2_SEARCH_CLEVEL);
            UF2_fillParameters(cctx, &UF2_defaultCParameters[UF2_tableRow((int)value, 0)]);
            cctx->params.levelSearch = value == UF2_SEARCH_CLEVEL;
        }
        cctx->params.compressionLevel = (unsigned)value;
        break;

    case UF2_p_highCompression:
        cctx->params.highCompression = value != 0;
        UF2_CCtx_setParameter(cctx, UF2_p_compressionLevel, cctx->params.compressionLevel);
        break;

    case UF2_p_dictionaryLog:
        CLAMPCHECK(value, UF2_DICTLOG_MIN, UF2_DICTLOG_MAX);
        cctx->params.rParams.dictionary_size = (size_t)1 << value;
        break;

    case UF2_p_dictionarySize:
        CLAMPCHECK(value, UF2_DICTSIZE_MIN, UF2_DICTSIZE_MAX);
        cctx->params.rParams.dictionary_size = value;
        break;

    case UF2_p_overlapFraction:
        MAXCHECK(value, UF2_BLOCK_OVERLAP_MAX);
        cctx->params.rParams.overlap_fraction = (unsigned)value;
        break;

    case UF2_p_resetInterval:
        if (value != 0)
            CLAMPCHECK(value, UF2_RESET_INTERVAL_MIN, UF2_RESET_INTERVAL_MAX);
        cctx->params.cParams.reset_interval = (unsigned)value;
        break;

    case UF2_p_bufferResize:
        MAXCHECK(value, UF2_BUFFER_RESIZE_MAX);
        cctx->params.rParams.match_buffer_resize = (unsigned)value;
        break;

    case UF2_p_hybridChainLog:
        CLAMPCHECK(value, UF2_CHAINLOG_MIN, UF2_CHAINLOG_MAX);
        cctx->params.cParams.second_dict_bits = (unsigned)value;
        break;

    case UF2_p_hybridCycles:
        CLAMPCHECK(value, UF2_HYBRIDCYCLES_MIN, UF2_HYBRIDCYCLES_MAX);
        cctx->params.cParams.match_cycles = (unsigned)value;
        break;

    case UF2_p_searchDepth:
        CLAMPCHECK(value, UF2_SEARCH_DEPTH_MIN, UF2_SEARCH_DEPTH_MAX);
        cctx->params.rParams.depth = (unsigned)value;
        break;

    case UF2_p_fastLength:
        CLAMPCHECK(value, UF2_FASTLENGTH_MIN, UF2_FASTLENGTH_MAX);
        cctx->params.cParams.fast_length = (unsigned)value;
        break;

    case UF2_p_divideAndConquer:
        cctx->params.rParams.divide_and_conquer = value != 0;
        break;

    case UF2_p_strategy:
        MAXCHECK(value, (unsigned)UF2_ultra);
        cctx->params.cParams.strategy = (UF2_strategy)value;
        break;

        /* lc, lp, pb can be changed between encoder chunks.
         * A condition where lc+lp > 4 is permitted to allow sequential setting,
         * but will return an error code to alert the calling function.
         * If lc+lp is still >4 when encoding begins, lc will be reduced. */
    case UF2_p_literalCtxBits:
        MAXCHECK(value, UF2_LC_MAX);
        cctx->params.cParams.lc = (unsigned)value;
        if (value + cctx->params.cParams.lp > UF2_LCLP_MAX)
            return UF2_ERROR(lclpMax_exceeded);
        break;

    case UF2_p_literalPosBits:
        MAXCHECK(value, UF2_LP_MAX);
        cctx->params.cParams.lp = (unsigned)value;
        if (cctx->params.cParams.lc + value > UF2_LCLP_MAX)
            return UF2_ERROR(lclpMax_exceeded);
        break;

    case UF2_p_posBits:
        MAXCHECK(value, UF2_PB_MAX);
        cctx->params.cParams.pb = (unsigned)value;
        break;

#ifndef NO_XXHASH
    case UF2_p_doXXHash:
        cctx->params.doXXH = value != 0;
        break;
#endif

    case UF2_p_omitProperties:
        cctx->params.omitProp = value != 0;
        break;

    case UF2_p_format:
        if (value != UF2_format_native && value != UF2_format_xz)
            return UF2_ERROR(parameter_outOfBound);
        cctx->params.format = (BYTE)value;
        break;

    case UF2_p_xzCheck:
        if (value != XZ_CHECK_NONE && value != XZ_CHECK_CRC32 && value != XZ_CHECK_CRC64)
            return UF2_ERROR(parameter_unsupported);
        cctx->params.xzCheck = (BYTE)value;
        break;

    case UF2_p_propertySearch:
        cctx->params.propSearch = value != 0;
        break;
#ifdef RMF_REFERENCE
    case UF2_p_useReferenceMF:
        cctx->params.rParams.use_ref_mf = value != 0;
        break;
#endif
    default: return UF2_ERROR(parameter_unsupported);
    }
    return value;
}

UF2LIB_API size_t UF2LIB_CALL UF2_CCtx_getParameter(UF2_CCtx *cctx, UF2_cParameter param)
{
    switch (param)
    {
    case UF2_p_compressionLevel:
        return cctx->params.compressionLevel;

    case UF2_p_highCompression:
        return cctx->params.highCompression;

    case UF2_p_dictionaryLog: {
        size_t dictLog = UF2_DICTLOG_MIN;
        while (((size_t)1 << dictLog) < cctx->params.rParams.dictionary_size)
            ++dictLog;
        return dictLog;
    }

    case UF2_p_dictionarySize:
        return cctx->params.rParams.dictionary_size;

    case UF2_p_overlapFraction:
        return cctx->params.rParams.overlap_fraction;

    case UF2_p_resetInterval:
        return cctx->params.cParams.reset_interval;

    case UF2_p_bufferResize:
        return cctx->params.rParams.match_buffer_resize;

    case UF2_p_hybridChainLog:
        return cctx->params.cParams.second_dict_bits;

    case UF2_p_hybridCycles:
        return cctx->params.cParams.match_cycles;

    case UF2_p_literalCtxBits:
        return cctx->params.cParams.lc;

    case UF2_p_literalPosBits:
        return cctx->params.cParams.lp;

    case UF2_p_posBits:
        return cctx->params.cParams.pb;

    case UF2_p_searchDepth:
        return cctx->params.rParams.depth;

    case UF2_p_fastLength:
        return cctx->params.cParams.fast_length;

    case UF2_p_divideAndConquer:
        return cctx->params.rParams.divide_and_conquer;

    case UF2_p_strategy:
        return (size_t)cctx->params.cParams.strategy;

#ifndef NO_XXHASH
    case UF2_p_doXXHash:
        return cctx->params.doXXH;
#endif

    case UF2_p_omitProperties:
        return cctx->params.omitProp;

    case UF2_p_format:
        return cctx->params.format;

    case UF2_p_xzCheck:
        return cctx->params.xzCheck;

    case UF2_p_propertySearch:
        return cctx->params.propSearch;
#ifdef RMF_REFERENCE
    case UF2_p_useReferenceMF:
        return cctx->params.rParams.use_ref_mf;
#endif
    default: return UF2_ERROR(parameter_unsupported);
    }
}

UF2LIB_API size_t UF2LIB_CALL UF2_CStream_setParameter(UF2_CStream* fcs, UF2_cParameter param, size_t value)
{
    return UF2_CCtx_setParameter(fcs, param, value);
}

UF2LIB_API size_t UF2LIB_CALL UF2_CStream_getParameter(UF2_CStream* fcs, UF2_cParameter param)
{
    return UF2_CCtx_getParameter(fcs, param);
}

UF2LIB_API UF2_CStream* UF2LIB_CALL UF2_createCStream(void)
{
    return UF2_createCCtx_internal(1, 0);
}

UF2LIB_API UF2_CStream* UF2LIB_CALL UF2_createCStreamMt(unsigned nbThreads, int dualBuffer)
{
    return UF2_createCCtx_internal(nbThreads, dualBuffer);
}

UF2LIB_API void UF2LIB_CALL UF2_freeCStream(UF2_CStream * fcs)
{
    UF2_freeCCtx(fcs);
}

UF2LIB_API size_t UF2LIB_CALL UF2_initCStream(UF2_CStream* fcs, int compressionLevel)
{
    DEBUGLOG(4, "UF2_initCStream level %d", compressionLevel);

    if (fcs->params.format != UF2_format_native)
        return UF2_ERROR(parameter_unsupported);

    fcs->endMarked = 0;
    fcs->wroteProp = 0;
    fcs->loopCount = 0;

    if(compressionLevel > 0)
        UF2_CCtx_setParameter(fcs, UF2_p_compressionLevel, compressionLevel);

    DICT_buffer *const buf = &fcs->buf;
    size_t const dictSize = fcs->params.rParams.dictionary_size;

    /* Free unsuitable objects before reallocating anything new */
    if (DICT_size(buf) < dictSize)
        DICT_free(buf);

    UF2_preBeginFrame(fcs, 0);

#ifdef NO_XXHASH
    int const doHash = 0;
#else
    int const doHash = (fcs->params.doXXH && !fcs->params.omitProp);
#endif
    size_t dictOverlap = OVERLAP_FROM_DICT_SIZE(fcs->params.rParams.dictionary_size, fcs->params.rParams.overlap_fraction);
    if (DICT_init(buf, dictSize, dictOverlap, fcs->params.cParams.reset_interval, doHash) != 0)
        return UF2_ERROR(memory_allocation);

    CHECK_F(UF2_beginFrame(fcs, 0));

    return 0;
}

static size_t UF2_compressStream_internal(UF2_CStream* const fcs, int const ending)
{
    CHECK_F(UF2_waitCStream(fcs));

    DICT_buffer *const buf = &fcs->buf;

    /* no compression can occur while compressed output exists */
    if (fcs->outThread == fcs->threadCount && DICT_hasUnprocessed(buf)) {
        fcs->streamTotal += fcs->curBlock.end - fcs->curBlock.start;

        DICT_getBlock(buf, &fcs->curBlock);

        int streamProp = -1;

        if (!fcs->wroteProp && !fcs->params.omitProp) {
            /* If the LZMA2 property byte is required and not already written,
             * pass it to the compression function 
             */
            size_t dictionarySize = ending ? MAX(fcs->dictMax, fcs->curBlock.end)
                : fcs->params.rParams.dictionary_size;
            streamProp = UF2_getProp(fcs, dictionarySize);
            DEBUGLOG(4, "Writing property byte : 0x%X", streamProp);
            fcs->wroteProp = 1;
        }

        CHECK_F(UF2_compressCurBlock(fcs, streamProp));
    }
    return UF2_error_no_error;
}

/* Copy the compressed output stored in the match table buffer.
 * One slice exists per thread.
 */
UF2LIB_API size_t UF2LIB_CALL UF2_copyCStreamOutput(UF2_CStream* fcs, UF2_outBuffer *output)
{
    for (; fcs->outThread < fcs->threadCount; ++fcs->outThread) {
        const BYTE* const outBuf = RMF_getTableAsOutputBuffer(fcs->matchTable, fcs->jobs[fcs->outThread].block.start) + fcs->outPos;
        BYTE* const dstBuf = (BYTE*)output->dst + output->pos;
        size_t const dstCapacity = output->size - output->pos;
        size_t toWrite = fcs->jobs[fcs->outThread].cSize;

        toWrite = MIN(toWrite - fcs->outPos, dstCapacity);

        DEBUGLOG(5, "CStream : writing %u bytes", (U32)toWrite);

        memcpy(dstBuf, outBuf, toWrite);
        fcs->outPos += toWrite;
        output->pos += toWrite;

        /* If the slice is not flushed, the output is full */
        if (fcs->outPos < fcs->jobs[fcs->outThread].cSize)
            return 1;

        fcs->outPos = 0;
    }
    return 0;
}

static size_t UF2_compressStream_input(UF2_CStream* fcs, UF2_inBuffer* input)
{
    CHECK_F(fcs->asyncRes);

    DICT_buffer * const buf = &fcs->buf;

    while (input->pos < input->size) {
        /* read input until the buffer(s) are full */
        if (DICT_needShift(buf)) {
            /* cannot shift single dict during compression */
            if(!DICT_async(buf))
                CHECK_F(UF2_waitCStream(fcs));
            DICT_shift(buf);
        }
        
        CHECK_F(fcs->asyncRes);

        DICT_put(buf, input);
        
        if (!DICT_availSpace(buf)) {
            /* break if the compressor is not available */
            if (fcs->outThread < fcs->threadCount)
                break;

            CHECK_F(UF2_compressStream_internal(fcs, 0));
        }

        CHECK_F(fcs->asyncRes);
    }

    return UF2_error_no_error;
}

static size_t UF2_loopCheck(UF2_CStream* fcs, int unchanged)
{
    if (unchanged) {
        ++fcs->loopCount;
        if (fcs->loopCount > UF2_MAX_LOOPS) {
            UF2_cancelCStream(fcs);
            return UF2_ERROR(buffer);
        }
    }
    else {
        fcs->loopCount = 0;
    }
    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_compressStream(UF2_CStream* fcs, UF2_outBuffer *output, UF2_inBuffer* input)
{
    if (!fcs->lockParams)
        return UF2_ERROR(init_missing);

    size_t const prevIn = input->pos;
    size_t const prevOut = (output != NULL) ? output->pos : 0;

    if (output != NULL && fcs->outThread < fcs->threadCount)
        UF2_copyCStreamOutput(fcs, output);

    CHECK_F(UF2_compressStream_input(fcs, input));

    if(output != NULL && fcs->outThread < fcs->threadCount)
        UF2_copyCStreamOutput(fcs, output);

    CHECK_F(UF2_loopCheck(fcs, prevIn == input->pos && (output == NULL || prevOut == output->pos)));

    return fcs->outThread < fcs->threadCount;
}

UF2LIB_API size_t UF2LIB_CALL UF2_getDictionaryBuffer(UF2_CStream * fcs, UF2_dictBuffer * dict)
{
    if (!fcs->lockParams)
        return UF2_ERROR(init_missing);

    CHECK_F(fcs->asyncRes);

    DICT_buffer *buf = &fcs->buf;

    if (!DICT_availSpace(buf) && DICT_hasUnprocessed(buf))
        CHECK_F(UF2_compressStream_internal(fcs, 0));

    if (DICT_needShift(buf) && !DICT_async(buf))
        CHECK_F(UF2_waitCStream(fcs));

    dict->size = (unsigned long)DICT_get(buf, &dict->dst);

    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_updateDictionary(UF2_CStream * fcs, size_t addedSize)
{
    if (DICT_update(&fcs->buf, addedSize))
        CHECK_F(UF2_compressStream_internal(fcs, 0));

    return fcs->outThread < fcs->threadCount;
}

UF2LIB_API size_t UF2LIB_CALL UF2_getNextCompressedBuffer(UF2_CStream* fcs, UF2_cBuffer* cbuf)
{
    cbuf->src = NULL;
    cbuf->size = 0;

    CHECK_F(UF2_waitCStream(fcs));

    if (fcs->outThread < fcs->threadCount) {
        cbuf->src = RMF_getTableAsOutputBuffer(fcs->matchTable, fcs->jobs[fcs->outThread].block.start) + fcs->outPos;
        cbuf->size = fcs->jobs[fcs->outThread].cSize - fcs->outPos;
        ++fcs->outThread;
        fcs->outPos = 0;
    }
    return cbuf->size;
}

UF2LIB_API unsigned long long UF2LIB_CALL UF2_getCStreamProgress(const UF2_CStream * fcs, unsigned long long *outputSize)
{
    if (outputSize != NULL)
        *outputSize = fcs->streamCsize + fcs->progressOut;

    U64 const encodeSize = fcs->curBlock.end - fcs->curBlock.start;

    if (fcs->progressIn == 0 && fcs->curBlock.end != 0)
        return fcs->streamTotal + ((fcs->matchTable->progress * encodeSize / fcs->curBlock.end * fcs->rmfWeight) >> 4);

    return fcs->streamTotal + ((fcs->rmfWeight * encodeSize) >> 4) + ((fcs->progressIn * fcs->encWeight) >> 4);
}

UF2LIB_API size_t UF2LIB_CALL UF2_waitCStream(UF2_CStream * fcs)
{
    if (UF2_CStream_waitAll(fcs) != 0)
        return UF2_ERROR(timedOut);
    CHECK_F(fcs->asyncRes);
    return fcs->outThread < fcs->threadCount;
}

UF2LIB_API void UF2LIB_CALL UF2_cancelCStream(UF2_CStream *fcs)
{
    UF2_cancelCStream_async(fcs);
    UF2_endFrame(fcs);
}

UF2LIB_API size_t UF2LIB_CALL UF2_remainingOutputSize(const UF2_CStream* fcs)
{
    CHECK_F(fcs->asyncRes);

    size_t cSize = 0;
    for (size_t u = fcs->outThread; u < fcs->threadCount; ++u)
        cSize += fcs->jobs[u].cSize;

    return cSize;
}

/* Write the properties byte (if required), the hash and the end marker
 * into the output buffer.
 */
static void UF2_writeEnd(UF2_CStream* const fcs)
{
    size_t thread = fcs->threadCount - 1;
    if (fcs->outThread == fcs->threadCount) {
        fcs->outThread = 0; 
        fcs->threadCount = 1;
        fcs->jobs[0].cSize = 0;
        thread = 0;
    }
    BYTE *const dst = RMF_getTableAsOutputBuffer(fcs->matchTable, fcs->jobs[thread].block.start)
        + fcs->jobs[thread].cSize;

    size_t pos = 0;

    if (!fcs->wroteProp && !fcs->params.omitProp) {
        /* no compression occurred */
        dst[pos] = UF2_getProp(fcs, 0);
        DEBUGLOG(4, "Writing property byte : 0x%X", dst[pos]);
        ++pos;
        fcs->wroteProp = 1;
    }

    DEBUGLOG(4, "Writing end marker");
    dst[pos++] = LZMA2_END_MARKER;

#ifndef NO_XXHASH
    if (fcs->params.doXXH && !fcs->params.omitProp) {
        XXH32_canonical_t canonical;

        XXH32_canonicalFromHash(&canonical, DICT_getDigest(&fcs->buf));
        DEBUGLOG(4, "Writing XXH32");
        memcpy(dst + pos, &canonical, XXHASH_SIZEOF);

        pos += XXHASH_SIZEOF;
    }
#endif
    fcs->jobs[thread].cSize += pos;
    fcs->endMarked = 1;

    UF2_endFrame(fcs);
}

static size_t UF2_flushStream_internal(UF2_CStream* fcs, int const ending)
{
    CHECK_F(fcs->asyncRes);

    DEBUGLOG(4, "UF2_flushStream_internal : %u to compress, %u to write",
        (U32)(fcs->buf.end - fcs->buf.start),
        (U32)UF2_remainingOutputSize(fcs));

    CHECK_F(UF2_compressStream_internal(fcs, ending));

    return fcs->outThread < fcs->threadCount;
}

UF2LIB_API size_t UF2LIB_CALL UF2_flushStream(UF2_CStream* fcs, UF2_outBuffer *output)
{
    if (!fcs->lockParams)
        return UF2_ERROR(init_missing);

    size_t const prevOut = (output != NULL) ? output->pos : 0;

    if (output != NULL && fcs->outThread < fcs->threadCount)
        UF2_copyCStreamOutput(fcs, output);

    size_t res = UF2_flushStream_internal(fcs, 0);
    CHECK_F(res);

    if (output != NULL && res != 0) {
        UF2_copyCStreamOutput(fcs, output);
        res = fcs->outThread < fcs->threadCount;
    }

    CHECK_F(UF2_loopCheck(fcs, output != NULL && prevOut == output->pos));

    return res;
}

UF2LIB_API size_t UF2LIB_CALL UF2_endStream(UF2_CStream* fcs, UF2_outBuffer *output)
{
    if (!fcs->endMarked && !fcs->lockParams)
        return UF2_ERROR(init_missing);

    size_t const prevOut = (output != NULL) ? output->pos : 0;
    
    if (output != NULL && fcs->outThread < fcs->threadCount)
        UF2_copyCStreamOutput(fcs, output);

    CHECK_F(UF2_flushStream_internal(fcs, 1));

    size_t res = UF2_waitCStream(fcs);
    CHECK_F(res);

    if (!fcs->endMarked && !DICT_hasUnprocessed(&fcs->buf)) {
        UF2_writeEnd(fcs);
        res = 1;
    }

    if (output != NULL && res != 0) {
        UF2_copyCStreamOutput(fcs, output);
        res = fcs->outThread < fcs->threadCount || DICT_hasUnprocessed(&fcs->buf);
    }

    CHECK_F(UF2_loopCheck(fcs, output != NULL && prevOut == output->pos));

    return res;
}

UF2LIB_API size_t UF2LIB_CALL UF2_getLevelParameters(int compressionLevel, int high, UF2_compressionParameters * params)
{
    if (high) {
        if (compressionLevel < 0 || compressionLevel > UF2_SEARCH_HIGH_CLEVEL)
            return UF2_ERROR(parameter_outOfBound);
        *params = UF2_highCParameters[UF2_tableRow(compressionLevel, 1)];
    }
    else {
        if (compressionLevel < 0 || compressionLevel > UF2_SEARCH_CLEVEL)
            return UF2_ERROR(parameter_outOfBound);
        *params = UF2_defaultCParameters[UF2_tableRow(compressionLevel, 0)];
    }
    return UF2_error_no_error;
}

static size_t UF2_memoryUsage_internal(size_t const dictionarySize, unsigned const bufferResize,
    unsigned const chainLog,
    UF2_strategy const strategy,
    unsigned const nbThreads)
{
    return RMF_memoryUsage(dictionarySize, bufferResize, nbThreads)
        + LZMA2_encMemoryUsage(chainLog, strategy, nbThreads);
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize(int compressionLevel, unsigned nbThreads)
{
    if (compressionLevel == 0)
        compressionLevel = UF2_CLEVEL_DEFAULT;

    CLAMPCHECK(compressionLevel, 1, UF2_SEARCH_CLEVEL);

    return UF2_estimateCCtxSize_byParams(UF2_defaultCParameters + UF2_tableRow(compressionLevel, 0), nbThreads);
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize_byParams(const UF2_compressionParameters * params, unsigned nbThreads)
{
    nbThreads = UF2_checkNbThreads(nbThreads);
    return UF2_memoryUsage_internal(params->dictionarySize,
        UF2_BUFFER_RESIZE_DEFAULT,
        params->chainLog,
        params->strategy,
        nbThreads);
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize_usingCCtx(const UF2_CCtx * cctx)
{
    return UF2_memoryUsage_internal(cctx->params.rParams.dictionary_size,
        cctx->params.rParams.match_buffer_resize,
        cctx->params.cParams.second_dict_bits,
        cctx->params.cParams.strategy,
        cctx->jobCount) + DICT_memUsage(&cctx->buf);
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize(int compressionLevel, unsigned nbThreads, int dualBuffer)
{
    /* This used to index the table with an unchecked level, reading past it for
     * any level estimateCCtxSize rejects; validate first, then look up the row. */
    size_t const cctxSize = UF2_estimateCCtxSize(compressionLevel, nbThreads);
    if (UF2_isError(cctxSize))
        return cctxSize;
    if (compressionLevel == 0)
        compressionLevel = UF2_CLEVEL_DEFAULT;
    return cctxSize
        + (UF2_defaultCParameters[UF2_tableRow(compressionLevel, 0)].dictionarySize << (dualBuffer != 0));
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize_byParams(const UF2_compressionParameters * params, unsigned nbThreads, int dualBuffer)
{
    return UF2_estimateCCtxSize_byParams(params, nbThreads)
        + (params->dictionarySize << (dualBuffer != 0));
}

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize_usingCStream(const UF2_CStream* fcs)
{
    return UF2_estimateCCtxSize_usingCCtx(fcs);
}
